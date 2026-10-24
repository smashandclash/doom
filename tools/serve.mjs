// A static server for web/ while developing:  node tools/serve.mjs [port]
import { createReadStream, statSync } from 'node:fs';
import { createServer } from 'node:http';
import { extname, join, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';

const dir = fileURLToPath(new URL('../web/', import.meta.url));
const port = Number(process.argv[2]) || 8123;
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.wad': 'application/octet-stream', '.json': 'application/json', '.png': 'image/png', '.mp4': 'video/mp4' };

createServer((req, res) => {
  const path = normalize(decodeURIComponent(new URL(req.url, 'http://x').pathname)).replace(/^([/\\])+/, '');
  const file = join(dir, path.endsWith('/') || !path ? join(path, 'index.html') : path);
  if (!file.startsWith(dir)) return res.writeHead(403).end();
  try {
    const st = statSync(file);
    res.writeHead(200, { 'content-type': TYPES[extname(file)] || 'application/octet-stream', 'content-length': st.size, 'cache-control': 'no-cache' });
    createReadStream(file).pipe(res);
  } catch {
    res.writeHead(404).end('not found');
  }
}).listen(port, () => console.log(`http://localhost:${port}/`));
