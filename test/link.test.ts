import assert from "node:assert/strict";
import { test } from "node:test";
import { startGuest } from "../src/guest.js";
import {
  DEFAULT_TIMEOUT_MS,
  buildImage,
  hostToolError,
  missingTools,
} from "../src/pipeline.js";
import { typeLine } from "./type-line.js";

test(
  "say hello-link arrives on the other Genesis Link guest",
  { timeout: 90_000 },
  async () => {
    const missing = missingTools(["qemu-system-x86_64"]);
    if (missing.length > 0) {
      throw hostToolError(missing);
    }

    const port = 25000 + (process.pid % 15000);
    const image = await buildImage();
    const left = await startGuest({
      isoPath: image.isoPath,
      display: "none",
      net: { mode: "listen", port, mac: "52:54:00:12:34:10" },
    });
    let right: Awaited<ReturnType<typeof startGuest>> | undefined;
    try {
      await left.waitForSerial("link: up", DEFAULT_TIMEOUT_MS);
      right = await startGuest({
        isoPath: image.isoPath,
        display: "none",
        net: { mode: "connect", port, mac: "52:54:00:12:34:11" },
      });
      await right.waitForSerial("link: up", DEFAULT_TIMEOUT_MS);
      await typeLine(right, "say hello-link");
      await right.waitForSerial("say: sent", DEFAULT_TIMEOUT_MS);
      await left.waitForSerial("link: hello-link", DEFAULT_TIMEOUT_MS);
      assert.match(left.serial, /link: hello-link/);

      await typeLine(left, "ping");
      await left.waitForSerial("pong", DEFAULT_TIMEOUT_MS);
    } finally {
      await left.stop();
      if (right) {
        await right.stop();
      }
    }
  },
);
