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
  "the editor shows readme text and Назад",
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
      let left = 200;
      let top = 320;
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
      await guest.waitForSerial("edit: welcome to genesis", DEFAULT_TIMEOUT_MS);
      assert.match(guest.serial, /Назад/);
      const lights = lightCount(await guest.screendump(), 48, 150, 400, 40);
      assert.ok(lights > 30, `текст редактора не виден (${lights})`);
    } finally {
      await guest.stop();
    }
  },
);
