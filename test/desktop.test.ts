import assert from "node:assert/strict";
import { mkdirSync, writeFileSync } from "node:fs";
import path from "node:path";
import { test } from "node:test";
import { startGuest } from "../src/guest.js";
import { parsePpm, ppmToPng } from "../src/png.js";
import {
  BOOT_MARKER,
  DEFAULT_TIMEOUT_MS,
  buildImage,
  hostToolError,
  missingTools,
} from "../src/pipeline.js";
import { pixel, typeLine } from "./helpers.js";
import { CELL, CORE } from "./ui.js";

function bright([r, g, b]: [number, number, number]): number {
  return r + g + b;
}

/** Light pixels on the outline ring of a hexagon cell (its glow is cyan/amber). */
function outlineLit(image: ReturnType<typeof parsePpm>, cx: number, cy: number, r: number): boolean {
  for (let dy = -2; dy <= 2; dy++) {
    if (bright(pixel(image, cx, cy - r + 1 + dy)) > 300) {
      return true;
    }
  }
  return false;
}

test(
  "Ядро: core hexagon in the middle, a ring of cells, the command bar",
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
      assert.match(guest.serial, /cells: Файлы Редактор Терминал Пакеты Память Лента Настройки\n/);
      assert.match(guest.serial, /core: mem \d+% tasks 0/);
      assert.equal(guest.serial.includes("Назад"), false);

      const ppm = await guest.screendump();
      const parsed = parsePpm(ppm);
      assert.equal(parsed.width, 1024);
      assert.equal(parsed.height, 768);
      // README screenshot of the landscape desktop.
      const docs = path.join(process.cwd(), "docs");
      mkdirSync(docs, { recursive: true });
      writeFileSync(path.join(docs, "ui-core.png"), ppmToPng(ppm));

      assert.ok(outlineLit(parsed, CORE.x, CORE.y, 112), "контур ядра не светится");
      for (const [name, c] of Object.entries(CELL)) {
        if (name.startsWith("extra")) {
          continue;
        }
        assert.ok(outlineLit(parsed, c.x, c.y, 52), `соты ${name} не видно`);
      }
      // Background is deep navy, not black and not grey.
      const [r, g, b] = pixel(parsed, 900, 300);
      assert.ok(b > r && b > g && b > 20, `фон не тёмно-синий: ${r},${g},${b}`);
    } finally {
      await guest.stop();
    }
  },
);
