// The Smash&Clash side of the DOOM client: runs @smashandclash/sdk and hands the engine what its seat may see.
//
// The engine (engine/snc.c) draws and picks; this file talks to the game network. It sends the engine snapshots as
// tab-separated lines (see snc_push in snc.c) and takes its commands (house, quick, room, join, watch, play, resign,
// leave, again, lobby, set). Every move is played by name from the legal moves the network gives, and the board, the
// hand and the counts come from the seat sync, so the engine never sees more than a player at the table could.

import { SmashAndClash, boardSides, cellToRC } from './sdk/index.js';

const CLIENT = 'smashandclash-doom/0.1.0';
const PH = { lobby: 0, busy: 1, waiting: 2, play: 3, over: 4, watch: 5 };
const PIECE = { KNIGHT: 'N', BISHOP: 'B', ROOK: 'R', QUEEN: 'Q' };
const ELO_K = 32;

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const clean = (t) => String(t ?? '').replace(/→/g, '>').replace(/[\t\n]/g, ' ').replace(/[^\x20-\x7e]/g, '');
const idx = (rc) => rc.r * 5 + rc.c;

function store(key, fallback) {
  try {
    return localStorage.getItem('sncdoom.' + key) ?? fallback;
  } catch {
    return fallback;
  }
}
function save(key, value) {
  try {
    localStorage.setItem('sncdoom.' + key, String(value));
  } catch {}
}

