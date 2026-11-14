/*
 * RageBots v1.1 (internal m8): "Bot skill: Normal" (named Mania inside the code; the user sees Normal).
 *
 * Included by sor4brain.c right before fight() (it uses that file's static helpers: walk_to, GO/seq_go, special_ok,
 * used_special, note, ...). It is NOT a separate translation unit; build.sh does not list it.
 *
 * Every function here is called only when C.botSkill == 1 ([Brain] BotSkill=1 or "Bot skill: Normal" in the F7 menu).
 * With BotSkill=0 (Easy, the default) nothing in this file runs and fight() is the v1.0 brain.
 *
 * Behaviors (from the Mania+ Axel run notes in the mania folder), using only state the mod already reads:
 *  1. keep every enemy on one side: reposition past the end of the group when enemies are on both sides;
 *     4+ enemies around: back toward the nearest screen edge / left wall (camera bounds) so all come from one side
 *  2. push the target toward the nearest screen edge: approach it from the side away from that edge (camera bounds)
 *  3. specials only with a follow-up: right after any special, a full string on a target in reach; nobody in reach ->
 *     step away on depth; no offensive special at all unless an enemy is in close range (special_ok gate)
 *  4. neutral special (Dragon Wing) as the panic button: held by an enemy, 3+ close on both sides, or one jumping in;
 *     cooldown 2.5 s and the DefensiveSpecialMinLifePercent guard (special_ok)
 *  5. dodge on depth: an enemy on our row at mid/long range that we are not engaging -> step off its row, then
 *     come in diagonally
 *  6. blitz (Axel's Grand Upper: fwd, fwd + attack) only with spacing: target at mid range on our lane, nobody else
 *     on top of us; also to catch a target knocked into the air; shorter gap than Easy
 *  7. holding an enemy with other enemies near: throw (back or forward + attack; invincible while it plays)
 *  +  forward special (Axel's Dragon Smash) right at the end of a string on a reeling target, then the follow-up string
 */

static int m_heavy(const Chr *c); static void m_seen_add(const Chr *c);   /* v1.1 stage 3 (below) */
static int m_live_enemy(const Chr *c, const Chr *me); static void m_pipe_track(const World *w, const Chr *me, uint64_t now);   /* v1.1 stage 4 (below) */

/* v1.1 stage 4: per-character specials for the SOR4 cast (mania/notes_specials.md). Ranges in attack ranges.
   fwd: 0 = string finisher only, 1 = also a gap closer at fwdLo..fwdHi, 2 = a pull at fwdLo..fwdHi (Floyd);
   air: 2 = mid-range opener / over a crowd or trap (forward held), 1 = short opener onto 2+ enemies; airLo..airHi;
   blitz band blLo..blHi; neutral360: the default neutral special hits all around (panic button).
   An equipped alternative move (PlayerState_s.alternative*) puts that slot back on the stage 3 behavior. Retro and
   unknown characters: no entry (stage 3 behavior). */
typedef struct { const char *name; int fwd; float fwdLo, fwdHi; int air; float airLo, airHi; float blLo, blHi; int altNeutralNo360; } MSpec;
static const MSpec k_mspec[] = {
    { "Axel",   0, 0.0f, 0.0f, 2, 1.0f, 3.0f, 1.05f, 2.6f, 1 },   /* Smash finisher; Dragon Dive; Grand Upper; alt Dragon Upper is not a 360 */
    { "Blaze",  0, 0.0f, 0.0f, 2, 1.0f, 2.2f, 0.8f,  2.0f, 0 },   /* Kikou Shou point blank; Tobi Kyaku opener; Hishousouzan */
    { "Cherry", 0, 0.0f, 0.0f, 1, 0.6f, 1.4f, 1.2f,  3.0f, 1 },   /* Townshend finisher; Air Townshend; Flying Knee */
    { "Floyd",  2, 1.2f, 2.2f, 1, 0.5f, 1.3f, 0.8f,  2.0f, 1 },   /* Magnetic Grab pull; Thunder Pounce; Thunder Twins */
    { "Adam",   0, 0.0f, 0.0f, 1, 0.5f, 1.5f, 0.8f,  1.8f, 0 },   /* Howl Fang finisher; Air Chopper; Uzi Punch */
    { "Max",    1, 1.5f, 3.5f, 1, 0.8f, 1.8f, 1.0f,  2.4f, 0 },   /* Thunder Tackle gap closer; Body Slam; Power Slide */
    { "Shiva",  1, 1.2f, 3.0f, 1, 0.8f, 2.0f, 0.8f,  1.8f, 0 },   /* Senretsu Ken (auto distance); Air Senretsu Ken; Final Crash */
    { "Estel",  1, 1.5f, 3.5f, 1, 0.6f, 1.2f, 1.2f,  2.6f, 1 },   /* Police Tackle onto a lone target; Air Grenade onto 2+; Boot Mark */
};
static const MSpec *m_spec(const CharProfile *pf) {
    for (size_t i = 0; i < sizeof k_mspec / sizeof *k_mspec; i++) if (pf && pf->name && !_stricmp(pf->name, k_mspec[i].name)) return &k_mspec[i];
    return NULL;
}
enum { MALT_BLITZ, MALT_NEUTRAL, MALT_FWD, MALT_AIR, MALT_STAR };
static int m_alt(const BotBrain *b, int slot) { return b->mAlt[slot] == 1; }

typedef struct {
    float range, lane, screenW, lifePct, banked;
    int nL, nR, nWide;              /* wide box (|dx| < 3.5 range, |dz| < 4 lane): enemies left / right of us */
    int fL, fR;                     /* far box (|dx| < 6 range, |dz| < 5 lane): a reposition is done when one side is empty */
    int cL, cR;                     /* close (|dx| < 1.2 range, |dz| < 1.5 lane) left / right */
    float minX, maxX, sumZ;         /* of the wide-box enemies */
    const Chr *row;                 /* not the target: on our row at mid/long range, facing us */
    const Chr *hugger;              /* not the target: right on top of us (a blitz would be hit in its startup) */
    const Chr *air;                 /* jumping at us (attacking) */
    int closeAny;                   /* an enemy in close range: offensive specials allowed */
} MCtx;

static float m_zclamp(float z) {
    if (s_fl.known) { if (z < s_fl.lo) z = s_fl.lo; if (z > s_fl.hi) z = s_fl.hi; }
    return z;
}
static int m_zroom(float z) { return !s_fl.known || (z >= s_fl.lo - 0.05f && z <= s_fl.hi + 0.05f); }
/* a depth step of 'dist' away from fromZ (the other way when the floor edge is there) */
static float m_awayz(const Chr *me, float fromZ, float dist) {
    int d = me->pos.z >= fromZ ? 1 : -1;
    float z = me->pos.z + d * dist;
    if (!m_zroom(z)) z = me->pos.z - d * dist;
    return m_zclamp(z);
}

static void mania_frame(MCtx *m, BotBrain *b, const World *w, const Chr *me, const Chr *tgt, float anchorX, uint64_t now) {
    float range = m->range, lane = m->lane;
    m->nL = m->nR = m->nWide = m->cL = m->cR = m->fL = m->fR = 0; m->minX = 1e30f; m->maxX = -1e30f; m->sumZ = 0;
    m->row = m->hugger = m->air = NULL; m->closeAny = 0;
    float rowBest = 1e30f, hugBest = 1e30f, airBest = 1e30f;
    for (int i = 0; i < w->nch; i++) {
        const Chr *c = &w->ch[i];
        if (is_enemy_of(c, me)) m_seen_add(c);                    /* v1.1 stage 3: heavies */
        if (!is_enemy_of(c, me) || c->life <= 0 || state_is_down(c->state) || c->scripted || c->untargettable || c->entered == 0 || not_released(c)) continue;
        if (fabsf_(c->pos.x - anchorX) > m->screenW * 0.75f) continue;
        if (w->haveCam && fabsf_(c->pos.x - w->cam.x) > m->screenW * 0.5f + range) continue;
        float dx = c->pos.x - me->pos.x, adx = fabsf_(dx), adz = fabsf_(c->pos.z - me->pos.z);
        if (adx < range * 1.3f && adz < lane * 1.3f) m->closeAny = 1;
        if (adx < range * 1.2f && adz < lane * 1.5f) { if (dx < 0) m->cL++; else m->cR++; }
        if (adx < range * 6.0f && adz < lane * 5.0f) {             /* level with us (|dx| < 0.5 range) counts on both sides */
            if (dx < 0 || adx < range * 0.5f) m->fL++;
            if (dx >= 0 || adx < range * 0.5f) m->fR++;
        }
        if (adx < range * 3.5f && adz < lane * 4.0f) {
            m->nWide++; if (dx < 0) m->nL++; else m->nR++;
            if (c->pos.x < m->minX) m->minX = c->pos.x;
            if (c->pos.x > m->maxX) m->maxX = c->pos.x;
            m->sumZ += c->pos.z;
        }
        if (c != tgt) {
            if (adz < lane * 0.7f && adx > range * 1.6f && adx < range * 6.0f && c->facing == -sgn(dx) && !state_is_hurt(c->state) && adx < rowBest) { rowBest = adx; m->row = c; }
            if (adx < range * 0.7f && adz < lane * 1.2f && adx < hugBest) { hugBest = adx; m->hugger = c; }
        }
        if ((c->state == CS_AIRMOVE || c->state == CS_JUMP) && c->aiAttacking && c->pos.y > 0.5f && !state_is_hurt(c->state) &&
            adx < range * 1.3f && adz < lane * 1.5f && adx < airBest && !m_heavy(c)) { airBest = adx; m->air = c; }   /* v1.1 stage 4: not a heavy's leap (the panic special lands under it) */
    }
    s_mNoOffSpecial = !m->closeAny;
    /* v1.1 stage 4: a dangerous heavy on screen (not reeling or down): no offensive specials (the green would be hit away) */
    s_mHeavyBlock = 0;
    for (int i = 0; i < w->nch; i++) { const Chr *c = &w->ch[i];
        if (!m_live_enemy(c, me) || !m_heavy(c) || state_is_hurt(c->state) || c->life <= 0) continue;
        if (w->haveCam ? fabsf_(c->pos.x - w->cam.x) <= m->screenW * 0.5f + range : fabsf_(c->pos.x - me->pos.x) < m->screenW * 0.6f) s_mHeavyBlock = 1; }
    m_pipe_track(w, me, now);
    if (!b->mLogged) {
        b->mLogged = 1;
        LOGI("%s: bot skill Normal: one-side positioning, edge push, specials only with a follow-up, neutral-special panic, depth dodges, spaced blitz, air special (dragon dive), throws, weapons only when clear.", b->name);
        const MSpec *sp = m_spec(b->prof);
        if (sp) LOGI("%s: Normal specials for %s: forward %s, air %s %.1f-%.1f ranges, blitz %.2f-%.1f ranges.", b->name, sp->name,
                     sp->fwd == 0 ? "string finisher" : sp->fwd == 1 ? "finisher + gap closer" : "pull", sp->air == 2 ? "opener" : "crowd opener", sp->airLo, sp->airHi, sp->blLo, sp->blHi);
        else LOGI("%s: Normal specials: %s character, stage 3 behavior (no per-character table entry).", b->name, b->prof && b->prof->name[0] ? b->prof->name : "retro/unknown");
    }
}

