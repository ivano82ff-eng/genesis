import type { Guest } from "./guest.js";

export function delay(ms: number): Promise<void> {
  return new Promise((resolve) => {
    setTimeout(resolve, ms);
  });
}

const PLAIN: Record<string, string> = {
  " ": "spc",
  "-": "minus",
  ".": "dot",
  ",": "comma",
  "/": "slash",
  ";": "semicolon",
  "'": "apostrophe",
  "`": "grave_accent",
  "\\": "backslash",
  "=": "equal",
  "[": "bracket_left",
  "]": "bracket_right",
};

const SHIFTED: Record<string, string> = {
  "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7", "*": "8",
  "(": "9", ")": "0", "_": "minus", "+": "equal", ":": "semicolon", '"': "apostrophe",
  "~": "grave_accent", "|": "backslash", "<": "comma", ">": "dot", "?": "slash",
  "{": "bracket_left", "}": "bracket_right",
};

/** QEMU key codes for one character: [key] or ["shift", key]. */
export function keysFor(ch: string): string[] {
  if (ch >= "A" && ch <= "Z") {
    return ["shift", ch.toLowerCase()];
  }
  if (SHIFTED[ch]) {
    return ["shift", SHIFTED[ch]];
  }
  return [PLAIN[ch] ?? ch];
}

/** Type a line into the guest shell, one key at a time. */
export async function typeLine(guest: Guest, text: string): Promise<void> {
  for (const ch of text) {
    const keys = keysFor(ch);
    if (keys.length === 1) {
      await guest.sendKey(keys[0]!);
    } else {
      await guest.qmp("send-key", { keys: keys.map((data) => ({ type: "qcode", data })) });
    }
    await delay(35);
  }
  await guest.sendKey("ret");
  await delay(40);
}
