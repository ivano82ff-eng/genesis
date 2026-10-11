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
import { screenTextError } from "../src/vga-text.js";
import { delay } from "./helpers.js";

const PROMPT = "genesis:/home> ";
const HELP_REPLY = "commands: help echo clear mem alloc heap ls cat write rm run spawn ps desktop";

test(
  "help replies on serial and on the VGA screen",
  { timeout: 90_000 },
  async () => {
    const missing = missingTools(["qemu-system-x86_64"]);
    if (missing.length > 0) {
      throw hostToolError(missing);
    }

    const image = await buildImage();
    const guest = await startGuest({ isoPath: image.isoPath, display: "none" });
    try {
      await guest.waitForSerial(PROMPT, DEFAULT_TIMEOUT_MS);
      assert.match(guest.serial, new RegExp(BOOT_MARKER));
      for (const key of ["h", "e", "l", "p", "ret"]) {
        await guest.sendKey(key);
        await delay(40);
      }
      await guest.waitForSerial(HELP_REPLY, DEFAULT_TIMEOUT_MS);
      const ppm = await guest.screendump();
      assert.equal(screenTextError(ppm, BOOT_MARKER), null);
      assert.equal(screenTextError(ppm, HELP_REPLY, 2), null);
    } finally {
      await guest.stop();
    }
  },
);
