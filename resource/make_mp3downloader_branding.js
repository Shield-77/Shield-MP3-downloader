// Small dependency-free PNG generator for the MP3 Downloader 3DS icon/banner.
const fs = require('fs');
const zlib = require('zlib');

const glyphs = {
  A:'01110/10001/10001/11111/10001/10001/10001', B:'11110/10001/10001/11110/10001/10001/11110',
  D:'11110/10001/10001/10001/10001/10001/11110', E:'11111/10000/10000/11110/10000/10000/11111',
  L:'10000/10000/10000/10000/10000/10000/11111',
  M:'10001/11011/10101/10101/10001/10001/10001', N:'10001/11001/10101/10011/10001/10001/10001',
  O:'01110/10001/10001/10001/10001/10001/01110', P:'11110/10001/10001/11110/10000/10000/10000',
  R:'11110/10001/10001/11110/10100/10010/10001', W:'10001/10001/10001/10101/10101/10101/01010',
  '3':'11110/00001/00001/01110/00001/00001/11110', ' ':'00000/00000/00000/00000/00000/00000/00000'
};

function crc32(data) {
  let c = 0xffffffff;
  for (const b of data) {
    c ^= b;
    for (let i = 0; i < 8; i++) c = (c >>> 1) ^ ((c & 1) ? 0xedb88320 : 0);
  }
  return (c ^ 0xffffffff) >>> 0;
}
function chunk(type, data) {
  const name = Buffer.from(type);
  const out = Buffer.alloc(data.length + 12);
  out.writeUInt32BE(data.length, 0); name.copy(out, 4); data.copy(out, 8);
  out.writeUInt32BE(crc32(Buffer.concat([name, data])), data.length + 8);
  return out;
}
function image(width, height, draw, path) {
  const pixels = Buffer.alloc(width * height * 4);
  const put = (x, y, color) => {
    if (x < 0 || y < 0 || x >= width || y >= height) return;
    const i = (y * width + x) * 4;
    pixels[i] = color[0]; pixels[i + 1] = color[1]; pixels[i + 2] = color[2]; pixels[i + 3] = 255;
  };
  const rect = (x, y, w, h, color) => {
    for (let py = y; py < y + h; py++) for (let px = x; px < x + w; px++) put(px, py, color);
  };
  const text = (value, x, y, scale, color) => {
    for (const char of value) {
      const rows = glyphs[char].split('/');
      rows.forEach((row, ry) => [...row].forEach((bit, rx) => {
        if (bit === '1') rect(x + rx * scale, y + ry * scale, scale, scale, color);
      }));
      x += 6 * scale;
    }
  };
  draw({put, rect, text});
  const raw = Buffer.alloc((width * 4 + 1) * height);
  for (let y = 0; y < height; y++) pixels.copy(raw, y * (width * 4 + 1) + 1, y * width * 4, (y + 1) * width * 4);
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0); ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8; ihdr[9] = 6;
  const png = Buffer.concat([Buffer.from([137,80,78,71,13,10,26,10]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
  fs.writeFileSync(path, png);
}

const navy = [16, 25, 48], orange = [255, 152, 45], cream = [255, 247, 230], teal = [72, 218, 192];
image(48, 48, ({rect, text}) => {
  rect(0, 0, 48, 48, navy);
  rect(3, 3, 42, 42, [28, 43, 72]);
  [8, 14, 20, 26, 20, 14, 8].forEach((h, i) => rect(6 + i * 5, 8 + (14 - h) / 2, 3, h, i === 3 ? teal : orange));
  text('MP3', 6, 31, 2, cream);
}, 'resource/mp3downloader_icon.png');

image(256, 128, ({rect, text}) => {
  for (let y = 0; y < 128; y++) {
    const t = y / 127;
    rect(0, y, 256, 1, [Math.round(13 + 12*t), Math.round(22 + 12*t), Math.round(45 + 24*t)]);
  }
  rect(12, 14, 62, 100, [28, 43, 72]);
  [16, 30, 46, 62, 48, 34, 20].forEach((h, i) => rect(17 + i * 7, 38 + (48 - h) / 2, 4, h, i === 3 ? teal : orange));
  text('MP3', 91, 26, 4, orange);
  text('DOWNLOADER', 91, 79, 2, cream);
  rect(91, 103, 145, 3, teal);
}, 'resource/mp3downloader_banner.png');
