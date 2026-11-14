/* Rage Bots (formerly SoR4Bots) F7 menu: model (rows, navigation) and the translucent overlay window that draws it. */
#pragma once
#include <stdint.h>
#include "sor4state.h"

#define MENU_BOTS 3
typedef struct { int on; char character[32]; int color; } BotChoice;      /* color 0 = Auto, else 1..N */

typedef struct {
    BotChoice bot[MENU_BOTS];       /* P2, P3, P4 bots */
    char where[64];                 /* "character select", "in a stage", ... */
    char note[MENU_BOTS][40];       /* shown after "Pn bot: ON", e.g. "playing as P3" (empty = nothing) */
    int nroster; char roster[ROSTER_MAX][32]; int palettes[ROSTER_MAX];
    int cursor;                     /* selected row */
    int changed;                    /* a choice changed since the menu opened (save on close) */
    /* m7 */
    char slots[64];                 /* "P1 human  P2 bot  P3 free  P4 free" ("" = not readable) */
    char live[MENU_BOTS][56];       /* what the game shows for this bot at character select ("" = nothing) */
    int locked[MENU_BOTS];          /* m7 sequential locking: this OFF bot can't be turned on yet (rows skipped, grey) */
    char lockWhy[MENU_BOTS][40];
    int cascaded;                   /* last change turned later bots off too (bit per bot), for the log */
    int skill;                      /* v1.1: bot skill for all bots, 0 = Easy, 1 = Normal (internal: Mania) (row MROW_SKILL) */
} MenuModel;

enum { MROW_PER_BOT = 3, MROW_SKILL = MENU_BOTS * MROW_PER_BOT, MROW_CLOSE, MROW_COUNT };   /* v1.1: Bot skill row before Close */
enum { MK_NONE, MK_UP, MK_DOWN, MK_LEFT, MK_RIGHT, MK_OK, MK_CLOSE };

/* model (pure logic, unit-tested) */
int  menu_nav(MenuModel *m, int key);            /* returns 1 if the menu should close */
int  menu_lines(const MenuModel *m, char lines[][96], int colors[], int max);   /* text to draw */
int  menu_color_count(const MenuModel *m, int bot);
/* m7: P3 needs P2 ON and joined, P4 needs P3 ON and joined (joined[i]: bot i has a player slot).
   Only an OFF bot is ever locked. Call before menu_nav and before drawing. */
void menu_update_locks(MenuModel *m, const int joined[MENU_BOTS]);
/* m7: P(n) OFF turns every later bot off too ("cascade"); returns the bits of bots it turned off */
int  menu_cascade_off(MenuModel *m, int bot);
enum { MC_TITLE, MC_TEXT, MC_SEL, MC_GREY };

/* overlay window (own thread; translucent layered window, never takes focus or clicks) */
void overlay_show(void *gameHwnd, const MenuModel *m);   /* show or redraw */
void overlay_hide(void);
void overlay_shutdown(void);
/* the panel as premultiplied BGRA pixels (malloc'd); used by the window and by the offline test */
uint32_t *overlay_compose(char lines[][96], const int *colors, int n, int gameH, int *outW, int *outH);
