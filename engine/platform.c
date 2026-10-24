/* The browser platform for doomgeneric (https://github.com/ozkl/doomgeneric): frames to a canvas, keys and the mouse
 * from the page, sound effects through Web Audio, the WADs from the page's in-memory file system. Single-threaded: the
 * page's requestAnimationFrame drives one engine tick per game tic.
 *
 * GPL-2.0, like the engine.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <emscripten.h>

#include "d_event.h"
#include "doomgeneric.h"
#include "doomkeys.h"
#include "i_sound.h"
#include "i_timer.h"
#include "m_misc.h"
#include "w_wad.h"
#include "z_zone.h"

#include "snc.h"

/* ---- frames ---- */

EM_JS(void, js_frame, (const uint32_t *pixels, int w, int h), {
  Module.drawFrame(HEAPU8.subarray(pixels, pixels + w * h * 4), w, h);
});

void DG_Init(void) {}

void DG_DrawFrame(void) {
  js_frame(DG_ScreenBuffer, DOOMGENERIC_RESX, DOOMGENERIC_RESY);
}

void DG_SleepMs(uint32_t ms) {
  (void)ms; /* single-threaded: the engine's few short waits spin on the clock instead */
}

uint32_t DG_GetTicksMs(void) {
  static double start = -1;
  double now = emscripten_get_now();
  if (start < 0) start = now;
  return (uint32_t)(now - start);
}

void DG_SetWindowTitle(const char *title) {
  (void)title;
}

/* ---- input: the page posts DOOM key codes; WASD move unless the Smash&Clash terminal is open (then they type) ---- */

#define KEYQ 128
static uint16_t keyq[KEYQ];
static unsigned keyq_read, keyq_write;

EMSCRIPTEN_KEEPALIVE void dg_key(int pressed, int key) {
  if (key <= 0 || key > 255) return;
  keyq[keyq_write++ % KEYQ] = (uint16_t)((pressed ? 1 << 8 : 0) | key);
}

EMSCRIPTEN_KEEPALIVE void dg_mouse(int dx, int buttons) {
  event_t ev = {ev_mouse, buttons, dx * 4, 0, 0};
  D_PostEvent(&ev);
}

static unsigned char held_as[256]; /* what each raw key went down as, so it comes up the same */
static int typed_raw;              /* the raw letter of the last key down, when it was turned into a movement key */

int DG_TypedChar(int typed) {
  return typed_raw ? typed_raw : typed;
}

int DG_GetKey(int *pressed, unsigned char *key) {
  if (keyq_read == keyq_write) return 0;
  uint16_t k = keyq[keyq_read++ % KEYQ];
  int raw = k & 0xff;
  *pressed = k >> 8;
  if (*pressed) {
    unsigned char as = (unsigned char)raw;
    if (!SNC_Active()) {
      switch (raw) {
        case 'w': as = KEY_UPARROW; break;
        case 's': as = KEY_DOWNARROW; break;
        case 'a': as = KEY_STRAFE_L; break;
        case 'd': as = KEY_STRAFE_R; break;
        case 'e': as = KEY_USE; break;
      }
    }
    held_as[raw] = as;
    typed_raw = as != raw ? raw : 0;
    *key = as;
  } else {
    *key = held_as[raw] ? held_as[raw] : (unsigned char)raw;
  }
  return 1;
}

/* ---- sound effects: each DOOM sound lump becomes a Web Audio buffer the first time it plays ---- */

int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

EM_JS(int, js_snd_start, (int ch, const uint8_t *samples, int len, int rate, int vol, int sep), {
  return Module.sound ? Module.sound.start(ch, samples, len, rate, vol, sep) : -1;
});
EM_JS(void, js_snd_params, (int ch, int vol, int sep), { if (Module.sound) Module.sound.params(ch, vol, sep); });
EM_JS(void, js_snd_stop, (int ch), { if (Module.sound) Module.sound.stop(ch); });
EM_JS(int, js_snd_playing, (int ch), { return Module.sound ? Module.sound.playing(ch) : 0; });

