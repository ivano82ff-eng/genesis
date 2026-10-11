import assert from "node:assert/strict";
import { rm, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { test } from "node:test";
import { startGuest, type Guest } from "../src/guest.js";
import { DEFAULT_TIMEOUT_MS, buildImage, hostToolError, missingTools } from "../src/pipeline.js";
import { ruKeysFor, toggleLayout } from "../src/typing.js";
import { screenShowsChar } from "../src/vga-text.js";
import { delay, typeLine } from "./helpers.js";

/** Press keys one by one; Cyrillic letters assume the Русская layout is on. */
async function press(guest: Guest, items: string[]): Promise<void> {
  for (const item of items) {
    const keys = ruKeysFor(item) ?? [item];
    await guest.qmp("send-key", { keys: keys.map((data) => ({ type: "qcode", data })) });
    await delay(40);
  }
}

test("Russian: layout switch, shell echo, screen glyphs, files, editor", { timeout: 120_000 }, async () => {
  const missing = missingTools(["qemu-system-x86_64"]);
  if (missing.length > 0) {
    throw hostToolError(missing);
  }
  const disk = path.join(os.tmpdir(), `genesis-cyr-${process.pid}-${Date.now()}.img`);
  await writeFile(disk, Buffer.alloc(8 * 1024 * 1024));
  const image = await buildImage();
  const guest = await startGuest({ isoPath: image.isoPath, display: "none", diskPath: disk });
  try {
    await guest.waitForSerial("genesis:/home> ", DEFAULT_TIMEOUT_MS);

    // Ctrl+Space switches to ЙЦУКЕН; echo comes back as UTF-8 on COM1.
    await typeLine(guest, "echo Привет мир");
    await guest.waitForSerial("Привет мир\nПривет мир\n", DEFAULT_TIMEOUT_MS);
    const ppm = await guest.screendump();
    for (const ch of ["П", "и", "в", "т", "м"]) {
      assert.ok(screenShowsChar(ppm, ch), `на экране нет «${ch}»`);
    }

    // Backspace removes a whole Russian letter, not one byte of it.
    await press(guest, ["e", "c", "h", "o", "spc"]);
    await toggleLayout(guest);
    await press(guest, ["ж", "у", "к", "backspace", "ё"]);
    await toggleLayout(guest);
    await guest.sendKey("ret");
    await guest.waitForSerial("\nжуё\n", DEFAULT_TIMEOUT_MS);

    await typeLine(guest, "write ru.txt Ёлка, ёж №5");
    await guest.waitForSerial("write: ru.txt", DEFAULT_TIMEOUT_MS);
    await typeLine(guest, "cat ru.txt");
    await guest.waitForSerial("cat: Ёлка, ёж №5", DEFAULT_TIMEOUT_MS);

    // Full-screen editor: cursor and Backspace move by character.
    await typeLine(guest, "edit r2.txt");
    await guest.waitForSerial("edit: new r2.txt", DEFAULT_TIMEOUT_MS);
    await toggleLayout(guest);
    await press(guest, ["ж", "у", "к", "backspace", "left", "ё", "right", "ш"]);
    await delay(200);
    const shot = await guest.screendump();
    assert.ok(screenShowsChar(shot, "ё"), "в редакторе нет «ё»");
    assert.ok(screenShowsChar(shot, "ш"), "в редакторе нет «ш»");
    await guest.qmp("send-key", { keys: [{ type: "qcode", data: "ctrl" }, { type: "qcode", data: "s" }] });
    await guest.waitForSerial("edit: saved r2.txt (8 bytes)", DEFAULT_TIMEOUT_MS);
    await guest.sendKey("esc");
    await guest.waitForSerial("edit: closed", DEFAULT_TIMEOUT_MS);
    await typeLine(guest, "cat r2.txt");
    await guest.waitForSerial("cat: жёуш", DEFAULT_TIMEOUT_MS);
  } finally {
    await guest.stop();
    await rm(disk, { force: true });
  }
});
