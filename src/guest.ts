import { spawn } from "node:child_process";
import { EventEmitter } from "node:events";
import { readFile, stat, unlink, writeFile } from "node:fs/promises";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import { StringDecoder } from "node:string_decoder";

export type DisplayMode = "none" | "gtk";

type QmpMessage = {
  QMP?: unknown;
  return?: unknown;
  error?: { desc?: string; class?: string };
  event?: string;
};

export type Guest = {
  serial: string;
  stderr: string;
  waitForSerial(text: string, timeoutMs: number): Promise<void>;
  screendump(): Promise<Buffer>;
  sendKey(qcode: string): Promise<void>;
  monitor(command: string): Promise<string>;
  qmp(command: string, args?: Record<string, unknown>): Promise<unknown>;
  /** Wait until the user closes the window. Does not kill QEMU. */
  waitUntilClosed(): Promise<void>;
  stop(): Promise<void>;
};

function delay(ms: number): Promise<void> {
  return new Promise((resolve) => {
    setTimeout(resolve, ms);
  });
}

class QmpClient {
  private buffer = "";
  private closedError: Error | null = null;
  private readonly queue: QmpMessage[] = [];
  private readonly waiters: Array<{
    resolve: (msg: QmpMessage) => void;
    reject: (err: Error) => void;
  }> = [];

  constructor(private readonly socket: net.Socket) {
    socket.setEncoding("utf8");
    const fail = (err: Error): void => {
      if (this.closedError) {
        return;
      }
      this.closedError = err;
      for (const waiter of this.waiters.splice(0)) {
        waiter.reject(err);
      }
    };
    socket.on("data", (chunk: string) => {
      this.buffer += chunk;
      while (true) {
        const nl = this.buffer.indexOf("\n");
        if (nl < 0) {
          break;
        }
        const line = this.buffer.slice(0, nl).replace(/\r$/, "").trim();
        this.buffer = this.buffer.slice(nl + 1);
        if (line === "") {
          continue;
        }
        const msg = JSON.parse(line) as QmpMessage;
        const waiter = this.waiters.shift();
        if (waiter) {
          waiter.resolve(msg);
        } else {
          this.queue.push(msg);
        }
      }
    });
    socket.on("error", fail);
    socket.on("close", () => {
      fail(new Error("QMP закрыт"));
    });
  }

  private next(): Promise<QmpMessage> {
    if (this.closedError) {
      return Promise.reject(this.closedError);
    }
    const queued = this.queue.shift();
    if (queued) {
      return Promise.resolve(queued);
    }
    return new Promise((resolve, reject) => {
      this.waiters.push({ resolve, reject });
    });
  }

  private async nextResponse(): Promise<QmpMessage> {
    while (true) {
      const msg = await this.next();
      if (msg.event != null && msg.return === undefined && msg.error === undefined) {
        continue;
      }
      return msg;
    }
  }

  async execute(command: string, args?: Record<string, unknown>): Promise<unknown> {
    const payload =
      args === undefined
        ? { execute: command }
        : { execute: command, arguments: args };
    this.socket.write(`${JSON.stringify(payload)}\n`);
    const msg = await this.nextResponse();
    if (msg.error) {
      throw new Error(msg.error.desc ?? `QMP ${command} failed`);
    }
    return msg.return;
  }

  static async connect(sockPath: string): Promise<QmpClient> {
    const deadline = Date.now() + 5000;
    let lastError: unknown;
    while (Date.now() < deadline) {
      try {
        const socket = await new Promise<net.Socket>((resolve, reject) => {
          const client = net.createConnection(sockPath);
          const onError = (err: Error): void => {
            client.removeListener("connect", onConnect);
            reject(err);
          };
          const onConnect = (): void => {
            client.removeListener("error", onError);
            resolve(client);
          };
          client.once("error", onError);
          client.once("connect", onConnect);
        });
        const qmp = new QmpClient(socket);
        const greeting = await qmp.next();
        if (greeting.QMP == null) {
          throw new Error("QMP не прислал приветствие");
        }
        await qmp.execute("qmp_capabilities");
        return qmp;
      } catch (err) {
        lastError = err;
        await delay(50);
      }
    }
    throw new Error(
      `Не удалось подключиться к QMP: ${lastError instanceof Error ? lastError.message : String(lastError)}`,
    );
  }
}

async function ensureDisk(diskPath: string): Promise<void> {
  try {
    await stat(diskPath);
  } catch {
    await writeFile(diskPath, Buffer.alloc(8 * 1024 * 1024));
  }
}

