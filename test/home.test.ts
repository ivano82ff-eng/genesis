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
import { click, countOf, delay, pixel, typeLine } from "./helpers.js";

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
      await memory.waitForSerial("genesis:/home> ", DEFAULT_TIMEOUT_MS);
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
      await files.waitForSerial("genesis:/home> ", DEFAULT_TIMEOUT_MS);
      await typeLine(files, "desktop");
      await files.waitForSerial("home: ready", DEFAULT_TIMEOUT_MS);
      await click(files, 200, 220);
      await files.waitForSerial("ls: ", DEFAULT_TIMEOUT_MS);
      assert.match(files.serial, /readme/);
      assert.match(files.serial, /Назад/);
      // /home: row 0 is "..", row 1 is readme.
      await click(files, 200, 208);
      await files.waitForSerial("txt: welcome to genesis", DEFAULT_TIMEOUT_MS);
    } finally {
      await files.stop();
    }

    const editor = await startGuest({ isoPath: image.isoPath, display: "none" });
    try {
      await editor.waitForSerial("genesis:/home> ", DEFAULT_TIMEOUT_MS);
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
