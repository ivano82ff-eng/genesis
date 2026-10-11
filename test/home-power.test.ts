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
import { click, countOf, delay, typeLine } from "./helpers.js";

test(
  "home poweroff exits QEMU and reboot returns to the home screen",
  { timeout: 90_000 },
  async () => {
    const missing = missingTools(["qemu-system-x86_64"]);
    if (missing.length > 0) {
      throw hostToolError(missing);
    }

    const image = await buildImage();

    const off = await startGuest({ isoPath: image.isoPath, display: "none" });
    try {
      await off.waitForSerial("genesis> ", DEFAULT_TIMEOUT_MS);
      await typeLine(off, "desktop");
      await off.waitForSerial("home: ready", DEFAULT_TIMEOUT_MS);
      await click(off, 200, 640);
      await off.waitForSerial("poweroff", DEFAULT_TIMEOUT_MS);
      await off.waitUntilClosed();
    } finally {
      await off.stop();
    }

    const again = await startGuest({ isoPath: image.isoPath, display: "none" });
    try {
      await again.waitForSerial("genesis> ", DEFAULT_TIMEOUT_MS);
      await typeLine(again, "desktop");
      await again.waitForSerial("home: ready", DEFAULT_TIMEOUT_MS);
      assert.equal(countOf(again.serial, BOOT_MARKER), 1);
      await click(again, 200, 510);
      const deadline = Date.now() + DEFAULT_TIMEOUT_MS;
      while (countOf(again.serial, "home: ready") < 2 || countOf(again.serial, BOOT_MARKER) < 2) {
        if (Date.now() > deadline) {
          throw new Error(`после reboot нет второго домашнего экрана\n${again.serial}`);
        }
        await delay(50);
      }
      assert.ok(again.serial.includes("reboot"));
    } finally {
      await again.stop();
    }
  },
);
