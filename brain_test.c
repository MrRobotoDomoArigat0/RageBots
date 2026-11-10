/* m4 offline test of the fighting brain: hand-made World snapshots (no game memory), one scenario at a
 * time, checking which buttons the bot presses. Build: brain_test.c ../sor4brain.c ../sor4state.c ../sor4meta.c */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include "../sor4common.h"
#include "../sor4state.h"
#include "../sor4brain.h"
Config C;
static int g_quiet = 1;
void logf_(int lv, const char *fmt, ...) {
    if (lv > C.logLevel || g_quiet) return;
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n');
}
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
int dbg_trap_zone_count(void); void dbg_trap_reset(void);
static void take_on_b(int fr) { (void)fr; if (out.btn[BTN_B] && w.npk) { w.npk = 0; g_me->heldAddr = 0x2000; } }
static void wt_gone(int fr) { (void)fr; if (out.btn[BTN_B] && out.axis[AX_LX] > 16000) w.ch[1].heldAddr = 0; }   /* m7.2: the thrown weapon leaves our hand */
static int g_p1dx = 0; static void move_me_toward_stick(int fr);
/* m7.3: the far target keeps its distance (5 ahead), P1 moves along (g_p1dx ahead, 0 = stays) */
static void wt_keep5(int fr) { move_me_toward_stick(fr); w.ch[1].pos.z = 1.5f; w.ch[2].pos.x = w.ch[1].pos.x + 5.0f; if (g_p1dx) w.ch[0].pos.x = w.ch[1].pos.x + g_p1dx * 0.5f; }
static void move_me_toward_stick(int fr) { (void)fr; g_me->pos.x += out.axis[AX_LX] / 32767.0f * 0.08f; g_me->pos.z += out.axis[AX_LY] / 32767.0f * 0.04f; }

