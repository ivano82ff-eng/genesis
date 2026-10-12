import assert from "node:assert/strict";
import { mkdirSync, writeFileSync } from "node:fs";
import path from "node:path";
import { test } from "node:test";
import { startGuest, type Guest } from "../src/guest.js";
import { parsePpm, ppmToPng } from "../src/png.js";
import {
  DEFAULT_TIMEOUT_MS,
  buildImage,
  hostToolError,
  missingTools,
} from "../src/pipeline.js";
import { click, countOf, delay, pixel, typeLine } from "./helpers.js";
import { CELL, CORE, coreAt, stopButton } from "./ui.js";

async function desktop(size = ""): Promise<Guest> {
  const missing = missingTools(["qemu-system-x86_64"]);
  if (missing.length > 0) {
    throw hostToolError(missing);
  }
  const image = await buildImage();
  const guest = await startGuest({ isoPath: image.isoPath, display: "none" });
  await guest.waitForSerial("genesis:/home> ", DEFAULT_TIMEOUT_MS);
  await typeLine(guest, size ? `desktop ${size}` : "desktop");
  await guest.waitForSerial("home: ready", DEFAULT_TIMEOUT_MS);
  return guest;
}

async function key(guest: Guest, name: string): Promise<void> {
  await guest.sendKey(name);
  await delay(250);
}

/** Wait until TEXT occurs more than BEFORE times (default: now). */
async function next(guest: Guest, text: string, before = countOf(guest.serial, text)): Promise<void> {
  const deadline = Date.now() + DEFAULT_TIMEOUT_MS;
  while (countOf(guest.serial, text) <= before) {
    if (Date.now() > deadline) {
      throw new Error(`нет «${text}»\n${guest.serial.slice(-1500)}`);
    }
    await delay(50);
  }
}

test("core counts tasks; spawn adds one, «стоп» and kill stop them", { timeout: 120_000 }, async () => {
  const guest = await desktop();
  try {
    await guest.waitForSerial("core: mem ", DEFAULT_TIMEOUT_MS);
    // Real memory use from the Multiboot map: never 0%.
    assert.match(guest.serial, /core: mem ([1-9]\d*)% tasks 0/);
    await typeLine(guest, "spawn pulse");
    await guest.waitForSerial("% tasks 1", DEFAULT_TIMEOUT_MS);
    assert.match(guest.serial, /core: mem [1-9]\d*% tasks 1/);
    await guest.waitForSerial("pulse\n", DEFAULT_TIMEOUT_MS);
    // Click the core: the task panel lists the shell and pulse.
    const dockedCore = coreAt(true); // the terminal panel docked: the ring slid left
    await click(guest, dockedCore.x, dockedCore.y);
    await guest.waitForSerial("tasks: 0:shell 1:pulse", DEFAULT_TIMEOUT_MS);
    await delay(300);
    let zero = countOf(guest.serial, "tasks 0");
    await click(guest, stopButton(1).x, stopButton(1).y);
    await guest.waitForSerial("kill: stopped task 1", DEFAULT_TIMEOUT_MS);
    await next(guest, "tasks 0", zero);
    // kill from the bar as well.
    const one = countOf(guest.serial, "tasks 1");
    await typeLine(guest, "spawn pulse");
    await next(guest, "tasks 1", one);
    zero = countOf(guest.serial, "tasks 0");
    const killed = countOf(guest.serial, "kill: stopped task 1");
    await typeLine(guest, "kill 1");
    await next(guest, "kill: stopped task 1", killed);
    await next(guest, "tasks 0", zero);
    await typeLine(guest, "kill 0");
    await guest.waitForSerial("kill: the shell cannot be stopped", DEFAULT_TIMEOUT_MS);
    const pulses = countOf(guest.serial, "pulse\n");
    await delay(2500);
    assert.equal(countOf(guest.serial, "pulse\n"), pulses, "pulse всё ещё печатает");
  } finally {
    await guest.stop();
  }
});

test("pkg install adds a program cell, pin adds a folder cell, both go away again", { timeout: 120_000 }, async () => {
  const guest = await desktop();
  try {
    await typeLine(guest, "pkg install strdemo");
    await guest.waitForSerial("cells: Файлы Редактор Терминал Пакеты Память Лента Настройки Браузер strdemo\n", DEFAULT_TIMEOUT_MS);
    // The new cell runs the program; output lands in the terminal panel.
    await delay(300);
    await click(guest, CELL.extra1.x, CELL.extra1.y);
    await guest.waitForSerial("HELLO GENESIS", DEFAULT_TIMEOUT_MS);
    await delay(500);
    await typeLine(guest, "mkdir proj");
    await typeLine(guest, "pin proj");
    await guest.waitForSerial("cells: Файлы Редактор Терминал Пакеты Память Лента Настройки Браузер strdemo /home/proj\n", DEFAULT_TIMEOUT_MS);
    await delay(300);
    await click(guest, CELL.extra2.x, CELL.extra2.y);
    await guest.waitForSerial("ls: \n", DEFAULT_TIMEOUT_MS);
    await key(guest, "esc");
    const plain = countOf(guest.serial, "cells: Файлы Редактор Терминал Пакеты Память Лента Настройки Браузер\n");
    await typeLine(guest, "unpin proj");
    await typeLine(guest, "pkg remove strdemo");
    await guest.waitForSerial("pkg: removed strdemo", DEFAULT_TIMEOUT_MS);
    await next(guest, "cells: Файлы Редактор Терминал Пакеты Память Лента Настройки Браузер\n", plain);
  } finally {
    await guest.stop();
  }
});

