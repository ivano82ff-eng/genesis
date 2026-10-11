/** «Ядро» desktop coordinates at the default 1024x768 framebuffer. */
export const CORE = { x: 300, y: 370 };
export const CELL = {
  files: { x: 300, y: 155 },
  editor: { x: 486, y: 262 },
  terminal: { x: 486, y: 478 },
  packages: { x: 300, y: 585 },
  memory: { x: 114, y: 478 },
  feed: { x: 114, y: 262 },
  settings: { x: 408, y: 184 },
  /** First program/pin cell, then the next one. */
  extra1: { x: 515, y: 370 },
  extra2: { x: 408, y: 556 },
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
