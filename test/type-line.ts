import type { Guest } from "../src/guest.js";

function delay(ms: number): Promise<void> {
  return new Promise((resolve) => {
    setTimeout(resolve, ms);
  });
}

export async function typeLine(guest: Guest, text: string): Promise<void> {
  for (const ch of text) {
    const key = ch === " " ? "spc" : ch === "-" ? "minus" : ch;
    await guest.sendKey(key);
    await delay(35);
  }
  await guest.sendKey("ret");
  await delay(40);
}
