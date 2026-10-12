import assert from "node:assert/strict";
import { test } from "node:test";
import { startGuest, type Guest } from "../src/guest.js";
import { parsePpm } from "../src/png.js";
import { DEFAULT_TIMEOUT_MS, buildImage, hostToolError, missingTools } from "../src/pipeline.js";
import { click, countOf, delay, pixel, typeLine } from "./helpers.js";
import { CELL, CORE, cellAt } from "./ui.js";

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

async function next(guest: Guest, text: string, before = countOf(guest.serial, text)): Promise<void> {
  const deadline = Date.now() + DEFAULT_TIMEOUT_MS;
  while (countOf(guest.serial, text) <= before) {
    if (Date.now() > deadline) {
      throw new Error(`нет «${text}»\n${guest.serial.slice(-1500)}`);
    }
    await delay(50);
  }
}

test("layout: the core is centred above the bar and the cells sit symmetric on a ring", () => {
  // 1024x768: bar top at 684, so the area above it is centred at x 512, y ~370.
  assert.equal(CORE.x, 512);
  assert.ok(Math.abs(CORE.y - (56 + 684) / 2) <= 2);
  for (const n of [7, 8, 9, 12]) {
    const cells = Array.from({ length: n }, (_, i) => cellAt(i, n));
    const r = cells.map((c) => Math.hypot(c.x - CORE.x, c.y - CORE.y));
    for (const d of r) {
      assert.ok(Math.abs(d - 215) <= 2, `ячейка не на кольце: ${d}`);
    }
    for (let i = 1; i < n; i++) {
      const a = cells[i]!;
      const b = cells[n - i]!;
      assert.equal(a.x - CORE.x, CORE.x - b.x, `несимметрично ${n}: ${i}`);
      assert.equal(a.y, b.y);
    }
    const cx = cells.reduce((s, c) => s + c.x, 0) / n;
    const cy = cells.reduce((s, c) => s + c.y, 0) / n;
    assert.ok(Math.abs(cx - CORE.x) <= 2 && Math.abs(cy - CORE.y) <= 3, `центр кольца ${cx},${cy}`);
  }
});

test("screen: the core is drawn in the middle and the ring is symmetric", { timeout: 120_000 }, async () => {
  const guest = await desktop();
  try {
    await delay(300);
    const img = parsePpm(await guest.screendump());
    // The core outline glows cyan on both sides at the same distance.
    const lit = (x: number, y: number) => {
      const [r, g, b] = pixel(img, x, y);
      return g > 120 && b > 150 && r < 160;
    };
    let left = 0;
    let right = 0;
    for (let d = 60; d < 130; d++) {
      if (lit(CORE.x - d, CORE.y)) left = d;
      if (lit(CORE.x + d, CORE.y)) right = d;
    }
    assert.ok(left > 0 && Math.abs(left - right) <= 3, `ядро не по центру: ${left} vs ${right}`);
    // Mirror cells (Редактор / Настройки) are both drawn.
    for (const c of [CELL.editor, CELL.settings, CELL.terminal, CELL.feed]) {
      const [r, g, b] = pixel(img, c.x, c.y);
      assert.ok(r + g + b > 20, `пустая ячейка в ${c.x},${c.y}`);
    }
  } finally {
    await guest.stop();
  }
});

test("clock reads the CMOS RTC (local UTC+3) and ticks; tz changes it", { timeout: 120_000 }, async () => {
  const guest = await desktop();
  try {
    await guest.waitForSerial("up 00:00:02", DEFAULT_TIMEOUT_MS);
    const times = [...guest.serial.matchAll(/time (\d\d):(\d\d):(\d\d) up/g)].map(
      (m) => +m[1]! * 3600 + +m[2]! * 60 + +m[3]!,
    );
    assert.ok(times.length >= 2);
    assert.ok(times.some((t) => t !== 0), "часы стоят на 00:00:00");
    const now = new Date();
    const expect = ((now.getUTCHours() + 3) % 24) * 3600 + now.getUTCMinutes() * 60 + now.getUTCSeconds();
    const last = times[times.length - 1]!;
    const diff = Math.min(Math.abs(last - expect), 86400 - Math.abs(last - expect));
    assert.ok(diff < 120, `время ${last} далеко от ${expect}`);
    assert.ok(times[times.length - 1]! !== times[times.length - 2]!, "часы не идут");
    await typeLine(guest, "tz +0");
    await guest.waitForSerial("tz: UTC+0", DEFAULT_TIMEOUT_MS);
    await typeLine(guest, "tz +3");
    await guest.waitForSerial("tz: UTC+3", DEFAULT_TIMEOUT_MS);
  } finally {
    await guest.stop();
  }
});

