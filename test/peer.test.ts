import assert from "node:assert/strict";
import { test } from "node:test";
import { startGuest, type Guest } from "../src/guest.js";
import { DEFAULT_TIMEOUT_MS, buildImage, hostToolError, missingTools } from "../src/pipeline.js";
import { click, countOf, delay, typeLine } from "./helpers.js";
import { fileRow } from "./ui.js";

function need(): void {
  const missing = missingTools(["qemu-system-x86_64"]);
  if (missing.length > 0) {
    throw hostToolError(missing);
  }
}

test("links: a program lights lines to the core and to its library", { timeout: 120_000 }, async () => {
  need();
  const image = await buildImage();
  const guest = await startGuest({ isoPath: image.isoPath, display: "none" });
  try {
    await guest.waitForSerial("genesis:/home> ", DEFAULT_TIMEOUT_MS);
    await typeLine(guest, "desktop");
    await guest.waitForSerial("home: ready", DEFAULT_TIMEOUT_MS);
    await typeLine(guest, "pkg install strdemo");
    await guest.waitForSerial("pkg: installed strdemo", DEFAULT_TIMEOUT_MS);
    await typeLine(guest, "run strdemo");
    await guest.waitForSerial("HELLO GENESIS", DEFAULT_TIMEOUT_MS);
    assert.match(guest.serial, /link: strdemo -> core/);
    assert.match(guest.serial, /link: strdemo -> strlib/);
  } finally {
    await guest.stop();
  }
});

test("Genesis Link: the peer shows up as a small core; a file goes to its /home/inbox", { timeout: 150_000 }, async () => {
  need();
  const port = 26000 + (process.pid % 14000);
  const image = await buildImage();
  const left = await startGuest({
    isoPath: image.isoPath,
    display: "none",
    net: { mode: "listen", port, mac: "52:54:00:12:34:20" },
  });
  let right: Guest | undefined;
  try {
    await left.waitForSerial("link: up", DEFAULT_TIMEOUT_MS);
    right = await startGuest({
      isoPath: image.isoPath,
      display: "none",
      net: { mode: "connect", port, mac: "52:54:00:12:34:21" },
    });
    await right.waitForSerial("link: up", DEFAULT_TIMEOUT_MS);
    await left.waitForSerial("genesis:/home> ", DEFAULT_TIMEOUT_MS);
    await right.waitForSerial("genesis:/home> ", DEFAULT_TIMEOUT_MS);
    await typeLine(right, "desktop");
    await right.waitForSerial("home: ready", DEFAULT_TIMEOUT_MS);
    await typeLine(left, "ping");
    await left.waitForSerial("pong", DEFAULT_TIMEOUT_MS);
    await right.waitForSerial("link: peer 52:54:00:12:34:20", DEFAULT_TIMEOUT_MS);
    // A file bigger than one chunk (112 bytes), sent from the shell.
    const part = "genesis-file-".repeat(6);
    await typeLine(left, `write /home/gift.txt ${part}`);
    await left.waitForSerial("write: ", DEFAULT_TIMEOUT_MS);
    for (let i = 0; i < 3; i++) {
      await typeLine(left, `echo ${part} >> /home/gift.txt`);
      await delay(300);
    }
    await typeLine(left, "wc /home/gift.txt");
    await left.waitForSerial("genesis:/home> wc /home/gift.txt\n", DEFAULT_TIMEOUT_MS);
    await delay(500);
    const wcLeft = left.serial.split("genesis:/home> wc /home/gift.txt\n")[1]!.split("\n").find((l) => /^\s*\d+\s+\d+/.test(l))!;
    await typeLine(left, "send gift.txt");
    await left.waitForSerial("send: done /home/gift.txt", DEFAULT_TIMEOUT_MS);
    await right.waitForSerial("inbox: /home/inbox/gift.txt (", DEFAULT_TIMEOUT_MS);
    const bytes = +right.serial.split("inbox: /home/inbox/gift.txt (")[1]!.split(" ")[0]!;
    assert.ok(bytes > 112 * 2, `мало байт: ${bytes}`);
    await right.waitForSerial("link: peer -> core", DEFAULT_TIMEOUT_MS);
    await typeLine(right, "wc /home/inbox/gift.txt");
    await right.waitForSerial("genesis:/home> wc /home/inbox/gift.txt\n", DEFAULT_TIMEOUT_MS);
    await delay(500);
    const wcRight = right.serial.split("genesis:/home> wc /home/inbox/gift.txt\n")[1]!.split("\n").find((l) => /^\s*\d+\s+\d+/.test(l))!;
    assert.equal(wcRight, wcLeft);
    // The other way, from the desktop: the peer core → pick readme.
    await delay(1500);
    await typeLine(right, "send /home/readme");
    await right.waitForSerial("send: done /home/readme", DEFAULT_TIMEOUT_MS);
    await left.waitForSerial("inbox: /home/inbox/readme", DEFAULT_TIMEOUT_MS);
    assert.equal(countOf(right.serial, "link: file damaged"), 0);
    void click;
    void fileRow;
  } finally {
    await left.stop();
    if (right) {
      await right.stop();
    }
  }
});
