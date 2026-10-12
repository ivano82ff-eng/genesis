/** «Ядро» desktop geometry at the default 1024x768 framebuffer. The core is
 * centred above the command bar; with a panel docked on the right the ring
 * slides left (core x = 300). Cells sit evenly on a ring, the first on top. */
const SIN = Array.from({ length: 91 }, (_, d) => Math.round(Math.sin((d * Math.PI) / 180) * 1024));
function isin(deg: number): number {
  deg = ((deg % 360) + 360) % 360;
  if (deg <= 90) return SIN[deg];
  if (deg <= 180) return SIN[180 - deg];
  return -isin(deg - 180);
}
const RING = 215;
export function coreAt(docked = false): { x: number; y: number } {
  return { x: docked ? 300 : 512, y: 370 };
}
/** Centre of cell i when n cells are shown. */
export function cellAt(i: number, n = 7, docked = false): { x: number; y: number } {
  const c = coreAt(docked);
  const mirror = i * 2 > n;
  const k = mirror ? n - i : i;
  const deg = Math.trunc((k * 360 + Math.trunc(n / 2)) / n);
  const dx = Math.trunc((isin(deg) * RING) / 1024);
  const dy = -Math.trunc((isin(deg + 90) * RING) / 1024);
  return { x: c.x + (mirror ? -dx : dx), y: c.y + dy };
}
export const CORE = coreAt();
export const CELL = {
  files: cellAt(0),
  editor: cellAt(1),
  terminal: cellAt(2),
  packages: cellAt(3),
  memory: cellAt(4),
  feed: cellAt(5),
  settings: cellAt(6),
  /** First program cell (8 cells, terminal docked), then a pin (9 cells). */
  extra1: cellAt(7, 8, true),
  extra2: cellAt(8, 9, true),
};
/** Close button of the docked panel. */
export const CLOSE = { x: 976, y: 94 };
/** Row R of the Files panel (row 0 is ".." outside /). */
export function fileRow(r: number): { x: number; y: number } {
  return { x: 700, y: 170 + 48 * r };
}
/** Settings buttons: 0 layout, 1 reboot, 2 power off. */
export function settingsButton(i: number): { x: number; y: number } {
  return { x: 802, y: 166 + 66 * i };
}
/** «стоп» of the Kth task row (row 0 is the shell). */
export function stopButton(k: number): { x: number; y: number } {
  return { x: 932, y: 162 + 70 * k };
}
/** Install/remove button of package row K. */
export function pkgButton(k: number): { x: number; y: number } {
  return { x: 917, y: 164 + 74 * k };
}
/** The command bar cursor (after "genesis:/home> "). */
export const BAR_CURSOR = { x: 308, y: 699 };
