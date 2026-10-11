import assert from "node:assert/strict";
import { test } from "node:test";
import { startGuest } from "../src/guest.js";
import {
  DEFAULT_TIMEOUT_MS,
  buildImage,
  hostToolError,
  missingTools,
} from "../src/pipeline.js";
import { click, typeLine } from "./helpers.js";
import { CELL } from "./ui.js";

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
      await guest.waitForSerial("genesis:/home> ", DEFAULT_TIMEOUT_MS);
      await typeLine(guest, "desktop");
      await guest.waitForSerial("home: ready", DEFAULT_TIMEOUT_MS);
      await click(guest, CELL.editor.x, CELL.editor.y);
      await guest.waitForSerial("edit: welcome to genesis", DEFAULT_TIMEOUT_MS);
      // The full-screen editor: type a line, Ctrl+S saves (18 + "hi\n").
      await typeLine(guest, "hi");
      await guest.qmp("send-key", { keys: [{ type: "qcode", data: "ctrl" }, { type: "qcode", data: "s" }] });
      await guest.waitForSerial("edit: saved /home/readme (21 bytes)", DEFAULT_TIMEOUT_MS);
      await guest.sendKey("esc");
      await guest.waitForSerial("edit: closed", DEFAULT_TIMEOUT_MS);
      assert.ok(guest.serial.indexOf("edit: closed") > guest.serial.indexOf("edit: saved"));
    } finally {
      await guest.stop();
    }
  },
);
