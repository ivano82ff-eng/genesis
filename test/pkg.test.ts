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
  "pkg lists, installs with deps, links at run time and removes",
  { timeout: 180_000 },
  async () => {
    const missing = missingTools(["qemu-system-x86_64"]);
    if (missing.length > 0) {
      throw hostToolError(missing);
    }

    const image = await buildImage();
    const guest = await startGuest({ isoPath: image.isoPath, display: "none" });
    const after = async (command: string, expect: string): Promise<string> => {
      const from = guest.serial.length;
      await typeLine(guest, command);
      await guest.waitForSerial(expect, DEFAULT_TIMEOUT_MS);
      await guest.waitForSerial("genesis> ", DEFAULT_TIMEOUT_MS);
      return guest.serial.slice(from);
    };
    try {
      await guest.waitForSerial("genesis> ", DEFAULT_TIMEOUT_MS);
      const list = await after("pkg list", "pkg: strlib 1.0 deps mathlib");
      assert.match(list, /pkg: mathlib 1\.0 - integer math/);
      assert.match(list, /pkg: strdemo 1\.0 deps strlib/);
      assert.ok(!list.includes("[installed]"));

      await after("run strdemo", "run: no such file");
      await after("pkg install nosuch", "pkg: no package nosuch");

      const install = await after("pkg install strdemo", "pkg: installed strdemo 1.0");
      const order = ["installed mathlib", "installed strlib", "installed strdemo"].map((s) => install.indexOf(s));
      assert.ok(order[0]! >= 0 && order[0]! < order[1]! && order[1]! < order[2]!, install);
      await after("pkg installed", "pkg: installed: mathlib strdemo strlib");
      await after("pkg install strdemo", "pkg: strdemo already installed");

      const run = await after("run strdemo", "strdemo: 2^10=1024");
      assert.match(run, /strdemo: HELLO GENESIS/);
      assert.match(run, /strdemo: SISENEG OLLEH/);

      await after("pkg remove mathlib", "pkg: mathlib needed by strlib");
      await after("pkg remove strlib", "pkg: strlib needed by strdemo");

      // A library vanishing under a program fails cleanly instead of crashing.
      await after("rm strlib", "rm: strlib");
      await after("run strdemo", "strdemo: missing library strlib");
      await after("pkg install strlib", "pkg: installed strlib 1.0");
      await after("run strdemo", "strdemo: 2^10=1024");

      await after("pkg remove strdemo", "pkg: removed strdemo");
      await after("pkg remove strlib", "pkg: removed strlib");
      await after("pkg remove mathlib", "pkg: removed mathlib");
      await after("pkg installed", "pkg: installed: none");
      await after("pkg remove mathlib", "pkg: mathlib not installed");
      const ls = await after("ls", "ls:");
      assert.ok(!/mathlib|strlib|strdemo/.test(ls), ls);
    } finally {
      await guest.stop();
    }
  },
);

test(
  "pkg refuses to install when Grid Store has too few free slots",
  { timeout: 150_000 },
  async () => {
    const image = await buildImage();
    const guest = await startGuest({ isoPath: image.isoPath, display: "none" });
    try {
      await guest.waitForSerial("genesis> ", DEFAULT_TIMEOUT_MS);
      for (let i = 0; i < 4; i++) {
        await typeLine(guest, `write fill${i} x`);
        await guest.waitForSerial(`write: fill${i}`, DEFAULT_TIMEOUT_MS);
      }
      await typeLine(guest, "pkg install strdemo");
      await guest.waitForSerial("pkg: need 3 free file slots, have 1", DEFAULT_TIMEOUT_MS);
      await typeLine(guest, "pkg installed");
      await guest.waitForSerial("pkg: installed: none", DEFAULT_TIMEOUT_MS);
    } finally {
      await guest.stop();
    }
  },
);
