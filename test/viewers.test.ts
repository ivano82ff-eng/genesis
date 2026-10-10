import assert from "node:assert/strict";
import { test } from "node:test";
import { startGuest, type Guest } from "../src/guest.js";
import {
  DEFAULT_TIMEOUT_MS,
  buildImage,
  hostToolError,
  missingTools,
} from "../src/pipeline.js";
import { typeLine } from "./type-line.js";

function delay(ms: number): Promise<void> {
  return new Promise((resolve) => {
    setTimeout(resolve, ms);
  });
}

async function click(guest: Guest, x: number, y: number): Promise<void> {
  let left = x;
  let top = y;
  while (left > 0 || top > 0) {
    const stepX = Math.min(40, left);
    const stepY = Math.min(40, top);
    await guest.monitor(`mouse_move ${stepX} ${stepY}`);
    left -= stepX;
    top -= stepY;
    await delay(150);
  }
  try {
    await guest.monitor("mouse_button 1");
    await delay(80);
    await guest.monitor("mouse_button 0");
  } catch {
    // viewer may keep the guest busy
  }
}

async function openRow(guest: Guest, rowY: number, expect: string): Promise<void> {
  await click(guest, 200, 420);
  await guest.waitForSerial("open: ", DEFAULT_TIMEOUT_MS);
  await delay(100);
  await click(guest, 200, rowY);
  await guest.waitForSerial(expect, DEFAULT_TIMEOUT_MS);
}

test(
  "Открыть runs txt, docx, image, pdf, and mp4 viewers",
  { timeout: 120_000 },
  async () => {
    const missing = missingTools(["qemu-system-x86_64"]);
    if (missing.length > 0) {
      throw hostToolError(missing);
    }
    const image = await buildImage();

    const txt = await startGuest({ isoPath: image.isoPath, display: "none" });
    try {
      await txt.waitForSerial("genesis> ", DEFAULT_TIMEOUT_MS);
      await typeLine(txt, "desktop");
      await txt.waitForSerial("home: ready", DEFAULT_TIMEOUT_MS);
      assert.match(txt.serial, /Открыть/);
      await openRow(txt, 208, "txt: hello from note.txt");
    } finally {
      await txt.stop();
    }

    const docx = await startGuest({ isoPath: image.isoPath, display: "none" });
    try {
      await docx.waitForSerial("genesis> ", DEFAULT_TIMEOUT_MS);
      await typeLine(docx, "desktop");
      await docx.waitForSerial("home: ready", DEFAULT_TIMEOUT_MS);
      await openRow(docx, 268, "docx: genesis docx note");
    } finally {
      await docx.stop();
    }

    const img = await startGuest({ isoPath: image.isoPath, display: "none" });
    try {
      await img.waitForSerial("genesis> ", DEFAULT_TIMEOUT_MS);
      await typeLine(img, "desktop");
      await img.waitForSerial("home: ready", DEFAULT_TIMEOUT_MS);
      await openRow(img, 328, "image: ok");
    } finally {
      await img.stop();
    }

    const pdf = await startGuest({ isoPath: image.isoPath, display: "none" });
    try {
      await pdf.waitForSerial("genesis> ", DEFAULT_TIMEOUT_MS);
      await typeLine(pdf, "desktop");
      await pdf.waitForSerial("home: ready", DEFAULT_TIMEOUT_MS);
      await openRow(pdf, 448, "pdf: genesis pdf");
    } finally {
      await pdf.stop();
    }

    const mp4 = await startGuest({ isoPath: image.isoPath, display: "none" });
    try {
      await mp4.waitForSerial("genesis> ", DEFAULT_TIMEOUT_MS);
      await typeLine(mp4, "desktop");
      await mp4.waitForSerial("home: ready", DEFAULT_TIMEOUT_MS);
      await openRow(mp4, 508, "mp4: play");
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
