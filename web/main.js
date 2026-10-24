// The page: loads the WADs and the engine, draws its frames, plays its sounds, passes it the keyboard and mouse, and
// connects it to the Smash&Clash network through bridge.js.

import createDoom from './doom.js';
import { createBridge } from './bridge.js';

const canvas = document.getElementById('screen');
const ctx2d = canvas.getContext('2d');
const statusEl = document.getElementById('status');
const progressEl = document.getElementById('progress');
const sideEl = document.getElementById('side');
const image = ctx2d.createImageData(320, 200);
const pixels = new Uint32Array(image.data.buffer);

let engine = null;
let terminalOpen = false;

// ---- sound: DOOM's effects through Web Audio, Smash&Clash's music on top

const audio = new (window.AudioContext || window.webkitAudioContext)();
const buffers = new Map();
const channels = [];
const sound = {
  start(ch, ptr, len, rate, vol, sep) {
    let buf = buffers.get(ptr);
    if (!buf) {
      buf = audio.createBuffer(1, len, rate);
      const out = buf.getChannelData(0);
      const src = engine.HEAPU8.subarray(ptr, ptr + len);
      for (let i = 0; i < len; i++) out[i] = (src[i] - 128) / 128;
      buffers.set(ptr, buf);
    }
    this.stop(ch);
    const node = audio.createBufferSource();
    const gain = audio.createGain();
    const pan = audio.createStereoPanner();
    node.buffer = buf;
    node.connect(gain).connect(pan).connect(audio.destination);
    const c = (channels[ch] = { node, gain, pan, ended: false });
    node.onended = () => (c.ended = true);
    this.params(ch, vol, sep);
    node.start();
    return ch;
  },
  params(ch, vol, sep) {
    const c = channels[ch];
    if (!c) return;
    c.gain.gain.value = (vol / 127) * 0.7;
    c.pan.pan.value = Math.max(-1, Math.min(1, (sep - 128) / 128));
  },
  stop(ch) {
    const c = channels[ch];
    if (c && !c.ended) {
      try {
        c.node.stop();
      } catch {}
    }
    channels[ch] = null;
  },
  playing(ch) {
    return channels[ch] && !channels[ch].ended ? 1 : 0;
  },
};

const music = new Audio();
music.loop = true;
music.volume = 0.35;
let musicOn = localStorage.getItem('sncdoom.music') !== 'off';
let kit = null;
let musicState = 'menu';
function updateMusic() {
  const tracks = kit?.audio?.music?.[musicState];
  if (!musicOn || !terminalOpen || !tracks?.length) return music.pause();
  if (!tracks.includes(music.src)) music.src = tracks[Math.floor(Math.random() * tracks.length)];
  music.play().catch(() => {});
}

// ---- input

const KEYS = {
  ArrowUp: 0xad, ArrowDown: 0xaf, ArrowLeft: 0xac, ArrowRight: 0xae,
  Enter: 13, NumpadEnter: 13, Escape: 27, Tab: 9, Backspace: 0x7f,
  Space: 0xa2, ControlLeft: 0xa3, ControlRight: 0xa3,
  ShiftLeft: 0x80 + 0x36, ShiftRight: 0x80 + 0x36, AltLeft: 0x80 + 0x38, AltRight: 0x80 + 0x38,
  Minus: 0x2d, Equal: 0x3d, NumpadSubtract: 0x2d, NumpadAdd: 0x3d, Comma: 0xa0, Period: 0xa1,
  F1: 0x80 + 0x3b, F2: 0x80 + 0x3c, F3: 0x80 + 0x3d, F4: 0x80 + 0x3e, F5: 0x80 + 0x3f, F6: 0x80 + 0x40,
  F7: 0x80 + 0x41, F8: 0x80 + 0x42, F9: 0x80 + 0x43, F10: 0x80 + 0x44, F11: 0x80 + 0x57, F12: 0x80 + 0x58,
};
function doomKey(e) {
  if (KEYS[e.code]) return KEYS[e.code];
  if (/^Key[A-Z]$/.test(e.code)) return e.code.charCodeAt(3) + 32;
  if (/^Digit[0-9]$/.test(e.code)) return e.code.charCodeAt(5);
  return 0;
}
function onKey(e, pressed) {
  if (!engine || e.target.closest?.('input, textarea, a, button')) return;
  const k = doomKey(e);
  if (!k) return;
  e.preventDefault();
  if (audio.state === 'suspended') audio.resume();
  engine._dg_key(pressed ? 1 : 0, k);
}
addEventListener('keydown', (e) => onKey(e, true));
addEventListener('keyup', (e) => onKey(e, false));

