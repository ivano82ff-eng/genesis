import assert from "node:assert/strict";
import { test } from "node:test";
import { startGuest, type Guest } from "../src/guest.js";
import {
  DEFAULT_TIMEOUT_MS,
  buildImage,
  hostToolError,
  missingTools,
} from "../src/pipeline.js";
import { click, delay, typeLine } from "./helpers.js";
import { CELL, fileRow } from "./ui.js";

async function openRow(guest: Guest, row: number, expect: string): Promise<void> {
  await click(guest, CELL.files.x, CELL.files.y);
  await guest.waitForSerial("ls: ", DEFAULT_TIMEOUT_MS);
  // Samples live in /docs: «..» (row 0) to /, then docs (bin, lib, docs, home: row 2).
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

test(
  "Файлы runs txt, docx, image, pdf, and mp4 viewers",
  { timeout: 120_000 },
  async () => {
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
      assert.match(txt.serial, /Файлы/);
      await openRow(txt, 1, "txt: hello from note.txt");
      await txt.waitForSerial("Привет из genesis — «ёж» №1", DEFAULT_TIMEOUT_MS);
    } finally {
      await txt.stop();
    }

    const docx = await startGuest({ isoPath: image.isoPath, display: "none" });
    try {
      await docx.waitForSerial("genesis:/home> ", DEFAULT_TIMEOUT_MS);
      await typeLine(docx, "desktop");
      await docx.waitForSerial("home: ready", DEFAULT_TIMEOUT_MS);
      await openRow(docx, 2, "docx: genesis docx note");
    } finally {
      await docx.stop();
    }

    const img = await startGuest({ isoPath: image.isoPath, display: "none" });
    try {
      await img.waitForSerial("genesis:/home> ", DEFAULT_TIMEOUT_MS);
      await typeLine(img, "desktop");
      await img.waitForSerial("home: ready", DEFAULT_TIMEOUT_MS);
      await openRow(img, 3, "image: ok");
    } finally {
      await img.stop();
    }

    const pdf = await startGuest({ isoPath: image.isoPath, display: "none" });
    try {
      await pdf.waitForSerial("genesis:/home> ", DEFAULT_TIMEOUT_MS);
      await typeLine(pdf, "desktop");
      await pdf.waitForSerial("home: ready", DEFAULT_TIMEOUT_MS);
      await openRow(pdf, 5, "pdf: genesis pdf");
    } finally {
      await pdf.stop();
    }

    const mp4 = await startGuest({ isoPath: image.isoPath, display: "none" });
    try {
      await mp4.waitForSerial("genesis:/home> ", DEFAULT_TIMEOUT_MS);
      await typeLine(mp4, "desktop");
      await mp4.waitForSerial("home: ready", DEFAULT_TIMEOUT_MS);
      await openRow(mp4, 6, "mp4: play");
      await mp4.waitForSerial("mp4: frame", DEFAULT_TIMEOUT_MS);
      const first = await mp4.screendump();
      await delay(400);
      const second = await mp4.screendump();
      assert.equal(first.equals(second), false);
    } finally {
      await mp4.stop();
    }
  },
);
