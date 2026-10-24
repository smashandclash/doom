/* snc.c: a Smash&Clash client that runs inside DOOM.
 *
 * DOOM draws the game itself: the board, the cards and the menus go into the engine's 320x200 8-bit screen with its
 * own palette, status-bar face, HUD font, sprites and sound effects, and the computer screens in the levels show the
 * board live through the engine's wall-texture code. The card art comes from smashandclash.wad, baked from the deck
 * by tools/bake.py.
 *
 * DOOM has no network, so the page runs the Smash&Clash SDK (@smashandclash/sdk) beside the engine (web/bridge.js).
 * The two talk through two calls: snc_push() hands the engine a snapshot of what its seat may see (the seat sync, the
 * legal moves as data), as tab-separated lines, and snc_js_send() hands the page a command (play a move, join a room).
 * The network decides every move; this file only draws, animates and picks.
 *
 * GPL-2.0, like the engine it is linked into.
 */
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <emscripten.h>

#include "doomdef.h"
#include "doomkeys.h"
#include "doomstat.h"
#include "d_player.h"
#include "i_swap.h"
#include "i_video.h"
#include "m_fixed.h"
#include "p_local.h"
#include "r_main.h"
#include "p_mobj.h"
#include "r_data.h"
#include "r_defs.h"
#include "r_state.h"
#include "s_sound.h"
#include "sounds.h"
#include "v_patch.h"
#include "w_wad.h"
#include "z_zone.h"

#include "hu_stuff.h"
#include "snc.h"

#define FIXED2DOUBLE(x) ((x) / 65536.0)
#define DOUBLE2FIXED(x) ((fixed_t)((x) * 65536.0))

static void teleport_to_screen(void);

extern patch_t *hu_font[HU_FONTSIZE];
extern boolean inhelpscreens;
extern int *texturewidthmask;
extern fixed_t *textureheight;

EM_JS(void, snc_js_send, (const char *cmd), {
  if (Module.sncCommand) Module.sncCommand(UTF8ToString(cmd));
});

EM_JS(void, snc_js_mode, (int open), {
  if (Module.sncMode) Module.sncMode(open);
});

static void send(const char *fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  snc_js_send(buf);
}

/* ---------------------------------------------------------------- state */

enum { PH_LOBBY, PH_BUSY, PH_WAITING, PH_PLAY, PH_OVER, PH_WATCH };

typedef struct {
  int id, effect, v[4]; /* top right bottom left, as printed */
  int frame;            /* palette index of the frame colour */
  char name[24], does[100];
} hcard_t;

typedef struct {
  int id, mine, frozen, v[4]; /* north east south west: board terms */
  int frame;
  char name[24];
} bcell_t;

typedef struct {
  char name[40];
  char type; /* p place, o overrun, h hop, e effect */
  int card, cell, target, dest;
} move_t;

#define MAXMOVES 200

typedef struct {
  int valid, phase;
  char seat; /* 'A' or 'B': which way round the board is drawn */
  int myturn, score_me, score_opp, opp_hand, deck, turn, classic;
  char status[80];
  char me[32], opp[32], opp_client[40], opp_kind[12];
  int me_rating, opp_rating;
  bcell_t board[15];
  char piece[15];
  int power_frame[15], power_boost[15];
  hcard_t hand[5];
  int nhand;
  move_t moves[MAXMOVES];
  int nmoves;
  int hop_from;
  /* lobby options */
  char opt_name[32], opt_opp[12];
  int opt_rating, opt_classic;
  /* a room or the queue */
  char code[12], link[100];
  /* the result */
  int winner, delta;
  char res1[80], res2[80];
} snc_state_t;

enum { EV_PLACE, EV_CAPTURE, EV_BLOCKED, EV_HOP, EV_BOULDER, EV_FLIP, EV_SWAP, EV_FREEZE, EV_RECRUIT, EV_OVERRUN, EV_OVER };

typedef struct {
  int kind, mine, n, cells[15];
} event_t2;

#define MAXEV 16

typedef struct {
  snc_state_t st;
  event_t2 ev[MAXEV];
  int nev;
} frame_t;

#define QLEN 24
static frame_t queue[QLEN];
static int qhead, qcount;
static snc_state_t cur, tv;
static int have_cur;

/* ---------------------------------------------------------------- palette, colours, remaps */

static byte *playpal;
static int inited;

static int nearest(int r, int g, int b) {
  int best = 0, bd = 1 << 30;
  for (int i = 0; i < 256; i++) {
    int pr = playpal[i * 3], pg = playpal[i * 3 + 1], pb = playpal[i * 3 + 2];
    int rm = (r + pr) / 2;
    int d = (512 + rm) * (r - pr) * (r - pr) / 256 + 4 * (g - pg) * (g - pg) + (767 - rm) * (b - pb) * (b - pb) / 256;
    if (d < bd) bd = d, best = i;
  }
  return best;
}

static int hexcolor(const char *hex) {
  unsigned v = (unsigned)strtoul(hex[0] == '#' ? hex + 1 : hex, NULL, 16);
  return nearest((v >> 16) & 255, (v >> 8) & 255, v & 255);
}

static int C_BLACK, C_WHITE, C_INK, C_YOU, C_YOU_DK, C_YOU_HI, C_THEM, C_THEM_DK, C_THEM_HI, C_PANEL, C_PANEL_HI,
  C_PANEL_LO, C_GOLD, C_LEGAL, C_ICE, C_BADGE[4], C_GRAPE, C_DIM;

/* Remap tables: DOOM's red HUD font (and any graphic) recoloured by brightness. */
static byte R_WHITE[256], R_GOLD[256], R_YOU[256], R_THEM[256], R_GRAY[256], R_GREEN[256], R_ICE[256], R_DARK[256];

static void make_remap(byte *t, int r, int g, int b) {
  for (int i = 0; i < 256; i++) {
    int pr = playpal[i * 3], pg = playpal[i * 3 + 1], pb = playpal[i * 3 + 2];
    int v = pr > pg ? (pr > pb ? pr : pb) : (pg > pb ? pg : pb);
    t[i] = (byte)nearest(r * v / 255, g * v / 255, b * v / 255);
  }
}

static void make_tint(byte *t, int r, int g, int b, int keep) {
  /* keep/256 of the original colour, the rest the tint scaled by the original's brightness */
  for (int i = 0; i < 256; i++) {
    int pr = playpal[i * 3], pg = playpal[i * 3 + 1], pb = playpal[i * 3 + 2];
    int l = (pr * 30 + pg * 59 + pb * 11) / 100;
    t[i] = (byte)nearest((pr * keep + r * l / 255 * (256 - keep)) / 256, (pg * keep + g * l / 255 * (256 - keep)) / 256,
                         (pb * keep + b * l / 255 * (256 - keep)) / 256);
  }
}

/* ---------------------------------------------------------------- drawing surfaces */

typedef struct {
  byte *p;
  int w, h, pitch, colmajor;
} surf_t;

static surf_t screen;

static inline void put(surf_t *s, int x, int y, int c) {
  if ((unsigned)x < (unsigned)s->w && (unsigned)y < (unsigned)s->h) s->p[s->colmajor ? x * s->pitch + y : y * s->pitch + x] = (byte)c;
}

static inline int get(surf_t *s, int x, int y) {
  return s->p[s->colmajor ? x * s->pitch + y : y * s->pitch + x];
}

static void fill(surf_t *s, int x, int y, int w, int h, int c) {
  for (int j = y; j < y + h; j++)
    for (int i = x; i < x + w; i++) put(s, i, j, c);
}

static void frame_rect(surf_t *s, int x, int y, int w, int h, int c) {
  fill(s, x, y, w, 1, c);
  fill(s, x, y + h - 1, w, 1, c);
  fill(s, x, y, 1, h, c);
  fill(s, x + w - 1, y, 1, h, c);
}

static void remap_rect(surf_t *s, int x, int y, int w, int h, const byte *t) {
  for (int j = y; j < y + h; j++)
    for (int i = x; i < x + w; i++)
      if ((unsigned)i < (unsigned)s->w && (unsigned)j < (unsigned)s->h) put(s, i, j, t[get(s, i, j)]);
}

static patch_t *patch(const char *name) {
  int n = W_CheckNumForName((char *)name);
  return n < 0 ? NULL : W_CacheLumpNum(n, PU_CACHE);
}

/* A patch at (x, y), ignoring its offsets, clipped, optionally remapped, scaled up and turned 180 degrees. */
static void draw_patch_x(surf_t *s, int x, int y, patch_t *p, const byte *remap, int scale, int rot) {
  if (!p) return;
  int w = SHORT(p->width), h = SHORT(p->height);
  for (int cx = 0; cx < w; cx++) {
    column_t *col = (column_t *)((byte *)p + LONG(p->columnofs[cx]));
    while (col->topdelta != 0xff) {
      byte *src = (byte *)col + 3;
      for (int i = 0; i < col->length; i++) {
        int c = remap ? remap[src[i]] : src[i];
        int px = rot ? w - 1 - cx : cx, py = rot ? h - 1 - (col->topdelta + i) : col->topdelta + i;
        for (int a = 0; a < scale; a++)
          for (int b = 0; b < scale; b++) put(s, x + px * scale + a, y + py * scale + b, c);
      }
      col = (column_t *)((byte *)col + col->length + 4);
    }
  }
}

/* A patch at (x, y), ignoring its offsets, clipped, optionally remapped and scaled up. */
static void draw_patch_s(surf_t *s, int x, int y, patch_t *p, const byte *remap, int scale) {
  draw_patch_x(s, x, y, p, remap, scale, 0);
}

static void draw_patch(surf_t *s, int x, int y, patch_t *p, const byte *remap) {
  draw_patch_s(s, x, y, p, remap, 1);
}

