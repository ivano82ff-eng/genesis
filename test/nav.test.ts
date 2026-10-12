import assert from "node:assert/strict";
import { test } from "node:test";
import { startGuest, type Guest } from "../src/guest.js";
import { DEFAULT_TIMEOUT_MS, buildImage, hostToolError, missingTools } from "../src/pipeline.js";
import { click, delay, typeLine } from "./helpers.js";
import { CELL, fileRow } from "./ui.js";

async function openRow(guest: Guest, row: number, expect: string): Promise<void> {
  await click(guest, CELL.files.x, CELL.files.y);
  await guest.waitForSerial("ls: ", DEFAULT_TIMEOUT_MS);
  await delay(100);
  await click(guest, fileRow(0).x, fileRow(0).y);
  await guest.waitForSerial("dir: / ls: bin/ lib/ docs/ home/", DEFAULT_TIMEOUT_MS);
  await delay(100);
  await click(guest, fileRow(2).x, fileRow(2).y);
  await guest.waitForSerial("dir: /docs ls: note.txt", DEFAULT_TIMEOUT_MS);
  await delay(100);
  await click(guest, fileRow(row).x, fileRow(row).y);
  await guest.waitForSerial(expect, DEFAULT_TIMEOUT_MS);
}

test("Back is one step (viewer -> folder -> parent -> window root -> home); Esc closes", { timeout: 120_000 }, async () => {
  const missing = missingTools(["qemu-system-x86_64"]);
  if (missing.length > 0) {
    throw hostToolError(missing);
  }
  const image = await buildImage();
  const txt = await startGuest({ isoPath: image.isoPath, display: "none" });
  try {
    await txt.waitForSerial("genesis:/home> ", DEFAULT_TIMEOUT_MS);
    await typeLine(txt, "desktop");
    await txt.waitForSerial("home: ready", DEFAULT_TIMEOUT_MS);
    await openRow(txt, 1, "txt: hello from note.txt");
      // Back walks the path one step at a time: viewer -> /docs -> / -> /home -> home.
      const step = async (want: string): Promise<void> => {
        const before = txt.serial.split(want).length;
        await txt.sendKey("backspace");
        const until = Date.now() + DEFAULT_TIMEOUT_MS;
        while (txt.serial.split(want).length <= before) {
          if (Date.now() > until) throw new Error(`no «${want}»: ` + txt.serial.slice(-800));
          await delay(100);
        }
        await delay(300);
      };
      await step("back: /docs ls: note.txt");
      // Still in /docs: the same row opens the same file again.
      const opened = txt.serial.split("txt: hello from note.txt").length;
      await click(txt, fileRow(1).x, fileRow(1).y);
      const until = Date.now() + DEFAULT_TIMEOUT_MS;
      while (txt.serial.split("txt: hello from note.txt").length <= opened) {
        if (Date.now() > until) throw new Error(txt.serial.slice(-800));
        await delay(100);
      }
      await step("back: /docs ls: note.txt");
      await step("back: / ls: bin/");
      await step("back: /home ls:");
      await step("home: ready");
      // Esc closes the whole window, even from deep inside a viewer.
      await openRow(txt, 1, "txt: hello from note.txt");
      const homes = txt.serial.split("home: ready").length;
      await txt.sendKey("esc");
      const until2 = Date.now() + DEFAULT_TIMEOUT_MS;
      while (txt.serial.split("home: ready").length <= homes) {
        if (Date.now() > until2) throw new Error(txt.serial.slice(-800));
        await delay(100);
      }
      assert.ok(!txt.serial.slice(txt.serial.lastIndexOf("txt: hello")).includes("back: "), "Esc stepped back instead of closing");
  } finally {
    await txt.stop();
  }
});
