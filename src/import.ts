import { open, readdir, readFile, stat, writeFile } from "node:fs/promises";
import path from "node:path";

/*
 * Host-side writer for the Genesis Grid Store (GBF2), mirroring kernel/disk.c.
 * Copies files from a host folder into /home on the disk image before QEMU starts.
 */

const SECTOR = 512;
const HEADER_LBA = 32;
const BITMAP_LBA = 33;
const BITMAP_SECTORS = 8;
const CAT_LBA = 2048;
const DATA_LBA = 2112;
const BLOCK_SIZE = 4096;
const ENTRY_BYTES = 80;
const PATH_BYTES = 64;
const VERSION = 2;
const MAX_BLOCKS = BITMAP_SECTORS * SECTOR * 8;
const TYPE_FILE = 1;
const TYPE_DIR = 2;
/** Grid Store disk image size (matches src/guest.ts). */
export const DISK_BYTES = 32 * 1024 * 1024;
/** Bit in the superblock flags: kernel adds any missing built-in files on next boot. */
export const FLAG_SEED = 2;

export const FS_MAX = 256;
export const FS_PART = 32; // one path component, includes the terminating zero
export const MAX_NAME = FS_PART - 1;
export const FS_DATA = 1024 * 1024;
/** Imported files land in this folder. */
export const IMPORT_DIR = "/home";
const CAT_SECTORS = (FS_MAX * ENTRY_BYTES) / SECTOR;

export interface ImportResult {
  imported: string[];
  skipped: { name: string; reason: string }[];
}

function entryPath(cat: Buffer, i: number): string {
  const e = cat.subarray(i * ENTRY_BYTES, i * ENTRY_BYTES + PATH_BYTES);
  const end = e.indexOf(0);
  return e.subarray(0, end < 0 ? PATH_BYTES : end).toString("latin1");
}