let buttons = 0;
const doomButtons = (b) => (b & 1 ? 1 : 0) | (b & 2 ? 2 : 0) | (b & 4 ? 4 : 0);
function toScreen(e) {
  const r = canvas.getBoundingClientRect();
  return [Math.floor(((e.clientX - r.left) / r.width) * 320), Math.floor(((e.clientY - r.top) / r.height) * 200)];
}
canvas.addEventListener('mousedown', (e) => {
  if (!engine) return;
  if (audio.state === 'suspended') audio.resume();
  if (terminalOpen) {
    const [x, y] = toScreen(e);
    engine._snc_pointer(x, y, e.button === 2 ? 2 : 1);
    return;
  }
  if (document.pointerLockElement !== canvas) return canvas.requestPointerLock?.();
  buttons = doomButtons(e.buttons);
  engine._dg_mouse(0, buttons);
});
addEventListener('mouseup', (e) => {
  if (!engine || terminalOpen || document.pointerLockElement !== canvas) return;
  buttons = doomButtons(e.buttons);
  engine._dg_mouse(0, buttons);
});
addEventListener('mousemove', (e) => {
  if (!engine) return;
  if (terminalOpen) {
    const [x, y] = toScreen(e);
    engine._snc_pointer(x, y, 0);
  } else if (document.pointerLockElement === canvas && e.movementX) {
    engine._dg_mouse(e.movementX, buttons);
  }
});
canvas.addEventListener('contextmenu', (e) => e.preventDefault());
addEventListener('blur', () => {
  if (engine) for (const k of new Set(Object.values(KEYS))) engine._dg_key(0, k);
});

// ---- the side panel: what to share, and where the game is

function setSide(html) {
  sideEl.innerHTML = html;
}
const esc = (t) => String(t).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]);
const ui = {
  kit(k) {
    kit = k;
    updateMusic();
  },
  ready() {
    statusEl.textContent = 'Connected to the Smash&Clash network.';
  },
  terminal(open) {
    terminalOpen = open;
    updateMusic();
  },
  state(s) {
    musicState = s.phase === 'play' || s.phase === 'watch' ? 'battle' : s.phase === 'room' ? 'matchmaking' : s.phase === 'over' ? (s.result?.startsWith('YOU WIN') ? 'victory' : 'defeat') : 'menu';
    updateMusic();
    if (s.phase === 'room')
      setSide(`<h2>Your room</h2><p class="code">${esc(s.code)}</p><p>Send the code or the link. It opens on any Smash&amp;Clash client.</p><p><a href="${esc(s.link)}" target="_blank" rel="noopener">${esc(s.link)}</a></p>`);
    else if (s.phase === 'play' && s.game)
      setSide(`<h2>In a game</h2><p>${esc(s.game.state.players.A)} vs ${esc(s.game.state.players.B || '…')}</p><p><a href="${esc(s.game.watchLink)}" target="_blank" rel="noopener">Watch this game on smashandclash.in</a></p>`);
    else if (s.phase === 'over')
      setSide(`<h2>${esc(s.result || 'Game over')}</h2>${s.replayUrl ? `<p><a href="${esc(s.replayUrl)}" target="_blank" rel="noopener">Replay and Game Review</a></p>` : ''}`);
    else if (s.phase === 'watch') setSide(`<h2>Watching live</h2><p><a href="https://www.smashandclash.in/watch/${esc(s.watchId)}" target="_blank" rel="noopener">Open it on smashandclash.in</a></p>`);
    else setSide('');
  },
};

