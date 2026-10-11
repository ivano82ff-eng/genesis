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
import { click, typeLine } from "./helpers.js";
import { CELL } from "./ui.js";

function lightCount(ppm: Buffer, x: number, y: number, w: number, h: number): number {
  const image = parsePpm(ppm);
  let count = 0;
  for (let row = y; row < y + h; row++) {
    for (let col = x; col < x + w; col++) {
      const pixel = (row * image.width + col) * 3;
      const sum = image.pixels[pixel]! + image.pixels[pixel + 1]! + image.pixels[pixel + 2]!;
      if (sum > 400) {
        count++;
      }
    }
  }
  return count;
}

test(
  "Редактор opens the full-screen editor with readme text",
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
      await typeLine(guest, "desktop");
      await guest.waitForSerial("home: ready", DEFAULT_TIMEOUT_MS);
      await click(guest, CELL.editor.x, CELL.editor.y);
      await guest.waitForSerial("edit: welcome to genesis", DEFAULT_TIMEOUT_MS);
      // Full-screen editor on the framebuffer: line 1 holds the readme text.
      await new Promise((r) => setTimeout(r, 300));
      const lights = lightCount(await guest.screendump(), 32, 9, 400, 30);
      assert.ok(lights > 30, `текст редактора не виден (${lights})`);
    } finally {
      await guest.stop();
    }
  },
);
