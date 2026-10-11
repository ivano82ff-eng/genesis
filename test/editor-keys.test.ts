import assert from "node:assert/strict";
import { test } from "node:test";
import { startGuest } from "../src/guest.js";
import {
  DEFAULT_TIMEOUT_MS,
  buildImage,
  hostToolError,
  missingTools,
} from "../src/pipeline.js";
import { delay, typeLine } from "./helpers.js";

test(
  "window key events type into the editor",
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
      let top = 320;
      while (top > 0) {
        const step = Math.min(40, top);
        await guest.monitor(`mouse_move 0 ${step}`);
        top -= step;
        await delay(200);
      }
      await guest.monitor("mouse_move 200 0");
      await delay(200);
      await guest.monitor("mouse_button 1");
      await delay(80);
      await guest.monitor("mouse_button 0");
      await guest.waitForSerial("edit: welcome to genesis", DEFAULT_TIMEOUT_MS);
      await typeLine(guest, "hi");
      await guest.waitForSerial("edit: saved", DEFAULT_TIMEOUT_MS);
      assert.match(guest.serial, /hi[\s\S]*edit: saved/);
    } finally {
      await guest.stop();
    }
  },
);