export async function importFiles(
  diskPath: string,
  files: { name: string; data: Buffer }[],
): Promise<ImportResult> {
  const result: ImportResult = { imported: [], skipped: [] };
  let size = -1;
  try {
    size = (await stat(diskPath)).size;
  } catch {
    await writeFile(diskPath, Buffer.alloc(0));
  }
  const fh = await open(diskPath, "r+");
  try {
    if (size < DISK_BYTES) {
      await fh.truncate(DISK_BYTES); // sparse where the host filesystem allows
      size = DISK_BYTES;
    }
    const read = async (lba: number, count: number): Promise<Buffer> => {
      const buf = Buffer.alloc(count * SECTOR);
      await fh.read(buf, 0, buf.length, lba * SECTOR);
      return buf;
    };
    const header = await read(HEADER_LBA, 1);
    const magic = header.toString("latin1", 0, 4);
    if (magic === "GBF1") {
      for (const { name } of files) {
        result.skipped.push({
          name,
          reason: "диск в старом формате GBF1: запустите genesis один раз, он переведёт диск, потом повторите",
        });
      }
      return result;
    }
    let blocks = Math.min(MAX_BLOCKS, Math.floor((size / SECTOR - DATA_LBA) / 8));
    let flags = 0;
    let bitmap: Buffer = Buffer.alloc(BITMAP_SECTORS * SECTOR);
    let cat: Buffer = Buffer.alloc(CAT_SECTORS * SECTOR);
    const valid =
      magic === "GBF2" &&
      header.readUInt32LE(16) === VERSION &&
      header.readUInt32LE(4) > 0 &&
      header.readUInt32LE(4) <= MAX_BLOCKS;
    if (valid) {
      blocks = Math.max(blocks, header.readUInt32LE(4));
      flags = header[8];
      bitmap = await read(BITMAP_LBA, BITMAP_SECTORS);
      cat = await read(CAT_LBA, CAT_SECTORS);
    }
    const bit = (b: number) => (bitmap[b >> 3] >> (b & 7)) & 1;
    const setBit = (b: number, on: boolean) => {
      if (on) bitmap[b >> 3] |= 1 << (b & 7);
      else bitmap[b >> 3] &= ~(1 << (b & 7));
    };
    const findEntry = (p: string) => {
      for (let i = 0; i < FS_MAX; i++) if (entryPath(cat, i) === p) return i;
      return -1;
    };
    const freeEntry = () => {
      for (let i = 0; i < FS_MAX; i++) if (cat[i * ENTRY_BYTES] === 0) return i;
      return -1;
    };
    let changed = false;
    if (findEntry(IMPORT_DIR) < 0) {
      const slot = freeEntry();
      if (slot >= 0) {
        cat.fill(0, slot * ENTRY_BYTES, (slot + 1) * ENTRY_BYTES);
        cat.write(IMPORT_DIR, slot * ENTRY_BYTES, "latin1");
        cat[slot * ENTRY_BYTES + 76] = TYPE_DIR;
        changed = true;
      }
    }

    for (const file of files) {
      const { name, data } = file;
      if (!/^[\x21-\x7e]+$/.test(name) || name.includes("/")) {
        result.skipped.push({ name, reason: "в имени можно только латиницу, цифры и знаки без пробелов" });
        continue;
      }
      if (name.length > MAX_NAME) {
        result.skipped.push({ name, reason: `имя длиннее ${MAX_NAME} символов` });
        continue;
      }
      if (data.length > FS_DATA) {
        result.skipped.push({ name, reason: `файл больше ${FS_DATA / 1024 / 1024} МБ` });
        continue;
      }
      const target = `${IMPORT_DIR}/${name}`;
      let slot = findEntry(target);
      // Free the old copy's blocks before allocating new ones.
      const freed: number[] = [];
      if (slot >= 0) {
        const off = slot * ENTRY_BYTES;
        if (cat[off + 76] === TYPE_FILE) {
          const start = cat.readUInt32LE(off + 68);
          const count = cat.readUInt32LE(off + 72);
          for (let b = start; b < start + count && b < blocks; b++) {
            if (bit(b)) {
              setBit(b, false);
              freed.push(b);
            }
          }
        }
      } else {
        slot = freeEntry();
      }
      if (slot < 0) {
        result.skipped.push({ name, reason: `на диске уже ${FS_MAX} файлов и папок, свободных мест нет` });
        continue;
      }
      const need = Math.max(1, Math.ceil(data.length / BLOCK_SIZE));
      let start = -1;
      for (let b = 0, run = 0; b < blocks; b++) {
        run = bit(b) ? 0 : run + 1;
        if (run === need) {
          start = b + 1 - need;
          break;
        }
      }
      if (start < 0) {
        for (const b of freed) setBit(b, true);
        result.skipped.push({ name, reason: "на диске не хватает места" });
        continue;
      }
      const padded = Buffer.alloc(need * BLOCK_SIZE);
      data.copy(padded);
      await fh.write(padded, 0, padded.length, (DATA_LBA + start * 8) * SECTOR);
      for (let b = start; b < start + need; b++) setBit(b, true);
      const off = slot * ENTRY_BYTES;
      cat.fill(0, off, off + ENTRY_BYTES);
      cat.write(target, off, "latin1");
      cat.writeUInt32LE(data.length, off + 64);
      cat.writeUInt32LE(start, off + 68);
      cat.writeUInt32LE(need, off + 72);
      cat[off + 76] = TYPE_FILE;
      result.imported.push(name);
      changed = true;
    }

    if (changed && result.imported.length > 0) {
      let free = 0;
      for (let b = 0; b < blocks; b++) if (!bit(b)) free++;
      const sb = Buffer.alloc(SECTOR);
      sb.write("GBF2", 0, "latin1");
      sb.writeUInt32LE(blocks, 4);
      sb[8] = flags | FLAG_SEED;
      sb.writeUInt32LE(free, 12);
      sb.writeUInt32LE(VERSION, 16);
      sb.writeUInt32LE(FS_MAX, 20);
      // Same order as the kernel: bitmap, catalog, superblock last.
      await fh.write(bitmap, 0, bitmap.length, BITMAP_LBA * SECTOR);
      await fh.write(cat, 0, cat.length, CAT_LBA * SECTOR);
      await fh.write(sb, 0, SECTOR, HEADER_LBA * SECTOR);
    }
  } finally {
    await fh.close();
  }
  return result;
}

/** Read every regular file in `folder` (except README/.gitkeep) and import it into /home. */
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
    console.error(`импорт: ${name} записан на диск genesis в ${IMPORT_DIR}`);
  }
  for (const { name, reason } of result.skipped) {
    console.error(`импорт: ${name} ПРОПУЩЕН — ${reason}`);
  }
}
