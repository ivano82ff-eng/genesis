import assert from "node:assert/strict";
import { test } from "node:test";
import { startGuest, type Guest } from "../src/guest.js";
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

function pixel(image: ReturnType<typeof parsePpm>, x: number, y: number): [number, number, number] {
  const index = (y * image.width + x) * 3;
  return [image.pixels[index]!, image.pixels[index + 1]!, image.pixels[index + 2]!];
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
  await guest.monitor("mouse_button 1");
  await delay(80);
  await guest.monitor("mouse_button 0");
}

test(
  "home buttons open memory, files, and the editor",
  { timeout: 90_000 },
  async () => {
    const missing = missingTools(["qemu-system-x86_64"]);
    if (missing.length > 0) {
      throw hostToolError(missing);
    }

    const image = await buildImage();

    const memory = await startGuest({ isoPath: image.isoPath, display: "none" });
    try {
      await memory.waitForSerial("genesis> ", DEFAULT_TIMEOUT_MS);
      await typeLine(memory, "desktop");
      await memory.waitForSerial("home: ready", DEFAULT_TIMEOUT_MS);
      assert.match(memory.serial, /Память/);
      assert.match(memory.serial, /Файлы/);
      assert.match(memory.serial, /Редактор/);
      assert.match(memory.serial, /Открыть/);
      assert.match(memory.serial, /Перезагрузка/);
      assert.match(memory.serial, /Выключить/);
      const home = parsePpm(await memory.screendump());
      const button = pixel(home, 40, 100);
      const background = pixel(home, 8, 8);
      assert.ok(button[0] + button[1] + button[2] > background[0] + background[1] + background[2] + 40);
      await click(memory, 200, 110);
      await memory.waitForSerial("usable ", DEFAULT_TIMEOUT_MS);
      assert.match(memory.serial, /usable \d+ MiB/);
      assert.match(memory.serial, /Назад/);
      const beforeBack = countOf(memory.serial, "home: ready");
      await click(memory, 80, 40);
      const deadline = Date.now() + DEFAULT_TIMEOUT_MS;
      while (countOf(memory.serial, "home: ready") <= beforeBack) {
        if (Date.now() > deadline) {
          throw new Error(`«Назад» не вернул домой\n${memory.serial}`);
        }
        await delay(50);
      }
    } finally {
      await memory.stop();
    }

    const files = await startGuest({ isoPath: image.isoPath, display: "none" });
    try {
      await files.waitForSerial("genesis> ", DEFAULT_TIMEOUT_MS);
      await typeLine(files, "desktop");
      await files.waitForSerial("home: ready", DEFAULT_TIMEOUT_MS);
      await click(files, 200, 220);
      await files.waitForSerial("ls: ", DEFAULT_TIMEOUT_MS);
      assert.match(files.serial, /readme/);
      assert.match(files.serial, /Назад/);
      await click(files, 200, 148);
      await files.waitForSerial("txt: welcome to genesis", DEFAULT_TIMEOUT_MS);
    } finally {
      await files.stop();
    }

    const editor = await startGuest({ isoPath: image.isoPath, display: "none" });
    try {
      await editor.waitForSerial("genesis> ", DEFAULT_TIMEOUT_MS);
      await typeLine(editor, "desktop");
      await editor.waitForSerial("home: ready", DEFAULT_TIMEOUT_MS);
      await click(editor, 200, 320);
      await editor.waitForSerial("edit: welcome to genesis", DEFAULT_TIMEOUT_MS);
      assert.match(editor.serial, /Назад/);
    } finally {
      await editor.stop();
    }
  },
);