static void draw_patch_centered(surf_t *s, int cx, int cy, patch_t *p, const byte *remap) {
  if (p) draw_patch(s, cx - SHORT(p->width) / 2, cy - SHORT(p->height) / 2, p, remap);
}

/* ---------------------------------------------------------------- text: DOOM's HUD font, and a 3x5 font for small print */

static int hu_width(const char *t) {
  int w = 0;
  for (; *t; t++) {
    int c = toupper((unsigned char)*t) - HU_FONTSTART;
    w += (c < 0 || c >= HU_FONTSIZE || !hu_font[c]) ? 4 : SHORT(hu_font[c]->width);
  }
  return w;
}

static int hu_text(surf_t *s, int x, int y, const char *t, const byte *remap, int scale) {
  for (; *t; t++) {
    int c = toupper((unsigned char)*t) - HU_FONTSTART;
    if (c < 0 || c >= HU_FONTSIZE || !hu_font[c]) {
      x += 4 * scale;
      continue;
    }
    draw_patch_s(s, x, y, hu_font[c], remap, scale);
    x += SHORT(hu_font[c]->width) * scale;
  }
  return x;
}

/* the widest prefix of t that fits in w pixels, with "." if cut */
static void hu_fit(char *out, size_t n, const char *t, int w) {
  snprintf(out, n, "%s", t);
  while (out[0] && hu_width(out) > w) {
    size_t l = strlen(out);
    out[l - 1] = 0;
    if (l >= 3) out[l - 2] = '.';
  }
}

static const char *const SMALL_CHARS = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ+-:./><!?&@'()%#_=,*|[]";
static const char *const SMALL_BITS[] = {
  "111101101101111", "010110010010111", "111001111100111", "111001111001111", "101101111001001", "111100111001111",
  "111100111101111", "111001010010010", "111101111101111", "111101111001111", "010101111101101", "110101110101110",
  "011100100100011", "110101101101110", "111100110100111", "111100110100100", "011100101101011", "101101111101101",
  "111010010010111", "001001001101010", "101101110101101", "100100100100111", "101111111101101", "110101101101101",
  "010101101101010", "110101110100100", "010101101110011", "110101110101101", "011100010001110", "111010010010010",
  "101101101101111", "101101101101010", "101101111111101", "101101010101101", "101101010010010", "111001010100111",
  "000010111010000", "000000111000000", "000010000010000", "000000000000010", "001001010100100", "100010001010100",
  "001010100010001", "010010010000010", "111001010000010", "010101010101011", "010101111100011", "010010000000000",
  "001010010010001", "100010010010100", "101001010100101", "101111101111101", "000000000000111", "000111000111000",
  "000000000010100", "000101010101000", "010010010010010", "011010010010011", "110010010010110",
};

static int small_text(surf_t *s, int x, int y, const char *t, int c) {
  for (; *t; t++) {
    const char *at = strchr(SMALL_CHARS, toupper((unsigned char)*t));
    if (*t != ' ' && at) {
      const char *bits = SMALL_BITS[at - SMALL_CHARS];
      for (int i = 0; i < 15; i++)
        if (bits[i] == '1') put(s, x + i % 3, y + i / 3, c);
    }
    x += 4;
  }
  return x;
}

static int small_width(const char *t) {
  return (int)strlen(t) * 4 - 1;
}

/* the small-print text with a one-pixel dark outline, for drawing over art */
static void small_text_outlined(surf_t *s, int x, int y, const char *t, int c, int o) {
  for (int dx = -1; dx <= 1; dx++)
    for (int dy = -1; dy <= 1; dy++)
      if (dx || dy) small_text(s, x + dx, y + dy, t, o);
  small_text(s, x, y, t, c);
}

/* word-wrapped small print; returns the y below it */
static int small_wrap(surf_t *s, int x, int y, int w, const char *t, int c, int maxlines) {
  int per = (w + 1) / 4, lines = 0;
  char line[64];
  while (*t && lines < maxlines) {
    while (*t == ' ') t++;
    int n = (int)strlen(t);
    if (n > per) {
      n = per;
      while (n > 0 && t[n] != ' ') n--;
      if (n == 0) n = per;
    }
    snprintf(line, sizeof line, "%.*s", n, t);
    small_text(s, x, y, line, c);
    t += n;
    y += 7;
    lines++;
  }
  return y;
}

/* ---------------------------------------------------------------- cards */

/* A card drawn from its parts: the frame in the card's colour, the art, four value badges (red top, yellow right,
 * blue bottom, green left: the kit's badge colours). v[] is the values as they face the viewer: up right down left. */
static void draw_badge(surf_t *s, int x, int y, int side, int value, int big, int rot) {
  int w = big ? 9 : 7, h = big ? 11 : 9, k = big ? 2 : 1;
  fill(s, x, y, w, h, C_BADGE[side]);
  frame_rect(s, x, y, w, h, C_INK);
  /* the 3x5 digit (twice as large on the preview); it stays upright on a card that faces the other way, since an
   * upside-down 6 reads as a 9 at this size */
  rot = 0;
  const char *at = strchr(SMALL_CHARS, '0' + (value % 10));
  const char *bits = SMALL_BITS[at ? at - SMALL_CHARS : 0];
  int ox = x + (w - 3 * k) / 2, oy = y + (h - 5 * k) / 2;
  for (int i = 0; i < 15; i++)
    if (bits[rot ? 14 - i : i] == '1') fill(s, ox + (i % 3) * k, oy + (i / 3) * k, k, k, C_INK);
}

static void draw_card(surf_t *s, int x, int y, int w, int h, int id, int effect, const int v[4], int frame, int size,
                      int owner, int frozen) {
  /* size: 0 board (art 24), 1 hand (art 28), 2 preview (art 56); owner: 0 none, 1 you, 2 them. Their cards face them,
   * as at a real table: the art, the badges and the digits are turned round. */
  int rot = owner == 2;
  char name[12];
  if (effect) {
    snprintf(name, sizeof name, size == 2 ? "SCEL%d" : size == 1 ? "SCEM%d" : "SCES%d", id);
    patch_t *p = patch(name);
    if (p) draw_patch(s, x + (w - SHORT(p->width)) / 2, y + (h - SHORT(p->height)) / 2, p, NULL);
    return;
  }
  int bord = owner == 1 ? C_YOU : owner == 2 ? C_THEM : C_INK;
  fill(s, x, y, w, h, frame);
  frame_rect(s, x, y, w, h, bord);
  if (owner) frame_rect(s, x + 1, y + 1, w - 2, h - 2, bord);
  int iw = w - (size == 2 ? 6 : 4), ih = h - (size == 2 ? 12 : 10);
  fill(s, x + (w - iw) / 2, y + (h - ih) / 2, iw, ih, C_PANEL_LO);
  snprintf(name, sizeof name, size == 2 ? "SCL%02d" : size == 1 ? "SCM%02d" : "SCS%02d", id);
  patch_t *art = patch(name);
  if (art) draw_patch_x(s, x + (w - SHORT(art->width)) / 2, y + (h - SHORT(art->height)) / 2, art, frozen ? R_ICE : NULL, 1, rot);
  if (frozen) frame_rect(s, x + 2, y + 2, w - 4, h - 4, C_ICE);
  int big = size == 2, bw = big ? 9 : 7, bh = big ? 11 : 9;
  /* v[] faces the viewer; the badge colours stay with the printed sides, so they turn with the card */
  draw_badge(s, x + (w - bw) / 2, y, rot ? 2 : 0, v[0], big, rot);
  draw_badge(s, x + w - bw, y + (h - bh) / 2, rot ? 3 : 1, v[1], big, rot);
  draw_badge(s, x + (w - bw) / 2, y + h - bh, rot ? 0 : 2, v[2], big, rot);
  draw_badge(s, x, y + (h - bh) / 2, rot ? 1 : 3, v[3], big, rot);
}

/* ---------------------------------------------------------------- board geometry */

#define BX 4
#define BY 18
#define CW 40
#define CH 44
#define HX 4
#define HY 153
#define RX 210

/* board index (r*5+c, r 0..2 = rows 1..3) to its place on screen, turned round for seat B */
static void cell_to_screen(const snc_state_t *st, int cell, int *sr, int *sc) {
  int r = cell / 5, c = cell % 5;
  if (st->seat == 'B') *sr = r, *sc = 4 - c;
  else *sr = 2 - r, *sc = c;
}

static int screen_to_cell(const snc_state_t *st, int sr, int sc) {
  return st->seat == 'B' ? sr * 5 + (4 - sc) : (2 - sr) * 5 + sc;
}

/* a board card's values as they face the viewer: up right down left */
static void facing(const snc_state_t *st, const bcell_t *b, int v[4]) {
  if (st->seat == 'B') v[0] = b->v[2], v[1] = b->v[3], v[2] = b->v[0], v[3] = b->v[1];
  else memcpy(v, b->v, sizeof b->v);
}

static void cell_name(int cell, char *out) {
  out[0] = (char)('A' + cell % 5);
  out[1] = (char)('1' + cell / 5);
  out[2] = 0;
}

/* ---------------------------------------------------------------- animations: DOOM's own sprites over the board */

typedef struct {
  const char *base; /* sprite name without the frame letter, e.g. "MISL" + 'B'.. */
  char first, last;
  int tics_per;
} anim_def_t;

static const anim_def_t A_EXPLODE = {"MISL", 'B', 'D', 5}, A_PUFF = {"PUFF", 'A', 'D', 4}, A_TFOG = {"TFOG", 'A', 'J', 3},
                        A_BFG = {"BFE1", 'A', 'F', 4}, A_IFOG = {"IFOG", 'A', 'E', 4}, A_PLASMA = {"PLSE", 'A', 'E', 4};

typedef struct {
  const anim_def_t *def;
  int cell, start;
} anim_t;

static anim_t anims[48];
static int nanims;
static int tic;
static int flash_cell = -1, flash_until;
static int shake_until;