/* 4: held by an enemy: neutral special at once (the Easy brain waits 250 ms and wants 2+ enemies or 70 percent life) */
static int mania_grabbed(BotBrain *b, const Chr *me, float lifePct, float banked, int nearCount, uint64_t now, PadOut *o) {
    if (now - b->grabbedSince < 80 || !special_ok(b, 1, lifePct, now) || (b->mLastPanic && now - b->mLastPanic < 1500)) return 0;
    GO(9, 1, 0, { 90, 0, 0, SB_Y }, { 300, 0, 0, 0 });
    used_special(b, lifePct, banked, now); b->lastDefSpecial = now; b->mLastPanic = now;
    note(b, D_M_PANIC, "held by uid %u (life %.0f%%, %d near): neutral special at once", me->grabbedBy, lifePct, nearCount);
    seq_run(b, now, o); WHY("normal: held by uid %u: neutral special", me->grabbedBy); return 1;
}

/* 4: surrounded on both sides (3+ close) or an enemy jumping in: neutral special */
static int mania_panic(BotBrain *b, const MCtx *m, float lifePct, float banked, int canInterrupt, uint64_t now, PadOut *o) {
    if (!canInterrupt || !special_ok(b, 1, lifePct, now)) return 0;
    { const MSpec *sp = m_spec(b->prof); if (sp && sp->altNeutralNo360 && m_alt(b, MALT_NEUTRAL)) return 0; }   /* v1.1 stage 4: the equipped alternate does not hit all around */
    if ((b->mLastPanic && now - b->mLastPanic < 2500) || (b->lastDefSpecial && now - b->lastDefSpecial < 2500)) return 0;
    const char *why = NULL;
    if (m->cL >= 1 && m->cR >= 1 && m->cL + m->cR >= 3) why = "surrounded on both sides";
    else if (m->air) why = "an enemy jumping in";
    if (!why) return 0;
    seq_clear(b);
    GO(10, 1, 0, { 90, 0, 0, SB_Y }, { 300, 0, 0, 0 });
    used_special(b, lifePct, banked, now); b->lastDefSpecial = now; b->mLastPanic = now;
    note(b, D_M_PANIC, "%s (%d left, %d right close%s%u), life %.0f%%", why, m->cL, m->cR, m->air ? ", jumper uid " : ", uid ", m->air ? m->air->uid : 0u, lifePct);
    seq_run(b, now, o); set_mode(b, M_DEFEND, now, "normal panic"); WHY("normal: neutral special: %s", why); return 1;
}

/* 3: right after any special: the follow-up string, or a step away on depth when nobody is in reach */
static int mania_followup(BotBrain *b, const MCtx *m, const Chr *me, const Chr *tgt, const CharProfile *pf, uint64_t now, PadOut *o) {
    float range = m->range, lane = m->lane;
    if (b->specialAt && b->specialAt != b->mSpecSeen) { b->mSpecSeen = b->specialAt; b->mFuUntil = b->specialAt + 1800; b->mFuDone = 0; }
    if (b->mStepUntil && now < b->mStepUntil) {
        walk_to(b, me, me->pos.x, b->mStepZ, 1e9f, lane * 0.4f, o);
        set_mode(b, M_DEFEND, now, "normal: step away"); WHY("normal: stepping off on depth to z %.1f (special with no follow-up)", b->mStepZ); return 1;
    }
    if (!b->mFuUntil || now >= b->mFuUntil || b->mFuDone) return 0;
    if (!tgt && !m->nWide) { b->mFuDone = 1; return 0; }
    if (tgt && !state_is_down(tgt->state)) {
        float dx = tgt->pos.x - me->pos.x, adx = fabsf_(dx), adz = fabsf_(tgt->pos.z - me->pos.z);
        if (adx <= range * 1.25f && adz <= lane * 1.1f) {
            int side = sgn(dx) ? sgn(dx) : (me->facing ? me->facing : 1);
            if (adx < range * 0.35f && me->facing) side = me->facing;   /* on top of it: no turning (dx flips sign every step) */
            if (me->facing && me->facing != side) {
                if (now < b->mFuUntil - 1500) { o->axis[AX_LX] = (int16_t)(side * b->xSign * STICK); WHY("normal: turning for the follow-up"); return 1; }
                b->mFuDone = 1; return 0;                            /* didn't turn in time: the normal attack logic */
            }
            b->mFuDone = 1;
            int n = pf->comboLen > 0 ? pf->comboLen : C.comboHits; if (n > 9) n = 9; if (n < 1) n = 1;
            uint16_t per = (uint16_t)C.comboTapMs; SeqStep stp[20]; int k = 0;
            for (int i = 0; i < n; i++) { stp[k++] = (SeqStep){ (uint16_t)(per / 2), 0, 0, SB_X }; stp[k++] = (SeqStep){ (uint16_t)(per - per / 2), 0, 0, 0 }; }
            seq_go(b, 26, side, 1, stp, k, now);
            b->comboEnd = now + (uint64_t)n * per; b->nextComboAt = b->comboEnd + 60; b->wantJuggle = 1; b->comboCount++;
            b->targetUid = tgt->uid;
            note(b, D_M_FOLLOWUP, "%d-hit string on uid %u right after the special (dx %.1f)", n, tgt->uid, dx);
            seq_run(b, now, o); set_mode(b, M_ATTACK, now, NULL); WHY("normal: follow-up string on uid %u", tgt->uid); return 1;
        }
        if (adx <= range * 1.8f && adz <= lane * 1.6f) return 0;      /* nearly there: the normal approach and string */
    }
    b->mFuDone = 1;
    float fromZ = tgt ? tgt->pos.z : m->nWide ? m->sumZ / (float)m->nWide : me->pos.z;
    b->mStepZ = m_awayz(me, fromZ, lane * 3.0f); b->mStepUntil = now + 650;
    note(b, D_M_STEPAWAY, "no target in reach after the special: stepping to z %.1f", b->mStepZ);
    walk_to(b, me, me->pos.x, b->mStepZ, 1e9f, lane * 0.4f, o);
    set_mode(b, M_DEFEND, now, "normal: step away"); WHY("normal: stepping off on depth to z %.1f", b->mStepZ); return 1;
}

/* 1: enemies on both sides -> past the end of the group; 4+ around -> back to the nearest screen edge / wall */
static int mania_reposition(BotBrain *b, const MCtx *m, const World *w, const Chr *me, const Chr *tgt, uint64_t now, PadOut *o) {
    float range = m->range, lane = m->lane;
    if (!tgt || state_is_hurt(me->state)) { b->mReposUntil = 0; return 0; }
    if (b->mReposUntil) {
        int arrived = fabsf_(me->pos.x - b->mReposX) < range * 0.3f && fabsf_(me->pos.z - b->mReposZ) < lane * 0.8f;
        int solved = b->mReposKind == 1 && !(m->fL && m->fR);
        if (now >= b->mReposUntil || arrived || solved || (m->cL && m->cR)) {
            LOGD("%s: normal: reposition ends (%s; far box %d left %d right, x %.2f z %.2f)", b->name, now >= b->mReposUntil ? "time" : arrived ? "arrived" : solved ? "one side now" : "pinned on both sides", m->fL, m->fR, me->pos.x, me->pos.z);
            b->mReposUntil = 0; b->mReposCool = now + (b->mReposKind == 2 ? 4000 : 2500); return 0;
        }
        walk_to(b, me, b->mReposX, b->mReposZ, range * 0.25f, lane * 0.6f, o);
        set_mode(b, M_APPROACH, now, "normal: repositioning");
        WHY("normal: %s: to (%.1f, %.1f), %d left %d right", b->mReposKind == 2 ? "back to the edge" : "all enemies to one side", b->mReposX, b->mReposZ, m->nL, m->nR); return 1;
    }
    if (now < b->mReposCool || (m->cL && m->cR)) return 0;      /* pinned on both sides: the panic special / the fight */
    float avgZ = m->nWide ? m->sumZ / (float)m->nWide : me->pos.z;
    float gz = m_awayz(me, avgZ, lane * 2.0f);
    if (m->nWide >= 4 && w->haveCam && s_walkLo > -1e29f && s_walkHi < 1e29f) {
        float dL = me->pos.x - s_walkLo, dR = s_walkHi - me->pos.x, lim = m->screenW * 0.35f;
        float gx = 0; int ok = 0;
        if (dL > range * 0.6f && dL < lim && (m->nL <= m->nR || !(dR > range * 0.6f && dR < lim))) { gx = s_walkLo + range * 0.2f; ok = 1; }
        else if (dR > range * 0.6f && dR < lim) { gx = s_walkHi - range * 0.2f; ok = 1; }
        if (ok) {
            b->mReposKind = 2; b->mReposX = gx; b->mReposZ = m_zclamp(me->pos.z); b->mReposSince = now; b->mReposUntil = now + 2200;
            note(b, D_M_WALL, "%d enemies around (%d left, %d right): backing to the %s edge at x %.1f", m->nWide, m->nL, m->nR, gx < me->pos.x ? "left" : "right", gx);
            walk_to(b, me, b->mReposX, b->mReposZ, range * 0.25f, lane * 0.6f, o);
            set_mode(b, M_APPROACH, now, "normal: repositioning"); WHY("normal: back to the edge at x %.1f", gx); return 1;
        }
    }
    if (m->nL && m->nR) {
        float gL = m->minX - range * 1.0f, gR = m->maxX + range * 1.0f;
        int okL = gL >= s_walkLo && fabsf_(gL - me->pos.x) < m->screenW * 0.45f, okR = gR <= s_walkHi && fabsf_(gR - me->pos.x) < m->screenW * 0.45f;
        float gx;
        if (okL && (!okR || m->nL <= m->nR)) gx = gL; else if (okR) gx = gR; else return 0;
        b->mReposKind = 1; b->mReposX = gx; b->mReposZ = gz; b->mReposSince = now; b->mReposUntil = now + 1500;
        note(b, D_M_REPOS, "enemies on both sides (%d left, %d right): to x %.1f z %.1f so all are on one side", m->nL, m->nR, gx, gz);
        walk_to(b, me, b->mReposX, b->mReposZ, range * 0.25f, lane * 0.6f, o);
        set_mode(b, M_APPROACH, now, "normal: repositioning"); WHY("normal: all enemies to one side: to (%.1f, %.1f)", gx, gz); return 1;
    }
    return 0;
}

/* 5: an enemy we are not engaging on our row at mid/long range: off its row first, then in diagonally */
static int mania_rowdodge(BotBrain *b, const MCtx *m, const Chr *me, const Chr *tgt, uint64_t now, PadOut *o) {
    float range = m->range, lane = m->lane;
    if (!tgt) { b->mRowUntil = 0; return 0; }
    float dx = tgt->pos.x - me->pos.x; int side = sgn(dx) ? sgn(dx) : 1;
    int engaged = fabsf_(dx) <= range * 1.2f && fabsf_(tgt->pos.z - me->pos.z) <= lane;
    if (b->mRowUntil) {
        if (now >= b->mRowUntil || engaged || fabsf_(me->pos.z - b->mRowZ) < lane * 0.5f) { b->mRowUntil = 0; b->mRowCool = now + 1200; return 0; }
        walk_to(b, me, me->pos.x + (C.botSkill == 1 && now < b->mReposCool ? 0.0f : side * range * 0.5f), b->mRowZ, range * 0.2f, lane * 0.4f, o);   /* v1.1 stage 4: just after a reposition, depth only (run3 17:08:28 stutter) */
        set_mode(b, M_APPROACH, now, "normal: off a row"); WHY("normal: off the row of uid %u to z %.1f, then in diagonally", b->mRowUid, b->mRowZ); return 1;
    }
    const Chr *r = m->row;
    if (!r || engaged || now < b->mRowCool) return 0;
    if (!(fabsf_(tgt->pos.z - r->pos.z) > lane * 1.2f || r->aiAttacking || r->state == CS_DASH)) return 0;   /* the target is on that row too: just go */
    float up = r->pos.z + lane * 2.2f, dn = r->pos.z - lane * 2.2f;
    int okU = m_zroom(up), okD = m_zroom(dn);
    if (!okU && !okD) return 0;
    float z = !okD || (okU && fabsf_(up - tgt->pos.z) <= fabsf_(dn - tgt->pos.z)) ? up : dn;
    b->mRowZ = m_zclamp(z); b->mRowUid = r->uid; b->mRowUntil = now + 1000;
    note(b, D_M_ROWDODGE, "uid %u on our row at dx %.1f%s: stepping to z %.1f (target uid %u)", r->uid, r->pos.x - me->pos.x, r->aiAttacking ? " (attacking)" : "", b->mRowZ, tgt->uid);
    walk_to(b, me, me->pos.x + (C.botSkill == 1 && now < b->mReposCool ? 0.0f : side * range * 0.5f), b->mRowZ, range * 0.2f, lane * 0.4f, o);   /* v1.1 stage 4: just after a reposition, depth only (run3 17:08:28 stutter) */
    set_mode(b, M_APPROACH, now, "normal: off a row"); WHY("normal: off the row of uid %u", r->uid); return 1;
}

