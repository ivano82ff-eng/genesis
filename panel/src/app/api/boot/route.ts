import { readFile } from "node:fs/promises";
import { spawn } from "node:child_process";
import path from "node:path";
import { NextResponse } from "next/server";

export const runtime = "nodejs";

const BOOT_MARKER = "genesis: kernel booted";

function failureText(stderr: string): string {
  const lines = stderr
    .split("\n")
    .map((line) => line.trim())
    .filter((line) => {
      if (line === "") {
        return false;
      }
      if (line.startsWith("+ ")) {
        return false;
      }
      return !(
        line.startsWith("xorriso") ||
        line.startsWith("Drive ") ||
        line.startsWith("Media ") ||
        line.startsWith("Added ") ||
        line.startsWith("ISO ") ||
        line.startsWith("Written ") ||
        line.startsWith("Writing ")
      );
    });
  return lines.slice(-8).join("\n");
}

function runShow(): Promise<{ code: number | null; stdout: string; stderr: string }> {
  const repoRoot = path.resolve(process.cwd(), "..");
  const cli = path.join(repoRoot, "dist", "src", "cli.js");
  const child = spawn(process.execPath, [cli, "show", "--timeout", "40"], {
    cwd: repoRoot,
    detached: true,
    stdio: ["ignore", "pipe", "pipe"],
  });

  let stdout = "";
  let stderr = "";
  return new Promise((resolve, reject) => {
    let settled = false;
    const finish = (code: number | null): void => {
      if (settled) {
        return;
      }
      settled = true;
      clearTimeout(bootTimer);
      resolve({ code, stdout, stderr });
    };
    const bootTimer = setTimeout(() => {
      try {
        if (child.pid != null) {
          process.kill(-child.pid, "SIGKILL");
        }
      } catch {
        child.kill("SIGKILL");
      }
      stderr += "\nНе дождались рабочего стола";
      finish(1);
    }, 60_000);
    const noteReady = (): void => {
      if (stderr.includes("снимок:")) {
        child.unref();
        finish(0);
      }
    };
    child.stdout.on("data", (buf: Buffer) => {
      stdout += buf.toString("utf8");
    });
    child.stderr.on("data", (buf: Buffer) => {
      stderr += buf.toString("utf8");
      noteReady();
    });
    child.on("error", (err: NodeJS.ErrnoException) => {
      if (settled) {
        return;
      }
      settled = true;
      clearTimeout(bootTimer);
      if (err.code === "ENOENT") {
        reject(new Error("Не найден Node.js для запуска конвейера"));
        return;
      }
      reject(err);
    });
    child.on("close", (code) => {
      finish(code);
    });
  });
}

export async function POST(): Promise<NextResponse> {
  try {
    const result = await runShow();
    const repoRoot = path.resolve(process.cwd(), "..");
    const pngPath = path.join(repoRoot, "build", "desktop.png");
    let image = "";
    if (result.code === 0) {
      image = (await readFile(pngPath)).toString("base64");
    }
    if (result.code !== 0 || !result.stdout.includes(BOOT_MARKER) || image === "") {
      return NextResponse.json(
        {
          ok: false,
          error:
            failureText(result.stderr) ||
            "Не удалось снять рабочий стол",
          log: result.stdout,
        },
        { status: 500 },
      );
    }
    return NextResponse.json({ ok: true, log: result.stdout, image });
  } catch (err) {
    const message = err instanceof Error ? err.message : "Не удалось запустить конвейер";
    return NextResponse.json({ ok: false, error: message, log: "" }, { status: 500 });
  }
}
