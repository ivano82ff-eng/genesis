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
