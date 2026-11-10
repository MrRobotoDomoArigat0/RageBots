/*
 * SoR4Bots brain (milestone 2).
 * Inputs: the World read by sor4state.c. Output: one virtual pad state per bot.
 *
 *  - join: right after the bot turns on it taps JoinButton (A) JoinPresses times, like milestone 1.
 *  - identity (m2.2): our controllerId (seen joining) -> the player slot that has it -> the fighter with
 *    that player id. A quick left/right stick wiggle only cross-checks it. Unknown = neutral.
 *  - fight: nearest enemy (|dx| + 3|dz|, knocked-down ones last); get on its lane (z), get within
 *    attack range on x, face it, tap attack in a short combo, pause, repeat. Range and lane tolerance
 *    start from the camera size and shrink by themselves if combos keep missing.
 *  - safety: below RetreatLifePercent it backs off behind P1; specials (they cost life) only at or
 *    above SpecialMinLifePercent and when two or more enemies are close.
 *  - follow: no enemy near -> stay near P1.
 *  - fail safe: if anything is unknown, paused or stale, the pad stays neutral (the milestone-1 test
 *    pattern only with [Brain] FallbackToTestPattern=1 or Mode=Test).
 */
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include "sor4brain.h"

#define STICK 30000
#define WHY(...) snprintf(b->why, sizeof b->why, __VA_ARGS__)
#include <stdio.h>
enum { M_JOIN, M_TEST, M_IDENT, M_IDLE, M_FOLLOW, M_APPROACH, M_ATTACK, M_RETREAT, M_HURT, M_SCRIPT, M_DEFEND, M_GRAB, M_PICKUP, M_CSEL };
static const char *const k_modes[] = { "join", "test pattern", "identify", "idle", "follow P1", "approach", "attack", "retreat", "hurt", "cutscene", "defend", "grab", "pickup", "character select" };

static float fabsf_(float v) { return v < 0 ? -v : v; }
static int s_focused = 1; static uint64_t s_focusSince;
void brain_set_focus(int focused, uint64_t now) {
    focused = focused ? 1 : 0;
    if (focused != s_focused) { s_focused = focused; if (focused) s_focusSince = now; }
}
/* ---- m2.4 safety gate: why the pad must stay neutral right now (logged on change, not per frame) ---- */
enum { G_PLAY, G_UNFOCUSED, G_FOCUSGRACE, G_PAUSED, G_RESUME, G_NOTLIVE, G_MENU_TAP, G_MENU_WAIT, G_MENU_DONE, G_NOSTATE, G_IDENT, G_CS_WAIT };
static const char *const k_gate[] = { "gameplay: inputs allowed", "BLOCKED: game window not focused", "BLOCKED: focus just came back",
    "BLOCKED: game paused / in a menu", "BLOCKED: just resumed", "BLOCKED: stage not advancing", "menu: character-select presses allowed",
    "BLOCKED: menu, not proven to be character select", "BLOCKED: menu (confirm taps used up or not armed)", "BLOCKED: game state not readable",
    "BLOCKED: not playing yet", "character select: on the pick, nothing to press" };
static void gate_note(BotBrain *b, int cat, uint64_t now, const char *detail) {
    if (cat != b->gateCat) { b->gateCat = cat; b->gateCatSince = now; snprintf(b->gateWhy, sizeof b->gateWhy, "%s", detail ? detail : ""); }
    /* log a change once it has lasted 250 ms (so GC hiccups don't flood the log); blocking itself is immediate */
    if (b->gateCat != b->gateLoggedCat && now - b->gateCatSince >= 250) {
        b->gateLoggedCat = b->gateCat;
        LOGI("%s: %s%s%s", b->name, k_gate[b->gateCat], b->gateWhy[0] ? " - " : "", b->gateWhy);
    }
}
static int sgn(float v) { return v > 0 ? 1 : v < 0 ? -1 : 0; }

void brain_reset(BotBrain *b, int index, int nbots, int slot, const char *name, uint64_t now) {
    BotIdent keep = b->id; int keepX = b->xSign;
    memset(b, 0, sizeof *b);
    b->id = keep; b->playerSlot = -1;
    b->index = index; b->nbots = nbots; b->slot = slot;
    strncpy(b->name, name, sizeof b->name - 1);
    b->activeSince = now; b->xSign = 1; b->zSign = 1;
    b->rangeScale = 1.0f; b->laneScale = 1.0f; b->mode = -1; b->gateCat = b->gateLoggedCat = -1;
    if (b->id.verifiedSlot >= 0 && keepX) b->xSign = keepX;
    b->id.ckSlot = -1;
}
void brain_init_detached(BotBrain *b, int index, const char *name) {
    memset(b, 0, sizeof *b);
    b->id.ctrlId = -1; b->id.verifiedSlot = -1; b->id.ckSlot = -1; b->playerSlot = -1;
    b->index = index; b->mode = -1; b->gateCat = b->gateLoggedCat = -1; b->xSign = b->zSign = 1;
    b->rangeScale = b->laneScale = 1.0f;
    strncpy(b->name, name, sizeof b->name - 1);
}
/* m3/m4: per-character profiles, matched by the roster name the game reports (m3 log: "Axel", "Blaze",
   "Cherry", "Floyd", "Adam", "Estel", "Max", retro ones like "Axel_SOR1"; DLC Shiva assumed "Shiva").
   Fields: buttons (-1 = ini), range x, comboLen, run, dash, juggle, pogo, grabStyle, fwdSpecial, stunGrenade, blitzThenGrab, blitz */
static const CharProfile k_profiles[] = {
    { "",       -1, -1, -1, 1.00f, 0, 0, 0, 0, 0, GS_DEFAULT, 0, 0, 0, 1, 0 },  /* default and retro characters: ini ComboHits, blitz, grabs */
    { "Axel",   -1, -1, -1, 1.00f, 5, 0, 0, 0, 0, GS_DEFAULT, 0, 0, 0, 1, 0 },  /* X x5, last hit launches; blitz Grand Upper */
    { "Blaze",  -1, -1, -1, 1.00f, 4, 0, 0, 0, 0, GS_DEFAULT, 0, 0, 0, 1, 1 },  /* 4th hit (Kakato) launches; m6: X x4 -> A -> air down+X -> air X */
    { "Cherry", -1, -1, -1, 1.00f, 4, 1, 0, 0, 1, GS_DEFAULT, 0, 0, 0, 1, 0 },  /* run + flying knee, pogo on heads, uppercut ender */
    { "Floyd",  -1, -1, -1, 1.15f, 4, 0, 0, 0, 0, GS_FLOYD,   1, 0, 0, 1, 0 },  /* long reach, double grab, fwd+Y pulls into a grab */
    { "Adam",   -1, -1, -1, 1.00f, 3, 0, 1, 0, 0, GS_DEFAULT, 0, 0, 0, 1, 0 },  /* 3rd hit launches; fwd,fwd = dash */
    { "Max",    -1, -1, -1, 1.10f, 4, 0, 0, 0, 0, GS_MAX,     0, 0, 1, 1, 0 },  /* grab + jump = piledriver; blitz stuns -> grab */
    { "Shiva",  -1, -1, -1, 1.00f, 4, 0, 0, 1, 0, GS_DEFAULT, 1, 0, 0, 1, 0 },  /* 4th hit launches -> jump cancel air X x4; fwd+Y auto-distance */
    { "Estel",  -1, -1, -1, 1.00f, 4, 0, 0, 0, 0, GS_DEFAULT, 0, 1, 0, 1, 0 },  /* neutral Y = stun grenade, then the full string */
};
static const char *k_retro_note = "retro character: default moves (string, blitz, back attack, grabs, specials)";
void brain_set_character(BotBrain *b, int rosterIndex, const char *name) {
    const CharProfile *p = &k_profiles[0];
    for (size_t i = 1; i < sizeof k_profiles / sizeof *k_profiles; i++) if (name && !_stricmp(name, k_profiles[i].name)) p = &k_profiles[i];
    if (p != b->prof || rosterIndex != b->profFor)
        LOGI("%s: plays %s (roster #%d); using the %s profile%s%s.", b->name, name && name[0] ? name : "an unknown character", rosterIndex,
             p->name[0] ? p->name : "default", p->name[0] ? "" : " - ", p->name[0] ? "" : name && strstr(name, "_SOR") ? k_retro_note : "default moves");
    b->prof = p; b->profFor = rosterIndex; b->profNamed = name && name[0] && strcmp(name, "?");
}
static int prof_btn(const BotBrain *b, int which, int def) {
    if (!b->prof) return def;
    int v = which == 0 ? b->prof->attackButton : which == 1 ? b->prof->specialButton : b->prof->jumpButton;
    return v >= 0 ? v : def;
}
static void neutral(PadOut *o) {
    memset(o, 0, sizeof *o);
    o->axis[AX_LT] = o->axis[AX_RT] = -32768;
}
static void set_mode(BotBrain *b, int m, uint64_t now, const char *extra) {
    if (b->mode == m) return;
    b->mode = m; b->modeSince = now;
    int important = m == M_JOIN || m == M_CSEL || m == M_TEST || m == M_IDENT || m == M_SCRIPT || m == M_RETREAT;
    logf_(important ? LV_INFO : LV_DEBUG, "%s: %s%s%s", b->name, k_modes[m], extra ? " - " : "", extra ? extra : "");
}

/* milestone-1 pattern; each bot is phase-shifted so the identification below can tell bots apart */
static void test_pattern(BotBrain *b, uint64_t t, PadOut *o, int attack) {
    uint64_t shift = (uint64_t)C.walkMs * 2 * (uint64_t)b->index / (uint64_t)(b->nbots > 0 ? b->nbots : 1);
    int goingLeft = (((t + shift) / C.walkMs) % 2) == 0;
    o->axis[AX_LX] = goingLeft ? -26000 : 26000;
    if (!attack) return;
    uint64_t phase = t % C.attackIntervalMs; int n = (int)(t / C.attackIntervalMs);
    if (phase < 90) {
        if (C.jumpEvery > 0 && n % C.jumpEvery == C.jumpEvery - 1) o->btn[BTN_A] = 1;
        else o->btn[C.testAttackButton] = 1;
    }
    if (n != b->attacks) { b->attacks = n; LOGD("%s tick: x=%d attack#%d", b->name, o->axis[AX_LX], n); }
}

/* ---------------- identity: controller -> player slot -> player id -> fighter ----------------
 * The game's publicPlayerSlotArray says which controllerId plays P1..P4. Our pad's controllerId is
 * learned deterministically: when our pad presses join, the controllerId that newly appears in a slot
 * is ours (controllerIds that were already in a slot before our pad was plugged in are humans and can
 * never be ours). If no join was seen, the device-order guess is used, but only if it wasn't a human's.
 * The fighter is then the Character whose playerId equals our slot index, whatever its uid (uids
 * change when a fighter dies and respawns). A short left/right stick wiggle is only a cross-check:
 * if the slot's fighter doesn't follow our stick, we stay neutral and never act on a guess. */
void brain_attached(BotBrain *b, int priorFna, int priorSdl, const World *w, uint64_t now) {
    memset(&b->id, 0, sizeof b->id);
    b->id.ctrlId = -1; b->id.verifiedSlot = -1; b->id.ckSlot = -1; b->playerSlot = -1;
    b->id.priorFna = priorFna; b->id.priorSdl = priorSdl; b->id.attachAt = now; b->id.lastSeen = now;
    if (w && w->nslot > 0) {
        b->id.preKnown = 1;
        for (int i = 0; i < w->nslot && i < 8; i++) if (w->slot[i].ctrl >= 0) b->id.pre[b->id.npre++] = w->slot[i].ctrl;
        for (int i = 0; i < w->nslot && i < 8; i++) b->id.last[i] = w->slot[i].ctrl;
        b->id.nlast = w->nslot < 8 ? w->nslot : 8;
    }
    char pre[64] = ""; int k = 0;
    for (int i = 0; i < b->id.npre; i++) k += snprintf(pre + k, sizeof pre - k, "%s%d", i ? "," : "", b->id.pre[i]);
    LOGI("%s: plugged in; device-order guesses: controller %d (FNA slot) / %d (SDL index). Controllers already playing (humans): %s",
         b->name, priorFna, priorSdl, !b->id.preKnown ? "unknown (slots not readable yet)" : b->id.npre ? pre : "none");
}
static int in_list(const int *l, int n, int v) { for (int i = 0; i < n; i++) if (l[i] == v) return 1; return 0; }
int brain_ctrl_is_bot(const BotBrain *all, int nall, int ctrlId) {
    for (int i = 0; i < nall; i++) if (ctrlId >= 0 && all[i].id.ctrlId == ctrlId) return 1;
    return 0;
}
/* m2.4: a join attempt starts only when the safety gate allows a press (b->joinAt), not at F7 */
static uint64_t join_len_n(int taps) { return (uint64_t)taps * 600; }
static uint64_t join_len(void) { return join_len_n(C.joinPresses); }
static uint64_t bjoin_len(const BotBrain *b) { return join_len_n(b->joinTaps > 0 ? b->joinTaps : C.joinPresses); }
static int in_join_window(const BotBrain *b, uint64_t now) {
    return b->id.ctrlId < 0 && b->joinAt && now >= b->joinAt && now < b->joinAt + bjoin_len(b) + 1500;
}
/* m3: one bot joins at a time: wait while another bot is in its join window, or a lower-numbered bot
   that also needs to join hasn't had its turn yet */
static int join_turn(const BotBrain *b, const BotBrain *all, int nall, uint64_t now) {
    for (int k = 0; k < nall; k++) {
        const BotBrain *o = &all[k];
        if (o == b || o->id.ctrlId >= 0 || !o->id.attachAt) continue;
        if (in_join_window(o, now) || (o->joinAt && now < o->joinAt)) return 0;
        if (o->index < b->index && !o->joinAt && o->activeSince && now - o->activeSince < 4000) return 0;
    }
    return 1;
}
static void set_ctrl(BotBrain *b, int ctrl, int slot, const char *how) {
    b->id.ctrlId = ctrl; b->id.verifiedSlot = -1; b->id.ckSlot = -1; b->id.ckFails = 0;
    snprintf(b->id.how, sizeof b->id.how, "%s", how);
    LOGI("%s: our controllerId is %d (%s); it plays P%d. Device-order guesses were %d/%d%s.", b->name, ctrl, how, slot + 1,
         b->id.priorFna, b->id.priorSdl, ctrl == b->id.priorSdl || ctrl == b->id.priorFna ? " (agrees)" : " (differs; trusting what the game shows)");
}
static void ident_update(BotBrain *b, BotBrain *all, int nall, const World *w, uint64_t now) {
    BotIdent *id = &b->id;
    if (w->nslot <= 0) return;
    if (!id->preKnown) {           /* first readable slots after attach: what's there now counts as human */
        id->preKnown = 1;
        for (int i = 0; i < w->nslot && i < 8; i++) if (w->slot[i].ctrl >= 0 && !brain_ctrl_is_bot(all, nall, w->slot[i].ctrl)) id->pre[id->npre++] = w->slot[i].ctrl;
        LOGI("%s: player slots readable now; %d controller(s) already playing are treated as human.", b->name, id->npre);
    }
    /* we weren't watching (bots off, or slots unreadable for a while): whatever is in the slots now
       joined while we weren't pressing anything, so it is human. Only changes seen frame to frame count. */
    if (id->lastSeen && now - id->lastSeen > 250) {
        int added = 0;
        for (int i = 0; i < w->nslot && i < 8; i++) {
            int c = w->slot[i].ctrl;
            if (c >= 0 && c != id->ctrlId && !brain_ctrl_is_bot(all, nall, c) && !in_list(id->pre, id->npre, c) && id->npre < 8) { id->pre[id->npre++] = c; added++; }
        }
        if (added) LOGI("%s: %d controller(s) joined while the bot wasn't watching; treated as human.", b->name, added);
        id->nlast = w->nslot < 8 ? w->nslot : 8;
        for (int i = 0; i < id->nlast; i++) id->last[i] = w->slot[i].ctrl;
    }
    id->lastSeen = now;
    /* 1. join observed: a controllerId appears in a slot while only this bot is pressing join */
    if (id->ctrlId < 0) {
        int others = 0;
        for (int k = 0; k < nall; k++) if (&all[k] != b && in_join_window(&all[k], now)) others++;
        for (int i = 0; i < w->nslot && i < 8; i++) {
            int cur = w->slot[i].ctrl, prev = i < id->nlast ? id->last[i] : -1;
            if (cur < 0 || cur == prev || in_list(id->pre, id->npre, cur) || in_list(id->rejected, id->nrej, cur) || brain_ctrl_is_bot(all, nall, cur)) continue;
            if (!in_join_window(b, now)) {
                LOGI("%s: controller %d joined as P%d, but not while we were pressing join: treating it as a human.", b->name, cur, i + 1);
                if (id->npre < 8) id->pre[id->npre++] = cur;
                continue;
            }
            if (others) { LOGI("%s: controller %d joined as P%d while another bot was also joining; ambiguous, waiting.", b->name, cur, i + 1); continue; }
            char how[64]; snprintf(how, sizeof how, "seen joining as P%d when our pad pressed join", i + 1);
            set_ctrl(b, cur, i, how);
            break;
        }
    }
    /* 2. device order, only when it can't be a human's controller */
    if (id->ctrlId < 0 && now > (b->joinAt ? b->joinAt : b->activeSince) + bjoin_len(b) + 1500) {
        int cands[2] = { id->priorSdl, id->priorFna };
        for (int c = 0; c < 2 && id->ctrlId < 0; c++) {
            int v = cands[c];
            if (c == 1 && v == cands[0]) continue;
            if (v < 0 || in_list(id->rejected, id->nrej, v) || brain_ctrl_is_bot(all, nall, v)) continue;
            int slot = -1; for (int i = 0; i < w->nslot; i++) if (w->slot[i].ctrl == v) { slot = i; break; }
            if (slot < 0) continue;
            if (in_list(id->pre, id->npre, v)) {
                if (!(id->loggedPre & (1 << c))) { id->loggedPre |= 1 << c; LOGI("%s: controller %d (device-order guess) was already P%d before our pad existed: a human. Not using it.", b->name, v, slot + 1); }
                continue;
            }
            char how[64]; snprintf(how, sizeof how, "device order (%s); checked silently", c == 0 ? "SDL index" : "FNA slot");
            set_ctrl(b, v, slot, how);
        }
    }
    id->nlast = w->nslot < 8 ? w->nslot : 8;
    for (int i = 0; i < id->nlast; i++) id->last[i] = w->slot[i].ctrl;
}
/* the fighter of player slot s: newest living Character with playerId == s */
static const Chr *slot_fighter(const World *w, int s) {
    const Chr *best = NULL;
    for (int i = 0; i < w->nch; i++) {
        const Chr *c = &w->ch[i];
        if (c->player != s || c->state == CS_DISAPEAR) continue;
        int alive = c->life > 0 && c->state != CS_STANDDIE;
        int balive = best && best->life > 0 && best->state != CS_STANDDIE;
        if (!best || (alive && !balive) || (alive == balive && c->uid > best->uid)) best = c;
    }
    return best;
}
static float auto_or(float v, float autoV) { return v > 0 ? v : autoV; }
/* m6.2: walking limits for this think: inside the camera view with a margin (enemies spawn at the screen
   edges and scripted exits sit there), and with no enemy engaged not ahead of P1 */
static float s_walkLo = -1e30f, s_walkHi = 1e30f;
float dbg_walkLo, dbg_walkHi;   /* test visibility */
/* m6.6: while an enemy is up in a jump slam, its ground x/z (the shadow) is where it lands: no walk ends in there */
static int s_slamOn; static float s_slamX, s_slamZ, s_slamR;
/* m6.7: floor traps. A HazardInstance / DamageAreaInstance in physicalObjectList is read and logged, but which
   animation of it hurts is not known up front; and the m6.6-run1 shocks (11-13 life, no enemy near) came from
   objects that build did not read at all. So: a bot hurt with no enemy, attack or projectile anywhere near marks
   the spot. Next to a hazard object the zone covers the hazard and the spot and is live while that hazard plays
   the animation it had when it hurt us (30 s); with no hazard object it is a plain remembered spot (20 s). Walk
   targets are moved out of live zones, a bot standing in one steps out, and next to a live hazard zone the bot
   waits at its edge (up to 4 s) instead of walking across. */
static float mnf(float a, float b) { return a < b ? a : b; }
static float mxf(float a, float b) { return a > b ? a : b; }
typedef struct { float x0, x1, z0, z1; uint64_t until; uint32_t hazUid; int hotAnim; uint32_t brkUid, partUid; } TrapZone;   /* m7.4: partUid = a lifted robot leg */   /* m7.2: brkUid = a lit barrel */
#define MAXTZ 8
static TrapZone s_tz[MAXTZ]; static int s_ntz, s_tzScene = -1; static const World *s_w;
int dbg_trap_zone_count(void) { return s_ntz; }
static int s_nbar;
void dbg_trap_reset(void) { s_ntz = 0; s_tzScene = -1; s_nbar = 0; }
static int tz_hazard_live(const TrapZone *z) {          /* -1 no hazard object, 0 hazard idle, 1 hazard hot */
    if (!z->hazUid || !s_w) return -1;
    for (int i = 0; i < s_w->nhz; i++) if (s_w->hz[i].uid == z->hazUid) return s_w->hz[i].anim == z->hotAnim;
    return -1;
}
static int tz_active(const TrapZone *z, uint64_t now) { return now < z->until && tz_hazard_live(z) != 0; }
static const TrapZone *tz_at(float x, float z, float m, uint64_t now) {
    for (int i = 0; i < s_ntz; i++) { const TrapZone *t = &s_tz[i];
        if (tz_active(t, now) && x > t->x0 - m && x < t->x1 + m && z > t->z0 - m && z < t->z1 + m) return t; }
    return NULL;
}
/* nearest point just outside zone t from (x, z); depth exits cost a little more (walls) */
static void tz_exit(const TrapZone *t, float x, float z, float m, float *ox, float *oz) {
    float c[4][2] = { { t->x0 - m, z }, { t->x1 + m, z }, { x, t->z0 - m }, { x, t->z1 + m } }, best = 1e30f;
    for (int k = 0; k < 4; k++) { float d = fabsf_(c[k][0] - x) + fabsf_(c[k][1] - z) + (k >= 2 ? (t->brkUid ? 1.8f : 0.3f) : 0.0f);   /* m7.2: blast areas: leave sideways (walls) */
        if (k < 2 && (c[k][0] < s_walkLo || c[k][0] > s_walkHi)) d += 50.0f;
        if (d < best) { best = d; *ox = c[k][0]; *oz = c[k][1]; } }
}
/* m7.4: the walkable depth. Observed: the z range grounded players (bots and humans) stand on in this scene.
   Learned walls: a bot pushing in depth at the observed edge without moving for 0.45 s (m7.3-run2: bots pinned
   at z 1.2 for 56 snapshots, walking at the robot's seated driver at z -0.2). */
