/* v1.2: Survival perks and the portal (Normal skill only; included by sor4brain.c before fight()).
 * Observed (v1.2-perklog run, 2026-11-25): after a wave the perks are ActivableInstances (state 0 spawning, 1 active,
 * 6 taken, 5 the others after a pick, then gone); taken with B inside the perk's activationBox (local to its pos);
 * the revive perk is SurvivalBonus.resuscitatePlayers = 1 (bonusId 0) and its screen position changes between rounds;
 * one pick per round; ~1.5 s later the portal (isSurvivalPortal) appears; walking into its box ends the round.
 * Rules (the user, 2026-11-25):
 *  - every player down: a living bot takes the revive perk at once (else any perk), then walks into the portal;
 *  - otherwise the bots wait PerkWaitSeconds for a player to pick; if nobody does, ONE bot takes a perk it doesn't own
 *    yet (never the revive perk), the picker rotating by round (round % living bots, in P2/P3/P4 order);
 *  - never while a player stands on / is picking a perk; the bots never press B on a perk otherwise;
 *  - with a player alive the bots follow as usual and keep out of the portal (the player takes it). */
static struct { int key; uint64_t activeAt; int noted; } s_sv = { -1, 0, 0 };
static uint64_t s_svPickFrame[8];          /* this bot is taking a perk on purpose this frame (B allowed) */
void dbg_surv_reset(void) { s_sv.key = -1; s_sv.activeAt = 0; s_sv.noted = 0; memset(s_svPickFrame, 0, sizeof s_svPickFrame); }
static void surv_box(const Activable *a, float *x0, float *x1, float *z0, float *z1) {
    if (a->haveBox && a->boxX1 > a->boxX0 && a->boxZ1 >= a->boxZ0 && a->boxX1 - a->boxX0 < 6.0f && a->boxZ1 - a->boxZ0 < 4.0f) {
        *x0 = a->pos.x + a->boxX0; *x1 = a->pos.x + a->boxX1; *z0 = a->pos.z + a->boxZ0; *z1 = a->pos.z + a->boxZ1;
    } else { *x0 = a->pos.x - 1.0f; *x1 = a->pos.x + 1.0f; *z0 = a->pos.z - 0.4f; *z1 = a->pos.z + 0.4f; }
    if (*z1 - *z0 < 0.3f) { float c = 0.5f * (*z0 + *z1); *z0 = c - 0.15f; *z1 = c + 0.15f; }
}
static int surv_in(const Activable *a, float x, float z, float m) {
    float x0, x1, z0, z1; surv_box(a, &x0, &x1, &z0, &z1);
    return x > x0 - m && x < x1 + m && z > z0 - m && z < z1 + m;
}
static int surv_is_revive(const Activable *a) { return a->resuscitate == 1 || (a->resuscitate < 0 && a->bonusId == 0); }
static int surv_owns(const PlayerStateInfo *ps, int bonusId) {
    if (!ps) return 0;
    for (int i = 0; i < ps->nBonuses && i < 64; i++) if (ps->bonuses[i] == bonusId) return 1;
    return 0;
}
static const Chr *surv_bot_fighter(const BotBrain *ob, const World *w) {
    if (!ob->identified || ob->playerSlot < 0) return NULL;
    const Chr *c = slot_fighter(w, ob->playerSlot);
    if (!c || c->life <= 0) return NULL;          /* a dead player leaves the character list */
    return c;
}
/* B pressed by any other rule while standing on a live perk: dropped (Normal, Survival) */
static void surv_guard_b(const BotBrain *b, const World *w, const Chr *me, uint64_t now, PadOut *o) {
    if (C.botSkill != 1 || !w->survActive || !o->btn[BTN_B] || s_svPickFrame[b->index & 7] == now) return;
    for (int i = 0; i < w->nact; i++) { const Activable *a = &w->act[i];
        if (!a->isPortal && (a->state == 0 || a->state == 1) && surv_in(a, me->pos.x, me->pos.z, 0.3f)) { o->btn[BTN_B] = 0; return; } }
}
static int survival_perks(BotBrain *b, BotBrain *all, int nall, const World *w, const Chr *me, float range, uint64_t now, PadOut *o) {
    if (C.botSkill != 1 || !w->survActive || w->nact <= 0) return 0;
    /* players (not bots) and whether any is alive */
    const Chr *hum[8]; int nHum = 0, nAlive = 0;
    for (int s = 0; s < w->nslot; s++) {
        if (s == b->playerSlot || w->slot[s].ctrl < 0 || brain_ctrl_is_bot(all, nall, w->slot[s].ctrl)) continue;
        nHum++;
        const Chr *h = slot_fighter(w, s);
        int alive = h && h->life > 0;              /* a dead player leaves the character list (PlayerState life stays <= 0) */
        if (alive && nAlive < 8) hum[nAlive++] = h;
    }
    int allDown = nHum > 0 && nAlive == 0;
    const Activable *portal = NULL, *perk[W_MAXACT]; int np = 0, spawning = 0;
    for (int i = 0; i < w->nact; i++) { const Activable *a = &w->act[i];
        if (a->isPortal == 1) { if (a->state == 0 || a->state == 1) portal = a; continue; }
        if (a->isPortal != 0 || a->bonusId < 0) continue;
        if (a->state == 1) perk[np++] = a; else if (a->state == 0) spawning++;
    }
    int key = w->survLevelId * 1000 + w->survWaveId;
    if (key != s_sv.key) { s_sv.key = key; s_sv.activeAt = 0; s_sv.noted = 0; }
    /* the portal */
    if (portal && !np && !spawning) {
        float x0, x1, z0, z1; surv_box(portal, &x0, &x1, &z0, &z1);
        float cx = 0.5f * (x0 + x1), cz = 0.5f * (z0 + z1);
        if (allDown) {
            if (!(s_sv.noted & 4)) { s_sv.noted |= 4; note(b, D_M_PORTAL, "round %d: every player is down: into the portal uid %u at (%.1f, %.1f)", w->survLevelId + 1, portal->uid, cx, cz); }
            seq_clear(b); walk_to(b, me, cx, cz, 0.2f, 0.1f, o); set_mode(b, M_FOLLOW, now, "survival: portal");
            WHY("normal: survival: every player down: into the portal at (%.1f, %.1f)", cx, cz); return 1;
        }
        if (surv_in(portal, me->pos.x, me->pos.z, 0.35f)) {       /* keep out: the player takes the portal */
            float ref = nAlive ? hum[0]->pos.z : z1 + 1.0f;
            float tz = ref >= cz ? z1 + 0.8f : z0 - 0.8f;
            walk_to(b, me, me->pos.x, tz, range, 0.1f, o); set_mode(b, M_FOLLOW, now, "survival: out of the portal");
            WHY("normal: survival: keeping out of the portal (the player takes it)"); return 1;
        }
        return 0;
    }
    if (!np) return 0;
    if (!s_sv.activeAt) s_sv.activeAt = now;
    /* a player on (or picking) a perk: hands off */
    for (int i = 0; i < nAlive; i++) for (int k = 0; k < np; k++) if (surv_in(perk[k], hum[i]->pos.x, hum[i]->pos.z, 0.25f)) {
        { uint64_t w3 = (uint64_t)C.perkWaitSec * 1000; if (now + 3000 > w3 && s_sv.activeAt < now + 3000 - w3) s_sv.activeAt = now + 3000 - w3; }   /* a player choosing: 3 s more at least after they step off */
        if (!(s_sv.noted & 1)) { s_sv.noted |= 1; LOGI("%s: survival round %d: a player is on a perk: leaving the pick to them.", b->name, w->survLevelId + 1); }
        return 0;
    }
    uint64_t waitMs = (uint64_t)C.perkWaitSec * 1000;
    if (!allDown && now - s_sv.activeAt < waitMs) {
        if (!(s_sv.noted & 2)) { s_sv.noted |= 2; LOGI("%s: survival round %d: %d perk(s) up: waiting %d s for a player to pick.", b->name, w->survLevelId + 1, np, C.perkWaitSec); }
        return 0;
    }
    /* the picker: round % living bots, in P2/P3/P4 order */
    int live[8], nl = 0;
    for (int k = 0; k < nall && nl < 8; k++) if (surv_bot_fighter(&all[k], w)) live[nl++] = k;
    if (!nl) return 0;
    int lv = w->survLevelId < 0 ? 0 : w->survLevelId;
    int pk = live[lv % nl];
    if (&all[pk] != b) return 0;
    const PlayerStateInfo *ps = b->playerSlot >= 0 && b->playerSlot < w->nps ? &w->ps[b->playerSlot] : NULL;
    const Activable *pick = NULL; float best = 1e30f; int owned = 0;
    for (int pass = 0; pass < 3 && !pick; pass++) for (int k = 0; k < np; k++) { const Activable *a = perk[k];
        int rv = surv_is_revive(a);
        if (pass == 0 && !(allDown && rv)) continue;                       /* everybody down: the revive perk first */
        if (pass == 1 && (rv || surv_owns(ps, a->bonusId))) continue;      /* a perk we don't have yet */
        if (pass == 2 && rv && !allDown) continue;                         /* never the revive perk with a player up */
        float d = fabsf_(a->pos.x - me->pos.x) + 2.0f * fabsf_(a->pos.z - me->pos.z);
        if (d < best) { best = d; pick = a; owned = pass == 2 && surv_owns(ps, a->bonusId); }
    }
    if (!pick) return 0;
    if (!(s_sv.noted & 8)) { s_sv.noted |= 8;
        if (allDown) note(b, D_M_PERK, "round %d: every player is down: taking %s perk uid %u (bonus %d \"%s\"), then the portal", w->survLevelId + 1, surv_is_revive(pick) ? "the REVIVE" : "a", pick->uid, pick->bonusId, pick->title[0] ? pick->title : "?");
        else note(b, D_M_PERK, "round %d: no player picked in %d s: our turn (bot %d of %d): perk uid %u (bonus %d \"%s\"%s)", w->survLevelId + 1, C.perkWaitSec, lv % nl + 1, nl, pick->uid, pick->bonusId, pick->title[0] ? pick->title : "?", owned ? ", already owned: all are" : "");
    }
    float x0, x1, z0, z1; surv_box(pick, &x0, &x1, &z0, &z1);
    float cx = 0.5f * (x0 + x1), cz = 0.5f * (z0 + z1);
    seq_clear(b); set_mode(b, M_PICKUP, now, "survival: perk");
    s_svPickFrame[b->index & 7] = now;
    if (fabsf_(me->pos.x - cx) < (x1 - x0) * 0.35f && fabsf_(me->pos.z - cz) < (z1 - z0) * 0.35f && me->pos.y < 0.1f && !state_is_hurt(me->state)) {
        o->btn[BTN_B] = (uint8_t)(((now - s_sv.activeAt) / 100) % 3 == 0);     /* tap B: 100 ms on, 200 ms off */
        WHY("normal: survival: taking perk uid %u (bonus %d) with B", pick->uid, pick->bonusId); return 1;
    }
    walk_to(b, me, cx, cz, 0.15f, 0.08f, o);
    WHY("normal: survival: to perk uid %u (bonus %d) at (%.1f, %.1f)", pick->uid, pick->bonusId, cx, cz); return 1;
}
