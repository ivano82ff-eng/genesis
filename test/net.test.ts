import assert from "node:assert/strict";
import { createServer as createHttp } from "node:http";
import { createServer as createTcp } from "node:net";
import { createSocket } from "node:dgram";
import type { AddressInfo } from "node:net";
import { test } from "node:test";
import { startGuest } from "../src/guest.js";
import { DEFAULT_TIMEOUT_MS, buildImage, hostToolError, missingTools } from "../src/pipeline.js";
import { typeLine } from "./helpers.js";

for (const nic of ["rtl8139", "ne2k_pci"] as const) {
  test(`TCP/IP over ${nic}: dhcp, ping, dns, http, nc, udp`, { timeout: 180_000 }, async () => {
    const missing = missingTools(["qemu-system-x86_64"]);
    if (missing.length > 0) {
      throw hostToolError(missing);
    }
    // Host services; the guest sees the host as 10.0.2.2 (slirp).
    const http = createHttp((req, res) => {
      res.writeHead(200, { "content-type": "text/plain" });
      res.end(`hello genesis, you asked for ${req.url}\n` + "x".repeat(3000) + "\nend-of-body\n");
    });
    await new Promise<void>((r) => http.listen(0, "127.0.0.1", r));
    const echo = createTcp((sock) => sock.on("data", (d) => sock.end(`echo: ${d.toString().trim()}\n`)));
    await new Promise<void>((r) => echo.listen(0, "127.0.0.1", r));
    const udp = createSocket("udp4");
    udp.on("message", (msg, rinfo) => udp.send(`pong ${msg}`, rinfo.port, rinfo.address));
    await new Promise<void>((r) => udp.bind(0, "127.0.0.1", r));
    const httpPort = (http.address() as AddressInfo).port;
    const echoPort = (echo.address() as AddressInfo).port;
    const udpPort = udp.address().port;

    const image = await buildImage();
    const guest = await startGuest({ isoPath: image.isoPath, display: "none", usernet: { nic } });
    try {
      await guest.waitForSerial("genesis:/home> ", DEFAULT_TIMEOUT_MS);
      await typeLine(guest, "dhcp");
      await guest.waitForSerial("dhcp: 10.0.2.15 gw 10.0.2.2 dns 10.0.2.3", DEFAULT_TIMEOUT_MS);
      await typeLine(guest, "net");
      await guest.waitForSerial(nic === "ne2k_pci" ? "net: ne2k mac 52:54:00:12:34:56 ip 10.0.2.15" : "net: rtl8139 mac 52:54:00:12:34:56 ip 10.0.2.15", DEFAULT_TIMEOUT_MS);
      await typeLine(guest, "ping 10.0.2.2");
      await guest.waitForSerial("ping: 4 sent, 4 received", 30_000);
      await typeLine(guest, `http http://10.0.2.2:${httpPort}/hi`);
      await guest.waitForSerial("hello genesis, you asked for /hi", DEFAULT_TIMEOUT_MS);
      await guest.waitForSerial("end-of-body", DEFAULT_TIMEOUT_MS);
      await guest.waitForSerial("http: ", DEFAULT_TIMEOUT_MS);
      await typeLine(guest, `nc 10.0.2.2 ${echoPort} привет`);
      await guest.waitForSerial("echo: привет", DEFAULT_TIMEOUT_MS);
      await guest.waitForSerial("nc: closed", DEFAULT_TIMEOUT_MS);
      await typeLine(guest, `udp 10.0.2.2 ${udpPort} hi`);
      await guest.waitForSerial("udp: pong hi", DEFAULT_TIMEOUT_MS);
      // A C program talks TCP through the cc builtins.
      if (nic === "rtl8139") {
        await typeLine(guest, `echo 'int main(){char b[64];int s;s=tcp_connect("10.0.2.2",${echoPort});' > /home/t.c`);
        await typeLine(guest, `echo 'tcp_send(s,"cc-hi",5);b[tcp_recv(s,b,60)]=0;puts(b);return 0;}' >> /home/t.c`);
        await typeLine(guest, "cc /home/t.c");
        await guest.waitForSerial("echo: cc-hi", DEFAULT_TIMEOUT_MS);
      }
      // DNS through slirp's resolver (10.0.2.3 forwards to the host's).
      await typeLine(guest, "dns localhost");
      await guest.waitForSerial("dns: localhost -> ", DEFAULT_TIMEOUT_MS);
      // The shell is still fine.
      await typeLine(guest, "echo ok-after-net");
      await guest.waitForSerial("ok-after-net\n", DEFAULT_TIMEOUT_MS);
      assert.ok(true);
    } finally {
      await guest.stop();
      http.close();
      echo.close();
      udp.close();
    }
  });
}
