import assert from "node:assert/strict";
import { writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { test } from "node:test";
import { startGuest } from "../src/guest.js";
import {
  DEFAULT_TIMEOUT_MS,
  buildImage,
  hostToolError,
  missingTools,
} from "../src/pipeline.js";
import { typeLine } from "./helpers.js";

test(
  "a file written to the raw disk is still there after reboot",
  { timeout: 90_000 },
  async () => {
    const missing = missingTools(["qemu-system-x86_64"]);
    if (missing.length > 0) {
      throw hostToolError(missing);
    }

    const disk = path.join(os.tmpdir(), `genesis-persist-${process.pid}-${Date.now()}.img`);
    await writeFile(disk, Buffer.alloc(8 * 1024 * 1024));
    const image = await buildImage();

    const first = await startGuest({
      isoPath: image.isoPath,
      display: "none",
      diskPath: disk,
    });
    try {
      await first.waitForSerial("genesis:/home> ", DEFAULT_TIMEOUT_MS);
      await typeLine(first, "write note persist me");
      await first.waitForSerial("write: note", DEFAULT_TIMEOUT_MS);
    } finally {
      await first.stop();
    }

    const second = await startGuest({
      isoPath: image.isoPath,
      display: "none",
      diskPath: disk,
    });
    try {
      await second.waitForSerial("genesis:/home> ", DEFAULT_TIMEOUT_MS);
      await typeLine(second, "cat note");
      await second.waitForSerial("cat: persist me", DEFAULT_TIMEOUT_MS);
      await typeLine(second, "cat readme");
      await second.waitForSerial("cat: welcome to genesis", DEFAULT_TIMEOUT_MS);
    } finally {
      await second.stop();
    }
  },
);