typedef struct { int known, n, scene, frames; float lo, hi; int wLo, wHi; float wLoZ, wLoX, wHiZ, wHiX; } Floor;
static Floor s_fl = { 0, 0, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
int g_brainLearnWalls = 1;
void brain_floor_reset(void) { memset(&s_fl, 0, sizeof s_fl); s_fl.scene = -1; }
int brain_floor(float *lo, float *hi) { if (lo) *lo = s_fl.lo; if (hi) *hi = s_fl.hi; return s_fl.known; }
static void floor_update(const World *w) {
    if (w->scene != s_fl.scene || w->frames + 600 < s_fl.frames) { memset(&s_fl, 0, sizeof s_fl); s_fl.scene = w->scene; }
    s_fl.frames = w->frames;
    for (int i = 0; i < w->nch; i++) { const Chr *c = &w->ch[i];
        if (c->player < 0 || c->team != 0 || c->scripted || c->life <= 0 || c->pos.y > 0.05f || state_is_hurt(c->state)) continue;
        if (!s_fl.n) { s_fl.lo = s_fl.hi = c->pos.z; } else { if (c->pos.z < s_fl.lo) s_fl.lo = c->pos.z; if (c->pos.z > s_fl.hi) s_fl.hi = c->pos.z; }
        if (s_fl.n < 1000000) s_fl.n++;
    }
    s_fl.known = s_fl.n >= 10;
}
static uint64_t g_nowMs;
static void wall_learn(BotBrain *b, const Chr *me, int dir) {
    static struct { int dir; float z; uint64_t at; } st[8]; int k = b->index & 7;
    if (!dir || !g_brainLearnWalls || state_is_hurt(me->state) || me->pos.y > 0.05f || me->scripted) { st[k].dir = 0; return; }
    if (st[k].dir != dir || fabsf_(me->pos.z - st[k].z) > 0.03f || g_nowMs < st[k].at || g_nowMs - st[k].at > 3000) { st[k].dir = dir; st[k].z = me->pos.z; st[k].at = g_nowMs; return; }
    if (g_nowMs - st[k].at < 450 || !s_fl.known) return;
    if (dir < 0 && me->pos.z <= s_fl.lo + 0.15f && (!s_fl.wLo || fabsf_(s_fl.wLoZ - me->pos.z) > 0.1f || fabsf_(s_fl.wLoX - me->pos.x) > 4.0f)) {
        s_fl.wLo = 1; s_fl.wLoZ = me->pos.z; s_fl.wLoX = me->pos.x; LOGI("%s: floor edge: can't walk below z %.2f at x %.1f (pushed 0.45 s without moving)", b->name, me->pos.z, me->pos.x); }
    if (dir > 0 && me->pos.z >= s_fl.hi - 0.15f && (!s_fl.wHi || fabsf_(s_fl.wHiZ - me->pos.z) > 0.1f || fabsf_(s_fl.wHiX - me->pos.x) > 4.0f)) {
        s_fl.wHi = 1; s_fl.wHiZ = me->pos.z; s_fl.wHiX = me->pos.x; LOGI("%s: floor edge: can't walk above z %.2f at x %.1f (pushed 0.45 s without moving)", b->name, me->pos.z, me->pos.x); }
}
static void walk_to(BotBrain *b, const Chr *me, float tx, float tz, float tolX, float tolZ, PadOut *o) {
    dbg_walkLo = s_walkLo; dbg_walkHi = s_walkHi;
    if (s_ntz) {                                         /* m6.7: trap zones */
        const TrapZone *t = tz_at(tx, tz, 0.25f, g_nowMs);
        if (t) tz_exit(t, tx, tz, 0.35f, &tx, &tz);
        /* m6.7: the straight line to the target runs through a live zone: around it in depth (above/below it: keep
           out of its depth band while passing; level with it: step out of the band first). Next to a live hazard
           object: wait at the edge instead. Both give up after 4 s (a zone spanning the whole floor depth). */
        int waited = 0;
        if (!tz_at(me->pos.x, me->pos.z, 0.05f, g_nowMs)) for (int i = 0; i < s_ntz; i++) { const TrapZone *q = &s_tz[i];
            if (!tz_active(q, g_nowMs)) continue;
            const float m = 0.3f; int hit = 0;
            for (int k = 1; k <= 16 && !hit; k++) { float px = me->pos.x + (tx - me->pos.x) * k / 16.0f, pz = me->pos.z + (tz - me->pos.z) * k / 16.0f;
                hit = px > q->x0 - m && px < q->x1 + m && pz > q->z0 - m && pz < q->z1 + m; }
            if (!hit) continue;
            if (!b->tzWaitSince) b->tzWaitSince = g_nowMs;
            if (g_nowMs - b->tzWaitSince >= 4000) continue;
            waited = 1;
            int bandZ = me->pos.z > q->z0 - m && me->pos.z < q->z1 + m;
            if (tz_hazard_live(q) == 1 && bandZ) { tx = me->pos.x <= q->x0 ? q->x0 - 0.35f : q->x1 + 0.35f; tz = me->pos.z; continue; }
            float zUp = q->z0 - m - 0.1f, zDn = q->z1 + m + 0.1f;
            if (!bandZ) tz = me->pos.z <= q->z0 ? zUp : zDn;                       /* pass above / below it */
            else { tz = fabsf_(me->pos.z - zUp) + fabsf_(tz - zUp) <= fabsf_(me->pos.z - zDn) + fabsf_(tz - zDn) ? zUp : zDn; tx = me->pos.x; }
        }
        if (!waited) b->tzWaitSince = 0; else if (tolZ > 0.05f) tolZ = 0.05f;   /* detours must really leave the band */
    }
    if (s_slamOn) {
        float ddx = tx - s_slamX, ddz = tz - s_slamZ;
        if (ddx * ddx + ddz * ddz < s_slamR * s_slamR) { int sd = me->pos.x >= s_slamX ? 1 : -1; tx = s_slamX + sd * s_slamR; }
    }
    if (tx < s_walkLo) tx = s_walkLo;
    if (tx > s_walkHi) tx = s_walkHi;
    float dx = tx - me->pos.x, dz = tz - me->pos.z;
    if (fabsf_(dx) > tolX) {
        /* m6.7 (m6.6-run1 back-and-forth): a walk that reverses within 200 ms of the last reversal waits instead */
        int d = sgn(dx);
        if (b->wlx && d != b->wlx && g_nowMs - b->wlxAt < 200) d = 0;
        else if (d != b->wlx) { b->wlx = d; b->wlxAt = g_nowMs; }
        if (d) o->axis[AX_LX] = (int16_t)(d * b->xSign * STICK);
    }
    if (fabsf_(dz) > tolZ) {
        int16_t ly = (int16_t)(sgn(dz) * b->zSign * STICK);
        o->axis[AX_LY] = ly;
        wall_learn(b, me, sgn(dz));
    } else wall_learn(b, me, 0);
}
/* learn which stick-y direction increases z (screen depth) from actual movement */
/* m6.6: an enemy up in a jump attack. Logs (stage 1/2 sub-boss uid 96/97/102/116 in m5.1-run1, m6-run1b, m6.2-run1c,
   m6.5-run1): state AirMove, move 3/4, aiAttack 1, y 2.8-15.9 (life 213-336), x/z fixed while up (the shadow);
   ordinary enemy hops are state Jump with y <= 2.3, move -1, aiAttack 0. */
static int slam_air(const Chr *c) {
    if (c->life <= 0 || state_is_hurt(c->state)) return 0;
    if (c->state == CS_AIRMOVE && c->aiAttacking && c->pos.y >= 1.5f) return 1;
    if ((c->state == CS_JUMP || c->state == CS_AIRMOVE) && c->pos.y >= 3.0f) return 1;
    return 0;
}
static void calibrate(BotBrain *b, const Chr *me) {
    float dz = me->pos.z - b->lastZ;
    float mx = me->pos.x - b->lastX;
    /* m6.5 (m6.4-run1b): sliding along the slanted back wall while walking sideways moved z against the stick and
       flipped zSign back and forth: only count clear depth moves, and a re-flip within 20 s needs 30 votes */
    if (b->lastLy && fabsf_(dz) > 0.008f && fabsf_(mx) < 2.0f * fabsf_(dz) && !state_is_hurt(me->state)) {
        int vote = sgn(dz) * sgn((float)b->lastLy);         /* +1: our current zSign is right */
        b->zVotes += vote * b->zSign;
        if (b->zVotes <= ((b->zFlipAt && g_nowMs - b->zFlipAt < 20000) ? -30 : -8)) { b->zSign = -b->zSign; b->zVotes = 0; b->zFlipAt = g_nowMs ? g_nowMs : 1; LOGI("%s: stick up/down was reversed; corrected (zSign %+d)", b->name, b->zSign); }
        if (b->zVotes > 30) b->zVotes = 30;
    }
    b->lastX = me->pos.x; b->lastZ = me->pos.z;
}

/* ---------------- m3: character select with the bot's own pad ----------------
 * Only runs on PROVEN character select (w->charSel: globalState.currentMenu is CharacterSelection and the
 * top menu screen is CharacterSelectionScreen, nothing paused). Every press is short (90 ms) and followed by
 * a wait; the next step is chosen from what the game shows after the wait (cursor = our menuEntryId, color
 * = our playablePalette, confirmed = innerPlayableIndex > 0). Budgets: 40 moves, 2 x colors + 2 color
 * presses, 1 confirm tap per character (2 characters at most), 2 special-moves confirm taps. B, Start and Back are never pressed.
 * A is never pressed once we are confirmed and the special-moves menu is done (that A would launch the
 * game). Returns 1 while it wants to output something this frame. */
enum { CSB_NONE, CSB_UP, CSB_DOWN, CSB_LEFT, CSB_RIGHT, CSB_RB, CSB_A, CSB_B, CSB_DUP, CSB_DDOWN, CSB_DLEFT, CSB_DRIGHT };
static const char *const k_csb[] = { "-", "up", "down", "left", "right", "RB (next color)", "A", "B (back)", "d-pad up", "d-pad down", "d-pad left", "d-pad right" };
static int csb_dir(int what) { return what >= CSB_UP && what <= CSB_RIGHT ? what - CSB_UP : what >= CSB_DUP && what <= CSB_DRIGHT ? what - CSB_DUP : -1; }
/* m7.1: press time: moves 120 ms (stick held well past the game's dead zone, then fully released for the settle wait) */
static uint64_t csb_hold(int what) { return csb_dir(what) >= 0 ? 120 : 90; }
static void cs_out(int what, PadOut *o) {
    switch (what) {
    case CSB_UP: o->axis[AX_LY] = -STICK; break;
    case CSB_DOWN: o->axis[AX_LY] = STICK; break;
    case CSB_LEFT: o->axis[AX_LX] = -STICK; break;
    case CSB_RIGHT: o->axis[AX_LX] = STICK; break;
    case CSB_RB: o->btn[BTN_RB] = 1; break;
    case CSB_A: o->btn[BTN_A] = 1; break;
    case CSB_B: o->btn[BTN_B] = 1; break;
    case CSB_DUP: o->btn[BTN_UP] = 1; break;
    case CSB_DDOWN: o->btn[BTN_DOWN] = 1; break;
    case CSB_DLEFT: o->btn[BTN_LEFT] = 1; break;
    case CSB_DRIGHT: o->btn[BTN_RIGHT] = 1; break;
    }
}
static void cs_press(CsDrive *c, int what, int before, uint64_t now) { c->pressing = what; c->before = before; c->pressAt = now; }
static int s_rosterFails[ROSTER_MAX];
static int s_dpadPref[4];                  /* m7.1: the stick did nothing in this direction but the d-pad worked: use the d-pad from now on */
static void cs_moved(int what, int from, int to, const char *name) {
    int d = csb_dir(what); if (d < 0) return;
    state_roster_observe(from, d, to);
    if (what >= CSB_DUP && to != from && !s_dpadPref[d]) { s_dpadPref[d] = 1; LOGI("%s: the d-pad moved the cursor %s where the stick did not; using the d-pad for %s from now on.", name, k_csb[CSB_UP + d], k_csb[CSB_UP + d]); }
}
/* m7.1: one cursor step toward 'to' over the navigation graph (shortest path, any direction). The stick is used;
   after a stick press in that direction did nothing from here, the d-pad is tried. 0 = no path at all. */
static int cs_step(BotBrain *b, CsDrive *c, int cur, int to, uint64_t now, PadOut *o, int *stepsOut) {
    int dir = -1, steps = state_roster_path(cur, to, &dir);
    if (stepsOut) *stepsOut = steps;
    if (steps <= 0 || dir < 0) return 0;
    int what = state_roster_noeffect(cur, dir) >= 1 || s_dpadPref[dir] ? CSB_DUP + dir : CSB_UP + dir;
    c->pulses++; cs_press(c, what, cur, now); cs_out(what, o);
    (void)b;
    return what;
}      /* this session: how often a character would not confirm */
static const char *cs_name(int i) { const RosterEntry *r; int n = state_roster(&r); return i >= 0 && i < n ? r[i].name : "?"; }
static void cs_done(BotBrain *b, const char *why) {
    b->cs.phase = CSP_DONE;
    LOGI("%s: character select done: %s.", b->name, why);
}
/* 1 = output set (outKind charsel), 0 = nothing to press */
static int cs_drive(BotBrain *b, const World *w, uint64_t now, PadOut *o) {
    CsDrive *c = &b->cs; int p = b->playerSlot;
    if (p < 0 || p >= w->ncs || !w->cs[p].haveUser) { WHY("character select: our player data is not readable"); return 0; }
    int cur = w->cs[p].cursor, val = w->cs[p].playable, pal = w->cs[p].palette, alt = w->cs[p].altConfirmed;
    const RosterEntry *r; int nr = state_roster(&r);
    if (c->pressing) {
        uint64_t t = now - c->pressAt, settle = c->pressing == CSB_A ? 650 : 220, hold = csb_hold(c->pressing);
        if (t < hold) { cs_out(c->pressing, o); WHY("character select: pressing %s", k_csb[c->pressing]); return 1; }
        if (t < hold + settle) { WHY("character select: waiting after %s", k_csb[c->pressing]); return 1; }
    }
    int just = c->pressing; c->pressing = CSB_NONE;
    if (csb_dir(just) >= 0) cs_moved(just, c->before, cur, b->name);
    if (!c->startAt) c->startAt = now;
    if (now - c->startAt < 400) { WHY("character select: settling"); return 1; }
    switch (c->phase) {
    case CSP_START: {
        if (val > 0) {
            if (alt == 0) { c->phase = CSP_ALT; LOGI("%s: already confirmed as %s; confirming the special moves.", b->name, cs_name(val - 1)); }
            else { cs_done(b, "already confirmed when we got here (not changing it; B is never pressed)"); }
            return 1;
        }
        c->origEntry = cur; c->target = cur;
        int want = b->wantChar[0] && _stricmp(b->wantChar, "Auto") ? state_roster_find(b->wantChar) : -1;
        if (b->wantChar[0] && _stricmp(b->wantChar, "Auto") && want < 0)
            LOGI("%s: character '%s' is not in the roster%s; keeping the game's pick.", b->name, b->wantChar, nr ? "" : " (roster not read yet)");
        else if (want >= 0 && s_rosterFails[want] >= 2) LOGI("%s: %s would not confirm twice this session (locked, not owned or taken); keeping the game's pick.", b->name, r[want].name);
        else if (want >= 0) c->target = want;
        LOGI("%s: character select as P%d: cursor on %s (#%d), want %s, color %s.", b->name, p + 1, cs_name(cur), cur,
             c->target == cur ? "this one" : cs_name(c->target), b->wantColor > 0 ? "set" : "Default");
        c->phase = CSP_NAV; return 1; }
    case CSP_NAV:
        if (val > 0) { c->phase = alt == 0 ? CSP_ALT : CSP_DONE; if (c->phase == CSP_DONE) cs_done(b, "confirmed (by someone else?)"); return 1; }
        if (csb_dir(just) >= 0) { if (cur == c->before) c->noChange++; else c->noChange = 0; }
        if (cur == c->target) { c->phase = CSP_COLOR; return 1; }
        if (c->noChange >= 6 || c->pulses >= 40) {
            LOGI("%s: could not move the cursor to %s (%d moves, %d without effect); staying on %s.", b->name, cs_name(c->target), c->pulses, c->noChange, cs_name(cur));
            c->target = cur; c->phase = CSP_COLOR; return 1;
        }
        {
            int steps = -1, what = cs_step(b, c, cur, c->target, now, o, &steps);
            if (!what) {                                         /* m7.1: never guess "right": no path = stop here */
                LOGI("%s: no way found from %s to %s on the navigation graph; staying on %s.", b->name, cs_name(cur), cs_name(c->target), cs_name(cur));
                c->target = cur; c->phase = CSP_COLOR; return 1;
            }
            WHY("character select: cursor %s -> %s (%d step(s)), pressing %s", cs_name(cur), cs_name(c->target), steps, k_csb[what]);
        }
        return 1;
    case CSP_COLOR: {
        if (val > 0) { c->phase = alt == 0 ? CSP_ALT : CSP_DONE; if (c->phase == CSP_DONE) cs_done(b, "confirmed"); return 1; }
        int count = cur >= 0 && cur < nr ? r[cur].palettes : 1;
        if (b->wantColor <= 0 || count <= 1) { c->phase = CSP_CONFIRM; return 1; }
        int desired = (b->wantColor - 1) % count;
        if (just == CSB_RB) { if (pal == c->before) c->colorNoChange++; else c->colorNoChange = 0; }
        if (pal == desired) { if (c->colorPresses) LOGI("%s: color %d of %d set.", b->name, desired + 1, count); c->phase = CSP_CONFIRM; return 1; }
        if (c->colorNoChange >= 3 || c->colorPresses >= 2 * count + 2) {
            LOGI("%s: could not set color %d on %s (now %d; %d presses); it may be in use by another player. Keeping color %d.", b->name, desired + 1, cs_name(cur), pal + 1, c->colorPresses, pal + 1);
            c->phase = CSP_CONFIRM; return 1;
        }
        c->colorPresses++; cs_press(c, CSB_RB, pal, now); cs_out(CSB_RB, o);
        WHY("character select: color %d -> %d of %d, pressing RB", pal + 1, desired + 1, count);
        return 1; }
    case CSP_CONFIRM:
        if (val > 0) {
            LOGI("%s: confirmed %s, color %d.", b->name, cs_name(val - 1), pal + 1);
            if (val - 1 >= 0 && val - 1 < ROSTER_MAX) s_rosterFails[val - 1] = 0;
            c->phase = alt == 0 ? CSP_ALT : CSP_DONE; if (c->phase == CSP_DONE) cs_done(b, "confirmed"); return 1;
        }
        if (just == CSB_A) c->taps++;
        /* one tap per character: on a character that can't be picked, the game treats a second A as
           "start the game" (refused while we're unconfirmed, but never go there) */
        if (c->taps >= 1) {
            if (cur >= 0 && cur < ROSTER_MAX) { c->unavail |= 1ull << cur; s_rosterFails[cur]++; }
            LOGI("%s: %s would not confirm (locked, DLC not owned, or taken by another player).", b->name, cs_name(cur));
            if (!c->triedOrig && c->origEntry != cur && c->origEntry >= 0 && !(c->unavail >> c->origEntry & 1)) {
                c->triedOrig = 1; c->target = c->origEntry; c->taps = 0; c->noChange = 0; c->phase = CSP_NAV;
                LOGI("%s: going back to %s instead.", b->name, cs_name(c->origEntry));
            } else cs_done(b, "nothing confirmed; the human can pick for this bot or turn it off");
            return 1;
        }
        cs_press(c, CSB_A, val, now); cs_out(CSB_A, o);
        WHY("character select: confirming %s", cs_name(cur));
        LOGI("%s: confirm tap (A) on %s, color %d.", b->name, cs_name(cur), pal + 1);
        return 1;
    case CSP_ALT:
        if (val <= 0) { cs_done(b, "no longer confirmed"); return 1; }
        if (alt != 0) { cs_done(b, alt > 0 ? "special moves confirmed" : "special-moves state unknown; not pressing A again"); return 1; }
        if (just == CSB_A) c->altTaps++;
        if (c->altTaps >= 2) { cs_done(b, "special-moves menu still open after 2 taps; stopping"); return 1; }
        cs_press(c, CSB_A, alt, now); cs_out(CSB_A, o);
        WHY("character select: confirming the special moves (tap %d of 2)", c->altTaps + 1);
        LOGI("%s: special-moves confirm tap %d (A).", b->name, c->altTaps + 1);
        return 1;
    default:
        WHY("character select: done (%s)", val > 0 ? "confirmed" : "not confirmed");
        return 0;
    }
}

/* ======================= m7: live character select =======================
 * While the F7 menu is open at a PROVEN character select, a bot that is ON follows the menu live:
 *   - it joins at once (A from its free pad, the game's own join rule = first FREE slot);
 *   - its cursor walks over the roster's menu links to the Character picked in the menu (one stick
 *     pulse, then the next step is chosen from the cursor the game reports, so a missed or extra step is
 *     corrected from where the cursor really is);
 *   - Color is set with RB (the game's own "next color" while not confirmed), only if no other player on
 *     the same character shows that color (the game skips colors in use);
 *   - it never confirms (A) while the menu is open: confirming happens after the menu closes (cs_drive);
 *   - if it is already confirmed and the menu asks for something else, it presses B once to un-confirm
 *     (at most 2 tries per change); if that B made it leave its slot instead, it taps A to join again
 *     (at most 3 times this visit).
 * Turning a bot OFF at character select: the host sets b->backout; the bot presses B (confirmed -> not
 * confirmed -> leaves the slot) at most 3 times, never as P1, then the host unplugs the pad. */
static int cs_wait_press(BotBrain *b, CsDrive *c, uint64_t now, PadOut *o) {
    if (!c->pressing) return 0;
    uint64_t t = now - c->pressAt, settle = c->pressing == CSB_A || c->pressing == CSB_B ? 650 : 220, hold = csb_hold(c->pressing);
    if (t < hold) { cs_out(c->pressing, o); WHY("character select: pressing %s", k_csb[c->pressing]); return 1; }
    if (t < hold + settle) { WHY("character select: waiting after %s", k_csb[c->pressing]); return 1; }
    return 0;
}
/* another player (not slot p) on roster entry ch showing color pal: its player number (1-based), else 0 */
static int cs_color_taken(const World *w, int p, int ch, int pal) {
    for (int q = 0; q < w->ncs && q < w->nslot; q++) {
        if (q == p || !w->cs[q].haveUser || w->slot[q].ctrl < 0) continue;
        int qch = w->cs[q].playable > 0 ? w->cs[q].playable - 1 : w->cs[q].cursor;
        if (qch == ch && w->cs[q].palette == pal) return q + 1;
    }
    return 0;
}
/* returns: 0 nothing pressed, 3 stick/RB (outKind 3), 4 B (outKind 4) */
static int cs_live(BotBrain *b, const World *w, uint64_t now, PadOut *o) {
    CsDrive *c = &b->cs; int p = b->playerSlot;
    if (p < 0 || p >= w->ncs || !w->cs[p].haveUser) { snprintf(b->liveStatus, sizeof b->liveStatus, "joined; player data not readable"); return 0; }
    int cur = w->cs[p].cursor, val = w->cs[p].playable, pal = w->cs[p].palette;
    const RosterEntry *r; int nr = state_roster(&r);
    if (c->pressing) {
        int what = c->pressing;
        if (cs_wait_press(b, c, now, o)) return what == CSB_B ? 4 : 3;
        c->pressing = CSB_NONE;
        if (what == CSB_B) LOGI("%s: live: after B as P%d: confirmed %s -> %s, cursor %s.", b->name, p + 1, c->before > 0 ? cs_name(c->before - 1) : "no",
                                val > 0 ? cs_name(val - 1) : "no", cs_name(cur));
        if (csb_dir(what) >= 0) { cs_moved(what, c->before, cur, b->name); if (cur == c->before) c->noChange++; else c->noChange = 0; }
        if (what == CSB_RB) { if (pal == c->before) c->colorNoChange++; else c->colorNoChange = 0; }
    }
    if (!c->startAt) c->startAt = now;
    if (now - c->startAt < 300) { WHY("character select (menu open): settling"); return 3; }
    if (nr <= 0 || cur < 0 || cur >= nr) {
        snprintf(b->liveStatus, sizeof b->liveStatus, "cursor %d not in the roster (%d read)", cur, nr);
        if (!(c->liveLogged & 1)) { c->liveLogged |= 1; LOGI("%s: live: our cursor reads %d but the roster has %d entries; not moving it.", b->name, cur, nr); }
        return 0;
    }
    int want = b->wantChar[0] && _stricmp(b->wantChar, "Auto") ? state_roster_find(b->wantChar) : -1;
    if (want != c->liveTarget || b->wantColor != c->liveColor) {       /* the menu changed: fresh budgets */
        if (c->liveLogged & 2) LOGI("%s: live: menu now wants %s, color %s (cursor on %s, color %d).", b->name, want >= 0 ? r[want].name : "Auto",
                                    b->wantColor > 0 ? "set" : "Default", r[cur].name, pal + 1);
        c->liveLogged |= 2;
        c->liveTarget = want; c->liveColor = b->wantColor;
        c->pulses = c->noChange = c->navRounds = c->colorPresses = c->colorNoChange = c->unconfTries = 0; c->retryAt = 0;
    }
    int ch = val > 0 ? val - 1 : cur, count = r[ch].palettes > 0 ? r[ch].palettes : 1;
    int desired = b->wantColor > 0 ? (b->wantColor - 1) % count : -1;
    if (val > 0) {
        int diff = (want >= 0 && want != val - 1) || (desired >= 0 && desired != pal);
        if (!diff) { snprintf(b->liveStatus, sizeof b->liveStatus, "confirmed %s, color %d", r[ch].name, pal + 1); WHY("character select (menu open): confirmed, matches the menu"); return 0; }
        if (p == 0) { snprintf(b->liveStatus, sizeof b->liveStatus, "confirmed %s (P1: not changing)", r[ch].name); return 0; }
        if (c->unconfTries >= 2) { snprintf(b->liveStatus, sizeof b->liveStatus, "confirmed %s; B did not un-confirm", r[ch].name); return 0; }
        c->unconfTries++;
        LOGI("%s: live: confirmed %s color %d but the menu wants %s color %s: pressing B to un-confirm (try %d of 2).", b->name, r[ch].name, pal + 1,
             want >= 0 ? r[want].name : r[ch].name, desired >= 0 ? "set" : "any", c->unconfTries);
        snprintf(b->liveStatus, sizeof b->liveStatus, "un-confirming %s (B)", r[ch].name);
        cs_press(c, CSB_B, val, now); cs_out(CSB_B, o); return 4;
    }
    c->unconfTries = 0;
    if (want >= 0 && cur != want) {
        if (now < c->retryAt) { snprintf(b->liveStatus, sizeof b->liveStatus, "on %s; cursor stuck, retrying", r[cur].name); return 0; }
        if (c->noChange >= 6 || c->pulses >= 40) {
            c->navRounds++;
            LOGI("%s: live: cursor did not reach %s (%d moves, %d without effect; now on %s); %s.", b->name, r[want].name, c->pulses, c->noChange, r[cur].name,
                 c->navRounds < 3 ? "re-planning from where it is in 1 s" : "giving up until the menu pick changes");
            c->pulses = c->noChange = 0;
            if (c->navRounds >= 3) { c->retryAt = UINT64_MAX; snprintf(b->liveStatus, sizeof b->liveStatus, "on %s; can't reach %s", r[cur].name, r[want].name); return 0; }
            c->retryAt = now + 1000; return 0;
        }
        int steps = -1, what = cs_step(b, c, cur, want, now, o, &steps);
        if (!what) {                                             /* m7.1: no path: say so, never loop right */
            if (!(c->liveLogged & 4)) { c->liveLogged |= 4; LOGI("%s: live: no way from %s to %s on the navigation graph (all moves from here tried?); waiting.", b->name, r[cur].name, r[want].name); }
            snprintf(b->liveStatus, sizeof b->liveStatus, "on %s; no way to %s", r[cur].name, r[want].name);
            return 0;
        }
        snprintf(b->liveStatus, sizeof b->liveStatus, "cursor %s -> %s", r[cur].name, r[want].name);
        WHY("character select (menu open): cursor %s -> %s (%d step(s)), pressing %s", r[cur].name, r[want].name, steps, k_csb[what]);
        return 3;
    }
    if (desired >= 0 && pal != desired) {
        int by = cs_color_taken(w, p, cur, desired);
        if (by) { snprintf(b->liveStatus, sizeof b->liveStatus, "on %s, color %d (color %d is P%d's)", r[cur].name, pal + 1, desired + 1, by); return 0; }
        if (c->colorNoChange >= 3 || c->colorPresses >= 2 * count + 2) {
            snprintf(b->liveStatus, sizeof b->liveStatus, "on %s, color %d (can't set %d)", r[cur].name, pal + 1, desired + 1);
            if (!(c->liveLogged & 8)) { c->liveLogged |= 8; LOGI("%s: live: could not set color %d on %s (now %d, %d RB presses).", b->name, desired + 1, r[cur].name, pal + 1, c->colorPresses); }
            return 0;
        }
        c->colorPresses++; cs_press(c, CSB_RB, pal, now); cs_out(CSB_RB, o);
        snprintf(b->liveStatus, sizeof b->liveStatus, "on %s, color %d -> %d", r[cur].name, pal + 1, desired + 1);
        WHY("character select (menu open): color %d -> %d of %d, pressing RB", pal + 1, desired + 1, count);
        return 3;
    }
    snprintf(b->liveStatus, sizeof b->liveStatus, "on %s, color %d%s", r[cur].name, pal + 1, want < 0 ? " (Auto)" : "");
    WHY("character select (menu open): on the menu's pick, waiting (confirms when the menu closes)");
    return 0;
}
/* bot turned OFF at character select: B until our slot is free. 4 = pressing B, 0 = nothing (b->backoutDone set when finished) */
static int cs_backout(BotBrain *b, const World *w, int proven, int charSel, uint64_t now, PadOut *o) {
    CsDrive *c = &b->cs; int p = b->playerSlot;
    if (!b->boSince) { b->boSince = now; b->boPresses = 0; c->pressing = 0; LOGI("%s: turned off at character select: backing out of P%d with B.", b->name, p + 1); }
#define BO_DONE(...) do { snprintf(b->boWhy, sizeof b->boWhy, __VA_ARGS__); b->backoutDone = 1; LOGI("%s: back-out done: %s.", b->name, b->boWhy); return 0; } while (0)
    if (b->backoutDone) return 0;
    if (c->pressing) {
        if (cs_wait_press(b, c, now, o)) return 4;
        c->pressing = 0;
        LOGI("%s: back-out: after B %d: %s.", b->name, b->boPresses, p < 0 ? "our controller left its slot" :
             w->cs[p].playable > 0 ? "still confirmed" : "in the slot, not confirmed");
    }
    if (!proven || !charSel) BO_DONE("not at a proven character select any more");
    if (b->id.ctrlId < 0 || p < 0) BO_DONE("our controller is not in a player slot (slot free)");
    if (p == 0) BO_DONE("we are P1 (the main player): B would leave character select, not pressing it");
    if (b->boPresses >= 3) BO_DONE("still in P%d after 3 B presses; the pad will be unplugged instead", p + 1);
    b->boPresses++;
    snprintf(b->liveStatus, sizeof b->liveStatus, "leaving P%d (B)", p + 1);
    cs_press(c, CSB_B, p < w->ncs ? w->cs[p].playable : 0, now); cs_out(CSB_B, o);
    WHY("character select: backing out (B %d of 3)", b->boPresses);
    return 4;
#undef BO_DONE
}

/* ======================= m4: fighting =======================
 * Layers, in this order, every frame (the first one that wants the pad wins):
 *   1. being hurt: held by an enemy -> mash (or Y special recovery with a healthy bar); knocked into the
 *      air -> tap A for the quick recovery (or Y if juggled); hit repeatedly while standing -> Y.
 *   2. threats: a bullet / thrown object about to hit -> neutral jump (invincible while rising) or the
 *      defensive special; an enemy attack or dash coming at us -> dodge; surrounded -> defensive special.
 *   3. a move already being played back (blitz, throw, ...) continues.
 *   4. star move (boss, big crowd, emergency) - only with a star in stock.
 *   5. holding an enemy: knees, then throw toward the bigger group / a wall (character specific).
 *   6. pickups: food when hurt (left for a human who needs it about as much), weapons, live grenades.
 *   7. low life: retreat behind P1 (with time limits, as before).
 *   8. target: mid range -> blitz / run / dash / pogo / fwd+Y; in range -> back attack, grab, full string
 *      ending in the launcher (Shiva juggles after it); out of range -> approach.
 * Specials cost life: offensive ones only at or above SpecialMinLifePercent, defensive ones at or above
 * DefensiveSpecialMinLifePercent, never two in a row; after one the bot keeps attacking (no new special)
 * for a few seconds to earn back the green health. */
const char *const k_dec_names[D_COUNT] = { "combo", "launcher", "blitz", "back attack", "grab try", "grab", "grab hit", "throw",
    "vault", "grab special", "defensive special", "dodge jump", "grab escape", "special recovery", "quick recover",
    "forward special", "star move", "food", "food left for a human", "weapon pickup", "weapon throw", "grenade thrown back",
    "grenade avoided", "run", "dash", "pogo", "juggle", "pull (fwd+Y)", "double grab", "stun grenade", "retreat",
    "unreachable", "detour", "shield: flank", "shield: grab", "jump attack", "breakable", "money", "chain kept",
    "jump-in", "air attack", "jump-cancel combo", "air special", "crowd jump kick", "anti-air: jumps paused", "made way for P1",
    "breakable: lined up", "breakable: missed, re-aligning", "food left for a lower bot", "spread: other side", "enemy down: waiting", "jump slam: cleared its landing spot", "shield", "trap: no-walk zone", "not released: hanging back", "explosive barrel", "off the floor: not chased", "robot part", "robot leg up: keeping out", "retaliate" };
/* m5: escalating "can't reach it" memory (caged prisoners, enemies behind a fence, objects out of the walkable area) */
static int unreach_find(const BotBrain *b, uint32_t uid) { for (int i = 0; i < 12; i++) if (b->unreach[i].uid == uid && uid) return i; return -1; }
static int unreach_now(const BotBrain *b, uint32_t uid, uint64_t now) { int i = unreach_find(b, uid); return i >= 0 && now < b->unreach[i].until; }
static int unreach_add(BotBrain *b, uint32_t uid, uint64_t now, int passive) {
    int i = unreach_find(b, uid);
    if (i < 0) { i = 0; for (int k = 1; k < 12; k++) if (b->unreach[k].until < b->unreach[i].until) i = k; b->unreach[i].uid = uid; b->unreach[i].fails = 0; }
    int f = ++b->unreach[i].fails; if (passive && f < 3) f = 3;
    uint64_t ms = f <= 1 ? 4000 : f == 2 ? 15000 : 60000;
    b->unreach[i].until = now + ms;
    return (int)(ms / 1000);
}
/* m5: when a pickup was first seen (shared by all bots); the human gets a head start on money and weapons */
static struct { uintptr_t addr; uint64_t first, last; } s_pkSeen[64];
static uint64_t pick_age(uintptr_t a, uint64_t now) {
    int fr = -1, old = 0;
    for (int i = 0; i < 64; i++) {
        if (s_pkSeen[i].addr == a) { s_pkSeen[i].last = now; return now - s_pkSeen[i].first; }
        if (!s_pkSeen[i].addr || now - s_pkSeen[i].last > 3000) { if (fr < 0) fr = i; }
        else if (s_pkSeen[i].last < s_pkSeen[old].last) old = i;
    }
    if (fr < 0) fr = old;
    s_pkSeen[fr].addr = a; s_pkSeen[fr].first = s_pkSeen[fr].last = now;
    return 0;
}
static void note(BotBrain *b, int d, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void note(BotBrain *b, int d, const char *fmt, ...) {
    b->dec[d]++; b->decTotal[d]++;
    char m[160]; va_list ap; va_start(ap, fmt); vsnprintf(m, sizeof m, fmt, ap); va_end(ap);
    /* the first of each kind per stage at Info, the rest at Debug (counts are summed up at the stage end) */
    logf_(b->dec[d] == 1 ? LV_INFO : LV_DEBUG, "%s: %s - %s", b->name, k_dec_names[d], m);
}
void brain_summary(BotBrain *b, const char *why) {
    char m[1024]; int k = 0, any = 0;
    for (int i = 0; i < D_COUNT && k < (int)sizeof m - 40; i++)
        if (b->dec[i]) { k += snprintf(m + k, sizeof m - k, "%s%s %d", any ? ", " : "", k_dec_names[i], b->dec[i]); any = 1; }
    if (any) LOGI("%s: decisions this stage (%s): %s; every other kind 0.", b->name, why, m);
    memset(b->dec, 0, sizeof b->dec);
}
/* ---- input sequences ---- */
static void seq_clear(BotBrain *b) { b->seq.n = b->seq.i = 0; }
static int seq_busy(const BotBrain *b) { return b->seq.i < b->seq.n; }
static void seq_go(BotBrain *b, int kind, int dir, int interruptible, const SeqStep *st, int n, uint64_t now) {
    if (n > 20) n = 20;
    memcpy(b->seq.s, st, (size_t)n * sizeof *st); b->seq.n = n; b->seq.i = 0; b->seq.at = now;
    b->seq.dir = dir ? dir : 1; b->seq.kind = kind; b->seq.interruptible = interruptible;
    if (b->heldAddr) for (int i = 0; i < n; i++) if (st[i].btn & SB_X) { b->heldSwings++; b->heldLastSwingAt = now; }   /* m7.2: every X in it is a swing */
}
#define GO(kind, dir, intr, ...) do { static const SeqStep st_[] = { __VA_ARGS__ }; seq_go(b, kind, dir, intr, st_, (int)(sizeof st_ / sizeof *st_), now); } while (0)
#define F 1
#define BK -1
static void out_btns(const BotBrain *b, int btn, PadOut *o) {
    if (btn & SB_X) o->btn[prof_btn(b, 0, C.attackButton)] = 1;
    if (btn & SB_A) o->btn[prof_btn(b, 2, C.jumpButton)] = 1;
    if (btn & SB_Y) o->btn[prof_btn(b, 1, C.specialButton)] = 1;
    if (btn & SB_B) o->btn[BTN_B] = 1;
    if (btn & SB_STAR) { if (b->seq.kind == -2) { o->btn[prof_btn(b, 1, C.specialButton)] = 1; o->btn[BTN_B] = 1; } else o->btn[C.starButton] = 1; }
    if (btn & SB_RT) o->axis[AX_RT] = 32767;
}
static int seq_run(BotBrain *b, uint64_t now, PadOut *o) {
    Seq *q = &b->seq;
    while (q->i < q->n && now - q->at >= q->s[q->i].ms) { q->at += q->s[q->i].ms; q->i++; }
    if (q->i >= q->n) { seq_clear(b); return 0; }
    const SeqStep *s = &q->s[q->i];
    if (s->x) o->axis[AX_LX] = (int16_t)(s->x * q->dir * b->xSign * STICK);
    if (s->y) o->axis[AX_LY] = (int16_t)(s->y * STICK);          /* raw stick down (+) / up (-) */
    out_btns(b, s->btn, o);
    return 1;
}
/* ---- health budget ---- */
static int special_ok(const BotBrain *b, int defensive, float lifePct, uint64_t now) {
    if (!C.useSpecials || now < b->noSpecialUntil) return 0;
    if (lifePct < (float)(defensive ? C.defSpecialMinLifePct : C.specialMinLifePct)) return 0;
    if (!defensive && now < b->recoverUntil) return 0;          /* still earning back the green health of the last one */
    return 1;
}
static void used_special(BotBrain *b, float lifePct, float banked, uint64_t now) {
    b->specialAt = now; b->lifeAtSpecial = lifePct; b->bankedAtSpecial = banked; b->greenChecks = 1;
    b->noSpecialUntil = now + 1500; b->recoverUntil = now + 4000;
}
static int star_ok(const BotBrain *b, int stars, uint64_t now) { return C.useStars && stars > 0 && (!b->lastStar || now - b->lastStar > 8000); }
static void do_star(BotBrain *b, uint64_t now) {
    GO(b->starYB ? -2 : 7, 1, 0, { 100, 0, 0, SB_STAR }, { 500, 0, 0, 0 });
    b->lastStar = now;
}
static int dodge_ok(BotBrain *b, uint64_t now) {
    uint64_t gap = b->aggression == 0 ? 1500 : b->aggression == 2 ? 2600 : 1900;
    if (now - b->dodgeWindow > 20000) { b->dodgeWindow = now; b->dodgeCount = 0; }
    int n = 0; for (int i = 0; i < 8; i++) if (b->jumpTimes[i] && now - b->jumpTimes[i] < 20000) n++;
    return (!b->lastDodge || now - b->lastDodge >= gap) && b->dodgeCount < 4 && n < 6 && (!b->lastJumpMove || now - b->lastJumpMove > 1000);
}
/* m6: attacking jumps share one budget: one every 1.5 s, 6 per 20 s, not right after a dodge, none for 20 s after
   being hit out of the air twice, and none at a target that just hit us out of the air (anti-air) for 30 s */
static int jump_ok(const BotBrain *b, uint64_t now, uint32_t tgtUid) {
    if (now < b->jumpBanUntil) return 0;
    if (tgtUid && tgtUid == b->antiAirUid && now < b->antiAirUntil) return 0;
    if (b->lastJumpMove && now - b->lastJumpMove < 1500) return 0;
    if (b->lastDodge && now - b->lastDodge < 800) return 0;
    int n = 0; for (int i = 0; i < 8; i++) if (b->jumpTimes[i] && now - b->jumpTimes[i] < 20000) n++;
    return n < 6;
}
static void used_jump(BotBrain *b, int kind, uint64_t now) { b->jumpTimes[b->jumpIdx++ & 7] = now; b->lastJumpMove = now; b->jumpMoveKind = kind; }
/* m6.1: atkDir != 0: an enemy is in front within air reach -> X on the way down (the jump is not wasted) */
static void do_dodge(BotBrain *b, uint64_t now, int atkDir) {
    if (atkDir) GO(8, atkDir, 0, { 90, 0, 0, SB_A }, { 230, 0, 0, 0 }, { 60, F, 0, SB_X }, { 200, 0, 0, 0 });
    else GO(8, 1, 0, { 90, 0, 0, SB_A }, { 380, 0, 0, 0 });     /* neutral jump: stick centred, invincible while rising */
    b->lastDodge = now; b->dodgeCount++;
    b->jumpTimes[b->jumpIdx++ & 7] = now;      /* m6.1: dodges share the jump budget (fewer hops in place) */
}
static float clampf_(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static uint32_t pick_key(uintptr_t a) { return (uint32_t)((a >> 3) ^ (a >> 35)) | 0x80000000u; }   /* m6.3: pickups in the unreachable memory */
static int is_bad_pick(const BotBrain *b, uintptr_t a, uint64_t now) {
    for (int i = 0; i < 8; i++) if (b->badPick[i] == a && now < b->badPickUntil[i]) return 1;
    return 0;
}
static void bad_pick(BotBrain *b, uintptr_t a, uint64_t now) {
    int k = 0; for (int i = 1; i < 8; i++) if (b->badPickUntil[i] < b->badPickUntil[k]) k = i;
    b->badPick[k] = a; b->badPickUntil[k] = now + 8000;
}
static float life_pct(const Chr *c) { float m = c->maxLife > 0 ? c->maxLife : 100.0f; return 100.0f * c->life / m; }
static int is_enemy_of(const Chr *c, const Chr *me) {
    return c->player < 0 && c->team != me->team && c->life > 0 && c->state != CS_DISAPEAR && c->state != CS_STANDDIE;
}
/* ---- m7.2: weapon throws ----
   Returns the reason to throw the weapon in hand at tgt now, or NULL. Throw-only weapons (grenades) keep the old
   rule (in lane, facing, 1.2..5 ranges away). Melee weapons (pipes, bats, knives...) are thrown only when
   WeaponThrows=1 and: knives at range (> 1.3 ranges) after ~0.8 s; other weapons out of melee reach (1.5..5 ranges)
   after 3 s or 4 swings with no enemy in melee range; any weapon whose durability is readable and about to break
   (next hit or <= 20% left) at a target in lane 0.6..5 ranges away. Never during scripted scenes / pauses, never
   with another player (P1, humans) in the throw lane when friendly fire is on or unknown. */
static int throw_lane_blocked(const World *w, const Chr *me, const Chr *tgt, int side, float range, float lane) {
    if (w->friendlyFire == 0) return 0;
    float reach = fabsf_(tgt->pos.x - me->pos.x) + range * 3.0f;
    for (int i = 0; i < w->nch; i++) {
        const Chr *c = &w->ch[i];
        if (c == me || c->player < 0 || c->life <= 0) continue;
        float ax = (c->pos.x - me->pos.x) * (float)side;
        if (ax > -range * 0.3f && ax < reach && fabsf_(c->pos.z - me->pos.z) <= lane * 1.2f) return c->player + 1;
    }
    return 0;
}
static void wt_skip(BotBrain *b, int kind, uint64_t now, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void wt_skip(BotBrain *b, int kind, uint64_t now, const char *fmt, ...) {
    if (b->wtNoteKind == kind && now - b->wtNoteAt < 4000) return;
    b->wtNoteKind = kind; b->wtNoteAt = now;
    char m[140]; va_list ap; va_start(ap, fmt); vsnprintf(m, sizeof m, fmt, ap); va_end(ap);
    note(b, D_WEAPONTHROW, "holding: %s", m);
}
static const char *weapon_throw_why(BotBrain *b, const World *w, const Chr *me, const Chr *tgt, int side, int facing,
                                    float adx, float adz, float range, float lane, uint64_t now) {
    static char why[112];
    float maxThrow = mxf(range * 5.0f, 6.5f);
    if (!me->heldAddr || !facing || adz > lane * 0.8f || adx >= maxThrow) return NULL;
    if (w->mode != 0 || me->scripted || !b->introDone || w->paused) return NULL;          /* scripted scene / menu */
    int onlyThrow = b->heldOnlyThrow || me->heldOnlyThrow;   /* WeaponData.onlyThrow: vials, balls, boomerangs, grenades */
    const char *r = NULL;
    if (onlyThrow) { if (adx > range * 1.2f) r = "throw-only weapon, target in lane at range"; }
    else if (C.weaponThrows) {
        /* m7.3 (m7.2-run1: 38 throws for 48 pickups, most at dx 2-3 after 3 s): melee weapons are for fighting.
           Thrown only (a) when worn out, (b) at a far target after a long quiet spell, (c) knives sometimes at
           range; at most one melee throw per bot every 15 s. */
        float held = (float)(now - b->heldSince) / 1000.0f;
        int nearEnemy = 0;
        for (int i = 0; i < w->nch; i++) { const Chr *c = &w->ch[i];
            if (is_enemy_of(c, me) && fabsf_(c->pos.x - me->pos.x) <= range * 1.2f && fabsf_(c->pos.z - me->pos.z) <= lane * 1.2f) nearEnemy = 1; }
        int cool = !b->lastMeleeThrowAt || now - b->lastMeleeThrowAt >= 15000;
        /* the held weapon's PickupInstance.life is its wear (m7.2-run1: -20 per hit, e.g. 110 -> 90) */
        int durKnown = me->heldMaxLife > 0 && me->heldLife >= 0 && me->heldLife <= me->heldMaxLife + 0.01f;
        int worn = durKnown && (me->heldLife <= me->heldMaxLife * 0.3f || (b->heldLastDrop > 0 && me->heldLife <= b->heldLastDrop * 1.05f));
        float quiet = b->meleeNearAt ? (float)(now - b->meleeNearAt) / 1000.0f : held;
        if (!cool) r = NULL;
        else if (worn && adx > range * 0.6f && now - b->heldSince > 300) {
            snprintf(why, sizeof why, "worn out (life %.0f/%.0f)", me->heldLife, me->heldMaxLife); r = why;
        } else if (me->heldKnife && b->heldKnifeOk && adx >= 4.0f && !nearEnemy && now - b->heldSince > 1000) {
            snprintf(why, sizeof why, "knife at range (dx %.1f, held %.1f s)", adx, held); r = why;
        } else if (adx >= 4.5f && !nearEnemy && quiet >= 6.0f && held >= 10.0f) {
            snprintf(why, sizeof why, "far target, no enemy in reach for %.0f s (held %.0f s)", quiet, held); r = why;
        }
    }
    if (!r) return NULL;
    int who = throw_lane_blocked(w, me, tgt, side, range, lane);
    if (who) { wt_skip(b, 1, now, "P%d is in the throw lane and friendly fire is %s", who, w->friendlyFire < 0 ? "unknown (treated as on)" : "on"); return NULL; }
    if (!onlyThrow) b->lastMeleeThrowAt = now;
    b->throwsDone++;
    return r;
}
/* m7.2 (m7.1-run1, stage 2 jail cells): prisoners behind the bars and enemies waiting for their entrance read
   AiCharacterController.currentBtRoot 1 while standing still with a neutral stick (uids 38-41 at z -4.8..-5.7 for
   80 s; 36 at (-6.8, 2.0) until its door opened). Released / active enemies read root 0 (2 while hurt). CurrentPlane
   is no help (m5.1) and they had entered the screen. Such an enemy is not a target, nor counted as near, unless it
   is right next to us or to P1 (then it is reachable and P1 is on it). */
/* m7.2: explosive barrels (stage 3 ship deck). Once one is hit (fuse timer running, life below its max or
   dropping) every bot keeps out of its blast area until it is gone or ~3 s pass (up to 6 s while the fuse timer
   still runs): it becomes a shared no-walk zone (the m6.7 trap zones: bots inside step out first, walks are
   routed around it, nothing inside is fought for or picked up). Blast area: the damage area's radius * 1.2 when
   readable, else 3.5 x / 2.0 z. */
static struct { uint32_t uid; float lastLife; uint64_t litAt, seenAt; int noted; } s_bar[16];
static void barrel_zones(BotBrain *b, const World *w, uint64_t now) {
    for (int i = 0; i < w->nbr; i++) { const Brk *r = &w->br[i];
        if (r->explosive != 1 && r->blowTimer <= 0) continue;
        int k = 0; for (; k < s_nbar; k++) if (s_bar[k].uid == r->uid) break;
        if (k == s_nbar) { if (s_nbar < 16) k = s_nbar++; else { k = 0; for (int j = 1; j < 16; j++) if (s_bar[j].seenAt < s_bar[k].seenAt) k = j; }
                           memset(&s_bar[k], 0, sizeof s_bar[k]); s_bar[k].uid = r->uid; s_bar[k].lastLife = r->life; }
        int fresh = r->blowTimer > 0 || r->life < s_bar[k].lastLife - 0.01f;          /* fuse running / just hit */
        int hit = fresh || (r->maxLife > 0 && r->life < r->maxLife - 0.01f);           /* (damaged when first seen) */
        s_bar[k].lastLife = r->life; s_bar[k].seenAt = now;
        if (hit && !s_bar[k].litAt) s_bar[k].litAt = now;
        else if (fresh && s_bar[k].litAt && now >= s_bar[k].litAt + 3000 && r->blowTimer <= 0) { s_bar[k].litAt = now; s_bar[k].noted = 0; }   /* hit again later */
        if (!s_bar[k].litAt) continue;
        uint64_t until = s_bar[k].litAt + 3000; if (r->blowTimer > 0 && now + 300 > until) until = now + 300;
        if (until > s_bar[k].litAt + 6000) until = s_bar[k].litAt + 6000;
        if (now >= until) continue;
        float rx = r->radX > 0 ? mxf(r->radX * 1.2f + 0.5f, 2.0f) : 3.5f, rz = r->radZ > 0 ? mxf(r->radZ * 1.2f + 0.3f, 1.2f) : 2.0f;
        int z = 0; for (; z < s_ntz; z++) if (s_tz[z].brkUid == r->uid) break;
        if (z == s_ntz) { if (s_ntz < MAXTZ) s_ntz++; else { z = 0; for (int j = 1; j < MAXTZ; j++) if (s_tz[j].until < s_tz[z].until) z = j; } }
        TrapZone *t = &s_tz[z]; memset(t, 0, sizeof *t);
        t->x0 = r->pos.x - rx; t->x1 = r->pos.x + rx; t->z0 = r->pos.z - rz; t->z1 = r->pos.z + rz; t->until = until; t->brkUid = r->uid;
        if (!s_bar[k].noted) { s_bar[k].noted = 1;
            b->dec[D_BARREL]++; b->decTotal[D_BARREL]++;   /* m7.3: always at Info (the m7.2 'not broken' notes pushed this to Debug) */
            LOGI("%s: explosive barrel - uid %u \"%s\" hit (life %.1f/%.1f, fuse timer %d): everyone keeps %.1f x / %.1f z away (blast %s)", b->name, r->uid, r->name[0] ? r->name : "?",
                 r->life, r->maxLife, r->blowTimer, rx, rz, r->radX > 0 ? "radius from its damage area" : "radius estimated"); }
    }
    /* a lit barrel that left the object list has exploded: its zone ends 0.5 s later */
    for (int z = 0; z < s_ntz; z++) if (s_tz[z].brkUid) {
        int there = 0; for (int i = 0; i < w->nbr; i++) if (w->br[i].uid == s_tz[z].brkUid && w->br[i].life > 0) there = 1;
        if (!there && s_tz[z].until > now + 500) s_tz[z].until = now + 500;
    }
}
static int moving_attack(int st);
static int not_released(const Chr *c) {
    return c->player < 0 && c->btRoot == 1 && !c->boss && !c->aiAttacking && c->state == CS_STANDWALK &&
           fabsf_(c->stickX) < 0.05f && fabsf_(c->stickY) < 0.05f && c->hitStun <= 0;
}
static int moving_attack(int st) { return st == CS_STANDMOVE || st == CS_AIRMOVE || st == CS_DASH; }
/* ---- m7.4: enemies off the walkable floor ----
   m7.3-run2 (19:31:48-19:33:27, giant robot): the seated driver (uid 9, then uid 10; scripted, boss) sat at
   z -0.2 / y 0.8-3.6 while the floor ends at z 1.2; the bots walked at it 50 samples, attacked it once. Kinds:
   1 = seated robot driver (in RobotInstance.driverUids and scripted), 2 = scripted and outside the observed floor
   depth (sticky while it stays scripted), 3 = beyond a learned floor edge for 1.5 s (-> unreachable). */
static uint32_t s_offSticky[8]; static int s_nOffSticky;
static int off_floor_kind(const Chr *c) {
    if (c->robotPart) return 0;
    if (c->robotDriver && c->scripted) return 1;
    int st = -1; for (int i = 0; i < s_nOffSticky; i++) if (s_offSticky[i] == c->uid) st = i;
    if (st >= 0 && !c->scripted) { s_offSticky[st] = s_offSticky[--s_nOffSticky]; st = -1; }
    if (st >= 0) return 2;
    if (!s_fl.known) return 0;
    if (c->scripted && (c->pos.z < s_fl.lo - 0.5f || c->pos.z > s_fl.hi + 0.5f ||
                        (c->pos.y > 1.0f && (c->pos.z < s_fl.lo - 0.2f || c->pos.z > s_fl.hi + 0.2f)))) {
        if (s_nOffSticky < 8) s_offSticky[s_nOffSticky++] = c->uid; else s_offSticky[c->uid & 7] = c->uid;
        return 2;
    }
    if (s_fl.wLo && fabsf_(c->pos.x - s_fl.wLoX) < 8.0f && c->pos.z < s_fl.wLoZ - 0.8f) return 3;
    if (s_fl.wHi && fabsf_(c->pos.x - s_fl.wHiX) < 8.0f && c->pos.z > s_fl.wHiZ + 0.8f) return 3;
    return 0;
}
static int unreach_add(BotBrain *b, uint32_t uid, uint64_t now, int passive);
static int unreach_now(const BotBrain *b, uint32_t uid, uint64_t now);
/* 1 = leave this enemy alone now */
static int off_floor_skip(BotBrain *b, const Chr *c, int kind, uint64_t now) {
    if (kind == 3) {
        if (unreach_now(b, c->uid, now)) return 1;
        int k = -1, old = 0; for (int i = 0; i < 4; i++) { if (b->offT[i].uid == c->uid) k = i; if (b->offT[i].seen < b->offT[old].seen) old = i; }
        if (k < 0 || now - b->offT[k].seen > 500) { if (k < 0) k = old; b->offT[k].uid = c->uid; b->offT[k].since = now; }
        b->offT[k].seen = now;
        if (now - b->offT[k].since < 1500) return 0;
        int secs = unreach_add(b, c->uid, now, 0); b->offT[k].since = now;
        note(b, D_OFFFLOOR, "uid %u past the floor edge for 1.5 s (z %.1f, edge %.1f..%.1f): unreachable for %d s", c->uid, c->pos.z, s_fl.wLo ? s_fl.wLoZ : s_fl.lo, s_fl.wHi ? s_fl.wHiZ : s_fl.hi, secs);
        return 1;
    }
    int seen = 0; for (int i = 0; i < b->nOffNoted; i++) if (b->offNoted[i] == c->uid) seen = 1;
    if (!seen) {
        if (b->nOffNoted < 8) b->offNoted[b->nOffNoted++] = c->uid; else b->offNoted[c->uid & 7] = c->uid;
        note(b, D_OFFFLOOR, "uid %u %s (z %.1f, y %.1f; floor z %.1f..%.1f): not chased", c->uid, kind == 1 ? "is the robot's seated driver" : "is scripted and off the walkable floor",
             c->pos.z, c->pos.y, s_fl.lo, s_fl.hi);
    }
    return 1;
}

/* ---- m7.4: robot parts (m7.3-run2 + a reference video: the mech's two front legs plant at the far left / right at
   the BACK edge of the floor, flash when hit and drain the driver's bar, never break; they lift for a few seconds
   and stab down). Unconfirmed in a live log: positions, height when planted, life, team. So: a part is "down"
   when it is within 0.35 of the lowest height it was seen at, below 1.5, and slow; "up" when 0.8+ above that or
   moving fast. Down parts within 1.0 of the floor depth become pseudo fighters (team 1) placed at the nearest
   walkable depth, so the normal approach / combo / spread code runs on them; up parts are no-walk zones. */
/* m7.5 (m7.4-run1, 20:19:10-20:20:52): parts sit hidden at y -9 before the robot activates (so m7.4's "near the
   lowest y seen" never held again); planted front legs (PatteLeft / PatteRight) read y 0.00 ON the floor (z 3.2, once
   1.27) at x -4.5 / 7.5; rear legs (y -4.4, z -1.3) and Body (z -0.3) are background. Player 1 hit both front legs
   (25 'hit' lines, driver uid 9 552.8 -> 266.6), standing and with jump attacks; legs can be hit while raised too.
   States (absolute heights): hidden y < -1; planted |y| <= 0.3 and slow for 0.2 s; stab = coming down fast;
   raised = above 0.3 otherwise. Only parts over the walkable floor matter. Planted: a normal target (ground combos);
   raised: a target for jump attacks from beside / under it; stab: keep out of its footprint until it has landed. */
#define PART_REACH 0.3f
enum { PS_UNKNOWN, PS_PLANTED, PS_RAISED, PS_STAB, PS_HIDDEN };
typedef struct { uint32_t uid; int st, partId; uint64_t stSince, lowSince, lastAt, stabEnd; float lastFreeze; int logs, hits, stabs; uint64_t plantedMs, raisedMs; char name[20]; } PartTrack;
static PartTrack s_pt[W_MAXRP]; static int s_npt, s_ptScene = -1; static uint64_t s_ptNow, s_ptSumAt, s_botPartMs, s_botPartJumps; static int s_ptLogs;
static float s_drvLife = -1; static uint64_t s_drvDropAt; static uint32_t s_drvUid;
static const char *const k_ps_names[] = { "unknown", "PLANTED (ground target)", "RAISED (jump-attack target)", "STABBING DOWN (keep out)", "hidden" };
static const Chr *robot_driver(const World *w) {
    const Chr *d = NULL;
    for (int i = 0; i < w->nch; i++) { const Chr *c = &w->ch[i];
        if (c->player >= 0 || c->life <= 0) continue;
        if (c->robotDriver && c->scripted) return c;
        if (!d && c->scripted && c->boss) for (int k = 0; k < s_nOffSticky; k++) if (s_offSticky[k] == c->uid) d = c;
    }
    return d;
}
static PartTrack *part_track(uint32_t uid) { for (int i = 0; i < s_npt; i++) if (s_pt[i].uid == uid) return &s_pt[i]; return NULL; }
/* over the walkable floor (with a margin); unknown floor: no */
static int part_on_floor(const RPart *q, float m) { return s_fl.known && q->pos.z >= s_fl.lo - m && q->pos.z <= s_fl.hi + m; }
static int part_is_body(const RPart *q) { return q->name[0] && (strstr(q->name, "Body") || strstr(q->name, "body")); }
static int part_is_rear(const RPart *q) { return q->name[0] && (strstr(q->name, "Rear") || strstr(q->name, "rear")); }
void brain_part_stats(int *n, uint32_t *uid, int *st, uint64_t *plantedMs, uint64_t *raisedMs, int *hits, uint64_t *botMs, uint64_t *botJumps) {
    *n = s_npt; for (int i = 0; i < s_npt; i++) { uid[i] = s_pt[i].uid; st[i] = s_pt[i].st; plantedMs[i] = s_pt[i].plantedMs; raisedMs[i] = s_pt[i].raisedMs; hits[i] = s_pt[i].hits; }
    *botMs = s_botPartMs; *botJumps = s_botPartJumps;
}
void brain_parts_reset(void) { s_npt = 0; s_ptScene = -1; s_ptNow = 0; s_botPartMs = s_botPartJumps = 0; s_drvLife = -1; }
static void parts_update(const World *w, uint64_t now) {
    if (now == s_ptNow) return;
    uint64_t dt = s_ptNow && now > s_ptNow && now - s_ptNow < 500 ? now - s_ptNow : 0; s_ptNow = now;
    if (w->scene != s_ptScene) { s_ptScene = w->scene; s_npt = 0; s_drvLife = -1; }
    const Chr *drv = robot_driver(w), *p1 = NULL;
    for (int i = 0; i < w->nch; i++) if (w->ch[i].player == 0) p1 = &w->ch[i];
    if (drv) { if (drv->uid == s_drvUid && drv->life < s_drvLife - 0.01f) s_drvDropAt = now; s_drvUid = drv->uid; s_drvLife = drv->life; }
    int legHit = 0;
    for (int pass = 0; pass < 2; pass++) for (int i = 0; i < w->nrp; i++) { const RPart *q = &w->rp[i];
        if ((pass == 0) == part_is_body(q)) continue;          /* legs first: a Body flash with a leg hit is not its own hit */
        PartTrack *t = part_track(q->uid);
        if (!t) { if (s_npt >= W_MAXRP) continue; t = &s_pt[s_npt++]; memset(t, 0, sizeof *t); t->uid = q->uid; }
        t->partId = q->partId; snprintf(t->name, sizeof t->name, "%s", q->name);
        float hsp = fabsf_(q->vel.x) + fabsf_(q->vel.z), sp = hsp + fabsf_(q->vel.y);
        int st = t->st;
        if (q->pos.y < -0.3f) { st = PS_HIDDEN; t->lowSince = 0; }   /* below the floor: hidden / background */
        else if (fabsf_(q->pos.y) <= 0.3f && sp < 0.04f) { if (!t->lowSince) t->lowSince = now; if (now - t->lowSince >= 200 || t->st == PS_PLANTED) st = PS_PLANTED; }
        else {
            t->lowSince = 0;
            if (q->vel.y < -0.03f && q->pos.y > 0.05f) st = PS_STAB;
            else if (q->pos.y > 0.3f) st = PS_RAISED;
            else if (st == PS_PLANTED || st == PS_UNKNOWN || st == PS_HIDDEN) st = PS_RAISED;   /* low but moving */
        }
        if (st == PS_STAB) t->stabEnd = now + 300; else if (t->st == PS_STAB && st != PS_STAB) t->stabEnd = now + 300;
        if (st != t->st) {
            if (st == PS_STAB) t->stabs++;
            if (s_ptLogs++ < 900 && !(st == PS_HIDDEN && t->st == PS_UNKNOWN))
                LOGI("robot part uid %u (part %d \"%s\", robot %d): %s at (%.2f, %.2f, %.2f) vel (%.3f, %.3f, %.3f), life %.1f, hitFreeze %.1f, team %d; %s floor z %.2f..%.2f",
                     q->uid, q->partId, q->name[0] ? q->name : "?", q->robotId, k_ps_names[st], q->pos.x, q->pos.y, q->pos.z, q->vel.x, q->vel.y, q->vel.z, q->life, q->hitFreeze, q->team,
                     part_on_floor(q, 0.5f) ? "over the" : "OFF the", s_fl.lo, s_fl.hi);
            t->st = st; t->stSince = now;
        }
        if (dt && part_on_floor(q, 0.5f)) { if (st == PS_PLANTED) t->plantedMs += dt; else if (st == PS_RAISED || st == PS_STAB) t->raisedMs += dt; }
        if (q->hitFreeze > 0 && t->lastFreeze <= 0) {
            if (pass == 0) { legHit = 1; t->hits++; } 
            if ((pass == 0 || !legHit) && t->logs++ < 200 && s_ptLogs++ < 900) {
                const Chr *hb = NULL; float hd = 1e30f;               /* nearest player: who probably hit it */
                for (int k = 0; k < w->nch; k++) { const Chr *c = &w->ch[k]; if (c->player < 0) continue; float d = fabsf_(c->pos.x - q->pos.x) + fabsf_(c->pos.z - q->pos.z); if (d < hd) { hd = d; hb = c; } }
                LOGI("robot part uid %u (\"%s\", %s) hit (hitFreeze %.1f) at (%.2f, %.2f, %.2f); driver uid %u life %.1f; nearest player P%d uid %u at (%.2f, %.2f, %.2f) %s move %d (dx %.2f dz %.2f)%s",
                     q->uid, q->name[0] ? q->name : "?", k_ps_names[st], q->hitFreeze, q->pos.x, q->pos.y, q->pos.z, drv ? drv->uid : 0, drv ? drv->life : -1.0f,
                     hb ? hb->player + 1 : 0, hb ? hb->uid : 0, hb ? hb->pos.x : 0, hb ? hb->pos.y : 0, hb ? hb->pos.z : 0, hb ? state_name(hb->state) : "-", hb ? hb->moveId : -1,
                     hb ? hb->pos.x - q->pos.x : 0, hb ? hb->pos.z - q->pos.z : 0, pass == 1 ? " (body only)" : "");
            }
        }
        t->lastFreeze = q->hitFreeze;
        t->lastAt = now;
    }
    (void)p1;
    if (s_npt && now - s_ptSumAt >= 15000) {                    /* summary every 15 s while parts exist */
        s_ptSumAt = now; char line[700]; int L = snprintf(line, sizeof line, "robot parts summary:");
        for (int i = 0; i < s_npt && L < (int)sizeof line - 90; i++) { const PartTrack *t = &s_pt[i];
            if (!t->plantedMs && !t->raisedMs && !t->hits) continue;
            L += snprintf(line + L, sizeof line - L, " %s (uid %u) planted %.1f s, raised %.1f s, %d stab(s), %d hit(s);", t->name[0] ? t->name : "?", t->uid, t->plantedMs / 1000.0, t->raisedMs / 1000.0, t->stabs, t->hits); }
        snprintf(line + L, sizeof line - L, " bots on legs %.1f s (bot-seconds), %llu jump attack(s)", s_botPartMs / 1000.0, (unsigned long long)s_botPartJumps);
        LOGI("%s", line);
    }
}
static int robot_ok(const World *w, int robotId) {
    for (int i = 0; i < w->nrb; i++) if (w->rb[i].uid == robotId) return w->rb[i].active != 0 && w->rb[i].broken != 1;
    return 1;                                                   /* robot list unreadable: allow */
}
static World s_aug;
static const World *robot_world(const World *w, const Chr *me, uint64_t now) {
    if (!w->nrp) return w;
    memcpy(&s_aug, w, sizeof *w);
    const Chr *drv = robot_driver(w);
    if (!drv && w->nrb > 0) {                                   /* robot read, nobody in the seat: parts drain nothing */
        static uint64_t s_noDrvLog; if (now - s_noDrvLog > 10000) { s_noDrvLog = now; LOGI("robot parts: %d part(s), no seated driver: not targets", w->nrp); }
        return w;
    }
    for (int i = 0; i < w->nrp && s_aug.nch < W_MAXCH; i++) { const RPart *q = &w->rp[i]; const PartTrack *t = part_track(q->uid);
        if (!t || (t->st != PS_PLANTED && t->st != PS_RAISED) || !robot_ok(w, q->robotId) || part_is_body(q) || part_is_rear(q)) continue;
        if (!part_on_floor(q, PART_REACH) || q->pos.y > 3.5f) continue;   /* background / out of jump reach */
        if (t->st == PS_RAISED && fabsf_(q->vel.x) + fabsf_(q->vel.z) >= 0.08f) continue;   /* swinging across: not now */
        if (now < t->stabEnd) continue;
        float z = q->pos.z;
        if (z < s_fl.lo + 0.05f) z = s_fl.lo + 0.05f; if (z > s_fl.hi - 0.05f) z = s_fl.hi - 0.05f;
        Chr *c = &s_aug.ch[s_aug.nch++]; memset(c, 0, sizeof *c);
        c->uid = q->uid; c->team = me->team == 1 ? 2 : 1; c->player = -1;
        c->life = drv ? drv->life : q->life > 0 ? q->life : 100.0f; c->maxLife = -1;
        /* the foot is wide: player 1 landed hits from up to ~1.0 beside the part's x (m7.4-run1 P1 at dx -1.0..+0.9, z within
           ~0.3): aim at the point of the foot nearest to us (x +-0.7, z +-0.5; P1 hit uid 11 from 0.7 in front of it) */
        { float ox = me->pos.x - q->pos.x, oz = me->pos.z - z; if (ox > 0.7f) ox = 0.7f; if (ox < -0.7f) ox = -0.7f; if (oz > 0.5f) oz = 0.5f; if (oz < -0.5f) oz = -0.5f; c->pos.x = q->pos.x + ox; z += oz; }
        c->pos.y = t->st == PS_RAISED ? q->pos.y : 0; c->pos.z = z; c->facing = 0;
        c->state = (q->hitFreeze > 0 || (s_drvDropAt && now - s_drvDropAt < 400)) ? CS_STANDHIT : CS_STANDWALK;
        c->ctrlKind = CTRL_AI; c->moveId = -1; c->cantBeGrabbed = 1; c->entered = 1; c->collidable = 1; c->plane = -1;
        c->btRoot = -1; c->lineOfSight = -1; c->aiTarget = 0xFFFFFFFEu; c->stamina = c->maxStamina = -1; c->heldLife = c->heldMaxLife = -1;
        c->robotPart = t->st == PS_RAISED ? 2 : 1; snprintf(c->dname, sizeof c->dname, "%s", q->name[0] ? q->name : "ROBOT_PART");
    }
    return &s_aug;
}
/* a leg stabbing down: keep out of its footprint (x +-1.6, z +-1.2 around it) until 0.3 s after it landed */
static TrapZone s_stabZ; static int s_stabIn;
static void robot_zones(BotBrain *b, const World *w, const Chr *me, uint64_t now) {
    s_stabIn = 0;
    for (int i = 0; i < w->nrp; i++) { const RPart *q = &w->rp[i]; const PartTrack *t = part_track(q->uid);
        if (!t || now >= t->stabEnd || t->st == PS_HIDDEN || !robot_ok(w, q->robotId) || part_is_body(q) || part_is_rear(q) || !part_on_floor(q, 0.5f)) continue;
        int z = 0; for (; z < s_ntz; z++) if (s_tz[z].partUid == q->uid) break;
        if (z == s_ntz) { if (s_ntz < MAXTZ) s_ntz++; else { z = 0; for (int j = 1; j < MAXTZ; j++) if (s_tz[j].until < s_tz[z].until) z = j; } memset(&s_tz[z], 0, sizeof s_tz[z]); }
        TrapZone *tz = &s_tz[z]; tz->partUid = q->uid; tz->x0 = q->pos.x - 1.6f; tz->x1 = q->pos.x + 1.6f; tz->z0 = q->pos.z - 1.2f; tz->z1 = q->pos.z + 1.2f; tz->until = t->stabEnd;
        if (me->pos.x > tz->x0 - 0.3f && me->pos.x < tz->x1 + 0.3f && me->pos.z > tz->z0 - 0.2f && me->pos.z < tz->z1 + 0.2f) { s_stabIn = 1; s_stabZ = *tz; }
        if (me->pos.x > tz->x0 - 1.0f && me->pos.x < tz->x1 + 1.0f && (b->partNoted != q->uid || now - b->partEvadeAt > 2000)) {
            b->partNoted = q->uid; b->partEvadeAt = now;
            note(b, D_PARTEVADE, "robot part uid %u stabbing down (y %.1f, vy %.3f at x %.1f, z %.1f): keeping out of x %.1f..%.1f z %.1f..%.1f", q->uid, q->pos.y, q->vel.y, q->pos.x, q->pos.z, tz->x0, tz->x1, tz->z0, tz->z1);
        }
    }
}
/* the robot (or a part) is what hurt us: never a learned floor trap */
static int robot_hurt_near(const World *w, const Chr *me) {
    for (int i = 0; i < w->nrp; i++) { const RPart *q = &w->rp[i];
        if (q->pos.y < -1.0f) continue;
        if (me->hitSrc && me->hitSrc == q->uid) return 1;
        if (fabsf_(q->pos.x - me->pos.x) < 4.5f && fabsf_(q->pos.z - me->pos.z) < 3.0f) return 1;
    }
    for (int i = 0; i < w->nch; i++) if (w->ch[i].robotDriver && me->hitSrc && w->ch[i].uid == me->hitSrc) return 1;
    return 0;
}

/* ---- m7.4: friendly fire retaliation (tit for tat). Each life drop of a bot whose last hitter
   (Character.lastHitReceivedSourceUid) is a HUMAN player standing near it adds one owed hit (max 10) for that bot
   toward that human; it is paid only by a hit of that bot's retaliation swing landing on that human. Bot-on-bot
   hits never count. Only with friendly fire on (the game option; unknown -> a player-on-player hit seen).
   Unpaid hits expire after 20 s without a chance (the human not near the bot). */
static struct { int debt[4][4]; uint64_t at[4][4]; float prevLife[4]; uint32_t prevUid[4]; uint64_t now; int ffSeen, bbHits, ffLogged; } s_rl;
void brain_retal_reset(void) { memset(&s_rl, 0, sizeof s_rl); }
int brain_retal_debt(int botSlot, int humanSlot) { return botSlot >= 0 && botSlot < 4 && humanSlot >= 0 && humanSlot < 4 ? s_rl.debt[botSlot][humanSlot] : 0; }
int brain_retal_ff_seen(void) { return s_rl.ffSeen; }
static int slot_is_bot(const BotBrain *all, int nall, const World *w, int s) {
    for (int k = 0; k < nall; k++) if (all[k].playerSlot == s && all[k].identified) return 1;
    return s < w->nslot && w->slot[s].ctrl >= 0 && brain_ctrl_is_bot(all, nall, w->slot[s].ctrl);
}
static int retal_on(const World *w) { return C.friendlyRetaliate && w->friendlyFire != 0 && (w->friendlyFire == 1 || s_rl.ffSeen); }
static void retal_update(const BotBrain *all, int nall, const World *w, uint64_t now) {
    if (now == s_rl.now) return; s_rl.now = now;
    for (int v = 0; v < 4; v++) {
        const Chr *vc = NULL; for (int i = 0; i < w->nch; i++) if (w->ch[i].player == v && w->ch[i].team == 0 && w->ch[i].state != CS_DISAPEAR) { vc = &w->ch[i]; break; }
        if (!vc) { s_rl.prevUid[v] = 0; continue; }
        int dropped = vc->uid == s_rl.prevUid[v] && vc->life < s_rl.prevLife[v] - 0.01f;
        s_rl.prevUid[v] = vc->uid; s_rl.prevLife[v] = vc->life;
        if (!dropped || !vc->hitSrc) continue;
        const Chr *ac = NULL; for (int i = 0; i < w->nch; i++) if (w->ch[i].uid == vc->hitSrc) ac = &w->ch[i];
        if (!ac || ac->player < 0 || ac->player > 3 || ac == vc || ac->team != vc->team) continue;
        if (fabsf_(ac->pos.x - vc->pos.x) > 4.0f || fabsf_(ac->pos.z - vc->pos.z) > 1.5f) continue;   /* a stale hitter uid (trap, fall) */
        int a = ac->player;
        if (w->friendlyFire == 0) continue;                     /* the game says off: a stale reading, never a debt */
        if (!s_rl.ffSeen) { s_rl.ffSeen = 1; LOGI("retaliate: friendly fire seen (P%d hit P%d)%s", a + 1, v + 1, w->friendlyFire == 1 ? "" : "; the game option was not readable"); }
        int vBot = slot_is_bot(all, nall, w, v), aBot = slot_is_bot(all, nall, w, a);
        if (vBot && aBot) { s_rl.bbHits++; continue; }          /* bot on bot: never a debt */
        if (vBot && !aBot) {
            if (!C.friendlyRetaliate) continue;
            if (s_rl.debt[v][a] < 10) s_rl.debt[v][a]++;
            s_rl.at[v][a] = now;
            LOGI("retaliate: P%d owes P%d %d (hit by uid %u, life %.1f)", v + 1, a + 1, s_rl.debt[v][a], ac->uid, vc->life);
        } else if (!vBot && aBot && s_rl.debt[a][v] > 0) {
            const BotBrain *ab = NULL; for (int k = 0; k < nall; k++) if (all[k].playerSlot == a) ab = &all[k];
            if (ab && ab->rlSwingSlot == v && now <= ab->rlSwingUntil) {
                s_rl.debt[a][v]--; s_rl.at[a][v] = now;
                LOGI("retaliate hit: P%d hit P%d back (%d still owed)", a + 1, v + 1, s_rl.debt[a][v]);
            }
        }
    }
    for (int v = 0; v < 4; v++) for (int a = 0; a < 4; a++) if (s_rl.debt[v][a] && now - s_rl.at[v][a] > 20000) {
        LOGI("retaliate: P%d lets P%d off (%d unpaid, no chance for 20 s)", v + 1, a + 1, s_rl.debt[v][a]); s_rl.debt[v][a] = 0; }
}
static void tz_add(BotBrain *b, const World *w, const Chr *me, uint64_t now) {
    const Haz *hz = NULL; float hb = 1e30f;
    for (int i = 0; i < w->nhz; i++) { const Haz *h = &w->hz[i];
        int in = h->haveZone && me->pos.x > h->x0 - 0.6f && me->pos.x < h->x1 + 0.6f && me->pos.z > h->z0 - 0.6f && me->pos.z < h->z1 + 0.6f;
        float d = fabsf_(h->pos.x - me->pos.x) + fabsf_(h->pos.z - me->pos.z);
        if ((in || (fabsf_(h->pos.x - me->pos.x) < 3.0f && fabsf_(h->pos.z - me->pos.z) < 2.0f)) && (in ? d - 100.0f : d) < hb) { hb = in ? d - 100.0f : d; hz = h; } }
    TrapZone nz; memset(&nz, 0, sizeof nz);
    if (hz) {
        if (hz->haveZone && hb < -50.0f) { nz.x0 = hz->x0 - 0.3f; nz.x1 = hz->x1 + 0.3f; nz.z0 = hz->z0 - 0.3f; nz.z1 = hz->z1 + 0.3f; }
        else { nz.x0 = mnf(hz->pos.x, me->pos.x) - 0.9f; nz.x1 = mxf(hz->pos.x, me->pos.x) + 0.9f; nz.z0 = mnf(hz->pos.z, me->pos.z) - 0.45f; nz.z1 = mxf(hz->pos.z, me->pos.z) + 0.45f; }
        nz.hazUid = hz->uid; nz.hotAnim = hz->anim; nz.until = now + 30000;
    } else { nz.x0 = me->pos.x - 1.2f; nz.x1 = me->pos.x + 1.2f; nz.z0 = me->pos.z - 0.6f; nz.z1 = me->pos.z + 0.6f; nz.until = now + 20000; }
    for (int i = 0; i < s_ntz; i++) { TrapZone *t = &s_tz[i];
        int same = (nz.hazUid && t->hazUid == nz.hazUid) || (!nz.hazUid && !t->hazUid && nz.x0 < t->x1 && nz.x1 > t->x0 && nz.z0 < t->z1 && nz.z1 > t->z0);
        if (!same) continue;
        float x0 = mnf(t->x0, nz.x0), x1 = mxf(t->x1, nz.x1), z0 = mnf(t->z0, nz.z0), z1 = mxf(t->z1, nz.z1);
        if (x1 - x0 <= 6.0f && z1 - z0 <= 3.0f) { t->x0 = x0; t->x1 = x1; t->z0 = z0; t->z1 = z1; }
        t->until = nz.until > t->until ? nz.until : t->until; t->hotAnim = nz.hotAnim;
        note(b, D_TRAP, "hurt at (%.1f, %.1f) with no enemy near: zone x %.1f..%.1f z %.1f..%.1f%s refreshed", me->pos.x, me->pos.z, t->x0, t->x1, t->z0, t->z1, t->hazUid ? " (hazard object)" : "");
        return;
    }
    int k = s_ntz < MAXTZ ? s_ntz++ : 0;
    if (k == 0 && s_ntz == MAXTZ) for (int i = 1; i < MAXTZ; i++) if (s_tz[i].until < s_tz[k].until) k = i;
    s_tz[k] = nz;
    if (hz) note(b, D_TRAP, "hurt at (%.1f, %.1f) with no enemy near, next to hazard uid %u (anim %d): no walking in x %.1f..%.1f z %.1f..%.1f while it plays anim %d (30 s)",
                 me->pos.x, me->pos.z, hz->uid, hz->anim, nz.x0, nz.x1, nz.z0, nz.z1, hz->anim);
    else note(b, D_TRAP, "hurt at (%.1f, %.1f) with no enemy, attack or projectile near (trap not visible in the game state): no walking in x %.1f..%.1f z %.1f..%.1f for 20 s",
              me->pos.x, me->pos.z, nz.x0, nz.x1, nz.z0, nz.z1);
}
/* hurt with nobody who could have done it: no enemy (any state: thrown bodies hit too) within 3.5 x / 1.5 z, no
   enemy attacking or dashing within 0.6 screen in our depth band, no projectile within 4, not held, no slam */
static int trap_hit_plausible(const World *w, const Chr *me, float screenW, float lane) {
    if (me->grabbedBy) return 0;
    if (robot_hurt_near(w, me)) return 0;                      /* m7.5: robot legs (m7.4-run1: zones on the leg landing spots) */
    if (me->hitSrc) for (int i = 0; i < w->nch; i++) { const Chr *c = &w->ch[i];   /* m7.5: friendly fire (m7.4-run1 20:08:57) */
        if (c != me && c->uid == me->hitSrc && c->player >= 0 && fabsf_(c->pos.x - me->pos.x) < 4.0f && fabsf_(c->pos.z - me->pos.z) < 1.5f) return 0; }
    for (int i = 0; i < w->nch; i++) { const Chr *c = &w->ch[i];
        if (c == me || c->life <= 0 || c->state == CS_DISAPEAR) continue;
        float adx = fabsf_(c->pos.x - me->pos.x), adz = fabsf_(c->pos.z - me->pos.z);
        if (is_enemy_of(c, me)) {
            if (adx < 3.5f && adz < 1.5f) return 0;
            if ((moving_attack(c->state) || c->aiAttacking || slam_air(c)) && adx < screenW * 0.6f && adz < lane * 4.0f) return 0;
        } else if (moving_attack(c->state) && adx < 2.0f && adz < 1.0f) return 0;     /* a teammate's swing */
    }
    for (int i = 0; i < w->npj; i++) if (fabsf_(w->pj[i].pos.x - me->pos.x) < 4.0f && fabsf_(w->pj[i].pos.z - me->pos.z) < 2.0f) return 0;
    return 1;
}

static void held_track(BotBrain *b, const Chr *me, uint64_t now) {   /* m7.2: called every frame from fight() */
    if (me->heldAddr != b->heldAddr) {
        b->heldAddr = me->heldAddr; if (!me->heldAddr) b->heldOnlyThrow = 0;
        b->heldSince = now; b->heldSwings = 0; b->heldLastSwingAt = 0; b->heldDurOk = 0; b->heldLastDrop = 0; b->heldLastLife = me->heldLife;
        if (me->heldAddr && me->heldKnife) { b->rng = b->rng * 1103515245u + 12345u + (uint32_t)now; b->heldKnifeOk = ((b->rng >> 16) % 100) < 40; }   /* m7.3: 40% of knives may be thrown */
        if (me->heldAddr) LOGI("%s: weapon in hand: \"%s\" life %.1f/%.1f type %d onlyThrow %d%s", b->name, me->heldName[0] ? me->heldName : "?",
                               me->heldLife, me->heldMaxLife, me->heldWType, me->heldOnlyThrow, me->heldKnife ? " (knife)" : "");
    } else if (me->heldAddr && me->heldLife >= 0 && b->heldLastLife >= 0 && me->heldLife < b->heldLastLife - 0.01f) {
        /* m7.2: the held weapon's PickupInstance.life went down. It only counts as durability when it drops right
           after one of our swings (a despawn timer would tick down on its own). */
        float drop = b->heldLastLife - me->heldLife;
        int onHit = b->heldLastSwingAt && now - b->heldLastSwingAt < 900 && (me->heldMaxLife <= 0 || drop >= me->heldMaxLife * 0.02f);   /* a timer ticks in tiny steps */
        if (onHit) { if (!b->heldDurOk) LOGI("%s: weapon life %.1f -> %.1f (max %.1f) after a swing: durability is readable", b->name, b->heldLastLife, me->heldLife, me->heldMaxLife);
                     b->heldDurOk = 1; b->heldLastDrop = drop; }
        else logf_(LV_DEBUG, "%s: weapon life %.1f -> %.1f with no swing (not counted as durability)", b->name, b->heldLastLife, me->heldLife);
        b->heldLastLife = me->heldLife;
    } else if (me->heldAddr) b->heldLastLife = me->heldLife;
}
static void fight(BotBrain *b, BotBrain *all, int nall, const World *w, const Chr *me, uint64_t now, PadOut *o) {
    held_track(b, me, now);
    if (!b->fightLastAt || now - b->fightLastAt > 1000) b->fightSince = now; b->fightLastAt = now;
    if (me->life > b->maxLife) b->maxLife = me->life;
    float maxLife = me->maxLife > 0 ? me->maxLife : b->maxLife > 0 ? b->maxLife : 100.0f;
    float lifePct = 100.0f * me->life / maxLife;
    const CharProfile *pf = b->prof ? b->prof : &k_profiles[0];
    s_walkLo = -1e30f; s_walkHi = 1e30f;
    if (me->scripted) { seq_clear(b); WHY("idle: cutscene / script controls our fighter"); set_mode(b, M_SCRIPT, now, NULL); return; }

    /* scale: from the camera, unless the ini sets absolute values */
    float sizeY = w->camSizeY > 1.0f ? w->camSizeY : 1080.0f;
    float screenW = sizeY * 16.0f / 9.0f;
    float range = auto_or(C.attackRange, screenW * 0.065f) * b->rangeScale * (pf->rangeMul > 0 ? pf->rangeMul : 1.0f);
    float lane = auto_or(C.laneTolerance, sizeY * 0.03f) * b->laneScale;
    float follow = auto_or(C.followDistance, screenW * 0.15f);

    const PlayerStateInfo *ps = b->playerSlot >= 0 && b->playerSlot < w->nps ? &w->ps[b->playerSlot] : NULL;
    int stars = ps ? ps->stars : -1;
    b->starYB = C.starMode == 2 || (C.starMode == 0 && ps && ps->starBtn == 0 && ps->starYB == 1);
    float banked = ps ? ps->banked : -1;
    /* is PlayerState.bankedLife the green (recoverable) health? Learned: it must rise right after our special */
    if (b->greenChecks && now - b->specialAt >= 600) {
        b->greenChecks = 0;
        if (banked >= 0 && banked > b->bankedAtSpecial + 0.5f) {
            if (!b->greenKnown) LOGI("%s: green health is readable (bankedLife %.1f -> %.1f after our special); no new special until it is earned back (8 s at most).", b->name, b->bankedAtSpecial, banked);
            b->greenKnown = 1;
        } else if (!b->greenKnown && ++b->greenNoChange == 3)
            LOGI("%s: bankedLife (%.1f) did not change after 3 specials; using a fixed 4 s attack-only window after each special.", b->name, banked);
    }
    if (b->greenKnown && banked > 1.0f && b->specialAt && now - b->specialAt < 8000 && b->recoverUntil < now + 100) b->recoverUntil = now + 100;

    /* P1 = first slot whose controller is not a bot's */
    const Chr *p1 = NULL; float humanNeed = 1000.0f;
    for (int s = 0; s < w->nslot; s++) {
        if (s == b->playerSlot || w->slot[s].ctrl < 0 || brain_ctrl_is_bot(all, nall, w->slot[s].ctrl)) continue;
        const Chr *h = slot_fighter(w, s);
        if (!h) continue;
        if (!p1) p1 = h;
        if (h->life > 0 && h->state != CS_STANDDIE && life_pct(h) < humanNeed) humanNeed = life_pct(h);
    }
    float anchorX = p1 ? p1->pos.x : w->haveCam ? w->cam.x : me->pos.x;
    float anchorZ = p1 ? p1->pos.z : me->pos.z;
    /* m6: watch P1 (position, velocity, facing, stick, attacks) to stay out of the way */
    if (p1) {
        if (p1->uid != b->p1Uid || !b->p1PrevAt || now - b->p1PrevAt > 1000 || now < b->p1PrevAt) {
            b->p1Uid = p1->uid; b->p1PrevX = p1->pos.x; b->p1PrevZ = p1->pos.z; b->p1PrevAt = now; b->p1Vx = b->p1Vz = 0;
        } else if (now - b->p1PrevAt >= 100) {
            float dt = (float)(now - b->p1PrevAt) / 1000.0f;
            b->p1Vx = 0.5f * b->p1Vx + 0.5f * (p1->pos.x - b->p1PrevX) / dt; b->p1Vz = 0.5f * b->p1Vz + 0.5f * (p1->pos.z - b->p1PrevZ) / dt;
            b->p1PrevX = p1->pos.x; b->p1PrevZ = p1->pos.z; b->p1PrevAt = now;
        }
        static uint64_t s_p1Log;
        if (C.p1WatchMs && (now - s_p1Log >= (uint64_t)C.p1WatchMs || now < s_p1Log)) {
            s_p1Log = now;
            LOGI("P1 watch: uid %u at (%.2f, %.2f) vel (%.2f, %.2f)/s facing %d stick (%.2f, %.2f) %s move %d life %.0f%%%s",
                 p1->uid, p1->pos.x, p1->pos.z, b->p1Vx, b->p1Vz, p1->facing, p1->stickX, p1->stickY, state_name(p1->state), p1->moveId,
                 life_pct(p1), moving_attack(p1->state) ? " ATTACKING" : "");
        }
    }

    /* ---- enemies ---- */
    const Chr *tgt = NULL, *incoming = NULL, *behind = NULL, *bossNear = NULL; float bestScore = 1e30f, inBest = 1e30f, bhBest = 1e30f;
    int nearCount = 0, nearFront = 0, nearBehind = 0; float nearestEnemy = 1e30f;
    /* m5.1: the m5 run showed CurrentPlane is NOT a cage/background marker (ordinary enemies read plane 1 and 2
       while the players read 0), so the plane filter is gone. The other m5 filters (never entered the screen,
       outside the camera) may never leave the bot with nothing to hit: if they drop every enemy, pass 2 targets
       like m4 did. */
    const Chr *slam = NULL; float slamBest = 1e30f; s_slamOn = 0;
    s_w = w; if (w->scene != s_tzScene) { s_tzScene = w->scene; s_ntz = 0; }   /* m6.7: trap zones belong to one scene */
    { int k = 0; for (int i = 0; i < s_ntz; i++) if (now < s_tz[i].until) s_tz[k++] = s_tz[i]; s_ntz = k; }
    barrel_zones(b, w, now);                                                    /* m7.2 */
    robot_zones(b, w, me, now);                                                 /* m7.4 */
    for (int i = 0; i < w->nch; i++) { const Chr *c = &w->ch[i];
        if (is_enemy_of(c, me) && slam_air(c)) { float d = fabsf_(c->pos.x - me->pos.x) + fabsf_(c->pos.z - me->pos.z); if (d < slamBest) { slamBest = d; slam = c; } } }
    int nCaged = 0; uint32_t cagedSum = 0; const Chr *caged1 = NULL;
    for (int pass = 0; pass < 2; pass++) {
    int dropped = 0; nCaged = 0; cagedSum = 0; caged1 = NULL;
    tgt = incoming = behind = bossNear = NULL; bestScore = inBest = bhBest = 1e30f; nearCount = nearFront = nearBehind = 0; nearestEnemy = 1e30f;
    for (int i = 0; i < w->nch; i++) {
        const Chr *c = &w->ch[i];
        if (!is_enemy_of(c, me)) continue;
        if (fabsf_(c->pos.x - anchorX) > screenW * 0.75f) continue;          /* off screen */
        if (not_released(c)) {                                                 /* m7.2: caged / waiting: never a fallback target either */
            int nearMe = fabsf_(c->pos.x - me->pos.x) <= range * 1.2f && fabsf_(c->pos.z - me->pos.z) <= lane * 1.2f;
            int nearP1 = p1 && fabsf_(c->pos.x - p1->pos.x) <= range * 1.5f && fabsf_(c->pos.z - p1->pos.z) <= lane * 2.0f;
            if (!nearMe && !nearP1) { nCaged++; cagedSum += c->uid; if (!caged1) caged1 = c; continue; }
        }
#ifndef NO_OFFFLOOR   /* (replay comparison build only) */
        { int of = off_floor_kind(c); if (of && off_floor_skip(b, c, of, now)) continue; }   /* m7.4 */
#endif
        if (pass == 0 && w->haveCam && fabsf_(c->pos.x - w->cam.x) > screenW * 0.5f + range && fabsf_(c->pos.x - anchorX) > screenW * 0.5f) { dropped++; continue; }   /* m5: outside the camera */
        if (pass == 0 && c->entered == 0) { dropped++; continue; }             /* m5: never entered the screen (background / waiting) */
        float dx = c->pos.x - me->pos.x, adx = fabsf_(dx), adz = fabsf_(c->pos.z - me->pos.z);
        float d = adx + 2.0f * adz; if (d < nearestEnemy) nearestEnemy = d;
        if (adx < range * 1.3f && adz < lane * 2.0f && !state_is_down(c->state)) {
            nearCount++; if (sgn(dx) == me->facing || dx == 0) nearFront++; else nearBehind++;
        }
        if (!state_is_hurt(c->state) && moving_attack(c->state) && (c->aiTarget == me->uid || c->aiTarget == 0) &&
            c->facing == -sgn(dx) && adx < range * (c->state == CS_DASH ? 2.5f : 1.4f) && adz < lane * 1.6f && adx < inBest) { inBest = adx; incoming = c; }
        if (sgn(dx) == -me->facing && adx < range * 0.9f && adz < lane && !state_is_down(c->state) && adx < bhBest) { bhBest = adx; behind = c; }
        if (c->boss && adx < range * 2.5f && adz < lane * 2.0f && !c->invincible) bossNear = c;
        if (c->uid == b->ignoreUid && now < b->ignoreUntil) continue;        /* couldn't reach it a moment ago */
        if (unreach_now(b, c->uid, now)) continue;                            /* m5: escalating */
        if (c->untargettable) continue;
        float s = adx + 3.0f * adz + (state_is_down(c->state) ? screenW : 0) + (c->uid == b->targetUid ? -range * 0.5f : 0);
        if (c->uid == b->punishUid && now < b->punishUntil) s -= range * 2.0f;   /* m6.6: punish the slam's landing recovery */
        int shieldUpC = (c->shieldUser || c->uid == b->shieldUid) && !c->staminaBroken && !state_is_hurt(c->state) && !state_is_down(c->state);
        for (int k = 0; k < nall; k++) if (&all[k] != b && all[k].identified && all[k].targetUid == c->uid)
            s += shieldUpC ? -range * 0.5f : range * 3.0f;   /* spread over enemies (m6: stronger); m6.7: pile onto a shield cop */
        if (s < bestScore) { bestScore = s; tgt = c; }
    }
    if (tgt || !dropped) break;
    if (!b->filterFallbackLogged) { b->filterFallbackLogged = 1; LOGI("%s: the m5 target filters left no enemy (%d dropped); targeting them anyway (m4 rules).", b->name, dropped); }
    }
    if (nCaged && (nCaged != b->cagedN || cagedSum != b->cagedSum) && now - b->cagedNoteAt > 3000) {
        b->cagedNoteAt = now;
        note(b, D_CAGED, "%d enem%s not released yet (behavior root 1, standing still; e.g. uid %u at (%.1f, %.1f)): not targeted%s",
             nCaged, nCaged == 1 ? "y" : "ies", caged1->uid, caged1->pos.x, caged1->pos.z, tgt ? "" : ", staying near P1");
    }
    if (nCaged != b->cagedN || cagedSum != b->cagedSum) { b->cagedN = nCaged; b->cagedSum = cagedSum; }
    if (nearCount) b->meleeNearAt = now;                                       /* m7.3: last time an enemy was in reach */
    int surrounded = (nearFront && nearBehind) || nearCount >= 3;
    /* m6.2 (m6-run1b softlock after the stage 1 sub-boss: a bot had walked past P1 to x 177.4, where the scripted
       exit of that area triggers; m5.1-run1 showed the same script with P1 in front): limits for every walk */
    s_walkLo = -1e30f; s_walkHi = 1e30f;
    if (w->haveCam) {
        float margin = tgt ? range * 0.6f : range * 1.5f;
        s_walkLo = w->cam.x - screenW * 0.5f + margin; s_walkHi = w->cam.x + screenW * 0.5f - margin;
    }
    if (w->haveWall && w->leftWallX + range * 0.4f > s_walkLo) s_walkLo = w->leftWallX + range * 0.4f;   /* m6.5: never against the left wall */
    float camLo = s_walkLo, camHi = s_walkHi; int pdir = 0;
    if (!tgt && p1) {
        pdir = fabsf_(b->p1Vx) > 0.5f ? sgn(b->p1Vx) : p1->facing;
        if (pdir >= 0) { if (anchorX + range < s_walkHi) s_walkHi = anchorX + range; }
        else { if (anchorX - range > s_walkLo) s_walkLo = anchorX - range; }
        if (s_walkLo > s_walkHi) s_walkLo = s_walkHi;
    }

    /* m6.7: when did anyone who could hurt us last come near (trap detection needs 0.7 s with nobody) */
    if (!trap_hit_plausible(w, me, screenW, lane) || slam) b->enemyNearAt = now;
    /* ---- 1. hurt ---- */
    int st = me->state;
    if (state_is_hurt(st) && !state_is_hurt(b->prevState)) {
        b->hitsTaken = now - b->lastHitAt < 1500 ? b->hitsTaken + 1 : 1; b->lastHitAt = now;
        /* m7.2 (m7.1-run1 17:23:46): fighters start some sections lying down (dropped in); not a trap */
        if (!slam && now - b->enemyNearAt > 700 && now - b->fightSince > 2500 && trap_hit_plausible(w, me, screenW, lane)) tz_add(b, w, me, now);   /* m6.7: floor trap */
        if (b->lastJumpMove && now - b->lastJumpMove < (b->jumpMoveKind == 34 ? 1500u : 800u) && (b->jumpMoveKind != 34 || now - b->lastJumpMove > 600)) {   /* m6: hit out of our own jump attack */
            if (now - b->airFailWindow > 20000) { b->airFailWindow = now; b->airFails = 0; }
            b->airFails++; b->antiAirUid = b->targetUid; b->antiAirUntil = now + 30000;
            if (b->airFails >= 2) b->jumpBanUntil = now + 20000;
            note(b, D_ANTIAIR, "hit %d ms into a jump move (uid %u anti-air for 30 s%s)", (int)(now - b->lastJumpMove), b->targetUid, b->airFails >= 2 ? "; no jump attacks for 20 s" : "");
            b->lastJumpMove = 0;
        }
    }
    if (st == CS_AIRLIGHTHIT && b->prevState == CS_AIRLIGHTHIT && me->hitStun > 0 && b->lastHitAt + 300 < now) { /* still juggled */ }
    b->prevState = st;
    if (me->grabbedBy) {
        if (b->grabbedByUid != me->grabbedBy) { b->grabbedByUid = me->grabbedBy; b->grabbedSince = now; b->grabEscapeCounted = 0; }
        seq_clear(b); set_mode(b, M_DEFEND, now, "held by an enemy");
        if (star_ok(b, stars, now) && lifePct < 30) {
            do_star(b, now); note(b, D_STAR, "emergency: held by uid %u at %.0f%% life", me->grabbedBy, lifePct); seq_run(b, now, o); WHY("held: star move"); return;
        }
        if (now - b->grabbedSince > 250 && special_ok(b, 1, lifePct, now) && (nearCount >= 2 || lifePct >= 70)) {
            GO(9, 1, 0, { 90, 0, 0, SB_Y }, { 300, 0, 0, 0 });
            used_special(b, lifePct, banked, now); note(b, D_SPECIALRECOVER, "held by uid %u, life %.0f%%, %d enemies near", me->grabbedBy, lifePct, nearCount);
            seq_run(b, now, o); WHY("held: special recovery (Y)"); return;
        }
        /* mash: stick left/right every 50 ms and the attack button */
        o->axis[AX_LX] = (int16_t)(((now / 50) % 2 ? 1 : -1) * STICK);
        o->btn[prof_btn(b, 0, C.attackButton)] = (uint8_t)((now / 60) % 2);
        if (!b->grabEscapeCounted) { b->grabEscapeCounted = 1; note(b, D_GRABESCAPE, "held by uid %u: mashing", me->grabbedBy); }
        WHY("held by uid %u: mashing to break free", me->grabbedBy); return;
    }
    b->grabbedByUid = 0;
    if (st == CS_AIRHIT || st == CS_AIRLIGHTHIT) {
        if (!b->airHitSince) { b->airHitSince = now; b->quickRecCounted = 0; }
        seq_clear(b); set_mode(b, M_DEFEND, now, "knocked into the air");
        int juggled = b->hitsTaken >= 3 && nearCount >= 1;
        if (juggled && special_ok(b, 1, lifePct, now)) {
            GO(9, 1, 0, { 90, 0, 0, SB_Y }, { 300, 0, 0, 0 });
            used_special(b, lifePct, banked, now); note(b, D_SPECIALRECOVER, "juggled (%d hits in a row), life %.0f%%", b->hitsTaken, lifePct);
            seq_run(b, now, o); WHY("juggled: special recovery (Y)"); return;
        }
        if (juggled && star_ok(b, stars, now) && lifePct < 25 && nearCount >= 2) {
            do_star(b, now); note(b, D_STAR, "emergency: juggled at %.0f%% life", lifePct); seq_run(b, now, o); WHY("juggled: star move"); return;
        }
        if (!me->noQuickRecover) {
            /* the quick recovery is A right when we touch the ground; the game buffers the press a little, so tap
               A every 120 ms while falling (A does nothing else while we are knocked into the air) */
            o->btn[prof_btn(b, 2, C.jumpButton)] = (uint8_t)(((now - b->airHitSince) % 120) < 60);
            if (!b->quickRecCounted) { b->quickRecCounted = 1; note(b, D_QUICKRECOVER, "knocked down (%s): tapping A for the quick recovery", state_name(st)); }
            WHY("knocked into the air: tapping A to recover on landing"); return;
        }
        WHY("knocked into the air (no quick recovery possible)"); return;
    }
    b->airHitSince = 0;
    if (state_is_hurt(st)) {
        seq_clear(b);
        if (st == CS_STANDHIT && b->hitsTaken >= 2 && nearCount >= 2 && special_ok(b, 1, lifePct, now)) {
            GO(9, 1, 0, { 90, 0, 0, SB_Y }, { 300, 0, 0, 0 });
            used_special(b, lifePct, banked, now); note(b, D_SPECIALRECOVER, "hit %d times in a row with %d enemies near, life %.0f%%", b->hitsTaken, nearCount, lifePct);
            seq_run(b, now, o); WHY("hit repeatedly: special recovery (Y)"); return;
        }
        WHY("idle: we are %s", state_name(st)); set_mode(b, M_HURT, now, state_name(st)); return;
    }

    /* m6.7: standing in a live trap zone: step out (the zone moves every walk target out of it as well) */
    { const TrapZone *t = s_ntz ? tz_at(me->pos.x, me->pos.z, 0.05f, now) : NULL;
      if (t && (!seq_busy(b) || b->seq.interruptible)) {
        float ex = me->pos.x, ez = me->pos.z; tz_exit(t, me->pos.x, me->pos.z, 0.4f, &ex, &ez);
        seq_clear(b); walk_to(b, me, ex, ez, 0.05f, 0.05f, o);
        if (now - b->trapNoteAt > 3000) { b->trapNoteAt = now; LOGI("%s: in a trap zone at (%.1f, %.1f): stepping out to (%.1f, %.1f)", b->name, me->pos.x, me->pos.z, ex, ez); }
        set_mode(b, M_DEFEND, now, "trap zone"); WHY("in a trap zone (x %.1f..%.1f z %.1f..%.1f): stepping out to (%.1f, %.1f)", t->x0, t->x1, t->z0, t->z1, ex, ez); return;
      } }
    /* ---- 2. threats ---- */
    const Proj *threat = NULL; float tBest = 1e30f;
    for (int i = 0; i < w->npj; i++) {
        const Proj *p = &w->pj[i];
        if (p->team == me->team) continue;
        float dx = me->pos.x - p->pos.x;
        if (fabsf_(p->vel.x) < 1e-4f || sgn(p->vel.x) != sgn(dx)) continue;          /* not moving toward us */
        if (fabsf_(me->pos.z - p->pos.z) > lane * 1.2f || fabsf_(dx) > range * 4.0f) continue;
        float t = fabsf_(dx) / fabsf_(p->vel.x);                                       /* reads (~frames) until it arrives */
        if (t < 18.0f && t < tBest) { tBest = t; threat = p; }
    }
    int canInterrupt = !seq_busy(b) || b->seq.interruptible;
    /* m6.1: the nearest standing enemy in front within air reach (for X during a defensive or stuck jump) */
    int airDir = 0; { float ab = 1e30f;
        for (int i = 0; i < w->nch; i++) { const Chr *c = &w->ch[i]; if (!is_enemy_of(c, me) || state_is_down(c->state)) continue;
            float ddx = c->pos.x - me->pos.x;
            if (fabsf_(ddx) < range * 1.3f && fabsf_(c->pos.z - me->pos.z) < lane * 1.2f && fabsf_(ddx) < ab) { ab = fabsf_(ddx); airDir = sgn(ddx) ? sgn(ddx) : me->facing; } } }
    /* ---- 2b. m6.6: jump slam (video: wind-up 0.2 s, ~1.1 s up, lands on a fixed shadow, shockwave ~ one character
       height). Leave a 2.5+ circle around its x/z, never walk into it, no attacks on it while it is up; after it lands
       it is the preferred target for 1.5 s. Capped at 2.5 s per jump; ordinary hops (Jump, y <= 2.3) don't count. */
    /* m7.5: inside a robot leg's stab footprint: drop everything and get out (shortest way, room permitting) */
    if (s_stabIn && !state_is_hurt(me->state)) {
        seq_clear(b);
        float cx = (s_stabZ.x0 + s_stabZ.x1) * 0.5f, ex = me->pos.x >= cx ? s_stabZ.x1 + 0.4f : s_stabZ.x0 - 0.4f;
        if (ex < s_walkLo || ex > s_walkHi) ex = me->pos.x >= cx ? s_stabZ.x0 - 0.4f : s_stabZ.x1 + 0.4f;
        float ez = me->pos.z <= (s_stabZ.z0 + s_stabZ.z1) * 0.5f ? s_stabZ.z0 - 0.3f : s_stabZ.z1 + 0.3f;
        int byZ = fabsf_(ez - me->pos.z) * 2.0f < fabsf_(ex - me->pos.x) && (!s_fl.known || (ez >= s_fl.lo - 0.1f && ez <= s_fl.hi + 0.1f));
        int sx = byZ ? 0 : (ex > me->pos.x ? 1 : -1), sz = byZ ? (ez > me->pos.z ? 1 : -1) : 0;
        o->axis[AX_LX] = (int16_t)(sx * b->xSign * STICK); o->axis[AX_LY] = (int16_t)(sz * b->zSign * STICK);
        set_mode(b, M_DEFEND, now, "robot leg stab");
        WHY("robot leg uid %u stabbing down: out of its footprint x %.1f..%.1f z %.1f..%.1f", s_stabZ.partUid, s_stabZ.x0, s_stabZ.x1, s_stabZ.z0, s_stabZ.z1); return;
    }
    if (slam && slam->uid != b->slamUid) { b->slamUid = slam->uid; b->slamSince = now; }
    if (slam && now - b->slamSince < 2500) {
        float R = range * 2.2f < 2.5f ? 2.5f : range * 2.2f, Re = R + 0.8f;
        s_slamOn = 1; s_slamX = slam->pos.x; s_slamZ = slam->pos.z; s_slamR = Re;
        float ddx = me->pos.x - slam->pos.x, ddz = me->pos.z - slam->pos.z, d = sqrtf(ddx * ddx + ddz * ddz);
        if (d < Re && !state_is_hurt(me->state)) {
            seq_clear(b);
            float ux, uz;
            if (d < 0.05f) { ux = (b->index & 1) ? 1.0f : -1.0f; uz = 0; } else { ux = ddx / d; uz = ddz / d; }
            if (fabsf_(ux) < 0.5f) { ux = ux >= 0 ? 0.8f : -0.8f; uz = uz >= 0 ? 0.6f : -0.6f; }   /* mostly sideways: more room in x than in depth */
            float tx = slam->pos.x + ux * Re * 1.15f, tz = slam->pos.z + uz * Re * 1.15f;
            if (tx < s_walkLo || tx > s_walkHi) {                                  /* no room on this side: the other side */
                float t2 = slam->pos.x - ux * Re * 1.15f;
                if (t2 >= s_walkLo && t2 <= s_walkHi && fabsf_(ddx) < Re * 0.5f) tx = t2; else { tx = me->pos.x; tz = slam->pos.z + (uz >= 0 ? 1.0f : -1.0f) * Re * 1.15f; }
            }
            s_slamOn = 0; walk_to(b, me, tx, tz, range * 0.1f, lane * 0.3f, o); s_slamOn = 1;
            if (!o->axis[AX_LX] && !o->axis[AX_LY]) o->axis[AX_LX] = (int16_t)((ddx >= 0 ? 1 : -1) * b->xSign * STICK);
            if (b->slamNoted != slam->uid) { b->slamNoted = slam->uid; note(b, D_SLAMEVADE, "uid %u up in a jump attack (y %.1f, move %d): leaving its landing spot (%.1f, %.1f), we were %.1f from it", slam->uid, slam->pos.y, slam->moveId, slam->pos.x, slam->pos.z, d); }
            set_mode(b, M_DEFEND, now, "jump slam");
            WHY("uid %u in the air (y %.1f): leaving its landing spot (%.1f, %.1f), d %.1f", slam->uid, slam->pos.y, slam->pos.x, slam->pos.z, d); return;
        }
        if (tgt == slam) {                                                         /* outside: wait for the landing, facing it */
            int f = sgn(slam->pos.x - me->pos.x); if (f && me->facing != f && d > Re + 0.3f) o->axis[AX_LX] = (int16_t)(f * b->xSign * STICK / 3);
            WHY("waiting for uid %u to land (y %.1f), %.1f from its landing spot", slam->uid, slam->pos.y, d); return;
        }
    } else if (!slam && b->slamUid) { b->punishUid = b->slamUid; b->punishUntil = now + 1500; b->slamUid = 0; b->slamNoted = 0; }

    if (threat && canInterrupt) {
        if (nearCount >= 2 && special_ok(b, 1, lifePct, now)) {
            GO(10, 1, 0, { 90, 0, 0, SB_Y }, { 300, 0, 0, 0 }); used_special(b, lifePct, banked, now); b->lastDefSpecial = now;
            note(b, D_DEFSPECIAL, "%s arriving in ~%.0f frames with %d enemies near", threat->kind ? "thrown object" : "projectile", tBest, nearCount);
            seq_run(b, now, o); set_mode(b, M_DEFEND, now, "projectile"); WHY("defensive special against a projectile"); return;
        }
        if (dodge_ok(b, now)) {
            do_dodge(b, now, airDir); note(b, D_DODGEJUMP, "%s uid %u arriving in ~%.0f frames", threat->kind ? "thrown object" : "projectile", threat->uid, tBest);
            seq_run(b, now, o); set_mode(b, M_DEFEND, now, "projectile"); WHY("neutral jump over a projectile"); return;
        }
    }
    /* m6.7: a shield cop (shield up) swinging at us from the front: get out of its reach instead of trading */
    if (incoming && canInterrupt && (incoming->shieldUser || incoming->uid == b->shieldUid) && !incoming->staminaBroken && !incoming->shieldMove &&
        (incoming->aiAttacking || incoming->state == CS_DASH)) {
        int sd = sgn(incoming->pos.x - me->pos.x); if (!sd) sd = me->facing ? me->facing : 1;
        seq_clear(b); b->lastShBackoff = now;
        walk_to(b, me, me->pos.x - sd * range * 2.4f, me->pos.z + (incoming->pos.z > me->pos.z ? -1.0f : 1.0f) * lane * 1.5f, range * 0.1f, lane * 0.3f, o);
        if (b->shTacUid != incoming->uid || b->shTacKind != 1 || now - b->shTacAt > 4000) { b->shTacUid = incoming->uid; b->shTacKind = 1; b->shTacAt = now;
            note(b, D_SHIELDTAC, "uid %u attacks (move %d) while we are in front of its shield: backing off", incoming->uid, incoming->moveId); }
        set_mode(b, M_DEFEND, now, "shield cop attacks"); WHY("shield cop uid %u attacking (move %d): backing off", incoming->uid, incoming->moveId); return;
    }
    if (incoming && canInterrupt) {
        int dangerous = incoming->state == CS_DASH || incoming->superArmor || incoming->boss;
        int recentHit = b->lastHitAt && now - b->lastHitAt < 2000;
        float adx = fabsf_(incoming->pos.x - me->pos.x);
        if (surrounded && special_ok(b, 1, lifePct, now) && now - b->lastDefSpecial > 3000) {
            GO(10, 1, 0, { 90, 0, 0, SB_Y }, { 300, 0, 0, 0 }); used_special(b, lifePct, banked, now); b->lastDefSpecial = now;
            note(b, D_DEFSPECIAL, "surrounded (%d near, %d behind) and uid %u is attacking", nearCount, nearBehind, incoming->uid);
            seq_run(b, now, o); set_mode(b, M_DEFEND, now, "surrounded"); WHY("defensive special: surrounded"); return;
        }
        if ((dangerous || recentHit || adx > range) && dodge_ok(b, now)) {
            do_dodge(b, now, airDir); note(b, D_DODGEJUMP, "uid %u %s toward us (dx %.1f)%s", incoming->uid, incoming->state == CS_DASH ? "dashes" : "attacks",
                                   incoming->pos.x - me->pos.x, dangerous ? ", can't be interrupted" : recentHit ? ", we were just hit" : "");
            seq_run(b, now, o); set_mode(b, M_DEFEND, now, "incoming attack"); WHY("neutral jump: attack incoming"); return;
        }
    }
    if (surrounded && nearCount >= 3 && canInterrupt && special_ok(b, 1, lifePct, now) && now - b->lastDefSpecial > 6000) {
        GO(10, 1, 0, { 90, 0, 0, SB_Y }, { 300, 0, 0, 0 }); used_special(b, lifePct, banked, now); b->lastDefSpecial = now;
        note(b, D_DEFSPECIAL, "surrounded by %d enemies (%d behind)", nearCount, nearBehind);
        seq_run(b, now, o); set_mode(b, M_DEFEND, now, "surrounded"); WHY("defensive special: surrounded"); return;
    }

    /* ---- 3. a move in progress ---- */
    if (seq_busy(b) && b->breakUid && b->seq.kind == 29) b->breakClaimAt = now;   /* m6.4: still swinging at our breakable: keep the claim */
    if (seq_busy(b) && seq_run(b, now, o)) { WHY("move in progress (%d)", b->seq.kind); return; }

    /* ---- 4. star move ---- */
    if (star_ok(b, stars, now)) {
        const char *why = NULL;
        if (bossNear && fabsf_(bossNear->pos.z - me->pos.z) < lane * 1.5f) why = "boss close";
        else if (nearCount >= 4) why = "big crowd";
        else if (lifePct < 30 && nearCount >= 2) why = "emergency";
        if (why) { do_star(b, now); note(b, D_STAR, "%s (%d near, %d star(s), life %.0f%%)", why, nearCount, stars, lifePct); seq_run(b, now, o); WHY("star move: %s", why); return; }
    }

    /* ---- 5. holding an enemy ---- */
    if (me->grabbing > 0 && (!b->grabStart || now - b->grabStart < 4000)) {
        int dir = me->facing ? me->facing : 1, front = 0, back = 0;
        if (!b->grabStart) { b->grabStart = now; b->grabHits = 0; b->grabTarget = me->grabUid; b->doubleGrabUntil = 0; note(b, D_GRAB, "holding uid %u", me->grabUid); }
        const Chr *other = NULL; float oBest = 1e30f;
        for (int i = 0; i < w->nch; i++) {
            const Chr *c = &w->ch[i];
            if (!is_enemy_of(c, me) || c->uid == me->grabUid || state_is_down(c->state)) continue;
            float dx = c->pos.x - me->pos.x, adx = fabsf_(dx), adz = fabsf_(c->pos.z - me->pos.z);
            if (adx < screenW * 0.4f && adz < lane * 3) { if (sgn(dx) == dir) front++; else back++; }
            if (adx < range * 2.0f && adz < lane * 1.5f && adx + adz < oBest) { oBest = adx + adz; other = c; }
        }
        set_mode(b, M_GRAB, now, NULL);
        if (incoming && sgn(incoming->pos.x - me->pos.x) == -dir && pf->grabStyle != GS_MAX) {
            GO(11, dir, 0, { 80, 0, 0, SB_A }, { 450, 0, 0, 0 }); note(b, D_VAULT, "attacked from behind while holding uid %u", me->grabUid);
            seq_run(b, now, o); WHY("vault over the held enemy"); return;
        }
        if (pf->grabStyle == GS_FLOYD && me->grabbing == 1 && other && now - b->grabStart < 1500) {
            if (!b->doubleGrabUntil) { b->doubleGrabUntil = now + 1200; note(b, D_DOUBLEGRAB, "walking into uid %u while holding uid %u", other->uid, me->grabUid); }
            if (now < b->doubleGrabUntil) { walk_to(b, me, other->pos.x, other->pos.z, 0.0f, lane * 0.3f, o); WHY("double grab: walking into uid %u", other->uid); return; }
        }
        if (b->grabHits < 2) {
            b->grabHits++; GO(12, dir, 0, { 70, 0, 0, SB_X }, { 160, 0, 0, 0 });
            note(b, D_GRABHIT, "hit %d on held uid %u", b->grabHits, me->grabUid); seq_run(b, now, o); WHY("grab: knee"); return;
        }
        /* finisher: throw behind if more enemies (or a wall / the screen edge) are there, else knock it forward */
        float edge = dir > 0 ? (w->haveCam ? w->cam.x - screenW * 0.5f : -1e9f) : (w->haveCam ? w->cam.x + screenW * 0.5f : 1e9f);
        if (dir > 0 && w->haveWall && w->leftWallX > edge) edge = w->leftWallX;
        int wallBehind = fabsf_(me->pos.x - edge) < range * 2.0f;
        if (back > front || wallBehind) {
            GO(13, dir, 0, { 80, BK, 0, SB_X }, { 550, 0, 0, 0 });
            note(b, D_THROW, "uid %u over the shoulder toward %s (%d behind, %d in front)", me->grabUid, back > front ? "the bigger group" : "the wall", back, front);
            seq_run(b, now, o); WHY("grab: throw behind"); return;
        }
        if (pf->grabStyle == GS_MAX) {
            GO(14, dir, 0, { 80, 0, 0, SB_A }, { 750, 0, 0, 0 }); note(b, D_GRABSPECIAL, "piledriver on uid %u", me->grabUid);
            seq_run(b, now, o); WHY("grab: jump (piledriver)"); return;
        }
        if (pf->grabStyle == GS_FLOYD && special_ok(b, 0, lifePct, now)) {
            GO(14, dir, 0, { 80, 0, 0, SB_Y }, { 650, 0, 0, 0 }); used_special(b, lifePct, banked, now);
            note(b, D_GRABSPECIAL, "grab special on uid %u (life %.0f%%)", me->grabUid, lifePct); seq_run(b, now, o); WHY("grab: special"); return;
        }
        GO(12, dir, 0, { 70, 0, 0, SB_X }, { 450, 0, 0, 0 });
        note(b, D_GRABHIT, "finisher on uid %u toward %d enemies in front", me->grabUid, front); seq_run(b, now, o); WHY("grab: finisher"); return;
    }
    if (!me->grabbing) b->grabStart = 0;

    /* ---- 6. pickups ---- */
    if (C.usePickups) {
        const Pick *goal = NULL; float gBest = 1e30f; int gKind = 0;
        for (int i = 0; i < w->npk; i++) {
            const Pick *p = &w->pk[i];
            uint64_t age = pick_age(p->addr, now);            /* m5: the human's head start on money and weapons */
            if (p->held || is_bad_pick(b, p->addr, now) || unreach_now(b, pick_key(p->addr), now) || fabsf_(p->pos.x - anchorX) > screenW * 0.55f || p->pos.y > sizeY * 0.1f) continue;
            if (p->kind == PK_FOOD || (p->kind == PK_GRENADE && p->ticking)) { if (p->pos.x < camLo - range * 0.3f || p->pos.x > camHi + range * 0.3f) continue; }   /* m6.2: food: camera limits only */
            else if (p->pos.x < s_walkLo - range * 0.2f || p->pos.x > s_walkHi + range * 0.2f) continue;   /* m6.2: money/weapons not ahead of P1; m6.3: must be reachable inside the walk limits */
            else if (tgt) {   /* m6.3 (m6.2-run1c, sub-boss): an enemy is engaged: no weapons/money at the screen edge or ahead of P1 (away from the fight) */
                if (w->haveCam && (p->pos.x < w->cam.x - screenW * 0.5f + range * 1.5f || p->pos.x > w->cam.x + screenW * 0.5f - range * 1.5f)) continue;
                if (p1) { int away = sgn(anchorX - tgt->pos.x); if (away && (p->pos.x - anchorX) * away > range * 1.5f) continue; }
            }
            float dx = p->pos.x - me->pos.x, adz = fabsf_(p->pos.z - me->pos.z), d = fabsf_(dx) + 2.0f * adz;
            if (p->kind == PK_GRENADE && p->ticking) {
                if (d < range * 0.9f && !me->heldAddr) { if (-1000.0f < gBest) { gBest = -1000.0f; goal = p; gKind = PK_GRENADE; } }
                else if (d < range * 1.6f && d >= range * 0.9f) {
                    note(b, D_GRENADEAVOID, "live grenade at dx %.1f", dx);
                    walk_to(b, me, me->pos.x - sgn(dx) * range * 2.0f, me->pos.z, range * 0.2f, lane, o); set_mode(b, M_PICKUP, now, "grenade");
                    WHY("stepping away from a live grenade"); return;
                }
                continue;
            }
            if (p->kind == PK_FOOD && lifePct < (float)C.foodLifePct) {
                /* m6: only the lowest-life bot goes for food; the others leave it */
                const BotBrain *lower = NULL; float lowerPct = 0;
                for (int k = 0; k < nall; k++) {
                    const BotBrain *ob = &all[k];
                    if (ob == b || !ob->identified || ob->playerSlot < 0) continue;
                    const Chr *oc = slot_fighter(w, ob->playerSlot);
                    if (!oc || oc->life <= 0 || oc->state == CS_STANDDIE) continue;
                    float op = life_pct(oc);
                    if (op < lifePct - 0.5f || (fabsf_(op - lifePct) <= 0.5f && ob->index < b->index)) { lower = ob; lowerPct = op; }
                }
                if (lower) {
                    int known = 0; for (int k = 0; k < b->nleftFor; k++) known |= b->leftFor[k] == p->addr;
                    if (!known) { if (b->nleftFor < 8) b->leftFor[b->nleftFor++] = p->addr; else b->leftFor[0] = p->addr;
                                  note(b, D_FOODSHARE, "%s is at %.0f%% life, we are at %.0f%%", lower->name, lowerPct, lifePct); }
                    continue;
                }
                if (humanNeed < (float)C.foodLifePct && humanNeed <= lifePct + 10.0f) {
                    int known = 0; for (int k = 0; k < b->nleftFor; k++) known |= b->leftFor[k] == p->addr;
                    if (!known) { if (b->nleftFor < 8) b->leftFor[b->nleftFor++] = p->addr; else b->leftFor[0] = p->addr;
                                  note(b, D_FOODLEFT, "a human is at %.0f%% life, we are at %.0f%%", humanNeed, lifePct); }
                    continue;
                }
                int taken = 0;
                for (int k = 0; k < nall; k++) if (&all[k] != b && all[k].goalPick == p->addr) taken = 1;
                if (taken || (nearestEnemy < range * 1.5f && lifePct > 35.0f)) continue;
                if (d < gBest) { gBest = d; goal = p; gKind = PK_FOOD; }
            } else if ((p->kind == PK_SCORE || p->kind == PK_STAR) && age >= (uint64_t)C.itemHeadStartMs && nearestEnemy > range * 1.5f && d < screenW * 0.6f) {
                int taken = 0;
                for (int k = 0; k < nall; k++) if (&all[k] != b && all[k].goalPick == p->addr) taken = 1;
                if (!taken && d + range < gBest) { gBest = d + range; goal = p; gKind = p->kind; }
            } else if (p->kind == PK_WEAPON && !me->heldAddr && d < range * 4.0f && nearestEnemy > range * 1.2f && age >= (uint64_t)C.itemHeadStartMs) {
                if (d + range * 2.0f < gBest) { gBest = d + range * 2.0f; goal = p; gKind = PK_WEAPON; }
            }
        }
        /* m6.3: near a pickup for 1.5 s and it is still there (stacked or out of reach): ignore it and its stack (escalating) */
        if (goal && fabsf_(goal->pos.x - me->pos.x) < range * 0.6f && fabsf_(goal->pos.z - me->pos.z) < lane * 1.5f) {
            if (!b->pickNearAt) b->pickNearAt = now;
            else if (now - b->pickNearAt > 1500) {
                int secs = 0, n = 0;
                for (int i = 0; i < w->npk; i++) { const Pick *q = &w->pk[i];
                    if (!q->held && fabsf_(q->pos.x - goal->pos.x) < range * 0.6f && fabsf_(q->pos.z - goal->pos.z) < lane * 4.0f) { int sx = unreach_add(b, pick_key(q->addr), now, 0); if (sx > secs) secs = sx; n++; } }
                note(b, D_UNREACHABLE, "pickup at (%.1f, %.1f) still there after 1.5 s next to it (%d stacked): ignoring it for %d s", goal->pos.x, goal->pos.z, n, secs);
                b->pickNearAt = 0; b->goalPick = 0; goal = NULL;
            }
        } else b->pickNearAt = 0;
        if (goal) {
            if (goal->addr != b->goalPick) { b->goalPick = goal->addr; b->goalTries = 0; b->goalSince = now; b->goalKind = gKind; }
            if (gKind == PK_FOOD || gKind == PK_GRENADE) { s_walkLo = camLo; s_walkHi = camHi; }
            if (now - b->goalSince > 6000 || b->goalTries >= 3) { bad_pick(b, goal->addr, now); b->goalPick = 0; LOGD("%s: giving up on a pickup for 8 s", b->name); }
            else {
                float dx = goal->pos.x - me->pos.x, dz = goal->pos.z - me->pos.z;
                const char *gName = gKind == PK_FOOD ? "food" : gKind == PK_WEAPON ? "a weapon" : gKind == PK_GRENADE ? "a grenade" : gKind == PK_STAR ? "a star" : "money";
                set_mode(b, M_PICKUP, now, gName);
                if (fabsf_(dx) > range * 0.25f || fabsf_(dz) > lane * 0.5f) {
                    walk_to(b, me, goal->pos.x, goal->pos.z, range * 0.15f, lane * 0.35f, o);
                    WHY("going for %s at dx %.1f dz %.1f", gName, dx, dz); return;
                }
                if (now - b->lastPickPress > 600) {
                    b->lastPickPress = now; b->goalTries++;
                    if (gKind == PK_GRENADE) {
                        int toward = tgt ? sgn(tgt->pos.x - me->pos.x) : me->facing; if (!toward) toward = 1;
                        GO(15, toward, 0, { 80, 0, 0, SB_B }, { 160, 0, 0, 0 }, { 120, F, 0, 0 }, { 80, F, 0, SB_B }, { 300, 0, 0, 0 });
                        note(b, D_GRENADEBACK, "picked up a live grenade, throwing it %s", toward > 0 ? "right" : "left");
                    } else {
                        GO(16, 1, 0, { 80, 0, 0, SB_B }, { 250, 0, 0, 0 });
                        if (gKind == PK_SCORE || gKind == PK_STAR) note(b, D_MONEY, "%s (score %d) after the %d s head start", gName, goal->scoreGain, C.itemHeadStartMs / 1000);
                        else if (gKind == PK_FOOD) note(b, D_FOOD, "life %.0f%% (+%.0f), humans at %.0f%%", lifePct, goal->lifeGain, humanNeed > 999 ? 100.0f : humanNeed);
                        else { note(b, D_WEAPONPICK, "%s", goal->onlyThrow ? "a throwing weapon" : "a weapon"); b->heldOnlyThrow = goal->onlyThrow; }
                    }
                    seq_run(b, now, o); WHY("picking up (B)"); return;
                }
                WHY("on the pickup, waiting"); return;
            }
        } else b->goalPick = 0;
    }

    /* ---- 7. retreat at low life ---- */
    int wantRetreat = lifePct < (float)C.retreatLifePct && p1 && now >= b->retreatCooldownUntil;
    if (wantRetreat && !b->retreatSince) { b->retreatSince = now; note(b, D_RETREAT, "life %.0f%% < %d%%", lifePct, C.retreatLifePct); }
    if (!wantRetreat) b->retreatSince = 0;
    if (wantRetreat && C.retreatMaxMs && now - b->retreatSince > (uint64_t)C.retreatMaxMs) {
        b->retreatSince = 0; b->retreatCooldownUntil = now + 5000; wantRetreat = 0;
        LOGD("%s: retreated %d ms; fighting again for 5 s", b->name, C.retreatMaxMs);
    }
    if (wantRetreat) {
        float side = tgt ? (float)sgn(tgt->pos.x - p1->pos.x) : (float)-p1->facing;
        if (side == 0) side = 1;
        /* m6.7 (m6.6-run1b 15:38:34, Axel at 24% between uid 20 and uid 21): the nearest enemy switched sides as
           he walked, and so did the retreat spot: keep the side for 1.5 s */
        if (b->retreatSideK && now - b->retreatSideAt < 1500) side = (float)b->retreatSideK;
        else { b->retreatSideK = (int)side; b->retreatSideAt = now; }
        WHY("retreat: life %.0f%% < %d%%, P1 uid %u", lifePct, C.retreatLifePct, p1->uid);
        set_mode(b, M_RETREAT, now, NULL);
        walk_to(b, me, p1->pos.x - side * range * 1.5f, p1->pos.z, range * 0.3f, lane, o);
        if (tgt && fabsf_(tgt->pos.x - me->pos.x) < range * 0.7f && fabsf_(tgt->pos.z - me->pos.z) < lane && (now / 120) % 3 == 0)
            o->btn[prof_btn(b, 0, C.attackButton)] = 1;          /* cornered: swing back */
        return;
    }

    /* ---- m6: make way for P1 ----
       Bodies collide: a bot standing in P1's path blocks P1 (video: P1 stuck 10 s behind a wall of three bots).
       When P1 walks (or pushes the stick) toward us within about a body width on our lane, step aside in depth,
       away from the other bots, and let P1 pass. Not while we are trading hits with an enemy right here. */
    int busyFighting = tgt && fabsf_(tgt->pos.x - me->pos.x) < range * 2.0f && fabsf_(tgt->pos.z - me->pos.z) < lane * 3.0f;
    if (p1 && !busyFighting) {
        float pdx = me->pos.x - p1->pos.x, pdz = me->pos.z - p1->pos.z;
        int intent = fabsf_(p1->stickX) > 0.3f ? sgn(p1->stickX) : fabsf_(b->p1Vx) > 0.5f ? sgn(b->p1Vx) : 0;
        float body = range * 0.9f;
        if (intent && intent == sgn(pdx) && fabsf_(pdx) < body * 1.6f && fabsf_(pdz) < lane * 2.5f && !state_is_hurt(p1->state)) {
            if (now >= b->yieldUntil) {
                int dirZ = 0;
                if (fabsf_(pdz) > lane * 0.5f) dirZ = sgn(pdz);
                else {
                    float push = 0;
                    for (int k = 0; k < nall; k++) {
                        const BotBrain *ob = &all[k];
                        if (ob == b || !ob->identified || ob->playerSlot < 0) continue;
                        const Chr *oc = slot_fighter(w, ob->playerSlot);
                        if (oc && fabsf_(oc->pos.x - me->pos.x) < follow) push += oc->pos.z > me->pos.z ? -1.0f : 1.0f;
                    }
                    dirZ = push > 0 ? 1 : push < 0 ? -1 : (b->index % 2 ? -1 : 1);
                }
                b->yieldZ = p1->pos.z + dirZ * lane * 4.5f;
                note(b, D_YIELD, "P1 uid %u coming at us (dx %.1f, dz %.1f, stick %.2f, vel %.2f): stepping %s", p1->uid, -pdx, -pdz, p1->stickX, b->p1Vx, dirZ > 0 ? "up-screen (+z)" : "down-screen (-z)");
            }
            b->yieldUntil = now + 700;
        }
        if (now < b->yieldUntil) {
            seq_clear(b); set_mode(b, M_FOLLOW, now, "making way for P1");
            walk_to(b, me, me->pos.x, b->yieldZ, 1e9f, lane * 0.4f, o);
            WHY("making way for P1: stepping to z %.1f", b->yieldZ); return;
        }
    }

    /* ---- 8. target ---- */
    if (!tgt && C.breakObjects && w->nbr > 0) {      /* m5: nothing to fight: break sinks, crates, phone booths... */
        const Brk *bt = NULL; float bb = 1e30f;
        for (int i = 0; i < w->nbr; i++) {
            const Brk *r = &w->br[i];
            if (r->life <= 0 || unreach_now(b, r->uid, now)) continue;   /* m5.1: 'entered' flips 0/1 on real objects; not used */
            if (r->explosive == 1 || r->blowTimer > 0) {                 /* m7.2: never break an explosive barrel */
                {   /* m7.3: logged once per barrel (m7.2 counted every switch between two barrels: 10384 'decisions') */
                    int seen = 0; for (int q = 0; q < 8; q++) if (b->barrelSeen[q] == r->uid) seen = 1;
                    if (!seen) { b->barrelSeen[b->nBarrelSeen++ & 7] = r->uid; b->dec[D_BARREL]++; b->decTotal[D_BARREL]++;
                                 LOGI("%s: explosive barrel uid %u \"%s\" at (%.1f, %.1f): not broken for items", b->name, r->uid, r->name[0] ? r->name : "?", r->pos.x, r->pos.z); } }
                continue;
            }
            if (r->pos.x < camLo - range || r->pos.x > camHi + range) continue;          /* m6.2: at the screen edge */
            if (fabsf_(r->pos.x - anchorX) > screenW * 0.45f || (w->haveCam && fabsf_(r->pos.x - w->cam.x) > screenW * 0.45f)) continue;
            /* m6.2: one bot per breakable: skip one another bot holds (claim refreshed every frame it works on it), and
               leave it to a free bot that is clearly closer */
            int other = 0; float myD = fabsf_(r->pos.x - me->pos.x) + 2.0f * fabsf_(r->pos.z - me->pos.z);
            for (int k = 0; k < nall; k++) {
                const BotBrain *ob = &all[k];
                if (ob == b || !ob->identified || ob->playerSlot < 0) continue;
                if (ob->breakUid == r->uid && ob->breakClaimAt && now - ob->breakClaimAt < 500) { other = 1; break; }
                if (r->uid != b->breakUid && !ob->targetUid && (!ob->breakUid || now - ob->breakClaimAt >= 500)) {
                    const Chr *oc = slot_fighter(w, ob->playerSlot);
                    if (oc && fabsf_(r->pos.x - oc->pos.x) + 2.0f * fabsf_(r->pos.z - oc->pos.z) + range < myD) { other = 1; break; }
                }
            }
            if (other) continue;
            float d = myD + (r->uid == b->breakUid ? -range : 0);
            if (d < bb) { bb = d; bt = r; }
        }
        if (!bt) b->breakUid = 0;
        if (bt) {
            b->breakClaimAt = now;
            s_walkLo = camLo; s_walkHi = camHi;   /* m6.3: any breakable on screen (the m6.2 "not ahead of P1" cap stopped the bots short) */
            /* m6.1: line up on the object's depth lane when we can, but never insist on it: the m6 live log had a
               trash can at z 0.0 on the front edge of the floor, the bots could not get below z 0.1-0.3, waited for
               |dz| <= 0.11 forever, flipped sides, went around, and never swung. Now: horizontally next to it and
               within a generous depth (or blocked in depth / 1.5 s of lining up) -> face it and swing. No damage after
               a string -> one depth step toward its lane (twice), then closer, then the other side; 5 misses or 12 s
               without damage -> leave it (4/15/60 s). */
            float dx = bt->pos.x - me->pos.x, dz = bt->pos.z - me->pos.z, adz = fabsf_(dz);
            if (bt->uid != b->breakUid) {
                b->breakUid = bt->uid; b->breakSide = sgn(me->pos.x - bt->pos.x) ? sgn(me->pos.x - bt->pos.x) : 1;
                b->breakFails = 0; b->breakStand = clampf_(range * 1.6f, 1.8f, 2.2f); b->breakSwingSince = 0; b->breakAlignSince = 0; b->breakZOff = 0; b->breakXAt = 0;
                b->breakStart = now; b->breakLastHit = now; b->breakInPos = 0; b->breakZBlocked = 0; b->breakZAt = 0; b->breakNudgeUntil = 0;
                b->breakLife = bt->life; b->progressPos = me->pos; b->lastProgress = now;
                note(b, D_BREAK, "uid %u at dx %.1f dz %.1f (life %.0f)", bt->uid, dx, dz, bt->life);
            }
            b->targetUid = 0; set_mode(b, M_ATTACK, now, "breakable");
            if (bt->life < b->breakLife) { b->breakLife = bt->life; b->breakLastHit = now; b->breakSwingSince = now; b->breakFails = 0; }   /* hits land */
            if (now - b->breakLastHit > 12000) {
                int secs = unreach_add(b, bt->uid, now, 0); note(b, D_UNREACHABLE, "breakable uid %u: no damage in 12 s, ignoring it for %d s", bt->uid, secs);
                b->breakUid = 0; WHY("breakable: giving up"); return;
            }
            /* m6.4 (m6.3-run1, barrel uid 6 at (4.6, 0.1)): the body deflects a walker in depth, so walking in "on its
               lane" from 1.7 ended at (3.6, 0.7): swing at dx -1.0 dz -0.55 missed, then the bot flipped between stepping
               out / stepping in / lining up every frame. P1 broke one from dx -2.2, dz 0.13. So, a 3-phase approach with
               hysteresis: 0) line up the depth FIRST at >= 2.2 out (back straight out if closer; no x moves near it until
               |dz| < 0.15); 1) walk straight in along x only (ly 0) to the swing distance (1.6 x attack range, clamped
               1.8-2.2; leave this phase only if |dz| grows past 0.3); 2) face it and swing. A miss: 0.5 closer (back out
               and line up again first if the depth is off by > 0.08); 4th miss: the other side; 5 misses or 12 s without damage: leave it (4/15/60 s). */
            float tz = bt->pos.z + b->breakZOff;
            float axo = fabsf_(me->pos.x - bt->pos.x);
            float alignX = b->breakStand + 0.4f; if (alignX < 2.2f) alignX = 2.2f;
            float standX = bt->pos.x + b->breakSide * alignX;   /* m6.5: the side we work from must be inside the walk limits */
            if (standX < s_walkLo || standX > s_walkHi) {
                float o2 = bt->pos.x - b->breakSide * alignX;
                if (o2 >= s_walkLo && o2 <= s_walkHi) { b->breakSide = -b->breakSide; standX = o2; b->breakInPos = 0; }
            }
            int onSide = sgn(me->pos.x - bt->pos.x) == b->breakSide, toward = -b->breakSide;
            float az = fabsf_(tz - me->pos.z);
            int ph = b->breakInPos;
            if (ph && !onSide) ph = 0;
            if (ph >= 1 && az > 0.30f && (!b->breakZBlocked || fabsf_(me->pos.z - b->breakZLim) > 0.3f)) { ph = 0; b->breakSwingSince = 0; b->breakZBlocked = 0; }
            if (ph == 2 && axo > b->breakStand + 0.6f) { ph = 1; b->breakSwingSince = 0; }
            b->breakInPos = ph;
            if (ph < 2) {
                b->breakSwingSince = 0;
                if (fabsf_(me->pos.x - b->progressPos.x) + fabsf_(me->pos.z - b->progressPos.z) > range * 0.1f) { b->progressPos = me->pos; b->lastProgress = now; }
                if (now - b->lastProgress > 2500) {
                    b->lastProgress = now; b->breakFails++; b->breakInPos = 0; b->breakZBlocked = 0;
                    if (b->breakFails >= 5) { int secs = unreach_add(b, bt->uid, now, 0); note(b, D_UNREACHABLE, "breakable uid %u: can't get next to it, ignoring it for %d s", bt->uid, secs); b->breakUid = 0; WHY("breakable out of reach"); return; }
                    float o2 = bt->pos.x - b->breakSide * alignX;
                    if (o2 >= s_walkLo && o2 <= s_walkHi) { b->breakSide = -b->breakSide; note(b, D_BREAKMISS, "uid %u: stuck getting next to it, trying the %s side", bt->uid, b->breakSide > 0 ? "right" : "left"); }
                    else note(b, D_BREAKMISS, "uid %u: stuck getting next to it (the other side is past the wall / screen edge)", bt->uid);
                    WHY("breakable: other side"); return;
                }
                if (!onSide && adz < lane * 5.0f && axo < alignX * 1.5f) {        /* go around the object (off its lane first) */
                    walk_to(b, me, me->pos.x, bt->pos.z + (dz > 0 ? -1.0f : 1.0f) * lane * 6.0f, 1e9f, lane * 0.5f, o);
                    WHY("breakable uid %u: going around it to the %s side", bt->uid, b->breakSide > 0 ? "right" : "left"); return;
                }
            }
            if (ph == 0) {
                /* m6.5 (m6.4-run1b, cans at the back wall): the can's lane can be past the reachable depth. No z progress
                   for 0.3 s -> that's the depth limit: use it (P1 breaks these from the normal floor). Far away: walk
                   toward our stand point (never away), keep x still near it. */
                if (onSide && (az < 0.06f || (b->breakZBlocked && axo <= alignX + 0.5f))) {   /* hysteresis: in at 0.06, out past 0.3; m6.7: only from our side */
                    b->breakInPos = ph = 1; b->breakXAt = 0;   /* on its lane (even if already close): only x moves from here */
                }
                if (ph == 0) {
                    if (axo < alignX - 0.15f && onSide) {         /* too close to change depth safely: straight back out, x only */
                        o->axis[AX_LX] = (int16_t)(-toward * b->xSign * STICK); o->axis[AX_LY] = 0; b->breakZAt = 0;
                        WHY("breakable uid %u: backing out to %.1f to line up (dx %.2f, dz %.2f)", bt->uid, alignX, -dx, dz); return;
                    }
                    int mvx = 0;
                    if (fabsf_(standX - me->pos.x) > 0.5f && axo > alignX + 0.5f) {   /* far: toward the stand point */
                        mvx = sgn(standX - me->pos.x);
                        if ((mvx < 0 && me->pos.x - 0.1f < s_walkLo) || (mvx > 0 && me->pos.x + 0.1f > s_walkHi)) mvx = 0;
                    }
                    if (mvx) {
                        o->axis[AX_LX] = (int16_t)(mvx * b->xSign * STICK);
                        if (!b->breakXAt || fabsf_(me->pos.x - b->breakXPrev) > 0.02f) { b->breakXPrev = me->pos.x; b->breakXAt = now; }
                        else if (now - b->breakXAt > 400 && now >= b->breakNudgeUntil) { b->breakNudgeUntil = now + 700; b->breakXAt = 0; }   /* something solid in the way: detour */
                    } else b->breakXAt = 0;
                    if (now < b->breakNudgeUntil) {                /* detour off the wall, toward the open floor */
                        int away = az > 0.05f ? -sgn(tz - me->pos.z) : 1;
                        o->axis[AX_LY] = (int16_t)(away * b->zSign * STICK); b->breakZAt = 0;
                        WHY("breakable uid %u: around something in the way (dx %.2f, dz %.2f)", bt->uid, -dx, dz); return;
                    }
                    if (az >= 0.03f && !b->breakZBlocked) o->axis[AX_LY] = (int16_t)(sgn(tz - me->pos.z) * b->zSign * (az < 0.3f ? STICK / 2 : STICK));
                    else if (b->breakZBlocked && az >= 0.03f && fabsf_(me->pos.z - b->breakZLim) > 0.15f) b->breakZBlocked = 0;   /* moved off the limit: try again */
                    if (o->axis[AX_LY]) {
                        if (!b->breakZAt || fabsf_(me->pos.z - b->breakZPrev) > 0.02f) { b->breakZPrev = me->pos.z; b->breakZAt = now; }
                        else if (now - b->breakZAt > 300) {      /* depth limit (back wall / floor edge): use the best depth we reach */
                            b->breakZBlocked = 1; b->breakZLim = me->pos.z; b->breakZAt = 0;
                            LOGD("%s: breakable uid %u: depth limit at z %.2f (dz %.2f), using it", b->name, bt->uid, me->pos.z, dz);
                        }
                    }
                    WHY("breakable uid %u: lining up its depth at dx %.2f (dz %.2f)%s", bt->uid, -dx, tz - me->pos.z, b->breakZBlocked ? ", at the depth limit" : ""); return;
                }
            }
            if (ph == 1) {
                if (axo > b->breakStand) {
                    if (!b->breakXAt || fabsf_(me->pos.x - b->breakXPrev) > 0.02f) { b->breakXPrev = me->pos.x; b->breakXAt = now; }
                    if (now - b->breakXAt <= 400) {
                        o->axis[AX_LX] = (int16_t)(toward * b->xSign * STICK); o->axis[AX_LY] = 0;
                        WHY("breakable uid %u: on its lane, stepping in (dx %.2f, dz %.2f)", bt->uid, -dx, dz); return;
                    }
                }
                b->breakInPos = ph = 2; b->breakSwingSince = 0;
                note(b, D_BREAKALIGN, "uid %u: in position (dx %.2f, dz %.2f%s), swinging", bt->uid, -dx, dz, axo > b->breakStand ? ", blocked" : "");
            }
            b->lastProgress = now; b->progressPos = me->pos;
            int face = toward;
            if (me->facing != face) { o->axis[AX_LX] = (int16_t)(face * b->xSign * STICK); WHY("turning to the breakable"); return; }
            if (!b->breakSwingSince) b->breakSwingSince = now;
            if (now - b->breakSwingSince > 1300 && now - b->breakLastHit > 1300) {   /* about two strings, no damage */
                b->breakFails++; b->breakSwingSince = 0; b->breakXAt = 0; b->breakZAt = 0; b->breakZOff = 0;
                int depthOff = fabsf_(bt->pos.z - me->pos.z) > 0.08f && !b->breakZBlocked;
                b->breakInPos = depthOff ? 0 : 1; b->breakZBlocked = 0;   /* off its depth: back out and line up again; on it: just closer */
                if (b->breakFails >= 5) { int secs = unreach_add(b, bt->uid, now, 0); note(b, D_UNREACHABLE, "breakable uid %u: 5 misses, ignoring it for %d s", bt->uid, secs); b->breakUid = 0; WHY("breakable: giving up"); return; }
                const char *what;
                if (b->breakFails == 4) { b->breakSide = -b->breakSide; b->breakStand = clampf_(range * 1.6f, 1.8f, 2.2f); what = "the other side"; }
                else { b->breakStand -= 0.5f; if (b->breakStand < 0.5f) b->breakStand = 0.5f; what = "0.5 closer"; }
                if (b->breakFails == 4) b->breakInPos = 0;
                note(b, D_BREAKMISS, "uid %u: life stays %.0f (dz %.2f, dx %.2f): %s, %s", bt->uid, bt->life, dz, -dx, b->breakInPos ? "depth is right" : "backing out to line up its depth again", what);
                WHY("breakable: re-aligning"); return;
            }
            if (now >= b->nextComboAt) {
                GO(29, face, 1, { 55, 0, 0, SB_X }, { 55, 0, 0, 0 }, { 55, 0, 0, SB_X }, { 55, 0, 0, 0 }, { 55, 0, 0, SB_X }, { 300, 0, 0, 0 });
                b->nextComboAt = now + 600; seq_run(b, now, o);
            }
            WHY("breaking uid %u (life %.0f)", bt->uid, bt->life); return;
        }
    }
    /* m7.4: friendly fire retaliation: one light hit per owed hit, at the human who hit us */
    if (retal_on(w) && b->playerSlot >= 0 && b->playerSlot < 4 && (!seq_busy(b) || b->seq.interruptible) && !state_is_hurt(me->state)) {
        int my = b->playerSlot;
        for (int h = 0; h < 4; h++) {
            if (!s_rl.debt[my][h] || slot_is_bot(all, nall, w, h)) continue;
            const Chr *hc = slot_fighter(w, h);
            if (!hc || hc->life <= 0 || state_is_down(hc->state) || hc->scripted) continue;
            float hdx = hc->pos.x - me->pos.x, hdz = hc->pos.z - me->pos.z;
            if (fabsf_(hdx) <= range * 2.5f && fabsf_(hdz) <= lane * 3.0f) s_rl.at[my][h] = now;     /* a chance: the debt stays fresh */
            if (nearCount && lifePct < 40.0f) {
                if (!b->rlSkipNoted) { b->rlSkipNoted = 1; note(b, D_RETALIATE, "retaliate: P%d owed %d, but enemies in reach and life %.0f%%: later", h + 1, s_rl.debt[my][h], lifePct); }
                break;
            }
            b->rlSkipNoted = 0;
            if (now < b->rlNextAt) break;
            int sd = sgn(hdx) ? sgn(hdx) : (me->facing ? me->facing : 1);
            if (fabsf_(hdx) <= range && fabsf_(hdx) >= range * 0.1f && fabsf_(hdz) <= lane) {
                seq_clear(b);
                GO(36, sd, 0, { 60, F, 0, 0 }, { 70, 0, 0, SB_X }, { 260, 0, 0, 0 });
                b->rlSwingSlot = h; b->rlSwingUntil = now + 700; b->rlSwings++;
                b->rlNextAt = now + (nearCount ? 1500 : 800);           /* with enemies around: a combo on them in between */
                note(b, D_RETALIATE, "retaliate: one hit at P%d (uid %u, dx %.1f), %d owed", h + 1, hc->uid, hdx, s_rl.debt[my][h]);
                seq_run(b, now, o); set_mode(b, M_ATTACK, now, NULL); WHY("retaliate: one hit at P%d (%d owed)", h + 1, s_rl.debt[my][h]); return;
            }
            if (!nearCount && (!tgt || (fabsf_(hdx) <= range * 2.5f && fabsf_(hdz) <= lane * 3.0f))) {
                float gx = hc->pos.x - sd * range * 0.6f;
                walk_to(b, me, gx, hc->pos.z, range * 0.15f, lane * 0.5f, o);
                set_mode(b, M_APPROACH, now, NULL); WHY("retaliate: closing in on P%d (dx %.1f dz %.1f), %d owed", h + 1, hdx, hdz, s_rl.debt[my][h]); return;
            }
            break;
        }
    }
    if (!tgt) {
        b->targetUid = 0; b->breakUid = 0;
        /* m6: trail P1: behind it (opposite to where it is heading / facing), staggered lanes, never parked in front */
        int dir = fabsf_(b->p1Vx) > 0.5f ? sgn(b->p1Vx) : p1 ? p1->facing : 0;
        if (!dir) dir = 1;
        int rank = 0;
        for (int k = 0; k < nall; k++) if (&all[k] != b && all[k].identified && all[k].index < b->index) rank++;
        if (rank > 2) rank = 2;
        static const float kBack[3] = { 0.55f, 0.55f, 0.95f }, kLane[3] = { 4.0f, -4.0f, 0.0f };
        float gx = anchorX - dir * follow * kBack[rank], gz = anchorZ + kLane[rank] * lane;
        float ahead = (me->pos.x - anchorX) * dir;
        int inFront = p1 && ahead > range * 0.3f && fabsf_(me->pos.z - anchorZ) < lane * 3.0f;
        int crowded = 0;            /* standing on another bot (a lower-numbered one keeps its spot) */
        for (int k = 0; k < nall; k++) {
            const BotBrain *ob = &all[k];
            if (ob == b || !ob->identified || ob->playerSlot < 0 || ob->index > b->index) continue;
            const Chr *oc = slot_fighter(w, ob->playerSlot);
            if (oc && fabsf_(oc->pos.x - me->pos.x) < range * 0.8f && fabsf_(oc->pos.z - me->pos.z) < lane * 1.5f) crowded = 1;
        }
        float tolX = b->mode == M_IDLE ? follow * 0.6f : follow * 0.25f, tolZ = b->mode == M_IDLE ? lane * 3.0f : lane * 1.0f;
        if (fabsf_(gx - me->pos.x) > tolX || fabsf_(gz - me->pos.z) > tolZ || crowded || inFront) {
            WHY("follow P1: no enemy target, to (%.1f, %.1f) behind P1 (dir %d, rank %d)%s%s", gx, gz, dir, rank, crowded ? ", making room for another bot" : "", inFront ? ", out of P1's way" : "");
            set_mode(b, M_FOLLOW, now, NULL);
            walk_to(b, me, gx, gz, follow * 0.15f, lane * 0.6f, o);
        } else { WHY("idle: behind P1 (rank %d), no enemy target among %d fighters", rank, w->nch); set_mode(b, M_IDLE, now, "near P1, no enemies"); }
        return;
    }
    b->breakUid = 0;
    if (tgt->uid != b->targetUid) { b->targetUid = tgt->uid; b->hitsSeen = 0; b->attackSince = 0; }
    float dx = tgt->pos.x - me->pos.x, dz = tgt->pos.z - me->pos.z, adx = fabsf_(dx), adz = fabsf_(dz);
    int side = sgn(dx) ? sgn(dx) : 1;
    /* m6.7 (m6.6-run1, 11 of 17 jitter spots): standing on top of the target, dx crosses 0 every step and the side,
       the facing tap and the back-off goal flipped every frame. Within 0.35 range keep the side we came from. */
    if (adx < range * 0.35f && b->sideUid == tgt->uid && now - b->sideAt < 1500) side = b->sideK;
    else { b->sideUid = tgt->uid; b->sideK = side; b->sideAt = now; }
    int facing = me->facing == side || me->facing == 0;
    /* m6.7: too close (< 0.15 range) only counts when we were not already attacking it (hysteresis): at the 0.15
       edge the bots flipped between "attack" (forward string) and "approach" (back off) every few frames */
    int inLane = adz <= lane, inRange = adx <= range && (adx >= range * 0.15f || (b->closeOkUid == tgt->uid && now < b->closeOkUntil));
    uint64_t blitzGap = b->aggression == 2 ? 2000 : b->aggression == 0 ? 4500 : 3000;
    if (tgt->robotPart && b->partTgtNoted != tgt->uid) { b->partTgtNoted = tgt->uid;
        note(b, D_ROBOTPART, "going for robot leg uid %u (%s, %s) at (%.1f, %.1f), dx %.1f dz %.1f", tgt->uid, tgt->dname, tgt->robotPart == 2 ? "raised: jump attacks" : "planted: ground combos", tgt->pos.x, tgt->pos.z, dx, dz); }
    /* m7.5: a raised robot leg: jump attacks from beside / under it (player 1 hit raised legs with jump attacks) */
    if (tgt->robotPart == 2) {
        if (adx <= range * 1.1f && adz <= lane * 1.5f) {
            if (now - b->lastPartJump > 700) {
                GO(37, side, 0, { 80, F, 0, SB_A }, { 180, F, 0, 0 }, { 70, F, 0, SB_X }, { 300, F, 0, 0 });
                b->lastPartJump = now; s_botPartJumps++;
                LOGD("%s: jump attack at raised robot leg uid %u (y %.1f, dx %.1f dz %.1f)", b->name, tgt->uid, tgt->pos.y, dx, dz);
                seq_run(b, now, o); set_mode(b, M_ATTACK, now, NULL); WHY("jump attack at raised robot leg uid %u (y %.1f, dx %.1f dz %.1f)", tgt->uid, tgt->pos.y, dx, dz); return;
            }
            if (!facing) o->axis[AX_LX] = (int16_t)(side * b->xSign * STICK / 3);
            set_mode(b, M_ATTACK, now, NULL); WHY("under raised robot leg uid %u: next jump attack", tgt->uid); return;
        }
        set_mode(b, M_APPROACH, now, NULL);
        walk_to(b, me, tgt->pos.x - side * range * 0.5f, tgt->pos.z, range * 0.2f, lane * 0.5f, o);
        WHY("approach raised robot leg uid %u (y %.1f): dx %.1f dz %.1f", tgt->uid, tgt->pos.y, dx, dz); return;
    }

    /* back attack: an enemy right behind us */
    if (behind && C.backAttack && now - b->lastBack > 700 && (moving_attack(behind->state) || nearCount >= 2 || (inRange && inLane))) {
        b->lastBack = now;
        if (C.backAttack == 2) GO(17, me->facing ? me->facing : 1, 1, { 80, BK, 0, SB_X }, { 260, 0, 0, 0 });
        else GO(17, 1, 1, { 90, 0, 0, SB_RT }, { 260, 0, 0, 0 });
        note(b, D_BACKATTACK, "uid %u behind us (dx %.1f)%s", behind->uid, behind->pos.x - me->pos.x, moving_attack(behind->state) ? ", attacking" : "");
        seq_run(b, now, o); set_mode(b, M_ATTACK, now, NULL); WHY("back attack on uid %u", behind->uid); return;
    }
    /* m6.1: the target is knocked down: hitting it on the floor does nothing (live run: bots kept mashing X at
       enemies lying on the ground). Wait just outside wake-up range on our side, each bot on its own lane, facing
       it, until it gets up (another standing enemy is preferred by the target score anyway). */
    if (state_is_down(tgt->state)) {
        if (b->waitDownUid != tgt->uid) { b->waitDownUid = tgt->uid; note(b, D_WAITDOWN, "uid %u is down (%s): waiting out of wake-up range", tgt->uid, state_name(tgt->state)); }
        b->attackSince = 0; set_mode(b, M_APPROACH, now, "enemy down");
        float wz = tgt->pos.z + (float)((b->index % 3) - 1) * lane * 1.5f;
        float wx = tgt->pos.x - side * range * 1.6f;
        /* m6.7: arrived = within 0.3 range; leave the spot again only past 0.9 range (the facing tap used to push the
           bot back out of the 0.3 window, then it walked back in: lx flipped every frame) */
        float ex = fabsf_(wx - me->pos.x), ez = fabsf_(wz - me->pos.z);
        if (b->waitArrived != (int)tgt->uid && ex <= range * 0.3f && ez <= lane * 0.6f) b->waitArrived = (int)tgt->uid;
        else if (b->waitArrived == (int)tgt->uid && (ex > range * 0.9f || ez > lane * 1.5f)) b->waitArrived = 0;
        if (b->waitArrived != (int)tgt->uid) walk_to(b, me, wx, wz, range * 0.2f, lane * 0.4f, o);
        else if (!facing) o->axis[AX_LX] = (int16_t)(side * b->xSign * STICK / 3);   /* gentle: turn without walking */
        WHY("uid %u is down (%s): waiting at dx %.1f until it gets up", tgt->uid, state_name(tgt->state), dx); return;
    }
    b->waitDownUid = 0;
    /* m6.7: shield cops (stage 2 Murphy). m6.6-run1b: canGuard read 1 for all 73 enemies and the guard flag never
       read 1, so the m5 shield tactics hardly ran. The shield is a block-armor move; it breaks after enough damage
       (Character.stamina -> staminaBrokenTimer). It can't be grabbed while up. Tactics: full committed strings plus
       specials/blitzes into it, other bots pile on (target score), take its back when another bot is in front,
       back off when it attacks while we are in front, grabs only once it is broken. */
    if (tgt->guard || tgt->lastHitGuarded || tgt->guardHits > 0 || tgt->shieldMove) b->shieldUid = tgt->uid;
    int shieldCop = tgt->shieldUser || b->shieldUid == tgt->uid;
    int shieldUp = shieldCop && !tgt->staminaBroken && !state_is_hurt(tgt->state) && !state_is_down(tgt->state);
    int shFront = shieldUp && (tgt->facing == -side || tgt->facing == 0);
    if (shieldCop && tgt->staminaBroken && b->shBrokenUid != tgt->uid) {
        b->shBrokenUid = tgt->uid; b->lastGrabTry = 0;
        note(b, D_SHIELDTAC, "uid %u: shield broken (stamina %.0f): full strings, grabs allowed", tgt->uid, tgt->stamina);
    }
#define SHTAC(k, ...) do { if (b->shTacUid != tgt->uid || b->shTacKind != (k) || now - b->shTacAt > 4000) { b->shTacUid = tgt->uid; b->shTacKind = (k); b->shTacAt = now; note(b, D_SHIELDTAC, __VA_ARGS__); } } while (0)
    if (shFront && moving_attack(tgt->state) && !tgt->shieldMove && (tgt->aiAttacking || tgt->state == CS_DASH) &&
        adx < range * 2.0f && adz < lane * 2.0f && (!seq_busy(b) || b->seq.interruptible)) {
        seq_clear(b); b->lastShBackoff = now;
        walk_to(b, me, tgt->pos.x - side * range * 2.4f, me->pos.z + (dz > 0 ? -1.0f : 1.0f) * lane * 1.5f, range * 0.1f, lane * 0.3f, o);
        SHTAC(1, "uid %u attacks (move %d) while we are in front of its shield: backing off", tgt->uid, tgt->moveId);
        set_mode(b, M_DEFEND, now, "shield cop attacks"); WHY("shield cop uid %u attacking (move %d): backing off", tgt->uid, tgt->moveId); return;
    }
    /* a weapon in hand: throw-only ones (grenades) at range; m7.2: melee weapons too (see weapon_throw_why) */
    if (me->heldAddr) {
        const char *why = weapon_throw_why(b, w, me, tgt, side, facing, adx, adz, range, lane, now);
        if (why) {
            GO(27, side, 0, { 80, F, 0, SB_B }, { 300, 0, 0, 0 }); b->heldOnlyThrow = 0;
            note(b, D_WEAPONTHROW, "%s \"%s\" at uid %u (dx %.1f): %s", me->heldOnlyThrow || b->heldOnlyThrow ? "throw-only weapon" : "melee weapon",
                 me->heldName[0] ? me->heldName : "?", tgt->uid, dx, why);
            seq_run(b, now, o); set_mode(b, M_ATTACK, now, NULL); WHY("throwing the weapon at uid %u (%s)", tgt->uid, why); return;
        }
    }
    /* mid range: close the gap with a move */
    if (adz <= lane * 0.8f && adx > range * 1.3f && adx < range * 3.0f && !state_is_down(tgt->state) && facing &&
        tgt->pos.x > camLo && tgt->pos.x < camHi) {          /* m6.2: no lunges into the screen-edge margin */
        const char *what = NULL;
        if (pf->fwdSpecial && special_ok(b, 0, lifePct, now) && now - b->lastFwdSpecial > 6000) {
            GO(18, side, 1, { 90, F, 0, SB_Y }, { 550, 0, 0, 0 }); b->lastFwdSpecial = now; used_special(b, lifePct, banked, now);
            note(b, pf->grabStyle == GS_FLOYD ? D_PULL : D_FWDSPECIAL, "uid %u at dx %.1f, life %.0f%%", tgt->uid, dx, lifePct); what = "forward special";
        } else if (pf->run && now - b->lastRun > 3500 && adx > range * 1.8f) {
            GO(19, side, 1, { 50, F, 0, 0 }, { 40, 0, 0, 0 }, { 330, F, 0, 0 }, { 80, F, 0, SB_X }, { 400, 0, 0, 0 }); b->lastRun = now;
            note(b, D_RUN, "running knee at uid %u (dx %.1f)", tgt->uid, dx); what = "run";
        } else if (pf->pogo && now - b->lastPogo > 5000 && adx < range * 2.2f) {
            GO(20, side, 1, { 80, F, 0, SB_A }, { 200, F, 0, 0 }, { 80, 0, 1, SB_X }, { 160, 0, 1, 0 }, { 80, 0, 1, SB_X }, { 400, 0, 0, 0 }); b->lastPogo = now;
            note(b, D_POGO, "jump + down+X onto uid %u", tgt->uid); what = "pogo";
        } else if (pf->dash && now - b->lastDash > 2500) {
            GO(21, side, 1, { 50, F, 0, 0 }, { 40, 0, 0, 0 }, { 90, F, 0, 0 }, { 250, 0, 0, 0 }); b->lastDash = now;
            note(b, D_DASH, "dash toward uid %u (dx %.1f)", tgt->uid, dx); what = "dash";
        } else if (pf->blitz && now - b->lastBlitz > blitzGap && adz <= lane * 0.6f) {
            GO(22, side, 1, { 50, F, 0, 0 }, { 40, 0, 0, 0 }, { 80, F, 0, SB_X }, { 450, 0, 0, 0 }); b->lastBlitz = now;
            if (pf->blitzThenGrab) b->lastGrabTry = 0;
            note(b, D_BLITZ, "uid %u at dx %.1f", tgt->uid, dx); what = "blitz";
        } else if (C.jumpAttacks && jump_ok(b, now, tgt->uid) && now - b->lastJumpIn > (b->aggression == 2 ? 3500u : 5000u) && adx > range * 1.4f && adx < range * 2.4f &&
                   adz <= lane * 0.5f && !tgt->boss && !moving_attack(tgt->state)) {
            /* m6: jump-in kick, then the ground string right on landing */
            GO(30, side, 1, { 80, F, 0, SB_A }, { 200, F, 0, 0 }, { 70, F, 0, SB_X }, { 260, F, 0, 0 }); b->lastJumpIn = now; used_jump(b, 30, now);
            b->nextComboAt = 0; note(b, D_JUMPIN, "jump-in kick at uid %u (dx %.1f), string on landing", tgt->uid, dx); what = "jump-in";
        }
        if (what) { seq_run(b, now, o); set_mode(b, M_ATTACK, now, NULL); WHY("%s at uid %u", what, tgt->uid); return; }
    }
    if (!inLane || !inRange) {
        WHY("approach uid %u: dx %.1f (range %.1f), dz %.1f (lane %.1f), %s", tgt->uid, dx, range, dz, lane, !inLane ? "off lane" : "out of range");
        /* a fresh approach (other mode before, or a break such as being hurt): new stuck timers */
        if ((b->mode != M_APPROACH || now - b->lastApproachAt > 300) && !(b->lastJump && now - b->lastJump < 1000)) {   /* m6.1: our own hop is not a break */ b->progressPos = me->pos; b->lastProgress = now; b->stuckJumps = 0; b->detours = 0; b->detourUntil = 0; b->approachSince = now; }
        b->lastApproachAt = now;
        set_mode(b, M_APPROACH, now, NULL);
        b->attackSince = 0;
        /* m5: shield up and facing us -> come in from behind (the far side), around it on another lane */
        float goalX = tgt->pos.x - side * range * 0.7f, goalZ = tgt->pos.z;
        /* m6: another bot already on our side of this enemy: take the other side (if it is on screen) */
        if (adx > range * 1.2f) {
            for (int k = 0; k < nall; k++) {
                const BotBrain *ob = &all[k];
                if (ob == b || !ob->identified || ob->playerSlot < 0 || ob->targetUid != tgt->uid) continue;
                const Chr *oc = slot_fighter(w, ob->playerSlot);
                float farX = tgt->pos.x + side * range * 0.7f;
                if (oc && fabsf_(oc->pos.x - goalX) < range * 0.8f && fabsf_(oc->pos.z - goalZ) < lane * 2.0f &&
                    (!w->haveCam || fabsf_(farX - w->cam.x) < screenW * 0.45f)) {
                    goalX = farX;
                    if (now - b->lastSpread > 3000) { b->lastSpread = now; note(b, D_SPREAD, "%s is on our side of uid %u: taking the far side", ob->name, tgt->uid); }
                    break;
                }
            }
        }
        if (now < b->flankUntil && b->shieldUid == tgt->uid) { goalX = tgt->pos.x + side * range * 0.7f; if (adx < range * 1.2f) goalZ = tgt->pos.z + (dz > 0 ? -1.0f : 1.0f) * lane * 2.5f; }
        if (now < b->detourUntil) {     /* m5: walking around an obstacle */
            walk_to(b, me, b->detourX, b->detourZ, range * 0.15f, lane * 0.4f, o);
            WHY("approach uid %u: walking around an obstacle (to %.1f, %.1f)", tgt->uid, b->detourX, b->detourZ);
        } else walk_to(b, me, goalX, goalZ, range * 0.25f, lane * 0.6f, o);
        /* stuck? m5: walk around it along the other axis (twice, alternating sides), then hop (twice), then leave
           that target alone: 4 s, 15 s, then 60 s if it keeps happening (or at once 60 s for one that never targets anyone) */
        if (fabsf_(me->pos.x - b->progressPos.x) + fabsf_(me->pos.z - b->progressPos.z) > range * 0.1f) { b->progressPos = me->pos; b->lastProgress = now; if (now >= b->detourUntil) b->stuckJumps = 0; }
        if (now - b->lastProgress > (b->detours < 2 ? 900u : 1500u) && now - b->approachSince > 1500 && now >= b->detourUntil && now - b->lastJump > 1200) {
            if (b->detours < 2) {
                b->detours++; b->detourFlip = !b->detourFlip;
                int dirAlt = b->detourFlip ? 1 : -1;
                if (adz > lane && adx < range * 1.5f) {        /* blocked going up/down: step sideways first */
                    b->detourX = me->pos.x + dirAlt * range * 1.8f; b->detourZ = me->pos.z;
                } else {                                     /* blocked going left/right: go around on another lane */
                    b->detourX = me->pos.x; b->detourZ = me->pos.z + dirAlt * lane * 5.0f;
                }
                b->detourUntil = now + 1100; b->lastProgress = now; b->progressPos = me->pos;
                note(b, D_DETOUR, "stuck going to uid %u (dx %.1f, dz %.1f): around via (%.1f, %.1f)", tgt->uid, dx, dz, b->detourX, b->detourZ);
            } else if (b->stuckJumps >= 2) {
                int passive = tgt->aiTarget == 0 && tgt->state == CS_STANDWALK;
                int secs = unreach_add(b, tgt->uid, now, passive);
                b->stuckJumps = 0; b->detours = 0; b->lastProgress = now;
                note(b, D_UNREACHABLE, "uid %u (dx %.1f, dz %.1f%s): ignoring it for %d s", tgt->uid, dx, dz, passive ? ", targets nobody" : "", secs);
            } else {
                /* m6.1: a hop over the obstacle; X in the air when an enemy is in reach in front */
                if (airDir) GO(35, airDir, 1, { 80, F, 0, SB_A }, { 180, F, 0, 0 }, { 60, F, 0, SB_X }, { 200, 0, 0, 0 });
                else GO(35, side, 1, { 80, F, 0, SB_A }, { 380, F, 0, 0 });
                seq_run(b, now, o); b->lastJump = now; b->stuckJumps++; b->lastProgress = now;
                LOGI("%s: stuck while approaching uid %u (dx %.1f, dz %.1f); jump %d of 2.", b->name, tgt->uid, dx, dz, b->stuckJumps);
            }
        }
        return;
    }
    /* in position */
    WHY("attack uid %u (%s, life %.0f): dx %.1f dz %.1f range %.1f lane %.1f, hits seen %d, scale %.2f/%.2f", tgt->uid, state_name(tgt->state), tgt->life, dx, dz, range, lane, b->hitsSeen, b->rangeScale, b->laneScale);
    set_mode(b, M_ATTACK, now, NULL);
    b->lastProgress = now; b->progressPos = me->pos; b->closeOkUid = tgt->uid; b->closeOkUntil = now + 800;   /* m6.7 */
    if (!facing) { o->axis[AX_LX] = (int16_t)(side * b->xSign * STICK); return; }     /* turn to face the enemy */
    if (!b->attackSince) b->attackSince = now;
    if (state_is_hurt(tgt->state) || tgt->state == CS_STANDGUARDHIT || tgt->state == CS_CROUCHGUARDHIT || (shieldCop && tgt->lastHitGuarded)) b->hitsSeen++;   /* m6.7: a blocked hit is still a hit in reach */
    /* after a full string: did the last hit launch? (Shiva jump-cancels into an air juggle) */
    if (b->wantJuggle && now >= b->comboEnd && now < b->comboEnd + 350 && (tgt->state == CS_AIRHIT || tgt->state == CS_AIRLIGHTHIT)) {
        b->wantJuggle = 0; note(b, D_LAUNCHER, "uid %u launched by the last hit of the string", tgt->uid);
        if (pf->juggle) {
            GO(23, side, 1, { 80, 0, 0, SB_A }, { 140, F, 0, 0 }, { 60, F, 0, SB_X }, { 190, F, 0, 0 }, { 60, F, 0, SB_X }, { 190, F, 0, 0 },
               { 60, F, 0, SB_X }, { 190, F, 0, 0 }, { 60, F, 0, SB_X }, { 300, 0, 0, 0 });
            note(b, D_JUGGLE, "jump cancel + air X x4 on uid %u", tgt->uid); seq_run(b, now, o); return;
        }
    }
    if (now >= b->comboEnd + 350) b->wantJuggle = 0;
    if (now < b->nextComboAt) return;
    /* m6.7: a raised shield facing us: no grab (it can't be held), no jump-over; a star move or a special when the
       budget allows, otherwise the full string, chained (the shield breaks after enough damage) */
    if (shFront) {
        float stFrac = tgt->maxStamina > 0 && tgt->stamina >= 0 ? tgt->stamina / tgt->maxStamina : 1.0f;
        if (star_ok(b, stars, now) && stFrac >= 0.5f && now - b->lastShieldStar > 15000 && adx < range * 1.2f) {
            b->lastShieldStar = now; do_star(b, now);
            SHTAC(2, "star move into the shield of uid %u (stamina %.0f%%)", tgt->uid, stFrac * 100.0f);
            seq_run(b, now, o); WHY("star move into the shield of uid %u", tgt->uid); return;
        }
        if (special_ok(b, 0, lifePct, now) && lifePct >= 45.0f && now - b->lastShieldSpecial > 2500) {
            b->lastShieldSpecial = now;
            if (pf->fwdSpecial) GO(18, side, 1, { 90, F, 0, SB_Y }, { 550, 0, 0, 0 }); else GO(9, side, 1, { 90, 0, 0, SB_Y }, { 450, 0, 0, 0 });
            used_special(b, lifePct, banked, now); b->nextComboAt = now + 500;
            SHTAC(3, "%s special into the shield of uid %u (life %.0f%%, its stamina %.0f%%)", pf->fwdSpecial ? "forward" : "neutral", tgt->uid, lifePct, stFrac * 100.0f);
            seq_run(b, now, o); WHY("special into the shield of uid %u", tgt->uid); return;
        }
        SHTAC(4, "full strings into the shield of uid %u (its stamina %.0f%%, life %.0f%%)", tgt->uid, stFrac * 100.0f, lifePct);
    } else if (shieldUp) SHTAC(5, "uid %u's shield faces away: full strings from %s", tgt->uid, adz > lane * 0.6f ? "the side" : "behind");
    /* m6: jumps in the fight: air hits on an airborne enemy, a jump kick into a crowd, an air special when healthy */
    if (C.jumpAttacks && !shFront && jump_ok(b, now, tgt->uid)) {
        if ((tgt->state == CS_AIRHIT || tgt->state == CS_AIRLIGHTHIT) && now - b->lastAirAttack > 2500 && adx < range * 0.9f) {
            b->lastAirAttack = now; used_jump(b, 32, now);
            GO(32, side, 1, { 70, 0, 0, SB_A }, { 130, F, 0, 0 }, { 60, F, 0, SB_X }, { 200, F, 0, 0 }, { 60, F, 0, SB_X }, { 300, 0, 0, 0 });
            note(b, D_AIRATTACK, "uid %u is in the air (%s): air X x2", tgt->uid, state_name(tgt->state)); seq_run(b, now, o); return;
        }
        if (nearFront >= 2 && now - b->lastCrowdKick > 5000) {
            b->lastCrowdKick = now; used_jump(b, 31, now);
            GO(31, side, 1, { 80, F, 0, SB_A }, { 170, F, 0, 0 }, { 80, F, 0, SB_X }, { 450, 0, 0, 0 });
            note(b, D_CROWDKICK, "%d enemies in front", nearFront); seq_run(b, now, o); return;
        }
        if (lifePct >= 70.0f && nearCount >= 2 && now - b->lastAirSpecial > 9000 && special_ok(b, 0, lifePct, now)) {
            b->lastAirSpecial = now; used_jump(b, 33, now); used_special(b, lifePct, banked, now);
            GO(33, side, 1, { 80, 0, 0, SB_A }, { 160, 0, 0, 0 }, { 80, 0, 0, SB_Y }, { 500, 0, 0, 0 });
            note(b, D_AIRSPECIAL, "%d enemies near, life %.0f%%", nearCount, lifePct); seq_run(b, now, o); return;
        }
    }
    /* walk-in grab: every few strings on an enemy that can be held */
    int every = b->aggression == 2 ? 2 : b->aggression == 0 ? 4 : 3;
    if (C.useGrabs && !shieldUp && adx < range * 0.6f && !tgt->boss && !tgt->superArmor && !tgt->cantBeGrabbed && tgt->state == CS_STANDWALK &&
        (b->lastGrabTry == 0 || (b->comboCount % every == every - 1 && now - b->lastGrabTry > 4000))) {
        b->lastGrabTry = now; b->comboCount++;
        GO(24, side, 1, { 420, F, 0, 0 }, { 60, 0, 0, 0 });
        note(b, D_GRABTRY, "walking into uid %u", tgt->uid); seq_run(b, now, o); return;
    }
    if (pf->stunGrenade && !shFront && nearCount >= 2 && special_ok(b, 0, lifePct, now) && now - b->lastDefSpecial > 6000) {
        GO(25, side, 1, { 90, 0, 0, SB_Y }, { 450, 0, 0, 0 }); used_special(b, lifePct, banked, now); b->lastDefSpecial = now;
        note(b, D_STUNGRENADE, "%d enemies near, life %.0f%%", nearCount, lifePct); seq_run(b, now, o); return;
    }
    /* the full string; its last hit launches or knocks away */
    {
        int n = pf->comboLen > 0 ? pf->comboLen : C.comboHits; if (n > 9) n = 9; if (n < 1) n = 1;
        uint16_t per = (uint16_t)C.comboTapMs; SeqStep stp[20]; int k = 0;
        for (int i = 0; i < n; i++) { stp[k++] = (SeqStep){ (uint16_t)(per / 2), 0, 0, SB_X }; stp[k++] = (SeqStep){ (uint16_t)(per - per / 2), 0, 0, 0 }; }
        int jc = 0;
        if (pf->jcCombo && C.jumpAttacks && !shFront && (b->jcToggle++ % 2) == 1 && !tgt->boss && !tgt->superArmor && jump_ok(b, now, tgt->uid)) {
            /* m6: Blaze X x4 -> A (jump cancel) -> air down+X -> air X */
            k = 0; for (int i = 0; i < 4; i++) { stp[k++] = (SeqStep){ (uint16_t)(per / 2), 0, 0, SB_X }; stp[k++] = (SeqStep){ (uint16_t)(per - per / 2), 0, 0, 0 }; }
            stp[k++] = (SeqStep){ 60, 0, 0, SB_A }; stp[k++] = (SeqStep){ 150, F, 0, 0 }; stp[k++] = (SeqStep){ 60, 0, 1, SB_X };
            stp[k++] = (SeqStep){ 200, 0, 0, 0 }; stp[k++] = (SeqStep){ 60, 0, 0, SB_X }; stp[k++] = (SeqStep){ 350, 0, 0, 0 };
            n = 4; jc = 1; used_jump(b, 34, now);
            note(b, D_JCCOMBO, "X x4, jump cancel, air down+X, air X on uid %u", tgt->uid);
        }
        seq_go(b, jc ? 34 : 26, side, 1, stp, k, now);
        uint64_t pause = (uint64_t)C.comboPauseMs * (b->aggression == 0 ? 16 : b->aggression == 2 ? 6 : 10) / 10;
        b->comboEnd = now + (uint64_t)n * per + (jc ? 880u : 0u); b->nextComboAt = b->comboEnd + pause; b->wantJuggle = !jc; b->comboCount++;
        /* m5: score comes from long chains: if the target is still reeling, follow up at once instead of pausing */
        if (shieldCop && !incoming) {     /* m6.7: keep pressing: chained strings into the shield, from behind, or once it broke */
            b->nextComboAt = b->comboEnd + 80;
            if (state_is_hurt(tgt->state)) note(b, D_CHAIN, "uid %u (shield cop) reeling: follow-up string", tgt->uid);
        } else if (state_is_hurt(tgt->state) && tgt->hitStun > 0 && !incoming && nearBehind == 0) { b->nextComboAt = b->comboEnd + 60; note(b, D_CHAIN, "uid %u still reeling (stun %d): follow-up string", tgt->uid, tgt->hitStun); }
        note(b, D_COMBO, "%d-hit string on uid %u", n, tgt->uid);
        seq_run(b, now, o);
    }
    /* adapt: two strings without the target ever reacting -> we are probably out of reach */
    uint64_t cycle = (uint64_t)C.comboTapMs * (uint64_t)(pf->comboLen > 0 ? pf->comboLen : C.comboHits) + (uint64_t)C.comboPauseMs;
    if (b->attackSince && now - b->attackSince > 2 * cycle) {
        if (!b->hitsSeen) {
            b->rangeScale = b->rangeScale * 0.8f < 0.3f ? 0.3f : b->rangeScale * 0.8f;
            b->laneScale = b->laneScale * 0.85f < 0.4f ? 0.4f : b->laneScale * 0.85f;
            LOGD("%s: combos missed; range x%.2f lane x%.2f", b->name, b->rangeScale, b->laneScale);
        } else if (b->rangeScale < 1.0f) b->rangeScale += 0.05f;
        b->attackSince = now; b->hitsSeen = 0;
    }
}
#undef F
#undef BK
/* m4: don't run the stick check while the stage intro walks the fighters in. In the m3 test log the intro
   ran with GameState.mode 1 and every fighter's stick reading (1.00, 0.01) while our pad sent nothing; the
   check then failed and cost 4 s. */
static const char *intro_wait(BotBrain *b, const World *w, const Chr *me, uint64_t now) {
    if (!b->levelSince) b->levelSince = now;
    if (w->mode != 0) {
        if (!b->introModeSince || b->introMode != w->mode) { b->introMode = w->mode; b->introModeSince = now; }
        if (now - b->introModeSince < 10000) return "stage intro or scripted scene (game session mode is not 0)";
        if (!b->introModeLogged) { b->introModeLogged = 1; LOGI("%s: game session mode stays %d for 10 s; treating it as normal play.", b->name, w->mode); }
    } else b->introModeSince = 0;
    if (b->prevLx == 0 && (me->stickX > 0.3f || me->stickX < -0.3f || me->stickY > 0.3f || me->stickY < -0.3f)) {
        b->stickQuietSince = 0; return "the game is moving our fighter (intro walk-in)";
    }
    if (!b->stickQuietSince) b->stickQuietSince = now;
    if (now - b->stickQuietSince < 1000) return "letting the stage start settle";
    if (me->state != CS_STANDWALK) return "fighter busy";
    return NULL;
}

void brain_think(BotBrain *b, BotBrain *all, int nall, const World *w, int stateUsable, uint64_t now, PadOut *o) {
    neutral(o);
    if (b->lastThink && now - b->lastThink > 150) seq_clear(b);       /* the brain was paused (menu, focus): drop a half-played move */
    b->lastThink = now;
    int outKind = 0;                        /* 0 = nothing may be pressed, 1 = menu/join (A only), 2 = gameplay, 3 = character select (A, RB, stick), 4 = character select B only (m7) */
    char d[120];
    if (!stateUsable || !C.smart) {
        if (!C.smart || C.fallbackTest) {
            if (!s_focused) { WHY("blocked: game window not focused"); gate_note(b, G_UNFOCUSED, now, NULL); goto done; }
            uint64_t t = now - b->activeSince;
            if (!b->fallbackLogged) { b->fallbackLogged = 1; LOGI("%s: using the test pattern (%s)", b->name, !C.smart ? "Brain Mode=Test" : "game state not readable, FallbackToTestPattern=1"); }
            WHY("test pattern (%s)", !C.smart ? "Mode=Test" : "game state not readable");
            set_mode(b, M_TEST, now, NULL);
            outKind = 2;
            if (t < join_len()) o->btn[C.joinButton] = (t % 600) < 100; else test_pattern(b, t - join_len(), o, 1);
            goto done;
        }
        if (!b->fallbackLogged) { b->fallbackLogged = 1; LOGI("%s: game state not readable; staying neutral (FallbackToTestPattern=0).", b->name); }
        gate_note(b, G_NOSTATE, now, NULL);
        WHY("idle: game state not readable (neutral; FallbackToTestPattern=0)"); set_mode(b, M_IDLE, now, "state not readable"); goto done;
    }
    if (w) ident_update(b, all, nall, w, now);
    /* where is our controller playing? */
    b->playerSlot = -1;
    if (w && b->id.ctrlId >= 0) for (int i = 0; i < w->nslot; i++) if (w->slot[i].ctrl == b->id.ctrlId) { b->playerSlot = i; break; }

    /* ---------- m2.4 safety gate: these always win ---------- */
    if (!s_focused) { gate_note(b, G_UNFOCUSED, now, NULL); WHY("blocked: game window not focused"); set_mode(b, M_IDLE, now, "not focused"); goto done; }
    if (now - s_focusSince < (uint64_t)C.focusGraceMs) {
        snprintf(d, sizeof d, "waiting %d ms after the window got focus back", C.focusGraceMs);
        gate_note(b, G_FOCUSGRACE, now, d); WHY("blocked: %s", d); goto done;
    }
    if (w && w->paused) {
        b->lastPauseAt = now;
        snprintf(d, sizeof d, "P%d slot status %d", w->pausedSlot + 1, w->pausedStatus);
        gate_note(b, G_PAUSED, now, d); WHY("blocked: game paused / menu (%s)", d); set_mode(b, M_IDLE, now, "paused"); goto done;
    }
    if (b->lastPauseAt && now - b->lastPauseAt < (uint64_t)C.resumeGraceMs) {
        snprintf(d, sizeof d, "waiting %d ms after the pause ended", C.resumeGraceMs);
        gate_note(b, G_RESUME, now, d); WHY("blocked: %s", d); goto done;
    }
    int level = w && w->ok && w->nch > 0;
    if (level && !w->live) {
        snprintf(d, sizeof d, "frame %d, %s, unchanged ~%d ms", w->frames, w->source, w->frozenMs);
        gate_note(b, G_NOTLIVE, now, d); WHY("idle: stage not advancing (%s)", d); set_mode(b, M_IDLE, now, "paused / not updating"); goto done;
    }
    if (level) { b->inLevel = 1; b->noLevelSince = 0; }
    else if (b->inLevel) {
        if (!b->noLevelSince) b->noLevelSince = now;
        /* m5: the m4 log never had a stage-end summary: between stages the "no level" gap (Loading screen) was only
           ~1 s. Also end the stage on the Loading / StageSummary / Video screens. */
        int endScreen = w && (!strcmp(w->menuName, "Loading") || !strcmp(w->menuName, "StageSummary") || !strcmp(w->menuName, "Video") || !strcmp(w->menuName, "MainMenu"));
        if (now - b->noLevelSince > 2000 || (endScreen && now - b->noLevelSince > 150)) { b->inLevel = 0; b->introDone = 0; brain_summary(b, "stage ended"); b->levelSince = 0; b->stickQuietSince = 0; seq_clear(b); }
    }
    if (level && !b->sawLevel) { b->sawLevel = 1; LOGI("%s: stage running; no more menu taps until F7 is pressed off and on.", b->name); }
    /* character select. m3: proven from the game's own menu state when it is readable (w->menuRead):
       currentMenu is CharacterSelection AND the top menu screen is CharacterSelectionScreen AND nothing is
       paused. Only when that can't be read: the m2.4 rule (no GameState anywhere, no stage seen since F7). */
    int humanIn = 0;
    if (w) for (int i = 0; i < w->nslot; i++) if (w->slot[i].ctrl >= 0 && w->slot[i].type != 0 && !brain_ctrl_is_bot(all, nall, w->slot[i].ctrl)) humanIn = 1;
    int proven = w && w->menuRead;
    int charSel = proven ? (!level && w->charSel && humanIn)
                         : (!level && w && !b->sawLevel && w->noGameState && humanIn && now - b->activeSince < 120000);
    if (!level && !b->sawLevel && !proven) state_want_menu_check();
    if (proven && charSel != b->prevCharSel) {
        b->prevCharSel = charSel;
        memset(&b->cs, 0, sizeof b->cs); b->joinAttempts = 0;
        if (charSel) { LOGI("%s: character select (proven: menu %s, screen %s).", b->name, w->menuName, w->topScreen); state_roster_nav_new_visit(); }
        if (b->id.ctrlId < 0) b->joinAt = 0;           /* a new visit gets fresh join attempts */
        b->csInSlot = 0; b->rejoins = 0; b->rejoinAt = 0;
    }
    if (b->live != b->prevLive) {                       /* m7: F7 menu opened / closed: start character select over */
        b->prevLive = b->live;
        memset(&b->cs, 0, sizeof b->cs);
        if (proven && charSel) LOGI("%s: F7 menu %s at character select: %s.", b->name, b->live ? "opened" : "closed",
                                    b->live ? "following the menu live (no confirming while it is open)" : "confirming the pick now");
    }
    if (!b->live) b->liveStatus[0] = 0;
    if (proven && charSel && b->playerSlot >= 0) b->csInSlot = 1;
    /* m7: turned OFF at character select: leave our slot with B (never as P1, only at a proven character select) */
    if (b->backout) {
        int k = cs_backout(b, w, proven, charSel, now, o);
        if (k) { outKind = 4; gate_note(b, G_MENU_TAP, now, "backing out"); set_mode(b, M_CSEL, now, "backing out"); }
        else { WHY("idle: backed out (%s)", b->boWhy); set_mode(b, M_IDLE, now, "backed out"); }
        goto done;
    }
    if (b->live && !(proven && charSel)) {
        b->liveStatus[0] = 0;
        WHY("idle: F7 menu open (hands off outside a proven character select)"); set_mode(b, M_IDLE, now, "menu open"); goto done;
    }

    /* join: only while our controller isn't playing yet, one bot after another */
    if (b->id.ctrlId < 0) {
        int may = level || charSel;
        if (may && !b->joinAt && join_turn(b, all, nall, now)) {
            if (proven && charSel && b->joinAttempts >= 3) may = 0;
            else {
                b->joinAt = now + 200; b->joinAttempts++;
                b->joinTaps = proven && charSel ? 1 : C.joinPresses;       /* m3: one tap; a second A would confirm the character */
                LOGI("%s: join: will tap A %d time(s) (%s, attempt %d).", b->name, b->joinTaps, level ? "stage running" : "character select", b->joinAttempts);
            }
        }
        /* nobody new joined: try again (stage: every 8 s; proven character select: after 2 s, at most 3 attempts) */
        if (b->joinAt && level && now > b->joinAt + bjoin_len(b) + 8000) { b->joinAt = 0; LOGI("%s: no controller joined for us; pressing join again.", b->name); }
        if (b->joinAt && proven && charSel && now > b->joinAt + bjoin_len(b) + 2000 && b->joinAttempts < 3) b->joinAt = 0;
        if (may && b->joinAt && now >= b->joinAt && now < b->joinAt + bjoin_len(b)) {
            gate_note(b, level ? G_PLAY : G_MENU_TAP, now, "joining");
            WHY("joining (tapping A; controller not known yet)"); set_mode(b, M_JOIN, now, NULL);
            o->btn[BTN_A] = ((now - b->joinAt) % 600) < 100; outKind = 1; goto done;
        }
    }
    if (b->id.ctrlId < 0) {
        gate_note(b, level ? G_IDENT : charSel ? G_IDENT : G_MENU_WAIT, now, level || charSel ? "our controller is not known yet" : w && w->noGsWhy && w->noGsWhy[0] ? w->noGsWhy : !humanIn ? "nobody has joined yet" : "");
        WHY("idle: don't know our controller yet (%s)", !w || w->nslot <= 0 ? "player slots not readable" : !b->joinAt ? "waiting for a safe moment to join" : "no new controller joined");
        set_mode(b, M_IDLE, now, "identity unknown"); goto done;
    }
    if (b->playerSlot < 0 && proven && charSel && b->csInSlot) {
        /* m7: we were in a slot at this character select and left it (a B that un-joined instead of un-confirming):
           tap A to join again, at most 3 times this visit, 2 s apart */
        if (!b->rejoinAt || (now >= b->rejoinAt + 2000 && b->rejoins < 3)) {
            b->rejoinAt = now; b->rejoins++;
            LOGI("%s: our controller %d left its slot at character select; tapping A to join again (%d of 3).", b->name, b->id.ctrlId, b->rejoins);
        }
        if (now - b->rejoinAt < 100) { o->btn[BTN_A] = 1; outKind = 1; WHY("character select: re-joining (A)"); set_mode(b, M_JOIN, now, "re-joining"); goto done; }
        if (b->live) snprintf(b->liveStatus, sizeof b->liveStatus, "left its slot; re-joining (%d of 3)", b->rejoins);
    }
    if (b->playerSlot < 0) {
        gate_note(b, G_IDENT, now, "our controller is not in a player slot");
        WHY("idle: our controller %d is not in any player slot", b->id.ctrlId);
        set_mode(b, M_IDLE, now, "not playing"); goto done;
    }
    if (!level && proven) {
        if (!charSel) {
            snprintf(d, sizeof d, "menu '%s', screen '%s'", w->menuName[0] ? w->menuName : "none", w->topScreen[0] ? w->topScreen : "none");
            gate_note(b, G_MENU_WAIT, now, d); WHY("idle: not character select (%s)", d); set_mode(b, M_IDLE, now, "no level running"); goto done;
        }
        if (b->live) {
            int k = cs_live(b, w, now, o);
            if (k) { outKind = k; gate_note(b, G_MENU_TAP, now, "character select, menu open"); set_mode(b, M_CSEL, now, "character select (live)"); goto done; }
            gate_note(b, G_CS_WAIT, now, "character select, menu open"); set_mode(b, M_CSEL, now, "character select (live)"); goto done;
        }
        if (cs_drive(b, w, now, o)) { outKind = 3; gate_note(b, G_MENU_TAP, now, "proven character select"); set_mode(b, M_CSEL, now, "character select"); goto done; }
        gate_note(b, G_MENU_DONE, now, "character select done");
        set_mode(b, M_IDLE, now, "character select done"); goto done;
    }
    if (!level) {
        /* character select, m2.4 fallback (menu state not readable): confirm our pick by tapping A (never
           anything else), about every 700 ms, at most MenuConfirmMaxTaps times within MenuConfirmSeconds,
           only once per F7 and never after a stage has run. */
        if (!charSel) {
            const char *why = b->sawLevel ? "a stage already ran since F7" : !w ? "no state" : !w->noGameState ? (w->noGsWhy ? w->noGsWhy : "-") : !humanIn ? "nobody else in a slot" : "more than 2 minutes since F7";
            gate_note(b, b->sawLevel ? G_MENU_DONE : G_MENU_WAIT, now, why);
            WHY("idle: no stage running; no menu taps (%s)", why); set_mode(b, M_IDLE, now, "no level running"); goto done;
        }
        if (!b->menuSince) {
            b->menuSince = now;
            LOGI("%s: character select (%s); we are P%d: tapping A to confirm, at most %d tap(s) in %d s.", b->name, w->noGsWhy,
                 b->playerSlot + 1, C.menuConfirmMaxTaps, C.menuConfirmMs / 1000);
        }
        uint64_t t = now - b->menuSince;
        if (C.menuConfirmMs && t < (uint64_t)C.menuConfirmMs && (b->menuTaps < C.menuConfirmMaxTaps || b->menuPrevTap)) {
            int tap = (t % 700) >= 300 && (t % 700) < 400;          /* 100 ms press, every 700 ms */
            if (tap && !b->menuPrevTap) {
                if (b->menuTaps >= C.menuConfirmMaxTaps) tap = 0;
                else { b->menuTaps++; LOGI("%s: menu: confirm tap %d of %d (A) as P%d, %.1f s in.", b->name, b->menuTaps, C.menuConfirmMaxTaps, b->playerSlot + 1, t / 1000.0); }
            }
            o->btn[BTN_A] = (uint8_t)tap; b->menuPrevTap = tap; outKind = 1;
            gate_note(b, G_MENU_TAP, now, w->noGsWhy);
            WHY("menu: tapping A to confirm our pick as P%d (%d of %d taps, %.1f s of %d s)", b->playerSlot + 1, b->menuTaps, C.menuConfirmMaxTaps, t / 1000.0, C.menuConfirmMs / 1000);
            set_mode(b, M_JOIN, now, "confirming in the menu");
            goto done;
        }
        if (!b->menuLoggedEnd) { b->menuLoggedEnd = 1; LOGI("%s: done confirming (%d tap(s)); waiting for the stage. F7 off/on allows another round.", b->name, b->menuTaps); }
        gate_note(b, G_MENU_DONE, now, "confirm taps used up");
        WHY("idle: no stage running (%s, %d fighters)", w->source, w->nch); set_mode(b, M_IDLE, now, "no level running"); goto done;
    }
    b->hadLevel = 1;
    gate_note(b, G_PLAY, now, w->source);
    outKind = 2;

    const Chr *me = slot_fighter(w, b->playerSlot);
    if (!me) { b->identified = 0; WHY("idle: no fighter with player id %d (P%d) on screen (respawning?)", b->playerSlot, b->playerSlot + 1); set_mode(b, M_IDLE, now, "our fighter not on screen"); goto done; }
    if (me->ctrlKind != CTRL_PLAYER) { WHY("idle: P%d's fighter uid %u isn't player-controlled right now", b->playerSlot + 1, me->uid); set_mode(b, M_IDLE, now, "not player-controlled"); goto done; }
    if (b->uid != me->uid) {
        if (b->uid) LOGI("%s: our fighter is now uid %u (was %u), still P%d (player id %d).", b->name, me->uid, b->uid, b->playerSlot + 1, b->playerSlot);
        b->uid = me->uid; b->maxLife = 0;
    }
    /* m6: no stick-check wiggle. Our controllerId -> player slot comes from the game's player slots (seen when our
       pad joined, m2.2); the slot's fighter is the Character with that playerId (slot_fighter, follows respawns).
       A silent check runs while the bot plays normally: the fighter's controller must report the stick we send. */
    if (b->id.verifiedSlot != b->playerSlot) {
        b->identified = 0;
        if (now < b->id.ckRetryAt) { WHY("idle: silent check failed; trusting the slot again shortly"); set_mode(b, M_IDLE, now, "check failed"); goto done; }
        b->id.verifiedSlot = b->playerSlot;
        b->id.pvPending = 1; b->id.pvSamples = b->id.pvFollow = b->id.pvAgree = 0;
        LOGI("%s: plays P%d's fighter uid %u (player id %d, our controller %d: %s). No stick check; checked silently while it plays.",
             b->name, b->playerSlot + 1, me->uid, b->playerSlot, b->id.ctrlId, b->id.how);
    }
    if (w->mode != 0 && b->introDone) { b->introDone = 0; b->stickQuietSince = 0; LOGI("%s: game session mode %d during the stage (scripted scene): hands off.", b->name, w->mode); }   /* m6.2 */
    if (!b->introDone) {        /* stage intro or a scripted scene: the game moves the fighters; we send nothing until it is over */
        const char *iw = intro_wait(b, w, me, now);
        if (iw && strcmp(iw, "fighter busy")) {
            if (iw != b->introWhy) { b->introWhy = iw; LOGI("%s: waiting: %s.", b->name, iw); }
            b->identified = 0; WHY("idle: waiting (%s)", iw); set_mode(b, M_IDLE, now, "waiting for the stage intro to end"); goto done;
        }
        b->introDone = 1;
    }
    if (b->id.pvPending && b->prevLx && w->mode == 0 && !me->scripted && me->state == CS_STANDWALK) {
        BotIdent *id = &b->id;
        id->pvSamples++;
        if (fabsf_(me->stickX) > 0.3f) { id->pvFollow++; id->pvAgree += b->prevLx * sgn(me->stickX); }
        if (id->pvSamples >= 40) {
            int agree = id->pvAgree < 0 ? -id->pvAgree : id->pvAgree;
            if (id->pvFollow >= 0.6f * id->pvSamples && agree >= 0.6f * id->pvSamples) {
                id->pvPending = 0; if (id->pvAgree < 0) b->xSign = -b->xSign;
                LOGI("%s: silent check passed: P%d's fighter (uid %u) reported our stick %d of %d frames.", b->name, b->playerSlot + 1, me->uid, agree, id->pvSamples);
            } else {
                id->ckFails++;
                LOGI("%s: silent check FAILED for P%d's fighter (uid %u): followed %d, agreed %d of %d frames. Standing still for 5 s.",
                     b->name, b->playerSlot + 1, me->uid, id->pvFollow, agree, id->pvSamples);
                if (strstr(id->how, "device order") || id->ckFails >= 3) {
                    if (id->nrej < 8) id->rejected[id->nrej++] = id->ctrlId;
                    LOGI("%s: controller %d is not ours after all; forgetting it.", b->name, id->ctrlId);
                    id->ctrlId = -1;
                }
                id->verifiedSlot = -1; id->ckRetryAt = now + 5000; id->pvPending = 0;
                b->identified = 0; neutral(o); goto done;
            }
        }
    }
    b->identified = 1;
    {   /* which character we play -> its profile */
        int pl = b->playerSlot < w->nps ? w->ps[b->playerSlot].playable : 0;
        if (pl > 0 && (!b->prof || b->profFor != pl - 1 || (!b->profNamed && strcmp(cs_name(pl - 1), "?")))) brain_set_character(b, pl - 1, cs_name(pl - 1));
    }
    g_nowMs = now; calibrate(b, me);
    floor_update(w); parts_update(w, now); retal_update(all, nall, w, now);      /* m7.4 */
    {   const World *fw = robot_world(w, me, now);                                  /* m7.4: + grounded robot parts */
        fight(b, all, nall, fw, fw == w ? me : &fw->ch[me - w->ch], now, o);
        if (b->targetUid && part_track(b->targetUid) && (b->mode == M_APPROACH || b->mode == M_ATTACK) && b->partLastAt && now > b->partLastAt && now - b->partLastAt < 500) s_botPartMs += now - b->partLastAt;
        b->partLastAt = now; }
done:
    /* final filter: Back, Guide and Start are never pressed. Outside gameplay only A (join / confirm)
       may be pressed, never B or anything else, and the sticks stay centred. */
    o->btn[BTN_BACK] = o->btn[BTN_GUIDE] = o->btn[BTN_START] = 0;
    /* m4: B (pick up / throw) only in a running stage with our own fighter identified */
    if (outKind == 4) {                     /* m7 character select back-out / un-confirm: B only */
        uint8_t bb = o->btn[BTN_B]; neutral(o); o->btn[BTN_B] = bb;
    } else if (outKind != 2 || !b->identified) o->btn[BTN_B] = 0;
    if (outKind == 4) {
    } else if (outKind == 3) {                     /* m3 character select: A, RB and the left stick only */
        PadOut k = *o; neutral(o);
        o->btn[BTN_A] = k.btn[BTN_A]; o->btn[BTN_RB] = k.btn[BTN_RB]; o->axis[AX_LX] = k.axis[AX_LX]; o->axis[AX_LY] = k.axis[AX_LY];
        o->btn[BTN_UP] = k.btn[BTN_UP]; o->btn[BTN_DOWN] = k.btn[BTN_DOWN]; o->btn[BTN_LEFT] = k.btn[BTN_LEFT]; o->btn[BTN_RIGHT] = k.btn[BTN_RIGHT];   /* m7.1 d-pad fallback */
    } else if (outKind != 2) {
        uint8_t a = outKind == 1 ? o->btn[BTN_A] : 0;
        neutral(o); o->btn[BTN_A] = a;
    }
    if (C.brainLogMs && now - b->lastDecisionLog >= (uint64_t)C.brainLogMs) {
        b->lastDecisionLog = now;
        char bt[40] = ""; int k = 0; static const char *bn = "ABXY-GS--LRudlr";
        for (int i = 0; i < BTN_COUNT; i++) if (o->btn[i] && k < 30) bt[k++] = bn[i];
        bt[k] = 0;
        LOGI("%s decision: %s | out lx=%d ly=%d buttons=%s", b->name, b->why[0] ? b->why : "-", o->axis[AX_LX], o->axis[AX_LY], k ? bt : "none");
    }
    b->prevLx = o->axis[AX_LX] > 16000 ? 1 : o->axis[AX_LX] < -16000 ? -1 : 0;
    b->lastLy = o->axis[AX_LY];
}
