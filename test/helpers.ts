import type { Guest } from "../src/guest.js";
import { delay, typeLine } from "../src/typing.js";

export { delay, typeLine };

export function countOf(text: string, needle: string): number {
  let count = 0;
  let from = 0;
  while (from < text.length) {
    const at = text.indexOf(needle, from);
    if (at < 0) {
      return count;
    }
    count += 1;
    from = at + needle.length;
  }
  return count;
}

export function pixel(
  image: { width: number; pixels: Buffer },
  x: number,
  y: number,
): [number, number, number] {
  const index = (y * image.width + x) * 3;
  return [image.pixels[index]!, image.pixels[index + 1]!, image.pixels[index + 2]!];
}

/** Push the pointer into the top-left corner (the guest clamps it at 0,0). */
export async function homePointer(guest: Guest): Promise<void> {
  for (let i = 0; i < 17; i++) {
    await guest.monitor("mouse_move -120 -120");
    await delay(15);
  }
  await delay(300);
}

/** Move the pointer to (x, y) in steps and press the left button. */
export async function click(guest: Guest, x: number, y: number): Promise<void> {
  await homePointer(guest);
  let left = x;
  let top = y;
  while (left > 0 || top > 0) {
    const stepX = Math.min(40, left);
    const stepY = Math.min(40, top);
    await guest.monitor(`mouse_move ${stepX} ${stepY}`);
    left -= stepX;
    top -= stepY;
    await delay(200);
  }
  try {
    await guest.monitor("mouse_button 1");
    await delay(80);
    await guest.monitor("mouse_button 0");
  } catch {
    // the guest may keep QMP busy or close it (poweroff) before the reply
  }
}

async function moveBy(guest: Guest, dx: number, dy: number): Promise<void> {
  while (dx !== 0 || dy !== 0) {
    const sx = Math.max(-40, Math.min(40, dx));
    const sy = Math.max(-40, Math.min(40, dy));
    await guest.monitor(`mouse_move ${sx} ${sy}`);
    dx -= sx;
    dy -= sy;
    await delay(200);
  }
}

/** Press at (x1, y1), move with the button held, release at (x2, y2). */
export async function drag(guest: Guest, x1: number, y1: number, x2: number, y2: number): Promise<void> {
  await homePointer(guest);
  await moveBy(guest, x1, y1);
  await guest.monitor("mouse_button 1");
  await delay(150);
  await moveBy(guest, x2 - x1, y2 - y1);
  await delay(150);
  await guest.monitor("mouse_button 0");
  await delay(150);
}

/** Hold the left button at (x, y) for MS milliseconds (touch long-press). */
export async function longPress(guest: Guest, x: number, y: number, ms = 900): Promise<void> {
  await homePointer(guest);
  await moveBy(guest, x, y);
  await guest.monitor("mouse_button 1");
  await delay(ms);
  await guest.monitor("mouse_button 0");
  await delay(150);
}
