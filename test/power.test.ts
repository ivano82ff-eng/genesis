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
import { typeLine } from "./type-line.js";

function delay(ms: number): Promise<void> {
  return new Promise((resolve) => {
    setTimeout(resolve, ms);
  });
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
  "poweroff makes QEMU exit",
  { timeout: 60_000 },
  async () => {
    const missing = missingTools(["qemu-system-x86_64"]);
    if (missing.length > 0) {
      throw hostToolError(missing);
    }
    const image = await buildImage();
    const guest = await startGuest({ isoPath: image.isoPath, display: "none" });
    try {
      await guest.waitForSerial("genesis> ", DEFAULT_TIMEOUT_MS);
      await typeLine(guest, "poweroff");
      await guest.waitForSerial("poweroff", DEFAULT_TIMEOUT_MS);
      await Promise.race([
        guest.waitUntilClosed(),
        delay(15_000).then(() => {
          throw new Error(`QEMU не завершился после poweroff\n${guest.serial}`);
        }),
      ]);
    } finally {
      await guest.stop();
    }
  },
);

test(
  "reboot boots again and prints the marker",
  { timeout: 60_000 },
  async () => {
    const missing = missingTools(["qemu-system-x86_64"]);
    if (missing.length > 0) {
      throw hostToolError(missing);
    }
    const image = await buildImage();
    const guest = await startGuest({ isoPath: image.isoPath, display: "none" });
    try {
      await guest.waitForSerial(BOOT_MARKER, DEFAULT_TIMEOUT_MS);
      assert.equal(countOf(guest.serial, BOOT_MARKER), 1);
      await guest.waitForSerial("genesis> ", DEFAULT_TIMEOUT_MS);
      await typeLine(guest, "reboot");
      const deadline = Date.now() + DEFAULT_TIMEOUT_MS;
      while (countOf(guest.serial, BOOT_MARKER) < 2) {
        if (Date.now() > deadline) {
          throw new Error(`после reboot маркер один\n${guest.serial}`);
        }
        await delay(50);
      }
      assert.ok(guest.serial.includes("reboot"));
    } finally {
      await guest.stop();
    }
  },
);
