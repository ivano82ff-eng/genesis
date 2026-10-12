/* genesis in the browser: v86 boots the i686 build (genesis.iso) with a
 * 32 MiB disk kept in IndexedDB, then opens the desktop sized for the screen.
 * Touch and the on-screen pad turn into PS/2 mouse and keys. */
"use strict";

const DISK_BYTES = 32 * 1024 * 1024;
const DB_NAME = "genesis";
const params = new URLSearchParams(location.search);
const $ = (id) => document.getElementById(id);

/* ---------- disk in IndexedDB ---------- */

function openDb() {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open(DB_NAME, 1);
    req.onupgradeneeded = () => req.result.createObjectStore("disk");
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}

async function loadDisk() {
  try {
    const db = await openDb();
    const saved = await new Promise((resolve) => {
      const req = db.transaction("disk").objectStore("disk").get("hda");
      req.onsuccess = () => resolve(req.result);
      req.onerror = () => resolve(null);
    });
    if (saved instanceof ArrayBuffer && saved.byteLength === DISK_BYTES) {
      return { db, buffer: saved };
    }
    return { db, buffer: new ArrayBuffer(DISK_BYTES) };
  } catch {
    return { db: null, buffer: new ArrayBuffer(DISK_BYTES) }; // private mode: no saving
  }
}

function saveDisk(db, buffer) {
  if (!db) return Promise.resolve();
  return new Promise((resolve) => {
    const tx = db.transaction("disk", "readwrite");
    tx.objectStore("disk").put(buffer.slice(0), "hda");
    tx.oncomplete = tx.onerror = tx.onabort = () => resolve();
  });
}

/* ---------- PS/2 helpers ---------- */

const SC = {
  up: [0xe0, 0x48], down: [0xe0, 0x50], left: [0xe0, 0x4b], right: [0xe0, 0x4d],
  enter: [0x1c], esc: [0x01], tab: [0x0f], backspace: [0x0e],
};

function press(emu, codes) {
  const up = codes.length === 2 ? [codes[0], codes[1] | 0x80] : [codes[0] | 0x80];
  emu.keyboard_send_scancodes([...codes, ...up]);
}

/* ЙЦУКЕН: Cyrillic letter -> the Latin key at the same place (genesis Русская layout). */
const RU = { й: "q", ц: "w", у: "e", к: "r", е: "t", н: "y", г: "u", ш: "i", щ: "o", з: "p", х: "[", ъ: "]",
  ф: "a", ы: "s", в: "d", а: "f", п: "g", р: "h", о: "j", л: "k", д: "l", ж: ";", э: "'",
  я: "z", ч: "x", с: "c", м: "v", и: "b", т: "n", ь: "m", б: ",", ю: ".", ё: "`" };
const SHIFTED = { "[": "{", "]": "}", ";": ":", "'": '"', ",": "<", ".": ">", "`": "~" };

/* ---------- main ---------- */

