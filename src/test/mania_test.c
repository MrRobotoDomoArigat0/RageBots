/* v1.1 (m8) offline test of the Bot skill setting (Easy = v1.0 brain, Normal = sor4mania.inc, named Mania in the code).
 * Build: mania_test.c ../sor4brain.c ../sor4state.c ../sor4meta.c */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <stdlib.h>
#include "../sor4common.h"
#include "../sor4state.h"
#include "../sor4brain.h"
Config C;
static int g_quiet = 1;
void logf_(int lv, const char *fmt, ...) {
    if (lv > C.logLevel || g_quiet) return;
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n');
}
int dbg_trap_zone_count(void); void dbg_trap_reset(void); void dbg_mania_pipes_reset(void);
void dbg_surv_reset(void); void dbg_floor_set_wall(int hi, float z, float x);
static World w; static BotBrain bb[3]; static PadOut out; static uint64_t now = 1000000;
static int fails;
#define CHECK(cond, ...) do { int ok_ = !!(cond); printf("%s: ", ok_ ? "OK  " : "FAIL"); printf(__VA_ARGS__); putchar('\n'); if (!ok_) fails++; } while (0)
/* fighters: [0] P1 human, [1] our bot (P2), enemies after */
static Chr *mk(int i, uint32_t uid, int team, int player, float x, float z, int facing, int state) {
    Chr *c = &w.ch[i]; memset(c, 0, sizeof *c);
    c->uid = uid; c->team = team; c->player = player; c->life = 100; c->maxLife = 100; c->pos.x = x; c->pos.z = z;
    c->facing = facing; c->state = state; c->ctrlKind = player >= 0 ? CTRL_PLAYER : CTRL_AI;
    c->entered = 1; c->plane = 0; c->collidable = 1; c->btRoot = c->lineOfSight = -1;
    if (i >= w.nch) w.nch = i + 1;
    return c;
}
static void world_reset(void) {
    memset(&w, 0, sizeof w);
    w.ok = 1; w.live = 1; w.source = "test"; w.frames = 100; w.haveCam = 1; w.camSizeY = 10.8f; w.cam.x = 0;
    w.nslot = 4; for (int i = 0; i < 4; i++) w.slot[i].ctrl = -1;
    w.slot[0].ctrl = 0; w.slot[0].type = 1; w.slot[1].ctrl = 1; w.slot[1].type = 1;
    w.nps = 4; for (int i = 0; i < 4; i++) { w.ps[i].stars = 0; w.ps[i].banked = 0; w.ps[i].starBtn = 1; w.ps[i].starYB = 1; }
    w.noGsWhy = "";
    mk(0, 1, 0, 0, -3, 1.5f, 1, CS_STANDWALK);
    mk(1, 2, 0, 1, 0, 1.5f, 1, CS_STANDWALK);
    brain_floor_reset(); brain_retal_reset(); brain_parts_reset();   /* m7.4 */
    dbg_trap_reset(); dbg_mania_pipes_reset();   /* v1.1 stage 4: shared trap zones and pipe ages */
}
static void bot_reset(int playable) {
    memset(bb, 0, sizeof bb);
    for (int i = 0; i < 3; i++) { char n[32]; snprintf(n, sizeof n, "Bot%d", i + 2); brain_init_detached(&bb[i], i, n); }
    BotBrain *b = &bb[0];
    brain_reset(b, 0, 3, 2, "Bot2", now);
    b->id.ctrlId = 1; b->id.verifiedSlot = 1; b->id.preKnown = 1; b->id.pre[0] = 0; b->id.npre = 1; b->id.attachAt = now - 60000; b->id.lastSeen = now;
    b->sawLevel = 1; b->xSign = 1; b->introDone = 1;
    w.ps[1].playable = playable;
}
typedef struct { int frames, btn[BTN_COUNT], rt, stickL, stickR, stickD, neutralFrames, firstBtn[BTN_COUNT]; int yWithStick; int rev, lastSx; } Tally;
static void run(int frames, Tally *t, void (*each)(int fr)) {
    memset(t, 0, sizeof *t); for (int k = 0; k < BTN_COUNT; k++) t->firstBtn[k] = -1;
    int last[BTN_COUNT] = { 0 }, lastRt = 0;
    for (int fr = 0; fr < frames; fr++, now += 16) {
        if (each) each(fr);
        w.frames++;
        brain_think(&bb[0], bb, 3, &w, 1, now, &out);
        t->frames++;
        int any = 0;
        for (int k = 0; k < BTN_COUNT; k++) { if (out.btn[k] && !last[k]) { t->btn[k]++; if (t->firstBtn[k] < 0) t->firstBtn[k] = fr; } last[k] = out.btn[k]; any |= out.btn[k]; }
        int rt = out.axis[AX_RT] > 0; if (rt && !lastRt) t->rt++; lastRt = rt;
        if (out.btn[BTN_Y] && (out.axis[AX_LX] || out.axis[AX_LY])) t->yWithStick++;
        { int sx = out.axis[AX_LX] > 16000 ? 1 : out.axis[AX_LX] < -16000 ? -1 : 0; if (sx && t->lastSx && sx != t->lastSx) t->rev++; if (sx) t->lastSx = sx; }
        if (out.axis[AX_LX] > 16000) t->stickR++; if (out.axis[AX_LX] < -16000) t->stickL++; if (out.axis[AX_LY] > 16000) t->stickD++;
        if (!any && !out.axis[AX_LX] && !out.axis[AX_LY] && !rt) t->neutralFrames++;
    }
}
static int dec(int d) { return bb[0].decTotal[d]; }
static Chr *g_me;
static void take_on_b(int fr) { (void)fr; if (out.btn[BTN_B] && w.npk) { w.npk = 0; g_me->heldAddr = 0x2000; } }
static void wt_gone(int fr) { (void)fr; if (out.btn[BTN_B] && out.axis[AX_LX] > 16000) w.ch[1].heldAddr = 0; }   /* m7.2: the thrown weapon leaves our hand */
static int g_p1dx = 0; static void move_me_toward_stick(int fr);
static void mv_take(int fr) { move_me_toward_stick(fr); take_on_b(fr); }   /* v1.3 */
/* m7.3: the far target keeps its distance (5 ahead), P1 moves along (g_p1dx ahead, 0 = stays) */
static void wt_keep5(int fr) { move_me_toward_stick(fr); w.ch[1].pos.z = 1.5f; w.ch[2].pos.x = w.ch[1].pos.x + 5.0f; if (g_p1dx) w.ch[0].pos.x = w.ch[1].pos.x + g_p1dx * 0.5f; }
static void move_me_toward_stick(int fr) { (void)fr; g_me->pos.x += out.axis[AX_LX] / 32767.0f * 0.08f; g_me->pos.z += out.axis[AX_LY] / 32767.0f * 0.04f; }
/* v1.3: stuck-on-walls watchdog: an invisible wall at x = g_wallX (nothing passes left of it); depth steps, hops, idle frames */
static float g_wallX = -1e9f; static int g_wdStep, g_wdNeutral, g_wdLate;
static void mv_wall(int fr) { move_me_toward_stick(fr); if (g_me->pos.x < g_wallX) g_me->pos.x = g_wallX;
    if (out.axis[AX_LX] && out.axis[AX_LY] && bb[0].wdStage == 1) g_wdStep++;
    if (!out.axis[AX_LX] && !out.axis[AX_LY]) g_wdNeutral++; }
static void frozen(int fr) { (void)fr; if (out.axis[AX_LX] < 0) g_wdLate++; }
static int g_pinZ, g_brkAirX, g_brkLastX;
static void move_brk_check(int fr) {
    (void)fr;
    g_me->pos.x += out.axis[AX_LX] / 32767.0f * 0.08f;
    if (!g_pinZ) g_me->pos.z += out.axis[AX_LY] / 32767.0f * 0.04f;
    if (out.btn[BTN_X] && !g_brkLastX) {
        if (fabsf(g_me->pos.z - w.br[0].pos.z) > 0.5f) g_brkAirX++;
        if (fabsf(g_me->pos.z - w.br[0].pos.z) <= 0.25f && fabsf(g_me->pos.x - w.br[0].pos.x) <= 2.2f) w.br[0].life -= 1;
    }
    g_brkLastX = out.btn[BTN_X];
}


/* v1.1 (m8): offline tests of the Bot skill switch. Every scenario runs with BotSkill=0 (Easy, must do what v1.0 did:
   no Mania decision ever) and BotSkill=1 (Mania: the new behavior shows up). Synthetic worlds as in brain_test.c. */
