import assert from "node:assert/strict";
import { test } from "node:test";
import { startGuest } from "../src/guest.js";
import { parsePpm } from "../src/png.js";
import {
  DEFAULT_TIMEOUT_MS,
  buildImage,
  hostToolError,
  missingTools,
} from "../src/pipeline.js";
import { typeLine } from "./type-line.js";

function delay(ms: number): Promise<void> {
  return new Promise((resolve) => {
    setTimeout(resolve, ms);
  });
}

test(
  "the desktop clock changes between two captures",
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
      const first = await guest.screendump();
      await guest.waitForSerial("time 00:00:01", DEFAULT_TIMEOUT_MS);
      await delay(200);
      const second = await guest.screendump();
      const a = parsePpm(first);
      const b = parsePpm(second);
      assert.equal(a.width, b.width);
      assert.equal(a.height, b.height);
      assert.equal(a.pixels.equals(b.pixels), false);
      assert.match(guest.serial, /time 00:00:01/);
    } finally {
      await guest.stop();
    }
  },
);
