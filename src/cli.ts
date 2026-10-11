#!/usr/bin/env node
import { mkdir, writeFile } from "node:fs/promises";
import path from "node:path";
import { startGuest } from "./guest.js";
import { ppmToPng } from "./png.js";
import { delay, typeLine } from "./typing.js";
import {
  BOOT_MARKER,
  DEFAULT_TIMEOUT_MS,
  PROMPT,
  buildImage,
  captureBootLog,
  hostToolError,
  missingTools,
  repoRoot,
  runGuest,
  saveSerialLog,
} from "./pipeline.js";

function usage(code: number): never {
  console.error(`genesis — минимальное ядро x86_64 в QEMU

Команды:
  build                 собрать build/genesis.iso
  run [--timeout СЕК]   запустить QEMU, снять COM1
  test [--timeout СЕК]  то же, что run: код 0 только если найден маркер
  log [--timeout СЕК]   дождаться приглашения и отдать журнал COM1
  show [--timeout СЕК]  открыть стол в окне и не закрывать его

Маркер: ${BOOT_MARKER}
Таймаут по умолчанию: ${DEFAULT_TIMEOUT_MS / 1000} с. Для show это только ожидание появления стола.
Код выхода: 0 — маркер есть, 1 — нет, 2 — сборка или аргументы.
Окно show закрывается только когда его закрывает человек.
`);
  process.exit(code);
}

function readTimeout(args: string[]): number {
  let timeoutMs = DEFAULT_TIMEOUT_MS;
  for (let i = 0; i < args.length; i++) {
    const arg = args[i];
    if (arg !== "--timeout") {
      throw new Error(`Неизвестный аргумент: ${arg}`);
    }
    const raw = args[++i];
    if (raw == null) {
      throw new Error("После --timeout нужно число секунд");
    }
    const seconds = Number(raw);
    if (!Number.isFinite(seconds) || seconds <= 0) {
      throw new Error("--timeout должен быть положительным числом секунд");
    }
    timeoutMs = Math.round(seconds * 1000);
  }
  return timeoutMs;
}

async function boot(timeoutMs: number): Promise<void> {
  const missing = missingTools(["qemu-system-x86_64"]);
  if (missing.length > 0) {
    throw hostToolError(missing);
  }
  const image = await buildImage();
  console.error(`образ: ${image.isoPath}`);
  const result = await runGuest({ isoPath: image.isoPath, timeoutMs });
  const logPath = await saveSerialLog(result.serial);
  console.error(`serial: ${logPath}`);
  if (!result.markerFound) {
    const why = result.timedOut
      ? `Маркер не найден за ${timeoutMs / 1000} с`
      : "QEMU завершился без маркера";
    console.error(why);
    process.exitCode = 1;
    return;
  }
  console.error("Маркер найден");
}

async function show(timeoutMs: number): Promise<void> {
  const missing = missingTools(["qemu-system-x86_64"]);
  if (missing.length > 0) {
    throw hostToolError(missing);
  }
  const image = await buildImage();
  const disk = path.join(repoRoot(), "build", "genesis-disk.img");
  const guest = await startGuest({
    isoPath: image.isoPath,
    display: "gtk",
    diskPath: disk,
  });
  try {
    await guest.waitForSerial(PROMPT, timeoutMs);
    const openedBy = Date.now() + timeoutMs;
    while (!guest.serial.includes("home: ready")) {
      if (Date.now() > openedBy) {
        throw new Error("Домашний экран не появился");
      }
      if (!guest.serial.includes("hello from program")) {
        await typeLine(guest, "desktop");
      }
      await delay(400);
    }
    const png = ppmToPng(await guest.screendump());
    const built = path.join(repoRoot(), "build", "desktop.png");
    await mkdir(path.dirname(built), { recursive: true });
    await writeFile(built, png);
    console.error(`снимок: ${built}`);
    if (!guest.serial.includes(BOOT_MARKER)) {
      console.error("Маркер не найден");
      process.exitCode = 1;
      await guest.stop();
      return;
    }
    console.error("Окно QEMU открыто, пока вы его не закроете.");
  } catch (err) {
    await guest.stop();
    throw err;
  }
  await guest.waitUntilClosed();
}

async function log(timeoutMs: number): Promise<void> {
  const result = await captureBootLog(timeoutMs);
  await saveSerialLog(result.serial);
  if (!result.markerFound) {
    console.error("Маркер не найден");
    process.exitCode = 1;
  }
}

async function main(): Promise<void> {
  const [command, ...rest] = process.argv.slice(2);
  if (
    command == null ||
    command === "-h" ||
    command === "--help" ||
    command === "help"
  ) {
    usage(0);
  }

  if (command === "build") {
    if (rest.length > 0) {
      usage(2);
    }
    const image = await buildImage();
    console.error(`образ: ${image.isoPath}`);
    return;
  }

  if (command === "run" || command === "test") {
    await boot(readTimeout(rest));
    return;
  }

  if (command === "log") {
    await log(readTimeout(rest));
    return;
  }

  if (command === "show") {
    await show(readTimeout(rest));
    return;
  }

  usage(2);
}

main().catch((err: unknown) => {
  console.error(err instanceof Error ? err.message : err);
  process.exitCode = 2;
});
