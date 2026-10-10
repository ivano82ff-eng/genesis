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
import { typeLine } from "./type-line.js";

function lightPixels(image: ReturnType<typeof parsePpm>, x: number, y: number, w: number, h: number): number {
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
  "the home screen draws one button for each section",
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
      assert.match(guest.serial, new RegExp(BOOT_MARKER));
      await typeLine(guest, "desktop");
      await guest.waitForSerial("home: ready", DEFAULT_TIMEOUT_MS);
      assert.match(guest.serial, /Память/);
      assert.match(guest.serial, /Открыть/);
      assert.equal(guest.serial.includes("Назад"), false);

      const parsed = parsePpm(await guest.screendump());
      assert.equal(parsed.width, 1024);
      assert.equal(parsed.height, 768);
      const bands = [64, 168, 272, 376, 480, 584];
      for (const top of bands) {
        const count = lightPixels(parsed, 48, top + 20, 200, 24);
        assert.ok(count > 20, `кнопка y=${top} почти пустая (${count})`);
      }
    } finally {
      await guest.stop();
    }
  },
);
