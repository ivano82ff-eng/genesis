import assert from "node:assert/strict";
import { test } from "node:test";
import { BOOT_MARKER } from "../src/pipeline.js";
import { paintTextPpm, screenShowsChar, screenTextError } from "../src/vga-text.js";

test("decoder accepts the painted boot line and rejects a blank screen", () => {
  assert.equal(screenTextError(paintTextPpm(BOOT_MARKER), BOOT_MARKER), null);

  const blank = paintTextPpm("");
  const missing = screenTextError(blank, BOOT_MARKER);
  assert.ok(missing);
  assert.match(missing, /столбец 0/);
  assert.equal(screenShowsChar(blank, "a"), false);
  assert.equal(screenShowsChar(paintTextPpm("a"), "a"), true);
  assert.equal(screenShowsChar(paintTextPpm("Жж"), "ж"), true);
  assert.equal(screenShowsChar(paintTextPpm("Жж"), "Я"), false);
  assert.equal(screenTextError(paintTextPpm("мир"), "мир"), null);
});
