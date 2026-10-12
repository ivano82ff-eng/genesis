import { execFileSync } from "node:child_process";
import { readdir, readFile, writeFile } from "node:fs/promises";
import path from "node:path";

/** Grid Store limits (kernel/fs_limits.h). */
const FS_NAME = 32; // one path component (kernel FS_PART)
const FS_DATA = 1024 * 1024;

export interface PackageManifest {
  name: string;
  version: string;
  deps: string[];
  description: string;
}

type Exec = (cmd: string, args: string[], cwd: string) => Promise<void>;

const pkgCc = [
  "-std=gnu11",
  "-ffreestanding",
  "-fno-builtin",
  "-fno-stack-protector",
  "-fPIE",
  "-fvisibility=hidden",
  "-fno-plt",
  "-fno-asynchronous-unwind-tables",
  "-fno-unwind-tables",
  "-fno-tree-loop-distribute-patterns",
  "-fcf-protection=none",
  "-Os",
  "-Wall",
  "-Wextra",
  "-Werror",
];

/** Relocations a flat binary loaded at any address can still honour. */
const PC_RELATIVE = new Set(["R_X86_64_PC32", "R_X86_64_PLT32"]);
/** i686 PIC reaches its data through the GOT base, which ld resolves inside the image. */
const PC_RELATIVE_I686 = new Set(["R_386_PC32", "R_386_PLT32", "R_386_GOTPC", "R_386_GOTOFF"]);

export async function readManifests(root: string): Promise<PackageManifest[]> {
  const dir = path.join(root, "packages");
  const entries = await readdir(dir, { withFileTypes: true });
  const manifests: PackageManifest[] = [];
  for (const entry of entries.filter((e) => e.isDirectory()).sort((a, b) => a.name.localeCompare(b.name))) {
    const raw = JSON.parse(await readFile(path.join(dir, entry.name, "pkg.json"), "utf8")) as PackageManifest;
    if (raw.name !== entry.name || !/^[a-z0-9]{1,15}$/.test(raw.name)) {
      throw new Error(`packages/${entry.name}: name must match the folder, a-z0-9, <= 15 chars`);
    }
    if (!/^\S+$/.test(raw.version) || raw.description.includes("\n")) {
      throw new Error(`packages/${entry.name}: bad version or description`);
    }
    manifests.push({ ...raw, deps: raw.deps ?? [] });
  }
  const names = new Set(manifests.map((m) => m.name));
  for (const m of manifests) {
    for (const dep of m.deps) {
      if (!names.has(dep)) {
        throw new Error(`package ${m.name}: unknown dependency ${dep}`);
      }
    }
  }
  return manifests;
}

/** Builds every packages/<name>/*.c into one flat binary and writes the repository blob. */
export async function buildPackages(
  root: string,
  buildDir: string,
  exec: Exec,
  arch: "x86_64" | "i686" = "x86_64",
): Promise<string> {
  const x64 = arch === "x86_64";
  const archCc = x64 ? ["-mno-red-zone", "-m64", "-mcmodel=small"] : ["-m32", "-march=i686"];
  const allowed = x64 ? PC_RELATIVE : PC_RELATIVE_I686;
  const manifests = await readManifests(root);
  const lines: string[] = [];
  const blobs: Buffer[] = [];
  let offset = 0;
  for (const m of manifests) {
    const srcDir = path.join(root, "packages", m.name);
    const sources = (await readdir(srcDir)).filter((f) => f.endsWith(".c")).sort();
    const objects: string[] = [];
    for (const source of sources) {
      const obj = path.join(buildDir, `pkg-${m.name}-${source.replace(/\.c$/, ".o")}`);
      await exec("gcc", [...pkgCc, ...archCc, "-c", path.join(srcDir, source), "-o", obj], root);
      const relocs = execFileSync("readelf", ["-rW", obj], { encoding: "utf8" });
      for (const type of relocs.match(/R_(?:X86_64|386)_\w+/g) ?? []) {
        if (!allowed.has(type)) {
          throw new Error(`package ${m.name}: ${source} needs ${type}; packages must be position-independent`);
        }
      }
      objects.push(obj);
    }
    const elf = path.join(buildDir, `pkg-${m.name}.elf`);
    const bin = path.join(buildDir, `pkg-${m.name}.bin`);
    await exec(
      "ld",
      ["-m", x64 ? "elf_x86_64" : "elf_i386", "-nostdlib", "-static", "--no-warn-rwx-segments", "-e", "0",
        "-T", path.join(root, "packages", "pkg.ld"), "-o", elf, ...objects],
      root,
    );
    await exec("objcopy", ["-O", "binary", "-j", ".all", elf, bin], root);
    const data = await readFile(bin);
    if (data.length > FS_DATA || m.name.length >= FS_NAME) {
      throw new Error(`package ${m.name}: ${data.length} bytes, Grid Store files hold ${FS_DATA}`);
    }
    lines.push(`P ${m.name} ${m.version} ${m.deps.length ? m.deps.join(",") : "-"} ${m.description}`);
    lines.push(`F ${m.name} ${m.name} ${offset} ${data.length}`);
    blobs.push(data);
    offset += data.length;
  }
  const repo = Buffer.concat([Buffer.from(lines.join("\n") + "\n\0", "ascii"), ...blobs]);
  const out = path.join(buildDir, "pkgrepo.bin");
  await writeFile(out, repo);
  return out;
}
