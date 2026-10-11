import assert from "node:assert/strict";
import { test } from "node:test";
import { startGuest } from "../src/guest.js";
import {
  BOOT_MARKER,
  DEFAULT_TIMEOUT_MS,
  buildImage,
  hostToolError,
  missingTools,
  runGuest,
  saveSerialLog,
} from "../src/pipeline.js";
import { screenShowsChar, screenTextError } from "../src/vga-text.js";
import { delay } from "./helpers.js";

test(
  "COM1 prints the genesis boot marker",
  { timeout: 90_000 },
  async () => {
    const missing = missingTools(["qemu-system-x86_64"]);
    if (missing.length > 0) {
      throw hostToolError(missing);
    }

    const image = await buildImage();
    const result = await runGuest({
      isoPath: image.isoPath,
      timeoutMs: DEFAULT_TIMEOUT_MS,
    });
    await saveSerialLog(result.serial);

    assert.equal(
      result.markerFound,
      true,
      [
        `? serial ??? ?????? ?${BOOT_MARKER}?`,
        `timeout=${result.timedOut} qemuExit=${result.qemuExitCode}`,
        "--- serial ---",
        result.serial,
        "--- qemu stderr ---",
        result.qemuStderr,
      ].join("\n"),
    );
  },
);

test(
  "VGA text screen shows the genesis boot marker",
  { timeout: 90_000 },
  async () => {
    const missing = missingTools(["qemu-system-x86_64"]);
    if (missing.length > 0) {
      throw hostToolError(missing);
    }

    const image = await buildImage();
    const guest = await startGuest({ isoPath: image.isoPath, display: "none" });
    try {
      await guest.waitForSerial(BOOT_MARKER, DEFAULT_TIMEOUT_MS);
      const ppm = await guest.screendump();
      const error = screenTextError(ppm, BOOT_MARKER);
      assert.equal(error, null, error ?? "");
    } finally {
      await guest.stop();
    }
  },
);

test(
  "a PS/2 key shows up on the VGA screen",
  { timeout: 90_000 },
  async () => {
    const missing = missingTools(["qemu-system-x86_64"]);
    if (missing.length > 0) {
      throw hostToolError(missing);
    }

    const image = await buildImage();
    const guest = await startGuest({ isoPath: image.isoPath, display: "none" });
    try {
      await guest.waitForSerial(BOOT_MARKER, DEFAULT_TIMEOUT_MS);
      await delay(100);
      const before = await guest.screendump();
      assert.equal(screenTextError(before, BOOT_MARKER), null);
      assert.equal(screenShowsChar(before, "a"), false);

      await guest.sendKey("a");
      const deadline = Date.now() + 2000;
      let after = before;
      while (Date.now() < deadline) {
        await delay(40);
        after = await guest.screendump();
        if (screenShowsChar(after, "a")) {
          break;
        }
      }
      assert.equal(screenShowsChar(after, "a"), true);
      assert.equal(screenTextError(after, BOOT_MARKER), null);
    } finally {
      await guest.stop();
    }
  },
);