/* 2: push the target toward the nearest screen edge (wall juggles): returns the push direction (-1 left, +1 right, 0 none)
   and where to stand; *wrong = we are on the edge side of it and have to come around (2.5 s at most per target) */
static int mania_push_plan(BotBrain *b, const MCtx *m, const World *w, const Chr *me, const Chr *tgt, float camLo, float camHi, float *stand, int *wrong, uint64_t now) {
    float range = m->range;
    *wrong = 0;
    if (!w->haveCam || camLo < -1e29f || camHi > 1e29f || tgt->aiAttacking || tgt->robotPart) return 0;
    float dLo = tgt->pos.x - camLo, dHi = camHi - tgt->pos.x;
    int push = dLo <= dHi ? -1 : 1; float dEdge = push < 0 ? dLo : dHi;
    float st = tgt->pos.x - push * range * 0.7f;
    int ok = dEdge <= m->screenW * 0.3f && st >= s_walkLo && st <= s_walkHi &&
             !(push < 0 ? m->maxX > st + range * 0.3f : m->minX < st - range * 0.3f);   /* no other enemy would end up behind us */
    /* hysteresis: the checks depend on our own position (the enemy box moves with us); keep a plan for 0.5 s */
    if (!ok && b->mPushUid == tgt->uid && b->mPushSide && now - b->mPushOkAt < 500) {
        push = b->mPushSide; st = tgt->pos.x - push * range * 0.7f; ok = st >= s_walkLo && st <= s_walkHi;
    }
    if (!ok) return 0;
    if (b->mPushUid != tgt->uid) { b->mPushUid = tgt->uid; b->mPushSince = now;
        if (b->mPushNoteUid != tgt->uid) { b->mPushNoteUid = tgt->uid; note(b, D_M_PUSH, "uid %u %.1f from the %s edge: coming from the other side to pin it", tgt->uid, dEdge, push < 0 ? "left" : "right"); } }
    b->mPushSide = push; b->mPushOkAt = now;
    int wr = sgn(me->pos.x - tgt->pos.x) == push;
    if (wr && now - b->mPushSince > 2500) return 0;                   /* couldn't get around it: the Easy approach */
    *stand = st; *wrong = wr;
    return push;
}
static void mania_approach_goal(BotBrain *b, const MCtx *m, const World *w, const Chr *me, const Chr *tgt, float camLo, float camHi, float *goalX, float *goalZ, uint64_t now) {
    float st, lane = m->lane; int wrong;
    if (!mania_push_plan(b, m, w, me, tgt, camLo, camHi, &st, &wrong, now)) return;
    *goalX = st;
    *goalZ = wrong && fabsf_(me->pos.x - tgt->pos.x) < m->range * 1.5f + 0.5f ? m_zclamp(tgt->pos.z + (me->pos.z >= tgt->pos.z ? 1.0f : -1.0f) * lane * 3.0f) : tgt->pos.z;
}

/* 6: the blitz (Axel: Grand Upper) with spacing, or to catch a target knocked into the air */
static int mania_gap(BotBrain *b, const MCtx *m, const World *w, const Chr *me, const CharProfile *pf, const Chr *tgt, int side, int facing, int shFront,
                     float adx, float adz, float camLo, float camHi, uint64_t now, PadOut *o) {
    float range = m->range, lane = m->lane;
    { float st; int wrong; if (mania_push_plan(b, m, w, me, tgt, camLo, camHi, &st, &wrong, now) && wrong) return 0; }   /* coming around first */
    if (!pf->blitz || shFront || state_is_down(tgt->state) || tgt->robotPart || tgt->pos.x <= camLo || tgt->pos.x >= camHi || m->hugger) return 0;
    uint64_t gap = b->aggression == 2 ? 900 : b->aggression == 0 ? 2000 : 1300;
    int air = tgt->state == CS_AIRHIT || tgt->state == CS_AIRLIGHTHIT;
    int kind = 0;
    if (air && adx >= range * 0.6f && adx <= range * 2.2f && adz <= lane * 0.8f && now - b->lastBlitz > 900) kind = 2;
    float blLo = 1.05f, blHi = 2.6f;   /* v1.1 stage 4: the character's blitz reach (stage 3 band with an alternate blitz or no table entry) */
    { const MSpec *sp = m_spec(b->prof); if (sp && !m_alt(b, MALT_BLITZ)) { blLo = sp->blLo; blHi = sp->blHi; } }
    if (kind == 0 && !air && facing && adz <= lane * 0.6f && adx >= range * blLo && adx <= range * blHi && now - b->lastBlitz > gap &&
             !(m_heavy(tgt) && !state_is_hurt(tgt->state) && tgt->facing == -side)) kind = 1;   /* v1.1 stage 3: not into a heavy's face */
    if (!kind) return 0;
    GO(22, side, 1, { 50, F, 0, 0 }, { 40, 0, 0, 0 }, { 80, F, 0, SB_X }, { 450, 0, 0, 0 });
    b->lastBlitz = now; b->mLastGU = now; b->nextComboAt = 0;
    if (pf->blitzThenGrab) b->lastGrabTry = 0;
    note(b, kind == 2 ? D_M_GUCATCH : D_M_GU, "uid %u at dx %.1f dz %.1f%s", tgt->uid, side * adx, adz, kind == 2 ? " (in the air)" : "");
    seq_run(b, now, o); set_mode(b, M_ATTACK, now, NULL); WHY("normal: %s at uid %u", kind == 2 ? "blitz catch" : "spaced blitz", tgt->uid); return 1;
}

/* +: forward special (Axel: Dragon Smash) right after a string on a reeling target; the follow-up string comes next */
static int mania_smash(BotBrain *b, const CharProfile *pf, const Chr *tgt, int side, int shFront, float lifePct, float banked, uint64_t now, PadOut *o) {
    if ((pf->grabStyle == GS_FLOYD && !m_alt(b, MALT_FWD)) || shFront || tgt->robotPart || state_is_down(tgt->state)) return 0;   /* v1.1 stage 4: Floyd's alternate (Rakushin Cannon) is point blank */
    if (!b->comboEnd || now < b->comboEnd || now > b->comboEnd + 300) return 0;
    if (!(tgt->state == CS_STANDHIT || tgt->state == CS_AIRHIT || tgt->state == CS_AIRLIGHTHIT)) return 0;
    if ((b->mLastSmash && now - b->mLastSmash < 3000) || !special_ok(b, 0, lifePct, now)) return 0;
    if (lifePct < 50.0f) return 0;                                       /* v1.1 stage 3: the special budget */
    GO(18, side, 1, { 90, F, 0, SB_Y }, { 550, 0, 0, 0 });
    used_special(b, lifePct, banked, now); b->mLastSmash = now; b->lastFwdSpecial = now;
    note(b, D_M_SMASH, "uid %u reeling (%s) after the string, life %.0f%%", tgt->uid, state_name(tgt->state), lifePct);
    seq_run(b, now, o); set_mode(b, M_ATTACK, now, NULL); WHY("normal: forward special on uid %u", tgt->uid); return 1;
}

/* 7: holding an enemy with others near: throw it (invincible while the throw plays) instead of more knees */
static int mania_throw(BotBrain *b, const World *w, const Chr *me, const CharProfile *pf, int dir, int front, int back, float range, float lane, uint64_t now, PadOut *o) {
    if (pf->grabStyle == GS_MAX) return 0;                            /* Max: his piledriver stays */
    int nearN = 0, closeN = 0;
    for (int i = 0; i < w->nch; i++) {
        const Chr *c = &w->ch[i];
        if (!is_enemy_of(c, me) || c->uid == me->grabUid || state_is_down(c->state) || c->life <= 0) continue;
        float adx = fabsf_(c->pos.x - me->pos.x), adz = fabsf_(c->pos.z - me->pos.z);
        if (adx < range * 2.5f && adz < lane * 3.0f) nearN++;
        if (adx < range * 1.3f && adz < lane * 1.5f) closeN++;
    }
    if (!nearN) return 0;
    if (!closeN && b->grabHits < 1) return 0;                         /* nobody about to hit us: one knee first */
    if (back > front) GO(13, dir, 0, { 80, BK, 0, SB_X }, { 550, 0, 0, 0 });
    else GO(13, dir, 0, { 80, F, 0, SB_X }, { 550, 0, 0, 0 });
    note(b, D_M_THROW, "uid %u thrown %s (%d near, %d close, %d behind, %d in front)", me->grabUid, back > front ? "over the shoulder" : "forward", nearN, closeN, back, front);
    seq_run(b, now, o); WHY("normal: grab: throw %s", back > front ? "back" : "forward"); return 1;
}

/* v1.1 stage 2: the air special (jump, then special in the air). Axel's is Dragon Dive: a flaming roll down and
   forward that ends in a dragon punch, so it closes a gap, opens on a target at mid range on our lane and goes over a
   ground crowd or a live floor trap between us and the target. The other SOR4 characters' air specials are used only
   as a short-range opener on a crowd (their reach is not measured); retro characters and unknown ones: never.
   Offensive-special life threshold (special_ok), a cooldown by aggression, the shared jump budget (jump_ok: anti-air
   bans), and the follow-up string after it lands (mania_followup sees the special through used_special). */