static void add_anim(const anim_def_t *def, int cell, int delay) {
  if (nanims < (int)(sizeof anims / sizeof anims[0])) anims[nanims++] = (anim_t){def, cell, tic + delay};
}

static void draw_anims(surf_t *s, const snc_state_t *st) {
  int keep = 0;
  for (int i = 0; i < nanims; i++) {
    anim_t *a = &anims[i];
    int t = tic - a->start, frames = a->def->last - a->def->first + 1;
    if (t >= frames * a->def->tics_per) continue;
    anims[keep++] = *a;
    if (t < 0) continue;
    char name[9];
    snprintf(name, sizeof name, "%s%c0", a->def->base, a->def->first + t / a->def->tics_per);
    int sr, sc;
    cell_to_screen(st, a->cell, &sr, &sc);
    draw_patch_centered(s, BX + sc * CW + CW / 2, BY + sr * CH + CH / 2, patch(name), NULL);
  }
  nanims = keep;
}

/* ---------------------------------------------------------------- the status-bar face */

static int face_until;
static const char *face_name = "STFST01";

static void set_face(const char *name, int tics) {
  face_name = name;
  face_until = tic + tics;
}

static const char *current_face(void) {
  if (tic < face_until) return face_name;
  if (have_cur && cur.phase == PH_OVER) return cur.winner > 0 ? "STFGOD0" : cur.winner < 0 ? "STFDEAD0" : "STFST01";
  static const char *idle[] = {"STFST01", "STFST00", "STFST01", "STFST02"};
  return idle[(tic / 70) % 4];
}

/* ---------------------------------------------------------------- terminal state and input */

static int active, was_paused;
static int hsel, focus_board, bsr = 1, bsc = 2; /* hand selection; board cursor (screen row/col) */
static int stage;     /* 0 pick a card, 1 pick a tile, 2 pick a destination */
static int pick_card = -1, pick_target = -1;
static int armed;     /* an untargeted effect waiting for a second ENTER */
static int console_i = -1;
static int sent_at = -100000; /* tic a move was sent: input waits for the answer */
static int confirm_resign;
static int lobby_sel;
static int editing; /* 1 name, 2 room code */
static char edit_buf[32];
static char toast[96];
static int toast_until;
static int mouse_x = -1, mouse_y = -1;
static int last_myturn;

static void set_toast(const char *t) {
  snprintf(toast, sizeof toast, "%s", t);
  toast_until = tic + 35 * 5;
}

static void hud_message(const char *t) {
  static char msg[96];
  if (gamestate != GS_LEVEL || active) return;
  snprintf(msg, sizeof msg, "SMASH&CLASH: %s", t);
  players[consoleplayer].message = msg;
}

static void sound(int sfx) {
  S_StartSound(NULL, sfx);
}

static void reset_pick(void) {
  stage = 0;
  pick_card = pick_target = -1;
  armed = 0;
  console_i = -1;
}

void SNC_Open(void) {
  if (active) return;
  active = 1;
  if (gamestate == GS_LEVEL && !netgame && !demoplayback) {
    was_paused = paused;
    paused = true;
  }
  sound(sfx_swtchn);
  snc_js_mode(1);
  send("open");
}

static void close_terminal(void) {
  if (!active) return;
  active = 0;
  editing = 0;
  confirm_resign = 0;
  if (gamestate == GS_LEVEL && !netgame && !demoplayback) paused = was_paused;
  sound(sfx_swtchx);
  snc_js_mode(0);
}

boolean SNC_Active(void) {
  return active;
}

/* ---------------------------------------------------------------- parsing the page's snapshots */

static int split(char *line, char **f, int max) {
  int n = 0;
  f[n++] = line;
  for (char *p = line; *p && n < max; p++)
    if (*p == '\t') {
      *p = 0;
      f[n++] = p + 1;
    }
  return n;
}

static void copy(char *dst, size_t n, const char *src) {
  snprintf(dst, n, "%s", src ? src : "");
}

static int cell_arg(const char *s) {
  return (s && *s) ? atoi(s) : -1;
}

static void parse_state(char *text, snc_state_t *st, frame_t *fr) {
  memset(st, 0, sizeof *st);
  st->valid = 1;
  st->seat = 'A';
  st->hop_from = -1;
  for (int i = 0; i < 15; i++) st->board[i].id = 0;
  char *save = NULL;
  for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
    char *f[16];
    int n = split(line, f, 16);
    switch (f[0][0]) {
      case 'G':
        if (n < 11) break;
        st->phase = atoi(f[1]);
        st->seat = f[2][0] == 'B' ? 'B' : 'A';
        st->myturn = atoi(f[3]);
        st->score_me = atoi(f[4]);
        st->score_opp = atoi(f[5]);
        st->opp_hand = atoi(f[6]);
        st->deck = atoi(f[7]);
        st->turn = atoi(f[8]);
        st->classic = f[9][0] == 'c';
        copy(st->status, sizeof st->status, f[10]);
        break;
      case 'P':
        if (n < 7) break;
        copy(st->me, sizeof st->me, f[1]);
        st->me_rating = atoi(f[2]);
        copy(st->opp, sizeof st->opp, f[3]);
        st->opp_rating = atoi(f[4]);
        copy(st->opp_client, sizeof st->opp_client, f[5]);
        copy(st->opp_kind, sizeof st->opp_kind, f[6]);
        break;
      case 'C': {
        if (n < 11) break;
        int c = atoi(f[1]);
        if (c < 0 || c >= 15) break;
        bcell_t *b = &st->board[c];
        b->id = atoi(f[2]);
        b->mine = atoi(f[3]);
        b->frozen = atoi(f[4]);
        for (int k = 0; k < 4; k++) b->v[k] = atoi(f[5 + k]);
        b->frame = hexcolor(f[9]);
        copy(b->name, sizeof b->name, f[10]);
        break;
      }
      case 'X':
        if (n >= 3 && atoi(f[1]) >= 0 && atoi(f[1]) < 15) st->piece[atoi(f[1])] = f[2][0];
        break;
      case 'W':
        if (n >= 4 && atoi(f[1]) >= 0 && atoi(f[1]) < 15) {
          st->power_frame[atoi(f[1])] = hexcolor(f[2]);
          st->power_boost[atoi(f[1])] = atoi(f[3]);
        }
        break;
      case 'H': {
        if (n < 10 || st->nhand >= 5) break;
        hcard_t *h = &st->hand[st->nhand++];
        h->id = atoi(f[1]);
        h->effect = atoi(f[2]);
        for (int k = 0; k < 4; k++) h->v[k] = atoi(f[3 + k]);
        h->frame = hexcolor(f[7]);
        copy(h->name, sizeof h->name, f[8]);
        copy(h->does, sizeof h->does, f[9]);
        break;
      }
      case 'M': {
        if (n < 7 || st->nmoves >= MAXMOVES) break;
        move_t *m = &st->moves[st->nmoves++];
        copy(m->name, sizeof m->name, f[1]);
        m->type = f[2][0];
        m->card = atoi(f[3]);
        m->cell = cell_arg(f[4]);
        m->target = cell_arg(f[5]);
        m->dest = cell_arg(f[6]);
        break;
      }
      case 'K':
        if (n >= 2) st->hop_from = cell_arg(f[1]);
        break;
      case 'O':
        if (n < 5) break;
        copy(st->opt_name, sizeof st->opt_name, f[1]);
        st->opt_rating = atoi(f[2]);
        st->opt_classic = f[3][0] == 'c';
        copy(st->opt_opp, sizeof st->opt_opp, f[4]);
        break;
      case 'R':
        if (n < 3) break;
        copy(st->code, sizeof st->code, f[1]);
        copy(st->link, sizeof st->link, f[2]);
        break;
      case 'E':
        if (n < 5) break;
        st->winner = atoi(f[1]);
        st->delta = atoi(f[2]);
        copy(st->res1, sizeof st->res1, f[3]);
        copy(st->res2, sizeof st->res2, f[4]);
        break;
      case 'V': {
        if (!fr || n < 4 || fr->nev >= MAXEV) break;
        event_t2 *e = &fr->ev[fr->nev++];
        static const char kinds[] = "pcbhxfszroe";
        const char *k = strchr(kinds, f[1][0]);
        e->kind = k ? (int)(k - kinds) : EV_PLACE;
        e->mine = atoi(f[3]);
        e->n = 0;
        for (char *p = f[2]; *p && e->n < 15;) {
          e->cells[e->n++] = atoi(p);
          while (*p && *p != ',') p++;
          if (*p == ',') p++;
        }
        break;
      }
    }
  }
}

/* The page's call: one snapshot. "F\tq" frames queue behind the animations; "F\tn" replaces everything now (a new
 * screen); "F\tt" is the live game the wall screens show when you are not playing; "L\t..." is a message. */
EMSCRIPTEN_KEEPALIVE void snc_push(const char *text) {
  char *buf = strdup(text);
  if (!buf) return;
  if (!strcmp(buf, "OPEN")) { /* the page opened a shared link: show the terminal */
    SNC_Open();
    free(buf);
    return;
  }
  if (buf[0] == 'L') {
    set_toast(buf + 2);
    hud_message(buf + 2);
    free(buf);
    return;
  }
  char kind = buf[0] == 'F' ? buf[2] : 'n';
  char *body = strchr(buf, '\n');
  body = body ? body + 1 : buf + strlen(buf);
  if (kind == 't') {
    parse_state(body, &tv, NULL);
  } else {
    if (kind == 'n') qcount = 0;
    if (qcount == QLEN) { /* far behind: drop the oldest */
      qhead = (qhead + 1) % QLEN;
      qcount--;
    }
    frame_t *fr = &queue[(qhead + qcount) % QLEN];
    memset(fr, 0, sizeof *fr);
    parse_state(body, &fr->st, fr);
    qcount++;
    sent_at = -100000;
  }
  free(buf);
}

