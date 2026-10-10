import assert from "node:assert/strict";
import { test } from "node:test";
import { startGuest } from "../src/guest.js";
import {
  DEFAULT_TIMEOUT_MS,
  buildImage,
  hostToolError,
  missingTools,
} from "../src/pipeline.js";
import { typeLine } from "./type-line.js";

test(
  "the clock advances on the home screen",
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
      await typeLine(guest, "desktop");
      await guest.waitForSerial("home: ready", DEFAULT_TIMEOUT_MS);
      await guest.waitForSerial("time 00:00:01", DEFAULT_TIMEOUT_MS);
      await guest.waitForSerial("time 00:00:02", DEFAULT_TIMEOUT_MS);
      assert.match(guest.serial, /time 00:00:01/);
      assert.match(guest.serial, /time 00:00:02/);
    } finally {
      await guest.stop();
    }
  },
);