export function createBridge({ push, ui }) {
  const sc = new SmashAndClash({ client: CLIENT });
  const settings = {
    name: store('name', 'DOOMGUY'),
    rating: Number(store('rating', 1200)) || 1200,
    ruleset: store('ruleset', 'mutators'),
    opponent: store('opponent', 'any'),
  };
  let cards = new Map();
  let kit = null;
  let loop = 0; // bumped to stop whatever is running
  let game = null; // the Game we hold a seat in
  let lastMode = 'house';
  let lastFrame = '';
  let playWaiter = null;
  let tvGame = null;

  const deck = Promise.all([sc.cards(), sc.assets()]).then(([c, k]) => {
    cards = new Map(c.map((card) => [card.id, card]));
    kit = k;
    ui.kit?.(k);
  });

  function send(kind, lines) {
    const text = `F\t${kind}\n${lines.join('\n')}`;
    if (kind !== 't') lastFrame = text;
    push(text);
  }
  const toast = (t) => push('L\t' + clean(t).toUpperCase());

  function lobby() {
    loop++;
    game = null;
    ui.state?.({ phase: 'lobby' });
    send('n', [`G\t${PH.lobby}\tA\t0\t0\t0\t0\t0\t0\t${settings.ruleset}\t`, `O\t${clean(settings.name)}\t${settings.rating}\t${settings.ruleset}\t${settings.opponent}`]);
  }

  function busy(status) {
    send('n', [`G\t${PH.busy}\tA\t0\t0\t0\t0\t0\t0\t${settings.ruleset}\t${clean(status).toUpperCase()}`]);
  }

  function waiting(status, code = '', link = '') {
    send('n', [`G\t${PH.waiting}\tA\t0\t0\t0\t0\t0\t0\t${settings.ruleset}\t${clean(status).toUpperCase()}`, `R\t${code}\t${link}`]);
  }

  // ---- one snapshot of the table as lines for the engine

  function tableLines({ phase, seat, state, players, ratings, clients, kinds, score, status, myturn, moves, pendingHop, hands }) {
    const other = seat === 'A' ? 'B' : 'A';
    const lines = [];
    const oppHand = hands ? hands[other] : state.opponentHand;
    lines.push(`G\t${phase}\t${seat}\t${myturn ? 1 : 0}\t${score[seat]}\t${score[other]}\t${oppHand ?? 0}\t${state.deck}\t${state.turn}\t${state.ruleset}\t${clean(status).toUpperCase()}`);
    const oppClient = clients?.[other] || (kinds?.[other] === 'house' ? 'the house' : kinds?.[other] || '');
    lines.push(`P\t${clean(players[seat] || '?')}\t${ratings?.[seat] ?? 0}\t${clean(players[other] || 'Waiting...')}\t${ratings?.[other] ?? 0}\t${clean(oppClient).toUpperCase()}\t${kinds?.[other] || ''}`);
    state.board.forEach((row, r) =>
      row.forEach((t, c) => {
        if (!t) return;
        const card = cards.get(t.cardId);
        if (!card) return;
        const s = boardSides(card, t.owner);
        lines.push(`C\t${r * 5 + c}\t${t.cardId}\t${t.owner === seat ? 1 : 0}\t${t.frozen ? 1 : 0}\t${s.north}\t${s.east}\t${s.south}\t${s.west}\t${card.colorHex || '#888888'}\t${clean(card.name).toUpperCase()}`);
      }),
    );
    for (const t of state.operatorTiles || []) lines.push(`X\t${idx(t.cell)}\t${PIECE[t.piece] || t.piece[0]}`);
    for (const t of state.powerTiles || []) lines.push(`W\t${idx(t.cell)}\t${kit?.colors?.[t.color] || '#888888'}\t${t.boost}`);
    for (const id of state.hand || []) {
      const card = cards.get(id);
      if (!card) continue;
      const fx = card.kind === 'effect';
      lines.push(`H\t${id}\t${fx ? 1 : 0}\t${card.top ?? 0}\t${card.right ?? 0}\t${card.bottom ?? 0}\t${card.left ?? 0}\t${card.colorHex || '#888888'}\t${clean(card.name).toUpperCase()}\t${clean(card.does || '').toUpperCase()}`);
    }
    for (const m of moves || []) {
      const cell = (n) => (n ? idx(cellToRC(n)) : '');
      lines.push(`M\t${clean(m.name)}\t${m.type[0]}\t${m.cardId ?? 0}\t${cell(m.cell)}\t${cell(m.target)}\t${cell(m.dest)}`);
    }
    if (pendingHop && myturn) lines.push(`K\t${idx(pendingHop.cell)}`);
    return lines;
  }

  function eventLines(events, seat) {
    const out = [];
    const cells = (list) => list.map(idx).join(',');
    for (const e of events) {
      switch (e.type) {
        case 'cardPlaced': out.push(`V\tp\t${idx(e.cell)}\t${e.player === seat ? 1 : 0}`); break;
        case 'cardsCaptured': out.push(`V\tc\t${cells(e.cells)}\t${e.newOwner === seat ? 1 : 0}`); break;
        case 'cardHopped': out.push(`V\th\t${idx(e.to)}\t0`); break;
        case 'boulderRemoved': out.push(`V\tx\t${cells(e.cells)}\t0`); break;
        case 'boardFlipped': out.push('V\tf\t\t0'); break;
        case 'handsSwapped': out.push('V\ts\t\t0'); break;
        case 'cardFrozen': out.push(`V\tz\t${idx(e.cell)}\t0`); break;
        case 'cardRecruited': out.push(`V\tr\t${idx(e.from)},${idx(e.to)}\t${e.newOwner === seat ? 1 : 0}`); break;
        case 'cardOverrun': out.push(`V\to\t${idx(e.cell)}\t0`); break;
        case 'gameOver': out.push(`V\te\t\t${e.winner === seat ? 1 : e.winner === 'draw' || !e.winner ? 0 : -1}`); break;
      }
    }
    return out;
  }

  // ---- playing a seat

  async function play(g, mode) {
    const id = ++loop;
    game = g;
    lastMode = mode;
    const seat = g.state.seat || 'A';
    let since = 0;
    ui.state?.({ phase: 'play', game: g });
    for (;;) {
      let s;
      try {
        const mine = g.state.status === 'active' && g.yourTurn;
        s = await g.sync({ since, ...(mine || g.state.status !== 'active' ? {} : { wait: 20 }) });
      } catch (e) {
        if (id !== loop) return;
        toast(e.message || 'The network dropped: retrying');
        await sleep(2000);
        continue;
      }
      if (id !== loop) return;
      if (s.status !== g.state.status || (s.status === 'active' && s.state.current === seat)) await g.refresh().catch(() => {});
      if (id !== loop) return;
      const over = s.status === 'finished' || s.status === 'abandoned';
      const myturn = !over && s.status === 'active' && g.yourTurn;
      const base = { seat, players: s.players, ratings: s.ratings, clients: s.clients, kinds: s.playerKinds };
      const status = over ? 'game over' : myturn ? 'your turn' : `${s.players[seat === 'A' ? 'B' : 'A'] || 'opponent'} to play`;
      const frames = s.moves.length ? s.moves : since === 0 ? [{ events: [], state: s.state }] : [];
      frames.forEach((m, i) => {
        const last = i === frames.length - 1;
        const score = last ? s.score : scoreOf(m.state);
        const lines = tableLines({
          ...base,
          phase: PH.play,
          state: m.state,
          score,
          status,
          myturn: last && myturn,
          moves: last && myturn ? g.view?.moves : null,
          pendingHop: last ? g.view?.pendingHop && { cell: cellToRC(g.view.pendingHop.from) } : null,
        });
        send(since === 0 && i === 0 ? 'n' : 'q', [...lines, ...eventLines(m.events, seat)]);
      });
      since = s.moveCount;
      if (over) return finish(g, s, id);
      if (myturn) {
        await new Promise((resolve) => (playWaiter = resolve));
        playWaiter = null;
        if (id !== loop) return;
      }
    }
  }

  function scoreOf(state) {
    const score = { A: 0, B: 0 };
    for (const row of state.board) for (const t of row) if (t) score[t.owner]++;
    return score;
  }

  async function finish(g, s, id) {
    const seat = s.seat;
    const other = seat === 'A' ? 'B' : 'A';
    const winner = s.winner === seat ? 1 : s.winner === other ? -1 : 0;
    let delta = 0;
    const ra = s.ratings?.[seat], rb = s.ratings?.[other];
    if (s.status === 'finished' && ra && rb) {
      const expected = 1 / (1 + 10 ** ((rb - ra) / 400));
      delta = Math.round(ELO_K * ((winner > 0 ? 1 : winner < 0 ? 0 : 0.5) - expected));
      settings.rating = Math.max(100, settings.rating + delta);
      save('rating', settings.rating);
    }
    const res1 = `${winner > 0 ? 'YOU WIN' : winner < 0 ? 'YOU LOSE' : 'DRAW'} ${s.score[seat]}-${s.score[other]}`;
    let res2 = s.status === 'abandoned' ? 'THE GAME WAS ABANDONED.' : `VS ${clean(s.players[other]).toUpperCase()}.`;
    const draw = (extra) => {
      const lines = tableLines({ seat, players: s.players, ratings: s.ratings, clients: s.clients, kinds: s.playerKinds, phase: PH.over, state: s.state, score: s.score, status: 'game over', myturn: false });
      send('q', [...lines, `E\t${winner}\t${delta}\t${res1}\t${clean(res2 + extra).toUpperCase()}`]);
    };
    ui.state?.({ phase: 'over', game: g, replayUrl: s.replayUrl || g.replayUrl, result: res1 });
    try {
      const review = await g.review();
      if (id !== loop) return;
      const acc = review.accuracy || {};
      const pct = (v) => (v == null ? '?' : Math.round(v) + '%');
      draw(` GAME REVIEW ACCURACY: YOU ${pct(acc[seat])}, THEM ${pct(acc[other])}.`);
    } catch {
      if (id === loop) draw('');
    }
  }

  // ---- getting a seat

  const seatOptions = () => ({ name: settings.name, as: 'person', rating: settings.rating });

  async function untilSeated(g, id, status, code, link) {
    waiting(status, code, link);
    while (g.waiting && id === loop) {
      try {
        await g.waitForTurn(20);
      } catch (e) {
        if (e.status === 408) continue;
        throw e;
      }
    }
    return id === loop;
  }

  async function start(mode, arg) {
    const id = ++loop;
    lastMode = mode;
    try {
      await deck;
      if (mode === 'house') {
        busy('Setting up a game against the house');
        const g = await sc.games.startHouse({ name: settings.name, as: 'person', strength: Math.min(1600, Math.max(800, settings.rating)), ruleset: settings.ruleset });
        if (id === loop) return play(g, mode);
      } else if (mode === 'quick') {
        busy('Joining the queue');
        const g = await sc.games.quickMatch({ ...seatOptions(), ruleset: settings.ruleset, opponent: settings.opponent });
        game = g;
        const who = { any: 'anyone', person: 'a person', agent: 'an agent' }[settings.opponent];
        if (await untilSeated(g, id, `Waiting in the queue for ${who}`)) return play(g, mode);
      } else if (mode === 'room') {
        busy('Making a room');
        const g = await sc.games.createDuel({ ...seatOptions(), ruleset: settings.ruleset });
        game = g;
        ui.state?.({ phase: 'room', code: g.code, link: g.joinLink });
        if (await untilSeated(g, id, 'Waiting for a player', g.code, g.joinLink)) return play(g, mode);
      } else if (mode === 'join' || mode === 'open') {
        busy(`Opening ${arg}`);
        const opened = await sc.games.open(arg, seatOptions());
        if (id !== loop) return;
        if (opened.kind === 'game') {
          game = opened.game;
          if (opened.game.waiting && !(await untilSeated(opened.game, id, 'Waiting for the host'))) return;
          return play(opened.game, 'join');
        }
        if (opened.kind === 'watch') return watch(opened.game.id);
        toast(`That link is a ${opened.kind}: open it on smashandclash.in`);
        return lobby();
      } else if (mode === 'watch') {
        busy('Finding a live game');
        const featured = await sc.games.featured();
        const live = featured.game || (await sc.games.live({ limit: 1 }))[0];
        if (id !== loop) return;
        if (!live) {
          toast('No live games right now: try again soon');
          return lobby();
        }
        return watch(live.id);
      }
    } catch (e) {
      if (id !== loop) return;
      toast(e.message || String(e));
      lobby();
    }
  }

  async function watch(gameId) {
    const id = ++loop;
    lastMode = 'watch';
    let since = 0;
    let status;
    ui.state?.({ phase: 'watch', watchId: gameId });
    for (;;) {
      let s;
      try {
        s = await sc.games.watchSync(gameId, since ? { since, status, wait: 20 } : { since });
        status = s.status;
      } catch (e) {
        if (id !== loop) return;
        toast(e.message);
        await sleep(3000);
        continue;
      }
      if (id !== loop) return;
      const lines = (st, score, events) => [
        ...tableLines({ seat: 'A', players: s.players, ratings: s.ratings, clients: s.clients, kinds: s.playerKinds, phase: PH.watch, state: st, hands: st.hands, score, status: `${s.players.A} (${s.clients?.A || s.playerKinds.A}) vs ${s.players.B} (${s.clients?.B || s.playerKinds.B})`, myturn: false }),
        ...eventLines(events, 'A'),
      ];
      if (!s.moves.length && since === 0) send('n', lines(s.state, s.score, []));
      s.moves.forEach((m, i) => send(since === 0 && i === 0 ? 'n' : 'q', lines(m.state, scoreOf(m.state), m.events)));
      since = s.moveCount;
      if (s.status === 'finished' || s.status === 'abandoned') {
        toast(`Final: ${s.players.A} ${s.score.A} - ${s.score.B} ${s.players.B}`);
        return;
      }
    }
  }

  // ---- the wall screens: the network's featured live game while you are not playing

  async function tvLoop() {
    for (;;) {
      if (game) {
        await sleep(10000);
        continue;
      }
      try {
        await deck;
        const f = await sc.games.featured();
        if (!f.game) {
          await sleep(30000);
          continue;
        }
        tvGame = f.game.id;
        let since = 0;
        let status;
        for (let n = 0; n < 30 && !game; n++) {
          const began = Date.now();
          const s = await sc.games.watchSync(tvGame, since ? { since, status, wait: 20 } : { since });
          since = s.moveCount;
          status = s.status;
          send('t', tableLines({ seat: 'A', players: s.players, ratings: s.ratings, clients: s.clients, kinds: s.playerKinds, phase: PH.watch, state: s.state, hands: s.state.hands, score: s.score, status: '', myturn: false }));
          if (s.status !== 'active') break;
          if (Date.now() - began < 2000) await sleep(2000);
        }
      } catch {
        await sleep(15000);
      }
      await sleep(5000);
    }
  }

  // ---- commands from the engine

  async function command(text) {
    const [cmd, ...args] = text.split('\t');
    switch (cmd) {
      case 'ready':
        lobby();
        tvLoop();
        ui.ready?.();
        break;
      case 'open':
        ui.terminal?.(true);
        if (!lastFrame) lobby();
        break;
      case 'house': case 'quick': case 'room': case 'watch':
        start(cmd);
        break;
      case 'join':
        start('join', args[0]);
        break;
      case 'again':
        start(lastMode === 'join' || lastMode === 'open' ? 'room' : lastMode);
        break;
      case 'lobby':
        lobby();
        break;
      case 'leave':
        if (game?.waiting) {
          try {
            await game.leave();
            lobby();
          } catch (e) {
            if (e.status === 409) toast('A player just arrived: the game is on');
            else lobby();
          }
        } else lobby();
        break;
      case 'resign':
        if (game && !game.over) {
          try {
            await game.resign();
          } catch (e) {
            toast(e.message);
          }
          playWaiter?.();
        }
        break;
      case 'play':
        if (!game) return;
        try {
          await game.play(args[0]);
        } catch (e) {
          toast(e.status === 422 ? 'That move is not legal' : e.message);
          await game.refresh().catch(() => {});
          if (lastFrame) push(lastFrame.replace(/^F\tq/, 'F\tn'));
        }
        playWaiter?.();
        break;
      case 'set':
        if (args[0] === 'name') settings.name = args[1].slice(0, 16) || 'DOOMGUY';
        if (args[0] === 'ruleset') settings.ruleset = args[1] === 'classic' ? 'classic' : 'mutators';
        if (args[0] === 'opponent') settings.opponent = ['any', 'person', 'agent'].includes(args[1]) ? args[1] : 'any';
        save(args[0], settings[args[0]]);
        break;
    }
  }

  return { command, open: (link) => start('open', link), settings, sdk: sc };
}