/* ---------------------------------------------------------------- applying frames, with their effects */

static int hold_until;

static int hop_mode(void);
static void cursor_to_legal(void);

static void apply_frame(frame_t *fr) {
  int was_phase = have_cur ? cur.phase : -1, was_turn = cur.turn, was_myturn = cur.myturn;
  cur = fr->st;
  have_cur = 1;
  if (cur.phase != PH_PLAY || cur.turn != was_turn || cur.myturn != was_myturn) reset_pick();
  if (hsel >= cur.nhand) hsel = cur.nhand ? cur.nhand - 1 : 0;
  int busy = 0, captured_mine = 0, captured_theirs = 0;
  for (int i = 0; i < fr->nev; i++) {
    event_t2 *e = &fr->ev[i];
    int c0 = e->n ? e->cells[0] : 0;
    switch (e->kind) {
      case EV_PLACE:
        flash_cell = c0;
        flash_until = tic + 12;
        sound(sfx_pstop);
        busy = 1;
        break;
      case EV_CAPTURE:
        for (int k = 0; k < e->n; k++) add_anim(&A_EXPLODE, e->cells[k], 6 + k * 3);
        if (e->mine) captured_theirs += e->n;
        else captured_mine += e->n;
        sound(e->n >= 2 ? sfx_rxplod : sfx_shotgn);
        busy = 1;
        break;
      case EV_BLOCKED:
        add_anim(&A_PUFF, c0, 4);
        sound(sfx_tink);
        break;
      case EV_HOP:
        add_anim(&A_TFOG, c0, 0);
        sound(sfx_telept);
        busy = 1;
        break;
      case EV_BOULDER:
        for (int k = 0; k < e->n; k++) add_anim(&A_BFG, e->cells[k], k * 2);
        sound(sfx_rxplod);
        shake_until = tic + 14;
        busy = 1;
        break;
      case EV_FLIP:
        shake_until = tic + 18;
        sound(sfx_bfg);
        busy = 1;
        break;
      case EV_SWAP:
        sound(sfx_telept);
        set_toast("SWAP! THE HANDS CHANGE PLACES");
        busy = 1;
        break;
      case EV_FREEZE:
        add_anim(&A_PLASMA, c0, 0);
        sound(sfx_plasma);
        busy = 1;
        break;
      case EV_RECRUIT:
        for (int k = 0; k < e->n; k++) add_anim(&A_TFOG, e->cells[k], k * 6);
        sound(sfx_telept);
        busy = 1;
        break;
      case EV_OVERRUN:
        add_anim(&A_IFOG, c0, 0);
        sound(sfx_stnmov);
        busy = 1;
        break;
      case EV_OVER:
        sound(e->mine > 0 ? sfx_getpow : e->mine < 0 ? sfx_pldeth : sfx_tink);
        busy = 1;
        break;
    }
  }
  if (captured_theirs >= 3) set_face("STFKILL0", 70);
  else if (captured_theirs) set_face("STFEVL0", 50);
  else if (captured_mine) set_face("STFOUCH0", 50);
  if (cur.phase == PH_PLAY && was_phase != PH_PLAY) {
    sound(sfx_sgtsit); /* an opponent sighted */
    char m[80];
    snprintf(m, sizeof m, "%s VS %s", cur.me, cur.opp);
    hud_message(m);
  }
  if (cur.phase == PH_PLAY && cur.myturn && !last_myturn) {
    sound(sfx_wpnup);
    hud_message("YOUR TURN - USE A COMPUTER OR PRESS C");
  }
  if (cur.phase == PH_OVER && was_phase != PH_OVER) hud_message(cur.res1);
  last_myturn = cur.phase == PH_PLAY && cur.myturn;
  if (cur.phase == PH_PLAY && cur.myturn && hop_mode()) {
    focus_board = 1;
    cursor_to_legal();
  }
  hold_until = tic + (busy ? (qcount > 3 ? 8 : 24) : 0);
}

/* ---------------------------------------------------------------- picking a move */

static const move_t *moves_for(const snc_state_t *st, int card, int cell, int target, const move_t **out, int max) {
  int n = 0;
  for (int i = 0; i < st->nmoves && n < max; i++) {
    const move_t *m = &st->moves[i];
    if (card >= 0 && m->card != card) continue;
    if (cell >= 0 && m->cell != cell && m->target != cell) continue;
    if (target >= 0 && m->target != target) continue;
    out[n++] = m;
  }
  if (n < max) out[n] = NULL;
  return n ? out[0] : NULL;
}

static int count_moves(const move_t **list) {
  int n = 0;
  while (list[n]) n++;
  return n;
}

static int hop_mode(void) {
  return cur.nmoves > 0 && cur.moves[0].type == 'h';
}

static int can_input(void) {
  return have_cur && cur.phase == PH_PLAY && cur.myturn && tic - sent_at > 35 * 8 && qcount == 0;
}

static void play(const char *name) {
  send("play\t%s", name);
  sent_at = tic;
  sound(sfx_pistol);
  reset_pick();
  focus_board = 0;
}

/* is this board cell a legal choice at the current stage? */
static int legal_cell(int cell) {
  if (!can_input()) return 0;
  if (hop_mode()) {
    for (int i = 0; i < cur.nmoves; i++)
      if (cur.moves[i].dest == cell || (cur.moves[i].dest < 0 && cell == cur.hop_from)) return 1;
    return 0;
  }
  for (int i = 0; i < cur.nmoves; i++) {
    const move_t *m = &cur.moves[i];
    if (stage == 1 && m->card == pick_card && (m->cell == cell || m->target == cell)) return 1;
    if (stage == 2 && m->card == pick_card && m->target == pick_target && m->dest == cell) return 1;
  }
  return 0;
}

/* put the board cursor on the first legal tile, if there is one */
static void cursor_to_legal(void) {
  for (int sr = 0; sr < 3; sr++)
    for (int sc = 0; sc < 5; sc++)
      if (legal_cell(screen_to_cell(&cur, sr, sc))) {
        bsr = sr, bsc = sc;
        return;
      }
}

static void choose_cell(int cell) {
  if (!can_input()) return;
  if (hop_mode()) {
    for (int i = 0; i < cur.nmoves; i++) {
      const move_t *m = &cur.moves[i];
      if (m->dest == cell || (m->dest < 0 && cell == cur.hop_from)) {
        play(m->name);
        return;
      }
    }
    sound(sfx_noway);
    return;
  }
  const move_t *list[MAXMOVES + 1];
  if (stage == 1) {
    moves_for(&cur, pick_card, cell, -1, list, MAXMOVES);
    int n = count_moves(list);
    if (!n) {
      sound(sfx_noway);
      return;
    }
    if (list[0]->dest >= 0 && list[0]->target == cell) { /* RECRUIT: now where it goes */
      stage = 2;
      pick_target = cell;
      sound(sfx_pstop);
      cursor_to_legal();
      return;
    }
    play(list[0]->name);
  } else if (stage == 2) {
    for (int i = 0; i < cur.nmoves; i++) {
      const move_t *m = &cur.moves[i];
      if (m->card == pick_card && m->target == pick_target && m->dest == cell) {
        play(m->name);
        return;
      }
    }
    sound(sfx_noway);
  }
}

static void choose_card(int i) {
  if (!can_input() || i < 0 || i >= cur.nhand) return;
  if (hop_mode()) return;
  hsel = i;
  const move_t *list[MAXMOVES + 1];
  moves_for(&cur, cur.hand[i].id, -1, -1, list, MAXMOVES);
  int n = count_moves(list);
  if (!n) {
    set_toast("THAT CARD CAN'T BE PLAYED NOW");
    sound(sfx_noway);
    return;
  }
  if (list[0]->cell < 0 && list[0]->target < 0) { /* FLIP, SWAP: press again to play */
    if (armed && pick_card == cur.hand[i].id) {
      play(list[0]->name);
      return;
    }
    armed = 1;
    pick_card = cur.hand[i].id;
    sound(sfx_pstop);
    return;
  }
  pick_card = cur.hand[i].id;
  stage = 1;
  armed = 0;
  focus_board = 1;
  sound(sfx_pstop);
  cursor_to_legal();
}

/* ---------------------------------------------------------------- hit areas (mouse) */

enum { HIT_HAND, HIT_CELL, HIT_LOBBY, HIT_DOOM, HIT_RESIGN, HIT_LOBBYBTN, HIT_AGAIN, HIT_CANCEL };

typedef struct {
  int x, y, w, h, kind, arg;
} hit_t;

static hit_t hits[64];
static int nhits;

static void add_hit(int x, int y, int w, int h, int kind, int arg) {
  if (nhits < 64) hits[nhits++] = (hit_t){x, y, w, h, kind, arg};
}

static const hit_t *hit_at(int x, int y) {
  for (int i = nhits - 1; i >= 0; i--)
    if (x >= hits[i].x && y >= hits[i].y && x < hits[i].x + hits[i].w && y < hits[i].y + hits[i].h) return &hits[i];
  return NULL;
}

/* ---------------------------------------------------------------- the lobby */

enum { L_HOUSE, L_QUICK, L_ROOM, L_JOIN, L_WATCH, L_RULES, L_NAME, L_DOOM, L_COUNT };

static const char *opp_label(const char *o) {
  return !strcmp(o, "person") ? "PEOPLE" : !strcmp(o, "agent") ? "AGENTS" : "ANYONE";
}

