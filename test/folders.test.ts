import assert from "node:assert/strict";
import { mkdtemp, open, rm } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { test } from "node:test";
import { startGuest } from "../src/guest.js";
import { importFiles } from "../src/import.js";
import { DEFAULT_TIMEOUT_MS, buildImage, hostToolError, missingTools } from "../src/pipeline.js";
import { typeLine } from "./helpers.js";

const PROMPT = "genesis:/home> ";

async function setup() {
  const missing = missingTools(["qemu-system-x86_64"]);
  if (missing.length > 0) {
    throw hostToolError(missing);
  }
  const dir = await mkdtemp(path.join(os.tmpdir(), "genesis-folders-"));
  const disk = path.join(dir, "disk.img");
  const image = await buildImage();
  const boot = (from: "cd" | "disk" = "cd") =>
    startGuest({ isoPath: from === "cd" ? image.isoPath : undefined, display: "none", diskPath: disk, boot: from });
  return { dir, disk, boot };
}

type Guest = Awaited<ReturnType<typeof startGuest>>;
const ask = async (guest: Guest, cmd: string, answer: string | RegExp) => {
  const before = guest.serial.length;
  await typeLine(guest, cmd);
  if (typeof answer === "string") {
    await guest.waitForSerial(answer, DEFAULT_TIMEOUT_MS);
    return;
  }
  for (let i = 0; i < 200 && !answer.test(guest.serial.slice(before)); i++) {
    await new Promise((r) => setTimeout(r, 100));
  }
  assert.match(guest.serial.slice(before), answer);
};

test("folders: mkdir, cd, pwd, ls, paths, mv and cp survive a reboot", { timeout: 180_000 }, async () => {
  const { dir, boot } = await setup();
  try {
    const g1 = await boot("cd");
    try {
      await g1.waitForSerial(PROMPT, DEFAULT_TIMEOUT_MS);
      await ask(g1, "pwd", "\n/home\n");
      await ask(g1, "ls /", "ls: bin/ lib/ docs/ home/");
      await ask(g1, "ls /bin", /ls:[^\n]*hello[^\n]*ticker/);
      await ask(g1, "mkdir proj", "mkdir: /home/proj");
      await ask(g1, "mkdir proj/src", "mkdir: /home/proj/src");
      await ask(g1, "mkdir nowhere/x", "mkdir: no parent folder");
      await ask(g1, "cd proj/src", "genesis:/home/proj/src> ");
      await ask(g1, "pwd", "\n/home/proj/src\n");
      await ask(g1, "write main.c int-main", "write: main.c");
      await ask(g1, "cd ..", "genesis:/home/proj> ");
      await ask(g1, "ls", "ls: src/");
      await ask(g1, "cat ./src/../src/main.c", "cat: int-main");
      await ask(g1, "write /home/proj/notes.txt a-long-file-name-is-fine-now", "write: /home/proj/notes.txt");
      await ask(g1, "cp notes.txt src", "cp: /home/proj/src/notes.txt");
      await ask(g1, "mv src/notes.txt /docs/moved-notes-long-name.txt", "mv: /docs/moved-notes-long-name.txt");
      await ask(g1, "ls src", /ls: main\.c\n/);
      await ask(g1, "rmdir src", "rmdir: not empty");
      await ask(g1, "rm src/main.c", "rm: src/main.c");
      await ask(g1, "rmdir src", "rmdir: /home/proj/src");
      await ask(g1, "cd /nope", "cd: no such folder");
      await ask(g1, "run hello", "hello from program");
      await ask(g1, "cd", PROMPT);
    } finally {
      await g1.stop();
    }
    const g2 = await boot("disk");
    try {
      await g2.waitForSerial(PROMPT, DEFAULT_TIMEOUT_MS);
      await ask(g2, "ls proj", "ls: notes.txt\n");
      await ask(g2, "cat /docs/moved-notes-long-name.txt", "cat: a-long-file-name-is-fine-now");
      await ask(g2, "cat proj/notes.txt", "cat: a-long-file-name-is-fine-now");
      await ask(g2, "cat /home/proj/src/main.c", "cat: no such file");
    } finally {
      await g2.stop();
    }
  } finally {
    await rm(dir, { recursive: true, force: true });
  }
});