export async function startGuest(options: {
  isoPath?: string;
  display: DisplayMode;
  qemuBin?: string;
  diskPath?: string;
  boot?: "cd" | "disk";
  net?: { mode: "listen" | "connect"; port: number; mac: string };
}): Promise<Guest> {
  const qemu = options.qemuBin ?? "qemu-system-x86_64";
  const sockPath = path.join(
    os.tmpdir(),
    `genesis-qmp-${process.pid}-${Date.now()}.sock`,
  );
  const dumpPath = path.join(
    os.tmpdir(),
    `genesis-screen-${process.pid}-${Date.now()}.ppm`,
  );
  const diskPath =
    options.diskPath ??
    path.join(os.tmpdir(), `genesis-disk-${process.pid}-${Date.now()}.img`);
  await ensureDisk(diskPath);
  const args = [
    "-machine",
    "pc",
    "-accel",
    "tcg",
    "-m",
    "128M",
    "-drive",
    `file=${diskPath},format=raw,if=ide,index=0,media=disk,cache=writethrough`,
    "-serial",
    "stdio",
    "-display",
    options.display === "gtk" ? "gtk,grab-on-hover=on" : options.display,
    "-vga",
    "std",
    "-qmp",
    `unix:${sockPath},server,nowait`,
    "-monitor",
    "none",
  ];
  if (options.boot === "disk") {
    args.push("-boot", "order=c");
  } else {
    if (!options.isoPath) {
      throw new Error("Для загрузки с CD нужен ISO");
    }
    args.push("-cdrom", options.isoPath, "-boot", "order=d");
  }
  if (options.net) {
    const endpoint =
      options.net.mode === "listen"
        ? `listen=127.0.0.1:${options.net.port}`
        : `connect=127.0.0.1:${options.net.port}`;
    args.push(
      "-netdev",
      `socket,id=link0,${endpoint}`,
      "-device",
      `rtl8139,netdev=link0,mac=${options.net.mac}`,
    );
  } else {
    args.push("-nic", "none");
  }

  const child = spawn(qemu, args, {
    stdio: ["ignore", "pipe", "pipe"],
    detached: true,
  });
  const events = new EventEmitter();
  let serial = "";
  let stderr = "";
  let exitCode: number | null = null;
  let stopped = false;

  // Serial bytes arrive in arbitrary chunks; keep multibyte UTF-8 (Cyrillic labels) intact.
  const serialDecoder = new StringDecoder("utf8");
  child.stdout?.on("data", (buf: Buffer) => {
    const text = serialDecoder.write(buf);
    process.stdout.write(text);
    serial += text;
    events.emit("serial");
  });
  child.stderr?.on("data", (buf: Buffer) => {
    const text = buf.toString("utf8");
    process.stderr.write(text);
    stderr += text;
  });
  const keepAlive = setInterval(() => {}, 1000);
  const closed = new Promise<void>((resolve) => {
    child.on("close", (code) => {
      exitCode = code;
      events.emit("close");
      resolve();
      setImmediate(() => {
        clearInterval(keepAlive);
      });
    });
  });
  child.on("error", (err: NodeJS.ErrnoException) => {
    stderr += err.message;
    if (err.code === "ENOENT") {
      events.emit(
        "spawn-error",
        new Error(`Не найдена программа ${qemu}. Установите пакет qemu-system-x86.`),
      );
      return;
    }
    events.emit("spawn-error", err);
  });

  const spawnError = new Promise<never>((_resolve, reject) => {
    events.once("spawn-error", reject);
  });

  const qmpPromise = QmpClient.connect(sockPath);
  void qmpPromise.catch(() => undefined);

  const kill = (): void => {
    if (child.pid == null) {
      child.kill("SIGKILL");
      return;
    }
    try {
      process.kill(-child.pid, "SIGKILL");
    } catch {
      child.kill("SIGKILL");
    }
  };

  const guest: Guest = {
    get serial() {
      return serial;
    },
    get stderr() {
      return stderr;
    },
    async waitForSerial(text: string, timeoutMs: number): Promise<void> {
      await Promise.race([
        spawnError,
        new Promise<void>((resolve, reject) => {
          const finishOk = (): void => {
            cleanup();
            resolve();
          };
          const timer = setTimeout(() => {
            cleanup();
            reject(
              new Error(
                `В serial нет «${text}» за ${timeoutMs} мс.\n${serial}\n${stderr}`,
              ),
            );
          }, timeoutMs);
          const onSerial = (): void => {
            if (serial.includes(text)) {
              finishOk();
            }
          };
          const onClose = (): void => {
            if (serial.includes(text)) {
              finishOk();
              return;
            }
            cleanup();
            reject(
              new Error(
                `QEMU завершился (код ${exitCode ?? "null"}) без «${text}».\n${serial}\n${stderr}`,
              ),
            );
          };
          const cleanup = (): void => {
            clearTimeout(timer);
            events.off("serial", onSerial);
            events.off("close", onClose);
          };
          events.on("serial", onSerial);
          events.on("close", onClose);
          if (serial.includes(text) || exitCode != null || child.signalCode != null) {
            if (serial.includes(text)) {
              finishOk();
            } else {
              onClose();
            }
          }
        }),
      ]);
    },
    async qmp(command: string, args?: Record<string, unknown>): Promise<unknown> {
      const client = await qmpPromise;
      return client.execute(command, args);
    },
    async screendump(): Promise<Buffer> {
      const client = await qmpPromise;
      await client.execute("screendump", { filename: dumpPath });
      return readFile(dumpPath);
    },
    async sendKey(qcode: string): Promise<void> {
      const client = await qmpPromise;
      await client.execute("send-key", {
        keys: [{ type: "qcode", data: qcode }],
      });
    },
    async monitor(command: string): Promise<string> {
      const client = await qmpPromise;
      const result = await client.execute("human-monitor-command", {
        "command-line": command,
      });
      return typeof result === "string" ? result : "";
    },
    async waitUntilClosed(): Promise<void> {
      if (exitCode != null || child.signalCode != null) {
        return;
      }
      await closed;
    },
    async stop(): Promise<void> {
      if (stopped) {
        return;
      }
      stopped = true;
      if (exitCode != null || child.signalCode != null) {
        await unlink(sockPath).catch(() => undefined);
        await unlink(dumpPath).catch(() => undefined);
        return;
      }
      await new Promise<void>((resolve) => {
        const giveUp = setTimeout(resolve, 2000);
        child.once("close", () => {
          clearTimeout(giveUp);
          resolve();
        });
        kill();
      });
      await unlink(sockPath).catch(() => undefined);
      await unlink(dumpPath).catch(() => undefined);
    },
  };

  return guest;
}
