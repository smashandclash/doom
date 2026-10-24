# Can DOOM run Smash&Clash?

**Play it: [doom.smashandclash.in](https://doom.smashandclash.in)**

[It runs DOOM](https://en.wikipedia.org/wiki/It_runs_Doom) has been a meme for decades: anything with a processor and a
screen ends up running the 1993 shooter. This goes the other way. DOOM is the client: the engine plays
[Smash&Clash](https://www.smashandclash.in) online, against any player on any other Smash&Clash client.

- **DOOM draws the game.** The board, the cards and the menus are drawn into the engine's own 320x200, 256-colour screen.
  The card art is baked into a PWAD in DOOM's palette, the text uses DOOM's HUD font, and the score uses the status-bar
  numbers. Doomguy's face grins when you capture and winces when you're captured. Captures set off rocket explosions,
  hops use the teleport fog, BOULDER is a BFG blast, and the sounds are DOOM's.
- **The computers in the levels run it.** Every computer screen in E1M1 (COMPUTE4, COMPUTE1, COMPSTA1/2, COMP2,
  AQCOMP01) shows the live board through the engine's wall-texture code. You see your own game, or the network's
  featured live game when you aren't playing.
- **It's a real client.** Walk up to a screen and press USE, or press `C`, or pick **SMASH&CLASH** in DOOM's main menu.
  You can play the house, join the quick-match queue, create a room by code, join one, or watch a live game. Rooms and
  the queue are the same ones the website, the apps, the CLI and agents use.

## How it works

DOOM has no network, so the page runs [`@smashandclash/sdk`](https://docs.smashandclash.in/sdk) next to the engine,
the same way the Smash&Clash GBA client carries its calls to a bridge.

```
 DOOM (doomgeneric, C -> WebAssembly)                  the page (JS)
 ┌───────────────────────────────────┐  snc_push()   ┌────────────────────────────┐   HTTPS   Smash&Clash
 │ engine/snc.c  draws, animates,    │ <──────────── │ web/bridge.js              │ <───────> game network
 │ picks moves; wall screens; menu   │ ────────────> │ @smashandclash/sdk 0.6.0   │
 └───────────────────────────────────┘ snc_js_send() └────────────────────────────┘
```

- `bridge.js` reads the seat sync (`game.sync`), the legal moves as data (`game.view.moves`) and the deck
  (`sc.cards()`), and hands the engine snapshots as tab-separated lines. The engine sends back commands: `play <move>`,
  `house`, `quick`, `room`, `join <code>`, `watch`, `resign`, `leave`.
- Moves are played by name from the network's legal moves, and the network resolves every one. The engine only sees what
  a player at the table could: your hand, the board, and counts for the other hand.
- The engine is [doomgeneric](https://github.com/ozkl/doomgeneric) at a pinned commit. `tools/build-wasm.mjs` adds
  five small hooks: the event chain, the display, the main menu, the USE line check and the wall-texture column fetch.

It follows the [client checklist](https://docs.smashandclash.in/clients): it hosts by code, opens codes and
`?join=` / `?game=&invite=` links through `games.open()`, uses the one quick-match queue and leaves it with `leave()`,
and sends its name (`smashandclash-doom`), `as: 'person'` and the player's rating. You are blue and they are orange,
and their cards face them, as at a real table. The opponent always has a name, never "CPU".

## Controls

| Where | Keys |
| - | - |
| DOOM | `W` `A` `S` `D` or arrows, `Ctrl` fires, `Space`/`E` uses, click to aim with the mouse |
| Anywhere | `C` opens the terminal; `T` teleports you to the next Smash&Clash screen on the level |
| Terminal | arrows and `Enter` (or the mouse); `1`-`5` pick a card; `Tab` steps through every legal move by name; `R` resigns; `Esc` goes back to DOOM |

## Build

You need Node 20+, Python 3 with Pillow (WebP support), and Emscripten (`EMSDK` set).

```bash
node tools/freedoom.mjs                 # Freedoom 0.13.0 -> web/freedoom1.wad (checksum-checked)
node tools/fetch-deck.mjs               # the deck and the client kit, through the SDK
python tools/bake.py web/freedoom1.wad build/deck.json web/smashandclash.wad
node tools/build-wasm.mjs               # doomgeneric + engine/*.c -> web/doom.js, web/doom.wasm
node tools/serve.mjs                    # http://localhost:8123/
```

`?timer=1` drives the engine from `setTimeout` instead of `requestAnimationFrame`, for a page that is in the background
(automated tests, a hidden preview).

## Licences

- The engine and this repo's C code: GPL-2.0 (see `LICENSE`), like doomgeneric and the DOOM source.
- `web/sdk/`: `@smashandclash/sdk`, MIT.
- Game data: [Freedoom](https://freedoom.github.io/), BSD-3-Clause. It is downloaded at build time, not committed.
- Smash&Clash, its name, characters, art and audio belong to Smash&Clash. They are used here as its
  [client terms](https://docs.smashandclash.in/build/overview#using-the-art-and-audio) allow, in a client built for
  Smash&Clash, with credit. They are not this project's own.
