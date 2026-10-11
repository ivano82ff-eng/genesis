import { open, readdir, readFile, stat, writeFile } from "node:fs/promises";
import path from "node:path";

/*
 * Host-side writer for the Genesis Grid Store (GBF1), mirroring kernel/disk.c.
 * Copies files from a host folder onto the disk image before QEMU starts.
 */

const SECTOR = 512;
const HEADER_LBA = 32;
const BITMAP_LBA = 33;
const DIR_LBA = 34;
const DATA_LBA = 2048;
const BLOCK_SIZE = 4096;
const CHUNKS = 16;
const NO_CHUNK = 0xffffffff;
const ENTRY_BYTES = 84;
const DIR_SECTORS = 4;
const MAX_BLOCKS = SECTOR * 8;
const DEFAULT_BLOCKS = 1792;
const DISK_BYTES = 8 * 1024 * 1024;
/** Bit in the superblock flags: kernel adds any missing built-in files on next boot. */
export const FLAG_SEED = 2;

export const FS_NAME = 16; // includes the terminating zero
export const MAX_NAME = FS_NAME - 1;
export const FS_DATA = 65536;
export const FS_MAX = 20;

export interface ImportResult {
  imported: string[];
  skipped: { name: string; reason: string }[];
}

function entryName(dir: Buffer, i: number): string {
  const e = dir.subarray(i * ENTRY_BYTES, i * ENTRY_BYTES + FS_NAME);
  const end = e.indexOf(0);
  return e.subarray(0, end < 0 ? FS_NAME : end).toString("latin1");
}

export async function importFiles(
  diskPath: string,
  files: { name: string; data: Buffer }[],
): Promise<ImportResult> {
  const result: ImportResult = { imported: [], skipped: [] };
  try {
    await stat(diskPath);
  } catch {
    await writeFile(diskPath, Buffer.alloc(DISK_BYTES));
  }
  const fh = await open(diskPath, "r+");
  try {
    const read = async (lba: number, count: number): Promise<Buffer> => {
      const buf = Buffer.alloc(count * SECTOR);
      await fh.read(buf, 0, buf.length, lba * SECTOR);
      return buf;
    };
    const header = await read(HEADER_LBA, 1);
    let blocks = DEFAULT_BLOCKS;
    let flags = 0;
    let bitmap: Buffer = Buffer.alloc(SECTOR);
    let dir: Buffer = Buffer.alloc(DIR_SECTORS * SECTOR);
    const valid =
      header.toString("latin1", 0, 4) === "GBF1" &&
      header.readUInt32LE(4) > 0 &&
      header.readUInt32LE(4) <= MAX_BLOCKS;
    if (valid) {
      blocks = header.readUInt32LE(4);
      flags = header[8];
      bitmap = await read(BITMAP_LBA, 1);
      dir = await read(DIR_LBA, DIR_SECTORS);
    }
    const bit = (b: number) => (bitmap[b >> 3] >> (b & 7)) & 1;
    const setBit = (b: number, on: boolean) => {
      if (on) bitmap[b >> 3] |= 1 << (b & 7);
      else bitmap[b >> 3] &= ~(1 << (b & 7));
    };

    for (const file of files) {
      const { name, data } = file;
      if (!/^[\x21-\x7e]+$/.test(name)) {
        result.skipped.push({ name, reason: "в имени можно только латиницу, цифры и знаки без пробелов" });
        continue;
      }
      if (name.length > MAX_NAME) {
        result.skipped.push({ name, reason: `имя длиннее ${MAX_NAME} символов` });
        continue;
      }
      if (data.length > FS_DATA) {
        result.skipped.push({ name, reason: `файл больше ${FS_DATA / 1024} КБ` });
        continue;
      }
      let slot = -1;
      for (let i = 0; i < FS_MAX; i++) {
        if (entryName(dir, i) === name) {
          slot = i;
          break;
        }
      }
      // Free the old copy's chunks before allocating new ones.
      const freed: number[] = [];
      if (slot >= 0) {
        for (let k = 0; k < CHUNKS; k++) {
          const b = dir.readUInt32LE(slot * ENTRY_BYTES + 20 + k * 4);
          if (b < blocks && bit(b)) {
            setBit(b, false);
            freed.push(b);
          }
        }
      } else {
        for (let i = 0; i < FS_MAX; i++) {
          if (dir[i * ENTRY_BYTES] === 0) {
            slot = i;
            break;
          }
        }
      }
      if (slot < 0) {
        result.skipped.push({ name, reason: `на диске уже ${FS_MAX} файлов, свободных мест нет` });
        continue;
      }
      const need = Math.ceil(data.length / BLOCK_SIZE);
      const chosen: number[] = [];
      for (let b = 0; b < blocks && chosen.length < need; b++) {
        if (!bit(b)) chosen.push(b);
      }
      if (chosen.length < need) {
        for (const b of freed) setBit(b, true);
        result.skipped.push({ name, reason: "на диске не хватает места" });
        continue;
      }
      for (let k = 0; k < chosen.length; k++) {
        const chunk = Buffer.alloc(BLOCK_SIZE);
        data.copy(chunk, 0, k * BLOCK_SIZE, Math.min(data.length, (k + 1) * BLOCK_SIZE));
        await fh.write(chunk, 0, BLOCK_SIZE, (DATA_LBA + chosen[k] * 8) * SECTOR);
        setBit(chosen[k], true);
      }
      const off = slot * ENTRY_BYTES;
      dir.fill(0, off, off + ENTRY_BYTES);
      dir.write(name, off, "latin1");
      dir.writeUInt32LE(data.length, off + 16);
      for (let k = 0; k < CHUNKS; k++) {
        dir.writeUInt32LE(k < chosen.length ? chosen[k] : NO_CHUNK, off + 20 + k * 4);
      }
      result.imported.push(name);
    }

    if (result.imported.length > 0) {
      let free = 0;
      for (let b = 0; b < blocks; b++) if (!bit(b)) free++;
      const sb = Buffer.alloc(SECTOR);
      sb.write("GBF1", 0, "latin1");
      sb.writeUInt32LE(blocks, 4);
      sb[8] = flags | FLAG_SEED;
      sb.writeUInt32LE(free, 12);
      // Same order as the kernel: bitmap, catalog, superblock last.
      await fh.write(bitmap, 0, bitmap.length, BITMAP_LBA * SECTOR);
      await fh.write(dir, 0, dir.length, DIR_LBA * SECTOR);
      await fh.write(sb, 0, SECTOR, HEADER_LBA * SECTOR);
    }
  } finally {
    await fh.close();
  }
  return result;
}

/** Read every regular file in `folder` (except README/.gitkeep) and import it. */
export async function importFolder(diskPath: string, folder: string): Promise<ImportResult> {
  let names: string[];
  try {
    names = await readdir(folder);
  } catch {
    return { imported: [], skipped: [] };
  }
  const files: { name: string; data: Buffer }[] = [];
  for (const name of names.sort()) {
    if (name === ".gitkeep" || name === "README.md") continue;
    const full = path.join(folder, name);
    const info = await stat(full);
    if (!info.isFile()) continue;
    if (info.size > FS_DATA) {
      files.push({ name, data: Buffer.alloc(FS_DATA + 1) });
      continue;
    }
    files.push({ name, data: await readFile(full) });
  }
  return importFiles(diskPath, files);
}

export function reportImport(result: ImportResult): void {
  for (const name of result.imported) {
    console.error(`импорт: ${name} записан на диск genesis`);
  }
  for (const { name, reason } of result.skipped) {
    console.error(`импорт: ${name} ПРОПУЩЕН — ${reason}`);
  }
}
