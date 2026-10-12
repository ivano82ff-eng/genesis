import assert from "node:assert/strict";
import { createServer } from "node:http";
import type { AddressInfo } from "node:net";
import { test } from "node:test";
import type { Guest } from "../src/guest.js";
import { startGuest } from "../src/guest.js";
import { DEFAULT_TIMEOUT_MS, buildImage, hostToolError, missingTools } from "../src/pipeline.js";
import { click, countOf, delay, typeLine } from "./helpers.js";
import { CELL } from "./ui.js";

/** Tab through the window until a link whose href contains WANT has focus. */
async function tabTo(guest: Guest, want: string): Promise<void> {
  for (let i = 0; i < 16; i++) {
    const mark = guest.serial.length;
    await guest.sendKey("tab");
    await delay(250);
    const said = guest.serial.slice(mark);
    if (new RegExp(`focus: link \\d+ [^\\n]*${want}`).test(said)) return;
  }
  throw new Error(`ссылка ${want} не получила фокус\n${guest.serial.slice(-2000)}`);
}

async function waitMore(guest: Guest, needle: string, before: number): Promise<void> {
  const deadline = Date.now() + DEFAULT_TIMEOUT_MS;
  while (countOf(guest.serial, needle) <= before) {
    if (Date.now() > deadline) throw new Error(`нет нового «${needle}»\n${guest.serial.slice(-2000)}`);
    await delay(50);
  }
}

test("Браузер: cell, Tab+Enter on links, Back one page at a time, address bar, Esc", { timeout: 110_000 }, async () => {
  const missing = missingTools(["qemu-system-x86_64"]);
  if (missing.length > 0) throw hostToolError(missing);
  const image = await buildImage();
  const guest = await startGuest({ isoPath: image.isoPath, display: "none" });
  try {
    await guest.waitForSerial("genesis:/home> ", DEFAULT_TIMEOUT_MS);
    await typeLine(guest, "desktop");
    await guest.waitForSerial("cells: Файлы Редактор Терминал Пакеты Память Лента Настройки Браузер\n", DEFAULT_TIMEOUT_MS);
    await click(guest, CELL.browser.x, CELL.browser.y);
    await guest.waitForSerial('web: loaded about:home "Браузер genesis"', DEFAULT_TIMEOUT_MS);
    await tabTo(guest, "/docs/index.html");
    await guest.sendKey("ret");
    await guest.waitForSerial('web: loaded /docs/index.html "Добро пожаловать в genesis"', DEFAULT_TIMEOUT_MS);
    await tabTo(guest, "web.html");
    await guest.sendKey("ret");
    await guest.waitForSerial("web: loaded /docs/web.html", DEFAULT_TIMEOUT_MS);
    // Back: exactly one page, then the window closes.
    await guest.sendKey("backspace");
    await guest.waitForSerial("web: back /docs/index.html", DEFAULT_TIMEOUT_MS);
    await guest.sendKey("backspace");
    await guest.waitForSerial("web: back about:home", DEFAULT_TIMEOUT_MS);
    const homes = countOf(guest.serial, "home: ready");
    await guest.sendKey("backspace");
    await waitMore(guest, "home: ready", homes);
    // An address typed in the bar opens in the window; Esc closes it at once.
    await click(guest, CELL.browser.x, CELL.browser.y);
    const loads = countOf(guest.serial, "web: loaded about:home");
    await waitMore(guest, "web: loaded about:home", loads - 1);
    await typeLine(guest, "/docs/web.html");
    await guest.waitForSerial("web: open /docs/web.html", DEFAULT_TIMEOUT_MS);
    const homes2 = countOf(guest.serial, "home: ready");
    await guest.sendKey("esc");
    await waitMore(guest, "home: ready", homes2);
    assert.ok(true);
  } finally {
    await guest.stop();
  }
});

test("browse over HTTP: page, relative link, redirect, error page", { timeout: 110_000 }, async () => {
  const missing = missingTools(["qemu-system-x86_64"]);
  if (missing.length > 0) throw hostToolError(missing);
  const http = createServer((req, res) => {
    if (req.url === "/r") {
      res.writeHead(302, { location: "/two.html" });
      res.end();
      return;
    }
    if (req.url === "/two.html") {
      res.writeHead(200, { "content-type": "text/html; charset=utf-8" });
      res.end("<html><head><title>Вторая</title></head><body><h1>Вторая страница</h1><p>ok-two</p></body></html>");
      return;
    }
    res.writeHead(200, { "content-type": "text/html; charset=utf-8" });
    res.end(
      "<!doctype html><html><head><title>Хост</title><script>var x='<p>hidden</p>';</script></head>" +
        "<body><h1>Привет из хоста</h1><p>Абзац &amp; текст.</p><ul><li><a href=\"two.html\">вторая</a></li>" +
        "<li><a href=\"/r\">через редирект</a></li></ul></body></html>",
    );
  });
  await new Promise<void>((r) => http.listen(0, "127.0.0.1", r));
  const port = (http.address() as AddressInfo).port;
  const image = await buildImage();
  const guest = await startGuest({ isoPath: image.isoPath, display: "none", usernet: { nic: "ne2k_pci" } });
  try {
    await guest.waitForSerial("genesis:/home> ", DEFAULT_TIMEOUT_MS);
    await typeLine(guest, "dhcp");
    await guest.waitForSerial("dhcp: 10.0.2.15", DEFAULT_TIMEOUT_MS);
    await typeLine(guest, `browse 10.0.2.2:${port}/`);
    await guest.waitForSerial(`web: loaded http://10.0.2.2:${port}/ "Хост" (`, DEFAULT_TIMEOUT_MS);
    await guest.waitForSerial("# Привет из хоста", DEFAULT_TIMEOUT_MS);
    await guest.waitForSerial("Абзац & текст.", DEFAULT_TIMEOUT_MS);
    assert.ok(!guest.serial.includes("hidden"), "скрипт попал на страницу");
    await typeLine(guest, "web follow 1");
    await guest.waitForSerial(`web: loaded http://10.0.2.2:${port}/two.html "Вторая"`, DEFAULT_TIMEOUT_MS);
    await typeLine(guest, "web back");
    await guest.waitForSerial(`web: back http://10.0.2.2:${port}/`, DEFAULT_TIMEOUT_MS);
    const twos = countOf(guest.serial, `two.html "Вторая"`);
    await typeLine(guest, "web follow 2");
    await waitMore(guest, `two.html "Вторая"`, twos);
    await typeLine(guest, "browse http://10.0.2.2:1/");
    await guest.waitForSerial("web: error http://10.0.2.2:1/", DEFAULT_TIMEOUT_MS);
    await guest.waitForSerial("Страница не открылась", DEFAULT_TIMEOUT_MS);
  } finally {
    await guest.stop();
    http.close();
  }
});
