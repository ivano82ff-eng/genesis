import assert from "node:assert/strict";
import { mkdir, mkdtemp, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { test } from "node:test";
import { startGuest } from "../src/guest.js";
import { importFiles, importFolder } from "../src/import.js";
import { DEFAULT_TIMEOUT_MS, buildImage, hostToolError, missingTools } from "../src/pipeline.js";
import { typeLine } from "./helpers.js";

test("files from an import folder land on the genesis disk", { timeout: 180_000 }, async () => {
  const missing = missingTools(["qemu-system-x86_64"]);
  if (missing.length > 0) {
    throw hostToolError(missing);
  }
  const dir = await mkdtemp(path.join(os.tmpdir(), "genesis-import-"));
  const folder = path.join(dir, "import");
  await mkdir(folder);
  await writeFile(path.join(folder, "my.txt"), "hello-import\nПривет, genesis\n");
  await writeFile(path.join(folder, "this-name-is-too-long.txt"), "x");
  await writeFile(path.join(folder, "big.txt"), Buffer.alloc(70_000, 0x61));
  const disk = path.join(dir, "disk.img");

  // Blank disk: the importer creates the store, the kernel adds built-ins.
  const first = await importFolder(disk, folder);
  assert.deepEqual(first.imported, ["my.txt"]);
  assert.deepEqual(first.skipped.map((s) => s.name).sort(), ["big.txt", "this-name-is-too-long.txt"]);

  const image = await buildImage();
  const boot = async () => startGuest({ isoPath: image.isoPath, display: "none", diskPath: disk, boot: "cd" });
  const ask = async (guest: Awaited<ReturnType<typeof boot>>, cmd: string, answer: string) => {
    await typeLine(guest, cmd);
    await guest.waitForSerial(answer, DEFAULT_TIMEOUT_MS);
  };

  const g1 = await boot();
  try {
    await g1.waitForSerial("genesis> ", DEFAULT_TIMEOUT_MS);
    await ask(g1, "ls", "my.txt");
    await ask(g1, "cat my.txt", "cat: hello-import");
    await g1.waitForSerial("Привет, genesis", DEFAULT_TIMEOUT_MS);
    await ask(g1, "cat readme", "cat: welcome to genesis");
    await ask(g1, "ls", "ticker");
    await ask(g1, "write keep kept", "write: keep");
  } finally {
    await g1.stop();
  }

  // Existing disk: same name is replaced, other files stay.
  const second = await importFiles(disk, [{ name: "my.txt", data: Buffer.from("second-version") }]);
  assert.deepEqual(second.imported, ["my.txt"]);
  const g2 = await boot();
  try {
    await g2.waitForSerial("genesis> ", DEFAULT_TIMEOUT_MS);
    await ask(g2, "cat my.txt", "cat: second-version");
    await ask(g2, "cat keep", "cat: kept");
    await ask(g2, "cat readme", "cat: welcome to genesis");
  } finally {
    await g2.stop();
  }
});