static int m_air_kind(const BotBrain *b, const CharProfile *pf, float *lo, float *hi) {
    /* v1.1 stage 4: per character (k_mspec); an equipped alternate air special: the stage 3 short crowd opener */
    const MSpec *sp = m_spec(pf);
    *lo = 0.5f; *hi = 1.3f;
    if (!sp) return 0;
    if (m_alt(b, MALT_AIR)) return 1;
    *lo = sp->airLo; *hi = sp->airHi;
    return sp->air;
}
/* a live trap zone across our lane between us and x1 */
static int m_trap_between(const Chr *me, float x1, float lane, uint64_t now) {
    float lo = me->pos.x < x1 ? me->pos.x : x1, hi = me->pos.x < x1 ? x1 : me->pos.x;
    for (int i = 0; i < s_ntz; i++) { const TrapZone *t = &s_tz[i];
        if (!tz_active(t, now) || t->partUid) continue;
        if (t->x1 < lo || t->x0 > hi || me->pos.z < t->z0 - lane * 0.5f || me->pos.z > t->z1 + lane * 0.5f) continue;
        if (t->x1 - t->x0 > 3.0f) continue;                       /* too wide to clear in one dive */
        return 1;
    }
    return 0;
}
static int mania_dive(BotBrain *b, const MCtx *m, const World *w, const Chr *me, const CharProfile *pf, const Chr *tgt, int side, int facing, int shFront,
                      float adx, float adz, float lifePct, float banked, float camLo, float camHi, uint64_t now, PadOut *o) {
    float range = m->range, lane = m->lane;
    float aLo, aHi; int kind = m_air_kind(b, pf, &aLo, &aHi);
    if (!kind || !C.jumpAttacks || shFront || !facing || tgt->robotPart || state_is_down(tgt->state) || tgt->state == CS_AIRHIT || tgt->state == CS_AIRLIGHTHIT) return 0;
    if (tgt->pos.x <= camLo || tgt->pos.x >= camHi || adz > lane * 0.7f || state_is_hurt(me->state)) return 0;
    uint64_t gap = b->aggression == 2 ? 3500 : b->aggression == 0 ? 7000 : 5000;
    if (b->mLastDive && now - b->mLastDive < gap) return 0;
    if (!jump_ok(b, now, tgt->uid)) return 0;
    { float st; int wrong; if (mania_push_plan(b, m, w, me, tgt, camLo, camHi, &st, &wrong, now) && wrong) return 0; }   /* coming around first */
    /* ground enemies (not the target) between us and the target on our lane */
    int crowd = 0;
    for (int i = 0; i < w->nch; i++) { const Chr *c = &w->ch[i];
        if (c == tgt || !is_enemy_of(c, me) || c->life <= 0 || state_is_down(c->state) || c->scripted || c->untargettable || c->entered == 0) continue;
        float cdx = (c->pos.x - me->pos.x) * side;
        if (cdx > range * 0.2f && cdx < adx && fabsf_(c->pos.z - me->pos.z) < lane * 1.5f && c->pos.y < 0.5f) crowd++;
    }
    const char *why = NULL;
    if (kind == 2) {
        float oLo = aLo > 1.0f ? aLo + 0.6f * (aHi - aLo) * 0.5f : 1.6f, oHi = aHi - 0.2f;   /* Axel: 1.6-2.8 (stage 2/3); Blaze: 1.6-2.0 */
        if (oLo > oHi) oLo = oHi;
        if (adx < range * aLo || adx > range * aHi || m->hugger) return 0;
        if (m_trap_between(me, tgt->pos.x, lane, now)) why = "over a floor trap";
        else if (crowd >= 2 && adx <= range * oHi) why = "over the crowd in front";
        else if (adx >= range * oLo && adx <= range * oHi) why = "opener at mid range";
    } else {
        if (adx < range * aLo || adx > range * aHi || crowd + 1 < 2 || m->hugger) return 0;
        why = "opener on a crowd";
    }
    if (!why) return 0;
    if (tgt->boss && !strcmp(why, "opener at mid range") && tgt->aiAttacking) return 0;   /* a boss mid-swing: not into it */
    { int sv = s_mNoOffSpecial; s_mNoOffSpecial = 0; int ok = special_ok(b, 0, lifePct, now); s_mNoOffSpecial = sv; if (!ok) return 0; }
    if (lifePct < (float)C.specialMinLifePct) return 0;
    if (lifePct < 70.0f || m->cL + m->cR >= 3) return 0;                 /* v1.1 stage 3: the special budget */
    if (m_heavy(tgt) && !state_is_hurt(tgt->state) && tgt->facing == -side) return 0;   /* not into a heavy's face */
    if (kind == 2) GO(38, side, 0, { 70, F, 0, SB_A }, { 150, F, 0, 0 }, { 80, F, 0, SB_Y }, { 650, F, 0, 0 }, { 150, 0, 0, 0 });
    else GO(38, side, 0, { 70, 0, 0, SB_A }, { 160, 0, 0, 0 }, { 80, 0, 0, SB_Y }, { 550, 0, 0, 0 });
    used_jump(b, 38, now); used_special(b, lifePct, banked, now); b->mLastDive = now; b->lastAirSpecial = now; b->nextComboAt = 0;
    note(b, D_M_DIVE, "%s at uid %u (dx %.1f dz %.1f, %d in between, life %.0f%%)", why, tgt->uid, side * adx, adz, crowd, lifePct);
    seq_run(b, now, o); set_mode(b, M_ATTACK, now, NULL); WHY("normal: air special %s at uid %u", why, tgt->uid); return 1;
}

/* v1.1 stage 2: a weapon pickup only with nobody about to hit us (v1.1-run1: 19 weapon pickups on Hardest, 4 of the
   29 hits taken were on the way to one or while picking it up); Easy keeps its own rule */
static int mania_weapon_ok(const World *w, const Chr *me, const Pick *p, float range, float lane) {
    /* v1.1 stage 3: no pickups while a heavy is around (Koobo pulls floor weapons in and parries thrown ones) */
    for (int i = 0; i < w->nch; i++) if (is_enemy_of(&w->ch[i], me) && m_heavy(&w->ch[i]) && w->ch[i].entered != 0) return 0;
    for (int i = 0; i < w->nch; i++) { const Chr *c = &w->ch[i];
        if (!is_enemy_of(c, me) || c->life <= 0 || state_is_down(c->state) || c->scripted || c->untargettable || c->entered == 0) continue;
        float ex = c->pos.x, ez = c->pos.z;
        if (fabsf_(ex - me->pos.x) < range * 2.2f && fabsf_(ez - me->pos.z) < lane * 3.0f) return 0;
        if (fabsf_(ex - p->pos.x) < range * 1.8f && fabsf_(ez - p->pos.z) < lane * 3.0f) return 0;
    }
    return 1;
}

/* ======================================================================================================================
 * v1.1 stage 3 (v1.1-run2, Hardest stage 1, three Normal Axel bots): the two Koobos (420 HP each, pipe swing, pipe
 * throw, super-jump slam, floating pipes he pulls in and shoots along a row) killed P1 twice, P2 and P4; P2 and P4 had
 * died before the arena at ~20% life, walking straight back in along a grunt's row right after the 2.5 s retreat.
 * 27 of the 42 hits tied to an attacker landed in front of it on its row. Everything below is Normal only.
 * ==================================================================================================================== */

/* heavy enemies: the boss flag, CharacterData.maxLife or a life seen >= 300 (Koobo reads boss=0) */
static struct { uint32_t uid; float maxSeen; } s_hvSeen[32]; static int s_nhvSeen, s_hvNext;
static float m_seen(const Chr *c) { for (int i = 0; i < s_nhvSeen; i++) if (s_hvSeen[i].uid == c->uid) return s_hvSeen[i].maxSeen; return 0; }
static void m_seen_add(const Chr *c) {
    if (c->player >= 0 || c->life < 300) return;
    for (int i = 0; i < s_nhvSeen; i++) if (s_hvSeen[i].uid == c->uid) { if (c->life > s_hvSeen[i].maxSeen) s_hvSeen[i].maxSeen = c->life; return; }
    int k = s_nhvSeen < 32 ? s_nhvSeen++ : (s_hvNext++ % 32);
    s_hvSeen[k].uid = c->uid; s_hvSeen[k].maxSeen = c->life;
}
static int m_heavy(const Chr *c) { return c && c->player < 0 && !c->robotPart && (c->boss || c->dataMaxLife >= 300 || c->life >= 300 || m_seen(c) >= 300); }
static int m_live_enemy(const Chr *c, const Chr *me) {
    return is_enemy_of(c, me) && !state_is_down(c->state) && !c->scripted && !c->untargettable && c->entered != 0 && !not_released(c);
}
/* (x, z) in a heavy's danger lane: in front of it on its row out to ~8 units (6.5 ranges: swing, pipe throw, pulled
   weapons), or close to it while it attacks. In reach of an idle heavy we are fighting: allowed (the string). */
static const Chr *m_heavy_lane(const World *w, const Chr *me, float x, float z, const Chr *tgt, float range, float lane) {
    for (int i = 0; i < w->nch; i++) { const Chr *h = &w->ch[i];
        if (!m_live_enemy(h, me) || !m_heavy(h) || state_is_hurt(h->state)) continue;
        float dx = x - h->pos.x, adx = fabsf_(dx), adz = fabsf_(z - h->pos.z);
        int front = h->facing == 0 || dx * (float)h->facing > 0;
        int busy = h->aiAttacking || h->moveId >= 0;
        if (front && adz < lane * 1.6f && adx < range * 6.5f) {
            if (adx <= range * 1.3f && !busy && tgt == h) continue;
            return h;
        }
        if (busy && adx < range * 1.6f && adz < lane * 2.5f && (front || adx < range * 0.6f)) return h;
        /* v1.1 stage 4: swinging a pipe (weapon in hand) reaches ~5 units in front (v1.1-run3: move 0 hits at dx 1.9-5.0) */
        if (C.botSkill == 1 && busy && h->heldAddr && front && adx < range * 4.2f && adz < lane * 2.2f) return h;
    }
    return NULL;
}
static int m_players_near(const World *w, const Chr *me, float x, float z, float rx, float rz) {
    int n = 0;
    for (int i = 0; i < w->nch; i++) { const Chr *p = &w->ch[i];
        if (p->player < 0 || p->uid == me->uid || p->life <= 0 || state_is_down(p->state)) continue;
        if (fabsf_(p->pos.x - x) < rx && fabsf_(p->pos.z - z) < rz) n++;
    }
    return n;
}
/* a z off row cz (lane*off away), our side first; not into another danger lane, fewer players there */
static float m_offrow_z(const World *w, const Chr *me, float cz, float off, int pref, const Chr *tgt, float range, float lane, int *signOut) {
    float best = me->pos.z; int bs = pref, bestScore = 1 << 30;
    for (int k = 0; k < 2; k++) {
        int sg = k == 0 ? pref : -pref;
        float z = m_zclamp(cz + sg * off);
        int score = 0;
        if (fabsf_(z - cz) < lane * 1.9f) score += 100;
        if (m_heavy_lane(w, me, me->pos.x, z, tgt, range, lane)) score += 10;
        score += 3 * m_players_near(w, me, me->pos.x, z, range * 1.2f, lane * 1.5f);
        if (score < bestScore) { bestScore = score; best = z; bs = sg; }
    }
    if (signOut) *signOut = bs;
    return best;
}

/* ======================================================================================================================
 * v1.1 stage 4 (v1.1-run3, Hardest, four Normal Axels): Normal only.
 * ==================================================================================================================== */

/* enemy objects: when we first saw each (Koobo's pipes rise from the floor at y ~0.3, hover ~2-2.7 s at y ~2.2-2.6,
   then shoot at a player in any direction, mostly along depth at ~0.5-0.8 per frame: no time to dodge in flight) */
static struct { uint32_t uid; uint64_t first, last; } s_pjT[32]; static int s_npjT;
static void m_pipe_track(const World *w, const Chr *me, uint64_t now) {
    int k = 0; for (int i = 0; i < s_npjT; i++) if (now - s_pjT[i].last < 1500 && now >= s_pjT[i].last) s_pjT[k++] = s_pjT[i]; s_npjT = k;
    for (int i = 0; i < w->npj; i++) { const Proj *p = &w->pj[i]; if (p->team == me->team || !p->uid) continue;
        int j = 0; for (; j < s_npjT; j++) if (s_pjT[j].uid == p->uid) break;
        if (j < s_npjT) { s_pjT[j].last = now; continue; }
        if (s_npjT < 32) { s_pjT[s_npjT].uid = p->uid; s_pjT[s_npjT].first = s_pjT[s_npjT].last = now; s_npjT++; } }
}
static uint64_t m_pj_age(uint32_t uid, uint64_t now) { for (int i = 0; i < s_npjT; i++) if (s_pjT[i].uid == uid) return now - s_pjT[i].first; return 0; }
void dbg_mania_pipes_reset(void) { s_npjT = 0; }
/* how dangerous enemy objects make (x, z): 2 = on a flying object's path in the next ~0.6 s, or next to a fresh hovering
   (or still rising: the low ones) object that is about to shoot; 1 = on a hovering object's row or column further out */
