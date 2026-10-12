import { buildPackages } from "./packages.js";
import { spawn, spawnSync } from "node:child_process";
import { pickQemu, startGuest } from "./guest.js";
import { copyFile, mkdir, readFile, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

/** Stable COM1 line. The VGA text screen shows the same words. */
export const BOOT_MARKER = "genesis: kernel booted";

/** Guest prompt printed after the boot line. */
export const PROMPT = "genesis:/home> ";

export const DEFAULT_TIMEOUT_MS = 20_000;

const HOST_PACKAGES =
  "build-essential nasm qemu-system-x86 grub-pc-bin grub-common xorriso mtools";

const MULTIBOOT2_MAGIC = 0xe85250d6;

/** x86_64 (QEMU, the default) or i686 (32-bit, for v86 in the browser). */
export type Arch = "x86_64" | "i686";

/** GENESIS_ARCH=i686 switches build, QEMU and tests to the 32-bit target. */
export function currentArch(): Arch {
  const raw = process.env.GENESIS_ARCH ?? "x86_64";
  if (raw !== "x86_64" && raw !== "i686") {
    throw new Error(`GENESIS_ARCH: ${raw}; ждём x86_64 или i686`);
  }
  return raw;
}

/** build/ for x86_64 (as before), build/i686/ for the 32-bit image. */
export function buildDirFor(arch: Arch): string {
  return arch === "x86_64" ? path.join(repoRoot(), "build") : path.join(repoRoot(), "build", "i686");
}

export type ImagePaths = {
  elfPath: string;
  isoPath: string;
};

export type RunResult = {
  serial: string;
  qemuStderr: string;
  markerFound: boolean;
  timedOut: boolean;
  qemuExitCode: number | null;
};

export function repoRoot(): string {
  const here = path.dirname(fileURLToPath(import.meta.url));
  return path.resolve(here, "../..");
}

export function missingTools(names: string[]): string[] {
  return names.filter((name) => {
    const found = spawnSync("which", [name], { encoding: "utf8" });
    return found.status !== 0 || found.stdout.trim() === "";
  });
}

export function hostToolError(missing: string[]): Error {
  return new Error(
    `Не найдены программы: ${missing.join(", ")}.\n` +
      "В WSL (не в PowerShell) установите пакеты:\n" +
      `sudo apt install -y ${HOST_PACKAGES}`,
  );
}

function exec(cmd: string, args: string[], cwd?: string): Promise<void> {
  console.error(`+ ${cmd} ${args.join(" ")}`);
  return new Promise((resolve, reject) => {
    const child = spawn(cmd, args, { cwd, stdio: ["ignore", "pipe", "pipe"] });
    child.stdout?.on("data", (buf: Buffer) => {
      process.stderr.write(buf);
    });
    child.stderr?.on("data", (buf: Buffer) => {
      process.stderr.write(buf);
    });
    child.on("error", (err: NodeJS.ErrnoException) => {
      if (err.code === "ENOENT") {
        reject(new Error(`Не найдена программа ${cmd}`));
        return;
      }
      reject(err);
    });
    child.on("close", (code) => {
      if (code === 0) {
        resolve();
        return;
      }
      reject(new Error(`${cmd} завершился с кодом ${code ?? "null"}`));
    });
  });
}

/** Fail the build if GRUB would not see a Multiboot2 header in the ELF file. */
export function assertMultiboot2Header(elf: Buffer): void {
  const window = elf.subarray(0, Math.min(elf.length, 32768));
  for (let off = 0; off + 16 <= window.length; off += 8) {
    if (window.readUInt32LE(off) !== MULTIBOOT2_MAGIC) {
      continue;
    }
    const arch = window.readUInt32LE(off + 4);
    const length = window.readUInt32LE(off + 8);
    const checksum = window.readUInt32LE(off + 12);
    const sum = (MULTIBOOT2_MAGIC + arch + length + checksum) >>> 0;
    if (sum !== 0 || arch !== 0 || length < 16 || off + length > window.length) {
      continue;
    }
    return;
  }
  throw new Error("В ELF нет корректного заголовка Multiboot2 в первых 32 КиБ");
}

export async function buildImage(arch: Arch = currentArch()): Promise<ImagePaths> {
  const missing = missingTools(["nasm", "gcc", "ld", "objcopy", "grub-mkrescue", "xorriso"]);
  if (missing.length > 0) {
    throw hostToolError(missing);
  }

  const root = repoRoot();
  const buildDir = buildDirFor(arch);
  const x64 = arch === "x86_64";
  const nasmElf = x64 ? "-felf64" : "-felf32";
  /* nasm flags every 32-bit cross-section reference; the linker resolves them. */
  const nasmQuiet = x64 ? [] : ["-w-reloc-abs-dword", "-w-reloc-rel-dword"];
  const ldEmu = x64 ? "elf_x86_64" : "elf_i386";
  /* 64-bit sources in kernel/ and programs/; the i686 twins of the assembly live in */
  /* kernel/i686/ and programs/i686/. All C is shared. */
  const kasm = (name: string): string => path.join(root, "kernel", ...(x64 ? [] : ["i686"]), name);
  const pasm = (name: string): string => path.join(root, "programs", ...(x64 ? [] : ["i686"]), name);
  /* Blob sources incbin "build/..."; the i686 copies point at build/i686/. */
  const blobSource = async (name: string): Promise<string> => {
    const src = path.join(root, "kernel", name);
    if (x64) {
      return src;
    }
    const text = (await readFile(src, "utf8")).replaceAll('incbin "build/', 'incbin "build/i686/');
    const out = path.join(buildDir, name);
    await writeFile(out, text);
    return out;
  };
  const isoRoot = path.join(buildDir, "iso");
  const elfPath = path.join(buildDir, "kernel.elf");
  const isoPath = path.join(buildDir, "genesis.iso");
  const bootObj = path.join(buildDir, "boot.o");
  const kernelObj = path.join(buildDir, "kernel.o");

  await mkdir(path.join(isoRoot, "boot", "grub"), { recursive: true });

  const helloBin = path.join(buildDir, "hello.bin");
  const blobObj = path.join(buildDir, "assets_blob.o");
  const desktopObj = path.join(buildDir, "desktop.o");
  const irqObj = path.join(buildDir, "irq.o");
  const diskObj = path.join(buildDir, "disk.o");
  const linkObj = path.join(buildDir, "link.o");
  const taskObj = path.join(buildDir, "task.o");
  const userObj = path.join(buildDir, "user.o");
  const editorObj = path.join(buildDir, "editor.o");
  const pkgObj = path.join(buildDir, "pkg.o");
  const ccObj = path.join(buildDir, "cc.o");
  const mbrBin = path.join(buildDir, "mbr.bin");
  const mbrObj = path.join(buildDir, "mbr_blob.o");
  const runtimeObj = path.join(buildDir, "runtime.o");

  const progCc = [
    "-std=gnu11",
    "-ffreestanding",
    "-fno-stack-protector",
    "-fno-pic",
    "-fno-pie",
    "-fno-asynchronous-unwind-tables",
    "-fno-unwind-tables",
    "-mno-red-zone",
    "-fcf-protection=none",
    ...(x64 ? ["-m64", "-mcmodel=large"] : ["-m32", "-march=i686"]),
    "-Os",
    "-Wall",
    "-Wextra",
    "-Wno-unused-parameter",
    "-U_FORTIFY_SOURCE",
    "-D_FORTIFY_SOURCE=0",
    `-I${path.join(root, "programs")}`,
  ];

  /* Kernel objects: like programs but small code model, all warnings fatal. */
  const kernelCc = [
    "-std=gnu11",
    "-ffreestanding",
    "-fno-stack-protector",
    "-fno-pic",
    "-fno-pie",
    "-fno-asynchronous-unwind-tables",
    "-fno-unwind-tables",
    "-mno-red-zone",
    "-mgeneral-regs-only",
    "-fcf-protection=none",
    ...(x64 ? ["-m64", "-mcmodel=small"] : ["-m32", "-march=i686"]),
    "-Os",
    "-Wall",
    "-Wextra",
    "-Werror",
  ];

  const compileKernel = (source: string, output: string): Promise<void> =>
    exec("gcc", [...kernelCc, "-c", path.join(root, "kernel", source), "-o", output], root);

  await exec("nasm", ["-fbin", pasm("hello.asm"), "-o", helloBin], root);
  await exec("nasm", ["-fbin", pasm("ticker.asm"), "-o", path.join(buildDir, "ticker.bin")], root);
  await exec("nasm", ["-fbin", pasm("pulse.asm"), "-o", path.join(buildDir, "pulse.bin")], root);
  await exec("nasm", ["-fbin", pasm("crashy.asm"), "-o", path.join(buildDir, "crashy.bin")], root);
  const startObj = path.join(buildDir, "start.o");
  await exec("nasm", [nasmElf, pasm("start.s"), "-o", startObj], root);
  await exec("gcc", [...progCc, "-c", path.join(root, "programs", "runtime.c"), "-o", runtimeObj], root);
  for (const name of ["open", "vtxt", "vdocx", "vimg", "vpdf", "vmp4"]) {
    const obj = path.join(buildDir, `${name}.o`);
    const elf = path.join(buildDir, `${name}.elf`);
    const bin = path.join(buildDir, `${name}.bin`);
    await exec("gcc", [...progCc, "-c", path.join(root, "programs", `${name}.c`), "-o", obj], root);
    await exec(
      "ld",
      [
        "-m",
        ldEmu,
        "-nostdlib",
        "--no-warn-rwx-segments",
        "-T",
        path.join(root, "programs", "program.ld"),
        "-o",
        elf,
        startObj,
        obj,
        runtimeObj,
      ],
      root,
    );
    await exec("objcopy", ["-O", "binary", elf, bin], root);
  }

  await buildPackages(root, buildDir, exec, arch);

  await exec("nasm", ["-fbin", path.join(root, "kernel", "mbr.asm"), "-o", mbrBin], root);

  await exec(
    "nasm",
    [nasmElf, "-w+all", ...nasmQuiet, kasm("boot.asm"), "-o", bootObj],
    root,
  );

  await exec(
    "nasm",
    [nasmElf, "-w+all", ...nasmQuiet, await blobSource("assets_blob.asm"), "-o", blobObj],
    root,
  );

  await exec(
    "nasm",
    [nasmElf, "-w+all", ...nasmQuiet, kasm("irq.asm"), "-o", irqObj],
    root,
  );

  await exec(
    "nasm",
    [nasmElf, "-w+all", ...nasmQuiet, await blobSource("mbr_blob.asm"), "-o", mbrObj],
    root,
  );

  await compileKernel("kernel.c", kernelObj);

  await compileKernel("desktop.c", desktopObj);

  await compileKernel("disk.c", diskObj);

  await compileKernel("link.c", linkObj);

  await compileKernel("task.c", taskObj);

  await compileKernel("user.c", userObj);

  await compileKernel("editor.c", editorObj);

  await compileKernel("pkg.c", pkgObj);

  await compileKernel("cc.c", ccObj);

  await exec("ld", [
    "-m",
    ldEmu,
    "-n",
    "-nostdlib",
    "-z",
    "max-page-size=0x1000",
    "-z",
    "noexecstack",
    "--no-warn-rwx-segments",
    "-T",
    path.join(root, "kernel", "linker.ld"),
    "-o",
    elfPath,
    bootObj,
    kernelObj,
    desktopObj,
    irqObj,
    diskObj,
    linkObj,
    taskObj,
    userObj,
    editorObj,
    pkgObj,
    ccObj,
    mbrObj,
    blobObj,
  ], root);

  assertMultiboot2Header(await readFile(elfPath));

  await copyFile(elfPath, path.join(isoRoot, "boot", "kernel.elf"));
  await copyFile(
    path.join(root, "kernel", "grub.cfg"),
    path.join(isoRoot, "boot", "grub", "grub.cfg"),
  );

  await exec("grub-mkrescue", ["-o", isoPath, isoRoot], root);
  return { elfPath, isoPath };
}

export async function runGuest(options: {
  isoPath: string;
  timeoutMs: number;
  qemuBin?: string;
}): Promise<RunResult> {
  const guest = await startGuest({
    isoPath: options.isoPath,
    display: "none",
    qemuBin: options.qemuBin,
  });
  let markerFound = false;
  let timedOut = false;
  try {
    await guest.waitForSerial(BOOT_MARKER, options.timeoutMs);
    markerFound = true;
  } catch {
    markerFound = guest.serial.includes(BOOT_MARKER);
    timedOut = !markerFound;
  } finally {
    await guest.stop();
  }
  return {
    serial: guest.serial,
    qemuStderr: guest.stderr,
    markerFound,
    timedOut,
    qemuExitCode: null,
  };
}

/** Build the image, boot headless, and keep serial through the prompt. */
export async function captureBootLog(timeoutMs: number): Promise<{
  serial: string;
  markerFound: boolean;
}> {
  const missing = missingTools([pickQemu("none")]);
  if (missing.length > 0) {
    throw hostToolError(missing);
  }
  const image = await buildImage();
  const guest = await startGuest({ isoPath: image.isoPath, display: "none" });
  try {
    await guest.waitForSerial(PROMPT, timeoutMs);
  } catch {
    // The caller inspects whatever reached COM1.
  } finally {
    await guest.stop();
  }
  return {
    serial: guest.serial,
    markerFound: guest.serial.includes(BOOT_MARKER),
  };
}

export async function saveSerialLog(serial: string): Promise<string> {
  const file = path.join(repoRoot(), "build", "serial.log");
  await mkdir(path.dirname(file), { recursive: true });
  await writeFile(file, serial);
  return file;
}