test("command bar: hints, Tab completion, history, runs commands and edit", { timeout: 120_000 }, async () => {
  const guest = await desktop();
  try {
    await typeLine(guest, "echo привет");
    await guest.waitForSerial("genesis:/home> echo привет\nпривет", DEFAULT_TIMEOUT_MS);
    // Typing "pk" highlights Пакеты; Tab completes "pk" to "pkg ".
    for (const k of ["p", "k"]) {
      await key(guest, k);
    }
    await guest.waitForSerial("hint: Пакеты", DEFAULT_TIMEOUT_MS);
    await key(guest, "tab");
    await typeLine(guest, "list");
    await guest.waitForSerial("genesis:/home> pkg list\n", DEFAULT_TIMEOUT_MS);
    // PgUp recalls the last line (Up/Down on an empty bar move focus).
    const lists = countOf(guest.serial, "genesis:/home> pkg list\n");
    await key(guest, "pgup");
    await key(guest, "ret");
    await next(guest, "genesis:/home> pkg list\n", lists);
    // bar `edit` opens the full-screen editor; Esc returns home.
    await typeLine(guest, "edit /home/x.txt");
    await guest.waitForSerial("edit: new /home/x.txt", DEFAULT_TIMEOUT_MS);
    const homes = countOf(guest.serial, "home: ready");
    await key(guest, "esc");
    await guest.waitForSerial("edit: closed", DEFAULT_TIMEOUT_MS);
    await next(guest, "home: ready", homes);
  } finally {
    await guest.stop();
  }
});

test("shell Tab completes command names and paths", { timeout: 90_000 }, async () => {
  const missing = missingTools(["qemu-system-x86_64"]);
  if (missing.length > 0) {
    throw hostToolError(missing);
  }
  const image = await buildImage();
  const guest = await startGuest({ isoPath: image.isoPath, display: "none" });
  try {
    await guest.waitForSerial("genesis:/home> ", DEFAULT_TIMEOUT_MS);
    for (const k of ["e", "c", "h"]) {
      await guest.sendKey(k);
      await delay(40);
    }
    await key(guest, "tab");
    await typeLine(guest, "ok");
    await guest.waitForSerial("\nok\n", DEFAULT_TIMEOUT_MS);
    for (const k of ["c", "a", "t", "spc", "slash", "d", "o"]) {
      await guest.sendKey(k);
      await delay(40);
    }
    await key(guest, "tab");
    for (const k of ["n", "o", "t", "e", "dot", "t"]) {
      await guest.sendKey(k);
      await delay(40);
    }
    await key(guest, "tab");
    await key(guest, "ret");
    await guest.waitForSerial("hello from note.txt", DEFAULT_TIMEOUT_MS);
    assert.match(guest.serial, /cat \/docs\/note\.txt/);
  } finally {
    await guest.stop();
  }
});

test("keyboard only (TV remote): arrows move focus, Enter opens, Esc closes", { timeout: 120_000 }, async () => {
  const guest = await desktop();
  try {
    // First arrow lands on the core, Up goes to the cell above it: Файлы.
    await key(guest, "right");
    await guest.waitForSerial("focus: Ядро", DEFAULT_TIMEOUT_MS);
    await key(guest, "up");
    await guest.waitForSerial("focus: Файлы", DEFAULT_TIMEOUT_MS);
    await key(guest, "ret");
    await guest.waitForSerial("ls: readme", DEFAULT_TIMEOUT_MS);
    // Panel items: Enter on the first row ("..") goes up to /.
    await key(guest, "ret");
    await guest.waitForSerial("dir: / ls: bin/ lib/ docs/ home/", DEFAULT_TIMEOUT_MS);
    let homes = countOf(guest.serial, "home: ready");
    await key(guest, "esc");
    await next(guest, "home: ready", homes);
    // Down from Файлы reaches the core again; Enter opens the task panel.
    const cores = countOf(guest.serial, "focus: Ядро");
    await key(guest, "down");
    await next(guest, "focus: Ядро", cores);
    await key(guest, "ret");
    await guest.waitForSerial("tasks: 0:shell", DEFAULT_TIMEOUT_MS);
    homes = countOf(guest.serial, "home: ready");
    await key(guest, "backspace");
    await next(guest, "home: ready", homes);
    // Focus stayed on the core: its inner hexagon is drawn highlighted.
    await delay(300);
    const img = parsePpm(await guest.screendump());
    const [r, g, b] = pixel(img, CORE.x, CORE.y + 80);
    assert.ok(b > 70 && r < 60, `нет подсветки фокуса на ядре: ${r},${g},${b}`);
  } finally {
    await guest.stop();
  }
});