document.getElementById('music').addEventListener('click', (e) => {
  musicOn = !musicOn;
  localStorage.setItem('sncdoom.music', musicOn ? 'on' : 'off');
  e.target.textContent = musicOn ? 'Music: on' : 'Music: off';
  updateMusic();
});
document.getElementById('music').textContent = musicOn ? 'Music: on' : 'Music: off';

// ---- boot

async function fetchWithProgress(url, label) {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`${url}: ${res.status}`);
  const total = Number(res.headers.get('content-length')) || 0;
  const reader = res.body.getReader();
  const parts = [];
  let got = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    parts.push(value);
    got += value.length;
    if (total) progressEl.style.width = `${Math.round((got / total) * 100)}%`;
    statusEl.textContent = `Loading ${label}: ${(got / 1048576).toFixed(1)} MB`;
  }
  const out = new Uint8Array(got);
  let at = 0;
  for (const p of parts) out.set(p, at), (at += p.length);
  return out;
}

async function boot() {
  const [iwad, pwad] = await Promise.all([fetchWithProgress('freedoom1.wad', 'Freedoom'), fetch('smashandclash.wad').then((r) => r.arrayBuffer())]);
  statusEl.textContent = 'Starting DOOM...';
  let bridge;
  engine = await createDoom({
    print: (t) => console.log(t),
    printErr: (t) => console.warn(t),
    drawFrame(rgb, w, h) {
      // xrgb8888 little-endian (B G R X) to the canvas's RGBA
      const src = new Uint32Array(rgb.buffer, rgb.byteOffset, w * h);
      for (let i = 0; i < w * h; i++) {
        const p = src[i];
        pixels[i] = 0xff000000 | ((p & 0xff) << 16) | (p & 0xff00) | ((p >>> 16) & 0xff);
      }
      ctx2d.putImageData(image, 0, 0);
    },
    sound,
    sncMode: (open) => {
      terminalOpen = window.sncTerminalOpen = !!open;
      if (open && document.pointerLockElement) document.exitPointerLock();
      updateMusic();
    },
    sncCommand: (cmd) => bridge?.command(cmd),
  });
  const recent = (window.sncFrames = []); // the last snapshots sent to the engine, for the console
  const push = (text) => {
    recent.push(text);
    if (recent.length > 40) recent.shift();
    const p = engine.stringToNewUTF8(text);
    engine._snc_push(p);
    engine._free(p);
  };
  bridge = createBridge({ push, ui });
  window.snc = bridge; // for the console
  engine.FS.writeFile('/freedoom1.wad', iwad);
  engine.FS.writeFile('/smashandclash.wad', new Uint8Array(pwad));
  progressEl.parentElement.hidden = true;
  statusEl.textContent = 'DOOM is running. Reaching the Smash&Clash network...';
  try {
    engine.callMain(['-iwad', 'freedoom1.wad', '-file', 'smashandclash.wad']);
  } catch (e) {
    if (e !== 'unwind' && e?.name !== 'ExitStatus') throw e;
  }
  canvas.focus();

  // a link someone shared (?join=CODE, or an invite ?game=…&invite=…) opens straight into the terminal
  const q = new URLSearchParams(location.search);
  if (q.get('join') || (q.get('game') && q.get('invite'))) {
    const link = q.get('join') || location.href;
    const tryOpen = () => (bridge ? (push('OPEN'), bridge.open(link)) : setTimeout(tryOpen, 500));
    setTimeout(tryOpen, 1500);
    history.replaceState(null, '', location.pathname);
  }
}

boot().catch((e) => {
  statusEl.textContent = 'Could not start: ' + e.message;
  console.error(e);
});