static int m_obj_danger(const World *w, const Chr *me, float x, float z, float range, float lane, uint64_t now, const Proj **out) {
    int best = 0; if (out) *out = NULL;
    for (int i = 0; i < w->npj; i++) { const Proj *p = &w->pj[i];
        if (p->team == me->team || p->pos.y > 4.5f) continue;
        float sp2 = p->vel.x * p->vel.x + p->vel.z * p->vel.z, adx = fabsf_(x - p->pos.x), adz = fabsf_(z - p->pos.z);
        int d = 0;
        if (sp2 > 0.03f * 0.03f) {                                   /* flying: along its velocity (x AND depth) */
            if (p->pos.y < 0.4f && p->vel.y < 0) continue;           /* falling onto the floor */
            for (int t = 0; t <= 36 && d < 2; t += 3) {
                float px = p->pos.x + p->vel.x * (float)t, pz = p->pos.z + p->vel.z * (float)t;
                if (fabsf_(px - x) < 0.9f && fabsf_(pz - z) < lane * 1.6f) d = 2;
            }
        } else {                                                     /* hovering / rising: about to shoot at somebody near */
            uint64_t age = m_pj_age(p->uid, now);
            if (age > 4500) continue;
            if (adx < 3.0f && adz < 1.3f) d = 2;
            else if ((adz < lane * 1.8f && adx < range * 7.0f) || (adx < 1.0f && adz < 4.0f)) d = 1;
        }
        if (d > best) { best = d; if (out) *out = p; }
    }
    return best;
}

/* the green-health lock and the equipped alternates (called every fight() frame, Normal only) */
static void mania_green(BotBrain *b, const PlayerStateInfo *ps, float banked, uint64_t now) {
    s_mGreen = banked > 0.5f;
    if (b->greenKnown && banked > 0.5f && b->specialAt && now - b->specialAt < 15000 && b->recoverUntil < now + 100) {
        b->recoverUntil = now + 100;
        if (now - b->specialAt > 8000 && (!b->mGreenNoteAt || now - b->mGreenNoteAt > 10000)) { b->mGreenNoteAt = now;
            note(b, D_M_GREEN, "green health %.1f still showing %.1f s after our special: no new special until it is back", banked, (float)(now - b->specialAt) / 1000.0f); }
    }
    int a[5]; for (int k = 0; k < 5; k++) a[k] = ps ? ps->alt[k] : -1;
    int changed = !b->mAltLogged; for (int k = 0; k < 5; k++) if ((a[k] == 1) != (b->mAlt[k] == 1)) changed = 1;
    if (!changed) return;
    b->mAltLogged = 1;
    if (a[0] < 0) { for (int k = 0; k < 5; k++) b->mAlt[k] = 0;
        LOGI("%s: equipped alternative moves not readable: assuming the default moves for every special.", b->name); return; }
    for (int k = 0; k < 5; k++) b->mAlt[k] = a[k] == 1;
    LOGI("%s: special moves equipped: blitz %s, neutral %s, forward %s, air %s, star %s%s", b->name,
         a[0] == 1 ? "ALT" : "default", a[1] == 1 ? "ALT" : "default", a[2] == 1 ? "ALT" : "default", a[3] == 1 ? "ALT" : "default", a[4] == 1 ? "ALT" : "default",
         a[0] == 1 || a[1] == 1 || a[2] == 1 || a[3] == 1 ? " (alternate slots use the stage 3 rules)" : "");
}

/* hurt with a hazard object as the hit source (v1.1-run3 section 2: traps 13/14/15/19, zone 0, anim 0; the old trap
   check needs no enemy near for 0.7 s, so it never fired in a fight): its no-walk zone at once, for every bot. The
   hit source is sticky (it stays the last thing that hit us), so the hazard must also be right where we are. */
static void mania_trap_src(BotBrain *b, const World *w, const Chr *me, uint64_t now) {
    if (!me->hitSrc) return;
    const Haz *hz = NULL;
    for (int i = 0; i < w->nhz; i++) if (w->hz[i].uid == me->hitSrc && fabsf_(w->hz[i].pos.x - me->pos.x) < 2.5f && fabsf_(w->hz[i].pos.z - me->pos.z) < 1.2f) hz = &w->hz[i];
    if (!hz) return;
    TrapZone nz; memset(&nz, 0, sizeof nz);
    if (hz->haveZone) { nz.x0 = hz->x0 - 0.3f; nz.x1 = hz->x1 + 0.3f; nz.z0 = hz->z0 - 0.3f; nz.z1 = hz->z1 + 0.3f; }
    else { nz.x0 = mnf(hz->pos.x, me->pos.x) - 1.3f; nz.x1 = mxf(hz->pos.x, me->pos.x) + 1.3f; nz.z0 = mnf(hz->pos.z, me->pos.z) - 0.6f; nz.z1 = mxf(hz->pos.z, me->pos.z) + 0.6f; }
    nz.hazUid = hz->uid; nz.hotAnim = hz->anim; nz.until = now + 60000;
    for (int i = 0; i < s_ntz; i++) { TrapZone *t = &s_tz[i];
        if (t->hazUid != hz->uid) continue;
        float x0 = mnf(t->x0, nz.x0), x1 = mxf(t->x1, nz.x1), z0 = mnf(t->z0, nz.z0), z1 = mxf(t->z1, nz.z1);
        if (x1 - x0 <= 6.0f && z1 - z0 <= 3.0f) { t->x0 = x0; t->x1 = x1; t->z0 = z0; t->z1 = z1; }
        t->until = nz.until; t->hotAnim = nz.hotAnim;
        note(b, D_M_TRAPSRC, "hit by hazard uid %u again at (%.1f, %.1f): zone x %.1f..%.1f z %.1f..%.1f (60 s)", hz->uid, me->pos.x, me->pos.z, t->x0, t->x1, t->z0, t->z1);
        return;
    }
    int k = s_ntz < MAXTZ ? s_ntz++ : 0;
    if (k == 0 && s_ntz == MAXTZ) for (int i = 1; i < MAXTZ; i++) if (s_tz[i].until < s_tz[k].until) k = i;
    s_tz[k] = nz;
    note(b, D_M_TRAPSRC, "hit by hazard uid %u at (%.1f, %.1f) (hazard at %.1f, %.1f): no walking or pickups in x %.1f..%.1f z %.1f..%.1f for 60 s (all bots)",
         hz->uid, me->pos.x, me->pos.z, hz->pos.x, hz->pos.z, nz.x0, nz.x1, nz.z0, nz.z1);
}
/* a pickup (money, food, weapon, star) inside a live no-walk zone: left alone */
static int mania_pick_in_trap(BotBrain *b, const Pick *p, uint64_t now) {
    const TrapZone *t = tz_at(p->pos.x, p->pos.z, 0.25f, now);
    if (!t || t->partUid) return 0;
    if (!b->mPickTrapNoteAt || now - b->mPickTrapNoteAt > 5000) { b->mPickTrapNoteAt = now;
        note(b, D_M_PICKTRAP, "%s at (%.1f, %.1f) lies in a trap zone (x %.1f..%.1f z %.1f..%.1f): left alone", p->kind == PK_FOOD ? "food" : p->kind == PK_SCORE ? "money" : p->kind == PK_WEAPON ? "weapon" : "pickup",
             p->pos.x, p->pos.z, t->x0, t->x1, t->z0, t->z1); }
    return 1;
}

/* a heavy up in its leap (Koobo move 4, y up to ~15; the landing, move 5, hit |dx| 1.1-3.4 and |dz| <= 0.66 in
   v1.1-run3, and bots had left the spot by only 0-2.4): the danger is a wide, shallow box around the spot, so the
   short way out is usually on depth. No approach while it is up; the target waits outside, facing it. */
static int mania_leap(BotBrain *b, const MCtx *m, const World *w, const Chr *me, const Chr *slam, const Chr *tgt, uint64_t now, PadOut *o) {
    float range = m->range, lane = m->lane;
    const float RX = 4.2f, RZ = 1.1f;
    s_slamOn = 1; s_slamEll = 1; s_slamX = slam->pos.x; s_slamZ = slam->pos.z; s_slamRX = RX; s_slamRZ = RZ; s_slamR = RX;
    b->mLeapBX = slam->pos.x; b->mLeapBZ = slam->pos.z; b->mLeapAt = now;   /* the retreat keeps out of it too */
    float dx = me->pos.x - slam->pos.x, dz = me->pos.z - slam->pos.z;
    uint32_t key = (uint32_t)b->slamSince ^ slam->uid;
    int atExit = b->mLeapUid == key && fabsf_(b->mLeapX - me->pos.x) < 0.2f && fabsf_(b->mLeapZ - me->pos.z) < 0.12f;
    float hx = b->mLeapUid == key && !atExit ? 0.4f : 0.0f, hz = b->mLeapUid == key && !atExit ? 0.3f : 0.0f;   /* hysteresis: once on the way out, all the way out */
    int inside = fabsf_(dx) < RX + hx && fabsf_(dz) < RZ + hz;
    int exitBad = b->mLeapUid == key && (!m_zroom(b->mLeapZ) || b->mLeapX < s_walkLo || b->mLeapX > s_walkHi);   /* floor edge / walk limit learned meanwhile */
    if (inside && !state_is_hurt(me->state)) {
        if (b->mLeapUid != key || atExit || exitBad) {
            float ex[4] = { me->pos.x, me->pos.x, slam->pos.x + RX + 0.4f, slam->pos.x - RX - 0.4f };
            float ez[4] = { m_zclamp(slam->pos.z + RZ + 0.35f), m_zclamp(slam->pos.z - RZ - 0.35f), me->pos.z, me->pos.z };
            float best = 1e30f; int bi = 0;
            for (int k = 0; k < 4; k++) {
                float c = k < 2 ? fabsf_(ez[k] - me->pos.z) * 1.8f : fabsf_(ex[k] - me->pos.x);
                if (k < 2 && fabsf_(ez[k] - slam->pos.z) < RZ + 0.05f) c += 1000.0f;   /* the floor ends inside the box on that side */
                if (k >= 2 && (ex[k] < s_walkLo || ex[k] > s_walkHi)) c += 1000.0f;
                if (tz_at(ex[k], ez[k], 0.2f, now)) c += 60.0f;
                for (int i = 0; i < w->nch; i++) { const Chr *h = &w->ch[i];   /* another heavy's lane there */
                    if (h == slam || !m_live_enemy(h, me) || !m_heavy(h) || state_is_hurt(h->state)) continue;
                    if (fabsf_(ex[k] - h->pos.x) < range * 6.5f && fabsf_(ez[k] - h->pos.z) < lane * 1.6f && (h->facing == 0 || (ex[k] - h->pos.x) * (float)h->facing > 0)) c += 40.0f; }
                c += 5.0f * (float)m_players_near(w, me, ex[k], ez[k], range * 1.0f, lane * 1.5f);
                if (c < best) { best = c; bi = k; }
            }
            b->mLeapX = ex[bi]; b->mLeapZ = ez[bi];
            if (b->mLeapUid != key) { b->mLeapUid = key;
                note(b, D_M_LEAP, "heavy uid %u up in its leap (y %.1f, move %d): out of its landing box (x +-%.1f, z +-%.1f around %.1f, %.1f) %s to (%.1f, %.1f)",
                     slam->uid, slam->pos.y, slam->moveId, RX, RZ, slam->pos.x, slam->pos.z, bi < 2 ? "on depth" : "sideways", b->mLeapX, b->mLeapZ); }
        }
        seq_clear(b);
        s_slamOn = 0; walk_to(b, me, b->mLeapX, b->mLeapZ, range * 0.1f, lane * 0.25f, o); s_slamOn = 1;
        if (!o->axis[AX_LX] && !o->axis[AX_LY]) o->axis[AX_LX] = (int16_t)((dx >= 0 ? 1 : -1) * b->xSign * STICK);
        set_mode(b, M_DEFEND, now, "leap landing");
        WHY("normal: heavy uid %u in its leap (y %.1f): out of the landing box to (%.1f, %.1f)", slam->uid, slam->pos.y, b->mLeapX, b->mLeapZ); return 1;
    }
    if (tgt == slam) {
        int f = sgn(slam->pos.x - me->pos.x);
        if (f && me->facing != f) o->axis[AX_LX] = (int16_t)(f * b->xSign * STICK / 3);
        WHY("normal: waiting for heavy uid %u to land (y %.1f), outside its landing box (dx %.1f dz %.1f)", slam->uid, slam->pos.y, dx, dz); return 1;
    }
    return 0;
}

