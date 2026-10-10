import { deflateSync } from "node:zlib";

type RgbImage = {
  width: number;
  height: number;
  pixels: Buffer;
};

export function parsePpm(ppm: Buffer): RgbImage {
  if (ppm.length < 3 || ppm[0] !== 0x50 || ppm[1] !== 0x36) {
    throw new Error("Снимок экрана не PPM P6");
  }
  let offset = 2;
  const tokens: string[] = [];
  while (tokens.length < 3) {
    while (offset < ppm.length && ppm[offset]! <= 0x20) {
      offset++;
    }
    if (offset >= ppm.length) {
      throw new Error("Обрезанный заголовок PPM");
    }
    if (ppm[offset] === 0x23) {
      while (offset < ppm.length && ppm[offset] !== 0x0a) {
        offset++;
      }
      continue;
    }
    const start = offset;
    while (offset < ppm.length && ppm[offset]! > 0x20) {
      offset++;
    }
    tokens.push(ppm.subarray(start, offset).toString("ascii"));
  }
  if (offset < ppm.length && ppm[offset]! <= 0x20) {
    offset++;
  }
  const width = Number(tokens[0]);
  const height = Number(tokens[1]);
  const maxval = Number(tokens[2]);
  if (!Number.isInteger(width) || !Number.isInteger(height) || maxval !== 255) {
    throw new Error(`Неожиданный PPM ${tokens.join(" ")}`);
  }
  const need = width * height * 3;
  const pixels = ppm.subarray(offset, offset + need);
  if (pixels.length !== need) {
    throw new Error("В PPM не хватает пикселей");
  }
  return { width, height, pixels };
}

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
