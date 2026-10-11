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
  "a background task ticks while the shell answers ps and help",
  { timeout: 150_000 },
  async () => {
    const missing = missingTools(["qemu-system-x86_64"]);
    if (missing.length > 0) {
      throw hostToolError(missing);
    }

    const image = await buildImage();
    const guest = await startGuest({ isoPath: image.isoPath, display: "none" });
    try {
      await guest.waitForSerial("genesis:/home> ", DEFAULT_TIMEOUT_MS);
      await typeLine(guest, "spawn ticker");
      await guest.waitForSerial("spawn: task 1", DEFAULT_TIMEOUT_MS);
      await guest.waitForSerial("tick", DEFAULT_TIMEOUT_MS);
      await typeLine(guest, "ps");
      await guest.waitForSerial("ps: 1 ticker", DEFAULT_TIMEOUT_MS);
      assert.match(guest.serial, /ps: 0 shell/);
      await typeLine(guest, "help");
      await guest.waitForSerial("commands:", DEFAULT_TIMEOUT_MS);
      const helpAt = guest.serial.lastIndexOf("commands:");
      await guest.waitForSerial("task 1 done", 100_000);
      const doneAt = guest.serial.indexOf("task 1 done");
      assert.equal((guest.serial.match(/tick\r?\n/g) ?? []).length, 5);
      assert.ok(guest.serial.slice(helpAt, doneAt).includes("tick\n"), "ticks after help");
      const before = guest.serial.length;
      await typeLine(guest, "ps");
      await guest.waitForSerial("ps: 0 shell", DEFAULT_TIMEOUT_MS);
      await guest.waitForSerial("genesis:/home> ", DEFAULT_TIMEOUT_MS);
      assert.ok(!guest.serial.slice(before).includes("ticker"));
    } finally {
      await guest.stop();
    }
  },
);