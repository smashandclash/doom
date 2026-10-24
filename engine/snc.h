/* The Smash&Clash client inside DOOM: the hooks the engine calls (see tools/build-wasm.mjs for where they go). */
#ifndef SNC_H
#define SNC_H

#include "doomtype.h"
#include "d_event.h"

struct line_s;
struct mobj_s;

void SNC_Open(void);
boolean SNC_Active(void);
boolean SNC_Responder(event_t *ev);          /* before the menu: eats input while the terminal is open */
void SNC_Ticker(void);                       /* once per game tic */
void SNC_Drawer(void);                       /* after the view, before the menu */
boolean SNC_UseLine(struct mobj_s *user, struct line_s *line); /* USE on a computer wall opens the terminal */
byte *SNC_GetColumn(int tex, int col);       /* live board on the computer textures, or NULL */
int DG_TypedChar(int typed);                 /* platform.c: the letter actually typed for the last key down */

#endif
