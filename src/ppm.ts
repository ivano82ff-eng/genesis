export type RgbImage = {
  width: number;
  height: number;
  pixels: Buffer;
};

/** Parse a PPM P6 screenshot emitted by QEMU. */
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