/* forward special as a gap closer (Max's Thunder Tackle, Shiva's Senretsu Ken, Estel's Police Tackle onto a lone
   target) or a pull (Floyd's Magnetic Grab: the grab logic takes over), from the character table. Gates: the
   offensive-special life floor, 70% for a gap closer, fewer than 3 close, cooldown, not into a heavy's face, green
   health back (special_ok) and no dangerous heavy on screen (special_ok). */
static int mania_fwd(BotBrain *b, const MCtx *m, const World *w, const Chr *me, const CharProfile *pf, const Chr *tgt, int side, int facing, int shFront,
                     float adx, float adz, float lifePct, float banked, float camLo, float camHi, uint64_t now, PadOut *o) {
    float range = m->range, lane = m->lane;
    const MSpec *sp = m_spec(pf);
    if (!sp || !sp->fwd || m_alt(b, MALT_FWD)) return 0;
    if (shFront || !facing || tgt->robotPart || state_is_down(tgt->state) || tgt->state == CS_AIRHIT || tgt->state == CS_AIRLIGHTHIT || state_is_hurt(me->state)) return 0;
    if (tgt->pos.x <= camLo || tgt->pos.x >= camHi || adz > lane * 0.6f || m->hugger) return 0;
    if (adx < range * sp->fwdLo || adx > range * sp->fwdHi) return 0;
    uint64_t gap = b->aggression == 2 ? 3500 : b->aggression == 0 ? 7000 : 5000;
    if (b->mLastFwdGap && now - b->mLastFwdGap < gap) return 0;
    { float st; int wrong; if (mania_push_plan(b, m, w, me, tgt, camLo, camHi, &st, &wrong, now) && wrong) return 0; }
    if (m_heavy(tgt) && !state_is_hurt(tgt->state) && tgt->facing == -side) return 0;
    if (tgt->boss && tgt->aiAttacking) return 0;
    if (!_stricmp(sp->name, "Estel")) {                              /* Police Tackle grabs the first one: a lone target only */
        for (int i = 0; i < w->nch; i++) { const Chr *c = &w->ch[i];
            if (c == tgt || !m_live_enemy(c, me) || c->life <= 0) continue;
            if (fabsf_(c->pos.x - tgt->pos.x) < range * 2.0f && fabsf_(c->pos.z - tgt->pos.z) < lane * 3.0f) return 0; }
    }
    if (lifePct < (float)C.specialMinLifePct || lifePct < (sp->fwd == 1 ? 70.0f : 50.0f) || m->cL + m->cR >= 3) return 0;
    { int sv = s_mNoOffSpecial; s_mNoOffSpecial = 0; int ok = special_ok(b, 0, lifePct, now); s_mNoOffSpecial = sv; if (!ok) return 0; }
    GO(18, side, 1, { 90, F, 0, SB_Y }, { 550, 0, 0, 0 });
    used_special(b, lifePct, banked, now); b->mLastFwdGap = now; b->lastFwdSpecial = now; b->nextComboAt = 0;
    if (sp->fwd == 2) b->lastGrabTry = 0;
    note(b, D_M_FWDSP, "%s %s at uid %u (dx %.1f dz %.1f, life %.0f%%)", sp->name, sp->fwd == 2 ? "pull" : "gap closer", tgt->uid, side * adx, adz, lifePct);
    seq_run(b, now, o); set_mode(b, M_ATTACK, now, NULL); WHY("normal: forward special (%s) at uid %u", sp->fwd == 2 ? "pull" : "gap closer", tgt->uid); return 1;
}

/* 1 + 3 + 4: out of a heavy's danger lane, off a flying object's path / away from a hovering one, out of the slam
   landing for 0.8 s. v1.1 stage 4: one picker scores spots around us against every threat at once (v1.1-run3: the
   depth dodge from a pipe walked bots into a Koobo's swing lane, and depth was the pipes' own flight direction). */
static float m_spot_cost(const World *w, const Chr *me, const Chr *tgt, const Chr *land, float x, float z, float range, float lane, uint64_t now) {
    float c = fabsf_(x - me->pos.x) + 1.6f * fabsf_(z - me->pos.z);
    if (!m_zroom(z) || x < s_walkLo || x > s_walkHi) c += 1000.0f;
    if (m_heavy_lane(w, me, x, z, tgt, range, lane)) c += 100.0f;
    int od = m_obj_danger(w, me, x, z, range, lane, now, NULL); c += od == 2 ? 100.0f : od == 1 ? 12.0f : 0.0f;
    if (land) { float dx = x - land->pos.x; if (fabsf_(dx) < range * 2.4f && fabsf_(z - land->pos.z) < lane * 4.0f && (land->facing == 0 || dx * (float)land->facing > 0)) c += 60.0f; }
    if (s_slamOn) { if (s_slamEll ? fabsf_(x - s_slamX) < s_slamRX && fabsf_(z - s_slamZ) < s_slamRZ : (x - s_slamX) * (x - s_slamX) + (z - s_slamZ) * (z - s_slamZ) < s_slamR * s_slamR) c += 150.0f; }
    if (tz_at(x, z, 0.2f, now)) c += 80.0f;
    c += 3.0f * (float)m_players_near(w, me, x, z, range * 1.2f, lane * 1.5f);
    if (m_heavy_lane(w, me, (x + me->pos.x) * 0.5f, (z + me->pos.z) * 0.5f, tgt, range, lane)) c += 15.0f;   /* the way there */
    return c;
}
static int mania_guard(BotBrain *b, const MCtx *m, const World *w, const Chr *me, const Chr *tgt, uint64_t now, PadOut *o) {
    float range = m->range, lane = m->lane;
    if (state_is_hurt(me->state)) { b->mGuardUntil = 0; return 0; }
    const Chr *land = NULL;
    if (b->punishUid && now + 700 < b->punishUntil) {                    /* the first 0.8 s after a slam landed */
        for (int i = 0; i < w->nch; i++) { const Chr *c = &w->ch[i];
            if (c->uid != b->punishUid || !m_live_enemy(c, me) || !m_heavy(c) || state_is_hurt(c->state)) continue;
            float dx = me->pos.x - c->pos.x;
            if (fabsf_(dx) < range * 2.4f && fabsf_(me->pos.z - c->pos.z) < lane * 4.0f && (c->facing == 0 || dx * (float)c->facing > 0)) land = c;
        }
    }
    if (b->mGuardUntil) {
        int arrived = fabsf_(me->pos.z - b->mGuardZ) < lane * 0.4f && fabsf_(me->pos.x - b->mGuardX) < range * 0.15f;
        if (now >= b->mGuardUntil || arrived || m_spot_cost(w, me, tgt, land, b->mGuardX, b->mGuardZ, range, lane, now) >= 100.0f) { b->mGuardEndAt = now; b->mGuardUntil = 0; }
        else {
            walk_to(b, me, b->mGuardX, b->mGuardZ, range * 0.1f, lane * 0.3f, o);
            set_mode(b, M_DEFEND, now, "normal: danger lane");
            WHY("normal: %s uid %u: to (%.1f, %.1f)", b->mGuardKind == 1 ? "out of the lane of heavy" : b->mGuardKind == 2 ? "away from flying object" : "out of the landing of", b->mGuardUid, b->mGuardX, b->mGuardZ); return 1;
        }
    }
    const Chr *h = m_heavy_lane(w, me, me->pos.x, me->pos.z, tgt, range, lane);
    const Proj *pj = NULL; int od = m_obj_danger(w, me, me->pos.x, me->pos.z, range, lane, now, &pj);
    if (od < 2) pj = NULL;
    int landHere = land != NULL;
    if (!h && !pj && !landHere) {
        if (b->mGuardEndAt && b->mGuardKind == 2 && now - b->mGuardEndAt < 1500 && (od >= 1 || m_obj_danger(w, me, b->mGuardFX, b->mGuardFZ, range, lane, now, NULL) >= 2)) {   /* just off a flying object's path, still near its row: a moment before going back (m7.3-run1 18:41:49 stutter) */
            set_mode(b, M_DEFEND, now, "normal: danger lane");
            WHY("normal: away from flying object uid %u: letting it pass", b->mGuardUid); return 1;
        }
        if (b->mGuardEndAt && b->mGuardKind == 3 && now + 700 < b->punishUntil && b->punishUid == b->mGuardUid && now - b->mGuardEndAt < 800) {   /* the landing window: stay out until it ends */
            set_mode(b, M_DEFEND, now, "normal: danger lane");
            WHY("normal: out of the landing of uid %u: waiting it out", b->mGuardUid); return 1;
        }
        if (b->mGuardEndAt && now - b->mGuardEndAt < 500 && b->mGuardKind == 1) {   /* v1.1 stage 4: just out of a busy heavy's lane: let the swing finish before going back in (run3 17:14:08 stutter) */
            for (int i = 0; i < w->nch; i++) { const Chr *c = &w->ch[i];
                if (c->uid != b->mGuardUid || !m_live_enemy(c, me) || state_is_hurt(c->state) || !(c->aiAttacking || c->moveId >= 0)) continue;
                if (fabsf_(c->pos.x - me->pos.x) > range * 3.0f || fabsf_(c->pos.z - me->pos.z) > lane * 3.0f) continue;
                set_mode(b, M_DEFEND, now, "normal: danger lane");
                WHY("normal: out of the lane of heavy uid %u: waiting out its swing", c->uid); return 1;
            }
        }
        return 0;
    }
    /* candidate spots: x steps 0 / 0.8 / 1.6 / 2.6 ranges, depth steps 0 / 2 / 3.5 / 5 lanes, both ways */
    static const float kx[7] = { 0.0f, 0.8f, -0.8f, 1.6f, -1.6f, 2.6f, -2.6f }, kz[7] = { 0.0f, 2.0f, -2.0f, 3.5f, -3.5f, 5.0f, -5.0f };
    float bx = me->pos.x, bz = me->pos.z, best = 1e30f;
    int pref = h && b->mDiagUid == h->uid && b->mDiagSign ? b->mDiagSign : 0;
    for (int ix = 0; ix < 7; ix++) for (int iz = 0; iz < 7; iz++) {
        if (!ix && !iz) continue;
        float x = me->pos.x + kx[ix] * range, z = me->pos.z + kz[iz] * lane;
        float c = m_spot_cost(w, me, tgt, land, x, z, range, lane, now);
        if (pref && kz[iz] * (float)pref < 0) c += 0.5f;                  /* keep the side we picked for this heavy */
        if (c < best) { best = c; bx = x; bz = z; }
    }
    if (bx < s_walkLo) bx = s_walkLo; if (bx > s_walkHi) bx = s_walkHi;
    bz = m_zclamp(bz);
    if (h) { b->mDiagUid = h->uid; b->mDiagSign = bz >= h->pos.z ? 1 : -1; }
    int kind = h ? 1 : pj ? 2 : 3;
    uint32_t uid = h ? h->uid : pj ? pj->uid : land->uid;
    if (b->mGuardUid != uid || b->mGuardKind != kind || now - b->mGuardUntil > 3000) {
        if (h) note(b, D_M_HEAVYLANE, "uid %u (life %.0f, move %d%s%s) has us in its lane at dx %.1f: to (%.1f, %.1f), spot score %.1f", h->uid, h->life, h->moveId, h->aiAttacking ? ", attacking" : "", h->heldAddr ? ", weapon" : "", me->pos.x - h->pos.x, bx, bz, best);
        else if (pj) {
            int hov = pj->vel.x * pj->vel.x + pj->vel.z * pj->vel.z <= 0.03f * 0.03f;
            note(b, hov ? D_M_PIPE : D_M_OBJROW, "%s uid %u at (%.1f, %.1f, %.1f) vel (%.2f, %.2f)%s: to (%.1f, %.1f), spot score %.1f", pj->kind ? "thrown object" : "projectile", pj->uid, pj->pos.x, pj->pos.y, pj->pos.z, pj->vel.x, pj->vel.z,
                 hov ? (pj->pos.y < 0.8f ? " rising (about to hover and shoot)" : " hovering (about to shoot)") : " on a path through us", bx, bz, best);
        }
        else note(b, D_M_HEAVYLANE, "uid %u just landed its slam (we are in front at dx %.1f): to (%.1f, %.1f) for 0.8 s", land->uid, me->pos.x - land->pos.x, bx, bz);
    }
    if (b->mGuardUid != uid || b->mGuardKind != kind || !b->mGuardUntil) { b->mGuardFX = me->pos.x; b->mGuardFZ = me->pos.z; }   /* where the danger was */
    b->mGuardUid = uid; b->mGuardKind = kind; b->mGuardX = bx; b->mGuardZ = bz; b->mGuardUntil = now + (kind == 3 ? 800 : 900);
    seq_clear(b); walk_to(b, me, bx, bz, range * 0.1f, lane * 0.3f, o);
    set_mode(b, M_DEFEND, now, "normal: danger lane");
    WHY("normal: %s uid %u: to (%.1f, %.1f)", kind == 1 ? "out of the lane of heavy" : kind == 2 ? "away from flying object" : "out of the landing of", uid, bx, bz); return 1;
}