test("portrait 480x800: core on top, honeycomb below, panel is a full sheet", { timeout: 90_000 }, async () => {
  const guest = await desktop("480x800");
  try {
    await guest.waitForSerial("cells: Файлы", DEFAULT_TIMEOUT_MS);
    const ppm = await guest.screendump();
    const img = parsePpm(ppm);
    assert.equal(img.width, 480);
    assert.equal(img.height, 800);
    const docs = path.join(process.cwd(), "docs");
    mkdirSync(docs, { recursive: true });
    writeFileSync(path.join(docs, "ui-core-portrait.png"), ppmToPng(ppm));
    const lit = (x: number, y: number): boolean => {
      const [r, g, b] = pixel(img, x, y);
      return r + g + b > 300;
    };
    // Core outline: top vertex below the header, centred.
    let core = false;
    for (let y = 60; y < 90 && !core; y++) {
      core = lit(240, y);
    }
    assert.ok(core, "ядра нет сверху по центру");
    // Four cells per honeycomb row, all below the core, every one >= 48 px wide.
    let litCells = 0;
    for (const x of [65, 165, 265, 365]) {
      let on = false;
      for (let y = 280; y < 300 && !on; y++) {
        on = lit(x, y);
      }
      if (on) {
        litCells++;
      }
    }
    assert.equal(litCells, 4, "в первом ряду сот не 4 соты");
    // Tapping Файлы opens a full-width sheet with «< Назад».
    await click(guest, 65, 340);
    await guest.waitForSerial("ls: readme", DEFAULT_TIMEOUT_MS);
    await delay(300);
    const sheet = parsePpm(await guest.screendump());
    const edgeL = pixel(sheet, 9, 400);
    const edgeR = pixel(sheet, 470, 400);
    assert.ok(edgeL[2] > 120 && edgeR[2] > 120, "лист не во всю ширину");
    const homes = countOf(guest.serial, "home: ready");
    await click(guest, 70, 80);
    await next(guest, "home: ready", homes);
  } finally {
    await guest.stop();
  }
});

test("arrows: Left/Right move focus with E0 and bare keypad scancodes; Up with history doesn't steal focus", { timeout: 120_000 }, async () => {
  const guest = await desktop();
  try {
    // Some history first: Up/Down on an empty bar must still move focus.
    await typeLine(guest, "echo a");
    await guest.waitForSerial("\na\n", DEFAULT_TIMEOUT_MS);
    // Real E0-prefixed arrows (QMP 'left'/'right').
    let cores = countOf(guest.serial, "focus: Ядро");
    await key(guest, "left");
    await next(guest, "focus: Ядро", cores);
    const before = countOf(guest.serial, "focus: ");
    await key(guest, "right");
    await next(guest, "focus: ", before);
    assert.ok(!guest.serial.trimEnd().endsWith("focus: Ядро"), "Right не сдвинул фокус с ядра");
    cores = countOf(guest.serial, "focus: Ядро");
    await key(guest, "left");
    await next(guest, "focus: Ядро", cores);
    await key(guest, "up");
    await guest.waitForSerial("focus: Файлы", DEFAULT_TIMEOUT_MS);
    // Bare keypad scancodes (no E0), as QEMU's GTK UI on Windows may send.
    cores = countOf(guest.serial, "focus: Ядро");
    await key(guest, "kp_2");
    await next(guest, "focus: Ядро", cores);
    const n = countOf(guest.serial, "focus: ");
    await key(guest, "kp_6");
    await next(guest, "focus: ", n);
    const m = countOf(guest.serial, "focus: ");
    await key(guest, "kp_4");
    await next(guest, "focus: ", m);
  } finally {
    await guest.stop();
  }
});

test("panel opened by mouse: arrows work at once inside it", { timeout: 120_000 }, async () => {
  const guest = await desktop();
  try {
    await click(guest, CELL.files.x, CELL.files.y);
    await guest.waitForSerial("ls: readme", DEFAULT_TIMEOUT_MS);
    await key(guest, "down");
    await guest.waitForSerial("focus: item 2", DEFAULT_TIMEOUT_MS);
    await key(guest, "up");
    await guest.waitForSerial("focus: item 1", DEFAULT_TIMEOUT_MS);
    await key(guest, "ret");
    await guest.waitForSerial("dir: / ls: bin/ lib/ docs/ home/", DEFAULT_TIMEOUT_MS);
    const homes = countOf(guest.serial, "home: ready");
    await key(guest, "esc");
    await next(guest, "home: ready", homes);
  } finally {
    await guest.stop();
  }
});
