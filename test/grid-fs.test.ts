import assert from "node:assert/strict";
import { writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { test } from "node:test";
import { startGuest } from "../src/guest.js";
import { importFiles } from "../src/import.js";
import {
  BOOT_MARKER,
  DEFAULT_TIMEOUT_MS,
  buildImage,
  hostToolError,
  missingTools,
} from "../src/pipeline.js";
import { typeLine } from "./helpers.js";

test(
  "rm removes a file and the deletion survives reboot",
  { timeout: 120_000 },
  async () => {
    const missing = missingTools(["qemu-system-x86_64"]);
    if (missing.length > 0) {
      throw hostToolError(missing);
    }

    const disk = path.join(os.tmpdir(), `grid-rm-${process.pid}-${Date.now()}.img`);
    await writeFile(disk, Buffer.alloc(8 * 1024 * 1024));
    const image = await buildImage();

    const first = await startGuest({
      isoPath: image.isoPath,
      display: "none",
      diskPath: disk,
      boot: "cd",
    });
    try {
      await first.waitForSerial("genesis:/home> ", DEFAULT_TIMEOUT_MS);
      await typeLine(first, "write alpha grid-alpha");
      await first.waitForSerial("write: alpha", DEFAULT_TIMEOUT_MS);
      await typeLine(first, "cat alpha");
      await first.waitForSerial("cat: grid-alpha", DEFAULT_TIMEOUT_MS);
      await typeLine(first, "rm alpha");
      await first.waitForSerial("rm: alpha", DEFAULT_TIMEOUT_MS);
      await typeLine(first, "cat alpha");
      await first.waitForSerial("cat: no such file", DEFAULT_TIMEOUT_MS);
    } finally {
      await first.stop();
    }

    const second = await startGuest({
      display: "none",
      diskPath: disk,
      boot: "disk",
    });
    try {
      await second.waitForSerial(BOOT_MARKER, DEFAULT_TIMEOUT_MS);
      await second.waitForSerial("genesis:/home> ", DEFAULT_TIMEOUT_MS);
      await typeLine(second, "cat alpha");
      await second.waitForSerial("cat: no such file", DEFAULT_TIMEOUT_MS);
      await typeLine(second, "cat readme");
      await second.waitForSerial("cat: welcome to genesis", DEFAULT_TIMEOUT_MS);
    } finally {
      await second.stop();
    }
  },
);

test(
  "freed catalog slots and chunks are reused across reboot",
  { timeout: 180_000 },
  async () => {
    const missing = missingTools(["qemu-system-x86_64"]);
    if (missing.length > 0) {
      throw hostToolError(missing);
    }

    const disk = path.join(os.tmpdir(), `grid-reuse-${process.pid}-${Date.now()}.img`);
    await writeFile(disk, Buffer.alloc(8 * 1024 * 1024));
    // 256 catalog slots: 23 built-ins + 228 imports leave five for the writes below.
    const pad = Array.from({ length: 228 }, (_, i) => ({ name: `pad${i}`, data: Buffer.from("p") }));
    assert.equal((await importFiles(disk, pad)).imported.length, 228);
    const image = await buildImage();

    const fill = async (guest: Awaited<ReturnType<typeof startGuest>>): Promise<void> => {
      // Five writes fill the catalog.
      for (const [name, text] of [
        ["f1", "a1"],
        ["f2", "a2"],
        ["f3", "a3"],
        ["f4", "a4"],
        ["f5", "a5"],
      ] as const) {
        await typeLine(guest, `write ${name} ${text}`);
        await guest.waitForSerial(`write: ${name}`, DEFAULT_TIMEOUT_MS);
      }
      await typeLine(guest, "write f7 nope");
      await guest.waitForSerial("write: full", DEFAULT_TIMEOUT_MS);
      await typeLine(guest, "rm f2");
      await guest.waitForSerial("rm: f2", DEFAULT_TIMEOUT_MS);
      await typeLine(guest, "rm f4");
      await guest.waitForSerial("rm: f4", DEFAULT_TIMEOUT_MS);
      await typeLine(guest, "write g1 b1");
      await guest.waitForSerial("write: g1", DEFAULT_TIMEOUT_MS);
      await typeLine(guest, "write g2 b2");
      await guest.waitForSerial("write: g2", DEFAULT_TIMEOUT_MS);
      await typeLine(guest, "write g3 nope");
      await guest.waitForSerial("write: full", DEFAULT_TIMEOUT_MS);
    };

    const first = await startGuest({
      isoPath: image.isoPath,
      display: "none",
      diskPath: disk,
      boot: "cd",
    });
    try {
      await first.waitForSerial("genesis:/home> ", DEFAULT_TIMEOUT_MS);
      await fill(first);
    } finally {
      await first.stop();
    }

    const second = await startGuest({
      display: "none",
      diskPath: disk,
      boot: "disk",
    });
    try {
      await second.waitForSerial(BOOT_MARKER, DEFAULT_TIMEOUT_MS);
      await second.waitForSerial("genesis:/home> ", DEFAULT_TIMEOUT_MS);
      const expect = async (command: string, answer: string): Promise<void> => {
        await typeLine(second, command);
        await second.waitForSerial(answer, DEFAULT_TIMEOUT_MS);
      };
      await expect("cat f1", "cat: a1");
      await expect("cat f3", "cat: a3");
      await expect("cat g1", "cat: b1");
      await expect("cat g2", "cat: b2");
      await expect("cat f2", "cat: no such file");
      await expect("cat f4", "cat: no such file");
      await expect("cat readme", "cat: welcome to genesis");
    } finally {
      await second.stop();
    }
  },
);
