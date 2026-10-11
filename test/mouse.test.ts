import assert from "node:assert/strict";
import { test } from "node:test";
import { startGuest } from "../src/guest.js";
import { parsePpm } from "../src/png.js";
import {
  BOOT_MARKER,
  DEFAULT_TIMEOUT_MS,
  buildImage,
  hostToolError,
  missingTools,
} from "../src/pipeline.js";
import { delay, pixel, typeLine } from "./helpers.js";

function isYellow([r, g, b]: [number, number, number]): boolean {
  return r > 180 && g > 140 && b < 120;
}

function isCursor([r, g, b]: [number, number, number]): boolean {
  return r > 220 && g > 220 && b > 200;
}

test(
  "a PS/2 click opens a home section and the cursor stays drawn",
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
      await typeLine(guest, "desktop");
      await guest.waitForSerial("home: ready", DEFAULT_TIMEOUT_MS);

      const before = parsePpm(await guest.screendump());
      assert.equal(isYellow(pixel(before, 40, 482)), true);

      let left = 200;
      let top = 110;
      while (left > 0 || top > 0) {
        const stepX = Math.min(40, left);
        const stepY = Math.min(40, top);
        await guest.monitor(`mouse_move ${stepX} ${stepY}`);
        left -= stepX;
        top -= stepY;
        await delay(200);
      }
      await guest.monitor("mouse_button 1");
      await delay(80);
      await guest.monitor("mouse_button 0");
      await guest.waitForSerial("usable ", DEFAULT_TIMEOUT_MS);
      assert.match(guest.serial, /Назад/);

      /* show_memory arms the pointer at 0,0 so the next click can hit «Назад». */
      /* The guest idles with hlt between ticks, so the redraw can land just after the serial line. */
      let cursor = false;
      for (let attempt = 0; attempt < 20 && !cursor; attempt++) {
        if (attempt > 0) {
          await delay(250);
        }
        const after = parsePpm(await guest.screendump());
        for (let y = 0; y < 40 && !cursor; y++) {
          for (let x = 0; x < 40; x++) {
            if (isCursor(pixel(after, x, y))) {
              cursor = true;
              break;
            }
          }
        }
      }
      assert.equal(cursor, true);
    } finally {
      await guest.stop();
    }
  },
);
