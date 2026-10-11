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

/** ЙЦУКЕН: Cyrillic letter -> QEMU key in the genesis Русская layout. */
const RU_KEYS: Record<string, string> = {
  й: "q", ц: "w", у: "e", к: "r", е: "t", н: "y", г: "u", ш: "i", щ: "o", з: "p", х: "bracket_left",
  ъ: "bracket_right", ф: "a", ы: "s", в: "d", а: "f", п: "g", р: "h", о: "j", л: "k", д: "l",
  ж: "semicolon", э: "apostrophe", я: "z", ч: "x", с: "c", м: "v", и: "b", т: "n", ь: "m",
  б: "comma", ю: "dot", ё: "grave_accent",
};

/** Keys for a character typed in the Русская layout, or null when it has none. */
export function ruKeysFor(ch: string): string[] | null {
  if (ch === "№") {
    return ["shift", "3"];
  }
  const lower = ch.toLowerCase();
  const key = RU_KEYS[lower];
  if (key == null) {
    return null;
  }
  return lower === ch ? [key] : ["shift", key];
}

/** True for characters that type the same in both layouts. */
function neutral(ch: string): boolean {
  return ch === " " || (ch >= "0" && ch <= "9") || ch === "-" || ch === "=";
}

const ruActive = new WeakMap<Guest, boolean>();

/** Ctrl+Space switches genesis between Latin and Русская. */
export async function toggleLayout(guest: Guest): Promise<void> {
  await guest.qmp("send-key", { keys: [{ type: "qcode", data: "ctrl" }, { type: "qcode", data: "spc" }] });
  ruActive.set(guest, !(ruActive.get(guest) ?? false));
  await delay(40);
}

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
    const ru = ruKeysFor(ch);
    if (!neutral(ch) && (ru != null) !== (ruActive.get(guest) ?? false)) {
      await toggleLayout(guest);
    }
    const keys = ru ?? keysFor(ch);
    if (keys.length === 1) {
      await guest.sendKey(keys[0]!);
    } else {
      await guest.qmp("send-key", { keys: keys.map((data) => ({ type: "qcode", data })) });
    }
    await delay(35);
  }
  if (ruActive.get(guest)) {
    await toggleLayout(guest);
  }
  await guest.sendKey("ret");
  await delay(40);
}