async function main() {
  const portrait = innerHeight > innerWidth;
  const size = params.get("size") || (portrait ? "480x800" : "1024x768");
  const wantDesktop = !params.has("shell");
  const { db, buffer } = await loadDisk();

  const emu = new V86({
    wasm_path: "v86/v86.wasm",
    bios: { url: "v86/seabios.bin" },
    vga_bios: { url: "v86/vgabios.bin" },
    cdrom: { url: "genesis.iso" },
    hda: { buffer },
    boot_order: 0x123,
    memory_size: 128 * 1024 * 1024,
    vga_memory_size: 16 * 1024 * 1024,
    screen_container: $("screen_container"),
    disable_mouse: true,
    disable_speaker: true,
    autostart: true,
  });
  window.genesis = emu;

  /* loading indicator */
  const bar = $("loading_bar").firstElementChild;
  emu.add_listener("download-progress", (e) => {
    if (e.lengthComputable && e.total) bar.style.width = `${Math.round((100 * e.loaded) / e.total)}%`;
  });
  emu.add_listener("emulator-ready", () => {
    $("loading_text").textContent = "Запуск ядра genesis…";
    bar.style.width = "100%";
  });
  const done = () => $("loading").classList.add("done");

  /* serial log (UTF-8) drives the start-up and the smoke test */
  const decoder = new TextDecoder();
  window.genesisSerial = "";
  let desktopSent = false;
  emu.add_listener("serial0-output-byte", (byte) => {
    window.genesisSerial += decoder.decode(new Uint8Array([byte]), { stream: true });
    const log = window.genesisSerial;
    if (!desktopSent && log.includes("genesis:/home> ")) {
      desktopSent = true;
      if (wantDesktop) {
        $("loading_text").textContent = "Открываю рабочий стол…";
        emu.keyboard_send_text(`desktop ${size}\n`);
      } else {
        done();
      }
    }
    if (log.endsWith("home: ready")) done();
  });
  setTimeout(done, 90000); // never hide the system behind the splash forever

  /* save the disk a moment after the guest stops writing */
  let saveTimer = 0;
  const saveSoon = () => {
    clearTimeout(saveTimer);
    saveTimer = setTimeout(() => saveDisk(db, buffer), 1500);
  };
  emu.add_listener("ide-write-end", saveSoon);
  addEventListener("pagehide", () => saveDisk(db, buffer));
  document.addEventListener("visibilitychange", () => {
    if (document.visibilityState === "hidden") saveDisk(db, buffer);
  });

  /* fit the canvas to the window, keeping the pixels square */
  const canvas = $("vga");
  const fit = () => {
    if (!canvas.width || !canvas.height) return;
    const s = Math.min(innerWidth / canvas.width, innerHeight / canvas.height);
    canvas.style.width = `${Math.floor(canvas.width * s)}px`;
    canvas.style.height = `${Math.floor(canvas.height * s)}px`;
  };
  let lastW = 0, lastH = 0;
  setInterval(() => {
    if (canvas.width !== lastW || canvas.height !== lastH) {
      lastW = canvas.width;
      lastH = canvas.height;
      fit();
    }
  }, 300);
  addEventListener("resize", fit);

  /* touch / mouse -> PS/2 mouse: absolute point -> relative moves */
  let cx = 0, cy = 0, homed = false;
  const nudge = (dx, dy) => {
    while (dx !== 0 || dy !== 0) {
      const sx = Math.max(-100, Math.min(100, dx));
      const sy = Math.max(-100, Math.min(100, dy));
      emu.bus.send("mouse-delta", [sx, -sy]); // bus: positive y is up
      dx -= sx;
      dy -= sy;
    }
  };
  const moveTo = (x, y) => {
    if (!homed) {
      nudge(-4000, -4000);
      cx = cy = 0;
      homed = true;
    }
    x = Math.max(0, Math.min(canvas.width - 1, x));
    y = Math.max(0, Math.min(canvas.height - 1, y));
    nudge(x - cx, y - cy);
    cx = x;
    cy = y;
  };
  const toScreen = (e) => {
    const r = canvas.getBoundingClientRect();
    return [Math.round(((e.clientX - r.left) * canvas.width) / r.width),
      Math.round(((e.clientY - r.top) * canvas.height) / r.height)];
  };
  const box = $("screen_container");
  box.addEventListener("pointerdown", (e) => {
    if (!canvas.width) return;
    e.preventDefault();
    $("typer").blur();
    const [x, y] = toScreen(e);
    moveTo(x, y);
    emu.bus.send("mouse-click", [true, false, false]);
  });
  box.addEventListener("pointerup", (e) => {
    if (!canvas.width) return;
    e.preventDefault();
    emu.bus.send("mouse-click", [false, false, false]);
  });
  box.addEventListener("pointermove", (e) => {
    if (!canvas.width || (e.pointerType !== "mouse" && e.buttons === 0)) return;
    const [x, y] = toScreen(e);
    moveTo(x, y);
  });
  box.addEventListener("contextmenu", (e) => e.preventDefault());

  /* on-screen pad (phones, TV remotes) */
  let ru = false;
  const toggleLayout = () => {
    emu.keyboard_send_scancodes([0x1d, 0x39, 0xb9, 0x9d]); // Ctrl+Space
    ru = !ru;
  };
  for (const b of document.querySelectorAll("#pad button")) {
    b.addEventListener("pointerdown", (e) => {
      e.preventDefault();
      const key = b.dataset.key;
      if (key === "layout") toggleLayout();
      else press(emu, SC[key]);
    });
    b.addEventListener("click", (e) => { // TV remotes and keyboards "click" a focused button
      if (e.detail === 0) {
        const key = b.dataset.key;
        if (key === "layout") toggleLayout();
        else press(emu, SC[key]);
      }
    });
  }
  const pad = $("pad");
  const showPad = (on) => {
    pad.hidden = !on;
    $("btn_keys").classList.toggle("on", on);
  };
  showPad(matchMedia("(pointer: coarse)").matches);
  $("btn_keys").addEventListener("click", () => showPad(pad.hidden));

  /* native keyboard for text: a hidden input with one sentinel space */
  const typer = $("typer");
  const sendChar = (ch) => {
    const lower = ch.toLowerCase();
    if (RU[lower]) {
      if (!ru) toggleLayout();
      let key = RU[lower];
      if (lower !== ch) key = SHIFTED[key] || key.toUpperCase();
      emu.keyboard_send_text(key);
    } else {
      if (ru && /[a-zA-Z]/.test(ch)) toggleLayout();
      emu.keyboard_send_text(ch);
    }
  };
  const resetTyper = () => {
    typer.value = " ";
    typer.setSelectionRange(1, 1);
  };
  typer.addEventListener("input", () => {
    const v = typer.value;
    if (v.length === 0) press(emu, SC.backspace);
    else for (const ch of v.slice(1)) sendChar(ch);
    resetTyper();
  });
  typer.addEventListener("keydown", (e) => {
    const map = { Enter: "enter", Escape: "esc", Tab: "tab", ArrowUp: "up", ArrowDown: "down",
      ArrowLeft: "left", ArrowRight: "right" };
    if (map[e.key]) {
      e.preventDefault();
      press(emu, SC[map[e.key]]);
    }
  });
  $("btn_type").addEventListener("click", () => {
    resetTyper();
    typer.focus();
  });

  /* fullscreen */
  $("btn_full").addEventListener("click", () => {
    if (document.fullscreenElement) document.exitFullscreen();
    else document.documentElement.requestFullscreen?.().catch(() => {});
  });
}

main();