test("Grid Store holds 100+ files and a ~900 KB file across reboots", { timeout: 240_000 }, async () => {
  const { dir, disk, boot } = await setup();
  try {
    const lines = ["mkdir /home/many"];
    for (let i = 0; i < 120; i++) {
      lines.push(`write /home/many/file-${String(i).padStart(3, "0")} v${i}`);
    }
    const big = Buffer.concat([Buffer.alloc(900_000, "abcdefghi\n"), Buffer.from("tail-marker-ok\n")]);
    const imported = await importFiles(disk, [
      { name: "mk.sh", data: Buffer.from(lines.join("\n") + "\n") },
      { name: "big.txt", data: big },
    ]);
    assert.deepEqual(imported.imported, ["mk.sh", "big.txt"]);

    const g1 = await boot("cd");
    try {
      await g1.waitForSerial(PROMPT, DEFAULT_TIMEOUT_MS);
      await ask(g1, "sh mk.sh", "write: /home/many/file-119");
      await ask(g1, "ls many | wc", /\n1 121 \d+\n/);
      await ask(g1, "cp big.txt /docs/big2.txt", "cp: /docs/big2.txt");
    } finally {
      await g1.stop();
    }
    const g2 = await boot("disk");
    try {
      await g2.waitForSerial(PROMPT, DEFAULT_TIMEOUT_MS);
      await ask(g2, "cat many/file-000", "cat: v0");
      await ask(g2, "cat many/file-119", "cat: v119");
      await ask(g2, "grep tail-marker /docs/big2.txt", "\ntail-marker-ok");
      await ask(g2, "ls /docs", /ls:[^\n]*big2\.txt/);
    } finally {
      await g2.stop();
    }
  } finally {
    await rm(dir, { recursive: true, force: true });
  }
});

test("an old flat GBF1 disk is migrated, user files kept in /home", { timeout: 120_000 }, async () => {
  const { dir, disk, boot } = await setup();
  try {
    // Hand-made GBF1: 8 MiB, catalog at LBA 34 (name[16], len, chunks[16]), data at LBA 2048.
    const fh = await open(disk, "w+");
    try {
      await fh.truncate(8 * 1024 * 1024);
      const sb = Buffer.alloc(512);
      sb.write("GBF1", 0, "latin1");
      sb.writeUInt32LE(1792, 4);
      const bitmap = Buffer.alloc(512);
      bitmap[0] = 0b11;
      const cat = Buffer.alloc(4 * 512);
      const put = (slot: number, name: string, data: Buffer, block: number) => {
        const off = slot * 84;
        cat.write(name, off, "latin1");
        cat.writeUInt32LE(data.length, off + 16);
        for (let k = 0; k < 16; k++) cat.writeUInt32LE(k === 0 ? block : 0xffffffff, off + 20 + k * 4);
        return fh.write(data, 0, data.length, (2048 + block * 8) * 512);
      };
      await put(0, "mine.txt", Buffer.from("old-flat-file"), 0);
      await put(1, "hello", Buffer.from("stale"), 1);
      await fh.write(bitmap, 0, 512, 33 * 512);
      await fh.write(cat, 0, cat.length, 34 * 512);
      await fh.write(sb, 0, 512, 32 * 512);
    } finally {
      await fh.close();
    }
    const g1 = await boot("cd");
    try {
      await g1.waitForSerial("disk: migrated GBF1 -> GBF2, 001 files kept", DEFAULT_TIMEOUT_MS);
      await g1.waitForSerial(PROMPT, DEFAULT_TIMEOUT_MS);
      await ask(g1, "cat /home/mine.txt", "cat: old-flat-file");
      await ask(g1, "run hello", "hello from program");
    } finally {
      await g1.stop();
    }
    const g2 = await boot("disk");
    try {
      await g2.waitForSerial(PROMPT, DEFAULT_TIMEOUT_MS);
      assert.doesNotMatch(g2.serial, /migrated/);
      await ask(g2, "cat mine.txt", "cat: old-flat-file");
    } finally {
      await g2.stop();
    }
  } finally {
    await rm(dir, { recursive: true, force: true });
  }
});