/* 2: approaching a heavy: from behind, around it off its row; the one it targets baits from off its row while somebody
   else is on it; bots that share it stand on slightly different depths */
static void mania_heavy_goal(BotBrain *b, const MCtx *m, const World *w, BotBrain *all, int nall, const Chr *me, const Chr *h, float *goalX, float *goalZ, uint64_t now) {
    float range = m->range, lane = m->lane;
    int hf = h->facing ? h->facing : (me->pos.x >= h->pos.x ? 1 : -1);
    int behind = (me->pos.x - h->pos.x) * (float)hf < 0;
    if (b->mDiagUid != h->uid || !b->mDiagSign) { b->mDiagUid = h->uid; b->mDiagSign = me->pos.z >= h->pos.z ? 1 : -1; }
    int sg; float oz = m_offrow_z(w, me, h->pos.z, lane * 3.0f, b->mDiagSign, h, range, lane, &sg); b->mDiagSign = sg;
    int rank = 0;
    for (int k = 0; k < nall; k++) if (&all[k] != b && all[k].identified && all[k].targetUid == h->uid && all[k].index < b->index) rank++;
    float rz = h->pos.z + (rank == 0 ? 0.0f : rank == 1 ? lane * 0.7f : -lane * 0.7f);
    if (!m_zroom(rz)) rz = h->pos.z;
    int hurt = state_is_hurt(h->state), idle = !h->aiAttacking && h->moveId < 0;
    if (h->aiTarget == me->uid && !hurt && m_players_near(w, me, h->pos.x, h->pos.z, range * 2.0f, lane * 3.0f) > 0) {
        *goalX = h->pos.x + hf * range * 3.0f; *goalZ = oz;
        if (*goalX < s_walkLo || *goalX > s_walkHi) *goalX = me->pos.x;
        if (b->mBaitUid != h->uid || now - b->mBaitNoteAt > 4000) { b->mBaitUid = h->uid; b->mBaitNoteAt = now;
            note(b, D_M_BAIT, "uid %u targets us and another player is on it: baiting from z %.1f", h->uid, oz); }
        return;
    }
    if (hurt) { *goalZ = rz; return; }                       /* reeling: punish it from where we are (default goal x) */
    float bx = h->pos.x - hf * range * 0.8f;
    if (bx < s_walkLo || bx > s_walkHi) {                    /* its back is against the edge: wait off its row for a window */
        if (idle) { *goalX = h->pos.x + hf * range * 0.7f; *goalZ = rz; }
        else { *goalX = h->pos.x + hf * range * 2.0f; *goalZ = oz; }
        return;
    }
    if (behind) { *goalX = bx; *goalZ = rz; return; }
    *goalX = bx; *goalZ = fabsf_(me->pos.x - bx) < range * 0.6f ? rz : oz;   /* in front: around it, off its row */
}

/* the Normal approach goal: the push plan (2), the heavy plan, the diagonal approach (6), unstacking (9) and never a
   goal inside a heavy's danger lane */
static void mania_goal(BotBrain *b, const MCtx *m, const World *w, BotBrain *all, int nall, const Chr *me, const Chr *tgt, float lifePct,
                       float camLo, float camHi, float *goalX, float *goalZ, uint64_t now) {
    float range = m->range, lane = m->lane;
    float dx = tgt->pos.x - me->pos.x, adx = fabsf_(dx); int side = sgn(dx) ? sgn(dx) : 1;
    if (m_heavy(tgt) && !state_is_down(tgt->state)) mania_heavy_goal(b, m, w, all, nall, me, tgt, goalX, goalZ, now);
    else {
        mania_approach_goal(b, m, w, me, tgt, camLo, camHi, goalX, goalZ, now);
        /* 6: it faces us and is after us (attacking, its AI target, or we are low): step off its row, in diagonally */
        int threat = tgt->facing == -side && !state_is_hurt(tgt->state) && !tgt->robotPart &&
                     (tgt->aiAttacking || moving_attack(tgt->state) || tgt->aiTarget == me->uid || lifePct < (float)C.retreatLifePct);
        int on = b->mDiagOn && b->mDiagUid == tgt->uid;
        if (threat && adx < range * 6.5f && (adx > range * 1.6f || (on && adx > range * 1.3f))) {
            if (b->mDiagUid != tgt->uid || !b->mDiagSign) { b->mDiagUid = tgt->uid; b->mDiagSign = me->pos.z >= tgt->pos.z ? 1 : -1; }
            int sg; float z = m_offrow_z(w, me, tgt->pos.z, lane * 2.2f, b->mDiagSign, tgt, range, lane, &sg); b->mDiagSign = sg;
            if (!on) note(b, D_M_DIAG, "uid %u faces us at dx %.1f%s: off its row to z %.1f, in diagonally", tgt->uid, dx, tgt->aiAttacking ? " (attacking)" : "", z);
            b->mDiagOn = 1; *goalZ = z;
        } else if (b->mDiagUid == tgt->uid) b->mDiagOn = 0;
    }
    /* 9: not on top of another player on the way in (the lower bot number / the human keeps the spot) */
    if (adx > range * 1.3f) {
        for (int i = 0; i < w->nch; i++) { const Chr *p = &w->ch[i];
            if (p->player < 0 || p->uid == me->uid || p->life <= 0 || state_is_down(p->state)) continue;
            int yields = 1;
            for (int k = 0; k < nall; k++) if (all[k].identified && all[k].uid == p->uid && all[k].index > b->index) yields = 0;
            if (!yields || fabsf_(p->pos.x - *goalX) >= range * 0.7f || fabsf_(p->pos.z - *goalZ) >= lane * 1.2f) continue;
            float z = m_zclamp(*goalZ + (p->pos.z > *goalZ ? -1.0f : 1.0f) * lane * 2.0f);
            if (fabsf_(z - p->pos.z) < lane * 1.2f) z = m_zclamp(*goalZ + (p->pos.z > *goalZ ? 1.0f : -1.0f) * lane * 2.0f);
            if (b->mUnstackNoteUid != p->uid) { b->mUnstackNoteUid = p->uid; note(b, D_M_UNSTACK, "uid %u stands on our way-in spot: to z %.1f", p->uid, z); }
            *goalZ = z; break;
        }
    }
    /* v1.1 stage 4: no goal inside the zone of a hovering object that is about to shoot (nearest spot outside it), and
       no step into such a zone or onto a flying object's path: around it on depth, else wait at its edge (the guard
       pushes out of it, so walking back in would be a back-and-forth) */
    {
        const Proj *pj = NULL;
        if (m_obj_danger(w, me, *goalX, *goalZ, range, lane, now, &pj) == 2 && pj && pj->vel.x * pj->vel.x + pj->vel.z * pj->vel.z <= 0.03f * 0.03f) {
            float cx[4] = { pj->pos.x + 3.3f, pj->pos.x - 3.3f, *goalX, *goalX }, cz[4] = { *goalZ, *goalZ, pj->pos.z + 1.7f, pj->pos.z - 1.7f };
            float bd = 1e30f; int bi = -1;
            for (int k = 0; k < 4; k++) {
                if (!m_zroom(cz[k]) || cx[k] < s_walkLo || cx[k] > s_walkHi || m_obj_danger(w, me, cx[k], cz[k], range, lane, now, NULL) == 2) continue;
                float d = fabsf_(cx[k] - me->pos.x) + 1.6f * fabsf_(cz[k] - me->pos.z);
                if (d < bd) { bd = d; bi = k; }
            }
            if (bi >= 0) { *goalX = cx[bi]; *goalZ = m_zclamp(cz[bi]); } else { *goalX = me->pos.x; *goalZ = me->pos.z; }
        }
        float ddx = *goalX - me->pos.x, ddz = *goalZ - me->pos.z, d = sqrtf(ddx * ddx + ddz * ddz);
        float st = range * 0.5f > 0.5f ? range * 0.5f : 0.5f; if (st > d) st = d;
        if (d > 0.05f && m_obj_danger(w, me, me->pos.x, me->pos.z, range, lane, now, NULL) < 2 &&
            m_obj_danger(w, me, me->pos.x + ddx / d * st, me->pos.z + ddz / d * st, range, lane, now, &pj) == 2 && pj) {
            int hov = pj->vel.x * pj->vel.x + pj->vel.z * pj->vel.z <= 0.03f * 0.03f, done = 0;
            if (hov) {
                float up = pj->pos.z + 1.7f, dn = pj->pos.z - 1.7f;
                float z = fabsf_(up - me->pos.z) <= fabsf_(dn - me->pos.z) ? up : dn;
                for (int k = 0; k < 2 && !done; k++, z = z == up ? dn : up) {
                    if (!m_zroom(z)) continue;
                    float s2 = fabsf_(z - me->pos.z) < st ? fabsf_(z - me->pos.z) : st;
                    if (m_obj_danger(w, me, me->pos.x, me->pos.z + (z > me->pos.z ? s2 : -s2), range, lane, now, NULL) < 2) { *goalX = me->pos.x; *goalZ = m_zclamp(z); done = 1; }
                }
            }
            if (!done) { *goalX = me->pos.x; *goalZ = me->pos.z; }   /* let it pass / wait at the edge */
        }
    }
    /* v1.1 stage 4: the next step would enter a heavy's danger lane we are not in (the guard would push us back out:
       a back-and-forth): around it on depth when that way is clear, else wait at its edge (3 s at most, then go) */
    {
        float ddx = *goalX - me->pos.x, ddz = *goalZ - me->pos.z, d = sqrtf(ddx * ddx + ddz * ddz);
        float st = range * 0.5f > 0.5f ? range * 0.5f : 0.5f; if (st > d) st = d;
        const Chr *h = NULL;
        if (d > 0.05f && !m_heavy_lane(w, me, me->pos.x, me->pos.z, tgt, range, lane)) {   /* the step, and its sideways part alone (depth drifts slowly) */
            h = m_heavy_lane(w, me, me->pos.x + ddx / d * st, me->pos.z + ddz / d * st, tgt, range, lane);
            if (!h) h = m_heavy_lane(w, me, me->pos.x + ddx / d * st, me->pos.z, tgt, range, lane);
        }
        if (h) {
            if (!b->mEdgeWaitLast || now - b->mEdgeWaitLast > 150) b->mEdgeWaitSince = now;
            b->mEdgeWaitLast = now;
            if (now - b->mEdgeWaitSince < 3000 || h == tgt) {   /* the target heavy itself: wait for its window */
                float z = m_offrow_z(w, me, h->pos.z, lane * 3.0f, me->pos.z >= h->pos.z ? 1 : -1, tgt, range, lane, NULL);
                float s2 = fabsf_(z - me->pos.z) < st ? fabsf_(z - me->pos.z) : st;
                if (fabsf_(z - me->pos.z) > lane * 0.5f && !m_heavy_lane(w, me, me->pos.x, me->pos.z + (z > me->pos.z ? s2 : -s2), tgt, range, lane)) { *goalX = me->pos.x; *goalZ = z; }
                else { *goalX = me->pos.x; *goalZ = me->pos.z; }
            }
        }
    }
    /* 1: never a goal in a heavy's danger lane (a target standing in front of a Koobo: wait off his row) */
    if (!m_heavy(tgt) && m_heavy_lane(w, me, *goalX, *goalZ, tgt, range, lane)) {
        const Chr *h = m_heavy_lane(w, me, *goalX, *goalZ, tgt, range, lane);
        *goalZ = m_offrow_z(w, me, h->pos.z, lane * 3.0f, me->pos.z >= h->pos.z ? 1 : -1, tgt, range, lane, NULL);
    }
}