static boolean use_prefix;

static boolean snd_init(boolean prefix) {
  use_prefix = prefix;
  return true;
}

static void snd_shutdown(void) {}

static int snd_lump(sfxinfo_t *sfx) {
  char name[9];
  if (sfx->link) sfx = sfx->link;
  M_snprintf(name, sizeof name, use_prefix ? "ds%s" : "%s", sfx->name);
  return W_CheckNumForName(name);
}

static void snd_update(void) {}

static void snd_params(int ch, int vol, int sep) {
  js_snd_params(ch, vol, sep);
}

/* A sound lump: [u16 format 3][u16 rate][u32 samples] then 8-bit unsigned PCM, padded 16 samples at each end. */
static int snd_start(sfxinfo_t *sfx, int ch, int vol, int sep) {
  if (sfx->lumpnum < 0) return -1;
  const uint8_t *data = W_CacheLumpNum(sfx->lumpnum, PU_STATIC);
  uint32_t lumplen = W_LumpLength(sfx->lumpnum);
  if (lumplen < 8 || data[0] != 3 || data[1] != 0) return -1;
  int rate = data[2] | (data[3] << 8);
  uint32_t len = data[4] | (data[5] << 8) | (data[6] << 16) | ((uint32_t)data[7] << 24);
  if (len > lumplen - 8) len = lumplen - 8;
  const uint8_t *s = data + 8;
  if (len > 32) s += 16, len -= 32;
  return js_snd_start(ch, s, (int)len, rate ? rate : 11025, vol, sep);
}

static void snd_stop(int ch) {
  js_snd_stop(ch);
}

static boolean snd_playing(int ch) {
  return js_snd_playing(ch) != 0;
}

static void snd_cache(sfxinfo_t *sounds, int num) {
  (void)sounds;
  (void)num;
}

static snddevice_t sound_devices[] = {SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32};

sound_module_t DG_sound_module = {
  sound_devices, sizeof sound_devices / sizeof sound_devices[0],
  snd_init, snd_shutdown, snd_lump, snd_update, snd_params, snd_start, snd_stop, snd_playing, snd_cache,
};

/* No music: DOOM's is MIDI. The page plays Smash&Clash's own music during a match instead. */
static boolean mus_init(void) { return false; }
static void mus_shutdown(void) {}
static void mus_volume(int volume) { (void)volume; }
static void mus_pause(void) {}
static void mus_resume(void) {}
static void *mus_register(void *data, int len) { (void)data; (void)len; return NULL; }
static void mus_unregister(void *handle) { (void)handle; }
static void mus_play(void *handle, boolean looping) { (void)handle; (void)looping; }
static void mus_stop(void) {}
static boolean mus_playing(void) { return false; }
static void mus_poll(void) {}

music_module_t DG_music_module = {
  NULL, 0, mus_init, mus_shutdown, mus_volume, mus_pause, mus_resume, mus_register, mus_unregister, mus_play, mus_stop, mus_playing, mus_poll,
};

/* ---- the loop: one engine tick per new game tic ---- */

/* ?timer=1: drive the loop from setTimeout instead of requestAnimationFrame, which a hidden page doesn't get */
EM_JS(int, js_use_timer, (void), { return /[?&]timer=1/.test(location.search) ? 1 : 0; });

static void frame(void) {
  static int last = -1;
  int now = I_GetTime();
  if (now == last) return;
  last = now;
  doomgeneric_Tick();
  SNC_Ticker();
}

int main(int argc, char **argv) {
  doomgeneric_Create(argc, argv);
  if (js_use_timer()) {
    emscripten_set_main_loop(frame, 0, 0);
    emscripten_set_main_loop_timing(EM_TIMING_SETTIMEOUT, 5);
    emscripten_exit_with_live_runtime();
  }
  emscripten_set_main_loop(frame, 0, 1);
  return 0;
}
