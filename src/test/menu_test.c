/* Offline test of the F7 menu model (navigation, values) and the overlay drawing (writes a preview). */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include "../sor4common.h"
#include "../sor4menu.h"
Config C;
void logf_(int lv, const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n'); }
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
int main(int argc, char **argv) {
    MenuModel m; memset(&m, 0, sizeof m);
    for (int i = 0; i < 3; i++) snprintf(m.bot[i].character, sizeof m.bot[i].character, "Auto");
    static const char *r[] = { "Axel", "Blaze", "Cherry", "Floyd", "Adam" }; int pal[] = { 6, 6, 5, 4, 3 };
    m.nroster = 5; for (int i = 0; i < 5; i++) { strcpy(m.roster[i], r[i]); m.palettes[i] = pal[i]; }
    /* defaults: everything OFF, cursor on P2 bot */
    CHECK(!m.bot[0].on && !m.bot[1].on && !m.bot[2].on && m.cursor == 0, "defaults");
    menu_nav(&m, MK_OK);                         CHECK(m.bot[0].on == 1 && m.changed, "Enter toggles P2 bot on");
    menu_nav(&m, MK_LEFT);                       CHECK(m.bot[0].on == 0, "Left toggles it back off");
    menu_nav(&m, MK_RIGHT);                      CHECK(m.bot[0].on == 1, "Right toggles on");
    menu_nav(&m, MK_DOWN);                       CHECK(m.cursor == 1, "down to Character");
    menu_nav(&m, MK_RIGHT);                      CHECK(!strcmp(m.bot[0].character, "Axel"), "Auto -> Axel (%s)", m.bot[0].character);
    for (int i = 0; i < 3; i++) menu_nav(&m, MK_RIGHT);
    CHECK(!strcmp(m.bot[0].character, "Floyd"), "-> Floyd (%s)", m.bot[0].character);
    menu_nav(&m, MK_DOWN);                       CHECK(m.cursor == 2, "down to Color");
    menu_nav(&m, MK_LEFT);                       CHECK(m.bot[0].color == 4, "Auto <- wraps to the last color of Floyd (4): %d", m.bot[0].color);
    menu_nav(&m, MK_RIGHT);                      CHECK(m.bot[0].color == 0, "4 -> Auto");
    menu_nav(&m, MK_RIGHT); menu_nav(&m, MK_RIGHT); menu_nav(&m, MK_RIGHT);
    CHECK(m.bot[0].color == 3, "color 3: %d", m.bot[0].color);
    menu_nav(&m, MK_UP); menu_nav(&m, MK_RIGHT); CHECK(!strcmp(m.bot[0].character, "Adam") && m.bot[0].color == 3, "Adam keeps color 3 (3 colors)");
    menu_nav(&m, MK_RIGHT); CHECK(!strcmp(m.bot[0].character, "Auto"), "Adam -> Auto (%s)", m.bot[0].character);
    for (int i = 0; i < 4; i++) menu_nav(&m, MK_RIGHT);
    menu_nav(&m, MK_UP); menu_nav(&m, MK_UP);    CHECK(m.cursor == MROW_CLOSE, "wrap up to Close: %d", m.cursor);
    int closed = menu_nav(&m, MK_OK);            CHECK(closed, "Enter on Close closes");
    CHECK(menu_nav(&m, MK_CLOSE), "Esc closes");
    m.cursor = 1;
    /* text */
    snprintf(m.where, sizeof m.where, "character select"); snprintf(m.note[0], sizeof m.note[0], "(playing)");
    char lines[32][96]; int col[32]; int n = menu_lines(&m, lines, col, 32);
    for (int i = 0; i < n; i++) printf("  [%c] %s\n", "TtSg"[col[i]], lines[i]);
    CHECK(n == 19 && !strcmp(lines[0], "RAGE BOTS v1.1") && col[0] == MC_TITLE && !strcmp(lines[1], "By: MrRobotoDomoArigato") && strstr(lines[2], "F7 close")
          && !strcmp(lines[3], "Where: character select"), "header (v1.1: RAGE BOTS v1.1 / By: MrRobotoDomoArigato)");
    CHECK(strstr(lines[5], "P2 bot: ON") && col[6] == MC_SEL && strstr(lines[6], "> Character: Floyd") != NULL, "selected row");
    CHECK(col[10] == MC_GREY && col[11] == MC_GREY, "sub-rows of an OFF bot are grey");
    for (int i = 0; i < n; i++) CHECK(!strstr(lines[i], "alette"), "the word Color, never Palette");
    /* ---- m7: sequential locking and cascade OFF ---- */
    {
        MenuModel k; memset(&k, 0, sizeof k);
        for (int i = 0; i < 3; i++) snprintf(k.bot[i].character, sizeof k.bot[i].character, "Auto");
        k.nroster = 5; for (int i = 0; i < 5; i++) { strcpy(k.roster[i], r[i]); k.palettes[i] = pal[i]; }
        int j0[3] = { 0, 0, 0 }, j1[3] = { 1, 0, 0 }, j2[3] = { 1, 1, 0 };
        menu_update_locks(&k, j0);
        CHECK(!k.locked[0] && k.locked[1] && k.locked[2] && strstr(k.lockWhy[1], "turn on P2 bot first") && strstr(k.lockWhy[2], "turn on P3 bot first"), "all OFF: P3, P4 locked");
        menu_nav(&k, MK_DOWN); menu_nav(&k, MK_DOWN); menu_nav(&k, MK_DOWN);
        CHECK(k.cursor == MROW_SKILL, "down skips the locked P3/P4 rows to Bot skill (v1.1): %d", k.cursor);
        menu_nav(&k, MK_DOWN); CHECK(k.cursor == MROW_CLOSE, "Bot skill -> Close: %d", k.cursor);
        menu_nav(&k, MK_DOWN); CHECK(k.cursor == 0, "Close -> P2 bot: %d", k.cursor);
        menu_nav(&k, MK_UP); menu_nav(&k, MK_UP); menu_nav(&k, MK_UP); CHECK(k.cursor == 2, "up from Close over Bot skill skips locked rows to P2 Color: %d", k.cursor);
        k.cursor = 3; menu_nav(&k, MK_OK); CHECK(!k.bot[1].on, "a locked bot can't be turned on");
        k.cursor = 0; menu_nav(&k, MK_OK); CHECK(k.bot[0].on, "P2 on");
        menu_update_locks(&k, j0);
        CHECK(k.locked[1] && strstr(k.lockWhy[1], "P2 bot must join first"), "P2 ON but not joined: P3 still locked (%s)", k.lockWhy[1]);
        menu_update_locks(&k, j1);
        CHECK(!k.locked[1] && k.locked[2], "P2 joined: P3 unlocked, P4 locked");
        menu_nav(&k, MK_DOWN); menu_nav(&k, MK_DOWN); menu_nav(&k, MK_DOWN); CHECK(k.cursor == 3, "P3 row reachable: %d", k.cursor);
        menu_nav(&k, MK_OK); CHECK(k.bot[1].on, "P3 on");
        menu_nav(&k, MK_DOWN); menu_nav(&k, MK_DOWN); menu_nav(&k, MK_DOWN); CHECK(k.cursor == MROW_SKILL, "P4 rows skipped (P3 not joined): %d", k.cursor);
        menu_update_locks(&k, j2); k.cursor = 6; menu_nav(&k, MK_OK); CHECK(k.bot[2].on, "P3 joined: P4 on");
        /* the chosen rule: turning a bot OFF turns the later ones OFF too (cascade) */
        k.cursor = 0; menu_nav(&k, MK_OK);
        CHECK(!k.bot[0].on && !k.bot[1].on && !k.bot[2].on && k.cascaded == 6, "P2 OFF cascades P3+P4 OFF (bits %d)", k.cascaded);
        menu_update_locks(&k, j0); CHECK(k.locked[1] && k.locked[2], "...and they are locked again");
        k.bot[0].on = 1; menu_update_locks(&k, j2); k.cursor = 3; menu_nav(&k, MK_OK); menu_update_locks(&k, j2); k.cursor = 6; menu_nav(&k, MK_OK);
        k.cursor = 3; menu_nav(&k, MK_LEFT); CHECK(k.bot[0].on && !k.bot[1].on && !k.bot[2].on && k.cascaded == 4, "P3 OFF cascades only P4");
        k.cursor = 6; menu_update_locks(&k, j1); CHECK(k.cursor == 3, "selected row got locked: cursor back to P3 bot (%d)", k.cursor);
        k.bot[0].on = 0; k.bot[1].on = 1; menu_update_locks(&k, j0); CHECK(!k.locked[1], "an ON bot is never locked");
        k.bot[1].on = 0; k.bot[0].on = 1;
        /* drawing: locked row grey with the reason, Slots line, live line */
        snprintf(k.where, sizeof k.where, "character select"); snprintf(k.slots, sizeof k.slots, "P1 human  P2 bot  P3 free  P4 free");
        snprintf(k.live[0], sizeof k.live[0], "cursor Axel -> Floyd");
        menu_update_locks(&k, j0); k.cursor = 0;
        char ln[32][96]; int cl[32]; int nn = menu_lines(&k, ln, cl, 32);
        for (int i = 0; i < nn; i++) printf("  [%c] %s\n", "TtSg"[cl[i]], ln[i]);
        CHECK(nn == 21 && !strcmp(ln[4], "Slots: P1 human  P2 bot  P3 free  P4 free") && strstr(ln[9], "Game: cursor Axel -> Floyd"), "slots + live lines");
        { int W2, H2; uint32_t *p2 = overlay_compose(ln, cl, nn, 1080, &W2, &H2); CHECK(p2 && H2 <= 1080 * 9 / 10 + 4, "m7 panel (%d lines) fits a 1080p game: %dx%d", nn, W2, H2); free(p2);
          p2 = overlay_compose(ln, cl, nn, 720, &W2, &H2); CHECK(p2 && H2 <= 720 * 9 / 10 + 4, "fits 720p: %dx%d", W2, H2); printf("m7 panel %d lines: 720p -> %dx%d\n", nn, W2, H2); free(p2); }
        CHECK(strstr(ln[8], "Color: Default") != NULL, "m7.1: color Auto is now shown as Default (%s)", ln[8]);
        for (int i = 0; i < nn; i++) CHECK(!strstr(ln[i], "Color: Auto"), "no \"Color: Auto\" left: %s", ln[i]);
        CHECK(!strcmp(ln[nn - 2], "  Bot skill: Easy") && !strcmp(ln[nn - 1], "  Close"), "v1.1: Bot skill row before Close (%s)", ln[nn - 2]);
        CHECK(strstr(ln[11], "P3 bot: OFF") && strstr(ln[11], "locked: P2 bot must join first") && cl[11] == MC_GREY, "locked P3 row grey with reason: %s", ln[11]);
    }
    /* v1.1: Bot skill row (Easy default, Enter / Left / Right toggle, never locked, nothing else changes) */
    {
        MenuModel q; memset(&q, 0, sizeof q);
        for (int i = 0; i < 3; i++) snprintf(q.bot[i].character, sizeof q.bot[i].character, "Auto");
        int j0[3] = { 0, 0, 0 }; menu_update_locks(&q, j0);
        CHECK(q.skill == 0, "skill defaults to Easy");
        q.cursor = MROW_SKILL; q.changed = 0;
        int cl0 = menu_nav(&q, MK_OK); CHECK(!cl0 && q.skill == 1 && q.changed, "Enter on Bot skill: Normal, menu stays open");
        menu_nav(&q, MK_RIGHT); CHECK(q.skill == 0, "Right: back to Easy");
        menu_nav(&q, MK_LEFT); CHECK(q.skill == 1, "Left: Normal");
        CHECK(!q.bot[0].on && !q.bot[1].on && !q.bot[2].on && !strcmp(q.bot[0].character, "Auto") && q.bot[0].color == 0, "the skill row changes no bot");
        char l2[32][96]; int c2[32]; int n2 = menu_lines(&q, l2, c2, 32);
        CHECK(!strcmp(l2[n2 - 2], "> Bot skill: Normal") && c2[n2 - 2] == MC_SEL, "selected skill row text: %s", l2[n2 - 2]);
        q.cursor = MROW_CLOSE; CHECK(menu_nav(&q, MK_OK) && q.skill == 1, "Close keeps the skill");
        printf("v1.1 skill row: %s\n", fails ? "FAIL" : "OK");
    }
    /* drawing (Wine's GDI): premultiplied alpha, translucent panel, opaque border */
    int W, H; uint32_t *px = overlay_compose(lines, col, n, 1080, &W, &H);
    CHECK(px && W > 200 && H > 300, "compose %dx%d", W, H);
    if (px) {
        uint32_t corner = px[0], inside = px[(H / 2) * W + W - 6];
        int bad = 0, textPx = 0;
        for (int i = 0; i < W * H; i++) { uint32_t v = px[i]; int a = v >> 24; if (((v >> 16) & 255) > a || ((v >> 8) & 255) > a || (v & 255) > a) bad++; if (a > 200) textPx++; }
        printf("compose: %dx%d, border alpha %u, panel alpha %u (%.0f%%), text/border pixels %d, non-premultiplied %d\n", W, H, corner >> 24, inside >> 24, (inside >> 24) / 2.55, textPx, bad);
        CHECK((inside >> 24) > 100 && (inside >> 24) < 200 && (corner >> 24) > 200 && !bad && textPx > 1000, "alpha");
        FILE *f = fopen(argc > 1 ? argv[1] : "menu_preview.bgra", "wb");
        if (f) { fwrite(&W, 4, 1, f); fwrite(&H, 4, 1, f); fwrite(px, 4, (size_t)W * H, f); fclose(f); }
        free(px);
    }
    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
