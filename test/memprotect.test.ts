import assert from "node:assert/strict";
import { test } from "node:test";
import { startGuest } from "../src/guest.js";
import {
  DEFAULT_TIMEOUT_MS,
  buildImage,
  hostToolError,
  missingTools,
} from "../src/pipeline.js";
import { countOf, typeLine } from "./helpers.js";

const PROMPT = "genesis:/home> ";

/** Wait until TEXT shows up in the serial log after offset FROM. */
async function waitAfter(guest: { serial: string }, text: string, from: number, ms = DEFAULT_TIMEOUT_MS) {
  const end = Date.now() + ms;
  while (guest.serial.indexOf(text, from) < 0) {
    if (Date.now() > end) {
      throw new Error(`timed out waiting for ${JSON.stringify(text)}`);
    }
    await new Promise((r) => setTimeout(r, 100));
  }
}

function heapUsed(serial: string, from: number): number {
  const m = /heap: used (\d+)/.exec(serial.slice(from));
  assert.ok(m, "heap line");
  return Number(m[1]);
}

test(
  "a crashing program stops only its own task; the system keeps running",
  { timeout: 240_000 },
  async () => {
    const missing = missingTools(["qemu-system-x86_64"]);
    if (missing.length > 0) {
      throw hostToolError(missing);
    }
    const image = await buildImage();
    const guest = await startGuest({ isoPath: image.isoPath, display: "none" });
    try {
      await guest.waitForSerial(PROMPT, DEFAULT_TIMEOUT_MS);
      const crashes: [string, RegExp][] = [
        ["null", /task \d \(crashy\): page fault at 0x0 \(write, kernel memory\)/],
        ["kernel", /task \d \(crashy\): page fault at 0x100000 \(write, kernel memory\)/],
        ["div", /task \d \(crashy\): divide error at 0x8000/],
        ["op", /task \d \(crashy\): invalid opcode at 0x8000/],
        ["sys", /task \d \(crashy\): bad pointer 0x100000 in print/],
      ];
      for (const [mode, expected] of crashes) {
        const before = guest.serial.length;
        await typeLine(guest, `run crashy ${mode}`);
        await waitAfter(guest, "task stopped, system OK", before);
        await waitAfter(guest, PROMPT, before);
        assert.match(guest.serial.slice(before), expected);
        assert.ok(!guest.serial.includes("KERNEL PANIC"), "the kernel survives");
      }

      // A runaway loop next to a well-behaved task: kill stops it, ticker finishes.
      await typeLine(guest, "spawn crashy loop");
      await guest.waitForSerial("spinning forever", DEFAULT_TIMEOUT_MS);
      await typeLine(guest, "spawn ticker");
      await guest.waitForSerial("tick", DEFAULT_TIMEOUT_MS);
      await typeLine(guest, "ps");
      await guest.waitForSerial("ps: 1 crashy", DEFAULT_TIMEOUT_MS);
      await typeLine(guest, "kill 1");
      await guest.waitForSerial("kill: stopped task 1", DEFAULT_TIMEOUT_MS);
      await guest.waitForSerial("task 2 done", 60_000);

      // The shell still answers, libraries still link, and all user memory is back.
      await typeLine(guest, "pkg install strdemo");
      await guest.waitForSerial("pkg: installed strdemo", DEFAULT_TIMEOUT_MS);
      await typeLine(guest, "run strdemo");
      await guest.waitForSerial("2^10=1024", DEFAULT_TIMEOUT_MS);
      const before = guest.serial.length;
      await typeLine(guest, "mem");
      await waitAfter(guest, "user pages 0/", before);
    } finally {
      await guest.stop();
    }
  },
);

test(
  "memory comes back after many spawn/exit and crash cycles",
  { timeout: 240_000 },
  async () => {
    const image = await buildImage();
    const guest = await startGuest({ isoPath: image.isoPath, display: "none" });
    try {
      await guest.waitForSerial(PROMPT, DEFAULT_TIMEOUT_MS);
      await typeLine(guest, "pkg install strdemo");
      await guest.waitForSerial("pkg: installed strdemo", DEFAULT_TIMEOUT_MS);
      const rounds = 12;
      let mark = guest.serial.length;
      await typeLine(guest, "heap");
      await waitAfter(guest, "heap: used", mark);
      const heapBefore = heapUsed(guest.serial, mark);
      for (let i = 0; i < rounds; i++) {
        mark = guest.serial.length;
        await typeLine(guest, "run strdemo; run crashy null; run hello");
        await waitAfter(guest, "hello from program", mark);
      }
      assert.ok(countOf(guest.serial, "2^10=1024") >= rounds);
      assert.ok(countOf(guest.serial, "(crashy): page fault") >= rounds);
      const before = guest.serial.length;
      await typeLine(guest, "mem");
      await waitAfter(guest, "user pages 0/", before);
      mark = guest.serial.length;
      await typeLine(guest, "heap");
      await waitAfter(guest, "heap: used", mark);
      assert.equal(heapUsed(guest.serial, mark), heapBefore, "no kernel heap leak per run");
    } finally {
      await guest.stop();
    }
  },
);

test(
  "on the desktop a crash lands in Лента as a red «сбой» card",
  { timeout: 120_000 },
  async () => {
    const image = await buildImage();
    const guest = await startGuest({ isoPath: image.isoPath, display: "none" });
    try {
      await guest.waitForSerial(PROMPT, DEFAULT_TIMEOUT_MS);
      await typeLine(guest, "desktop");
      await guest.waitForSerial("home: ready", DEFAULT_TIMEOUT_MS);
      const mark = guest.serial.length;
      await typeLine(guest, "run crashy null");
      await waitAfter(guest, "task stopped, system OK", mark);
      await waitAfter(guest, "journal: x crashy: page fault at 0x0", mark);
      await typeLine(guest, "ps");
      await waitAfter(guest, "ps: 0 shell", mark);
    } finally {
      await guest.stop();
    }
  },
);