static void lobby_item_text(int i, char *out, size_t n) {
  const snc_state_t *st = &cur;
  switch (i) {
    case L_HOUSE: snprintf(out, n, "PLAY THE HOUSE"); break;
    case L_QUICK: snprintf(out, n, "QUICK MATCH: %s", opp_label(st->opt_opp)); break;
    case L_ROOM: snprintf(out, n, "CREATE A ROOM"); break;
    case L_JOIN: snprintf(out, n, "JOIN A ROOM: %s%s", editing == 2 ? edit_buf : "", editing == 2 && (tic / 8) % 2 ? "_" : ""); break;
    case L_WATCH: snprintf(out, n, "WATCH A LIVE GAME"); break;
    case L_RULES: snprintf(out, n, "RULES: %s", st->opt_classic ? "CLASSIC" : "MUTATORS"); break;
    case L_NAME: snprintf(out, n, "NAME: %s%s", editing == 1 ? edit_buf : st->opt_name, editing == 1 && (tic / 8) % 2 ? "_" : ""); break;
    case L_DOOM: snprintf(out, n, "BACK TO DOOM"); break;
  }
}

static void lobby_activate(int i, int dir) {
  snc_state_t *st = &cur;
  switch (i) {
    case L_HOUSE: send("house"); break;
    case L_QUICK:
      if (dir) {
        const char *next = !strcmp(st->opt_opp, "any") ? (dir > 0 ? "person" : "agent")
                         : !strcmp(st->opt_opp, "person") ? (dir > 0 ? "agent" : "any")
                         : (dir > 0 ? "any" : "person");
        snprintf(st->opt_opp, sizeof st->opt_opp, "%s", next);
        send("set\topponent\t%s", next);
        sound(sfx_pstop);
        return;
      }
      send("quick");
      break;
    case L_ROOM: send("room"); break;
    case L_JOIN:
      if (dir) return;
      editing = 2;
      edit_buf[0] = 0;
      sound(sfx_pstop);
      return;
    case L_WATCH: send("watch"); break;
    case L_RULES:
      st->opt_classic = !st->opt_classic;
      send("set\truleset\t%s", st->opt_classic ? "classic" : "mutators");
      sound(sfx_pstop);
      return;
    case L_NAME:
      if (dir) return;
      editing = 1;
      snprintf(edit_buf, sizeof edit_buf, "%s", st->opt_name);
      sound(sfx_pstop);
      return;
    case L_DOOM: close_terminal(); return;
  }
  sound(sfx_pistol);
}

static void edit_key(int key, int ch) {
  size_t l = strlen(edit_buf);
  if (key == KEY_ENTER) {
    if (editing == 1 && l) send("set\tname\t%s", edit_buf), snprintf(cur.opt_name, sizeof cur.opt_name, "%s", edit_buf);
    if (editing == 2 && l) send("join\t%s", edit_buf);
    editing = 0;
    sound(sfx_pistol);
  } else if (key == KEY_ESCAPE) {
    editing = 0;
    sound(sfx_swtchx);
  } else if (key == KEY_BACKSPACE) {
    if (l) edit_buf[l - 1] = 0;
  } else if (ch >= 32 && ch < 127 && l < (editing == 2 ? 6u : 16u)) {
    int c = toupper(ch);
    if (editing == 2 && !isalnum(c)) return;
    if (editing == 1 && !strchr(SMALL_CHARS, c) && c != ' ') return;
    edit_buf[l] = (char)c;
    edit_buf[l + 1] = 0;
  }
}

/* ---------------------------------------------------------------- input */

static void move_board(int dr, int dc) {
  bsr = (bsr + dr + 3) % 3;
  bsc = (bsc + dc + 5) % 5;
  sound(sfx_pstop);
}

static int phase(void) {
  return have_cur ? cur.phase : PH_BUSY;
}

static void key_play(int key, int ch) {
  if (confirm_resign) {
    if (ch == 'y' || ch == 'Y') send("resign"), sound(sfx_pldeth);
    confirm_resign = 0;
    return;
  }
  if (key == KEY_TAB && can_input() && cur.nmoves) {
    console_i = (console_i + 1) % cur.nmoves;
    sound(sfx_pstop);
    return;
  }
  if (key == KEY_ENTER && console_i >= 0 && can_input()) {
    play(cur.moves[console_i].name);
    return;
  }
  if (key == KEY_ESCAPE) {
    if (focus_board && !hop_mode()) {
      focus_board = 0;
      reset_pick();
      sound(sfx_swtchx);
    } else {
      close_terminal();
    }
    return;
  }
  if (ch == 'r' || ch == 'R') {
    confirm_resign = 1;
    set_toast("RESIGN THIS GAME? Y / N");
    return;
  }
  if (ch >= '1' && ch <= '5') {
    choose_card(ch - '1');
    return;
  }
  console_i = -1;
  if (hop_mode()) focus_board = 1;
  if (focus_board) {
    switch (key) {
      case KEY_UPARROW: move_board(-1, 0); break;
      case KEY_DOWNARROW:
        if (bsr == 2 && !hop_mode()) {
          focus_board = 0;
          reset_pick();
          sound(sfx_pstop);
        } else {
          move_board(1, 0);
        }
        break;
      case KEY_LEFTARROW: move_board(0, -1); break;
      case KEY_RIGHTARROW: move_board(0, 1); break;
      case KEY_ENTER: case KEY_USE: choose_cell(screen_to_cell(&cur, bsr, bsc)); break;
      case KEY_BACKSPACE:
        if (hop_mode()) { /* stay */
          for (int i = 0; i < cur.nmoves; i++)
            if (cur.moves[i].dest < 0) play(cur.moves[i].name);
        } else {
          focus_board = 0;
          reset_pick();
        }
        break;
    }
  } else {
    switch (key) {
      case KEY_LEFTARROW: if (cur.nhand) hsel = (hsel + cur.nhand - 1) % cur.nhand, armed = 0, sound(sfx_pstop); break;
      case KEY_RIGHTARROW: if (cur.nhand) hsel = (hsel + 1) % cur.nhand, armed = 0, sound(sfx_pstop); break;
      case KEY_UPARROW:
        if (can_input()) choose_card(hsel);
        else focus_board = 1, sound(sfx_pstop); /* look around the board while waiting */
        break;
      case KEY_ENTER: case KEY_USE: choose_card(hsel); break;
    }
  }
}

boolean SNC_Responder(event_t *ev) {
  if (!inited) return false;
  if (!active) {
    if (ev->type == ev_keydown && (ev->data1 == 'c') && !menuactive && gamestate != GS_INTERMISSION &&
        gamestate != GS_FINALE) {
      SNC_Open();
      return true;
    }
    if (ev->type == ev_keydown && ev->data1 == 't' && !menuactive && gamestate == GS_LEVEL && !demoplayback && !netgame) {
      teleport_to_screen();
      return true;
    }
    return false;
  }
  if (ev->type == ev_mouse) return true;
  if (ev->type != ev_keydown) return true;
  int key = ev->data1, ch = ev->data2 ? ev->data2 : (key < 128 ? key : 0);
  if (editing) {
    edit_key(key, ch);
    return true;
  }
  switch (phase()) {
    case PH_LOBBY:
      if (key == KEY_UPARROW) lobby_sel = (lobby_sel + L_COUNT - 1) % L_COUNT, sound(sfx_pstop);
      else if (key == KEY_DOWNARROW) lobby_sel = (lobby_sel + 1) % L_COUNT, sound(sfx_pstop);
      else if (key == KEY_LEFTARROW) lobby_activate(lobby_sel, -1);
      else if (key == KEY_RIGHTARROW) lobby_activate(lobby_sel, 1);
      else if (key == KEY_ENTER || key == KEY_USE) lobby_activate(lobby_sel, 0);
      else if (key == KEY_ESCAPE || key == KEY_BACKSPACE) close_terminal();
      break;
    case PH_WAITING:
      if (key == KEY_ESCAPE || key == KEY_BACKSPACE) send("leave"), sound(sfx_swtchx);
      break;
    case PH_PLAY: key_play(key, ch); break;
    case PH_OVER:
      if (key == KEY_ENTER || key == KEY_USE) send("again"), sound(sfx_pistol);
      else if (key == KEY_BACKSPACE || ch == 'l' || ch == 'L') send("lobby"), sound(sfx_swtchx);
      else if (key == KEY_ESCAPE) close_terminal();
      break;
    case PH_WATCH:
      if (key == KEY_BACKSPACE || key == KEY_ENTER) send("lobby"), sound(sfx_swtchx);
      else if (key == KEY_ESCAPE) close_terminal();
      break;
    default:
      if (key == KEY_ESCAPE) close_terminal();
      break;
  }
  return true;
}

/* The page's pointer, in 320x200 screen coordinates: kind 0 move, 1 left click, 2 right click. */
EMSCRIPTEN_KEEPALIVE void snc_pointer(int x, int y, int kind) {
  mouse_x = x, mouse_y = y;
  if (!active || kind == 0) {
    const hit_t *h = active ? hit_at(x, y) : NULL;
    if (h && h->kind == HIT_LOBBY && !editing) lobby_sel = h->arg;
    if (h && h->kind == HIT_CELL && have_cur && cur.phase == PH_PLAY) bsr = h->arg / 5, bsc = h->arg % 5;
    return;
  }
  if (kind == 2) {
    if (phase() == PH_PLAY) {
      focus_board = 0;
      reset_pick();
      sound(sfx_swtchx);
    }
    return;
  }
  const hit_t *h = hit_at(x, y);
  if (!h) return;
  switch (h->kind) {
    case HIT_HAND: choose_card(h->arg); break;
    case HIT_CELL:
      bsr = h->arg / 5, bsc = h->arg % 5;
      focus_board = 1;
      choose_cell(screen_to_cell(&cur, bsr, bsc));
      break;
    case HIT_LOBBY:
      lobby_sel = h->arg;
      lobby_activate(h->arg, 0);
      break;
    case HIT_DOOM: close_terminal(); break;
    case HIT_RESIGN:
      confirm_resign = 1;
      set_toast("RESIGN THIS GAME? Y / N");
      break;
    case HIT_LOBBYBTN: send("lobby"), sound(sfx_swtchx); break;
    case HIT_AGAIN: send("again"), sound(sfx_pistol); break;
    case HIT_CANCEL: send("leave"), sound(sfx_swtchx); break;
  }
}

