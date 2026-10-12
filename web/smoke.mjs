// Smoke test for the web build: serve web/ (with genesis.iso next to it), open it
// in headless Chromium and wait until genesis reports its desktop on COM1.
// Usage: node web/smoke.mjs [site dir] [screenshot dir]
import { createServer } from "node:http";
import { readFile, mkdir } from "node:fs/promises";
import path from "node:path";
import { chromium } from "playwright";

const site = path.resolve(process.argv[2] ?? "web");
const shots = path.resolve(process.argv[3] ?? "build/web-smoke");
const types = { ".html": "text/html", ".js": "text/javascript", ".css": "text/css", ".wasm": "application/wasm" };

const server = createServer(async (req, res) => {
  const url = new URL(req.url, "http://x");
  const file = path.join(site, decodeURIComponent(url.pathname === "/" ? "/index.html" : url.pathname));
  try {
    const body = await readFile(file);
    res.writeHead(200, { "content-type": types[path.extname(file)] ?? "application/octet-stream" });
    res.end(body);
  } catch {
    res.writeHead(404);
    res.end();
  }
});
await new Promise((r) => server.listen(0, "127.0.0.1", r));
const base = `http://127.0.0.1:${server.address().port}/`;
await mkdir(shots, { recursive: true });

const browser = await chromium.launch(process.env.CHROME ? { executablePath: process.env.CHROME } : {});
let failed = false;
for (const [name, viewport, extra] of [
  ["desktop", { width: 1280, height: 800 }, ""],
  ["phone", { width: 390, height: 844 }, ""],
]) {
  const page = await browser.newPage({ viewport, hasTouch: name === "phone" });
  page.on("pageerror", (e) => console.error(`[${name}] page error: ${e.message}`));
  await page.goto(base + extra);
  try {
    await page.waitForFunction(() => (window.genesisSerial ?? "").includes("home: ready"), null, {
      timeout: 240_000,
      polling: 1000,
    });
    await page.waitForTimeout(1500);
    const size = await page.evaluate(() => [document.getElementById("vga").width, document.getElementById("vga").height]);
    console.log(`[${name}] desktop ready, framebuffer ${size.join("x")}`);
    if (name === "phone" && !(size[1] > size[0])) {
      console.error(`[${name}] expected a portrait framebuffer`);
      failed = true;
    }
  } catch (e) {
    console.error(`[${name}] no desktop: ${e.message}`);
    failed = true;
  }
  const serial = await page.evaluate(() => window.genesisSerial ?? "");
  console.log(`[${name}] serial:\n${serial.slice(-600)}`);
  await page.screenshot({ path: path.join(shots, `${name}.png`) });
  await page.close();
}
await browser.close();
server.close();
process.exit(failed ? 1 : 0);
