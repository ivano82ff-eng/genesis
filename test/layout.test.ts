import { writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { test } from "node:test";
import { startGuest } from "../src/guest.js";
import { DEFAULT_TIMEOUT_MS, buildImage, hostToolError, missingTools } from "../src/pipeline.js";
import { delay, drag, longPress, typeLine } from "./helpers.js";
import { cellAt } from "./ui.js";

const DEFAULT = "cells: Файлы Редактор Терминал Пакеты Память Лента Настройки Браузер\n";

test("cells layout: move/remove/add commands, drag, keyboard edit, long press, kept on disk", { timeout: 115_000 }, async () => {
  const missing = missingTools(["qemu-system-x86_64"]);
  if (missing.length > 0) throw hostToolError(missing);
  const disk = path.join(os.tmpdir(), `genesis-layout-${process.pid}-${Date.now()}.img`);
  await writeFile(disk, Buffer.alloc(8 * 1024 * 1024));
  const image = await buildImage();
  const first = await startGuest({ isoPath: image.isoPath, display: "none", diskPath: disk });
  try {
    await first.waitForSerial("genesis:/home> ", DEFAULT_TIMEOUT_MS);
    await typeLine(first, "desktop");
    await first.waitForSerial(DEFAULT, DEFAULT_TIMEOUT_MS);
    await typeLine(first, "layout move web 1");
    await first.waitForSerial("cells: Браузер Файлы Редактор Терминал Пакеты Память Лента Настройки\n", DEFAULT_TIMEOUT_MS);
    await typeLine(first, "layout remove Память");
    await first.waitForSerial("cells: Браузер Файлы Редактор Терминал Пакеты Лента Настройки\n", DEFAULT_TIMEOUT_MS);
    await first.waitForSerial("layout: web files editor term pkg feed set (hidden: mem)", DEFAULT_TIMEOUT_MS);
    // Edit mode: drag Браузер (cell 0 of 7) onto cell 2.
    await typeLine(first, "layout edit");
    await first.waitForSerial("layout: edit on", DEFAULT_TIMEOUT_MS);
    const a = cellAt(0, 7);
    const b = cellAt(2, 7);
    await drag(first, a.x, a.y, b.x, b.y);
    await first.waitForSerial("cells: Файлы Редактор Браузер Терминал Пакеты Лента Настройки\n", DEFAULT_TIMEOUT_MS);
    // Keyboard / TV remote: Tab to the first cell, Enter picks it, Right moves it, Enter puts it down.
    for (let i = 0; i < 9 && !first.serial.endsWith("focus: Файлы\n"); i++) {
      await first.sendKey("tab");
      await delay(250);
    }
    await first.waitForSerial("focus: Файлы", DEFAULT_TIMEOUT_MS);
    await first.sendKey("ret");
    await first.waitForSerial("layout: picked files", DEFAULT_TIMEOUT_MS);
    await first.sendKey("right");
    await first.waitForSerial("cells: Редактор Файлы Браузер Терминал Пакеты Лента Настройки\n", DEFAULT_TIMEOUT_MS);
    await first.sendKey("ret");
    await first.waitForSerial("layout: placed files", DEFAULT_TIMEOUT_MS);
    await first.sendKey("esc");
    await first.waitForSerial("layout: edit off", DEFAULT_TIMEOUT_MS);
    // A long press on a cell starts the edit mode too (touch).
    const c = cellAt(3, 7);
    await longPress(first, c.x, c.y);
    await first.waitForSerial("layout: edit on", DEFAULT_TIMEOUT_MS);
    await first.sendKey("esc");
  } finally {
    await first.stop();
  }
  const second = await startGuest({ isoPath: image.isoPath, display: "none", diskPath: disk });
  try {
    await second.waitForSerial("genesis:/home> ", DEFAULT_TIMEOUT_MS);
    await typeLine(second, "desktop");
    await second.waitForSerial("cells: Редактор Файлы Браузер Терминал Пакеты Лента Настройки\n", DEFAULT_TIMEOUT_MS);
    await typeLine(second, "layout add mem");
    await second.waitForSerial("cells: Редактор Файлы Браузер Терминал Пакеты Лента Настройки Память\n", DEFAULT_TIMEOUT_MS);
    await typeLine(second, "layout reset");
    await second.waitForSerial(DEFAULT, DEFAULT_TIMEOUT_MS);
  } finally {
    await second.stop();
  }
});