/* ---------------------------------------------------------------- drawing the terminal */

static void button(surf_t *s, int x, int y, int w, const char *label, int fillc, int kind, int arg) {
  fill(s, x, y, w, 11, fillc);
  frame_rect(s, x, y, w, 11, C_INK);
  int hover = mouse_x >= x && mouse_y >= y && mouse_x < x + w && mouse_y < y + 11;
  if (hover) frame_rect(s, x - 1, y - 1, w + 2, 13, C_WHITE);
  small_text(s, x + (w - small_width(label)) / 2, y + 3, label, C_WHITE);
  add_hit(x, y, w, 11, kind, arg);
}

static void draw_plate(surf_t *s, int x, int y, int w, const char *name, int rating, const char *client, int you) {
  fill(s, x, y, w, 15, you ? C_YOU_DK : C_THEM_DK);
  frame_rect(s, x, y, w, 15, you ? C_YOU : C_THEM);
  char line[48], fit[48];
  hu_fit(fit, sizeof fit, name, w - 8);
  hu_text(s, x + 4, y + 1, fit, R_WHITE, 1);
  if (rating) snprintf(line, sizeof line, "%d", rating);
  else line[0] = 0;
  if (client && client[0]) {
    char both[64];
    snprintf(both, sizeof both, "%s%s%s", line, line[0] ? " - " : "", client);
    snprintf(line, sizeof line, "%.*s", (w - 8) / 4, both);
  }
  small_text(s, x + 4, y + 9, line, you ? C_YOU_HI : C_THEM_HI);
}

static void draw_board(surf_t *s, const snc_state_t *st, int interactive) {
  int shake = tic < shake_until ? ((tic & 2) ? 2 : -2) : 0;
  /* column letters and row numbers, as the viewer sees them */
  for (int sc = 0; sc < 5; sc++) {
    char l[2] = {(char)('A' + (st->seat == 'B' ? 4 - sc : sc)), 0};
    small_text(s, BX + sc * CW + CW / 2 - 1, BY + 3 * CH + 1, l, C_PANEL_HI);
  }
  for (int sr = 0; sr < 3; sr++) {
    char l[2] = {(char)('1' + (st->seat == 'B' ? sr : 2 - sr)), 0};
    small_text(s, BX - 4, BY + sr * CH + CH / 2 - 2, l, C_PANEL_HI);
  }
  for (int sr = 0; sr < 3; sr++)
    for (int sc = 0; sc < 5; sc++) {
      int cell = screen_to_cell(st, sr, sc);
      int x = BX + sc * CW + shake, y = BY + sr * CH;
      const bcell_t *b = &st->board[cell];
      int bg = st->power_boost[cell] ? st->power_frame[cell] : C_PANEL;
      fill(s, x, y, CW, CH, bg);
      if (st->power_boost[cell]) remap_rect(s, x + 2, y + 2, CW - 4, CH - 4, R_DARK);
      frame_rect(s, x, y, CW, CH, C_PANEL_HI);
      /* the overrun zones B2 C2 D2: corner ticks */
      if (!st->classic && cell >= 6 && cell <= 8) {
        fill(s, x + 1, y + 1, 4, 1, C_GRAPE), fill(s, x + 1, y + 1, 1, 4, C_GRAPE);
        fill(s, x + CW - 5, y + CH - 2, 4, 1, C_GRAPE), fill(s, x + CW - 2, y + CH - 5, 1, 4, C_GRAPE);
      }
      if (b->id) {
        int v[4];
        facing(st, b, v);
        int dy = (cell == flash_cell && tic < flash_until) ? -(flash_until - tic) / 2 : 0;
        draw_card(s, x + 4, y + 1 + dy, CW - 8, CH - 2, b->id, 0, v, b->frame, 0, b->mine ? 1 : 2, b->frozen);
        if (cell == flash_cell && tic < flash_until && (tic & 1)) frame_rect(s, x + 3, y + dy, CW - 6, CH, C_WHITE);
      }
      if (st->piece[cell]) {
        char p[2] = {st->piece[cell], 0};
        fill(s, x + CW - 8, y + 1, 7, 7, C_GOLD);
        small_text(s, x + CW - 6, y + 2, p, C_INK);
      }
      if (st->power_boost[cell]) {
        char p[4];
        snprintf(p, sizeof p, "+%d", st->power_boost[cell]);
        small_text_outlined(s, x + 2, y + CH - 7, p, C_WHITE, C_INK);
      }
      if (interactive) {
        add_hit(x, y, CW, CH, HIT_CELL, sr * 5 + sc);
        if (legal_cell(cell)) {
          int c = (tic / 6) % 2 ? C_LEGAL : C_WHITE;
          frame_rect(s, x + 1, y + 1, CW - 2, CH - 2, c);
        }
        if (focus_board && sr == bsr && sc == bsc) {
          int c = (tic / 4) % 2 ? C_GOLD : C_WHITE;
          frame_rect(s, x, y, CW, CH, c);
          frame_rect(s, x + 1, y + 1, CW - 2, CH - 2, c);
        }
        if (stage == 2 && cell == pick_target) frame_rect(s, x + 2, y + 2, CW - 4, CH - 4, C_THEM_HI);
        if (hop_mode() && cell == st->hop_from) frame_rect(s, x + 2, y + 2, CW - 4, CH - 4, C_GOLD);
      }
    }
}

static void draw_hand(surf_t *s, const snc_state_t *st) {
  for (int i = 0; i < st->nhand; i++) {
    const hcard_t *h = &st->hand[i];
    int x = HX + i * CW + 3, y = HY;
    int selected = (i == hsel && !focus_board) || h->id == pick_card;
    if (selected) y -= 3;
    draw_card(s, x, y, 34, 46, h->id, h->effect, h->v, h->frame, 1, 0, 0);
    if (selected) {
      int c = (tic / 4) % 2 ? C_GOLD : C_WHITE;
      frame_rect(s, x - 1, y - 1, 36, 48, c);
      if (armed && h->id == pick_card) small_text_outlined(s, x + 2, y + 18, "AGAIN!", C_GOLD, C_INK);
    }
    add_hit(x, y, 34, 46, HIT_HAND, i);
  }
}

static void draw_score(surf_t *s, int x, int y, int v) {
  char d[4];
  snprintf(d, sizeof d, "%02d", v);
  for (int i = 0; d[i]; i++) {
    char name[9];
    snprintf(name, sizeof name, "STTNUM%c", d[i]);
    draw_patch(s, x + i * 14, y, patch(name), NULL);
  }
}

static void draw_preview(surf_t *s, const snc_state_t *st, int x, int y) {
  /* the card under the cursor: a hand card, or a board card while looking around */
  int pv[4], frame = 0, id = 0, effect = 0, owner = 0, frozen = 0;
  const char *name = "", *does = "";
  if (focus_board || hop_mode()) {
    const bcell_t *b = &st->board[screen_to_cell(st, bsr, bsc)];
    if (b->id) {
      facing(st, b, pv);
      id = b->id, frame = b->frame, name = b->name, owner = b->mine ? 1 : 2, frozen = b->frozen;
    }
  }
  if (!id && st->nhand && (!focus_board || pick_card >= 0)) {
    const hcard_t *h = &st->hand[hsel < st->nhand ? hsel : 0];
    for (int i = 0; i < st->nhand; i++)
      if (st->hand[i].id == pick_card) h = &st->hand[i];
    memcpy(pv, h->v, sizeof pv);
    id = h->id, frame = h->frame, effect = h->effect, name = h->name, does = h->does;
  }
  if (!id) return;
  draw_card(s, x, y, 62, 85, id, effect, pv, frame, 2, owner, frozen);
  int tx = x + 66, tw = 318 - tx, ty = y + 2;
  if (hu_width(name) <= tw) {
    hu_text(s, tx, ty, name, owner == 2 ? R_THEM : owner == 1 ? R_YOU : R_GOLD, 1);
    ty += 12;
  } else {
    ty = small_wrap(s, tx, ty, tw, name, owner == 2 ? C_THEM_HI : owner == 1 ? C_YOU_HI : C_GOLD, 3) + 3;
  }
  if (effect) small_wrap(s, tx, ty, tw, does, C_WHITE, 9);
  else if (frozen) small_text(s, tx, ty, "FROZEN", C_ICE);
  else if (owner) small_text(s, tx, ty, owner == 1 ? "YOURS" : "THEIRS", owner == 1 ? C_YOU_HI : C_THEM_HI);
}

static const char *prompt(void) {
  if (!cur.myturn) return NULL;
  if (tic - sent_at <= 35 * 8) return "PLAYING...";
  if (hop_mode()) return "HOP? PICK A TILE, BKSP STAYS";
  if (stage == 2) return "PICK WHERE IT GOES";
  if (stage == 1) {
    for (int i = 0; i < cur.nmoves; i++)
      if (cur.moves[i].card == pick_card && cur.moves[i].type == 'e') return "PICK A TARGET";
    return "PICK A TILE";
  }
  if (armed) return "ENTER AGAIN TO PLAY IT";
  return "PICK A CARD";
}