static int mania_any(void) { int n = 0; for (int d = D_M_REPOS; d < D_COUNT; d++) n += dec(d); return n; }   /* stage 4: every Normal decision id */
void dbg_mania_pipes_reset(void);
static void spec_char(const char *n) { brain_set_character(&bb[0], 0, n); w.ps[1].playable = 0; }
int main(void) {
    g_brainLearnWalls = 0;   /* m7.4: the test world never moves: every push would look like a wall */
    C.logLevel = LV_DEBUG; C.smart = 1; C.attackButton = BTN_X; C.jumpButton = BTN_A; C.specialButton = BTN_Y; C.useSpecials = 1;
    C.specialMinLifePct = 60; C.defSpecialMinLifePct = 40; C.retreatLifePct = 30; C.retreatMaxMs = 2500; C.comboHits = 4; C.comboTapMs = 110; C.comboPauseMs = 350;
    C.joinButton = BTN_A; C.joinPresses = 2; C.focusGraceMs = 0; C.resumeGraceMs = 0; C.starButton = BTN_RB; C.starMode = 0; C.backAttack = 1;
    C.useStars = 1; C.usePickups = 1; C.useGrabs = 1; C.foodLifePct = 75; C.brainLogMs = 0;
    C.itemHeadStartMs = 0; C.weaponHeadStartMs = 0; C.breakObjects = 1; C.jumpAttacks = 1; C.weaponThrows = 1;
    brain_set_focus(1, 0);
    if (getenv("MT_LOG")) { g_quiet = 0; C.logLevel = LV_DEBUG; C.brainLogMs = 160; }
    Tally t;
    for (int sk = 0; sk < 2; sk++) {
        C.botSkill = sk; const char *S = sk ? "Normal" : "Easy";
        Chr *me;
        /* 1. held at 60% life, one enemy: Easy mashes (needs 2+ near or 70% life); Mania: neutral special at once */
        world_reset(); bot_reset(1); me = &w.ch[1]; me->life = 60; me->state = CS_STANDHIT; me->grabbedBy = 50;
        mk(2, 50, 1, -1, 0.5f, 1.5f, -1, CS_STANDMOVE);
        run(30, &t, NULL);
        if (sk) CHECK(t.btn[BTN_Y] == 1 && t.firstBtn[BTN_Y] <= 8 && !t.yWithStick && dec(D_M_PANIC) == 1, "%s: held at 60%%, 1 enemy: neutral special at frame %d (Y %d)", S, t.firstBtn[BTN_Y], t.btn[BTN_Y]);
        else CHECK(!t.btn[BTN_Y] && dec(D_GRABESCAPE) == 1 && !mania_any(), "%s: held at 60%%, 1 enemy: mash, no Y (%d), no Mania decision (%d)", S, t.btn[BTN_Y], mania_any());
        /* 1b. held at 35%: below DefensiveSpecialMinLifePercent: no special in either skill */
        world_reset(); bot_reset(1); me = &w.ch[1]; me->life = 35; me->state = CS_STANDHIT; me->grabbedBy = 50;
        mk(2, 50, 1, -1, 0.5f, 1.5f, -1, CS_STANDMOVE);
        run(30, &t, NULL);
        CHECK(!t.btn[BTN_Y], "%s: held at 35%%: no special (HP guard), Y %d", S, t.btn[BTN_Y]);

        /* 2. an enemy jumping in at us (y 1.0, attacking, not a slam): Mania neutral special, then a follow-up string */
        world_reset(); bot_reset(1);
        { Chr *j = mk(2, 50, 1, -1, 0.8f, 1.5f, -1, CS_AIRMOVE); j->aiAttacking = 1; j->pos.y = 1.0f; j->aiTarget = 2; }
        run(6, &t, NULL); int y1 = t.btn[BTN_Y];
        w.ch[2].state = CS_STANDHIT; w.ch[2].pos.y = 0; w.ch[2].aiAttacking = 0; w.ch[2].hitStun = 20;
        run(90, &t, NULL);
        if (sk) CHECK(y1 == 1 && dec(D_M_PANIC) == 1 && dec(D_M_FOLLOWUP) == 1 && t.btn[BTN_X] >= 3, "%s: jumper: neutral special %d, follow-up string %d (X %d)", S, y1, dec(D_M_FOLLOWUP), t.btn[BTN_X]);
        else CHECK(!mania_any(), "%s: jumper: no Mania decision (%d), Y %d", S, mania_any(), y1);

        /* 3. a special with nobody in reach afterwards: Mania steps away on depth */
        world_reset(); bot_reset(1);
        { Chr *j = mk(2, 50, 1, -1, 0.8f, 1.5f, -1, CS_AIRMOVE); j->aiAttacking = 1; j->pos.y = 1.0f; j->aiTarget = 2; }
        run(6, &t, NULL);
        w.ch[2].state = CS_STANDWALK; w.ch[2].pos.y = 0; w.ch[2].aiAttacking = 0; w.ch[2].pos.x = 5.0f; w.ch[2].pos.z = 1.5f;
        run(40, &t, NULL);
        if (sk) CHECK(dec(D_M_STEPAWAY) == 1 && dec(D_M_FOLLOWUP) == 0, "%s: special, target walked off to 5.0: stepped away on depth (%d)", S, dec(D_M_STEPAWAY));
        else CHECK(!mania_any(), "%s: no step-away (%d)", S, mania_any());

        /* 4. spaced blitz: target 1.5 ahead (1.2 x range), on our lane: Easy walks in, Mania fwd,fwd+X */
        world_reset(); bot_reset(1);
        mk(2, 50, 1, -1, 1.5f, 1.5f, -1, CS_STANDWALK);
        run(20, &t, NULL);
        if (sk) CHECK(dec(D_M_GU) == 1 && t.btn[BTN_X] == 1 && t.stickR > 0, "%s: target at 1.5: spaced blitz (%d, X %d)", S, dec(D_M_GU), t.btn[BTN_X]);
        else CHECK(dec(D_BLITZ) == 0 && !mania_any(), "%s: target at 1.5: no blitz (%d), no Mania (%d)", S, dec(D_BLITZ), mania_any());
        /* 4b. same with another enemy right on top of us: no blitz (it would be hit in its startup) */
        world_reset(); bot_reset(1);
        mk(2, 50, 1, -1, 1.5f, 1.5f, -1, CS_STANDWALK); mk(3, 51, 1, -1, -0.5f, 1.5f, 1, CS_STANDWALK);
        run(20, &t, NULL);
        CHECK(dec(D_M_GU) == 0, "%s: enemy hugging us: no spaced blitz (%d)", S, dec(D_M_GU));
        /* 4c. target knocked into the air at 1.8: blitz catch */
        world_reset(); bot_reset(1);
        { Chr *a = mk(2, 50, 1, -1, 1.8f, 1.5f, -1, CS_AIRHIT); a->pos.y = 1.5f; a->hitStun = 30; }
        run(20, &t, NULL);
        if (sk) CHECK(dec(D_M_GUCATCH) == 1, "%s: target in the air at 1.8: blitz catch (%d)", S, dec(D_M_GUCATCH));
        else CHECK(dec(D_M_GUCATCH) == 0, "%s: no blitz catch", S);

        /* 5. holding an enemy, another one close behind: Mania throws (back+X), Easy knees */
        world_reset(); bot_reset(1); me = &w.ch[1]; me->grabbing = 1; me->grabUid = 50;
        mk(2, 50, 1, -1, 0.5f, 1.5f, -1, CS_STANDHIT); mk(3, 51, 1, -1, -1.0f, 1.5f, 1, CS_STANDWALK);
        { int backX = 0; for (int fr = 0; fr < 10; fr++, now += 16) { w.frames++; brain_think(&bb[0], bb, 3, &w, 1, now, &out); if (out.btn[BTN_X] && out.axis[AX_LX] < -16000) backX++; }
          if (sk) CHECK(backX > 0 && dec(D_M_THROW) == 1 && dec(D_GRABHIT) == 0, "%s: holding with an enemy behind: back throw (%d frames), knees %d", S, backX, dec(D_GRABHIT));
          else CHECK(dec(D_GRABHIT) >= 1 && dec(D_M_THROW) == 0, "%s: holding: knee (%d), no Mania throw", S, dec(D_GRABHIT)); }

        /* 6. enemies on both sides (not close): Mania walks past the end of the group */
        world_reset(); bot_reset(1); g_me = &w.ch[1];
        mk(2, 50, 1, -1, 2.6f, 1.5f, -1, CS_STANDWALK); mk(3, 51, 1, -1, -2.6f, 1.6f, 1, CS_STANDWALK);
        run(60, &t, move_me_toward_stick);
        if (sk) { float x = g_me->pos.x; int oneSide = (2.6f - x) * (-2.6f - x) > 0; CHECK(dec(D_M_REPOS) >= 1 && oneSide, "%s: enemies both sides: repositioned to x %.1f (one side: %d)", S, x, oneSide); }
        else CHECK(dec(D_M_REPOS) == 0, "%s: enemies both sides: no reposition", S);

        /* 7. 4 enemies around near the left screen edge: Mania backs to the edge */
        world_reset(); bot_reset(1); g_me = &w.ch[1]; g_me->pos.x = -5.0f; w.ch[0].pos.x = -6.0f;
        mk(2, 50, 1, -1, -3.0f, 1.5f, -1, CS_STANDWALK); mk(3, 51, 1, -1, -2.5f, 2.0f, -1, CS_STANDWALK);
        mk(4, 52, 1, -1, -7.0f, 1.0f, 1, CS_STANDWALK); mk(5, 53, 1, -1, -3.2f, 1.0f, -1, CS_STANDWALK);
        run(5, &t, move_me_toward_stick);
        if (sk) CHECK(dec(D_M_WALL) == 1 && t.stickL > 0, "%s: 4 around near the left edge: backing to it (%d, left %d frames)", S, dec(D_M_WALL), t.stickL);
        else CHECK(dec(D_M_WALL) == 0, "%s: no back-to-the-edge", S);

        /* 8. target off our lane, another enemy on our row far ahead facing us: Mania steps off that row */
        world_reset(); bot_reset(1);
        mk(2, 50, 1, -1, 3.0f, 2.5f, -1, CS_STANDWALK); mk(3, 51, 1, -1, 7.0f, 1.5f, -1, CS_STANDWALK);
        run(10, &t, NULL);
        if (sk) CHECK(dec(D_M_ROWDODGE) == 1, "%s: enemy on our row at 7.0: off its row (%d)", S, dec(D_M_ROWDODGE));
        else CHECK(dec(D_M_ROWDODGE) == 0, "%s: no row dodge", S);

        /* 9. target near the left screen edge, we are between it and the edge: Mania goes around to pin it */
        world_reset(); bot_reset(1); g_me = &w.ch[1]; g_me->pos.x = -8.5f; w.ch[0].pos.x = -4.0f;
        mk(2, 50, 1, -1, -6.5f, 1.5f, -1, CS_STANDWALK);
        run(150, &t, move_me_toward_stick);
        if (sk) CHECK(dec(D_M_PUSH) == 1 && g_me->pos.x > -6.5f, "%s: target near the left edge: came around to x %.1f (target -6.5)", S, g_me->pos.x);
        else CHECK(dec(D_M_PUSH) == 0 && g_me->pos.x < -6.5f, "%s: stays on the edge side (x %.1f)", S, g_me->pos.x);

        /* 10. Floyd, target at mid range only: Easy pulls (fwd+Y); Mania: no offensive special without a close target */
        world_reset(); bot_reset(4); brain_set_character(&bb[0], 3, "Floyd"); bb[0].profFor = 3; w.ps[1].playable = 0;
        mk(2, 50, 1, -1, 2.5f, 1.5f, -1, CS_STANDWALK);
        run(30, &t, NULL);
        if (sk) CHECK(t.btn[BTN_Y] == 1 && dec(D_M_FWDSP) == 1 && dec(D_PULL) == 0, "%s: Floyd, target at 2.5 (1.7 ranges): Magnetic Grab pull from the table (Y %d, %d)", S, t.btn[BTN_Y], dec(D_M_FWDSP));
        else CHECK(t.btn[BTN_Y] == 1 && dec(D_PULL) == 1, "%s: Floyd, target at 2.5: pull (Y %d)", S, t.btn[BTN_Y]);

        /* 11. forward special right after a string on a reeling target (Axel: Dragon Smash), then the follow-up */
        world_reset(); bot_reset(1);
        mk(2, 50, 1, -1, 0.8f, 1.5f, -1, CS_STANDWALK);
        { int fy = 0, react = 0;
          for (int fr = 0; fr < 120; fr++, now += 16) { w.frames++; brain_think(&bb[0], bb, 3, &w, 1, now, &out);
              if (out.btn[BTN_X] && !react) { react = 1; w.ch[2].state = CS_STANDHIT; w.ch[2].hitStun = 30; }
              if (out.btn[BTN_Y] && out.axis[AX_LX] > 16000) fy++; }
          if (sk) CHECK(dec(D_M_SMASH) == 1 && fy > 0, "%s: reeling target after the string: forward special (%d, %d frames fwd+Y), follow-up %d", S, dec(D_M_SMASH), fy, dec(D_M_FOLLOWUP));
          else CHECK(dec(D_M_SMASH) == 0 && !mania_any(), "%s: no Mania forward special", S); }

        /* 12. v1.1 stage 2: Axel, target at mid range on our lane (2.8 = 2.2 x range): Normal jumps and presses special in
           the air (Dragon Dive), then the follow-up string once it is next to the target; Easy: no air special there */
        world_reset(); bot_reset(1); me = &w.ch[1]; brain_set_character(&bb[0], 0, "Axel"); w.ps[1].playable = 0;
        mk(2, 50, 1, -1, 2.8f, 1.5f, -1, CS_STANDWALK); mk(3, 51, 1, -1, 3.5f, 1.55f, 1, CS_STANDWALK);   /* v1.2: 2 in its path */
        { int aFr = -1, yFr = -1, fwdY = 0;
          for (int fr = 0; fr < 70; fr++, now += 16) { w.frames++; brain_think(&bb[0], bb, 3, &w, 1, now, &out);
              if (out.btn[BTN_A] && aFr < 0) aFr = fr;
              if (out.btn[BTN_Y] && yFr < 0) yFr = fr;
              if (out.btn[BTN_Y] && out.axis[AX_LX] > 16000) fwdY++;
              if (fr == 40) { me->pos.x = 2.0f; } }     /* the dive carried us next to it */
          if (sk) CHECK(dec(D_M_DIVE) == 1 && aFr >= 0 && yFr > aFr && fwdY > 0 && dec(D_M_FOLLOWUP) == 1,
                        "%s: target at 2.8: air special (A at %d, Y at %d, fwd+Y %d frames), follow-up string %d", S, aFr, yFr, fwdY, dec(D_M_FOLLOWUP));
          else CHECK(dec(D_M_DIVE) == 0 && dec(D_AIRSPECIAL) == 0 && !mania_any(), "%s: target at 2.8: no air special (%d)", S, dec(D_AIRSPECIAL)); }
        /* 12b. same at 50%% life (below SpecialMinLifePercent 60): no air special */
        world_reset(); bot_reset(1); me = &w.ch[1]; me->life = 50; brain_set_character(&bb[0], 0, "Axel"); w.ps[1].playable = 0;
        mk(2, 50, 1, -1, 2.8f, 1.5f, -1, CS_STANDWALK);
        run(40, &t, NULL);
        CHECK(dec(D_M_DIVE) == 0 && !t.btn[BTN_Y], "%s: 50%% life, target at 2.8: no air special (%d, Y %d)", S, dec(D_M_DIVE), t.btn[BTN_Y]);
        /* 12c. cooldown: right after one, a second chance within 3 s: none */
        if (sk) { world_reset(); bot_reset(1); bb[0].mLastDive = now - 1000; brain_set_character(&bb[0], 0, "Axel"); w.ps[1].playable = 0;
          mk(2, 50, 1, -1, 2.8f, 1.5f, -1, CS_STANDWALK);
          run(40, &t, NULL);
          CHECK(dec(D_M_DIVE) == 0, "%s: air special 1 s ago: no second one (%d)", S, dec(D_M_DIVE)); }
        /* 12d. a retro character (default profile): no air special */
        world_reset(); bot_reset(1); brain_set_character(&bb[0], 1, "Axel_SOR1"); w.ps[1].playable = 0;
        mk(2, 50, 1, -1, 2.8f, 1.5f, -1, CS_STANDWALK);
        run(40, &t, NULL);
        CHECK(dec(D_M_DIVE) == 0, "%s: retro character: no air special (%d)", S, dec(D_M_DIVE));

        /* 13. v1.1 stage 2: a weapon 1.0 behind us, an enemy 2.0 past it: Easy picks it up (enemy > 1.2 x range away),
           Normal leaves it (the enemy is near it) */
        world_reset(); bot_reset(1); g_me = &w.ch[1];
        w.npk = 1; w.pk[0].addr = 0x3000; w.pk[0].kind = PK_WEAPON; w.pk[0].pos.x = -1.0f; w.pk[0].pos.z = 1.5f; w.ch[0].pos.x = -1.5f; w.ch[0].pos.z = 3.5f;
        mk(2, 50, 1, -1, -3.0f, 1.5f, 1, CS_STANDWALK);
        run(25, &t, move_me_toward_stick);
        if (sk) CHECK(dec(D_WEAPONPICK) == 0 && !t.btn[BTN_B], "%s: enemy next to the weapon: left it (B %d)", S, t.btn[BTN_B]);
        else CHECK(dec(D_WEAPONPICK) >= 1 && t.btn[BTN_B] >= 1, "%s: weapon picked up as v1.0 (B %d)", S, t.btn[BTN_B]);

        /* ===== v1.1 stage 4 ===== */
        /* 14. life percent with the max unreadable and a section started at partial life: 65 is 65% (no air special,
           gap closers need 70), not 100% */
        world_reset(); bot_reset(1); me = &w.ch[1]; me->life = 65; me->maxLife = 0; spec_char("Axel");
        mk(2, 50, 1, -1, 2.8f, 1.5f, -1, CS_STANDWALK);
        run(40, &t, NULL);
        if (sk) CHECK(dec(D_M_DIVE) == 0, "%s: life 65 with no max read: 65%%, no air special (%d)", S, dec(D_M_DIVE));
        else CHECK(!mania_any(), "%s: life 65 with no max read: no Mania decision (%d)", S, mania_any());
        world_reset(); bot_reset(1); me = &w.ch[1]; me->life = 85; me->maxLife = 0; spec_char("Axel");
        mk(2, 50, 1, -1, 2.8f, 1.5f, -1, CS_STANDWALK); mk(3, 51, 1, -1, 3.5f, 1.55f, 1, CS_STANDWALK);   /* v1.2: 2 in its path */
        run(40, &t, NULL);
        if (sk) CHECK(dec(D_M_DIVE) == 1, "%s: life 85 with no max read: 85%%, air special (%d)", S, dec(D_M_DIVE));

        /* 15. a heavy up in its leap right next to us: out of the wide, shallow landing box (on depth here), no panic
           special under it */
        dbg_trap_reset(); world_reset(); bot_reset(1); g_me = me = &w.ch[1]; w.ch[0].pos.z = 3.5f; w.ch[0].pos.x = -6.0f;
        { Chr *h = mk(2, 60, 1, -1, 1.0f, 1.5f, -1, CS_AIRMOVE); h->life = 420; h->dataMaxLife = 420; h->aiAttacking = 1; h->pos.y = 6.0f; h->moveId = 4; }
        run(100, &t, move_me_toward_stick);
        { float ddx = fabsf(me->pos.x - 1.0f), ddz = fabsf(me->pos.z - 1.5f); int out_ = ddx >= 4.2f || ddz >= 1.1f;
          if (sk) CHECK(dec(D_M_LEAP) >= 1 && out_ && !t.btn[BTN_Y], "%s: heavy leaping 1.0 away: out of its landing box to (%.1f, %.1f) (leap %d, Y %d)", S, me->pos.x, me->pos.z, dec(D_M_LEAP), t.btn[BTN_Y]);
          else CHECK(dec(D_M_LEAP) == 0 && !mania_any(), "%s: heavy leaping: v1.0 slam evade, no Mania decision (%d)", S, mania_any()); }

        /* 16. a pipe flying along depth through our spot: step aside in x (depth is its own path) */
        world_reset(); bot_reset(1); dbg_mania_pipes_reset(); g_me = me = &w.ch[1]; w.ch[0].pos.z = 3.5f; w.ch[0].pos.x = -6.0f;
        mk(2, 50, 1, -1, 7.0f, 2.5f, -1, CS_STANDWALK);
        w.npj = 1; w.pj[0].uid = 133; w.pj[0].team = 1; w.pj[0].kind = 1; w.pj[0].pos.x = 0.2f; w.pj[0].pos.y = 2.2f; w.pj[0].pos.z = 4.5f; w.pj[0].vel.z = -0.5f; w.pj[0].vel.x = 0.02f;
        run(25, &t, move_me_toward_stick);
        if (sk) CHECK(dec(D_M_OBJROW) >= 1 && fabsf(me->pos.x) > 0.8f, "%s: pipe flying along depth at us: aside in x to %.1f (%d)", S, me->pos.x, dec(D_M_OBJROW));
        else CHECK(!mania_any(), "%s: pipe along depth: no Mania decision (%d)", S, mania_any());

        /* 17. a low pipe just risen from the floor (y 0.28, still): it will hover and shoot: out of its zone */
        world_reset(); bot_reset(1); dbg_mania_pipes_reset(); g_me = me = &w.ch[1]; w.ch[0].pos.z = 3.5f; w.ch[0].pos.x = -6.0f;
        mk(2, 50, 1, -1, 8.0f, 2.5f, -1, CS_STANDWALK);
        w.npj = 1; w.pj[0].uid = 144; w.pj[0].team = 1; w.pj[0].kind = 1; w.pj[0].pos.x = 0.8f; w.pj[0].pos.y = 0.28f; w.pj[0].pos.z = 1.6f;
        run(90, &t, move_me_toward_stick);
        { int out_ = fabsf(me->pos.x - 0.8f) >= 3.0f || fabsf(me->pos.z - 1.6f) >= 1.3f;
          if (sk) CHECK(dec(D_M_PIPE) >= 1 && out_, "%s: low pipe rising next to us: out of its zone to (%.1f, %.1f) (%d)", S, me->pos.x, me->pos.z, dec(D_M_PIPE));
          else CHECK(!mania_any(), "%s: low pipe: no Mania decision (%d)", S, mania_any()); }

        /* 18. hit by a floor trap (hit source = hazard uid) with an enemy near: zone at once (Easy: the v1.0 check needs no
           enemy near); then money lying in that zone is left alone */
        dbg_trap_reset(); world_reset(); bot_reset(1); g_me = me = &w.ch[1];
        mk(2, 50, 1, -1, 2.0f, 1.5f, -1, CS_STANDWALK);
        w.nhz = 1; w.hz[0].uid = 13; w.hz[0].pos.x = 0.4f; w.hz[0].pos.z = 1.6f; w.hz[0].kind = 0; w.hz[0].anim = 0;
        run(10, &t, NULL);
        me->state = CS_AIRHIT; me->hitSrc = 13; me->life = 80;
        run(3, &t, NULL);
        if (sk) CHECK(dec(D_M_TRAPSRC) == 1 && dbg_trap_zone_count() == 1, "%s: hit by hazard 13 with an enemy near: zone at once (%d, zones %d)", S, dec(D_M_TRAPSRC), dbg_trap_zone_count());
        else CHECK(dec(D_M_TRAPSRC) == 0 && dbg_trap_zone_count() == 0, "%s: hit by hazard 13 with an enemy near: no zone (v1.0) (%d)", S, dbg_trap_zone_count());
        if (sk) {
            me->state = CS_STANDWALK; me->pos.x = -3.0f; w.nch = 2;   /* enemy gone */
            w.npk = 1; w.pk[0].addr = 0x4000; w.pk[0].kind = PK_SCORE; w.pk[0].scoreGain = 100; w.pk[0].pos.x = 0.5f; w.pk[0].pos.z = 1.6f;
            run(40, &t, move_me_toward_stick);
            CHECK(dec(D_M_PICKTRAP) >= 1 && bb[0].goalPick != 0x4000 && me->pos.x < -1.0f, "%s: money inside the trap zone: left alone (%d, x %.1f)", S, dec(D_M_PICKTRAP), me->pos.x);
        }

        /* 19. green health still showing 9 s after our special: no offensive special (finisher), no surrounded/jumper
           panic; held: the panic still goes */
        world_reset(); bot_reset(1); bb[0].greenKnown = 1; bb[0].specialAt = now - 9000; w.ps[1].banked = 5.0f;
        mk(2, 50, 1, -1, 0.8f, 1.5f, -1, CS_STANDWALK);
        { for (int fr = 0; fr < 120; fr++, now += 16) { w.frames++; brain_think(&bb[0], bb, 3, &w, 1, now, &out);
              if (out.btn[BTN_X] && w.ch[2].state != CS_STANDHIT) { w.ch[2].state = CS_STANDHIT; w.ch[2].hitStun = 30; } }
          if (sk) CHECK(dec(D_M_SMASH) == 0 && dec(D_M_GREEN) == 1, "%s: green showing 9 s after the special: no forward special (%d), lock noted %d", S, dec(D_M_SMASH), dec(D_M_GREEN));
          else CHECK(dec(D_M_GREEN) == 0 && !mania_any(), "%s: green showing: no Mania decision (%d)", S, mania_any()); }
        world_reset(); bot_reset(1); bb[0].greenKnown = 1; w.ps[1].banked = 5.0f;
        { Chr *j = mk(2, 50, 1, -1, 0.8f, 1.5f, -1, CS_AIRMOVE); j->aiAttacking = 1; j->pos.y = 1.0f; j->aiTarget = 2; }
        run(6, &t, NULL);
        if (sk) CHECK(!t.btn[BTN_Y] && dec(D_M_PANIC) == 0, "%s: green showing, enemy jumping in: no panic special (Y %d)", S, t.btn[BTN_Y]);
        world_reset(); bot_reset(1); me = &w.ch[1]; me->life = 60; me->state = CS_STANDHIT; me->grabbedBy = 50; w.ps[1].banked = 5.0f; bb[0].greenKnown = 1;
        mk(2, 50, 1, -1, 0.5f, 1.5f, -1, CS_STANDMOVE);
        run(30, &t, NULL);
        if (sk) CHECK(t.btn[BTN_Y] == 1 && dec(D_M_PANIC) == 1, "%s: green showing but held: neutral special still (Y %d)", S, t.btn[BTN_Y]);

        /* 20. a dangerous heavy on screen (not reeling): no offensive special (Axel at 2.8: no air special) */
        world_reset(); bot_reset(1); spec_char("Axel");
        mk(2, 50, 1, -1, 2.8f, 1.5f, -1, CS_STANDWALK);
        { Chr *h = mk(3, 60, 1, -1, -7.0f, 3.0f, 1, CS_STANDWALK); h->life = 420; h->dataMaxLife = 420; }
        run(40, &t, NULL);
        if (sk) CHECK(dec(D_M_DIVE) == 0, "%s: heavy on screen: no air special (%d)", S, dec(D_M_DIVE));

        /* 21. per-character specials (Normal) and the alternates */
        { static const struct { const char *ch; float dx; int alt; int d; int want; const char *what; } k[] = {
              { "Max",    3.0f, -1,       D_M_FWDSP, 1, "Thunder Tackle gap closer at 3.0" },
              { "Max",    3.0f, 2,        D_M_FWDSP, 0, "alternate forward special: no gap closer" },
              { "Shiva",  2.5f, -1,       D_M_FWDSP, 1, "Senretsu Ken at 2.5" },
              { "Estel",  3.0f, -1,       D_M_FWDSP, 1, "Police Tackle onto a lone target at 3.0" },
              { "Blaze",  2.2f, -1,       D_M_DIVE,  1, "Tobi Kyaku opener at 2.2" },
              { "Blaze",  2.2f, 3,        D_M_DIVE,  0, "alternate air special: no mid-range opener" },
              { "Cherry", 3.5f, -1,       D_M_GU,    1, "Flying Knee blitz at 3.5 (2.8 ranges)" },
              { "Cherry", 3.5f, 0,        D_M_GU,    0, "alternate blitz: stage 3 band (2.6 ranges)" },
              { "Axel",   3.5f, -1,       D_M_GU,    0, "Axel: no blitz at 3.5" } };
          for (size_t i = 0; i < sizeof k / sizeof *k; i++) {
              world_reset(); bot_reset(1); spec_char(k[i].ch); if (k[i].alt >= 0) w.ps[1].alt[k[i].alt] = 1;
              mk(2, 50, 1, -1, k[i].dx, 1.5f, -1, CS_STANDWALK);
              run(25, &t, NULL);
              if (sk) CHECK((dec(k[i].d) >= 1) == k[i].want, "%s: %s: %s (%d)", S, k[i].ch, k[i].what, dec(k[i].d));
              else CHECK(!mania_any(), "%s: %s at %.1f: no Mania decision (%d)", S, k[i].ch, k[i].dx, mania_any());
          } }
        /* 21b. Estel with a second enemy next to the target: no Police Tackle (it grabs the first one) */
        world_reset(); bot_reset(1); spec_char("Estel");
        mk(2, 50, 1, -1, 3.0f, 1.5f, -1, CS_STANDWALK); mk(3, 51, 1, -1, 4.0f, 1.7f, -1, CS_STANDWALK);
        run(25, &t, NULL);
        if (sk) CHECK(dec(D_M_FWDSP) == 0, "%s: Estel, two enemies together: no Police Tackle (%d)", S, dec(D_M_FWDSP));
        /* 21c. alternates unreadable (-1): defaults assumed (Max still tackles) */
        world_reset(); bot_reset(1); spec_char("Max"); for (int a = 0; a < 5; a++) w.ps[1].alt[a] = -1;
        mk(2, 50, 1, -1, 3.0f, 1.5f, -1, CS_STANDWALK);
        run(25, &t, NULL);
        if (sk) CHECK(dec(D_M_FWDSP) == 1 && bb[0].mAltLogged, "%s: alternates unreadable: defaults assumed, Max tackles (%d)", S, dec(D_M_FWDSP));
        /* 21d. a retro character: no table entry, no forward-special gap closer */
        world_reset(); bot_reset(1); spec_char("Max_SOR2");
        mk(2, 50, 1, -1, 3.0f, 1.5f, -1, CS_STANDWALK);
        run(25, &t, NULL);
        CHECK(dec(D_M_FWDSP) == 0, "%s: retro Max: no gap closer (%d)", S, dec(D_M_FWDSP));

        /* ===== v1.2 ===== */
        /* 22. Survival perks: P1 alive, 2 perks (one revive) up: wait PerkWaitSeconds (10) for a player, then take the
           non-revive perk with B; Easy: never */
        #define PERK(i_, uid_, bon_, x_, z_, rv_) do { Activable *a_ = &w.act[i_]; memset(a_, 0, sizeof *a_); a_->uid = uid_; a_->bonusId = bon_; a_->pos.x = x_; a_->pos.z = z_; a_->havePos = 1; \
            a_->state = 1; a_->haveData = 1; a_->haveBox = 1; a_->boxX0 = -1; a_->boxX1 = 1; a_->boxZ0 = -0.4f; a_->boxZ1 = 0.4f; a_->resuscitate = rv_; a_->destroySame = 1; if (i_ >= w.nact) w.nact = i_ + 1; } while (0)
        #define PORTAL(i_, x_, z_) do { Activable *a_ = &w.act[i_]; memset(a_, 0, sizeof *a_); a_->uid = 99; a_->bonusId = -1; a_->pos.x = x_; a_->pos.z = z_; a_->havePos = 1; a_->state = 1; \
            a_->haveData = 1; a_->isPortal = 1; a_->actType = 1; a_->haveBox = 1; a_->boxX0 = -1; a_->boxX1 = 1; a_->boxZ0 = 0; a_->boxZ1 = 0.5f; a_->resuscitate = -1; if (i_ >= w.nact) w.nact = i_ + 1; } while (0)
        C.perkWaitSec = 10; C.starSavePct = 25;
        dbg_surv_reset(); world_reset(); bot_reset(1); g_me = me = &w.ch[1]; w.survActive = 1; w.survLevelId = 0; w.survWaveId = 1; w.survEventId = 1;
        w.ch[0].pos.x = 0; w.ch[0].pos.z = 3.5f;
        PERK(0, 9, 5, 2.5f, 1.5f, 0); PERK(1, 10, 0, -2.5f, 1.5f, 1);
        run(300, &t, move_me_toward_stick);
        { int b5 = t.btn[BTN_B], d5 = dec(D_M_PERK);
          run(450, &t, move_me_toward_stick);
          int onPerk = fabsf(me->pos.x - 2.5f) < 0.7f && fabsf(me->pos.z - 1.5f) < 0.3f;
          if (sk) CHECK(!b5 && !d5 && t.btn[BTN_B] >= 1 && dec(D_M_PERK) == 1 && onPerk, "%s: survival perks: waited (B %d in the first 5 s), then took the non-revive perk with B (%d presses, at %.1f, %.1f)", S, b5, t.btn[BTN_B], me->pos.x, me->pos.z);
          else CHECK(!t.btn[BTN_B] && !b5 && !mania_any(), "%s: survival perks: no perk pick (B %d), no Normal decision (%d)", S, t.btn[BTN_B], mania_any()); }
        /* 22b. P1 standing on a perk the whole time: no bot pick even after 12 s */
        dbg_surv_reset(); world_reset(); bot_reset(1); g_me = me = &w.ch[1]; w.survActive = 1; w.survLevelId = 0; w.survWaveId = 1;
        w.ch[0].pos.x = 2.4f; w.ch[0].pos.z = 1.5f; me->pos.x = -4.0f;
        PERK(0, 9, 5, 2.5f, 1.5f, 0); PERK(1, 11, 7, 5.0f, 1.5f, 0);
        run(750, &t, move_me_toward_stick);
        CHECK(!t.btn[BTN_B] && dec(D_M_PERK) == 0, "%s: a player on a perk: no bot pick in 12 s (B %d)", S, t.btn[BTN_B]);
        /* 22c. P1 dead (not in the character list): the bot goes straight for the REVIVE perk (left this time) with B */
        dbg_surv_reset(); world_reset(); bot_reset(1); g_me = me = &w.ch[1]; w.survActive = 1; w.survLevelId = 2; w.survWaveId = 3;
        w.ch[0].player = 7; w.ch[0].life = -0.5f;   /* P1's fighter gone */
        PERK(0, 31, 0, -1.6f, 1.5f, 1); PERK(1, 32, 12, 2.0f, 1.5f, 0);
        run(150, &t, move_me_toward_stick);
        { int onRev = fabsf(me->pos.x + 1.6f) < 0.7f;
          if (sk) CHECK(t.btn[BTN_B] >= 1 && dec(D_M_PERK) == 1 && onRev, "%s: P1 down: revive perk taken at once (B %d, at x %.1f)", S, t.btn[BTN_B], me->pos.x);
          else CHECK(!t.btn[BTN_B] && !mania_any(), "%s: P1 down: no perk pick (B %d)", S, t.btn[BTN_B]); }
        /* 22d. then the portal (perks gone): every player down: into the portal */
        w.nact = 0; PORTAL(0, 0.3f, 0.6f);
        run(150, &t, move_me_toward_stick);
        { int in = fabsf(me->pos.x - 0.3f) < 1.0f && me->pos.z >= 0.6f - 0.05f && me->pos.z <= 1.1f + 0.05f;
          if (sk) CHECK(in && dec(D_M_PORTAL) == 1, "%s: P1 down, portal up: walked into it (at %.1f, %.1f)", S, me->pos.x, me->pos.z);
          else CHECK(dec(D_M_PORTAL) == 0, "%s: no portal walk", S); }
        /* 22e. portal up with P1 alive, the bot standing in it: steps out (the player takes it) */
        dbg_surv_reset(); world_reset(); bot_reset(1); g_me = me = &w.ch[1]; w.survActive = 1; w.survLevelId = 0; w.survWaveId = 1;
        PORTAL(0, 0.0f, 1.2f); me->pos.x = 0.0f; me->pos.z = 1.45f; w.ch[0].pos.x = -2.0f; w.ch[0].pos.z = 2.5f;
        run(60, &t, move_me_toward_stick);
        { int in = fabsf(me->pos.x) < 1.0f && me->pos.z >= 1.2f && me->pos.z <= 1.7f;
          if (sk) CHECK(!in, "%s: P1 alive: bot kept out of the portal (at %.1f, %.1f)", S, me->pos.x, me->pos.z);
          else CHECK(dec(D_M_PORTAL) == 0, "%s: portal: no Normal decision", S); }
        /* 22f. a perk the bot already owns is skipped (owned 5 near, 7 farther) */
        dbg_surv_reset(); world_reset(); bot_reset(1); g_me = me = &w.ch[1]; w.survActive = 1; w.survLevelId = 0; w.survWaveId = 1;
        w.ch[0].pos.x = 0; w.ch[0].pos.z = 3.5f; w.ps[1].nBonuses = 1; w.ps[1].bonuses[0] = 5;
        PERK(0, 9, 5, 1.5f, 1.5f, 0); PERK(1, 10, 7, -3.0f, 1.5f, 0);
        run(800, &t, move_me_toward_stick);
        if (sk) CHECK(t.btn[BTN_B] >= 1 && fabsf(me->pos.x + 3.0f) < 0.7f, "%s: owned perk skipped: took bonus 7 (at x %.1f)", S, me->pos.x);
        /* 22g. rotation: two living bots (P2, P3); round 1 -> P2 picks, round 2 -> P3 picks (P2 waits) */
        for (int rnd = 0; rnd < 2; rnd++) {
            dbg_surv_reset(); world_reset(); bot_reset(1); g_me = me = &w.ch[1]; w.survActive = 1; w.survLevelId = rnd; w.survWaveId = rnd + 1;
            w.ch[0].pos.x = 0; w.ch[0].pos.z = 3.5f;
            w.slot[2].ctrl = 2; w.slot[2].type = 1; mk(2, 3, 0, 2, -5.0f, 2.5f, 1, CS_STANDWALK);
            bb[1].identified = 1; bb[1].playerSlot = 2; bb[1].id.ctrlId = 2;
            PERK(0, 9, 5, 2.5f, 1.5f, 0); PERK(1, 10, 6, 5.0f, 1.5f, 0);
            run(750, &t, move_me_toward_stick);
            if (sk) CHECK((t.btn[BTN_B] >= 1) == (rnd == 0), "%s: rotation round %d: P2 %s (B %d)", S, rnd + 1, rnd == 0 ? "picks" : "waits for P3", t.btn[BTN_B]);
        }
        /* 22h. a weapon lying on a perk while waiting: Normal never presses B on a perk; Easy picks the weapon (v1.0) */
        dbg_surv_reset(); world_reset(); bot_reset(1); g_me = me = &w.ch[1]; w.survActive = 1; w.survLevelId = 0; w.survWaveId = 1;
        w.ch[0].pos.x = 0; w.ch[0].pos.z = 3.5f; PERK(0, 9, 5, 0.6f, 1.5f, 0);
        w.npk = 1; w.pk[0].addr = 0x3000; w.pk[0].kind = PK_WEAPON; w.pk[0].pos.x = 0.6f; w.pk[0].pos.z = 1.5f;
        run(200, &t, move_me_toward_stick);
        if (sk) CHECK(!t.btn[BTN_B], "%s: weapon on a perk during the wait: no B on the perk (B %d)", S, t.btn[BTN_B]);
        else CHECK(t.btn[BTN_B] >= 1, "%s: weapon on a perk: picked as v1.0 (B %d)", S, t.btn[BTN_B]);
        w.npk = 0;

        /* 23. star saving (Survival, Normal): boss close at 60% life: star kept; at 20%: star used; Arcade: used */
        for (int mode = 0; mode < 3; mode++) {
            dbg_surv_reset(); world_reset(); bot_reset(1); me = &w.ch[1]; w.ps[1].stars = 1;
            w.survActive = mode < 2; me->life = mode == 1 ? 20 : 60;
            mk(2, 50, 1, -1, 1.0f, 1.5f, -1, CS_STANDWALK)->boss = 1;
            run(60, &t, NULL);
            int want = !(sk && mode == 0);
            CHECK((dec(D_STAR) >= 1) == want && (!sk || mode != 0 || dec(D_M_STARSAVE) == 1), "%s: star, %s, life %d%%, boss close: %s (star %d, kept %d)", S,
                  mode == 2 ? "Arcade" : "Survival", mode == 1 ? 20 : 60, want ? "used" : "kept", dec(D_STAR), dec(D_M_STARSAVE));
        }
        /* 23b. Survival, 60% but surrounded (both sides): star used */
        dbg_surv_reset(); world_reset(); bot_reset(1); me = &w.ch[1]; w.ps[1].stars = 1; w.survActive = 1; me->life = 60;
        mk(2, 50, 1, -1, 1.0f, 1.5f, -1, CS_STANDWALK)->boss = 1; mk(3, 51, 1, -1, -0.8f, 1.6f, 1, CS_STANDWALK); mk(4, 52, 1, -1, 1.2f, 1.45f, -1, CS_STANDWALK);
        run(120, &t, NULL);
        CHECK(dec(D_STAR) >= 1, "%s: star, Survival, 60%%, surrounded: used (%d)", S, dec(D_STAR));

        /* 24. Axel's Dragon Dive needs 2+ in its path: lone target at 2.8: none; second enemy just past it: dive */
        world_reset(); bot_reset(1); spec_char("Axel"); mk(2, 50, 1, -1, 2.8f, 1.5f, -1, CS_STANDWALK);
        run(40, &t, NULL);
        CHECK(dec(D_M_DIVE) == 0, "%s: Axel, lone target at 2.8: no Dragon Dive (%d)", S, dec(D_M_DIVE));
        world_reset(); bot_reset(1); spec_char("Axel"); mk(2, 50, 1, -1, 2.8f, 1.5f, -1, CS_STANDWALK); mk(3, 51, 1, -1, 3.5f, 1.55f, 1, CS_STANDWALK);
        run(40, &t, NULL);
        if (sk) CHECK(dec(D_M_DIVE) == 1, "%s: Axel, 2 in the dive's path: Dragon Dive (%d)", S, dec(D_M_DIVE));
        else CHECK(dec(D_M_DIVE) == 0, "%s: no Dragon Dive", S);
        /* 24b. Cherry / Floyd use their air special on a lone target at their reach */
        { static const char *const cf[2] = { "Cherry", "Floyd" };
          for (int k = 0; k < 2; k++) {
              world_reset(); bot_reset(1); spec_char(cf[k]); mk(2, 50, 1, -1, 1.4f, 1.5f, -1, CS_STANDWALK);
              run(40, &t, NULL);
              if (sk) CHECK(dec(D_M_DIVE) == 1, "%s: %s, lone target at 1.4: air special (%d)", S, cf[k], dec(D_M_DIVE));
              else CHECK(dec(D_M_DIVE) == 0, "%s: %s: no Normal air special", S, cf[k]);
          } }
        /* 25. a pipe flying along depth 1.6 to our side (inside its real hit width): out of its path */
        world_reset(); bot_reset(1); dbg_mania_pipes_reset(); g_me = me = &w.ch[1]; w.ch[0].pos.z = 3.5f; w.ch[0].pos.x = -6.0f;
        mk(2, 50, 1, -1, 7.0f, 2.5f, -1, CS_STANDWALK);
        w.npj = 1; w.pj[0].uid = 135; w.pj[0].team = 1; w.pj[0].kind = 1; w.pj[0].pos.x = 1.6f; w.pj[0].pos.y = 2.2f; w.pj[0].pos.z = 4.5f; w.pj[0].vel.z = -0.5f; w.pj[0].vel.x = 0.02f;
        run(5, &t, move_me_toward_stick);
        if (sk) CHECK(dec(D_M_OBJROW) >= 1, "%s: pipe along depth 1.6 to the side: dodged (%d)", S, dec(D_M_OBJROW));
        else CHECK(!mania_any(), "%s: pipe 1.6 aside: no Normal decision", S);
        w.npj = 0;
        /* 26. low life: 25% (below RetreatLifePercent 30, above 2/3 of it): Normal keeps fighting; 15%: retreats */
        for (int lv = 0; lv < 2; lv++) {
            world_reset(); bot_reset(1); me = &w.ch[1]; me->life = lv ? 15 : 25;
            mk(2, 50, 1, -1, 1.0f, 1.5f, -1, CS_STANDWALK);
            run(20, &t, NULL);
            if (sk) CHECK((dec(D_M_LOWLIFE) >= 1) == (lv == 1), "%s: life %d%%, enemy near: %s (%d)", S, lv ? 15 : 25, lv ? "retreat" : "keeps fighting", dec(D_M_LOWLIFE));
        }
        /* 27. sloped floor (both skills): an edge learned at x 0 (z 1.6); an enemy at x 6, z 2.6 is chased (v1.1: skipped
           within 8 x); an enemy right past the edge at x 0.5, z 2.9 is still left alone */
        { int ofl[2];
          for (int k = 0; k < 2; k++) {
              world_reset(); bot_reset(1); w.ch[0].pos.x = -8.0f;
              mk(2, 50, 1, -1, k ? 0.5f : 6.0f, k ? 2.9f : 2.6f, -1, CS_STANDWALK);
              run(12, &t, NULL); dbg_floor_set_wall(1, 1.6f, 0.0f);
              run(150, &t, NULL); ofl[k] = dec(D_OFFFLOOR);
          }
          CHECK(ofl[0] == 0 && ofl[1] >= 1, "%s: sloped floor: enemy 6 x from the learned edge chased (off-floor %d), right past it skipped (%d)", S, ofl[0], ofl[1]); }

        /* 28a. breakables: interrupted while lining up depth (1.5 lanes off) -> no stale breakZAt air-punching; breaks once free */
        world_reset(); bot_reset(1); g_me = me = &w.ch[1]; me->pos.x = -2.0f; me->pos.z = 3.0f; w.ch[0].pos.x = -0.5f; w.ch[0].pos.z = 3.5f;
        w.nbr = 1; w.br[0].uid = 90; w.br[0].life = 2; w.br[0].entered = 1; w.br[0].pos.x = 0.0f; w.br[0].pos.z = 1.5f;
        g_pinZ = 1; g_brkAirX = 0; g_brkLastX = 0;
        run(12, &t, move_brk_check);
        /* interrupt breakables for 400 ms with a pickup, keeping z pinned at 3.0 */
        w.npk = 1; w.pk[0].addr = 0x3100; w.pk[0].kind = PK_SCORE; w.pk[0].pos.x = -2.0f; w.pk[0].pos.z = 3.0f;
        run(25, &t, move_brk_check);
        w.npk = 0; g_pinZ = 0;
        run(240, &t, move_brk_check);
        CHECK(g_brkAirX == 0 && w.br[0].life <= 0, "%s: breakable interrupted during z-align: no air punches (%d) and broken (%d)", S, g_brkAirX, w.br[0].life <= 0);

        /* 28b. breakables: z stuck 1.1 lanes away -> Normal caps wall dz (no air punches); Easy unchanged */
        world_reset(); bot_reset(1); g_me = me = &w.ch[1]; me->pos.x = -2.0f; me->pos.z = 2.6f; w.ch[0].pos.x = -0.5f; w.ch[0].pos.z = 3.5f;
        w.nbr = 1; w.br[0].uid = 92; w.br[0].life = 2; w.br[0].entered = 1; w.br[0].pos.x = 0.0f; w.br[0].pos.z = 1.5f;
        g_pinZ = 1; g_brkAirX = 0; g_brkLastX = 0;
        run(70, &t, move_brk_check);
        g_pinZ = 0;
        if (sk) CHECK(g_brkAirX == 0, "%s: breakable stuck 1.1 lanes off: no air punches (%d)", S, g_brkAirX);
        else CHECK(g_brkAirX >= 0, "%s: Easy breakable z-wall behavior kept (%d)", S, g_brkAirX);

        /* 28c. breakables in/blocked by a trap zone (v1.2-run1 uid 20 + hazard 7): Normal skips, Easy unchanged */
        world_reset(); bot_reset(1); g_me = me = &w.ch[1];
        w.nhz = 1; w.hz[0].uid = 7; w.hz[0].pos.x = 0.07f; w.hz[0].pos.z = -0.15f; w.hz[0].haveZone = 1;
        w.hz[0].x0 = -3.3f; w.hz[0].x1 = 1.1f; w.hz[0].z0 = -0.5f; w.hz[0].z1 = 0.3f;
        me->pos.x = -2.0f; me->pos.z = 0.0f;
        run(5, &t, NULL);
        me->hitSrc = 7; me->life = 80; me->state = CS_STANDHIT;
        run(2, &t, NULL);
        me->state = CS_STANDWALK; me->hitSrc = 0; me->pos.x = -1.0f; me->pos.z = 1.2f; w.ch[0].pos.x = -2.0f; w.ch[0].pos.z = 2.0f;
        w.nbr = 1; w.br[0].uid = 20; w.br[0].life = 1; w.br[0].entered = 1; w.br[0].pos.x = -3.8f; w.br[0].pos.z = -1.0f;
        run(120, &t, move_me_toward_stick);
        if (sk) CHECK(dec(D_BREAK) == 0 && t.btn[BTN_X] == 0, "%s: breakable behind trap zone: skipped (break %d, X %d)", S, dec(D_BREAK), t.btn[BTN_X]);
        else CHECK(dec(D_BREAK) >= 1, "%s: Easy breakable behind trap zone unchanged (break %d)", S, dec(D_BREAK));

        /* 29a. food: Survival cleared (portal open), 4 apples, bot at 81% (>75%), P1 at 100%: Normal eats, Easy unchanged */
        dbg_surv_reset(); world_reset(); bot_reset(1); g_me = me = &w.ch[1]; w.survActive = 1;
        w.nact = 1; w.act[0].uid = 30; w.act[0].isPortal = 1; w.act[0].state = 1; w.act[0].pos.x = 0.0f; w.act[0].pos.z = -1.0f;
        me->life = 81; w.ch[0].life = 100; me->pos.x = 0.0f; me->pos.z = 1.5f;
        w.npk = 4;
        for (int i = 0; i < 4; i++) { w.pk[i].addr = (uintptr_t)(0x4000 + i); w.pk[i].kind = PK_FOOD; w.pk[i].lifeGain = 30; w.pk[i].pos.x = 1.2f + i * 1.5f; w.pk[i].pos.z = 1.5f; }
        run(120, &t, move_me_toward_stick);
        if (sk) CHECK(t.btn[BTN_B] >= 1 && dec(D_FOOD) >= 1, "%s: Survival cleared, 81%% life, 4 apples: picked one (B %d, food %d)", S, t.btn[BTN_B], dec(D_FOOD));
        else CHECK(t.btn[BTN_B] == 0 && dec(D_FOOD) == 0, "%s: Easy 81%% life (>75%%): food left alone (B %d)", S, t.btn[BTN_B]);

        /* 29b. food: mid-wave, only 1 apple, P1 at 60%, bot at 81%: left for P1 */
        dbg_surv_reset(); world_reset(); bot_reset(1); g_me = me = &w.ch[1]; w.survActive = 1;
        me->life = 81; w.ch[0].life = 60; me->pos.x = 0.0f; me->pos.z = 1.5f;
        w.npk = 1; w.pk[0].addr = 0x4100; w.pk[0].kind = PK_FOOD; w.pk[0].lifeGain = 30; w.pk[0].pos.x = 1.0f; w.pk[0].pos.z = 1.5f;
        run(100, &t, move_me_toward_stick);
        CHECK(t.btn[BTN_B] == 0 && dec(D_FOOD) == 0, "%s: 1 apple, P1 60%%, bot 81%%: left for P1 (B %d)", S, t.btn[BTN_B]);

        /* 29c. food: mid-wave, 2 apples, P1 at 85%, bot at 81%: enough safe food for everyone -> Normal eats, Easy unchanged */
        dbg_surv_reset(); world_reset(); bot_reset(1); g_me = me = &w.ch[1]; w.survActive = 1;
        me->life = 81; w.ch[0].life = 85; me->pos.x = 0.0f; me->pos.z = 1.5f;
        w.npk = 2;
        for (int i = 0; i < 2; i++) { w.pk[i].addr = (uintptr_t)(0x4200 + i); w.pk[i].kind = PK_FOOD; w.pk[i].lifeGain = 30; w.pk[i].pos.x = 1.0f + i * 2.0f; w.pk[i].pos.z = 1.5f; }
        run(120, &t, move_me_toward_stick);
        if (sk) CHECK(t.btn[BTN_B] >= 1 && dec(D_FOOD) >= 1, "%s: 2 apples, P1 85%%, bot 81%%: Normal eats one (B %d, food %d)", S, t.btn[BTN_B], dec(D_FOOD));
        else CHECK(t.btn[BTN_B] == 0 && dec(D_FOOD) == 0, "%s: Easy 81%% life (>75%%): food left alone (B %d)", S, t.btn[BTN_B]);

        /* 30a. v1.3: 20 s against one standing enemy: Normal mixes varied / longer strings in, Easy never does */
        world_reset(); bot_reset(1); g_me = me = &w.ch[1]; me->pos.x = 0.0f; me->pos.z = 1.5f; me->life = 100;
        mk(2, 160, 1, -1, 0.9f, 1.5f, -1, CS_STANDHIT); w.ch[2].cantBeGrabbed = 1;
        run(60 * 20, &t, NULL);
        if (sk) CHECK(dec(D_M_VARIED) >= 3 && dec(D_COMBO) >= 2 && t.btn[BTN_X] >= 30, "%s: 20 s on one enemy: varied strings %d, plain strings %d, X %d", S, dec(D_M_VARIED), dec(D_COMBO), t.btn[BTN_X]);
        else CHECK(dec(D_M_VARIED) == 0 && dec(D_COMBO) >= 5 && !mania_any(), "%s: 20 s on one enemy: plain strings only (%d), varied %d", S, dec(D_COMBO), dec(D_M_VARIED));
        /* 30b. v1.3: a star 1.2 away, a knocked-down enemy 1.5 away (inside the old 1.5-range enemy gate), 7 s head start:
           Normal takes it after 7 s (not before); Easy keeps the old money rule, only the 7 s timer */
        world_reset(); bot_reset(1); g_me = me = &w.ch[1]; me->pos.x = 0.0f; me->pos.z = 1.5f; C.itemHeadStartMs = 7000;
        mk(2, 161, 1, -1, -1.5f, 1.5f, 1, CS_ONGROUND);   /* knocked down next to us: still counts as near */
        w.npk = 1; w.pk[0].addr = 0x5100; w.pk[0].kind = PK_STAR; w.pk[0].starGain = 1; w.pk[0].life = 1; w.pk[0].pos.x = 1.2f; w.pk[0].pos.z = 1.5f;
        run(60 * 6, &t, mv_take);
        { int bEarly = 0; for (int i = 0; i < 1; i++) bEarly = t.btn[BTN_B];
          w.ch[1].pos.x = 0.0f; w.ch[2].pos.x = -1.5f;
          run(60 * 3, &t, mv_take);
          if (sk) CHECK(bEarly == 0 && t.btn[BTN_B] >= 1, "%s: star (enemy 1.5 away): B before 7 s %d, after %d", S, bEarly, t.btn[BTN_B]);
          else CHECK(bEarly == 0, "%s: Easy star: not before the 7 s head start (B %d, then %d with the old money rule)", S, bEarly, t.btn[BTN_B]); }
        C.itemHeadStartMs = 0;
        /* 31. v1.3 stuck-on-walls watchdog (both skills: a bug fix). a) following P1 into a wall: depth step, hop, give
           up for 3 s (stands), no left/right flip-flop; b) the wall goes: walks on, no more stuck notes; c) free walking:
           never fires; d) food behind a wall: given up and marked bad (not pushed at again); e) standing next to P1 /
           attacking an enemy with the world frozen: never fires; f) approaching an enemy behind a wall: left to the
           approach's own stuck logic */
        world_reset(); bot_reset(1); g_me = me = &w.ch[1]; me->pos.x = 0.0f; w.ch[0].pos.x = -8.0f; g_wallX = -1.0f; g_wdStep = g_wdNeutral = 0;
        run(60 * 5, &t, mv_wall);
        { int st = dec(D_WALLSTUCK), hop = t.btn[BTN_A], rv = t.rev, stp = g_wdStep, nt = g_wdNeutral; uint64_t gu = bb[0].wdGiveUntil;
          CHECK(st >= 1 && stp >= 20 && hop >= 1 && hop <= 2 && gu > 0 && nt >= 60 && rv == 0 && me->pos.x <= -0.99f,
                "%s: walled in following P1 (5 s): stuck notes %d, depth-step frames %d, hops %d, gave up %d (idle frames %d), x flips %d", S, st, stp, hop, gu > 0, nt, rv);
          g_wallX = -1e9f; run(60 * 4, &t, move_me_toward_stick);
          CHECK(dec(D_WALLSTUCK) == st && me->pos.x < -4.0f && t.rev == 0, "%s: wall gone: walked on to x %.1f, new stuck notes %d, x flips %d", S, me->pos.x, dec(D_WALLSTUCK) - st, t.rev); }
        world_reset(); bot_reset(1); g_me = me = &w.ch[1]; me->pos.x = 0.0f; w.ch[0].pos.x = -8.0f;
        run(60 * 4, &t, move_me_toward_stick);
        CHECK(dec(D_WALLSTUCK) == 0 && me->pos.x < -4.0f, "%s: free walk to P1: no stuck note (%d), x %.1f", S, dec(D_WALLSTUCK), me->pos.x);
        world_reset(); bot_reset(1); g_me = me = &w.ch[1]; me->pos.x = 0.0f; me->life = 40; w.ch[0].pos.x = 1.5f; g_wallX = -1.0f; g_wdStep = g_wdNeutral = 0;
        w.npk = 1; w.pk[0].addr = 0x4400; w.pk[0].kind = PK_FOOD; w.pk[0].lifeGain = 30; w.pk[0].pos.x = -4.0f; w.pk[0].pos.z = 1.5f;
        run(60 * 5, &t, mv_wall);
        { int bad = 0; for (int i = 0; i < 8; i++) if (bb[0].badPick[i] == 0x4400 && bb[0].badPickUntil[i] > now) bad = 1;
          g_wdLate = 0; run(60 * 2, &t, frozen);
          CHECK(dec(D_WALLSTUCK) >= 1 && bad && !t.btn[BTN_B] && g_wdLate < 20, "%s: food behind a wall: stuck %d, marked bad %d, then pushed at it %d frames of 120", S, dec(D_WALLSTUCK), bad, g_wdLate); }
        g_wallX = -1e9f;
        world_reset(); bot_reset(1); g_me = me = &w.ch[1]; me->pos.x = 0.0f; w.ch[0].pos.x = -3.0f;
        run(60 * 3, &t, move_me_toward_stick); { float ax = me->pos.x;
        run(60 * 4, &t, NULL);
        CHECK(dec(D_WALLSTUCK) == 0 && t.neutralFrames >= 230, "%s: at its spot by P1 (x %.1f; then 4 s frozen): no stuck note (%d), idle frames %d", S, ax, dec(D_WALLSTUCK), t.neutralFrames); }
        world_reset(); bot_reset(1); g_me = me = &w.ch[1];
        { Chr *e = mk(2, 170, 1, -1, 0.7f, 1.5f, -1, CS_STANDMOVE); e->cantBeGrabbed = 1; }
        run(60 * 6, &t, move_me_toward_stick);
        CHECK(dec(D_WALLSTUCK) == 0 && t.btn[BTN_X] >= 4, "%s: fighting one enemy 6 s: no stuck note (%d), X %d", S, dec(D_WALLSTUCK), t.btn[BTN_X]);
        world_reset(); bot_reset(1); g_me = me = &w.ch[1]; me->pos.x = 0.0f; g_wallX = -1.0f;
        mk(2, 171, 1, -1, -5.0f, 1.5f, 1, CS_STANDMOVE);
        run(60 * 3, &t, mv_wall);
        CHECK(dec(D_WALLSTUCK) == 0 && dec(D_DETOUR) >= 1, "%s: approaching an enemy behind a wall: approach's own detour %d, watchdog %d", S, dec(D_DETOUR), dec(D_WALLSTUCK));
        g_wallX = -1e9f;
    }
    C.botSkill = 0;
    printf("%s (%d failure(s))\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
