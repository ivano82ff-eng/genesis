import assert from "node:assert/strict";
import { test } from "node:test";
import { startGuest } from "../src/guest.js";
import {
  DEFAULT_TIMEOUT_MS,
  buildImage,
  hostToolError,
  missingTools,
} from "../src/pipeline.js";
import { typeLine } from "./helpers.js";

test(
  "the editor copies, cuts and pastes with Ctrl+C/X/V",
  { timeout: 110_000 },
  async () => {
    const missing = missingTools(["qemu-system-x86_64"]);
    if (missing.length > 0) {
      throw hostToolError(missing);
    }
    const image = await buildImage();
    const guest = await startGuest({ isoPath: image.isoPath, display: "none" });
    const ctrl = (k: string) =>
      guest.qmp("send-key", { keys: [{ type: "qcode", data: "ctrl" }, { type: "qcode", data: k }] });
    try {
      await guest.waitForSerial("genesis:/home> ", DEFAULT_TIMEOUT_MS);
      await typeLine(guest, "edit readme");
      await guest.waitForSerial("edit: welcome to genesis", DEFAULT_TIMEOUT_MS);
      await typeLine(guest, "ab"); // "ab\n" + 18 bytes
      await guest.sendKey("up");
      await ctrl("c"); // no selection: the whole line "ab\n"
      await guest.waitForSerial("edit: copied", DEFAULT_TIMEOUT_MS);
      await ctrl("v");
      await guest.waitForSerial("edit: pasted", DEFAULT_TIMEOUT_MS);
      await ctrl("s");
      await guest.waitForSerial("edit: saved readme (24 bytes)", DEFAULT_TIMEOUT_MS);
      // Shift+Right twice selects "ab"; Ctrl+X removes it.
      for (let i = 0; i < 2; i++) {
        await guest.qmp("send-key", { keys: [{ type: "qcode", data: "shift" }, { type: "qcode", data: "right" }] });
      }
      await ctrl("x");
      await guest.waitForSerial("edit: cut", DEFAULT_TIMEOUT_MS);
      await ctrl("s");
      await guest.waitForSerial("edit: saved readme (22 bytes)", DEFAULT_TIMEOUT_MS);
      await guest.sendKey("esc");
      await guest.waitForSerial("edit: closed", DEFAULT_TIMEOUT_MS);
    } finally {
      await guest.stop();
    }
  },
);
