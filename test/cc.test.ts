import assert from "node:assert/strict";
import { test } from "node:test";
import { startGuest } from "../src/guest.js";
import { DEFAULT_TIMEOUT_MS, buildImage, hostToolError, missingTools } from "../src/pipeline.js";
import { typeLine } from "./helpers.js";

test("cc compiles and runs C inside genesis", { timeout: 120_000 }, async () => {
  const missing = missingTools(["qemu-system-x86_64"]);
  if (missing.length > 0) {
    throw hostToolError(missing);
  }
  const image = await buildImage();
  const guest = await startGuest({ isoPath: image.isoPath, display: "none" });
  try {
    await guest.waitForSerial("genesis:/home> ", DEFAULT_TIMEOUT_MS);
    await typeLine(guest, "cc /docs/hello.c");
    await guest.waitForSerial("hello from C, 2 + 3 = 5", DEFAULT_TIMEOUT_MS);
    await typeLine(guest, "run /docs/fib.c");
    await guest.waitForSerial("0 1 1 2 3 5 8 13 21 34 55 89 144 233 377", DEFAULT_TIMEOUT_MS);
    await guest.waitForSerial("siseneg", DEFAULT_TIMEOUT_MS);
    // A program written in the shell: loops, pointers, exit code.
    await typeLine(guest, "write /home/sq.c 'int main(){int i,s=0;for(i=1;i<11;i++)s+=i*i;printf(\"sq %d\\n\",s);return 7;}'");
    await typeLine(guest, "cc /home/sq.c");
    await guest.waitForSerial("sq 385", DEFAULT_TIMEOUT_MS);
    await guest.waitForSerial("cc: exit 7", DEFAULT_TIMEOUT_MS);
    // Errors point at the line; a wild pointer is stopped by the sandbox.
    await typeLine(guest, "write /home/bad.c 'int main() { int x = 1 return x; }'");
    await typeLine(guest, "cc /home/bad.c");
    await guest.waitForSerial("cc: /home/bad.c:1: ; expected", DEFAULT_TIMEOUT_MS);
    await typeLine(guest, "write /home/np.c 'int main() { int *p = 0; return *p; }'");
    await typeLine(guest, "cc /home/np.c");
    await guest.waitForSerial("cc: runtime error: null pointer", DEFAULT_TIMEOUT_MS);
    // The shell is still alive afterwards.
    await typeLine(guest, "echo still-here");
    await guest.waitForSerial("still-here\n", DEFAULT_TIMEOUT_MS);
    assert.ok(true);
  } finally {
    await guest.stop();
  }
});
