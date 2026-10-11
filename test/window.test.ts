import assert from "node:assert/strict";
import { test } from "node:test";
import { startGuest } from "../src/guest.js";
import {
  DEFAULT_TIMEOUT_MS,
  buildImage,
  hostToolError,
  missingTools,
} from "../src/pipeline.js";
import { click, countOf, delay, typeLine } from "./helpers.js";

test(
  "a file name opens its text and Назад returns home",
  { timeout: 90_000 },
  async () => {
    const missing = missingTools(["qemu-system-x86_64"]);
    if (missing.length > 0) {
      throw hostToolError(missing);
    }

    const image = await buildImage();
    const guest = await startGuest({ isoPath: image.isoPath, display: "none" });
    try {
      await guest.waitForSerial("genesis> ", DEFAULT_TIMEOUT_MS);
      await typeLine(guest, "desktop");
      await guest.waitForSerial("home: ready", DEFAULT_TIMEOUT_MS);
      await click(guest, 200, 220);
      await guest.waitForSerial("ls: ", DEFAULT_TIMEOUT_MS);
      await click(guest, 200, 148);
      await guest.waitForSerial("txt: welcome to genesis", DEFAULT_TIMEOUT_MS);
      const homes = countOf(guest.serial, "home: ready");
      await click(guest, 80, 40);
      const deadline = Date.now() + DEFAULT_TIMEOUT_MS;
      while (countOf(guest.serial, "home: ready") <= homes) {
        if (Date.now() > deadline) {
          throw new Error(`«Назад» не вернул домой\n${guest.serial}`);
        }
        await delay(50);
      }
    } finally {
      await guest.stop();
    }
  },
);
