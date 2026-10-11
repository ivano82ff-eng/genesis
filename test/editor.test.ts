import assert from "node:assert/strict";
import { test } from "node:test";
import { startGuest, type Guest } from "../src/guest.js";
import {
  DEFAULT_TIMEOUT_MS,
  buildImage,
  hostToolError,
  missingTools,
} from "../src/pipeline.js";
import { parsePpm } from "../src/ppm.js";
import { delay, typeLine } from "./helpers.js";

async function combo(guest: Guest, ...keys: string[]): Promise<void> {
  await guest.qmp("send-key", { keys: keys.map((data) => ({ type: "qcode", data })) });
  await delay(60);
}

async function press(guest: Guest, key: string, times = 1): Promise<void> {
  for (let i = 0; i < times; i++) {
    await guest.sendKey(key);
    await delay(60);
  }
}

/** Count pixels close to one VGA text color. */
function countColor(ppm: Buffer, want: [number, number, number]): number {
  const image = parsePpm(ppm);
  let count = 0;
  for (let i = 0; i + 2 < image.pixels.length; i += 3) {
    const r = image.pixels[i]!;
    const g = image.pixels[i + 1]!;
    const b = image.pixels[i + 2]!;
    if (Math.abs(r - want[0]) < 40 && Math.abs(g - want[1]) < 40 && Math.abs(b - want[2]) < 40) {
      count++;
    }
  }
  return count;
}

test(
  "full-screen editor: new file, lines, cursor keys, highlight, save, cat",
  { timeout: 120_000 },
  async () => {
    const missing = missingTools(["qemu-system-x86_64"]);
    if (missing.length > 0) {
      throw hostToolError(missing);
    }
    const image = await buildImage();
    const guest = await startGuest({ isoPath: image.isoPath, display: "none" });
    try {
      await guest.waitForSerial("genesis:/home> ", DEFAULT_TIMEOUT_MS);
      await typeLine(guest, "edit hello.c");
      await guest.waitForSerial("edit: new hello.c", DEFAULT_TIMEOUT_MS);
      await typeLine(guest, "int main() {");
      await typeLine(guest, "    return 42; // done");
      await typeLine(guest, "}");
      // Back to the first line, start of line, insert a word.
      await press(guest, "up", 3);
      await press(guest, "end");
      await press(guest, "home");
      for (const key of ["s", "t", "a", "t", "i", "c", "spc"]) {
        await press(guest, key);
      }
      // Delete removes the stray character after the cursor: "xint" -> "int".
      await press(guest, "x");
      await press(guest, "left");
      await press(guest, "delete");
      await combo(guest, "ctrl", "s");
      await guest.waitForSerial("edit: saved hello.c (", DEFAULT_TIMEOUT_MS);

      const ppm = await guest.screendump();
      // Yellow keywords, green comment, cyan number, gray status bar.
      assert.ok(countColor(ppm, [255, 255, 85]) > 20, "keywords are yellow");
      assert.ok(countColor(ppm, [85, 255, 85]) > 20, "comments are green");
      assert.ok(countColor(ppm, [85, 255, 255]) > 5, "numbers are cyan");

      await press(guest, "esc");
      await guest.waitForSerial("edit: closed", DEFAULT_TIMEOUT_MS);
      await typeLine(guest, "cat hello.c");
      await guest.waitForSerial("cat: static int main() {", DEFAULT_TIMEOUT_MS);
      await guest.waitForSerial("    return 42; // done\n}", DEFAULT_TIMEOUT_MS);
    } finally {
      await guest.stop();
    }
  },
);
