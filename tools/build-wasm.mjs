// Builds DOOM (doomgeneric, GPL-2.0) with the Smash&Clash client into web/doom.js and web/doom.wasm.
//   node tools/build-wasm.mjs        (needs Emscripten: emcc on PATH, or EMSDK set)
//
// The engine is fetched at a pinned commit and changed in a handful of places, each a small hook into engine/snc.c:
// the event chain, the display, the main menu, the USE line check and the wall-texture column fetch.

import { execFileSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { cp, mkdir, readFile, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('..', import.meta.url));
const DOOMGENERIC = { repo: 'https://github.com/ozkl/doomgeneric.git', commit: 'dcb7a8dbc7a16ce3dda29382ac9aae9d77d21284' };

const SOURCES = `dummy am_map doomdef doomstat dstrings d_event d_items d_iwad d_loop d_main d_mode d_net f_finale f_wipe
  g_game hu_lib hu_stuff info i_cdmus i_endoom i_joystick i_scale i_sound i_system i_timer memio m_argv m_bbox m_cheat
  m_config m_controls m_fixed m_menu m_misc m_random p_ceilng p_doors p_enemy p_floor p_inter p_lights p_map p_maputl
  p_mobj p_plats p_pspr p_saveg p_setup p_sight p_spec p_switch p_telept p_tick p_user r_bsp r_data r_draw r_main
  r_plane r_segs r_sky r_things sha1 sounds statdump st_lib st_stuff s_sound tables v_video wi_stuff w_checksum w_file
  w_main w_wad z_zone i_input i_video doomgeneric w_file_stdc`.split(/\s+/);

// [file, find, replace]: every find must be in the engine exactly once.
const PATCHES = [
  ['d_main.c', '\tif (M_Responder (ev))', '\tif (SNC_Responder (ev))\n\t    continue;               // the Smash&Clash terminal ate it\n\tif (M_Responder (ev))'],
  ['d_main.c', '    M_Drawer ();          // menu is drawn even on top of everything', '    SNC_Drawer ();        // the Smash&Clash terminal\n    M_Drawer ();          // menu is drawn even on top of everything'],
  ['d_main.c', '    if (paused)\n    {\n\t\tif (automapactive)', '    if (paused && !SNC_Active())\n    {\n\t\tif (automapactive)'],
  // the melt waits in a loop the browser can't draw from: skip it
  ['d_main.c', '\t\twipe = true;\n', '\t\twipe = false;\n'],
  ['m_menu.c', '    newgame = 0,\n', '    newgame = 0,\n    smashclash,\n'],
  ['m_menu.c', '    {1,"M_NGAME",M_NewGame,\'n\'},\n', '    {1,"M_NGAME",M_NewGame,\'n\'},\n    {1,"M_SNC",M_SmashClash,\'c\'},\n'],
  ['m_menu.c', 'void M_ClearMenus (void);\n', 'void M_ClearMenus (void);\nvoid M_SmashClash(int choice);\n'],
  ['p_map.c', 'boolean\tPTR_UseTraverse (intercept_t* in)\n{\n    int\t\tside;\n', 'boolean\tPTR_UseTraverse (intercept_t* in)\n{\n    int\t\tside;\n\n    if (SNC_UseLine (usething, in->d.line))\n\treturn false;               // a computer: the Smash&Clash terminal\n'],
  ['r_data.c', '    col &= texturewidthmask[tex];\n', '    {\n\tbyte *snc = SNC_GetColumn (tex, col);   // the live board on the computer screens\n\tif (snc)\n\t    return snc;\n    }\n    col &= texturewidthmask[tex];\n'],
];
const APPEND = {
  'm_menu.c': '\n// The Smash&Clash terminal, from the main menu\nvoid M_SmashClash(int choice)\n{\n    M_ClearMenus ();\n    SNC_Open ();\n}\n',
};

function git(args, cwd) {
  execFileSync('git', args, { cwd, stdio: 'inherit' });
}

const cache = join(root, 'build', 'doomgeneric');
if (!existsSync(join(cache, '.git'))) {
  await mkdir(cache, { recursive: true });
  git(['init', '-q'], cache);
  git(['remote', 'add', 'origin', DOOMGENERIC.repo], cache);
}
git(['fetch', '-q', '--depth', '1', 'origin', DOOMGENERIC.commit], cache);
git(['checkout', '-q', '--force', DOOMGENERIC.commit], cache);

// a patched copy, so the checkout stays pristine
const src = join(root, 'build', 'engine');
await cp(join(cache, 'doomgeneric'), src, { recursive: true, force: true });
const touched = new Set();
for (const [file, find, replace] of PATCHES) {
  const path = join(src, file);
  const text = (await readFile(path, 'utf8')).replace(/\r\n/g, '\n');
  const at = text.indexOf(find);
  if (at < 0 || text.indexOf(find, at + 1) >= 0) throw new Error(`patch for ${file} does not apply: ${JSON.stringify(find)}`);
  let out = text.slice(0, at) + replace + text.slice(at + find.length);
  if (!touched.has(file)) out = '#include "snc.h"\n' + out;
  touched.add(file);
  await writeFile(path, out);
}
for (const [file, text] of Object.entries(APPEND)) await writeFile(join(src, file), (await readFile(join(src, file), 'utf8')) + text);

const emsdk = process.env.EMSDK;
const emcc = emsdk ? join(emsdk, 'upstream', 'emscripten', process.platform === 'win32' ? 'emcc.exe' : 'emcc') : 'emcc';
const env = { ...process.env };
if (emsdk && !env.EM_CONFIG) env.EM_CONFIG = join(emsdk, '.emscripten');

const out = join(root, 'web');
const args = [
  ...SOURCES.filter((s) => s !== 'w_file_stdc').map((s) => join(src, s + '.c')),
  join(src, 'w_file_stdc.c'),
  join(root, 'engine', 'platform.c'),
  join(root, 'engine', 'snc.c'),
  '-I', src, '-I', join(root, 'engine'), '-I', join(root, 'engine', 'compat'),
  '-DFEATURE_SOUND', '-DDOOMGENERIC_RESX=320', '-DDOOMGENERIC_RESY=200',
  '-O2', '-w',
  '-sMODULARIZE', '-sEXPORT_ES6', '-sEXPORT_NAME=createDoom', '-sENVIRONMENT=web',
  '-sINVOKE_RUN=0', '-sEXIT_RUNTIME=0',
  '-sINITIAL_MEMORY=64MB', '-sALLOW_MEMORY_GROWTH=1', '-sSTACK_SIZE=1MB',
  '-sEXPORTED_FUNCTIONS=_main,_snc_push,_snc_pointer,_dg_key,_dg_mouse,_malloc,_free',
  '-sEXPORTED_RUNTIME_METHODS=callMain,FS,HEAPU8,UTF8ToString,stringToNewUTF8',
  '-o', join(out, 'doom.js'),
];
console.log(`compiling ${args.filter((a) => a.endsWith('.c')).length} files with ${emcc}`);
try {
  execFileSync(emcc, args, { stdio: 'inherit', env });
} catch {
  console.error('emcc failed');
  process.exit(1);
}
console.log('wrote web/doom.js and web/doom.wasm');