test("journal records actions; Лента re-runs a command, confirms rm, filters", { timeout: 150_000 }, async () => {
  const guest = await desktop();
  try {
    await typeLine(guest, "echo first");
    await guest.waitForSerial("journal: c echo first", DEFAULT_TIMEOUT_MS);
    await typeLine(guest, "write /home/t.txt hello");
    await typeLine(guest, "pkg install strdemo");
    await guest.waitForSerial("journal: i pkg install strdemo", DEFAULT_TIMEOUT_MS);
    await typeLine(guest, "rm /home/t.txt");
    await guest.waitForSerial("journal: c rm /home/t.txt", DEFAULT_TIMEOUT_MS);
    // Back home (the terminal panel docked), then tap the Лента cell.
    await key(guest, "esc");
    await guest.waitForSerial("home: ready", DEFAULT_TIMEOUT_MS);
    await delay(300);
    const feedCell = cellAt(5, 8); // strdemo added an eighth cell
    await click(guest, feedCell.x, feedCell.y);
    await guest.waitForSerial("feed: Все ", DEFAULT_TIMEOUT_MS);
    // Newest card is focused: rm. Enter asks first; Esc cancels.
    await key(guest, "ret");
    await guest.waitForSerial("feed: confirm rm /home/t.txt", DEFAULT_TIMEOUT_MS);
    await key(guest, "esc");
    await guest.waitForSerial("feed: cancel", DEFAULT_TIMEOUT_MS);
    // Walk left to «echo first» and run it again.
    const deadline = Date.now() + 30_000;
    while (!guest.serial.trimEnd().endsWith("feed: at echo first") && !/feed: at echo first\n[^\n]*$/.test(guest.serial)) {
      if (Date.now() > deadline) throw new Error(guest.serial.slice(-800));
      await key(guest, "left");
    }
    const echoes = countOf(guest.serial, "\nfirst\n");
    await key(guest, "ret");
    await guest.waitForSerial("feed: replay echo first", DEFAULT_TIMEOUT_MS);
    await next(guest, "\nfirst\n", echoes);
    // Filter: `feed pkg` shows only the install.
    await typeLine(guest, "feed pkg");
    await guest.waitForSerial("feed: Пакеты 1", DEFAULT_TIMEOUT_MS);
    // «В ленту» from the Пакеты panel opens Лента filtered to it.
    await key(guest, "esc");
    const pkgCell = cellAt(3, 8);
    await click(guest, pkgCell.x, pkgCell.y);
    await guest.waitForSerial("Назад", DEFAULT_TIMEOUT_MS);
    const pk = countOf(guest.serial, "feed: Пакеты");
    await key(guest, "up"); // the title row: «В ленту» / X
    await key(guest, "ret");
    await next(guest, "feed: Пакеты", pk);
    // The journal survives: it is a file in /home.
    await key(guest, "esc");
    await typeLine(guest, "cat /home/.journal");
    await guest.waitForSerial("pkg install strdemo", DEFAULT_TIMEOUT_MS);
  } finally {
    await guest.stop();
  }
});

test("portrait Лента: a vertical timeline, Up/Down walk it", { timeout: 120_000 }, async () => {
  const guest = await desktop("480x800");
  try {
    await typeLine(guest, "echo one");
    await typeLine(guest, "echo two");
    await guest.waitForSerial("journal: c echo two", DEFAULT_TIMEOUT_MS);
    await key(guest, "esc");
    await typeLine(guest, "feed");
    await guest.waitForSerial("feed: Все ", DEFAULT_TIMEOUT_MS);
    await key(guest, "up");
    await guest.waitForSerial("feed: at echo one", DEFAULT_TIMEOUT_MS);
    await key(guest, "down");
    await guest.waitForSerial("feed: at echo two", DEFAULT_TIMEOUT_MS);
    await delay(300);
    // The «Сейчас» marker glows yellow across the sheet below the cards.
    const img = parsePpm(await guest.screendump());
    let yellow = 0;
    for (let y = 150; y < 700; y++) {
      const [r, g, b] = pixel(img, 240, y);
      if (r > 200 && g > 160 && b < 120) yellow++;
    }
    assert.ok(yellow > 0, "нет отметки «Сейчас»");
  } finally {
    await guest.stop();
  }
});
