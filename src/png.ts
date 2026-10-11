import { deflateSync } from "node:zlib";
import { parsePpm, type RgbImage } from "./ppm.js";

export { parsePpm };
export type { RgbImage };

function crc(data: Buffer): number {
  let value = ~0;
  for (const byte of data) {
    value ^= byte;
    for (let bit = 0; bit < 8; bit++) {
      value = (value >>> 1) ^ (0xedb88320 & -(value & 1));
    }
  }
  return ~value >>> 0;
}

function chunk(tag: string, data: Buffer): Buffer {
  const name = Buffer.from(tag, "ascii");
  const head = Buffer.alloc(4);
  head.writeUInt32BE(data.length, 0);
  const body = Buffer.concat([name, data]);
  const sum = Buffer.alloc(4);
  sum.writeUInt32BE(crc(body), 0);
  return Buffer.concat([head, body, sum]);
}

export function ppmToPng(ppm: Buffer): Buffer {
  const image = parsePpm(ppm);
  const raw = Buffer.alloc(image.height * (1 + image.width * 3));
  for (let y = 0; y < image.height; y++) {
    const row = y * (1 + image.width * 3);
    raw[row] = 0;
    image.pixels.copy(raw, row + 1, y * image.width * 3, (y + 1) * image.width * 3);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(image.width, 0);
  ihdr.writeUInt32BE(image.height, 4);
  ihdr[8] = 8;
  ihdr[9] = 2;
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk("IHDR", ihdr),
    chunk("IDAT", deflateSync(raw)),
    chunk("IEND", Buffer.alloc(0)),
  ]);
}
