import assert from "node:assert/strict";
import { test } from "node:test";
import { startGuest } from "../src/guest.js";
import {
  BOOT_MARKER,
  DEFAULT_TIMEOUT_MS,
  buildImage,
  hostToolError,
  missingTools,
} from "../src/pipeline.js";
import { typeLine } from "./helpers.js";

test(
  "ls, cat, and write use the RAM filesystem",
  { timeout: 90_000 },
  async () => {
    const missing = missingTools(["qemu-system-x86_64"]);
    if (missing.length > 0) {
      throw hostToolError(missing);
    }

    const image = await buildImage();
    const guest = await startGuest({ isoPath: image.isoPath, display: "none" });
    try {
      await guest.waitForSerial("genesis> ", DEFAULT_TIMEOUT_MS);
      assert.match(guest.serial, new RegExp(BOOT_MARKER));

      await typeLine(guest, "ls");
      await guest.waitForSerial("ls: readme hello", DEFAULT_TIMEOUT_MS);

      await typeLine(guest, "cat readme");
      await guest.waitForSerial("cat: welcome to genesis", DEFAULT_TIMEOUT_MS);

      await typeLine(guest, "write note hi");
      await guest.waitForSerial("write: note", DEFAULT_TIMEOUT_MS);

      await typeLine(guest, "cat note");
      await guest.waitForSerial("cat: hi", DEFAULT_TIMEOUT_MS);

      await typeLine(guest, "ls");
      await guest.waitForSerial("clip.mp4 note", DEFAULT_TIMEOUT_MS);
    } finally {
      await guest.stop();
    }
  },
);
