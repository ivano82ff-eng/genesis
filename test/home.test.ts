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
import { CELL, CLOSE, CORE, fileRow } from "./ui.js";

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
      assert.match(memory.serial, /Терминал/);
      assert.match(memory.serial, /Пакеты/);
      assert.match(memory.serial, /Настройки/);
      const home = parsePpm(await memory.screendump());
      const button = pixel(home, CORE.x, CORE.y - 112);
      const background = pixel(home, 8, 8);
      assert.ok(button[0] + button[1] + button[2] > background[0] + background[1] + background[2] + 40);
      await click(memory, CELL.memory.x, CELL.memory.y);
      await memory.waitForSerial("usable ", DEFAULT_TIMEOUT_MS);
      assert.match(memory.serial, /usable \d+ MiB/);
      assert.match(memory.serial, /Назад/);
      const beforeBack = countOf(memory.serial, "home: ready");
      await click(memory, CLOSE.x, CLOSE.y);
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
      await click(files, CELL.files.x, CELL.files.y);
      await files.waitForSerial("ls: ", DEFAULT_TIMEOUT_MS);
      assert.match(files.serial, /readme/);
      assert.match(files.serial, /Назад/);
      // /home: row 0 is "..", row 1 is readme.
      await click(files, fileRow(1).x, fileRow(1).y);
      await files.waitForSerial("txt: welcome to genesis", DEFAULT_TIMEOUT_MS);
    } finally {
      await files.stop();
    }

    const editor = await startGuest({ isoPath: image.isoPath, display: "none" });
    try {
      await editor.waitForSerial("genesis:/home> ", DEFAULT_TIMEOUT_MS);
      await typeLine(editor, "desktop");
      await editor.waitForSerial("home: ready", DEFAULT_TIMEOUT_MS);
      await click(editor, CELL.editor.x, CELL.editor.y);
      await editor.waitForSerial("edit: welcome to genesis", DEFAULT_TIMEOUT_MS);
    } finally {
      await editor.stop();
    }
  },
);
