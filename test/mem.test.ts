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
  "mem, alloc, and heap report the memory map",
  { timeout: 90_000 },
  async () => {
    const missing = missingTools(["qemu-system-x86_64"]);
    if (missing.length > 0) {
      throw hostToolError(missing);
    }

    const image = await buildImage();
    const guest = await startGuest({ isoPath: image.isoPath, display: "none" });
    try {
      await guest.waitForSerial("genesis:/home> ", DEFAULT_TIMEOUT_MS);
      assert.match(guest.serial, new RegExp(BOOT_MARKER));

      await typeLine(guest, "mem");
      await guest.waitForSerial("usable ", DEFAULT_TIMEOUT_MS);
      assert.match(guest.serial, /usable \d+ MiB/);
      assert.match(guest.serial, /map 0x[0-9a-f]+ 0x[0-9a-f]+ type 1/);

      await typeLine(guest, "alloc 16");
      await guest.waitForSerial("alloc: 0x0000000000200000", DEFAULT_TIMEOUT_MS);

      await typeLine(guest, "heap");
      await guest.waitForSerial("heap: used 16 left 1048560", DEFAULT_TIMEOUT_MS);
    } finally {
      await guest.stop();
    }
  },
);