/* 7 + 3: low life. Retreat while anything that can hit us is near (not 2.5 s and back in); behind P1 only when P1 is
   not in a heavy's lane, next to an enemy or in a slam landing; else to the far side of the screen, off the nearest
   enemy's row. 7 s at most, then 2.5 s of (careful: diagonal) fighting. */
static int mania_retreat(BotBrain *b, const MCtx *m, const World *w, const Chr *me, const Chr *p1, float lifePct, float camLo, float camHi, uint64_t now, PadOut *o) {
    float range = m->range, lane = m->lane;
    if (C.retreatLifePct <= 0 || lifePct >= (float)C.retreatLifePct) { b->mRetSince = 0; return 0; }
    const Chr *thr = NULL; float tb = 1e30f, sumX = 0; int n = 0;
    for (int i = 0; i < w->nch; i++) { const Chr *c = &w->ch[i];
        if (!m_live_enemy(c, me)) continue;
        if (w->haveCam && fabsf_(c->pos.x - w->cam.x) > m->screenW * 0.5f + range) continue;
        float adx = fabsf_(c->pos.x - me->pos.x), adz = fabsf_(c->pos.z - me->pos.z);
        float rx = b->mRetSince ? 6.0f : 3.5f, rz = b->mRetSince ? 7.0f : 5.0f;   /* hysteresis: once retreating, keep going until clearly clear */
        float zr = lane * rz > range * rx * 0.5f ? lane * rz : range * rx * 0.5f;
        if ((adx < range * rx && adz < zr) || (m_heavy(c) && adx < range * (b->mRetSince || (C.botSkill == 1 && b->mRetLastEnd && now - b->mRetLastEnd < 1500) ? 9.0f : 7.0f))) {
            sumX += c->pos.x; n++;
            if (!state_is_hurt(c->state) && adx + adz < tb) { tb = adx + adz; thr = c; }
        }
    }
    if (!thr) {
        if (b->mRetSince) b->mRetLastEnd = now;
        b->mRetSince = 0;
        if (b->mRetLastEnd && now - b->mRetLastEnd < 1500) {   /* v1.1 stage 4: just clear at low life: hold here a moment, not straight back in (run1 14:12:30 stutter) */
            set_mode(b, M_RETREAT, now, NULL);
            WHY("normal: low-life retreat: clear (life %.0f%%), holding", lifePct);
            return 1;
        }
        return 0;
    }
    if (now < b->mRetRestUntil) return 0;
    if (!b->mRetSince) { b->mRetSince = now; note(b, D_M_LOWLIFE, "life %.0f%% < %d%%, uid %u near (%d around): retreating until clear", lifePct, C.retreatLifePct, thr->uid, n); }
    if (now - b->mRetSince > 7000) { b->mRetSince = 0; b->mRetRestUntil = now + 2500; LOGD("%s: normal: retreated 7 s; fighting for 2.5 s", b->name); return 0; }
    float gx, gz; int useP1 = 0;
    if (p1 && p1->life > 0 && !state_is_hurt(p1->state) && !state_is_down(p1->state)) {
        int side = sgn(thr->pos.x - p1->pos.x); if (!side) side = 1;
        if (b->mRetSideK && now - b->mRetSideAt < 1500) side = b->mRetSideK; else { b->mRetSideK = side; b->mRetSideAt = now; }
        gx = p1->pos.x - side * range * 1.5f; gz = p1->pos.z;
        useP1 = gx >= camLo && gx <= camHi && !m_heavy_lane(w, me, p1->pos.x, p1->pos.z, NULL, range, lane) && !m_heavy_lane(w, me, gx, gz, NULL, range, lane);
        for (int i = 0; i < w->nch && useP1; i++) { const Chr *c = &w->ch[i];
            if (m_live_enemy(c, me) && !state_is_hurt(c->state) && fabsf_(c->pos.x - gx) < range * 1.2f && fabsf_(c->pos.z - gz) < lane * 2.0f) useP1 = 0; }
        if (useP1 && s_slamOn) { float ddx = gx - s_slamX, ddz = gz - s_slamZ; if (ddx * ddx + ddz * ddz < (s_slamR + 0.3f) * (s_slamR + 0.3f)) useP1 = 0; }
    }
    if (!useP1) {
        float cx = n ? sumX / (float)n : thr->pos.x;
        int away = me->pos.x >= cx ? 1 : -1;
        if (b->mRetSideK && now - b->mRetSideAt < 3000) away = b->mRetSideK; else { b->mRetSideK = away; b->mRetSideAt = now; }
        gx = away > 0 ? (camHi < 1e29f ? camHi - range * 0.3f : me->pos.x + range * 4.0f) : (camLo > -1e29f ? camLo + range * 0.3f : me->pos.x - range * 4.0f);
        gz = m_offrow_z(w, me, thr->pos.z, lane * 3.0f, me->pos.z >= thr->pos.z ? 1 : -1, NULL, range, lane, NULL);
        if (s_slamOn) { float ddx = gx - s_slamX, ddz = gz - s_slamZ; if (ddx * ddx + ddz * ddz < (s_slamR + 0.3f) * (s_slamR + 0.3f)) gx = s_slamX + (me->pos.x >= s_slamX ? 1.0f : -1.0f) * (s_slamR + 0.5f); }
    }
    {   /* the way there runs through a heavy's lane (v1.1-run2 14:37:43, P4 stutter): around it in depth first */
        float ddx = gx - me->pos.x, ddz = gz - me->pos.z, d = sqrtf(ddx * ddx + ddz * ddz);
        if (d > range * 0.3f) {
            float st = d < range * 1.0f ? d : range * 1.0f;
            const Chr *h = m_heavy_lane(w, me, me->pos.x + ddx / d * st, me->pos.z + ddz / d * st, NULL, range, lane);
            if (h) { gz = m_offrow_z(w, me, h->pos.z, lane * 3.0f, me->pos.z >= h->pos.z ? 1 : -1, NULL, range, lane, NULL);
                     if (fabsf_(me->pos.z - gz) > lane * 0.5f) gx = me->pos.x;
                     b->mRetDetZ = gz; b->mRetDetHold = gx == me->pos.x; b->mRetDetUntil = now + 600; }
            else if (now < b->mRetDetUntil) { gz = b->mRetDetZ; if (b->mRetDetHold && fabsf_(me->pos.z - gz) > lane * 0.5f) gx = me->pos.x; }   /* v1.1 stage 4: keep the detour a moment (run2 14:37:47 stutter) */
        }
    }
    set_mode(b, M_RETREAT, now, NULL);
    {   /* v1.1 stage 4: the next step lands in a hazard (leap box, flying object, heavy lane, floor trap) we are not in: hold here */
        float ddx = gx - me->pos.x, ddz = gz - me->pos.z, d = sqrtf(ddx * ddx + ddz * ddz);
        if (d > 0.05f) {
            float st = range * 0.5f > 0.5f ? range * 0.5f : 0.5f; if (st > d) st = d;
            float sx = me->pos.x + ddx / d * st, sz = me->pos.z + ddz / d * st;
            const Chr *tg = NULL; for (int i = 0; i < w->nch; i++) if (w->ch[i].uid == b->targetUid) { tg = &w->ch[i]; break; }   /* the same lane rules as the guard */
            float hNow = m_spot_cost(w, me, tg, NULL, me->pos.x, me->pos.z, range, lane, now);
            float hStep = m_spot_cost(w, me, tg, NULL, sx, sz, range, lane, now) - (fabsf_(sx - me->pos.x) + 1.6f * fabsf_(sz - me->pos.z));
            if (b->mLeapAt && now - b->mLeapAt < 600 && fabsf_(sx - b->mLeapBX) < 4.2f + 0.4f && fabsf_(sz - b->mLeapBZ) < 1.1f + 0.3f) hStep += 150.0f;   /* a leap landing box seen this moment */
            if (hStep >= 50.0f && hNow < 50.0f) {
                WHY("normal: low-life retreat (life %.0f%%, uid %u near): hazard on the way to (%.1f, %.1f), holding", lifePct, thr->uid, gx, gz);
                return 1;
            }
        }
    }
    walk_to(b, me, gx, gz, range * 0.3f, lane * 0.6f, o);
    if (fabsf_(thr->pos.x - me->pos.x) < range * 0.7f && fabsf_(thr->pos.z - me->pos.z) < lane && (now / 120) % 3 == 0)
        o->btn[prof_btn(b, 0, C.attackButton)] = 1;          /* cornered: swing back */
    WHY("normal: low-life retreat (life %.0f%%, uid %u near): to (%.1f, %.1f)%s", lifePct, thr->uid, gx, gz, useP1 ? " behind P1" : ", away from the enemies");
    return 1;
}
