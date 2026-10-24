// Fetches Freedoom 0.13.0 (BSD-3-Clause), checks it, and puts freedoom1.wad in web/.
//   node tools/freedoom.mjs
import { createHash } from 'node:crypto';
import { existsSync } from 'node:fs';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { inflateRawSync } from 'node:zlib';

const URL_ZIP = 'https://github.com/freedoom/freedoom/releases/download/v0.13.0/freedoom-0.13.0.zip';
const SHA256 = '3f9b264f3e3ce503b4fb7f6bdcb1f419d93c7b546f4df3e874dd878db9688f59';
const cache = new URL('../build/freedoom-0.13.0.zip', import.meta.url);

await mkdir(new URL('../build/', import.meta.url), { recursive: true });
let zip;
if (existsSync(cache)) zip = await readFile(cache);
else {
  console.log('downloading ' + URL_ZIP);
  const res = await fetch(URL_ZIP);
  if (!res.ok) throw new Error(`${URL_ZIP}: ${res.status}`);
  zip = Buffer.from(await res.arrayBuffer());
}
if (createHash('sha256').update(zip).digest('hex') !== SHA256) throw new Error('freedoom zip: checksum mismatch');
await writeFile(cache, zip);

// read the zip's central directory and inflate the entries we want
const end = zip.lastIndexOf(Buffer.from([0x50, 0x4b, 0x05, 0x06]));
const count = zip.readUInt16LE(end + 10);
let at = zip.readUInt32LE(end + 16);
const want = { 'freedoom1.wad': 'freedoom1.wad', 'COPYING.txt': 'freedoom-COPYING.txt' };
for (let i = 0; i < count; i++) {
  const method = zip.readUInt16LE(at + 10), size = zip.readUInt32LE(at + 20);
  const nameLen = zip.readUInt16LE(at + 28), extraLen = zip.readUInt16LE(at + 30), commentLen = zip.readUInt16LE(at + 32);
  const local = zip.readUInt32LE(at + 42);
  const name = zip.toString('utf8', at + 46, at + 46 + nameLen);
  const base = name.split('/').pop();
  if (want[base]) {
    const start = local + 30 + zip.readUInt16LE(local + 26) + zip.readUInt16LE(local + 28);
    const raw = zip.subarray(start, start + size);
    const data = method === 8 ? inflateRawSync(raw) : raw;
    await writeFile(new URL('../web/' + want[base], import.meta.url), data);
    console.log(`web/${want[base]}: ${data.length} bytes`);
  }
  at += 46 + nameLen + extraLen + commentLen;
}