int main(void) {
    g_brainLearnWalls = 0;   /* m7.4: the test world never moves: every push would look like a wall */
    C.logLevel = LV_DEBUG; C.smart = 1; C.attackButton = BTN_X; C.jumpButton = BTN_A; C.specialButton = BTN_Y; C.useSpecials = 1;
    C.specialMinLifePct = 60; C.defSpecialMinLifePct = 40; C.retreatLifePct = 30; C.retreatMaxMs = 2500; C.comboHits = 4; C.comboTapMs = 110; C.comboPauseMs = 350;
    C.joinButton = BTN_A; C.joinPresses = 2; C.focusGraceMs = 0; C.resumeGraceMs = 0; C.starButton = BTN_RB; C.starMode = 0; C.backAttack = 1;
    C.useStars = 1; C.usePickups = 1; C.useGrabs = 1; C.foodLifePct = 75; C.brainLogMs = 0;
    C.itemHeadStartMs = 0; C.breakObjects = 1; C.jumpAttacks = 1; C.weaponThrows = 1;
    brain_set_focus(1, 0);
    Tally t;

    /* 1. held by an enemy at low life: mash, never Y or B */
    world_reset(); bot_reset(1);
    Chr *me = &w.ch[1]; me->life = 35; me->state = CS_STANDHIT; me->grabbedBy = 50;
    mk(2, 50, 1, -1, 0.5f, 1.5f, -1, CS_STANDMOVE);
    run(60, &t, NULL);
    CHECK(t.stickL > 10 && t.stickR > 10 && t.btn[BTN_X] > 5 && !t.btn[BTN_Y] && !t.btn[BTN_B] && dec(D_GRABESCAPE) == 1,
          "held at 35%% life: mash (stick L %d / R %d frames, X %d presses), Y %d, B %d", t.stickL, t.stickR, t.btn[BTN_X], t.btn[BTN_Y], t.btn[BTN_B]);
    /* 1b. held at 90% life with two enemies near: Y special recovery once */
    world_reset(); bot_reset(1);
    me = &w.ch[1]; me->life = 90; me->state = CS_STANDHIT; me->grabbedBy = 50;
    mk(2, 50, 1, -1, 0.5f, 1.5f, -1, CS_STANDMOVE); mk(3, 51, 1, -1, -0.7f, 1.5f, 1, CS_STANDWALK);
    run(90, &t, NULL);
    CHECK(t.btn[BTN_Y] == 1 && !t.yWithStick && dec(D_SPECIALRECOVER) == 1, "held at 90%% life, 2 enemies near: Y special recovery %d time(s) (stick neutral on Y: %s)", t.btn[BTN_Y], t.yWithStick ? "no" : "yes");

    /* 2. knocked into the air: tap A, nothing else */
    world_reset(); bot_reset(1);
    me = &w.ch[1]; me->state = CS_AIRHIT; me->life = 50;
    mk(2, 50, 1, -1, 2.0f, 1.5f, -1, CS_STANDWALK);
    run(60, &t, NULL);
    CHECK(t.btn[BTN_A] >= 6 && !t.btn[BTN_X] && !t.btn[BTN_Y] && !t.btn[BTN_B] && dec(D_QUICKRECOVER) == 1, "knocked into the air: A taps %d, X %d, Y %d, B %d", t.btn[BTN_A], t.btn[BTN_X], t.btn[BTN_Y], t.btn[BTN_B]);

    /* 3. surrounded with an attack coming, healthy: neutral Y (defensive special), once */
    world_reset(); bot_reset(1);
    mk(2, 50, 1, -1, 0.8f, 1.5f, -1, CS_STANDMOVE)->aiAttacking = 1;
    mk(3, 51, 1, -1, -0.8f, 1.5f, 1, CS_STANDMOVE);
    w.ch[2].aiTarget = 2;
    run(40, &t, NULL);
    CHECK(t.btn[BTN_Y] == 1 && !t.yWithStick && dec(D_DEFSPECIAL) == 1, "surrounded, attack incoming, 100%% life: defensive special %d (neutral stick: %s)", t.btn[BTN_Y], t.yWithStick ? "no" : "yes");
    /* 3b. same at 35% life: no special (below DefensiveSpecialMinLifePercent) */
    world_reset(); bot_reset(1); w.ch[1].life = 35;
    mk(2, 50, 1, -1, 0.8f, 1.5f, -1, CS_STANDMOVE); mk(3, 51, 1, -1, -0.8f, 1.5f, 1, CS_STANDMOVE);
    run(120, &t, NULL);
    CHECK(!t.btn[BTN_Y], "surrounded at 35%% life: Y presses %d (expect 0)", t.btn[BTN_Y]);

    /* 4. a bullet flying at us: neutral jump */
    world_reset(); bot_reset(1);
    mk(2, 50, 1, -1, 6.0f, 1.5f, -1, CS_STANDWALK);
    w.npj = 1; w.pj[0].uid = 900; w.pj[0].team = 1; w.pj[0].pos.x = 2.0f; w.pj[0].pos.z = 1.5f; w.pj[0].vel.x = -0.2f;
    run(10, &t, NULL);
    CHECK(t.btn[BTN_A] == 1 && dec(D_DODGEJUMP) == 1 && !t.btn[BTN_Y], "projectile 10 frames away: neutral jump %d, Y %d", t.btn[BTN_A], t.btn[BTN_Y]);

    /* 5. food: P1 needs it more -> leave it; P1 healthy -> walk there and press B */
    world_reset(); bot_reset(1);
    w.ch[1].life = 40; w.ch[0].life = 30;
    w.npk = 1; w.pk[0].addr = 0x1000; w.pk[0].kind = PK_FOOD; w.pk[0].lifeGain = 30; w.pk[0].pos.x = 1.0f; w.pk[0].pos.z = 1.5f;
    run(120, &t, NULL);
    CHECK(!t.btn[BTN_B] && dec(D_FOODLEFT) == 1 && !dec(D_FOOD), "food, us 40%% / P1 30%%: left for P1 (B %d, noted %d)", t.btn[BTN_B], dec(D_FOODLEFT));
    world_reset(); bot_reset(1);
    w.ch[1].life = 40; w.ch[0].life = 95; g_me = &w.ch[1];
    w.npk = 1; w.pk[0].addr = 0x1000; w.pk[0].kind = PK_FOOD; w.pk[0].lifeGain = 30; w.pk[0].pos.x = 1.0f; w.pk[0].pos.z = 1.5f;
    run(120, &t, move_me_toward_stick);
    CHECK(t.btn[BTN_B] >= 1 && dec(D_FOOD) >= 1 && t.stickR > 0, "food, us 40%% / P1 95%%: walked to it (%d frames right) and pressed B %d", t.stickR, t.btn[BTN_B]);

    /* 6. live grenade at our feet: B, then fwd+B toward the enemy */
    world_reset(); bot_reset(1);
    mk(2, 50, 1, -1, 5.0f, 1.5f, -1, CS_STANDWALK);
    w.npk = 1; w.pk[0].addr = 0x2000; w.pk[0].kind = PK_GRENADE; w.pk[0].ticking = 1; w.pk[0].pos.x = 0.1f; w.pk[0].pos.z = 1.5f;
    g_me = &w.ch[1]; run(50, &t, take_on_b);
    CHECK(t.btn[BTN_B] == 2 && dec(D_GRENADEBACK) == 1 && t.stickR > 0, "live grenade: B presses %d (pick up + throw), stick toward the enemy %d frames", t.btn[BTN_B], t.stickR);

    /* 7. weapon nearby, no enemy close: pick it up */
    world_reset(); bot_reset(1); g_me = &w.ch[1];
    w.npk = 1; w.pk[0].addr = 0x3000; w.pk[0].kind = PK_WEAPON; w.pk[0].pos.x = -1.0f; w.pk[0].pos.z = 1.5f;  w.ch[0].pos.x = -1.5f; w.ch[0].pos.z = 3.5f;   /* m6.2: P1 level with it (bots stay behind P1 when quiet) */
    run(90, &t, move_me_toward_stick);
    CHECK(t.btn[BTN_B] >= 1 && dec(D_WEAPONPICK) >= 1, "weapon on the floor: walked (%d frames left) and pressed B %d", t.stickL, t.btn[BTN_B]);

    /* 7b. a throw-only weapon in hand and a target at distance: fwd+B */
    world_reset(); bot_reset(1);
    w.ch[1].heldAddr = 0x5000; bb[0].heldAddr = 0x5000; bb[0].heldOnlyThrow = 1;
    mk(2, 50, 1, -1, 3.5f, 1.5f, -1, CS_STANDWALK);
    { int fb = 0; for (int fr = 0; fr < 20; fr++, now += 16) { w.frames++; brain_think(&bb[0], bb, 3, &w, 1, now, &out); if (out.btn[BTN_B] && out.axis[AX_LX] > 16000) fb++; }
      CHECK(fb > 0 && dec(D_WEAPONTHROW) == 1, "throw-only weapon in hand, target at 3.5: fwd+B (%d frames)", fb); }

    /* m7.2: explosive barrels (stage 3 ship deck) */
    {
        /* a: nothing to fight, an explosive barrel next to P1: not broken (an ordinary crate would be) */
        dbg_trap_reset(); world_reset(); bot_reset(1); g_me = &w.ch[1]; w.scene = 7;
        w.nbr = 1; memset(&w.br[0], 0, sizeof w.br[0]); w.br[0].addr = 0x9000; w.br[0].uid = 300; w.br[0].pos.x = 1.0f; w.br[0].pos.z = 1.5f; w.br[0].life = 4; w.br[0].maxLife = 4;
        w.br[0].explosive = 1; snprintf(w.br[0].name, sizeof w.br[0].name, "Barrel");
        run(150, &t, move_me_toward_stick);
        CHECK(!t.btn[BTN_X] && dec(D_BARREL) >= 1 && dec(D_BREAK) == 0, "m7.2 explosive barrel, nothing to fight: not broken (X %d), noted %d", t.btn[BTN_X], dec(D_BARREL));
        w.br[0].explosive = 0; g_me->pos.x = 0; g_me->pos.z = 1.5f;
        run(150, &t, move_me_toward_stick);
        CHECK(t.btn[BTN_X] > 0, "m7.2 control: the same object when ordinary is broken (X %d)", t.btn[BTN_X]);
        /* b: barrel hit (fuse timer running) 1 unit from us, enemy beside it: the bot walks out of 3.5 x / 2 z and stays out */
        dbg_trap_reset(); world_reset(); bot_reset(1); g_me = &w.ch[1]; w.scene = 8; run(170, &t, NULL);
        w.nbr = 1; memset(&w.br[0], 0, sizeof w.br[0]); w.br[0].addr = 0x9000; w.br[0].uid = 301; w.br[0].pos.x = 1.0f; w.br[0].pos.z = 1.5f; w.br[0].life = 3; w.br[0].maxLife = 4;
        w.br[0].explosive = 1; w.br[0].blowTimer = 90;
        mk(2, 60, 1, -1, 1.8f, 1.5f, -1, CS_STANDWALK);
        { int xIn = 0, outAt = -1, back = 0; float minD = 99;
          for (int fr = 0; fr < 150; fr++, now += 16) { w.frames++; brain_think(&bb[0], bb, 3, &w, 1, now, &out);
              g_me->pos.x += out.axis[AX_LX] / 32767.0f * 0.065f; g_me->pos.z += out.axis[AX_LY] / 32767.0f * 0.032f;
              int inside = fabsf(g_me->pos.x - 1.0f) < 3.5f && fabsf(g_me->pos.z - 1.5f) < 2.0f;
              if (inside && out.btn[BTN_X] && fr > 5) xIn++;
              if (!inside && outAt < 0) outAt = fr;
              if (outAt >= 0 && inside) back++; }
          (void)minD;
          CHECK(outAt >= 0 && outAt < 120 && xIn == 0 && back == 0 && dec(D_BARREL) == 1, "m7.2 lit barrel 1 unit away: out of the blast area at frame %d (now at %.1f, %.1f), X presses inside %d, frames back inside %d, noted %d", outAt, g_me->pos.x, g_me->pos.z, xIn, back, dec(D_BARREL)); }
        /* c: it exploded (gone from the list): the zone ends */
        w.nbr = 0; run(40, &t, NULL);
        CHECK(dbg_trap_zone_count() == 0, "m7.2 barrel gone (exploded): blast zone dropped (%d zones)", dbg_trap_zone_count());
        /* d: life dropped (hit) with no fuse timer readable: zone too, ends after ~3 s */
        dbg_trap_reset(); world_reset(); bot_reset(1); g_me = &w.ch[1]; w.scene = 9;
        w.nbr = 1; memset(&w.br[0], 0, sizeof w.br[0]); w.br[0].addr = 0x9000; w.br[0].uid = 302; w.br[0].pos.x = 6.0f; w.br[0].pos.z = 1.5f; w.br[0].life = 4; w.br[0].maxLife = -1; w.br[0].explosive = 1;
        run(10, &t, NULL); int z0 = dbg_trap_zone_count();
        w.br[0].life = 3; run(10, &t, NULL); int z1 = dbg_trap_zone_count();
        run(200, &t, NULL); int z2 = dbg_trap_zone_count();
        CHECK(z0 == 0 && z1 == 1 && z2 == 0, "m7.2 barrel life 4 -> 3 (no fuse timer): zones before %d, after the hit %d, 3.4 s later %d", z0, z1, z2);
    }
    /* m7.2: stage 2 jail cells (m7.1-run1 17:25:11): prisoners behind the bars read btRoot 1, standing still */
    {
        world_reset(); bot_reset(1);
        w.ch[0].pos.x = -1.0f; w.ch[0].pos.z = 1.5f;
        Chr *pr[3]; float px[3] = { 2.6f, 5.7f, 12.2f }, pz[3] = { -4.8f, -5.7f, -5.1f };
        for (int i = 0; i < 3; i++) { pr[i] = mk(2 + i, 41 + i, 1, -1, px[i], pz[i], 1, CS_STANDWALK); pr[i]->btRoot = 1; pr[i]->lineOfSight = 1; }
        int up = 0, xs = 0; uint32_t tg = 0;
        for (int fr = 0; fr < 180; fr++, now += 16) { w.frames++; brain_think(&bb[0], bb, 3, &w, 1, now, &out); if (out.axis[AX_LY] < -16000) up++; if (out.btn[BTN_X]) xs++; if (bb[0].targetUid) tg = bb[0].targetUid; }
        CHECK(!up && !xs && !tg && dec(D_CAGED) >= 1, "m7.2 3 prisoners behind the bars (btRoot 1, still): no target (%u), no walking up to the bars (%d frames), noted %d", tg, up, dec(D_CAGED));
        /* released: the door opens, root 0, it walks out -> fought */
        pr[0]->btRoot = 0; pr[0]->stickY = 1.0f; pr[0]->pos.z = 0.5f; pr[0]->pos.x = 1.5f;
        tg = 0; for (int fr = 0; fr < 60; fr++, now += 16) { w.frames++; brain_think(&bb[0], bb, 3, &w, 1, now, &out); if (bb[0].targetUid) tg = bb[0].targetUid; }
        CHECK(tg == 41, "m7.2 prisoner released (root 0, walking out): targeted (uid %u)", tg);
        /* a waiting enemy (root 1, still) right next to P1: P1 is on it, so it counts */
        world_reset(); bot_reset(1);
        w.ch[0].pos.x = 1.0f; w.ch[0].pos.z = 1.5f;
        Chr *wt = mk(2, 50, 1, -1, 2.2f, 1.6f, -1, CS_STANDWALK); wt->btRoot = 1;
        tg = 0; for (int fr = 0; fr < 60; fr++, now += 16) { w.frames++; brain_think(&bb[0], bb, 3, &w, 1, now, &out); if (bb[0].targetUid) tg = bb[0].targetUid; }
        CHECK(tg == 50, "m7.2 a waiting enemy next to P1: targeted (uid %u)", tg);
    }
    /* m7.3: melee weapon throws (m7.2-run1: 38 throws for 48 pickups; now mostly fought with) */
    {
        #define WT_SETUP(tx) do { world_reset(); bot_reset(1); w.ch[1].heldAddr = 0x5000; snprintf(w.ch[1].heldName, sizeof w.ch[1].heldName, "WPN_LEAD_PIPE"); \
            w.ch[1].heldLife = 150; w.ch[1].heldMaxLife = 150; w.ch[0].pos.x = 4.5f; w.ch[0].pos.z = 3.5f; mk(2, 50, 1, -1, (tx), 1.5f, -1, CS_STANDWALK); } while (0)
        int fb, first;
        #define WT_RUN(n, each) do { fb = 0; first = -1; for (int fr = 0; fr < (n); fr++, now += 16) { void (*e_)(int) = (each); if (e_) e_(fr); wt_gone(fr); w.frames++; \
            brain_think(&bb[0], bb, 3, &w, 1, now, &out); if (out.btn[BTN_B] && out.axis[AX_LX] > 16000) { fb++; if (first < 0) first = fr; } } } while (0)
        /* a: pipe, target at 3.5 (barely out of reach, the m7.2 case): kept for 12 s */
        WT_SETUP(3.5f);
        WT_RUN(750, NULL);
        CHECK(!fb, "m7.3 pipe, target at 3.5: kept and not thrown in 12 s (fwd+B %d)", fb);
        /* b: target far (5.0), nobody in reach: not before 10 s held, then thrown */
        WT_SETUP(5.0f); g_me = &w.ch[1]; g_p1dx = 9;
        WT_RUN(560, wt_keep5);
        { int early = fb; WT_RUN(150, wt_keep5);
          if (getenv("DBG")) printf("  why: %s (meleeNearAt %llu now %llu)\n", bb[0].why, (unsigned long long)bb[0].meleeNearAt, (unsigned long long)now);
          CHECK(!early && fb > 0 && bb[0].throwsDone == 1, "m7.3 pipe, target at 5.0, nobody in reach: no throw before 9 s (%d), thrown after 10 s (fwd+B %d)", early, fb); }
        /* c: toggle off */
        WT_SETUP(5.0f); C.weaponThrows = 0;
        WT_RUN(750, NULL);
        CHECK(!fb, "m7.3 WeaponThrows=0: pipe kept for 12 s (fwd+B %d)", fb);
        C.weaponThrows = 1;
        /* d: an enemy in melee range behind us: never */
        WT_SETUP(5.0f); mk(3, 51, 1, -1, -0.7f, 1.5f, 1, CS_STANDWALK);
        WT_RUN(750, NULL);
        CHECK(!fb, "m7.3 enemy in melee range: no weapon throw in 12 s (fwd+B %d)", fb);
        /* e: P1 in the throw lane: friendly fire on / unknown: no; off: thrown; P1 two lanes away: thrown */
        WT_SETUP(5.0f); g_me = &w.ch[1]; g_p1dx = 4; w.ch[0].pos.z = 1.5f; w.friendlyFire = 1;
        WT_RUN(750, wt_keep5);
        CHECK(!fb && dec(D_WEAPONTHROW) >= 1, "m7.3 P1 in the lane, friendly fire on: no throw (fwd+B %d), noted %d", fb, dec(D_WEAPONTHROW));
        WT_SETUP(5.0f); g_me = &w.ch[1]; g_p1dx = 4; w.ch[0].pos.z = 1.5f; w.friendlyFire = -1;
        WT_RUN(750, wt_keep5);
        CHECK(!fb, "m7.3 P1 in the lane, friendly fire unknown: no throw (fwd+B %d)", fb);
        WT_SETUP(5.0f); g_me = &w.ch[1]; g_p1dx = 4; w.ch[0].pos.z = 1.5f; w.friendlyFire = 0;
        WT_RUN(750, wt_keep5);
        CHECK(fb > 0, "m7.3 P1 in the lane, friendly fire off: thrown (fwd+B %d)", fb);
        WT_SETUP(5.0f); g_me = &w.ch[1]; g_p1dx = 4; w.ch[0].pos.z = 3.5f; w.friendlyFire = 1;
        WT_RUN(750, wt_keep5);
        CHECK(fb > 0, "m7.3 P1 two lanes away, friendly fire on: thrown (fwd+B %d)", fb);
        /* f: knife: when its 40% roll allows, thrown at >= 4 units after ~1 s; otherwise kept like any weapon; never at 3.5 */
        WT_SETUP(4.5f); snprintf(w.ch[1].heldName, sizeof w.ch[1].heldName, "WPN_KNIFE"); w.ch[1].heldKnife = 1;
        WT_RUN(1, NULL); bb[0].heldKnifeOk = 1;
        WT_RUN(120, NULL);
        CHECK(fb > 0 && first < 80, "m7.3 knife (roll ok), target at 4.5: thrown early (frame %d)", first);
        WT_SETUP(4.5f); snprintf(w.ch[1].heldName, sizeof w.ch[1].heldName, "WPN_KNIFE"); w.ch[1].heldKnife = 1;
        WT_RUN(1, NULL); bb[0].heldKnifeOk = 0;
        WT_RUN(400, NULL);
        CHECK(!fb, "m7.3 knife (roll says keep), target at 4.5: kept 6.4 s (fwd+B %d)", fb);
        WT_SETUP(3.5f); snprintf(w.ch[1].heldName, sizeof w.ch[1].heldName, "WPN_KNIFE"); w.ch[1].heldKnife = 1;
        WT_RUN(1, NULL); bb[0].heldKnifeOk = 1;
        WT_RUN(400, NULL);
        CHECK(!fb, "m7.3 knife (roll ok), target at 3.5 (< 4): kept (fwd+B %d)", fb);
        { int ok = 0; world_reset(); bot_reset(1); mk(2, 50, 1, -1, 30.0f, 1.5f, -1, CS_STANDWALK); snprintf(w.ch[1].heldName, sizeof w.ch[1].heldName, "WPN_KNIFE"); w.ch[1].heldKnife = 1;
          for (int k = 0; k < 200; k++) { w.ch[1].heldAddr = 0x6000 + 16 * k; w.frames++; now += 16; brain_think(&bb[0], bb, 3, &w, 1, now, &out); ok += bb[0].heldKnifeOk; }
          CHECK(ok >= 60 && ok <= 100, "m7.3 knives allowed to be thrown: %d of 200 pickups (about 40%%)", ok); }
        /* g: worn out (durability readable): thrown at <= 30% life at a target in lane, even close */
        WT_SETUP(0.8f); w.ch[1].heldLife = 40; w.ch[1].heldMaxLife = 40;
        { int lastX = 0, hits = 0, lifeAtThrow = -1; fb = 0; first = -1;
          for (int fr = 0; fr < 300; fr++, now += 16) {
              wt_gone(fr);
              if (out.btn[BTN_X] && !lastX && w.ch[1].heldAddr && w.ch[1].heldLife > 0) { w.ch[1].heldLife -= 4; hits++; } lastX = out.btn[BTN_X];
              w.frames++; brain_think(&bb[0], bb, 3, &w, 1, now, &out);
              if (out.btn[BTN_B] && out.axis[AX_LX] > 16000) { fb++; if (first < 0) { first = fr; lifeAtThrow = (int)w.ch[1].heldLife; } }
          }
          CHECK(fb > 0 && lifeAtThrow > 0 && lifeAtThrow <= 12, "m7.3 worn out: pipe life 40 -> %d after %d hits, thrown before it breaks (frame %d)", lifeAtThrow, hits, first); }
        /* h: per-bot cooldown: a second worn weapon right after a throw is kept; after 15 s it may go */
        WT_SETUP(0.8f); w.ch[1].heldLife = 10; w.ch[1].heldMaxLife = 100;
        WT_RUN(60, NULL); { int t1 = fb;
          w.ch[1].heldAddr = 0x5100; w.ch[1].heldLife = 10;
          WT_RUN(300, NULL); int t2 = fb;
          now += 12000; w.ch[1].heldAddr = 0x5200; w.ch[1].heldLife = 10;
          WT_RUN(60, NULL);
          CHECK(t1 > 0 && !t2 && fb > 0, "m7.3 throw cooldown: worn weapon thrown (%d), next one within 15 s kept (%d), after 15 s thrown (%d)", t1, t2, fb); }
        /* i: scripted scene / paused: never */
        WT_SETUP(5.0f); w.ch[1].scripted = 1;
        WT_RUN(750, NULL);
        CHECK(!fb, "m7.3 scripted: no throw (fwd+B %d)", fb);
        WT_SETUP(5.0f); w.paused = 1;
        WT_RUN(750, NULL);
        CHECK(!fb, "m7.3 paused (menu open): no throw (fwd+B %d)", fb);
    }
    /* 8. holding an enemy, more enemies behind: 2 knees then the over-the-shoulder throw (back + X) */
    world_reset(); bot_reset(1);
    me = &w.ch[1]; me->grabbing = 1; me->grabUid = 50; me->facing = 1;
    mk(2, 50, 1, -1, 0.4f, 1.5f, -1, CS_STANDHIT);
    mk(3, 51, 1, -1, -3.0f, 1.5f, 1, CS_STANDWALK); mk(4, 52, 1, -1, -4.0f, 1.8f, 1, CS_STANDWALK);
    int backX = 0;
    { int lastX = 0;
      for (int fr = 0; fr < 70; fr++, now += 16) { w.frames++; brain_think(&bb[0], bb, 3, &w, 1, now, &out);
          if (out.btn[BTN_X] && !lastX && out.axis[AX_LX] < -16000) backX++; lastX = out.btn[BTN_X]; } }
    CHECK(dec(D_GRABHIT) == 2 && dec(D_THROW) >= 1 && backX >= 1, "holding an enemy, 2 more behind us: knees %d, throws %d (back+X presses %d)", dec(D_GRABHIT), dec(D_THROW), backX);
    /* 8b. Max: piledriver (A) when the group is in front */
    world_reset(); bot_reset(20);  /* roster index 19 = Max (playable = index + 1); roster isn't readable here, so name by profile */
    brain_set_character(&bb[0], 19, "Max");
    me = &w.ch[1]; me->grabbing = 1; me->grabUid = 50; me->facing = 1;
    mk(2, 50, 1, -1, 0.4f, 1.5f, -1, CS_STANDHIT); mk(3, 51, 1, -1, 3.0f, 1.5f, -1, CS_STANDWALK);
    bb[0].profFor = 19; w.ps[1].playable = 0;
    run(70, &t, NULL);
    CHECK(dec(D_GRABSPECIAL) >= 1 && t.btn[BTN_A] >= 1, "Max holding an enemy: piledriver (A) %d", t.btn[BTN_A]);

    /* 9. safety: paused -> neutral, never B; unfocused -> neutral */
    world_reset(); bot_reset(1);
    w.npk = 1; w.pk[0].addr = 0x2000; w.pk[0].kind = PK_GRENADE; w.pk[0].ticking = 1; w.pk[0].pos.x = 0.1f; w.pk[0].pos.z = 1.5f;
    w.paused = 1; w.pausedStatus = 1;
    run(60, &t, NULL);
    CHECK(t.neutralFrames == 60, "paused with a grenade at our feet: neutral %d of 60 frames", t.neutralFrames);
    w.paused = 0; brain_set_focus(0, now);
    run(60, &t, NULL);
    CHECK(t.neutralFrames == 60, "unfocused: neutral %d of 60 frames", t.neutralFrames);
    brain_set_focus(1, now - 100000);
    /* level not live (frozen): neutral */
    w.live = 0; w.frozenMs = 500;
    run(30, &t, NULL);
    CHECK(t.neutralFrames == 30, "stage not advancing: neutral %d of 30 frames", t.neutralFrames);

    /* 10. stage intro: nothing sent while session mode is 1 or the game walks our fighter; m6: then identified
       from the player slot with NO stick-check wiggle */
    world_reset(); bot_reset(1); bb[0].id.verifiedSlot = -1; bb[0].identified = 0; bb[0].introDone = 0;
    w.mode = 1; w.ch[1].stickX = 1.0f; w.ch[0].stickX = 1.0f;
    run(60 * 4, &t, NULL);
    int introNeutral = t.neutralFrames;
    w.mode = 0;
    run(60 * 1, &t, NULL); int walkInNeutral = t.neutralFrames;      /* stick still moved by the game */
    w.ch[1].stickX = 0; w.ch[0].stickX = 0;
    Tally t2; run(30, &t2, NULL);                                     /* 0.5 s quiet: still waiting */
    Tally t3; run(60, &t3, NULL);                                     /* after 1 s quiet: m6: identified, no wiggle */
    CHECK(introNeutral == 240 && walkInNeutral == 60 && t2.neutralFrames == 30 && bb[0].identified && bb[0].id.verifiedSlot == 1 && t3.rev == 0 && bb[0].mode != 2 /* M_IDENT */,
          "intro: neutral %d/240 (mode 1), %d/60 (game moving our fighter), %d/30 (settling); m6: then identified %d (slot %d) with no wiggle (stick reversals %d, mode %d)",
          introNeutral, walkInNeutral, t2.neutralFrames, bb[0].identified, bb[0].id.verifiedSlot, t3.rev, bb[0].mode);

    /* 11. health budget: Floyd at 50% life never uses fwd+Y; at 100% once, then not again for a while */
    world_reset(); bot_reset(4); brain_set_character(&bb[0], 3, "Floyd"); bb[0].profFor = 3; w.ps[1].playable = 0;
    w.ch[1].life = 50; mk(2, 50, 1, -1, 2.6f, 1.5f, -1, CS_STANDWALK);
    run(120, &t, NULL);
    CHECK(!t.btn[BTN_Y], "Floyd at 50%% life, enemy at mid range: Y %d (expect 0)", t.btn[BTN_Y]);
    world_reset(); bot_reset(4); brain_set_character(&bb[0], 3, "Floyd"); bb[0].profFor = 3; w.ps[1].playable = 0;
    mk(2, 50, 1, -1, 2.6f, 1.5f, -1, CS_STANDWALK);
    run(60 * 3, &t, NULL);
    CHECK(t.btn[BTN_Y] == 1 && dec(D_PULL) == 1, "Floyd at 100%%, enemy at mid range for 3 s: fwd+Y pull %d time(s)", t.btn[BTN_Y]);

    /* 12. star move: boss close and 1 star -> once; no stars -> never */
    world_reset(); bot_reset(1); w.ps[1].stars = 1;
    mk(2, 50, 1, -1, 1.0f, 1.5f, -1, CS_STANDWALK)->boss = 1;
    run(60, &t, NULL);
    CHECK(t.btn[BTN_RB] == 1 && dec(D_STAR) == 1, "boss close, 1 star: star move (RB) %d", t.btn[BTN_RB]);
    world_reset(); bot_reset(1); w.ps[1].stars = 0;
    mk(2, 50, 1, -1, 1.0f, 1.5f, -1, CS_STANDWALK)->boss = 1;
    run(120, &t, NULL);
    CHECK(!t.btn[BTN_RB], "boss close, no stars: RB %d (expect 0)", t.btn[BTN_RB]);
    world_reset(); bot_reset(1); w.ps[1].stars = 2; w.ps[1].starBtn = 0; w.ps[1].starYB = 1;       /* star button switched off in the game's options */
    mk(2, 50, 1, -1, 1.0f, 1.5f, -1, CS_STANDWALK)->boss = 1;
    { int yb = 0; for (int fr = 0; fr < 30; fr++, now += 16) { w.frames++; brain_think(&bb[0], bb, 3, &w, 1, now, &out); if (out.btn[BTN_Y] && out.btn[BTN_B]) yb++; } 
      CHECK(yb > 0 && !out.btn[BTN_RB], "star button off in the game's options: star move with Y+B (%d frames)", yb); }

    /* 13. back attack: enemy attacking from behind */
    world_reset(); bot_reset(1);
    mk(2, 50, 1, -1, 1.0f, 1.5f, -1, CS_STANDWALK);           /* in front, our target */
    mk(3, 51, 1, -1, -0.8f, 1.5f, 1, CS_STANDMOVE);           /* behind us, attacking someone else */
    w.ch[3].aiTarget = 1;
    run(30, &t, NULL);
    CHECK(t.rt >= 1 && dec(D_BACKATTACK) >= 1, "enemy right behind us: back attack (RT) %d", t.rt);

    /* 14. combos and per-character moves */
    world_reset(); bot_reset(1); brain_set_character(&bb[0], 0, "Axel"); bb[0].profFor = 0; w.ps[1].playable = 0;
    mk(2, 50, 1, -1, 0.9f, 1.5f, -1, CS_STANDHIT); w.ch[2].cantBeGrabbed = 1;
    run(60, &t, NULL);
    CHECK(t.btn[BTN_X] >= 5 && dec(D_COMBO) >= 1, "Axel in range: X presses %d in 1 s (5-hit string)", t.btn[BTN_X]);
    world_reset(); bot_reset(1); brain_set_character(&bb[0], 17, "Shiva"); bb[0].profFor = 17; w.ps[1].playable = 0;
    mk(2, 50, 1, -1, 0.9f, 1.5f, -1, CS_STANDHIT); w.ch[2].cantBeGrabbed = 1;
    { for (int fr = 0; fr < 90; fr++, now += 16) { w.frames++; if (fr == 30) w.ch[2].state = CS_AIRHIT; brain_think(&bb[0], bb, 3, &w, 1, now, &out); } }
    CHECK(dec(D_LAUNCHER) == 1 && dec(D_JUGGLE) == 1, "Shiva: string, launcher seen %d, jump-cancel juggle %d", dec(D_LAUNCHER), dec(D_JUGGLE));
    world_reset(); bot_reset(1);
    mk(2, 50, 1, -1, 2.4f, 1.5f, -1, CS_STANDWALK);
    run(20, &t, NULL);
    CHECK(dec(D_BLITZ) == 1, "default profile, enemy at 2.4 (mid range): blitz %d", dec(D_BLITZ));
    world_reset(); bot_reset(1); brain_set_character(&bb[0], 2, "Cherry"); bb[0].profFor = 2; w.ps[1].playable = 0;
    mk(2, 50, 1, -1, 3.0f, 1.5f, -1, CS_STANDWALK);
    run(20, &t, NULL);
    CHECK(dec(D_RUN) == 1, "Cherry, enemy at 3.0: run + knee %d", dec(D_RUN));
    world_reset(); bot_reset(1); brain_set_character(&bb[0], 4, "Adam"); bb[0].profFor = 4; w.ps[1].playable = 0;
    mk(2, 50, 1, -1, 2.4f, 1.5f, -1, CS_STANDWALK);
    run(20, &t, NULL);
    CHECK(dec(D_DASH) == 1, "Adam, enemy at mid range: dash %d", dec(D_DASH));
    world_reset(); bot_reset(1); brain_set_character(&bb[0], 18, "Estel"); bb[0].profFor = 18; w.ps[1].playable = 0;
    mk(2, 50, 1, -1, 0.9f, 1.5f, -1, CS_STANDWALK); w.ch[2].cantBeGrabbed = 1; mk(3, 51, 1, -1, 1.1f, 1.7f, -1, CS_STANDWALK); w.ch[3].cantBeGrabbed = 1;
    run(60, &t, NULL);
    CHECK(dec(D_STUNGRENADE) == 1, "Estel with 2 enemies close: stun grenade %d", dec(D_STUNGRENADE));
    world_reset(); bot_reset(4); brain_set_character(&bb[0], 3, "Floyd"); bb[0].profFor = 3; w.ps[1].playable = 0;
    me = &w.ch[1]; me->grabbing = 1; me->grabUid = 50;
    mk(2, 50, 1, -1, 0.4f, 1.5f, -1, CS_STANDHIT); mk(3, 51, 1, -1, 1.5f, 1.5f, -1, CS_STANDWALK);
    run(30, &t, NULL);
    CHECK(dec(D_DOUBLEGRAB) == 1 && t.stickR > 5, "Floyd holding one enemy with another close: walks into it (double grab %d, %d frames)", dec(D_DOUBLEGRAB), t.stickR);

    /* 14b. two bots, two enemies: each bot plays its own slot and they pick different targets */
    world_reset(); bot_reset(1);
    w.slot[2].ctrl = 2; w.slot[2].type = 1; mk(2, 3, 0, 2, 0.2f, 2.0f, 1, CS_STANDWALK);
    brain_reset(&bb[1], 1, 3, 3, "Bot3", now);
    bb[1].id.ctrlId = 2; bb[1].id.verifiedSlot = 2; bb[1].id.preKnown = 1; bb[1].id.pre[0] = 0; bb[1].id.npre = 1; bb[1].id.attachAt = now - 60000; bb[1].id.lastSeen = now; bb[1].sawLevel = 1; bb[1].introDone = 1;
    mk(3, 50, 1, -1, 1.0f, 1.6f, -1, CS_STANDWALK); mk(4, 51, 1, -1, 1.3f, 1.9f, -1, CS_STANDWALK);
    { PadOut o2; for (int fr = 0; fr < 30; fr++, now += 16) { w.frames++; brain_think(&bb[0], bb, 3, &w, 1, now, &out); brain_think(&bb[1], bb, 3, &w, 1, now, &o2); }
      CHECK(bb[0].playerSlot == 1 && bb[1].playerSlot == 2 && bb[0].uid == 2 && bb[1].uid == 3 && bb[0].targetUid && bb[1].targetUid && bb[0].targetUid != bb[1].targetUid,
            "two bots: P%d fighter %u -> target %u, P%d fighter %u -> target %u", bb[0].playerSlot + 1, bb[0].uid, bb[0].targetUid, bb[1].playerSlot + 1, bb[1].uid, bb[1].targetUid); }

    /* ---------------- m5 ---------------- */
    /* m5-1 (m5.1 regression): enemies on another CurrentPlane are ordinary enemies (the m5 run: plane 1/2 vs players 0)
       and must be fought; one that never entered the screen is skipped only while another target exists */
    world_reset(); bot_reset(1);
    { Chr *c = mk(2, 60, 1, -1, 0.8f, 1.5f, -1, CS_STANDWALK); c->plane = 2; }
    run(60, &t, NULL);
    CHECK(bb[0].targetUid == 60 && t.btn[BTN_X] > 0, "m5.1: enemy on plane 2 (we are on 0) is fought (target %u, X %d)", bb[0].targetUid, t.btn[BTN_X]);
    world_reset(); bot_reset(1);
    { Chr *d = mk(3, 61, 1, -1, -0.8f, 1.5f, 1, CS_STANDWALK); d->entered = 0; }
    mk(4, 62, 1, -1, 1.0f, 1.5f, -1, CS_STANDWALK);
    run(60, &t, NULL);
    CHECK(bb[0].targetUid == 62 && t.btn[BTN_X] > 0, "m5: not-yet-entered enemy skipped while a normal one is there (target %u, X %d)", bb[0].targetUid, t.btn[BTN_X]);
    w.ch[4].life = 0;
    run(60, &t, NULL);
    CHECK(bb[0].targetUid == 61, "m5.1: filters never leave nothing to hit: the only enemy (never 'entered') is targeted (target %u)", bb[0].targetUid);

    /* m5-2. unreachable (we never move): detour twice, hop twice, then ignored for 60 s (it targets nobody) */
    world_reset(); bot_reset(1);
    mk(2, 63, 1, -1, 2.0f, -3.0f, -1, CS_STANDWALK);
    run(60 * 11, &t, NULL);
    { int i = -1; for (int k = 0; k < 12; k++) if (bb[0].unreach[k].uid == 63) i = k;
      CHECK(dec(D_DETOUR) == 2 && t.btn[BTN_A] == 2 && dec(D_UNREACHABLE) == 1 && i >= 0 && bb[0].unreach[i].until > now + 50000 && bb[0].targetUid != 63,
            "m5: stuck: detours %d, hops %d, unreachable %d (ignored %llu s more)", dec(D_DETOUR), t.btn[BTN_A], dec(D_UNREACHABLE), i >= 0 ? (unsigned long long)((bb[0].unreach[i].until - now) / 1000) : 0ULL); }

    /* m5-3. a desk in the way: the detour moves us along the other axis, and we get there */
    world_reset(); bot_reset(1); g_me = &w.ch[1];
    mk(2, 64, 1, -1, 4.0f, 1.5f, -1, CS_STANDWALK);
    { int fr2; for (fr2 = 0; fr2 < 60 * 8; fr2++, now += 16) {
          w.frames++; brain_think(&bb[0], bb, 3, &w, 1, now, &out);
          float nx = g_me->pos.x + out.axis[AX_LX] / 32767.0f * 0.08f, nz = g_me->pos.z + out.axis[AX_LY] / 32767.0f * 0.04f;
          if (nx > 1.0f && nx < 2.0f && nz > 1.0f && nz < 2.2f) nx = g_me->pos.x;     /* the desk: x 1..2, z 1..2.2 */
          g_me->pos.x = nx; g_me->pos.z = nz;
          if (out.btn[BTN_X] && fabsf(g_me->pos.x - 4.0f) < 1.5f) break; }
      CHECK(dec(D_DETOUR) >= 1 && fr2 < 60 * 8, "m5: desk in the way: detours %d, attacking after %.1f s (at %.1f, %.1f)", dec(D_DETOUR), fr2 / 60.0f, g_me->pos.x, g_me->pos.z); }

    /* m5-4. shield cop blocking from the front: no X strings into the shield; grab try, jump attack, flank */
    world_reset(); bot_reset(1);
    { Chr *c = mk(2, 65, 1, -1, 0.9f, 1.5f, -1, CS_STANDGUARD); c->guard = 1; c->canGuard = 1; }
    run(60 * 5, &t, NULL);
    /* m6.7 (user: full strings into the shield work, it breaks after enough damage; it can't be grabbed while up) */
    CHECK(dec(D_COMBO) >= 3 && dec(D_SHIELDGRAB) == 0 && dec(D_GRABTRY) == 0 && dec(D_JUMPATTACK) == 0 && t.btn[BTN_Y] >= 1 && dec(D_SHIELDTAC) >= 2,
          "m5/m6.7: shield in front: combos %d, grab tries %d/%d (expect 0), jump attacks %d (expect 0), Y specials %d, shield tactic notes %d",
          dec(D_COMBO), dec(D_SHIELDGRAB), dec(D_GRABTRY), dec(D_JUMPATTACK), t.btn[BTN_Y], dec(D_SHIELDTAC));
    w.ch[2].facing = 1; w.ch[2].guard = 0; w.ch[2].state = CS_STANDWALK;       /* turned away: hit it */
    run(60, &t, NULL);
    CHECK(dec(D_COMBO) >= 1 || dec(D_BACKATTACK) >= 1, "m5: shield cop turned away: attacked (combos %d, back attacks %d)", dec(D_COMBO), dec(D_BACKATTACK));

    /* m5-5. money: the human gets a 10 s head start, then the bot picks it up */
    world_reset(); bot_reset(1); C.itemHeadStartMs = 10000;
    w.npk = 1; w.pk[0].addr = 0x7700; w.pk[0].kind = PK_SCORE; w.pk[0].scoreGain = 500; w.pk[0].life = 1; w.pk[0].pos.x = 0.0f; w.pk[0].pos.z = 1.5f;  w.ch[0].pos.x = -0.5f; w.ch[0].pos.z = 3.5f;   /* m6.2: P1 level with it (bots stay behind P1 when quiet) */
    g_me = &w.ch[1]; run(60 * 9, &t, take_on_b);
    int bEarly = t.btn[BTN_B];
    run(60 * 3, &t, take_on_b);
    CHECK(bEarly == 0 && t.btn[BTN_B] == 1 && dec(D_MONEY) == 1, "m5: money: B in the first 9 s %d, after the head start %d", bEarly, t.btn[BTN_B]);
    C.itemHeadStartMs = 0;

    /* m5-6. no enemies: break the sink next to us; with an enemy around, fight instead */
    world_reset(); bot_reset(1);
    w.nbr = 1; w.br[0].uid = 900; w.br[0].life = 10; w.br[0].entered = 1; w.br[0].pos.x = 0.9f; w.br[0].pos.z = 1.5f;  w.ch[0].pos.x = 0.4f; w.ch[0].pos.z = 3.5f;   /* m6.2: P1 level with it (bots stay behind P1 when quiet) */
    run(60 * 2, &t, NULL);
    CHECK(dec(D_BREAK) == 1 && t.btn[BTN_X] >= 3, "m5: breakable with no enemies: X %d", t.btn[BTN_X]);
    mk(2, 66, 1, -1, -0.9f, 1.5f, 1, CS_STANDWALK); w.ch[1].facing = -1;
    run(60, &t, NULL);
    CHECK(bb[0].targetUid == 66, "m5: an enemy shows up: target %u (not the breakable)", bb[0].targetUid);

    /* ---------------- m6 ---------------- */
    /* m6-2a. a solid trash can (body 0.5 wide, 0.3 deep) at (0, 1.6): hits land only from its exact lane (|dz| <= 0.12)
       and within 1.0 to the side. The bot starts below-left of it (the m5.1 log: stuck in front, whiffing). */
    world_reset(); bot_reset(1); g_me = &w.ch[1]; g_me->pos.x = -2.0f; g_me->pos.z = 1.0f; g_me->facing = 1;
    w.nbr = 1; w.br[0].uid = 901; w.br[0].life = 3; w.br[0].entered = 0; w.br[0].pos.x = 0.0f; w.br[0].pos.z = 1.6f;  w.ch[0].pos.x = -0.5f; w.ch[0].pos.z = 3.5f;   /* m6.2: P1 level with it (bots stay behind P1 when quiet) */
    { int xs = 0, xsOff = 0, last = 0, fr; for (fr = 0; fr < 60 * 8 && w.br[0].life > 0; fr++, now += 16) {
          w.frames++; brain_think(&bb[0], bb, 3, &w, 1, now, &out);
          float nx = g_me->pos.x + out.axis[AX_LX] / 32767.0f * 0.08f, nz = g_me->pos.z + out.axis[AX_LY] / 32767.0f * 0.04f;
          if (fabsf(nx) < 0.5f && fabsf(nz - 1.6f) < 0.45f) { nx = g_me->pos.x; nz = g_me->pos.z; }   /* solid */
          if (out.axis[AX_LX]) g_me->facing = out.axis[AX_LX] > 0 ? 1 : -1;
          g_me->pos.x = nx; g_me->pos.z = nz;
          if (out.btn[BTN_X] && !last) { xs++; if (fabsf(g_me->pos.z - 1.6f) <= 0.12f && fabsf(g_me->pos.x) <= 1.0f && g_me->facing == (g_me->pos.x < 0 ? 1 : -1)) w.br[0].life -= 1; else xsOff++; }
          last = out.btn[BTN_X]; }
      CHECK(w.br[0].life <= 0 && dec(D_BREAKALIGN) >= 1 && fr < 60 * 5, "m6: breakable: lined up on its lane from the side (dz %.2f), broken in %.1f s, X %d, X out of the hit zone %d (m6.1: allowed, re-aligns)",
            g_me->pos.z - 1.6f, fr * 0.016f, xs, xsOff); }
    /* m6-2b. hits only from closer (0.7): the first stand distance whiffs -> life unchanged -> re-align closer */
    world_reset(); bot_reset(1); g_me = &w.ch[1]; g_me->pos.x = -2.0f; g_me->pos.z = 1.6f; g_me->facing = 1;
    w.nbr = 1; w.br[0].uid = 902; w.br[0].life = 3; w.br[0].pos.x = 0.0f; w.br[0].pos.z = 1.6f;  w.ch[0].pos.x = -0.5f; w.ch[0].pos.z = 3.5f;   /* m6.2: P1 level with it (bots stay behind P1 when quiet) */
    { int last = 0, fr; for (fr = 0; fr < 60 * 10 && w.br[0].life > 0; fr++, now += 16) {
          w.frames++; brain_think(&bb[0], bb, 3, &w, 1, now, &out);
          float nx = g_me->pos.x + out.axis[AX_LX] / 32767.0f * 0.08f, nz = g_me->pos.z + out.axis[AX_LY] / 32767.0f * 0.04f;
          if (fabsf(nx) < 0.5f && fabsf(nz - 1.6f) < 0.45f) { nx = g_me->pos.x; nz = g_me->pos.z; }
          if (out.axis[AX_LX]) g_me->facing = out.axis[AX_LX] > 0 ? 1 : -1;
          g_me->pos.x = nx; g_me->pos.z = nz;
          if (out.btn[BTN_X] && !last && fabsf(g_me->pos.z - 1.6f) <= 0.12f && fabsf(g_me->pos.x) <= 0.7f) w.br[0].life -= 1;
          last = out.btn[BTN_X]; }
      CHECK(w.br[0].life <= 0 && dec(D_BREAKMISS) >= 1, "m6: breakable: whiffs seen (life unchanged) -> re-aligned %d time(s), broken %d (%.1f s)", dec(D_BREAKMISS), w.br[0].life <= 0, fr * 0.016f); }

    /* m6.1: the m6 live log: a trash can at z 0.0 on the front edge of the floor; the bots can't get below z 0.2,
       so its exact lane is out of reach. They must still swing from the side (hits land within 0.3 in depth). */
    world_reset(); bot_reset(1); g_me = &w.ch[1]; g_me->pos.x = -2.7f; g_me->pos.z = 0.3f; g_me->facing = 1;
    w.ch[0].pos.x = -4.0f; w.ch[0].pos.z = 0.3f;
    w.nbr = 1; w.br[0].uid = 5; w.br[0].life = 1; w.br[0].entered = 1; w.br[0].pos.x = 3.0f; w.br[0].pos.z = 0.0f;  w.ch[0].pos.x = 2.5f; w.ch[0].pos.z = 3.5f;   /* m6.2: P1 level with it (bots stay behind P1 when quiet) */
    { int xs = 0, last = 0, fr; for (fr = 0; fr < 60 * 10 && w.br[0].life > 0; fr++, now += 16) {
          w.frames++; brain_think(&bb[0], bb, 3, &w, 1, now, &out);
          float nx = g_me->pos.x + out.axis[AX_LX] / 32767.0f * 0.08f, nz = g_me->pos.z + out.axis[AX_LY] / 32767.0f * 0.04f;
          if (nz < 0.2f) nz = 0.2f;                                                    /* front edge of the floor */
          if (fabsf(nx - 3.0f) < 0.5f && fabsf(nz) < 0.45f) { nx = g_me->pos.x; nz = g_me->pos.z; }   /* solid */
          if (out.axis[AX_LX]) g_me->facing = out.axis[AX_LX] > 0 ? 1 : -1;
          g_me->pos.x = nx; g_me->pos.z = nz;
          if (out.btn[BTN_X] && !last) { xs++; if (fabsf(g_me->pos.z) <= 0.3f && fabsf(g_me->pos.x - 3.0f) <= 1.2f && g_me->facing == (g_me->pos.x < 3.0f ? 1 : -1)) w.br[0].life -= 2; }
          last = out.btn[BTN_X]; }
      CHECK(w.br[0].life <= 0 && fr < 60 * 6, "m6.1: trash can on the floor edge (its lane out of reach): broken %d in %.1f s, X %d, bot at dz %.2f", w.br[0].life <= 0, fr * 0.016f, xs, g_me->pos.z); }
    /* m6.1: hits land only within 0.15 in depth; the bot starts swinging 0.3 off -> misses -> depth steps -> broken */
    world_reset(); bot_reset(1); g_me = &w.ch[1]; g_me->pos.x = -2.0f; g_me->pos.z = 2.0f; g_me->facing = 1;
    w.nbr = 1; w.br[0].uid = 6; w.br[0].life = 1; w.br[0].pos.x = 0.0f; w.br[0].pos.z = 1.6f;  w.ch[0].pos.x = -0.5f; w.ch[0].pos.z = 3.5f;   /* m6.2: P1 level with it (bots stay behind P1 when quiet) */
    { int fr, last = 0; for (fr = 0; fr < 60 * 14 && w.br[0].life > 0; fr++, now += 16) {
          w.frames++; brain_think(&bb[0], bb, 3, &w, 1, now, &out);
          float nx = g_me->pos.x + out.axis[AX_LX] / 32767.0f * 0.08f, nz = g_me->pos.z + out.axis[AX_LY] / 32767.0f * 0.04f;
          if (nz < 1.9f && fr < 60 * 2) nz = 1.9f;                                    /* another bot in the way at first */
          if (fabsf(nx) < 0.5f && fabsf(nz - 1.6f) < 0.45f) { nx = g_me->pos.x; nz = g_me->pos.z; }
          if (out.axis[AX_LX]) g_me->facing = out.axis[AX_LX] > 0 ? 1 : -1;
          g_me->pos.x = nx; g_me->pos.z = nz;
          if (out.btn[BTN_X] && !last && fabsf(g_me->pos.z - 1.6f) <= 0.15f && fabsf(g_me->pos.x) <= 1.0f) w.br[0].life -= 2;
          last = out.btn[BTN_X]; }
      CHECK(w.br[0].life <= 0, "m6.1: swings from 0.3 off its lane miss -> re-aligned %d time(s) -> broken %d (%.1f s)", dec(D_BREAKMISS), w.br[0].life <= 0, fr * 0.016f); }
    /* m6.1: a breakable that can never be hit: no endless walking, it is dropped */
    world_reset(); bot_reset(1); g_me = &w.ch[1]; g_me->pos.x = -2.0f; g_me->pos.z = 1.6f;
    w.nbr = 1; w.br[0].uid = 7; w.br[0].life = 1; w.br[0].pos.x = 0.0f; w.br[0].pos.z = 1.6f;  w.ch[0].pos.x = -0.5f; w.ch[0].pos.z = 3.5f;   /* m6.2: P1 level with it (bots stay behind P1 when quiet) */
    run(60 * 16, &t, move_me_toward_stick);
    CHECK(dec(D_UNREACHABLE) >= 1, "m6.1: unbreakable object: given up (%d), not walking forever (m6.7: it may be tried again after the 4 s ignore)", dec(D_UNREACHABLE));

    /* m6.1: an enemy lying on the floor: no X at it; wait outside wake-up range; attack again once it is up */
    world_reset(); bot_reset(1); g_me = &w.ch[1]; mk(2, 50, 1, -1, 0.9f, 1.5f, -1, CS_ONGROUND); w.ch[2].cantBeGrabbed = 1;
    run(120, &t, move_me_toward_stick);
    { int xDown = t.btn[BTN_X], aDown = t.btn[BTN_A]; float gap = fabsf(g_me->pos.x - 0.9f);
      w.ch[2].state = CS_STANDWALK; run(90, &t, move_me_toward_stick);
      CHECK(xDown == 0 && aDown == 0 && dec(D_WAITDOWN) == 1 && gap > 1.5f && t.btn[BTN_X] >= 1,
            "m6.1: enemy on the floor: X %d, A %d while down, waited %.2f away; after it got up X %d", xDown, aDown, gap, t.btn[BTN_X]); }

    /* m6.1: a dodge jump with an enemy in front attacks on the way down (no wasted hop in place) */
    world_reset(); bot_reset(1); mk(2, 50, 1, -1, 1.3f, 1.5f, -1, CS_STANDMOVE); w.ch[2].superArmor = 1; w.ch[2].cantBeGrabbed = 1;
    run(30, &t, NULL);
    CHECK(dec(D_DODGEJUMP) == 1 && t.btn[BTN_A] == 1 && t.btn[BTN_X] >= 1 && t.firstBtn[BTN_X] > t.firstBtn[BTN_A],
          "m6.1: dodge jump with an enemy in front: A at frame %d, X at frame %d (dodges %d)", t.firstBtn[BTN_A], t.firstBtn[BTN_X], dec(D_DODGEJUMP));
    /* m6.1: dodges share the jump budget: an enemy attacking all the time for 20 s -> at most 4 hops */
    run(60 * 20, &t, NULL);
    CHECK(dec(D_DODGEJUMP) <= 4, "m6.1: 20 s of incoming attacks: dodge jumps %d (max 4)", dec(D_DODGEJUMP));

    /* m6.2: scripted scene mid-stage (game session mode 1, m6-run1b softlock): hands off, back after it ends */
    world_reset(); bot_reset(1); mk(2, 50, 1, -1, 0.9f, 1.5f, -1, CS_STANDWALK); w.ch[2].cantBeGrabbed = 1;
    run(30, &t, NULL); int before = t.neutralFrames;
    w.mode = 1; run(120, &t, NULL); int during = t.neutralFrames;
    w.mode = 0; run(120, &t, NULL);
    CHECK(before < 30 && during == 120 && t.btn[BTN_X] >= 1, "m6.2: scripted scene (mode 1) mid-stage: neutral %d/120 (fighting before: %d/30 neutral), fights again after: X %d", during, before, t.btn[BTN_X]);
    /* m6.2: the m6-run1b positions: P1 at 171.7 (still), camera 173.2, a weapon at 178.2 near the scripted exit;
       the bot (at 175) must not go for it, must drop back behind P1's front and stay inside the camera margin */
    world_reset(); bot_reset(1); w.cam.x = 173.2f; g_me = &w.ch[1]; g_me->pos.x = 175.0f; g_me->pos.z = 15.5f;
    w.ch[0].pos.x = 171.7f; w.ch[0].pos.z = 15.5f; w.ch[0].facing = 1;
    w.npk = 1; w.pk[0].addr = 0x5000; w.pk[0].kind = PK_WEAPON; w.pk[0].pos.x = 178.2f; w.pk[0].pos.z = 15.5f;
    { float maxX = g_me->pos.x; for (int fr = 0; fr < 60 * 4; fr++, now += 16) { w.frames++; brain_think(&bb[0], bb, 3, &w, 1, now, &out); move_me_toward_stick(fr); if (g_me->pos.x > maxX) maxX = g_me->pos.x; }
      CHECK(maxX <= 175.01f && g_me->pos.x <= 171.7f + 1.3f && t.btn[BTN_B] == 0 && dec(D_WEAPONPICK) == 0,
            "m6.2: quiet, bot ahead of P1 near the exit: max x %.2f (start 175), ends at %.2f (P1 171.7), weapon ahead of P1 left alone (%d)", maxX, g_me->pos.x, dec(D_WEAPONPICK)); }
    /* m6.2: an enemy at the far screen edge: the bot does not walk into the edge margin */
    world_reset(); bot_reset(1); g_me = &w.ch[1]; g_me->pos.x = 5.0f; mk(2, 50, 1, -1, 9.4f, 1.5f, -1, CS_STANDWALK); w.ch[2].cantBeGrabbed = 1;
    { float maxX = 0; for (int fr = 0; fr < 60 * 3; fr++, now += 16) { w.frames++; brain_think(&bb[0], bb, 3, &w, 1, now, &out); move_me_toward_stick(fr); if (g_me->pos.x > maxX) maxX = g_me->pos.x; }
      CHECK(maxX < 9.6f - 0.6f * 1.25f + 0.1f, "m6.2: enemy at the screen edge (9.4, edge 9.6): bot stays inside the margin (max x %.2f)", maxX); }
    /* m6.6 (m6.5-run1 15:11:43.9, sub-boss uid 97, cam 169, left wall 159.3): AirMove, move 4, aiAttack 1, y 12.3 at
       (170.4, 15.9); the bots stood at (169.2, 15.8), (170.2, 16.0), (170.8, 15.6) and all lost ~22 life on the landing.
       Video: ~1.1 s from takeoff to landing on a fixed shadow, shockwave ~ one character height (2.5). Bots walk 3.9/s. */
    for (int sc = 0; sc < 2; sc++) {
        world_reset(); bot_reset(1); w.cam.x = 169.0f; w.haveWall = 1; w.leftWallX = 159.3f;
        w.ch[0].pos.x = 163.0f; w.ch[0].pos.z = 14.5f;
        static const float bx[3] = { 169.2f, 170.2f, 170.8f }, bz[3] = { 15.8f, 16.0f, 15.6f };
        w.nslot = 4; for (int k = 1; k < 4; k++) { w.slot[k].ctrl = k; w.slot[k].type = 1; }
        for (int k = 0; k < 3; k++) {
            mk(k + 1, k + 2, 0, k + 1, bx[k], bz[k], 1, CS_STANDWALK);
            if (k) { brain_reset(&bb[k], k, k + 2, k + 2, k == 1 ? "Bot3" : "Bot4", now); bb[k].id.ctrlId = k + 1; bb[k].id.verifiedSlot = k + 1; bb[k].id.preKnown = 1; bb[k].id.npre = 1; bb[k].id.attachAt = now - 60000; bb[k].id.lastSeen = now; bb[k].sawLevel = 1; bb[k].introDone = 1; bb[k].xSign = 1; }
        }
        Chr *boss = mk(4, 97, 1, -1, 170.4f, 15.9f, -1, CS_STANDWALK); boss->life = 213.6f; boss->maxLife = 336; boss->cantBeGrabbed = 1;
        float minD = 99; int xAir = 0, xAfter = 0, lastX[3] = { 0, 0, 0 }, hit = 0, fr, evNotes = 0;
        if (sc == 1) { boss->state = CS_JUMP; boss->aiAttacking = 0; }   /* control: an ordinary hop is ignored */
        for (fr = 0; fr < 60 * 4; fr++, now += 16) {
            float t = (fr - 30) * 0.016f;   /* 0.5 s of normal fighting, then takeoff */
            if (sc == 0) {
                if (t >= 0 && t < 1.1f) { boss->state = CS_AIRMOVE; boss->moveId = 4; boss->aiAttacking = 1; boss->invincible = 40; boss->pos.y = t < 0.4f ? 12.0f * t / 0.4f : t < 0.8f ? 12.3f : 12.3f * (1.1f - t) / 0.3f; }
                else if (t >= 1.1f && t < 1.9f) { boss->state = CS_STANDMOVE; boss->moveId = 0; boss->pos.y = 0; boss->invincible = 0; boss->aiAttacking = 0; }
                else { boss->state = CS_STANDWALK; boss->moveId = -1; boss->pos.y = 0; boss->aiAttacking = 0; }
            } else { boss->pos.y = (t >= 0 && t < 0.5f) ? 2.0f * (1.0f - fabsf(t - 0.25f) * 4.0f) : 0; boss->state = boss->pos.y > 0 ? CS_JUMP : CS_STANDWALK; }
            w.frames++;
            for (int k = 0; k < 3; k++) {
                PadOut ok; brain_think(&bb[k], bb, 3, &w, 1, now, &ok); Chr *c = &w.ch[k + 1];
                c->pos.x += ok.axis[AX_LX] / 32767.0f * 0.065f; c->pos.z += ok.axis[AX_LY] / 32767.0f * 0.032f;
                if (ok.axis[AX_LX]) c->facing = ok.axis[AX_LX] > 0 ? 1 : -1;
                int xp = ok.btn[BTN_X] && !lastX[k]; lastX[k] = ok.btn[BTN_X];
                if (sc == 0 && t >= 0.1f && t < 1.1f && xp) xAir++;
                if (sc == 0 && t >= 1.1f && xp) xAfter++;
                if (sc == 0 && fr == 30 + 69) { float dx = c->pos.x - 170.4f, dz = c->pos.z - 15.9f, d = sqrtf(dx * dx + dz * dz); if (d < minD) minD = d; if (d < 2.5f) hit++; }
            }
        }
        for (int k = 0; k < 3; k++) evNotes += bb[k].decTotal[D_SLAMEVADE];
        if (sc == 0) CHECK(hit == 0 && minD >= 2.5f && xAir == 0 && xAfter > 0 && evNotes >= 3,
              "m6.6: sub-boss jump slam (logged uid 97 at 170.4/15.9, bots at 169.2-170.8): bots inside the 2.5 shockwave at landing %d (closest %.2f), X while it was up %d, X after it landed %d, evade notes %d",
              hit, minD, xAir, xAfter, evNotes);
        else CHECK(evNotes == 0, "m6.6: an ordinary enemy hop (Jump, y 2.0, aiAttack 0) next to the bots: no evading (%d)", evNotes);
    }
    /* m6.6: capped: an enemy hanging in the air (y 4) for 5 s: the bot gives up evading after 2.5 s */
    world_reset(); bot_reset(1); g_me = &w.ch[1]; g_me->pos.x = 0.0f; g_me->pos.z = 1.5f;
    { Chr *e = mk(2, 60, 1, -1, 1.0f, 1.5f, -1, CS_AIRMOVE); e->aiAttacking = 1; e->pos.y = 4.0f; int evadeFrames = 0;
      for (int fr = 0; fr < 60 * 5; fr++, now += 16) { w.frames++; brain_think(&bb[0], bb, 3, &w, 1, now, &out);
          if (strstr(bb[0].why, "leaving its landing spot")) evadeFrames++; g_me->pos.x += out.axis[AX_LX] / 32767.0f * 0.065f; g_me->pos.z += out.axis[AX_LY] / 32767.0f * 0.032f; if (g_me->pos.x < -3.0f) g_me->pos.x = -3.0f; if (g_me->pos.x > 3.0f) g_me->pos.x = 3.0f; }
      CHECK(evadeFrames > 0 && evadeFrames * 16 <= 2600, "m6.6: enemy stuck in the air 5 s: evaded %.1f s (cap 2.5 s)", evadeFrames * 0.016f); }

    /* ---------------- m6.7 ---------------- */
    /* m6.7 jitter: logged spots from m6.6-run1 where the m6.6 brain flipped lx 4+ times within 1 s once the bot walked
       (found by jitter_test replaying every snapshot). Walk the bot by its stick for 3 s; at most 3 reversals per 1 s. */
    for (int sc = 0; sc < 2; sc++) {
        world_reset(); bot_reset(1); g_me = &w.ch[1];
        if (sc == 0) {   /* 15:21:55.255: P2 at (26.0, 1.6) next to uid 31 lying at (26.7, 1.6), cam 18.1, P1 (19.8, 1.6) */
            w.cam.x = 18.1f; w.ch[0].pos.x = 19.8f; w.ch[0].pos.z = 1.6f; w.ch[0].facing = -1;
            g_me->pos.x = 26.0f; g_me->pos.z = 1.6f; g_me->facing = 1;
            Chr *e = mk(2, 31, 1, -1, 26.7f, 1.6f, -1, CS_ONGROUND); e->life = 25.2f; e->noQuickRecover = 1;
        } else {         /* 15:23:37.921: on top of uid 51 (AirHit) at (65.5, -0.6); uid 50 walking in at 57.4; cam 62.8, P1 (60.9, -2.2) */
            w.cam.x = 62.8f; w.ch[0].pos.x = 60.9f; w.ch[0].pos.z = -2.2f; w.ch[0].facing = -1;
            g_me->pos.x = 65.7f; g_me->pos.z = -0.6f; g_me->facing = 1;
            Chr *e = mk(2, 51, 1, -1, 65.5f, -0.6f, -1, CS_AIRHIT); e->life = 19.2f; e->hitStun = 30;
            mk(3, 50, 1, -1, 57.4f, -0.6f, 1, CS_STANDWALK)->life = 24.0f;
            Chr *f = mk(4, 49, 1, -1, 71.4f, -0.7f, -1, CS_ONGROUND); f->life = 17.8f;
        }
        int revAt[64], nrev = 0, lastSx = 0, worst = 0;
        for (int fr = 0; fr < 180; fr++, now += 16) {
            w.frames++; brain_think(&bb[0], bb, 3, &w, 1, now, &out);
            int sx = out.axis[AX_LX] > 8000 ? 1 : out.axis[AX_LX] < -8000 ? -1 : 0;
            if (sx && lastSx && sx != lastSx && nrev < 64) revAt[nrev++] = fr;
            if (sx) lastSx = sx;
            if (g_me->state == CS_STANDWALK) { g_me->pos.x += out.axis[AX_LX] / 32767.0f * 0.065f; g_me->pos.z += out.axis[AX_LY] / 32767.0f * 0.032f; }
            if (sx) g_me->facing = sx;
        }
        for (int i = 0; i < nrev; i++) { int n = 0; for (int j = i; j < nrev && revAt[j] - revAt[i] < 60; j++) n++; if (n > worst) worst = n; }
        CHECK(worst < 4, "m6.7: back-and-forth at the logged m6.6-run1 spot %s: most stick reversals within 1 s %d (m6.6: 4+), total %d",
              sc == 0 ? "15:21:55 (enemy down next to us)" : "15:23:37 (on top of a juggled enemy)", worst, nrev);
    }

    /* m6.7 traps, damage-memory fallback (m6.6-run1 15:28:10: P2 at (3.8, 5.5) lost 11 life and was knocked down; the
       only standing enemy was uid 38 at (-8.7, 5.5); no trap object was read by m6.6) */
    dbg_trap_reset();
    world_reset(); bot_reset(1); g_me = &w.ch[1]; w.cam.x = -1.1f; w.scene = 3;
    w.ch[0].pos.x = 5.5f; w.ch[0].pos.z = 7.5f; g_me->pos.x = 3.8f; g_me->pos.z = 5.5f; g_me->life = 61.8f;
    mk(2, 38, 1, -1, -8.7f, 5.5f, 1, CS_STANDWALK)->life = 23.8f;
    run(170, &t, NULL);   /* m7.2: > 2.5 s into the fight (section starts with the fighters lying down are not traps) */
    g_me->state = CS_AIRHIT; g_me->life = 50.8f; run(40, &t, NULL);
    g_me->state = CS_ONGROUND; run(30, &t, NULL);
    g_me->state = CS_STANDWALK; g_me->pos.x = 3.8f; g_me->pos.z = 5.5f;
    { int z1 = dbg_trap_zone_count(), notes = dec(D_TRAP), inside = 0;
      for (int fr = 0; fr < 120; fr++, now += 16) { w.frames++; brain_think(&bb[0], bb, 3, &w, 1, now, &out);
          g_me->pos.x += out.axis[AX_LX] / 32767.0f * 0.065f; g_me->pos.z += out.axis[AX_LY] / 32767.0f * 0.032f;
          if (getenv("DBG") && fr % 5 == 0) printf("  tz fr %d (%.2f,%.2f) %s\n", fr, g_me->pos.x, g_me->pos.z, bb[0].why);
          if (fr > 45 && fabsf(g_me->pos.x - 3.8f) < 1.2f && fabsf(g_me->pos.z - 5.5f) < 0.6f) inside++; }
      CHECK(z1 == 1 && notes == 1 && inside == 0, "m6.7: hurt with no enemy near (logged 15:28:10 spot): no-walk zones %d, trap notes %d, frames still inside after 0.75 s %d (now at %.1f, %.1f)",
            z1, notes, inside, g_me->pos.x, g_me->pos.z); }
    /* m7.2 (m7.1-run1 17:23:46): a section starts with the fighters lying down, no enemy near: no zone */
    dbg_trap_reset();
    world_reset(); bot_reset(1); g_me = &w.ch[1]; w.scene = 5;
    g_me->state = CS_ONGROUND; w.ch[0].state = CS_ONGROUND; run(40, &t, NULL); g_me->state = w.ch[0].state = CS_STANDWALK; run(20, &t, NULL);
    CHECK(dbg_trap_zone_count() == 0 && dec(D_TRAP) == 0, "m7.2: lying down at a section start with no enemy near: no trap zone (%d)", dbg_trap_zone_count());
    /* control: hurt by an enemy swinging right next to us: no zone */
    dbg_trap_reset();
    world_reset(); bot_reset(1); g_me = &w.ch[1]; w.scene = 3;
    { Chr *e = mk(2, 70, 1, -1, 1.0f, 1.5f, -1, CS_STANDMOVE); e->aiAttacking = 1; }
    run(10, &t, NULL); g_me->state = CS_STANDHIT; run(20, &t, NULL); g_me->state = CS_STANDWALK; run(10, &t, NULL);
    CHECK(dbg_trap_zone_count() == 0 && dec(D_TRAP) == 0, "m6.7: hurt by an enemy attacking next to us: no trap zone (%d)", dbg_trap_zone_count());
    /* hazard object next to the spot: the bot waits at the zone edge while the hazard plays the animation that hurt
       us, and crosses once it changes */
    dbg_trap_reset();
    world_reset(); bot_reset(1); g_me = &w.ch[1]; w.scene = 4; w.cam.x = 10.0f;
    w.nhz = 1; w.hz[0].uid = 900; w.hz[0].pos.x = 10.0f; w.hz[0].pos.z = 3.0f; w.hz[0].anim = 2;
    w.ch[0].pos.x = 10.0f; w.ch[0].pos.z = 6.0f; g_me->pos.x = 10.4f; g_me->pos.z = 3.0f;
    run(170, &t, NULL); g_me->state = CS_AIRHIT; run(30, &t, NULL); g_me->state = CS_STANDWALK;
    g_me->pos.x = 6.0f; g_me->pos.z = 3.0f; w.ch[0].pos.x = 15.0f; w.ch[0].pos.z = 3.0f; w.ch[0].facing = 1;
    { float maxX = 0; int fr;
      for (fr = 0; fr < 150; fr++, now += 16) { w.frames++; brain_think(&bb[0], bb, 3, &w, 1, now, &out);
          g_me->pos.x += out.axis[AX_LX] / 32767.0f * 0.065f; g_me->pos.z += out.axis[AX_LY] / 32767.0f * 0.032f; if (g_me->pos.x > maxX) maxX = g_me->pos.x; }
      w.hz[0].anim = 0;
      float crossed = 0; for (fr = 0; fr < 180; fr++, now += 16) { w.frames++; brain_think(&bb[0], bb, 3, &w, 1, now, &out);
          g_me->pos.x += out.axis[AX_LX] / 32767.0f * 0.065f; g_me->pos.z += out.axis[AX_LY] / 32767.0f * 0.032f; if (g_me->pos.x > crossed) crossed = g_me->pos.x; }
      CHECK(dbg_trap_zone_count() == 1 && maxX < 9.2f && crossed > 11.5f, "m6.7: live hazard zone (uid 900 at x 10, anim 2): bot stayed at x <= %.2f while it played anim 2, crossed to %.1f after it changed",
            maxX, crossed); }
    dbg_trap_reset();

    /* m6.7 shield cops. Positions from m6.6-run1b 15:37:15.959 (P2 46.6/-1.3, P3 45.8/-1.0, P4 49.4/-1.3, enemy at
       53.0/-1.4 facing them); the cop is a shield user (block-armor move), stamina 60/60, can't be grabbed while up */
    for (int sc = 0; sc < 4; sc++) {
        world_reset(); bot_reset(1); w.cam.x = 50.0f; w.ch[0].pos.x = 44.0f; w.ch[0].pos.z = -1.8f;
        static const float px[3] = { 46.6f, 45.8f, 49.4f }, pz[3] = { -1.3f, -1.0f, -1.3f };
        w.nslot = 4; for (int k = 1; k < 4; k++) { w.slot[k].ctrl = k; w.slot[k].type = 1; }
        for (int k = 0; k < 3; k++) {
            mk(k + 1, k + 2, 0, k + 1, px[k], pz[k], 1, CS_STANDWALK);
            if (k) { brain_reset(&bb[k], k, k + 2, k + 2, k == 1 ? "Bot3" : "Bot4", now); bb[k].id.ctrlId = k + 1; bb[k].id.verifiedSlot = k + 1; bb[k].id.preKnown = 1; bb[k].id.npre = 1; bb[k].id.attachAt = now - 60000; bb[k].id.lastSeen = now; bb[k].sawLevel = 1; bb[k].introDone = 1; bb[k].xSign = 1; }
        }
        int nb = sc == 0 ? 3 : 1;
        if (nb == 1) { w.ch[2].pos.x = 30.0f; w.ch[3].pos.x = 30.5f; w.ch[2].state = w.ch[3].state = CS_ONGROUND; }   /* the other two out of it */
        Chr *cop = mk(4, 80, 1, -1, 53.0f, -1.4f, -1, CS_STANDWALK); cop->life = 72; cop->shieldUser = 1; cop->stamina = 60; cop->maxStamina = 60; cop->cantBeGrabbed = 1;
        if (sc == 0) mk(5, 81, 1, -1, 56.0f, -0.6f, -1, CS_STANDWALK)->life = 40;          /* an ordinary enemy next to it */
        if (sc >= 1) { w.ch[1].pos.x = 52.0f; w.ch[1].pos.z = -1.4f; }
        if (sc == 2) { cop->facing = 1; }                                                   /* shield turned away */
        if (sc == 3) { cop->staminaBroken = 1; cop->state = CS_STANDHIT; cop->hitStun = 20; cop->cantBeGrabbed = 0; }   /* broken */
        int lastX[3] = { 0 }, xs = 0, ys = 0, onCop = 0, backed = 0; float x0 = w.ch[1].pos.x;
        int frames = sc == 0 ? 60 * 6 : 60 * 3;
        for (int fr = 0; fr < frames; fr++, now += 16) {
            if (sc == 1) { if (fr == 60) { cop->state = CS_STANDMOVE; cop->moveId = 5; cop->aiAttacking = 1; x0 = w.ch[1].pos.x; } if (fr == 100) { cop->state = CS_STANDWALK; cop->moveId = -1; cop->aiAttacking = 0; } }
            if (sc == 3) { cop->state = (fr / 20) % 3 == 2 ? CS_STANDWALK : CS_STANDHIT; cop->hitStun = cop->state == CS_STANDHIT ? 20 : 0; }
            w.frames++;
            for (int k = 0; k < nb; k++) {
                PadOut ok; brain_think(&bb[k], bb, 3, &w, 1, now, &ok); Chr *c = &w.ch[k + 1];
                if (c->state == CS_STANDWALK) { c->pos.x += ok.axis[AX_LX] / 32767.0f * 0.065f; c->pos.z += ok.axis[AX_LY] / 32767.0f * 0.032f; }
                if (ok.axis[AX_LX]) c->facing = ok.axis[AX_LX] > 0 ? 1 : -1;
                if (ok.btn[BTN_X] && !lastX[k]) xs++; lastX[k] = ok.btn[BTN_X];
                if (ok.btn[BTN_Y]) ys++;
                if (sc == 0 && fr == frames - 1 && bb[k].targetUid == 80) onCop++;
            }
            if (sc == 1 && getenv("DBG") && fr >= 56 && fr < 100 && fr % 4 == 0) printf("  sh fr %d x %.2f st %d %s\n", fr, w.ch[1].pos.x, w.ch[1].state, bb[0].why);
            if (sc == 1 && fr == 99) backed = w.ch[1].pos.x < x0 - 0.3f;
        }
        int grabs = 0, tac = 0, chains = 0; for (int k = 0; k < 3; k++) { grabs += bb[k].decTotal[D_GRABTRY] + bb[k].decTotal[D_SHIELDGRAB]; tac += bb[k].decTotal[D_SHIELDTAC]; chains += bb[k].decTotal[D_CHAIN]; }
        if (sc == 0) CHECK(grabs == 0 && onCop >= 2 && xs >= 12 && ys >= 1 && tac >= 2,
              "m6.7: shield cop facing 3 bots (logged spots): bots on it %d of 3 (pile on), X presses %d, Y frames %d (special), grab tries %d (expect 0), shield notes %d", onCop, xs, ys, grabs, tac);
        if (sc == 1) CHECK(backed, "m6.7: shield cop attacks (StandMove 5, aiAttack 1) with the bot in front: backed off %d (from %.1f to the left), shield notes %d", backed, x0, tac);
        if (sc == 2) CHECK(xs >= 6 && ys == 0 && grabs == 0, "m6.7: shield turned away: full strings from behind (X %d, Y %d expect 0, grab tries %d)", xs, ys, grabs);
        if (sc == 3) CHECK(xs >= 8 && tac >= 1, "m6.7: shield broken: chained strings (X %d, chains %d), broken noted %d", xs, chains, tac);
    }

    /* m6.5 (m6.4-run1b 14:58:40-14:59:11, stage 1 x 30-43): trash cans against the back wall, uid 7 (43.4, -3.1) and
       uid 8 (35.5, -2.8); the back edge of the floor is slanted (Axel stopped at z -2.2 at x 33.2, P1 got to -3.24 at
       x 39-41): modelled as z >= -2.2 - 0.2 (x - 33), clamped to [-3.3, -1.0]. Left wall 28.8, camera 38.4, P1 at
       (42.3, -0.3). Axel (33.3, -2.2) and Blaze (29.9, -0.5). On m6.4 Blaze walked LEFT to the wall while 13.5 left
       of uid 7, zSign flipped back and forth, Axel flipped sides at uid 8 ("can't line up in depth (dz -0.64)").
       Hits land within |dz| 0.7 (P1 breaks them from the normal floor) and |dx| 2.3. The cans are solid. */
    {
        world_reset(); bot_reset(1); w.cam.x = 38.4f; w.haveWall = 1; w.leftWallX = 28.8f;
        w.ch[0].pos.x = 42.3f; w.ch[0].pos.z = -0.3f; w.ch[0].facing = 1;
        w.slot[2].ctrl = 2; w.slot[2].type = 1; Chr *c3 = mk(2, 3, 0, 2, 29.9f, -0.5f, 1, CS_STANDWALK);
        brain_reset(&bb[1], 1, 3, 3, "Bot3", now); bb[1].id.ctrlId = 2; bb[1].id.verifiedSlot = 2; bb[1].id.preKnown = 1; bb[1].id.npre = 1; bb[1].id.attachAt = now - 60000; bb[1].id.lastSeen = now; bb[1].sawLevel = 1; bb[1].introDone = 1; bb[1].xSign = 1;
        g_me = &w.ch[1]; g_me->pos.x = 33.3f; g_me->pos.z = -2.2f;
        w.nbr = 2; w.br[0].uid = 7; w.br[0].life = 1; w.br[0].entered = 1; w.br[0].pos.x = 43.4f; w.br[0].pos.z = -3.1f;
        w.br[1].uid = 8; w.br[1].life = 1; w.br[1].entered = 1; w.br[1].pos.x = 35.5f; w.br[1].pos.z = -2.8f;
        Chr *fs[2] = { g_me, c3 }; int fr, last[2] = { 0, 0 }, away = 0, flips = 0, zs0 = bb[0].zSign, zs1 = bb[1].zSign; float minX = 99;
        for (fr = 0; fr < 60 * 16 && (w.br[0].life > 0 || w.br[1].life > 0); fr++, now += 16) {
            PadOut oo[2]; w.frames++; brain_think(&bb[0], bb, 3, &w, 1, now, &oo[0]); brain_think(&bb[1], bb, 3, &w, 1, now, &oo[1]);
            if (bb[0].zSign != zs0) { flips++; zs0 = bb[0].zSign; }
            if (bb[1].zSign != zs1) { flips++; zs1 = bb[1].zSign; }
            for (int k = 0; k < 2; k++) {
                Chr *c = fs[k]; int zs = k ? bb[1].zSign : bb[0].zSign;   /* the game's real mapping: +ly = +z */
                (void)zs;
                for (int q = 0; q < 2; q++) { const Brk *bo = &w.br[q]; if (bo->life > 0 && (k ? bb[1].breakUid : bb[0].breakUid) == bo->uid) {
                    float bdx = bo->pos.x - c->pos.x; if (fabsf(bdx) > 4.0f && oo[k].axis[AX_LX] && (oo[k].axis[AX_LX] > 0) != (bdx > 0)) away++; } }
                float nx = c->pos.x + oo[k].axis[AX_LX] / 32767.0f * 0.08f, nz = c->pos.z + oo[k].axis[AX_LY] / 32767.0f * 0.04f;
                if (nx < 28.8f) nx = 28.8f;
                float zmin = -2.2f - 0.2f * (nx - 33.0f); if (zmin < -3.3f) zmin = -3.3f; if (zmin > -1.0f) zmin = -1.0f;
                if (nz < zmin) nz = zmin;
                for (int q = 0; q < 2; q++) { const Brk *bo = &w.br[q]; if (bo->life <= 0) continue;
                    float ex = (nx - bo->pos.x) / 1.1f, ez = (nz - bo->pos.z) / 0.6f;
                    if (ex * ex + ez * ez < 1.0f) { float ox = (c->pos.x - bo->pos.x) / 1.1f, oz = (c->pos.z - bo->pos.z) / 0.6f; if (ox * ox + oz * oz >= 1.0f) { nx = c->pos.x; nz = c->pos.z; } } }   /* solid: against the wall it can't be slid around */
                if (oo[k].axis[AX_LX]) c->facing = oo[k].axis[AX_LX] > 0 ? 1 : -1;
                c->pos.x = nx; c->pos.z = nz; if (nx < minX) minX = nx;
                if (oo[k].btn[BTN_X] && !last[k]) for (int q = 0; q < 2; q++) { Brk *bo = &w.br[q];
                    if (bo->life > 0 && fabsf(nz - bo->pos.z) <= 0.7f && fabsf(nx - bo->pos.x) <= 2.3f && c->facing == (nx < bo->pos.x ? 1 : -1)) bo->life -= 2; }
                last[k] = oo[k].btn[BTN_X];
            }
        }
        CHECK(w.br[0].life <= 0 && w.br[1].life <= 0 && away == 0 && flips == 0 && minX > 28.85f,
              "m6.5: back-wall cans uid 7/8: broken %d/%d in %.1f s, frames walking away from the can while > 4 off %d, zSign flips %d, leftmost x %.2f (wall 28.8); Axel (%.1f, %.1f) Blaze (%.1f, %.1f)",
              w.br[0].life <= 0, w.br[1].life <= 0, fr * 0.016f, away, flips, minX, g_me->pos.x, g_me->pos.z, c3->pos.x, c3->pos.z);
    }
    /* m6.4 (m6.3-run1 14:46:38-14:47:20): barrel uid 6 at (4.6, 0.1). Its solid body (modelled as an ellipse 1.1 wide,
       0.6 deep) deflects a walker in depth: Axel walked in on its lane from (1.7, 0.2), got pushed to (3.6, 0.7), swung
       at dx -1.0 dz -0.55 and missed, then danced between "stepping out", "stepping in" and "lining up" at (3.1, 0.6).
       Hits land within |dz| 0.2 and |dx| 2.3 (P1 broke one from dx -2.2, dz 0.13). P1 stands at (-3.6, 1.6). */
    for (int sc = 0; sc < 3; sc++) {
        static const float sx[3] = { -6.1f, 1.7f, 3.1f }, sz[3] = { 1.6f, 0.2f, 0.6f };
        world_reset(); bot_reset(1); w.cam.x = -0.3f; w.ch[0].pos.x = -3.6f; w.ch[0].pos.z = 1.6f; w.ch[0].facing = 1;
        g_me = &w.ch[1]; g_me->pos.x = sx[sc]; g_me->pos.z = sz[sc];
        w.nbr = 1; w.br[0].uid = 6; w.br[0].life = 1; w.br[0].entered = 1; w.br[0].pos.x = 4.6f; w.br[0].pos.z = 0.1f;
        int fr, lastX = 0, swings = 0, flips = 0, lastSgn = 0; float minAx = 99;
        for (fr = 0; fr < 60 * 12 && w.br[0].life > 0; fr++, now += 16) {
            w.frames++; brain_think(&bb[0], bb, 3, &w, 1, now, &out);
            float nx = g_me->pos.x + out.axis[AX_LX] / 32767.0f * 0.08f, nz = g_me->pos.z + out.axis[AX_LY] / 32767.0f * 0.04f;
            if (nz < 0.0f) nz = 0.0f;
            float ex = (nx - 4.6f) / 1.1f, ez = (nz - 0.1f) / 0.6f;
            if (ex * ex + ez * ez < 1.0f) { float side = g_me->pos.z >= 0.1f ? 1.0f : -1.0f; nz = 0.1f + side * 0.6f * sqrtf(1.0f - ex * ex) + side * 0.01f; if (nz < 0.0f) { nz = g_me->pos.z; nx = g_me->pos.x; } }
            if (out.axis[AX_LX]) g_me->facing = out.axis[AX_LX] > 0 ? 1 : -1;
            int sg = out.axis[AX_LX] > 0 ? 1 : out.axis[AX_LX] < 0 ? -1 : 0;
            if (sg && lastSgn && sg != lastSgn) flips++;
            if (sg) lastSgn = sg;
            g_me->pos.x = nx; g_me->pos.z = nz;
            if (fabsf(nx - 4.6f) < minAx) minAx = fabsf(nx - 4.6f);
            if (out.btn[BTN_X] && !lastX) { swings++; if (fabsf(nz - 0.1f) <= 0.2f && fabsf(nx - 4.6f) <= 2.3f && g_me->facing == (nx < 4.6f ? 1 : -1)) w.br[0].life -= 2; }
            lastX = out.btn[BTN_X];
        }
        CHECK(w.br[0].life <= 0 && flips <= 3, "m6.4: barrel uid 6 (4.6, 0.1), Axel from (%.1f, %.1f): broken %d in %.1f s, swings %d, left/right flips %d, closest |dx| %.2f, ends at (%.2f, %.2f)",
              sx[sc], sz[sc], w.br[0].life <= 0, fr * 0.016f, swings, flips, minAx, g_me->pos.x, g_me->pos.z);
    }
    /* m6.3 (m6.2-run1c 14:37:59, stage 1 sub-boss, scene 8, cam 169): boss uid 102 at (163.5, 16.0), P1 at (170.2, 14.3),
       the bot at the right screen edge (177.6, 14.4) next to a weapon stack at 178.1 (z 14.5 / 15.0 / 15.6) it can't reach */
    world_reset(); bot_reset(1); w.cam.x = 169.0f; w.ch[0].pos.x = 170.2f; w.ch[0].pos.z = 14.27f; w.ch[0].facing = -1;
    g_me = &w.ch[1]; g_me->pos.x = 177.6f; g_me->pos.z = 14.4f; mk(2, 102, 1, -1, 163.5f, 16.0f, 1, CS_STANDWALK);
    w.npk = 3; for (int k = 0; k < 3; k++) { w.pk[k].addr = 0x8100 + k * 0x40; w.pk[k].kind = PK_WEAPON; w.pk[k].life = 110; w.pk[k].pos.x = 178.1f; w.pk[k].pos.z = 14.5f + k * 0.55f; }
    { float maxX = 0; int bPress = 0, lastB = 0; for (int fr = 0; fr < 60 * 5; fr++, now += 16) { w.frames++; brain_think(&bb[0], bb, 3, &w, 1, now, &out); move_me_toward_stick(fr); if (g_me->pos.x > 177.6f && g_me->pos.x > maxX) maxX = g_me->pos.x; if (g_me->pos.x > 177.6f) g_me->pos.x = 177.6f; if (out.btn[BTN_B] && !lastB) bPress++; lastB = out.btn[BTN_B]; }
      CHECK(dec(D_WEAPONPICK) == 0 && bPress == 0 && g_me->pos.x < 172.0f && bb[0].targetUid == 102,
            "m6.3: sub-boss fight, weapon stack at the screen edge (178.1) ahead of P1: left alone (picks %d, B %d), bot heads to the boss (x %.2f, target uid %u)", dec(D_WEAPONPICK), bPress, g_me->pos.x, bb[0].targetUid); }
    /* m6.3: a weapon stack that never gets picked up (quiet, P1 level with it): ignored after ~1.5 s next to it */
    world_reset(); bot_reset(1); g_me = &w.ch[1]; g_me->pos.x = 0.0f; g_me->pos.z = 1.5f; w.ch[0].pos.x = 0.6f; w.ch[0].pos.z = 3.0f;
    w.npk = 2; for (int k = 0; k < 2; k++) { w.pk[k].addr = 0x8200 + k * 0x40; w.pk[k].kind = PK_WEAPON; w.pk[k].life = 110; w.pk[k].pos.x = 1.0f; w.pk[k].pos.z = 1.5f + k * 0.5f; }
    { int bPress = 0, lastB = 0, fr, gaveAt = -1; for (fr = 0; fr < 60 * 8; fr++, now += 16) { w.frames++; brain_think(&bb[0], bb, 3, &w, 1, now, &out); move_me_toward_stick(fr); if (out.btn[BTN_B] && !lastB) bPress++; lastB = out.btn[BTN_B]; if (gaveAt < 0 && dec(D_UNREACHABLE)) gaveAt = fr; }
      CHECK(gaveAt >= 0 && gaveAt * 16 < 1500 + 3000 + 600 && bPress >= 1 && bPress <= 4, "m6.3: unpickable weapon stack: given up after %.1f s (B presses %d), escalating ignore", gaveAt * 0.016f, bPress); }
    /* m6.2: one bot per breakable */
    world_reset(); bot_reset(1);
    w.slot[2].ctrl = 2; w.slot[2].type = 1; Chr *c3b = mk(2, 3, 0, 2, 0.3f, 1.2f, 1, CS_STANDWALK);
    brain_reset(&bb[1], 1, 3, 3, "Bot3", now); bb[1].id.ctrlId = 2; bb[1].id.verifiedSlot = 2; bb[1].id.preKnown = 1; bb[1].id.npre = 1; bb[1].id.attachAt = now - 60000; bb[1].id.lastSeen = now; bb[1].sawLevel = 1; bb[1].introDone = 1; bb[1].xSign = 1;
    g_me = &w.ch[1]; g_me->pos.x = 0.0f; g_me->pos.z = 1.5f; w.ch[0].pos.x = 1.5f; w.ch[0].pos.z = 3.5f; w.ch[0].facing = 1;
    w.nbr = 1; w.br[0].uid = 950; w.br[0].life = 100; w.br[0].pos.x = 2.5f; w.br[0].pos.z = 1.5f;
    { PadOut o2; int both = 0; for (int fr = 0; fr < 60 * 3; fr++, now += 16) { w.frames++; brain_think(&bb[0], bb, 3, &w, 1, now, &out); brain_think(&bb[1], bb, 3, &w, 1, now, &o2);
          move_me_toward_stick(fr); c3b->pos.x += o2.axis[AX_LX] / 32767.0f * 0.08f; c3b->pos.z += o2.axis[AX_LY] / 32767.0f * 0.04f;
          if (bb[0].breakUid == 950 && bb[1].breakUid == 950) both++; }
      int claimer = bb[0].breakUid == 950 ? 0 : bb[1].breakUid == 950 ? 1 : -1;
      w.br[0].life = -5; for (int fr = 0; fr < 60; fr++, now += 16) { w.frames++; brain_think(&bb[0], bb, 3, &w, 1, now, &out); brain_think(&bb[1], bb, 3, &w, 1, now, &o2); }
      CHECK(claimer >= 0 && both == 0 && bb[0].breakUid == 0 && bb[1].breakUid == 0,
            "m6.2: one bot per breakable: claimed by bot %d, frames both on it %d; released after it broke (%u/%u)", claimer, both, bb[0].breakUid, bb[1].breakUid); }

    /* m6.3: the m6.2-run1b container (uid 5 at 21.6, 0.0): solid within 1.1 in x and 0.6 in depth, floor edge z 0.1.
       P1 (Shiva) broke it with one kick from dx 2.2, dz 0.13. Bots start where they stood at 14:32:55 (P1 16.0/0.3
       facing right, camera 16.9); the m6.2 bots stopped short (ahead-of-P1 cap) or got stuck at its corner (20.5, 0.6). */
    for (int sc = 0; sc < 2; sc++) {
        world_reset(); bot_reset(1); w.cam.x = 16.9f; w.ch[0].pos.x = 16.0f; w.ch[0].pos.z = 0.3f; w.ch[0].facing = 1;
        w.slot[2].ctrl = 2; w.slot[2].type = 1; Chr *c3c = mk(2, 3, 0, 2, 14.2f, 0.1f, 1, CS_STANDWALK);
        brain_reset(&bb[1], 1, 3, 3, "Bot3", now); bb[1].id.ctrlId = 2; bb[1].id.verifiedSlot = 2; bb[1].id.preKnown = 1; bb[1].id.npre = 1; bb[1].id.attachAt = now - 60000; bb[1].id.lastSeen = now; bb[1].sawLevel = 1; bb[1].introDone = 1; bb[1].xSign = 1;
        g_me = &w.ch[1]; g_me->pos.x = sc ? 20.5f : 13.6f; g_me->pos.z = sc ? 0.6f : 2.8f;
        w.nbr = 1; w.br[0].uid = 5; w.br[0].life = 1; w.br[0].entered = 1; w.br[0].pos.x = 21.6f; w.br[0].pos.z = 0.0f;
        Chr *fs[2] = { g_me, c3c }; int both = 0, fr, last[2] = { 0, 0 }, swings = 0;
        for (fr = 0; fr < 60 * 10 && w.br[0].life > 0; fr++, now += 16) {
            PadOut oo[2]; w.frames++; brain_think(&bb[0], bb, 3, &w, 1, now, &oo[0]); brain_think(&bb[1], bb, 3, &w, 1, now, &oo[1]);
            if (bb[0].breakUid == 5 && bb[1].breakUid == 5) both++;
            for (int k = 0; k < 2; k++) {
                Chr *c = fs[k]; float nx = c->pos.x + oo[k].axis[AX_LX] / 32767.0f * 0.08f, nz = c->pos.z + oo[k].axis[AX_LY] / 32767.0f * 0.04f;
                if (nz < 0.1f) nz = 0.1f;
                if (fabsf(nx - 21.6f) < 1.1f && fabsf(nz) < 0.6f) { if (!(fabsf(c->pos.x - 21.6f) < 1.1f && fabsf(nz) < 0.6f)) nx = c->pos.x; if (fabsf(nx - 21.6f) < 1.1f && fabsf(c->pos.z) >= 0.6f) nz = c->pos.z; }
                if (oo[k].axis[AX_LX]) c->facing = oo[k].axis[AX_LX] > 0 ? 1 : -1;
                c->pos.x = nx; c->pos.z = nz;
                if (oo[k].btn[BTN_X] && !last[k]) { swings++; if (fabsf(c->pos.z) <= 0.35f && fabsf(c->pos.x - 21.6f) <= 2.2f && c->facing == (c->pos.x < 21.6f ? 1 : -1)) w.br[0].life -= 2; }
                last[k] = oo[k].btn[BTN_X];
            }
        }
        CHECK(w.br[0].life <= 0 && both == 0, "m6.3: real container, bots from %s: broken %d in %.1f s (swings %d), frames with both bots on it %d; bot ends at (%.2f, %.2f)",
              sc ? "its corner (20.5, 0.6)" : "their 14:32:55 spots", w.br[0].life <= 0, fr * 0.016f, swings, both, g_me->pos.x, g_me->pos.z);
    }

    /* m6-3. food goes to the lowest-life bot; P1 keeps priority */
    world_reset(); bot_reset(1);
    w.slot[2].ctrl = 2; w.slot[2].type = 1; mk(2, 3, 0, 2, 3.0f, 1.5f, 1, CS_STANDWALK);
    brain_reset(&bb[1], 1, 3, 3, "Bot3", now); bb[1].identified = 1; bb[1].playerSlot = 2; bb[1].id.ctrlId = 2;
    w.ch[1].life = 50; w.ch[2].life = 30;
    w.npk = 1; w.pk[0].addr = 0x4000; w.pk[0].kind = PK_FOOD; w.pk[0].lifeGain = 30; w.pk[0].pos.x = 0.5f; w.pk[0].pos.z = 1.5f;
    run(60, &t, NULL); int bLow = t.btn[BTN_B], share = dec(D_FOODSHARE);
    w.ch[2].life = 90; g_me = &w.ch[1];
    run(60, &t, move_me_toward_stick);
    CHECK(bLow == 0 && share == 1 && t.btn[BTN_B] >= 1, "m6: food: other bot at 30%% (we 50%%): left it (B %d, noted %d); other bot at 90%%: we take it (B %d)", bLow, share, t.btn[BTN_B]);
    world_reset(); bot_reset(1); w.ch[1].life = 50; w.ch[0].life = 40;
    w.npk = 1; w.pk[0].addr = 0x4001; w.pk[0].kind = PK_FOOD; w.pk[0].lifeGain = 30; w.pk[0].pos.x = 0.5f; w.pk[0].pos.z = 1.5f;
    run(60, &t, NULL);
    CHECK(t.btn[BTN_B] == 0 && dec(D_FOODLEFT) == 1, "m6: food: P1 at 40%% (we 50%%): P1 first (B %d)", t.btn[BTN_B]);

    /* m6-4a. quiet: the bot in front of a walking P1 drops behind it and never idles in front */
    world_reset(); bot_reset(1); g_me = &w.ch[1]; g_me->pos.x = 1.0f; g_me->pos.z = 2.6f;
    w.ch[0].pos.x = 0; w.ch[0].pos.z = 1.5f; w.ch[0].facing = 1;
    { int idleFront = 0; for (int fr = 0; fr < 60 * 4; fr++, now += 16) {
          w.frames++; w.ch[0].pos.x += 0.02f; w.ch[0].stickX = 1.0f; brain_think(&bb[0], bb, 3, &w, 1, now, &out); move_me_toward_stick(fr);
          if (!out.axis[AX_LX] && !out.axis[AX_LY] && g_me->pos.x > w.ch[0].pos.x + 0.3f) idleFront++; }
      CHECK(g_me->pos.x < w.ch[0].pos.x && idleFront == 0, "m6: follow: bot ends behind P1 (dx %.2f), idle in front %d frames", g_me->pos.x - w.ch[0].pos.x, idleFront); }
    /* m6-4b. P1 walks into the bot on its lane: it steps aside in depth */
    world_reset(); bot_reset(1); g_me = &w.ch[1]; g_me->pos.x = 0.9f; g_me->pos.z = 1.5f;
    w.ch[0].pos.x = 0; w.ch[0].pos.z = 1.5f; w.ch[0].facing = 1; w.ch[0].stickX = 1.0f;
    run(30, &t, move_me_toward_stick);
    CHECK(dec(D_YIELD) == 1 && fabsf(g_me->pos.z - 1.5f) > 0.6f && t.stickL + t.stickR == 0, "m6: P1 pushes toward the bot: made way %d, moved %.2f in depth, sideways frames %d",
          dec(D_YIELD), g_me->pos.z - 1.5f, t.stickL + t.stickR);

    /* m6-5a. two bots trailing P1: staggered lanes, apart */
    world_reset(); bot_reset(1);
    w.slot[2].ctrl = 2; w.slot[2].type = 1; Chr *c3 = mk(2, 3, 0, 2, 0.5f, 1.5f, 1, CS_STANDWALK);
    brain_reset(&bb[1], 1, 3, 3, "Bot3", now); bb[1].id.ctrlId = 2; bb[1].id.verifiedSlot = 2; bb[1].id.preKnown = 1; bb[1].id.npre = 1; bb[1].id.attachAt = now - 60000; bb[1].id.lastSeen = now; bb[1].sawLevel = 1; bb[1].introDone = 1; bb[1].xSign = 1;
    g_me = &w.ch[1]; g_me->pos.x = 0.6f; g_me->pos.z = 1.5f; w.ch[0].pos.x = 3.0f; w.ch[0].pos.z = 1.5f; w.ch[0].facing = 1;
    { PadOut o2; for (int fr = 0; fr < 60 * 4; fr++, now += 16) { w.frames++; brain_think(&bb[0], bb, 3, &w, 1, now, &out); brain_think(&bb[1], bb, 3, &w, 1, now, &o2);
          move_me_toward_stick(fr); c3->pos.x += o2.axis[AX_LX] / 32767.0f * 0.08f; c3->pos.z += o2.axis[AX_LY] / 32767.0f * 0.04f; }
      CHECK(fabsf(g_me->pos.z - c3->pos.z) > 1.5f && g_me->pos.x < 3.0f && c3->pos.x < 3.0f, "m6: two bots behind P1 on different lanes: z %.2f / %.2f, x %.2f / %.2f (P1 at 3.0)", g_me->pos.z, c3->pos.z, g_me->pos.x, c3->pos.x); }
    /* m6-5b. another bot already on our side of the only enemy: take the far side */
    world_reset(); bot_reset(1);
    w.slot[2].ctrl = 2; w.slot[2].type = 1; mk(2, 3, 0, 2, 2.1f, 1.5f, 1, CS_STANDWALK);
    brain_reset(&bb[1], 1, 3, 3, "Bot3", now); bb[1].identified = 1; bb[1].playerSlot = 2; bb[1].id.ctrlId = 2; bb[1].targetUid = 50;
    mk(3, 50, 1, -1, 3.0f, 1.5f, -1, CS_STANDWALK); bb[0].lastBlitz = now; bb[0].lastJumpIn = now;
    run(10, &t, NULL);
    CHECK(dec(D_SPREAD) == 1, "m6: spread: the other bot holds our side of uid 50, we take the far side (%d)", dec(D_SPREAD));

    /* m6-6. jumps: jump-in when the blitz is cooling down, air hits on an airborne enemy, Blaze jump-cancel combo, caps, anti-air */
    world_reset(); bot_reset(1); mk(2, 50, 1, -1, 2.4f, 1.5f, -1, CS_STANDWALK); bb[0].lastBlitz = now;
    run(20, &t, NULL);
    CHECK(dec(D_JUMPIN) == 1 && t.btn[BTN_A] == 1, "m6: jump-in at mid range (blitz cooling down): %d, A %d", dec(D_JUMPIN), t.btn[BTN_A]);
    world_reset(); bot_reset(1); mk(2, 50, 1, -1, 0.9f, 1.5f, -1, CS_AIRLIGHTHIT); w.ch[2].cantBeGrabbed = 1;
    run(60 * 20, &t, NULL);
    { int jumps = dec(D_AIRATTACK) + dec(D_JUMPIN) + dec(D_CROWDKICK) + dec(D_AIRSPECIAL) + dec(D_JCCOMBO);
      CHECK(dec(D_AIRATTACK) >= 3 && jumps <= 6, "m6: airborne enemy for 20 s: air attacks %d, jump moves %d (cap 6 per 20 s)", dec(D_AIRATTACK), jumps); }
    world_reset(); bot_reset(1); brain_set_character(&bb[0], 1, "Blaze"); bb[0].profFor = 1; w.ps[1].playable = 0;
    mk(2, 50, 1, -1, 0.9f, 1.5f, -1, CS_STANDWALK); w.ch[2].cantBeGrabbed = 1;
    run(60 * 4, &t, NULL);
    CHECK(dec(D_JCCOMBO) >= 1 && dec(D_COMBO) >= 2, "m6: Blaze: X x4 -> jump cancel -> air down+X -> air X: %d (strings %d)", dec(D_JCCOMBO), dec(D_COMBO));
    world_reset(); bot_reset(1); mk(2, 50, 1, -1, 2.4f, 1.5f, -1, CS_STANDWALK); bb[0].lastBlitz = now;
    run(20, &t, NULL);                       /* jump-in starts */
    w.ch[1].state = CS_STANDHIT; run(10, &t, NULL); w.ch[1].state = CS_STANDWALK;   /* knocked out of it */
    bb[0].lastBlitz = now; w.ch[2].pos.x = 2.4f; run(60 * 6, &t, NULL);
    CHECK(dec(D_ANTIAIR) == 1 && dec(D_JUMPIN) == 1, "m6: hit out of a jump-in: anti-air noted %d, no more jump-ins at it (%d)", dec(D_ANTIAIR), dec(D_JUMPIN));

    /* m6-1. silent identity check: our stick never shows up on the slot's fighter -> stand still, retry later */
    world_reset(); bot_reset(1); bb[0].id.verifiedSlot = -1; snprintf(bb[0].id.how, sizeof bb[0].id.how, "seen joining as P2");
    mk(2, 50, 1, -1, 6.0f, 1.5f, -1, CS_STANDWALK);
    run(120, &t, NULL);
    int failed = bb[0].id.verifiedSlot == -1 && !bb[0].identified && bb[0].id.ckFails == 1 && bb[0].id.ctrlId == 1;
    run(60, &t, NULL); int still = t.neutralFrames;
    CHECK(failed && still == 60 && t.rev == 0, "m6: silent check fails (fighter ignores our stick): unidentified %d, controller kept, neutral %d/60 after", failed, still);
    world_reset(); bot_reset(1); bb[0].id.verifiedSlot = -1; snprintf(bb[0].id.how, sizeof bb[0].id.how, "device order (P2)");
    mk(2, 50, 1, -1, 6.0f, 1.5f, -1, CS_STANDWALK);
    run(120, &t, NULL);
    CHECK(bb[0].id.ctrlId == -1 && bb[0].id.nrej == 1, "m6: device-order guess fails the silent check: controller dropped (%d, rejected %d)", bb[0].id.ctrlId, bb[0].id.nrej);
    world_reset(); bot_reset(1); bb[0].id.verifiedSlot = -1; g_me = &w.ch[1];
    mk(2, 50, 1, -1, 6.0f, 1.5f, -1, CS_STANDWALK);
    { for (int fr = 0; fr < 120; fr++, now += 16) { w.frames++; brain_think(&bb[0], bb, 3, &w, 1, now, &out); g_me->stickX = out.axis[AX_LX] / 32767.0f; } }
    CHECK(!bb[0].id.pvPending && bb[0].identified && bb[0].id.verifiedSlot == 1, "m6: silent check passes while walking (pending %d, identified %d)", bb[0].id.pvPending, bb[0].identified);

    /* m5-7. stage end through a short Loading screen (the m4 log had no summaries) */
    world_reset(); bot_reset(1); mk(2, 67, 1, -1, 0.9f, 1.5f, -1, CS_STANDWALK);
    run(30, &t, NULL);
    w.ok = 0; w.nch = 0; strcpy(w.menuName, "Loading");
    run(30, &t, NULL);
    { int left = 0; for (int i = 0; i < D_COUNT; i++) left += bb[0].dec[i];
      CHECK(!bb[0].inLevel && left == 0, "m5: 0.5 s Loading screen ends the stage (in level %d, left %d)", bb[0].inLevel, left); }

    /* 15. stage end: summary logged, counts reset */
    g_quiet = 0; C.logLevel = LV_INFO;
    world_reset(); bot_reset(1); mk(2, 50, 1, -1, 0.9f, 1.5f, -1, CS_STANDWALK);
    run(30, &t, NULL);
    w.ok = 0; w.nch = 0;
    run(60 * 3, &t, NULL);
    g_quiet = 1;
    int left = 0; for (int i = 0; i < D_COUNT; i++) left += bb[0].dec[i];
    CHECK(!bb[0].inLevel && left == 0, "stage end: per-stage counts reset after the summary (in level %d, left %d)", bb[0].inLevel, left);

    /* 16. B never outside a running stage: character select menus are covered by state_test (forbidden buttons 0) */
    world_reset(); bot_reset(1); bb[0].identified = 0; bb[0].id.verifiedSlot = -1; bb[0].id.ckRetryAt = now + 60000;   /* m6: silent check failed */
    w.npk = 1; w.pk[0].addr = 0x3000; w.pk[0].kind = PK_WEAPON; w.pk[0].pos.x = 0.0f; w.pk[0].pos.z = 1.5f;
    run(240, &t, NULL);
    CHECK(!t.btn[BTN_B], "not identified (silent check failed), weapon underfoot: B %d (expect 0)", t.btn[BTN_B]);

    /* ---------------- m7.4 ---------------- */
    /* a. the robot's seated driver (m7.3-run2: scripted boss at z -0.2, y 0.8-3.6; the floor ends at z 1.2) is never chased */
    world_reset(); bot_reset(1); w.ch[0].pos.z = 1.2f;
    { Chr *d = mk(2, 70, 1, -1, 2.0f, -0.2f, -1, CS_STANDWALK); d->pos.y = 2.0f; d->scripted = 1; d->boss = 1; d->robotDriver = 1; d->life = 500; }
    run(120, &t, NULL);
    CHECK(bb[0].targetUid != 70 && t.btn[BTN_X] == 0 && dec(D_OFFFLOOR) == 1, "m7.4: seated robot driver not chased (target %u, X %d, off-floor notes %d)", bb[0].targetUid, t.btn[BTN_X], dec(D_OFFFLOOR));
    world_reset(); bot_reset(1); w.ch[0].pos.z = 1.2f;   /* no robot data (the m7.3-run2 log): scripted boss off the floor */
    { Chr *d = mk(2, 71, 1, -1, 2.0f, -0.2f, -1, CS_STANDWALK); d->pos.y = 2.0f; d->scripted = 1; d->boss = 1; d->life = 500; }
    { Chr *e = mk(3, 72, 1, -1, -2.0f, 1.0f, 1, CS_STANDWALK); (void)e; }
    run(120, &t, NULL);
    { float lo = 0, hi = 0; int known = brain_floor(&lo, &hi);
      CHECK(known && lo > 1.15f && lo < 1.25f && bb[0].targetUid == 72, "m7.4: scripted boss at z -0.2 skipped, an ordinary enemy at z 1.0 (0.2 past the seen floor) still fought (floor %d %.2f..%.2f, target %u)", known, lo, hi, bb[0].targetUid); }
    /* b. a non-scripted enemy past a learned floor edge: unreachable after 1.5 s */
    g_brainLearnWalls = 1;
    world_reset(); bot_reset(1); w.ch[1].pos.z = 1.2f;
    mk(2, 73, 1, -1, 0.6f, -1.5f, -1, CS_STANDWALK);
    run(60 * 4, &t, NULL);
    { int i = -1; for (int k = 0; k < 12; k++) if (bb[0].unreach[k].uid == 73) i = k;
      CHECK(dec(D_OFFFLOOR) == 1 && i >= 0 && bb[0].targetUid != 73, "m7.4: enemy 2.7 past the floor edge (bot pinned at z 1.2): unreachable after 1.5 s (notes %d, marked %d, target %u)", dec(D_OFFFLOOR), i >= 0, bb[0].targetUid); }
    g_brainLearnWalls = 0;
    /* c. m7.5: robot legs as in m7.4-run1: hidden at y -9 first, then planted ON the floor (y 0, z 3.2) = ground target;
       raised = jump-attack target; stabbing down = keep out; Body / rear legs (background) never */
    world_reset(); bot_reset(1); w.cam.x = 3.0f; w.ch[0].pos.z = 1.16f; w.ch[0].pos.x = -1.0f; w.ch[1].pos.z = 4.59f; w.ch[1].pos.x = 3.0f; g_me = &w.ch[1];
    { Chr *d = mk(2, 90, 1, -1, 0.0f, -0.2f, -1, CS_STANDWALK); d->pos.y = 2.0f; d->scripted = 1; d->boss = 1; d->robotDriver = 1; d->life = 552.8f; }
    w.nrb = 1; w.rb[0].uid = 1; w.rb[0].active = 1; w.rb[0].broken = 0; w.rb[0].nDrivers = 2; w.rb[0].drivers[0] = 90; w.rb[0].drivers[1] = 91;
    w.nrp = 4;
    { static const struct { uint32_t uid; const char *n; float x, y, z; } P[4] = { { 80, "PatteLeft", -3.52f, -9.16f, -0.96f }, { 81, "PatteRight", 7.5f, -9.16f, -0.96f }, { 82, "Body", 0.0f, -9.0f, -0.28f }, { 83, "PatteRightRear", 5.0f, -9.0f, -1.34f } };
      for (int i = 0; i < 4; i++) { w.rp[i].uid = P[i].uid; w.rp[i].robotId = 1; w.rp[i].partId = i; snprintf(w.rp[i].name, sizeof w.rp[i].name, "%s", P[i].n); w.rp[i].pos.x = P[i].x; w.rp[i].pos.y = P[i].y; w.rp[i].pos.z = P[i].z; } }
    run(30, &t, NULL);
    CHECK(bb[0].targetUid < 80 || bb[0].targetUid > 83, "m7.5: parts hidden at y -9: not targets (target %u)", bb[0].targetUid);
    w.ch[1].pos.z = 3.0f;
    w.rp[0].pos.x = -4.5f; w.rp[0].pos.y = 0.0f; w.rp[0].pos.z = 3.2f; w.rp[1].pos.x = 7.5f; w.rp[1].pos.y = 0.0f; w.rp[1].pos.z = 3.2f;
    w.rp[2].pos.y = 0.5f; w.rp[2].pos.z = -0.28f; w.rp[3].pos.y = -4.44f; w.rp[3].pos.z = -1.34f;
    run(150, &t, move_me_toward_stick);
    CHECK(bb[0].targetUid == 81 && t.btn[BTN_X] > 0 && dec(D_ROBOTPART) >= 1, "m7.5: planted right leg (y 0, z 3.2, on the floor): walked over and hitting it (target %u, X %d, at x %.1f)", bb[0].targetUid, t.btn[BTN_X], w.ch[1].pos.x);
    w.rp[1].pos.x = 11.25f; w.rp[1].pos.y = 1.56f; w.rp[1].pos.z = 3.97f; w.rp[1].vel.y = 0.0f;   /* raised (as logged), hovering */
    { int a0 = t.btn[BTN_A]; (void)a0; }
    run(240, &t, move_me_toward_stick);
    CHECK(bb[0].targetUid == 81 && t.btn[BTN_A] >= 1 && t.btn[BTN_X] >= 1, "m7.5: raised right leg (y 1.56): still the target, jump attacks (A %d, X %d, at x %.1f z %.1f)", t.btn[BTN_A], t.btn[BTN_X], w.ch[1].pos.x, w.ch[1].pos.z);
    w.ch[1].pos.x = 11.0f; w.ch[1].pos.z = 3.9f; w.rp[1].vel.y = -0.12f; w.rp[1].pos.y = 1.0f;   /* stabbing down on top of us */
    run(40, &t, move_me_toward_stick);
    CHECK(t.btn[BTN_X] == 0 && dec(D_PARTEVADE) >= 1 && (fabsf(w.ch[1].pos.x - 11.25f) > 1.0f || fabsf(w.ch[1].pos.z - 3.97f) > 0.8f), "m7.5: leg stabbing down: no swings, moving out of the footprint (X %d, notes %d, at x %.1f z %.1f)", t.btn[BTN_X], dec(D_PARTEVADE), w.ch[1].pos.x, w.ch[1].pos.z);
    w.rp[1].vel.y = 0.0f; w.rp[1].pos.y = 0.0f; w.rp[1].pos.z = 3.2f;
    run(150, &t, move_me_toward_stick);
    CHECK(bb[0].targetUid == 81 && t.btn[BTN_X] > 0, "m7.5: leg planted again: hitting it again (target %u, X %d)", bb[0].targetUid, t.btn[BTN_X]);
    { int n = 0, st[W_MAXRP], h[W_MAXRP]; uint32_t u[W_MAXRP]; uint64_t pm[W_MAXRP], rm[W_MAXRP], bm = 0, bj = 0; brain_part_stats(&n, u, st, pm, rm, h, &bm, &bj);
      uint64_t bodyMs = 0; for (int i = 0; i < n; i++) if (u[i] == 82 || u[i] == 83) bodyMs += pm[i] + rm[i];
      CHECK(bm > 3000 && bj >= 1 && bodyMs == 0, "m7.5: part stats: bot on legs %.1f s, %llu jump attack(s); body/rear on floor %.1f s", bm / 1000.0, (unsigned long long)bj, bodyMs / 1000.0); }
    w.rb[0].broken = 1;
    run(60, &t, NULL);
    CHECK(bb[0].targetUid != 81 && bb[0].targetUid != 80, "m7.4: robot broken: legs no longer targets (target %u)", bb[0].targetUid);
    w.rb[0].broken = 0; w.ch[2].life = -2.0f;
    run(60, &t, NULL);
    CHECK(bb[0].targetUid != 81 && bb[0].targetUid != 80 && t.btn[BTN_X] == 0, "m7.4: nobody in the seat (driver down): legs not targets (target %u, X %d)", bb[0].targetUid, t.btn[BTN_X]);
    /* d. friendly fire retaliation: 3 human hits -> exactly 3 hits back; bot-on-bot hits -> nothing */
    for (int ff = 1; ff >= 0; ff--) {
        world_reset(); bot_reset(1); w.friendlyFire = ff; C.friendlyRetaliate = 1;
        w.slot[2].ctrl = 2; w.slot[2].type = 1; mk(2, 3, 0, 2, -1.5f, 1.5f, 1, CS_STANDWALK);
        brain_reset(&bb[1], 1, 3, 3, "Bot3", now);
        bb[1].id.ctrlId = 2; bb[1].id.verifiedSlot = 2; bb[1].id.preKnown = 1; bb[1].id.pre[0] = 0; bb[1].id.npre = 1; bb[1].id.attachAt = now - 60000; bb[1].id.lastSeen = now; bb[1].sawLevel = 1; bb[1].introDone = 1; bb[1].xSign = 1;
        w.ch[0].pos.x = 3.6f; w.ch[0].pos.z = 1.5f; w.ch[0].life = 100;   /* hits from 3.6 away (a long move): out of our reach while the debt builds */ w.ch[1].life = 100; w.ch[2].life = 100;
        PadOut o2; int lastX[2] = { 0, 0 }, xs[2] = { 0, 0 }, connects = 0, pend = -1;
#define STEP(n) for (int fr_ = 0; fr_ < (n); fr_++, now += 16) { w.frames++; brain_think(&bb[0], bb, 3, &w, 1, now, &out); brain_think(&bb[1], bb, 3, &w, 1, now, &o2); \
            if (out.btn[BTN_X] && !lastX[0]) { xs[0]++; pend = 3; } lastX[0] = out.btn[BTN_X]; if (o2.btn[BTN_X] && !lastX[1]) xs[1]++; lastX[1] = o2.btn[BTN_X]; \
            if (pend > 0 && --pend == 0) { w.ch[0].life -= 3; w.ch[0].hitSrc = 2; connects++; } }
        STEP(4);
        for (int h = 0; h < 3; h++) { w.ch[1].life -= 5; w.ch[1].hitSrc = 1; w.ch[1].state = CS_STANDHIT; STEP(2); w.ch[1].state = CS_STANDWALK; STEP(20); }   /* P1 hits P2 x3 */
        for (int h = 0; h < 2; h++) { w.ch[1].life -= 5; w.ch[1].hitSrc = 3; STEP(20); }                                                                     /* P3 (bot) hits P2 x2 */
        for (int h = 0; h < 2; h++) { w.ch[2].life -= 5; w.ch[2].hitSrc = 2; STEP(20); }                                                                     /* P2 (bot) hits P3 x2 */
        int owedP1 = brain_retal_debt(1, 0), owedP3 = brain_retal_debt(1, 2), owed3to2 = brain_retal_debt(2, 1);
        w.ch[0].pos.x = 0.8f;                                           /* P1 in reach: pay back */
        STEP(60 * 6);
        if (ff) CHECK(owedP1 == 3 && owedP3 == 0 && owed3to2 == 0 && connects == 3 && brain_retal_debt(1, 0) == 0 && xs[0] == 3 && xs[1] == 0,
                      "m7.4: friendly fire on: P1 hit P2 3x -> owed %d, landed %d back (X %d, still owed %d); bot-on-bot: P2 owes P3 %d, P3 owes P2 %d, P3 swung %d",
                      owedP1, connects, xs[0], brain_retal_debt(1, 0), owedP3, owed3to2, xs[1]);
        else CHECK(owedP1 == 0 && xs[0] == 0 && connects == 0, "m7.4: friendly fire off: no debt (%d), no swings (%d)", owedP1, xs[0]);
#undef STEP
    }
    /* e. debt expires after 20 s without a chance (P1 far away) */
    world_reset(); bot_reset(1); w.friendlyFire = 1;
    w.ch[0].pos.x = 0.8f; run(4, &t, NULL);
    w.ch[1].life -= 5; w.ch[1].hitSrc = 1; run(4, &t, NULL);
    { int owed = brain_retal_debt(1, 0); w.ch[0].pos.x = -8.0f; w.ch[0].pos.z = 3.5f; mk(2, 74, 1, -1, 1.0f, 1.5f, -1, CS_STANDWALK);
      run(60 * 21, &t, NULL);
      CHECK(owed == 1 && brain_retal_debt(1, 0) == 0, "m7.4: owed %d, gone after 21 s with P1 out of reach (%d)", owed, brain_retal_debt(1, 0)); }

    printf("%s (%d failure(s))\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