static void draw_play(surf_t *s) {
  const snc_state_t *st = &cur;
  int watch = st->phase == PH_WATCH;
  /* the other side: their plate and their hand as card backs (counts only: fair play) */
  draw_plate(s, BX, 1, 140, st->opp, st->opp_rating, st->opp_client, 0);
  if (watch) {
    draw_plate(s, BX + 145, 1, 56, st->me, 0, NULL, 1);
  } else {
    for (int i = 0; i < st->opp_hand && i < 5; i++) draw_patch(s, BX + 146 + i * 11, 1, patch("SCBKS"), NULL);
  }
  draw_board(s, st, st->phase == PH_PLAY);
  draw_anims(s, st);
  if (!watch) draw_hand(s, st);
  else {
    fill(s, HX, HY + 4, 200, 40, C_PANEL_LO);
    char line[64];
    snprintf(line, sizeof line, "LIVE ON THE SMASH&CLASH NETWORK");
    small_text(s, HX + 6, HY + 10, line, C_GOLD);
    snprintf(line, sizeof line, "%s", st->status);
    small_wrap(s, HX + 6, HY + 19, 190, line, C_WHITE, 3);
  }

  /* the right panel */
  fill(s, RX, 0, 320 - RX, 200, C_PANEL_LO);
  frame_rect(s, RX, 0, 320 - RX, 200, C_PANEL_HI);
  draw_patch(s, RX + 3, 17, patch(current_face()), NULL);
  small_text(s, RX + 32, 20, watch ? "A" : "YOU", C_YOU_HI);
  draw_score(s, RX + 70, 16, st->score_me);
  small_text(s, RX + 32, 36, watch ? "B" : "THEM", C_THEM_HI);
  draw_score(s, RX + 70, 32, st->score_opp);
  hu_text(s, RX + 4, 3, "SMASH&CLASH", R_GOLD, 1);

  char line[80], fit[80];
  int over = st->phase == PH_OVER;
  int bc = over ? C_PANEL_HI : st->myturn ? C_YOU : C_THEM;
  fill(s, RX + 2, 50, 320 - RX - 4, 12, bc);
  if (over) snprintf(line, sizeof line, "%s", st->winner > 0 ? "YOU WIN!" : st->winner < 0 ? "YOU LOSE" : "DRAW");
  else if (watch) snprintf(line, sizeof line, "LIVE: TURN %d", st->turn);
  else snprintf(line, sizeof line, st->myturn ? "YOUR TURN" : "THEIR TURN");
  hu_fit(fit, sizeof fit, line, 320 - RX - 8);
  hu_text(s, RX + 4, 52, fit, st->myturn && !over && (tic / 10) % 2 ? R_GOLD : R_WHITE, 1);

  if (over) {
    int y = 66;
    hu_fit(fit, sizeof fit, st->res1, 320 - RX - 8);
    hu_text(s, RX + 4, y, fit, R_GOLD, 1);
    y = small_wrap(s, RX + 4, y + 11, 320 - RX - 8, st->res2, C_WHITE, 8);
    if (st->delta) {
      snprintf(line, sizeof line, "RATING %+d", st->delta);
      small_text(s, RX + 4, y + 2, line, st->delta > 0 ? C_LEGAL : C_THEM_HI);
    }
    button(s, RX + 4, 150, 100, "ENTER: PLAY AGAIN", C_YOU_DK, HIT_AGAIN, 0);
    button(s, RX + 4, 164, 100, "BKSP: LOBBY", C_PANEL, HIT_LOBBYBTN, 0);
    button(s, RX + 4, 178, 100, "ESC: BACK TO DOOM", C_PANEL, HIT_DOOM, 0);
  } else if (watch) {
    small_wrap(s, RX + 4, 68, 320 - RX - 8, "YOU ARE WATCHING. NOBODY'S HAND IS SHOWN, ONLY THE BOARD.", C_WHITE, 6);
    button(s, RX + 4, 164, 100, "BKSP: LOBBY", C_PANEL, HIT_LOBBYBTN, 0);
    button(s, RX + 4, 178, 100, "ESC: BACK TO DOOM", C_PANEL, HIT_DOOM, 0);
  } else {
    draw_preview(s, st, RX + 3, 65);
    int y = 153;
    const char *p = prompt();
    if (console_i >= 0 && console_i < st->nmoves) {
      snprintf(line, sizeof line, "> %s", st->moves[console_i].name);
      for (char *c = line; *c; c++)
        if ((unsigned char)*c >= 0x80) *c = '>';
      small_text(s, RX + 4, y, line, C_GOLD);
      small_text(s, RX + 4, y + 7, "ENTER PLAYS IT  TAB NEXT", C_PANEL_HI);
    } else if (p) {
      small_text(s, RX + 4, y, p, C_GOLD);
      small_text(s, RX + 4, y + 7, "ARROWS+ENTER OR MOUSE", C_PANEL_HI);
    } else {
      snprintf(line, sizeof line, "DECK %d", st->deck);
      small_text(s, RX + 4, y, line, C_PANEL_HI);
      small_text(s, RX + 4, y + 7, "UP: LOOK AT THE BOARD", C_PANEL_HI);
    }
    button(s, RX + 4, 170, 48, "R: RESIGN", C_THEM_DK, HIT_RESIGN, 0);
    button(s, RX + 56, 170, 48, "ESC: DOOM", C_PANEL, HIT_DOOM, 0);
  }
  if (tic < toast_until) {
    fill(s, RX + 2, 186, 320 - RX - 4, 12, C_INK);
    char t[40];
    snprintf(t, sizeof t, "%.26s", toast);
    small_text(s, RX + 4, 189, t, C_GOLD);
  }
}

static void draw_lobby(surf_t *s) {
  const snc_state_t *st = &cur;
  patch_t *logo = patch("SCLOGOM");
  if (logo) draw_patch(s, (320 - SHORT(logo->width)) / 2, 2, logo, NULL);
  int y0 = logo ? SHORT(logo->height) + 6 : 40;
  for (int i = 0; i < L_COUNT; i++) {
    char t[64];
    lobby_item_text(i, t, sizeof t);
    int y = y0 + i * 12, x = 100;
    hu_text(s, x, y, t, i == lobby_sel ? R_GOLD : R_WHITE, 1);
    add_hit(x - 20, y - 2, 200, 12, HIT_LOBBY, i);
    if (i == lobby_sel) draw_patch(s, x - 20, y - 4, patch((tic / 8) % 2 ? "M_SKULL1" : "M_SKULL2"), NULL);
    if (i == L_QUICK && i == lobby_sel) small_text(s, x + hu_width(t) + 4, y + 1, "< >", C_PANEL_HI);
  }
  char line[96];
  snprintf(line, sizeof line, "%s - RATING %d - ANY CLIENT, ANY PLAYER", st->opt_name, st->opt_rating);
  small_text(s, (320 - small_width(line)) / 2, 186, line, C_PANEL_HI);
  small_text(s, (320 - small_width("SMASH&CLASH ART AND NAME (C) SMASH&CLASH")) / 2, 193, "SMASH&CLASH ART AND NAME (C) SMASH&CLASH", C_PANEL_HI);
  if (tic < toast_until) {
    char t[80];
    snprintf(t, sizeof t, "%.70s", toast);
    fill(s, 0, 176, 320, 9, C_INK);
    small_text(s, (320 - small_width(t)) / 2, 178, t, C_GOLD);
  }
}

static void draw_center_box(surf_t *s, const char *title, const char *l1, const char *l2) {
  fill(s, 30, 50, 260, 100, C_PANEL_LO);
  frame_rect(s, 30, 50, 260, 100, C_PANEL_HI);
  hu_text(s, 160 - hu_width(title), 58, title, R_GOLD, 2);
  if (l1) small_wrap(s, 40, 82, 240, l1, C_WHITE, 4);
  if (l2) small_wrap(s, 40, 112, 240, l2, C_PANEL_HI, 3);
}

static void draw_waiting(surf_t *s) {
  const snc_state_t *st = &cur;
  static const char *spin = "|/-\\";
  char t[96];
  if (st->code[0]) {
    fill(s, 30, 40, 260, 120, C_PANEL_LO);
    frame_rect(s, 30, 40, 260, 120, C_PANEL_HI);
    hu_text(s, 160 - hu_width("ROOM CODE") / 2, 46, "ROOM CODE", R_GOLD, 1);
    hu_text(s, 160 - hu_width(st->code), 60, st->code, R_WHITE, 2);
    small_wrap(s, 40, 84, 240, "ANYONE CAN JOIN WITH THIS CODE FROM ANY SMASH&CLASH CLIENT: THE WEBSITE, THE APPS, THE TERMINAL, AN AGENT OR ANOTHER DOOM.", C_WHITE, 4);
    snprintf(t, sizeof t, "%s", st->link);
    for (char *c = t; *c; c++) *c = (char)toupper((unsigned char)*c);
    small_wrap(s, 40, 116, 240, t, C_YOU_HI, 2);
    snprintf(t, sizeof t, "WAITING FOR A PLAYER %c", spin[(tic / 6) % 4]);
    small_text(s, 40, 134, t, C_GOLD);
  } else {
    snprintf(t, sizeof t, "%s %c", st->status, spin[(tic / 6) % 4]);
    draw_center_box(s, "SEARCHING", t, "ONE QUEUE FOR EVERY CLIENT: PEOPLE ON THE WEBSITE, THE APPS, THE CLI AND AGENTS WAIT HERE TOO.");
  }
  button(s, 110, 150, 100, "BKSP: CANCEL", C_THEM_DK, HIT_CANCEL, 0);
}

void SNC_Drawer(void) {
  if (!inited || !active) return;
  surf_t *s = &screen;
  s->p = I_VideoBuffer;
  nhits = 0;
  /* the world behind, darkened with DOOM's own light tables */
  remap_rect(s, 0, 0, 320, 200, R_DARK);
  switch (phase()) {
    case PH_LOBBY: draw_lobby(s); break;
    case PH_WAITING: draw_waiting(s); break;
    case PH_PLAY: case PH_OVER: case PH_WATCH: draw_play(s); break;
    default: {
      char t[64];
      snprintf(t, sizeof t, "%s", have_cur && cur.status[0] ? cur.status : "REACHING THE SMASH&CLASH NETWORK");
      draw_center_box(s, "CONNECTING", t, "ESC GOES BACK TO DOOM");
      break;
    }
  }
}

/* ---------------------------------------------------------------- the computer screens on the walls */

