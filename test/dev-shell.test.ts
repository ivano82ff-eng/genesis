import assert from "node:assert/strict";
import { test } from "node:test";
import { startGuest } from "../src/guest.js";
import { DEFAULT_TIMEOUT_MS, buildImage, hostToolError, missingTools } from "../src/pipeline.js";
import { countOf, delay, typeLine } from "./helpers.js";

test("developer shell: shift, variables, redirects, pipes, scripts, history", { timeout: 240_000 }, async () => {
  const missing = missingTools(["qemu-system-x86_64"]);
  if (missing.length > 0) {
    throw hostToolError(missing);
  }
  const image = await buildImage();
  const guest = await startGuest({ isoPath: image.isoPath, display: "none" });
  const ask = async (cmd: string, answer: string) => {
    await typeLine(guest, cmd);
    await guest.waitForSerial(answer, DEFAULT_TIMEOUT_MS);
  };
  try {
    await guest.waitForSerial("genesis> ", DEFAULT_TIMEOUT_MS);
    // Shift: capitals and symbols reach the shell (quotes keep them literal).
    await ask('echo "A_B:C!|@<>"', "\nA_B:C!|@<>\n");
    // Variables and ; sequencing.
    await ask('X=hello; echo "$X world"', "\nhello world\n");
    // Redirect then cat; >> appends.
    await ask("echo one > f1; echo two >> f1; cat f1", "cat: one\ntwo\n");
    // Pipes.
    await ask("ls | grep ticker > found; cat found", "cat: ls:");
    for (let i = 0; i < 100 && !/cat: ls:[^\n]*\n/.test(guest.serial); i++) {
      await delay(100);
    }
    assert.match(guest.serial, /cat: ls:[^\n]*ticker/);
    await ask("ls | grep nothing-here | wc", "\n0 0 0\n");
    // A script with a comment, single quotes and a variable.
    await ask("echo 'Y=two # comment' > s1; echo 'echo got-$Y' >> s1; sh s1", "\ngot-two\n");
    // History: arrow up repeats the last line.
    await ask("echo hist-one", "\nhist-one\n");
    await guest.sendKey("up");
    await delay(100);
    await guest.sendKey("ret");
    const deadline = Date.now() + DEFAULT_TIMEOUT_MS;
    while (countOf(guest.serial, "\nhist-one\n") < 2 && Date.now() < deadline) {
      await delay(100);
    }
    assert.equal(countOf(guest.serial, "\nhist-one\n"), 2);
  } finally {
    await guest.stop();
  }
});
