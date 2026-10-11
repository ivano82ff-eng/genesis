import assert from "node:assert/strict";
import { test } from "node:test";
import { startGuest } from "../src/guest.js";
import { DEFAULT_TIMEOUT_MS, buildImage, hostToolError, missingTools } from "../src/pipeline.js";
import { parsePpm } from "../src/ppm.js";
import { screenShowsChar } from "../src/vga-text.js";
import { delay, pixel, typeLine } from "./helpers.js";

test("blinking cursor after the prompt, boot hint, desktop command bar", { timeout: 120_000 }, async () => {
  const missing = missingTools(["qemu-system-x86_64"]);
  if (missing.length > 0) {
    throw hostToolError(missing);
  }
  const image = await buildImage();
  const guest = await startGuest({ isoPath: image.isoPath, display: "none" });
  try {
    await guest.waitForSerial("genesis:/home> ", DEFAULT_TIMEOUT_MS);
    assert.match(guest.serial, /Введите команду и нажмите Enter/);

    // Text mode: the cursor cell sits right after "genesis:/home> " (row 1, col 15).
    let cursorSeen = false;
    let hintSeen = false;
    for (let i = 0; i < 10 && !cursorSeen; i++) {
      const ppm = await guest.screendump();
      const img = parsePpm(ppm);
      hintSeen ||= screenShowsChar(ppm, "В") && screenShowsChar(ppm, "д");
      cursorSeen = pixel(img, 15 * 9 + 3, 16 + 14)[0] > 128;
      await delay(150);
    }
    assert.ok(hintSeen, "на экране нет подсказки");
    assert.ok(cursorSeen, "курсора после genesis:/home> не видно");

    // Desktop: the command bar runs a line and its cursor blinks.
    await typeLine(guest, "desktop");
    await guest.waitForSerial("home: ready", DEFAULT_TIMEOUT_MS);
    await typeLine(guest, "echo привет мир");
    await guest.waitForSerial("genesis:/home> echo привет мир\nпривет мир", DEFAULT_TIMEOUT_MS);
    const colors = new Set<string>();
    for (let i = 0; i < 12 && colors.size < 2; i++) {
      const img = parsePpm(await guest.screendump());
      colors.add(pixel(img, 308, 699).join(","));
      await delay(200);
    }
    assert.ok(colors.has("240,193,74"), `курсор строки команд не появился: ${[...colors].join(" ")}`);
    assert.equal(colors.size, 2, "курсор строки команд не мигает");
  } finally {
    await guest.stop();
  }
});