typedef struct {
  const char *name;
  int x, y, w, h; /* where the monitor goes on the texture */
  int tex, width;
  byte *base, *live;
} screen_tex_t;

static screen_tex_t walls[] = {
  {"COMPUTE4", 4, 4, 248, 120}, {"COMPSTA1", 4, 3, 120, 64}, {"COMPSTA2", 4, 3, 120, 64}, {"COMPUTE1", 4, 4, 120, 44},
  {"COMPUTE3", 4, 4, 120, 48}, {"COMP2", 4, 4, 120, 48}, {"AQCOMP01", 4, 4, 120, 60},
};
#define NWALLS ((int)(sizeof walls / sizeof walls[0]))

static void draw_monitor(surf_t *s, int x0, int y0, int w, int h) {
  const snc_state_t *st = have_cur && cur.phase >= PH_WAITING ? &cur : tv.valid ? &tv : NULL;
  fill(s, x0, y0, w, h, C_INK);
  frame_rect(s, x0, y0, w, h, C_PANEL_HI);
  if (!st || st->phase == PH_WAITING) {
    patch_t *logo = patch(w > 160 ? "SCLOGOM" : "SCLOGOS");
    if (logo && SHORT(logo->height) < h - 10) draw_patch_centered(s, x0 + w / 2, y0 + h / 2 - 4, logo, NULL);
    const char *t = st ? (st->code[0] ? st->code : "SEARCHING...") : "PRESS USE TO PLAY";
    small_text(s, x0 + (w - small_width(t)) / 2, y0 + h - 8, t, (tic / 16) % 2 ? C_GOLD : C_WHITE);
    return;
  }
  char head[64];
  snprintf(head, sizeof head, "%s %d-%d %s", st == &tv ? "LIVE" : "YOU", st->score_me, st->score_opp, st->opp);
  head[(w - 4) / 4] = 0;
  small_text(s, x0 + 2, y0 + 2, head, C_GOLD);
  int cw = (w - 4) / 5, ch = (h - 10) / 3;
  int bx = x0 + (w - cw * 5) / 2, by = y0 + 8;
  for (int sr = 0; sr < 3; sr++)
    for (int sc = 0; sc < 5; sc++) {
      int cell = screen_to_cell(st, sr, sc);
      const bcell_t *b = &st->board[cell];
      int x = bx + sc * cw, y = by + sr * ch;
      /* the bright you/them colours: the wall's light level darkens them in the world */
      fill(s, x, y, cw, ch, b->id ? (b->mine ? C_YOU : C_THEM) : C_PANEL_LO);
      frame_rect(s, x, y, cw, ch, b->id ? (b->mine ? C_YOU_HI : C_THEM_HI) : C_PANEL);
      if (b->id) {
        char name[9];
        snprintf(name, sizeof name, cw >= 30 && ch >= 28 ? "SCS%02d" : "SCT%02d", b->id);
        patch_t *art = patch(name);
        if (art) draw_patch_x(s, x + (cw - SHORT(art->width)) / 2, y + (ch - SHORT(art->height)) / 2, art, b->frozen ? R_ICE : NULL, 1, !b->mine);
      }
    }
}

static void init_walls(void) {
  for (int i = 0; i < NWALLS; i++) {
    screen_tex_t *t = &walls[i];
    t->tex = R_CheckTextureNumForName((char *)t->name);
    if (t->tex < 0 || (textureheight[t->tex] >> FRACBITS) != 128) {
      t->tex = -1;
      continue;
    }
    t->width = texturewidthmask[t->tex] + 1;
    t->base = Z_Malloc(t->width * 128, PU_STATIC, 0);
    t->live = Z_Malloc(t->width * 128, PU_STATIC, 0);
    for (int x = 0; x < t->width; x++) memcpy(t->base + x * 128, R_GetColumn(t->tex, x), 128);
    memcpy(t->live, t->base, t->width * 128);
  }
}

static void update_walls(void) {
  for (int i = 0; i < NWALLS; i++) {
    screen_tex_t *t = &walls[i];
    if (t->tex < 0) continue;
    memcpy(t->live, t->base, t->width * 128);
    surf_t s = {t->live, t->width, 128, 128, 1};
    draw_monitor(&s, t->x, t->y, t->w, t->h);
  }
}

byte *SNC_GetColumn(int tex, int col) {
  if (!inited) return NULL;
  for (int i = 0; i < NWALLS; i++)
    if (walls[i].tex == tex && walls[i].live) return walls[i].live + (col & (walls[i].width - 1)) * 128;
  return NULL;
}

static int is_screen_line(const line_t *l) {
  if (l->sidenum[0] < 0) return 0;
  const side_t *side = &sides[l->sidenum[0]];
  for (int i = 0; i < NWALLS; i++) {
    int t = walls[i].tex;
    if (t >= 0 && (side->midtexture == t || side->toptexture == t || side->bottomtexture == t)) return 1;
  }
  return 0;
}

/* T: step through the level's Smash&Clash screens, teleporting in front of the next one */
static void teleport_to_screen(void) {
  static int next;
  mobj_t *mo = players[consoleplayer].mo;
  if (!mo) return;
  int count = 0;
  for (int i = 0; i < numlines; i++)
    if (is_screen_line(&lines[i]) && fabs(FIXED2DOUBLE(lines[i].dx)) + fabs(FIXED2DOUBLE(lines[i].dy)) >= 64) count++;
  if (!count) {
    hud_message("NO SCREENS ON THIS LEVEL - PRESS C");
    return;
  }
  int want = next++ % count;
  for (int i = 0; i < numlines; i++) {
    line_t *l = &lines[i];
    if (!is_screen_line(l) || fabs(FIXED2DOUBLE(l->dx)) + fabs(FIXED2DOUBLE(l->dy)) < 64 || want--) continue;
    double dx = FIXED2DOUBLE(l->dx), dy = FIXED2DOUBLE(l->dy), len = sqrt(dx * dx + dy * dy);
    double mx = FIXED2DOUBLE(l->v1->x) + dx / 2, my = FIXED2DOUBLE(l->v1->y) + dy / 2;
    double x = mx + dy / len * 176, y = my - dx / len * 176; /* the front side is to the right of v1 -> v2 */
    fixed_t fx = DOUBLE2FIXED(x), fy = DOUBLE2FIXED(y);
    P_SpawnMobj(mo->x, mo->y, mo->z, MT_TFOG);
    if (!P_TeleportMove(mo, fx, fy)) return;
    mo->angle = R_PointToAngle2(fx, fy, DOUBLE2FIXED(mx), DOUBLE2FIXED(my));
    mo->momx = mo->momy = mo->momz = 0;
    players[consoleplayer].viewz = mo->z + players[consoleplayer].viewheight;
    S_StartSound(mo, sfx_telept);
    hud_message("A SMASH&CLASH SCREEN: PRESS USE TO PLAY");
    return;
  }
}

boolean SNC_UseLine(struct mobj_s *user, struct line_s *line) {
  if (!inited || !user || !user->player || line->sidenum[0] < 0) return false;
  side_t *side = &sides[line->sidenum[0]];
  for (int i = 0; i < NWALLS; i++) {
    int t = walls[i].tex;
    if (t >= 0 && (side->midtexture == t || side->toptexture == t || side->bottomtexture == t)) {
      SNC_Open();
      return true;
    }
  }
  return false;
}

/* ---------------------------------------------------------------- set-up and the tick */

static void init(void) {
  playpal = W_CacheLumpName("PLAYPAL", PU_STATIC);
  screen = (surf_t){I_VideoBuffer, SCREENWIDTH, SCREENHEIGHT, SCREENWIDTH, 0};
  C_BLACK = nearest(0, 0, 0);
  C_WHITE = nearest(255, 255, 255);
  C_INK = nearest(12, 16, 40);
  C_YOU = hexcolor("2173e8");
  C_YOU_HI = hexcolor("5fa6ff");
  C_YOU_DK = hexcolor("134a9f");
  C_THEM = hexcolor("f4581f");
  C_THEM_HI = hexcolor("ff9a62");
  C_THEM_DK = hexcolor("a8380c");
  C_PANEL = nearest(52, 52, 60);
  C_PANEL_HI = nearest(130, 130, 140);
  C_PANEL_LO = nearest(24, 24, 30);
  C_GOLD = nearest(255, 222, 89);
  C_LEGAL = nearest(126, 217, 87);
  C_ICE = nearest(170, 220, 255);
  C_GRAPE = nearest(150, 70, 220);
  C_BADGE[0] = hexcolor("FD5656");
  C_BADGE[1] = hexcolor("FFDE59");
  C_BADGE[2] = hexcolor("5271FF");
  C_BADGE[3] = hexcolor("7ED957");
  make_remap(R_WHITE, 255, 255, 255);
  make_remap(R_GOLD, 255, 222, 89);
  make_remap(R_YOU, 95, 166, 255);
  make_remap(R_THEM, 255, 154, 98);
  make_remap(R_GRAY, 160, 160, 170);
  make_remap(R_GREEN, 126, 217, 87);
  make_tint(R_ICE, 170, 220, 255, 90);
  const byte *cmap = W_CacheLumpName("COLORMAP", PU_STATIC);
  memcpy(R_DARK, cmap + 256 * 22, 256);
  C_DIM = R_DARK[C_PANEL];
  init_walls();
  inited = 1;
  send("ready");
}

void SNC_Ticker(void) {
  if (!inited) {
    if (gamestate == GS_LEVEL || gamestate == GS_DEMOSCREEN) init();
    else return;
  }
  tic++;
  /* like a help screen: when the terminal closes, D_Display sees it go and redraws the status bar in full */
  if (active) inhelpscreens = true;
  while (qcount && tic >= hold_until) {
    apply_frame(&queue[qhead]);
    qhead = (qhead + 1) % QLEN;
    qcount--;
  }
  if (tic % 4 == 0) update_walls();
}
