import assert from "node:assert/strict";
import { test } from "node:test";
import { startGuest, type Guest } from "../src/guest.js";
import {
  BOOT_MARKER,
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

async function click(guest: Guest, x: number, y: number): Promise<void> {
  let left = x;
  let top = y;
  while (left > 0 || top > 0) {
    const stepX = Math.min(40, left);
    const stepY = Math.min(40, top);
    await guest.monitor(`mouse_move ${stepX} ${stepY}`);
    left -= stepX;
    top -= stepY;
    await delay(200);
  }
  try {
    await guest.monitor("mouse_button 1");
    await delay(80);
    await guest.monitor("mouse_button 0");
  } catch {
    // poweroff closes QMP before the button reply comes back
  }
}

function countOf(text: string, needle: string): number {
  let count = 0;
  let from = 0;
  while (from < text.length) {
    const at = text.indexOf(needle, from);
    if (at < 0) {
      return count;
    }
    count += 1;
    from = at + needle.length;
  }
  return count;
}

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
