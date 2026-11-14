/*
 * SoR4Bots state reader.
 *
 * Everything is looked up at runtime from the game's own NativeAOT reflection data (see sor4meta.c):
 * types by full name -> EEType (method table) addresses, instance field offsets by name, getter code by
 * method name. The offsets recovered offline for the March 2023 build are only used to cross-check and,
 * for that exact build, as a fallback (sor4_expected.h).
 *
 * Finding the live objects: the root is the [ThreadStatic] field game.state (a GameState). Its storage
 * is a per-thread statics object; the game's main thread reads it through a helper that sits in the
 * DRM-protected .text section, so we do not call or patch it. Instead:
 *   1. cheap: online.displayedState / confirmedState / controlState (regular GC statics, found through
 *      the FieldAccessMap cookie) if they hold a GameState;
 *   2. background search (one low-priority thread, only when needed): scan committed private memory
 *      for objects whose EEType is GameState, validate them, then find the object that points at one
 *      from offset +8 and is not a normal type: that is the thread-statics block of `game` (get_LevelState
 *      reads [statics+8]). We then read [holder+8] every frame, so the root follows level changes.
 * All memory reads go through ReadProcessMemory on our own process, so a stale or wrong pointer gives
 * a failed read, never a crash. Every object is checked against its expected EEType before use.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include "sor4state.h"
#include "sor4meta.h"
#include "sor4common.h"
#include "sor4_expected.h"

const char *const k_state_names[CS_COUNT] = { "AirHit", "AirLightHit", "AirMove", "Crouch", "CrouchGuard",
    "CrouchGuardHit", "CrouchHit", "CrouchMove", "Dash", "Disapear", "GroundHitState", "Jump", "OnGround",
    "StandDie", "StandGuard", "StandGuardHit", "StandHit", "StandMove", "StandWalk" };
const char *state_name(int cs) {
    if (cs >= 0 && cs < CS_COUNT) return k_state_names[cs];
    return cs == CS_NONE ? "-" : "other";
}
int state_is_hurt(int cs) {
    switch (cs) { case CS_AIRHIT: case CS_AIRLIGHTHIT: case CS_CROUCHHIT: case CS_STANDHIT: case CS_GROUNDHIT:
                  case CS_ONGROUND: case CS_STANDDIE: case CS_DISAPEAR: return 1; default: return 0; }
}
int state_is_down(int cs) { return cs == CS_GROUNDHIT || cs == CS_ONGROUND || cs == CS_STANDDIE || cs == CS_DISAPEAR; }

#define G_ "BeatThemAll.Gameplay."
static MetaImage M;
static int g_ok, g_dead;
static char g_fail[512];
static struct {
    int gs_levels, gs_players, gs_frames, gs_mode, gs_over;
    int ls_chars, ls_cams, ls_picks, ls_scene;
    int pob_uid, pob_team, pob_player, pob_life, pob_sprite, spr_pos, spr_flip;
    int ch_ctrl, ch_state, ch_stun, ch_inv, ch_move, ch_scripted, ch_maxlife;
    int cc_analog, cam_pos, cam_sizeY, pk_pos, pk_life, pk_team, pk_player;
    int sl_ctrl, sl_type, sl_status;
    int ps_playable, ps_continues, ps_life, ps_fury, ps_score, ps_ally;
    int chrRead, sprRead, pkRead, gsRead, lsRead;
    /* m3: menus and character select */
    int mgs_menu, mgs_players, cs_altConfirmed, ps_palette, sl_user, ud_entry, sm_list;
    int mc_chars, pc_name, pc_skins, pc_left, pc_right, pc_up, pc_down, pc_id, pc_adam, pc_roo;
    /* m4: fighting details */
    int ch_grabList, ch_held, ch_armor, ch_untarget, ch_noGrabbed, ch_noQR, ch_airRec, ch_airTime, ch_data;
    int cd_boss, sh_grabSrc, ai_attacking, ai_target, ctlRead;
    int pob_plane, ch_entered, ch_coll, ch_neverColl, cc_guard, ai_los, ai_root, cd_guardCrush, pd_score, do_entered;   /* m5 */
    int pk_data, pk_expl, pd_weapon, pd_life, pd_star, wd_onlyThrow, wd_explDur, wd_maxLife, wd_type, wd_name, mgs_ff, ps_wlife, do_blow, dod_blowTime, dod_dmg, dod_haz, dod_proj, dod_maxLife, dod_name, dad_rad;
    int ls_objs, ls_wall, gs_boss, ps_star, ps_banked, ps_starBtn, ps_starYB, ps_alt[5];
} O;
static const uint8_t *EE_PROJ, *EE_THROWN, *EE_DESTR, *EE_HAZ, *EE_DMGA, *EE_DOD, *EE_DAD;
/* m7.4: robot boss parts, robots, and who hit a fighter last */
static const uint8_t *EE_RPART, *EE_ROBOT, *EE_RDATA, *EE_STRING;
static int OFF_rp_partId = -1, OFF_rp_robotId = -1, OFF_pob_freeze = -1, OFF_ls_robots = -1, OFF_ch_hitSrc = -1;
static int OFF_rb_uid = -1, OFF_rb_active = -1, OFF_rb_broken = -1, OFF_rb_anim = -1, OFF_rb_drivers = -1, OFF_rb_parts = -1, OFF_rb_pos = -1, OFF_rb_data = -1, OFF_rb_first = -1;
static int OFF_rd_parts = -1, OFF_rd_heal = -1, OFF_rd_dmg = -1, OFF_rpd_life = -1;
static int OFF_ch_stamina = -1, OFF_ch_stBroken = -1, OFF_ch_guardHits = -1, OFF_ch_lastGuard = -1, OFF_cd_maxSt = -1, OFF_cd_moves = -1, OFF_cd_name = -1, OFF_mv_block = -1;   /* m6.7 */
static int OFF_cd_dname = -1, OFF_cd_maxLife = -1;   /* v1.1 stage 3 */
static int OFF_hz_anim = -1, OFF_hz_timer = -1, OFF_hz_cool = -1, OFF_hz_data = -1, OFF_hd_zone = -1, OFF_da_radius = -1;   /* m6.7 */
static const uint8_t *EE_MGS, *EE_MENU_CS, *EE_CSSCREEN, *EE_SM, *EE_MCD, *EE_PC, *EE_UD;
static int g_onGlobal = -1;                                    /* online.globalState offset in the online statics */
static const uint8_t *g_progCell, *g_mainCell; static int g_offMainMenu = -1, g_offPauseMenu = -1, g_offCfgRef = -1;
static const uint8_t *EE_GS, *EE_LS, *EE_CHR, *EE_CAM, *EE_PICK, *EE_PCTRL, *EE_ACTRL, *EE_SLOT, *EE_ST[CS_COUNT];
static const uint8_t *g_onlineCell; static int g_onOff[4] = { -1, -1, -1, -1 }, g_onSlots = -1;
static const char *const k_onNames[4] = { "displayedState", "confirmedState", "controlState", "previousControlState" };

/* ---------- safe memory access ---------- */
static int rd(uintptr_t a, void *o, size_t n) {
    if (a < 0x10000 || a > 0x7FFFFFFF0000ull) return 0;
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)a, o, n, &got) && got == n;
}
static uintptr_t rp(uintptr_t a) { uintptr_t v = 0; return rd(a, &v, 8) ? v : 0; }
static int32_t ri(uintptr_t a) { int32_t v = 0; rd(a, &v, 4); return v; }
static int obj_is(uintptr_t o, const uint8_t *ee) { return ee && o && !(o & 7) && rp(o) == (uintptr_t)ee; }
static int ee_in_image(uintptr_t e) { return e && meta_in_image(&M, (const void *)e); }
static float fx(int32_t v) { return (float)v / 65536.0f; }
static int32_t g32(const uint8_t *b, int off) { int32_t v; memcpy(&v, b + off, 4); return v; }
static uintptr_t g64(const uint8_t *b, int off) { uintptr_t v; memcpy(&v, b + off, 8); return v; }

/* ---------- resolution ---------- */
static int expected_field(const char *t, const char *f) {
    for (size_t i = 0; i < sizeof k_expected_fields / sizeof *k_expected_fields; i++)
        if (!strcmp(k_expected_fields[i].type, t) && !strcmp(k_expected_fields[i].field, f)) return k_expected_fields[i].offset;
    return -1;
}
static int knownBuild(void) { return M.timestamp == SOR4_EXPECTED_TIMESTAMP && M.size == SOR4_EXPECTED_SIZEOFIMAGE; }
static void add_fail(const char *what) {
    size_t n = strlen(g_fail);
    snprintf(g_fail + n, sizeof g_fail - n, "%s%s", n ? ", " : "", what);
}
static int fld(const char *type, const char *field, int essential) {
    int off = meta_field_offset(&M, meta_find_type(&M, type), field), exp = expected_field(type, field);
    if (C.diagnostics || (exp >= 0 && off != exp))
        LOGI("layout %s.%s = %d%s", type, field, off,
             exp < 0 ? "" : off == exp ? " (matches offline value)" : " (DIFFERENT from offline value)");
    if (off < 0 && exp >= 0 && knownBuild()) { LOGI("  using offline value %d for this known game build", exp); off = exp; }
    if (off < 0 && essential) { char b[128]; snprintf(b, sizeof b, "field %s.%s", type, field); add_fail(b); }
    return off;
}
static const uint8_t *typ(const char *type, int essential) {
    uint32_t td = meta_find_type(&M, type);
    const uint8_t *ee = meta_eetype(&M, td);
    unsigned exp = 0;
    for (size_t i = 0; i < sizeof k_expected_types / sizeof *k_expected_types; i++)
        if (!strcmp(k_expected_types[i].type, type)) exp = k_expected_types[i].rva;
    unsigned rva = ee ? (unsigned)(ee - M.base) : 0;
    if (C.diagnostics || (exp && rva != exp))
        LOGI("type %s: metadata 0x%x, EEType rva 0x%x%s", type, td, rva,
             !exp ? "" : rva == exp ? " (matches offline value)" : " (DIFFERENT from offline value)");
    if (!ee && essential) { char b[128]; snprintf(b, sizeof b, "type %s", type); add_fail(b); }
    return ee;
}
static void hexdump(const char *tag, const uint8_t *p, int n) {
    char line[3 * 64 + 1]; int k = 0;
    for (int i = 0; i < n && i < 64; i++) {
        uint8_t v; if (!rd((uintptr_t)(p + i), &v, 1)) break;
        k += snprintf(line + k, sizeof line - k, "%02x ", v);
    }
    line[k] = 0;
    LOGI("%s @rva 0x%x [%s]: %s", tag, meta_in_image(&M, p) ? (unsigned)(p - M.base) : 0, meta_section_of(&M, p), line);
}
static const uint8_t *method(const char *type, const char *name) {
    const uint8_t *ep = meta_method_entry(&M, meta_find_type(&M, type), name);
    if (C.diagnostics) { char t[160]; snprintf(t, sizeof t, "code %s::%s", type, name); if (ep) hexdump(t, ep, 32); else LOGI("%s: not found", t); }
    return ep;
}
/* tiny pattern decoders for the getters we rely on */
static int dec_life(const uint8_t *p) {            /* mov eax,[rcx+d8]; ret  |  mov eax,[rcx+d32]; ret */
    uint8_t b[8]; if (!p || !rd((uintptr_t)p, b, 8)) return -1;
    if (b[0] == 0x8B && b[1] == 0x41 && b[3] == 0xC3) return b[2];
    if (b[0] == 0x8B && b[1] == 0x81 && b[6] == 0xC3) return g32(b, 2);
    return -1;
}
static int dec_position(const uint8_t *p, int *spr, int *pos) {   /* mov rax,[rcx+s]; add rax,p */
    uint8_t b[16]; if (!p || !rd((uintptr_t)p, b, 16)) return 0;
    if (b[0] == 0x48 && b[1] == 0x8B && b[2] == 0x41 && b[4] == 0x48 && b[5] == 0x83 && b[6] == 0xC0) { *spr = b[3]; *pos = b[7]; return 1; }
    if (b[0] == 0x48 && b[1] == 0x8B && b[2] == 0x41 && b[4] == 0x48 && b[5] == 0x05) { *spr = b[3]; *pos = g32(b, 6); return 1; }
    return 0;
}
static int dec_flip(const uint8_t *p, int *spr, int *flip) {      /* mov rax,[rcx+s] ... cmp byte [rax+f],0 */
    uint8_t b[32]; if (!p || !rd((uintptr_t)p, b, 32)) return 0;
    int s = -1;
    for (int i = 0; i + 4 <= 32; i++) {
        if (s < 0 && b[i] == 0x48 && b[i+1] == 0x8B && b[i+2] == 0x41) { s = b[i+3]; i += 3; continue; }
        if (s >= 0 && b[i] == 0x80 && b[i+1] == 0x78 && b[i+3] == 0x00) { *spr = s; *flip = b[i+2]; return 1; }
    }
    return 0;
}
static uintptr_t cell_object(const uint8_t *cell) {   /* GC static cell: object or handle -> object */
    uintptr_t v = rp((uintptr_t)cell);
    if (!v || (v & 1)) return 0;
    if (ee_in_image(rp(v))) return v;
    uintptr_t o = rp(v);
    return (o && ee_in_image(rp(o))) ? o : 0;
}

static void tls_setup(const uint8_t *helper, const void *exeBase);
int state_init(const void *exeBase) {
    char err[200];
    g_fail[0] = 0;
    if (!meta_open(&M, exeBase, err, sizeof err)) { snprintf(g_fail, sizeof g_fail, "game metadata: %s", err); g_dead = 1; LOGE("State reader off: %s", g_fail); return 0; }
    LOGI("Game image: timestamp %u, size 0x%zx (%s build), ReadyToRun %d.%d, metadata %u bytes",
         M.timestamp, M.size, knownBuild() ? "known March 2023" : "UNKNOWN/updated", M.rtrMajor, M.rtrMinor, M.mdSize);
    EE_GS = typ(G_ "GameState", 1); EE_LS = typ(G_ "LevelState", 1); EE_CHR = typ(G_ "Character", 1);
    EE_CAM = typ(G_ "Camera", 0); EE_PICK = typ(G_ "PickupInstance", 0);
    EE_PCTRL = typ(G_ "PlayerCharacterController", 0); EE_ACTRL = typ(G_ "AiCharacterController", 0);
    EE_SLOT = typ("CommonLib.online+PlayerSlot", 0);
    for (int i = 0; i < CS_COUNT; i++) { char n[96]; snprintf(n, sizeof n, G_ "%s", k_state_names[i]); EE_ST[i] = typ(n, 0); }

    O.gs_levels = fld(G_ "GameState", "levelStateList", 1); O.gs_players = fld(G_ "GameState", "playerStateArray", 0);
    O.gs_frames = fld(G_ "GameState", "framesElapsed", 0); O.gs_mode = fld(G_ "GameState", "mode", 0);
    O.gs_over = fld(G_ "GameState", "isGameOver", 0);
    O.ls_chars = fld(G_ "LevelState", "characterList", 1); O.ls_cams = fld(G_ "LevelState", "cameraList", 0);
    O.ls_picks = fld(G_ "LevelState", "pickupList", 0); O.ls_scene = fld(G_ "LevelState", "sceneId", 0);
    O.pob_uid = fld(G_ "PhysicalObjectBase", "uid", 0); O.pob_team = fld(G_ "PhysicalObjectBase", "teamId", 1);
    O.pob_player = fld(G_ "PhysicalObjectBase", "playerId", 1);
    O.ch_ctrl = fld(G_ "Character", "controller", 0); O.ch_state = fld(G_ "Character", "state", 0);
    O.ch_stun = fld(G_ "Character", "hitStunTimer", 0); O.ch_inv = fld(G_ "Character", "invincibleTimer", 0);
    O.ch_move = fld(G_ "Character", "currentMoveId", 0); O.ch_scripted = fld(G_ "Character", "isScriptControlled", 0);
    O.ch_maxlife = fld(G_ "Character", "overrideMaxLife", 0);
    O.cc_analog = fld(G_ "CharacterController", "analogStickDirection", 0);
    O.cam_pos = fld(G_ "Camera", "position", 0); O.cam_sizeY = fld(G_ "Camera", "sizeY", 0);
    O.pk_pos = fld("BeatThemAll.AnimatedSprite", "position", 0); O.pk_life = fld(G_ "PickupInstance", "life", 0);
    O.pk_team = fld(G_ "PickupInstance", "teamId", 0); O.pk_player = fld(G_ "PickupInstance", "playerId", 0);
    O.sl_ctrl = fld("CommonLib.online+PlayerSlot", "_controllerId", 0); O.sl_type = fld("CommonLib.online+PlayerSlot", "_slotType", 0);
    O.sl_status = fld("CommonLib.online+PlayerSlot", "_gameplayStatus", 0);
    O.ps_playable = fld(G_ "PlayerState_s", "innerPlayableIndex", 0); O.ps_continues = fld(G_ "PlayerState_s", "continues", 0);
    O.ps_wlife = fld(G_ "PlayerState_s", "weaponInHandLife", 0);
    O.ps_life = fld(G_ "PlayerState_s", "life", 0); O.ps_fury = fld(G_ "PlayerState_s", "fury", 0);
    O.ps_score = fld(G_ "PlayerState_s", "score", 0); O.ps_ally = fld(G_ "PlayerState_s", "allyId", 0);

    /* m3: menus / character select (all optional: without them the bots fall back to the m2.4 rules) */
#define MG_ "BeatThemAll.MetaGame."
    EE_MGS = typ(MG_ "MetaGameGlobalState", 0); EE_MENU_CS = typ(MG_ "MetaGameGlobalState+Menu+CharacterSelection", 0);
    EE_CSSCREEN = typ(MG_ "CharacterSelectionScreen", 0); EE_SM = typ(MG_ "ScreenManager", 0);
    EE_MCD = typ(MG_ "MetaGameConfigData", 0); EE_PC = typ(MG_ "MetaGameConfigData+PlayableCharacter", 0);
    EE_UD = typ(MG_ "PlayerUserData", 0);
    O.mgs_ff = fld(MG_ "MetaGameGlobalState", "friendlyFireEnabled", 0);
    O.mgs_menu = fld(MG_ "MetaGameGlobalState", "currentMenu", 0); O.mgs_players = fld(MG_ "MetaGameGlobalState", "playerArray", 0);
    O.cs_altConfirmed = fld(MG_ "MetaGameGlobalState+Menu+CharacterSelection", "alternateMoveConfirmed", 0);
    O.ps_palette = fld(G_ "PlayerState_s", "playablePalette", 0);
    O.sl_user = fld("CommonLib.online+PlayerSlot", "_userData", 0); O.ud_entry = fld(MG_ "PlayerUserData", "menuEntryId", 0);
    O.sm_list = fld(MG_ "ScreenManager", "screenList", 0);
    O.mc_chars = fld(MG_ "MetaGameConfigData", "playableCharacterArray", 0);
    /* m4: fighting details (all optional; a missing one only switches the matching behavior off) */
    O.ch_grabList = fld(G_ "Character", "grabbedCharacterUidList", 0); O.ch_held = fld(G_ "Character", "heldPickup", 0);
    O.ch_armor = fld(G_ "Character", "superArmorTimer", 0); O.ch_untarget = fld(G_ "Character", "untargettableTimer", 0);
    O.ch_noGrabbed = fld(G_ "Character", "canNotBeGrabbedTimer", 0); O.ch_noQR = fld(G_ "Character", "quickRecoverDeactivated", 0);
    O.ch_airRec = fld(G_ "Character", "AirRecoverTimer", 0); O.ch_airTime = fld(G_ "Character", "TimeHitInAirSinceRelaunch", 0);
    O.ch_data = fld(G_ "Character", "dataRef", 0); O.cd_boss = fld("BeatThemAll.CharacterData", "isBoss", 0);
    O.sh_grabSrc = fld(G_ "StandHit", "grabSourceUid", 0);
    O.ai_attacking = fld(G_ "AiCharacterController", "isAttacking", 0); O.ai_target = fld(G_ "AiCharacterController", "targetCharacterRef", 0);
    O.pk_data = fld(G_ "PickupInstance", "pickupDataRef", 0); O.pk_expl = fld(G_ "PickupInstance", "explosionTimer", 0);
    O.pd_weapon = fld(G_ "PickupData", "weapon", 0); O.pd_life = fld(G_ "PickupData", "lifeGain", 0); O.pd_star = fld(G_ "PickupData", "starGain", 0);
    O.wd_onlyThrow = fld(G_ "WeaponData", "onlyThrow", 0); O.wd_explDur = fld(G_ "WeaponData", "explosionTimerDuration", 0);
    O.wd_maxLife = fld(G_ "WeaponData", "maxLife", 0); O.wd_type = fld(G_ "WeaponData", "type", 0); O.wd_name = fld(G_ "WeaponData", "displayName", 0);
    O.ls_objs = fld(G_ "LevelState", "physicalObjectList", 0); O.ls_wall = fld(G_ "LevelState", "leftWallX", 0);
    O.gs_boss = fld(G_ "GameState", "bossMode", 0);
    O.ps_star = fld(G_ "PlayerState_s", "starCount", 0); O.ps_banked = fld(G_ "PlayerState_s", "bankedLife", 0);
    {   /* v1.1 stage 4: the alternative special moves picked at character select (bool bytes) */
        static const char *const an[5] = { "alternativeBlitz", "alternativeNeutralSpecial", "alternativeForwardSpecial", "alternativeAirSpecial", "alternativeStarMove" };
        for (int i = 0; i < 5; i++) O.ps_alt[i] = fld(G_ "PlayerState_s", an[i], 0);
    }
    O.ps_starBtn = fld(G_ "PlayerState_s", "ctrlOptionStarMoveButtonActive", 0); O.ps_starYB = fld(G_ "PlayerState_s", "ctrlOptionStarMoveWithPickupSpecial", 0);
    EE_PROJ = typ(G_ "ProjectileInstance", 0); EE_THROWN = typ(G_ "ThrownPickup", 0);
    /* m5 */
    O.pob_plane = fld(G_ "PhysicalObjectBase", "CurrentPlane", 0); O.ch_entered = fld(G_ "Character", "hasEnteredTheScreenOnce", 0);
    O.ch_coll = fld(G_ "Character", "Collidable", 0); O.ch_neverColl = fld(G_ "Character", "NeverCollidable", 0);
    O.cc_guard = fld(G_ "CharacterController", "Guard", 0); O.ai_los = fld(G_ "AiCharacterController", "lineOfSightToTarget", 0);
    O.ai_root = fld(G_ "AiCharacterController", "currentBtRoot", 0); O.cd_guardCrush = fld("BeatThemAll.CharacterData", "guardCrushDuration", 0);
    O.pd_score = fld(G_ "PickupData", "scoreGain", 0); O.do_entered = fld(G_ "DestroyableObjectInstance", "hasEnteredTheScreenOnce", 0);
    EE_DESTR = typ(G_ "DestroyableObjectInstance", 0);
    /* m7.2: explosive barrels */
    EE_DOD = typ(G_ "DestroyableObjectData", 0); EE_DAD = typ(G_ "DamageAreaData", 0);
    O.do_blow = fld(G_ "DestroyableObjectInstance", "willBlowTimer", 0);
    O.dod_blowTime = fld(G_ "DestroyableObjectData", "willBlowStateTime", 0); O.dod_dmg = fld(G_ "DestroyableObjectData", "damageAreaToSpawnWhenDestroyed", 0);
    O.dod_haz = fld(G_ "DestroyableObjectData", "hazardToTriggerWhenDestroyed", 0); O.dod_proj = fld(G_ "DestroyableObjectData", "projectileToSpawnWhenDestroyed", 0);
    O.dod_maxLife = fld(G_ "DestroyableObjectData", "maxLife", 0); O.dod_name = fld(G_ "DestroyableObjectData", "displayName", 0);
    O.dad_rad = fld(G_ "DamageAreaData", "ellipseRadiusMax", 0);
    /* m6.7: floor traps (optional: nothing fails without them) */
    OFF_ch_stamina = fld(G_ "Character", "stamina", 0); OFF_ch_stBroken = fld(G_ "Character", "staminaBrokenTimer", 0);
    OFF_ch_guardHits = fld(G_ "Character", "TakenHitInGuardCounter", 0); OFF_ch_lastGuard = fld(G_ "Character", "LastHitReceivedWasInGuard", 0);
    OFF_cd_maxSt = fld("BeatThemAll.CharacterData", "maxStamina", 0); OFF_cd_moves = fld("BeatThemAll.CharacterData", "moveArray", 0);
    OFF_cd_name = fld("BeatThemAll.CharacterData", "_name", 0); OFF_mv_block = fld("BeatThemAll.CharacterData+Move", "superArmorIsBlock", 0);
    OFF_cd_dname = fld("BeatThemAll.CharacterData", "displayName", 0); OFF_cd_maxLife = fld("BeatThemAll.CharacterData", "maxLife", 0);   /* v1.1 stage 3 */
    EE_HAZ = typ(G_ "HazardInstance", 0); EE_DMGA = typ(G_ "DamageAreaInstance", 0);
    OFF_hz_anim = fld(G_ "HazardInstance", "curAnimId", 0); OFF_hz_timer = fld(G_ "HazardInstance", "timer", 0);
    OFF_hz_cool = fld(G_ "HazardInstance", "cooldownTimer", 0); OFF_hz_data = fld(G_ "HazardInstance", "dataRef", 0);
    OFF_hd_zone = fld(G_ "HazardData", "zone", 0); OFF_da_radius = fld(G_ "DamageAreaInstance", "radius", 0);
    /* m7.4 (optional) */
    EE_RPART = typ(G_ "RobotPartInstance", 0); EE_ROBOT = typ(G_ "RobotInstance", 0); EE_RDATA = typ(G_ "RobotData", 0); EE_STRING = typ("System.String", 0);
    OFF_rp_partId = fld(G_ "RobotPartInstance", "partId", 0); OFF_rp_robotId = fld(G_ "RobotPartInstance", "robotId", 0);
    OFF_pob_freeze = fld(G_ "PhysicalObjectBase", "hitFreezeTimer", 0); OFF_ls_robots = fld(G_ "LevelState", "robotList", 0);
    OFF_ch_hitSrc = fld(G_ "Character", "lastHitReceivedSourceUid", 0);
    OFF_rb_uid = fld(G_ "RobotInstance", "uid", 0); OFF_rb_active = fld(G_ "RobotInstance", "isActive", 0); OFF_rb_broken = fld(G_ "RobotInstance", "isBroken", 0);
    OFF_rb_anim = fld(G_ "RobotInstance", "animId", 0); OFF_rb_drivers = fld(G_ "RobotInstance", "driverUids", 0); OFF_rb_parts = fld(G_ "RobotInstance", "partUidArray", 0);
    OFF_rb_pos = fld(G_ "RobotInstance", "position", 0); OFF_rb_data = fld(G_ "RobotInstance", "dataRef", 0); OFF_rb_first = fld(G_ "RobotInstance", "isFirstDriver", 0);
    OFF_rd_parts = fld(G_ "RobotData", "partArray", 0); OFF_rd_heal = fld(G_ "RobotData", "healLife", 0); OFF_rd_dmg = fld(G_ "RobotData", "hitDamageFactor", 0);
    OFF_rpd_life = fld(G_ "RobotData+RobotPartData", "life", 0);
#define PC_ MG_ "MetaGameConfigData+PlayableCharacter"
    O.pc_name = fld(PC_, "name", 0); O.pc_skins = fld(PC_, "alternateSkinArray", 0);
    O.pc_left = fld(PC_, "menuLeftId", 0); O.pc_right = fld(PC_, "menuRightId", 0); O.pc_up = fld(PC_, "menuUpId", 0);
    O.pc_down = fld(PC_, "menuDownId", 0); O.pc_id = fld(PC_, "menuId", 0); O.pc_adam = fld(PC_, "isSor4Adam", 0); O.pc_roo = fld(PC_, "isSor3Roo", 0);
    {
        const uint8_t *ck; int off, st;
        uint32_t tp = meta_find_type(&M, MG_ "program"), tm = meta_find_type(&M, "BeatThemAll.main");
        if (meta_static_field(&M, tp, "mainMenu", &ck, &off, &st) && st == 2) { g_progCell = ck; g_offMainMenu = off; }
        if (meta_static_field(&M, tp, "pauseMenu", &ck, &off, &st) && st == 2) { g_progCell = ck; g_offPauseMenu = off; }
        if (meta_static_field(&M, tm, "metaGameConfigRef", &ck, &off, &st) && st == 2) { g_mainCell = ck; g_offCfgRef = off; }
        LOGI("menu statics: program cell rva 0x%x (mainMenu +%d, pauseMenu +%d), main cell rva 0x%x (metaGameConfigRef +%d)",
             g_progCell ? (unsigned)(g_progCell - M.base) : 0, g_offMainMenu, g_offPauseMenu, g_mainCell ? (unsigned)(g_mainCell - M.base) : 0, g_offCfgRef);
    }
    /* fields that are not in the reflection tables come from their getters' machine code */
    const uint8_t *gLife = method(G_ "PhysicalObjectBase", "get_Life");
    const uint8_t *gPos = method(G_ "PhysicalObjectBase", "get_Position");
    const uint8_t *gDir = method(G_ "PhysicalObjectBase", "get_Direction");
    method(G_ "Character", "get_IsPlayer"); method(G_ "Character", "get_IsEnemy");
    O.pob_life = dec_life(gLife);
    int s1 = -1, p1 = -1, s2 = -1, f2 = -1;
    if (!dec_position(gPos, &s1, &p1)) s1 = p1 = -1;
    if (!dec_flip(gDir, &s2, &f2)) s2 = f2 = -1;
    LOGI("getters: life +0x%x, sprite +0x%x, sprite.position +0x%x, sprite.flipX +0x%x (sprite seen twice: %s)",
         O.pob_life, s1, p1, f2, s1 == s2 ? "agree" : "DISAGREE");
    if (O.pob_life != EXP_OFF_LIFE || s1 != EXP_OFF_SPRITE || p1 != EXP_OFF_SPRITE_POS || f2 != EXP_OFF_SPRITE_FLIPX) {
        LOGI("getter offsets differ from the offline values (0x%x/0x%x/0x%x/0x%x)%s", EXP_OFF_LIFE, EXP_OFF_SPRITE,
             EXP_OFF_SPRITE_POS, EXP_OFF_SPRITE_FLIPX, knownBuild() ? "; known build, using offline values where decoding failed" : "");
        if (knownBuild()) {
            if (O.pob_life < 0) O.pob_life = EXP_OFF_LIFE;
            if (s1 < 0) { s1 = EXP_OFF_SPRITE; p1 = EXP_OFF_SPRITE_POS; }
            if (f2 < 0) f2 = EXP_OFF_SPRITE_FLIPX;
        }
    }
    O.pob_sprite = s1; O.spr_pos = p1; O.spr_flip = f2;
    if (O.pob_life < 0) add_fail("life getter");
    if (O.pob_sprite < 0 || O.spr_pos < 0) add_fail("position getter");

    /* root: game.state is [ThreadStatic]; log the helper the game uses so a future build can be checked */
    const uint8_t *gls = method(G_ "game", "get_LevelState");
    if (gls) {
        uint8_t b[9];
        if (rd((uintptr_t)gls, b, 9) && b[0] == 0x48 && b[1] == 0x83 && b[2] == 0xEC && b[4] == 0xE8) {
            const uint8_t *h = gls + 9 + g32(b, 5);
            hexdump("game statics helper", h, 64);
            tls_setup(h, exeBase);
        } else LOGI("get_LevelState has an unexpected shape (game update?)");
    }
    /* online statics (regular GC statics) */
    {
        uint32_t td = meta_find_type(&M, "CommonLib.online");
        const uint8_t *ck; int off, st;
        for (int i = 0; i < 4; i++)
            if (meta_static_field(&M, td, k_onNames[i], &ck, &off, &st) && st == 2) { g_onlineCell = ck; g_onOff[i] = off; }
        if (meta_static_field(&M, td, "publicPlayerSlotArray", &ck, &off, &st) && st == 2) { g_onlineCell = ck; g_onSlots = off; }
        if (meta_static_field(&M, td, "globalState", &ck, &off, &st) && st == 2) { g_onlineCell = ck; g_onGlobal = off; }
        LOGI("online.globalState +%d (menus and character select)", g_onGlobal);
        LOGI("online statics cell rva 0x%x: displayedState +%d confirmedState +%d controlState +%d publicPlayerSlotArray +%d",
             g_onlineCell ? (unsigned)(g_onlineCell - M.base) : 0, g_onOff[0], g_onOff[1], g_onOff[2], g_onSlots);
    }
    O.chrRead = 8; int m[] = { O.pob_uid, O.pob_team, O.pob_player, O.pob_life, O.pob_sprite, O.ch_ctrl, O.ch_state,
                               O.ch_stun, O.ch_inv, O.ch_move, O.ch_scripted, O.ch_maxlife,
                               O.ch_grabList, O.ch_held, O.ch_armor, O.ch_untarget, O.ch_noGrabbed, O.ch_noQR, O.ch_airRec, O.ch_airTime,
                               O.ch_data >= 0 ? O.ch_data + 8 : -1, O.pob_plane, O.ch_entered, O.ch_coll, O.ch_neverColl,
                               OFF_ch_stamina, OFF_ch_stBroken, OFF_ch_guardHits, OFF_ch_lastGuard };
    for (size_t i = 0; i < sizeof m / sizeof *m; i++) if (m[i] + 8 > O.chrRead) O.chrRead = m[i] + 8;
    O.sprRead = (O.spr_pos > O.spr_flip ? O.spr_pos : O.spr_flip) + 16;
    O.pkRead = 8; int pm[] = { O.pk_pos + 12, O.pk_life + 4, O.pk_team + 4, O.pk_player + 4, O.pk_data >= 0 ? O.pk_data + 16 : 0, O.pk_expl + 4 };
    for (int i = 0; i < 6; i++) if (pm[i] > O.pkRead) O.pkRead = pm[i];
    O.gsRead = 8; int gm[] = { O.gs_levels + 8, O.gs_players + 8, O.gs_frames + 4, O.gs_mode + 4, O.gs_over + 1, O.gs_boss + 1 };
    for (int i = 0; i < 6; i++) if (gm[i] > O.gsRead) O.gsRead = gm[i];
    O.lsRead = 8; int lm[] = { O.ls_chars + 8, O.ls_cams + 8, O.ls_picks + 8, O.ls_scene + 4, O.ls_objs + 8, O.ls_wall + 4 };
    for (int i = 0; i < 6; i++) if (lm[i] > O.lsRead) O.lsRead = lm[i];
    O.ctlRead = 8; int cm[] = { O.cc_analog + 8, O.ai_attacking + 1, O.ai_target + 4, O.cc_guard + 1, O.ai_los + 1, O.ai_root + 4 };
    for (int i = 0; i < 6; i++) if (cm[i] > O.ctlRead) O.ctlRead = cm[i];
    if (O.ctlRead > 512) O.ctlRead = 512;
    if (O.chrRead > 1024 || O.sprRead > 512 || O.pkRead > 512 || O.gsRead > 512 || O.lsRead > 512) add_fail("implausible offsets");

    if (g_fail[0]) { g_dead = 1; LOGE("State reader off, missing: %s. Bots use the simple test pattern instead.", g_fail); return 0; }
    g_ok = 1;
    LOGI("State reader ready (all layouts found from the game's own metadata).");
    return 1;
}
int state_available(void) { return g_ok && !g_dead; }
const char *state_failure(void) { return g_fail[0] ? g_fail : g_ok ? "" : "not initialised"; }

/* ---------- background search ---------- */
#define MAXC 64
static CRITICAL_SECTION g_cs; static int g_csInit;
typedef struct { volatile LONG stop; HANDLE evt; } ScanCtx;       /* per thread, never freed (thread may be abandoned) */
static HANDLE g_thread; static ScanCtx *g_ctx; static volatile LONG g_wanted, g_busy;
static uintptr_t g_resGS[MAXC]; static int g_nResGS;
static uintptr_t g_resHolder[MAXC]; static int g_nResHolder; static volatile LONG g_resReady;
static ULONGLONG g_lastScanReq; static volatile LONG g_stopping;
static uintptr_t g_holder, g_holderEE, g_direct; static int g_holderEmpty;
/* thread stacks: never search them (a stack slot that happens to hold a GameState pointer looked like
   a "holder" in the m2 log and was lost again within seconds) */
#define MAXSTK 256
static uintptr_t g_stk[MAXSTK][2]; static int g_nstk;
static int in_stack(uintptr_t a, uintptr_t e) {
    for (int i = 0; i < g_nstk; i++) if (a < g_stk[i][1] && e > g_stk[i][0]) return 1;
    return 0;
}
static int g_scans;

static int gs_plausible(uintptr_t gs) {
    uintptr_t l = rp(gs + O.gs_levels);
    if (!l) return 1;                                  /* allowed: no level yet */
    return ee_in_image(rp(l)) && ri(l + 16) >= 0 && ri(l + 16) < 64;
}
typedef int (*qword_cb)(uintptr_t addr, uintptr_t val, void *ctx);
/* Read-only sweep of plain private read/write memory. Skips guard, no-cache and write-combined pages
 * (GPU-mapped memory: slow to read, and touching it can stall the driver), image/mapped files, and
 * our own buffer. Sleeps 1 ms every 4 MB so it never competes with the game for long. */
#include <stdlib.h>
static void scan_memory(ScanCtx *sc, const uintptr_t *want, int nwant, qword_cb cb, void *ctx, uint8_t *buf, size_t bufSize,
                        size_t *scanned, ULONGLONG deadline) {
    uintptr_t lo = ~(uintptr_t)0, hi = 0;
    for (int k = 0; k < nwant; k++) { if (want[k] < lo) lo = want[k]; if (want[k] > hi) hi = want[k]; }
    int sorted = 1; for (int k = 1; k < nwant; k++) if (want[k] < want[k - 1]) sorted = 0;
    MEMORY_BASIC_INFORMATION mbi; uintptr_t p = 0x10000; unsigned chunks = 0;
    while (!sc->stop && p < 0x7FFFFFFF0000ull && VirtualQuery((LPCVOID)p, &mbi, sizeof mbi)) {
        uintptr_t base = (uintptr_t)mbi.BaseAddress, end = base + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.Protect == PAGE_READWRITE &&
            !(base <= (uintptr_t)buf && (uintptr_t)buf < end) && !in_stack(base, end)) {
            for (uintptr_t a = base; a < end && !sc->stop; a += bufSize) {
                if ((++chunks & 15) == 0) { Sleep(1); if (GetTickCount64() > deadline) { LOGI("search pass hit its time limit; will continue later"); return; } }
                size_t n = end - a < bufSize ? end - a : bufSize; SIZE_T got = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)a, buf, n, &got)) continue;
                *scanned += got;
                const uintptr_t *q = (const uintptr_t *)buf;
                for (size_t i = 0; i < got / 8; i++) {
                    uintptr_t v = q[i];
                    if (v < lo || v > hi) continue;
                    if (sorted && nwant > 4) {                 /* many targets (diagnostic trace): binary search */
                        int L = 0, H = nwant - 1;
                        while (L <= H) { int Mid = (L + H) / 2; if (want[Mid] == v) { if (cb(a + i * 8, v, ctx)) return; break; } if (want[Mid] < v) L = Mid + 1; else H = Mid - 1; }
                        continue;
                    }
                    for (int k = 0; k < nwant; k++) if (v == want[k]) { if (cb(a + i * 8, v, ctx)) return; break; }
                }
            }
        }
        if (end <= p) break;
        p = end;
    }
}
typedef struct { uintptr_t *out; int n; } Hits;
/* search bookkeeping for the "no GameState anywhere" check (character select) */
static volatile ULONGLONG g_lastSearchDone; static volatile LONG g_lastSearchN = -1;
/* m2.4 diagnostic: who points at the heap "statics block" holder? (to learn why the thread-static
   chain is never found). Filled by the search thread once, logged by the main thread. */
#define TR 16
static struct { uintptr_t holder; int nA, nB, nC, nD; uintptr_t a[TR], arr[TR]; int idx[TR];
                uintptr_t b[TR], bv[TR], c[TR], cv[TR], d[TR], dv[TR]; } g_tr;
static volatile LONG g_trState;    /* 0 = not done, 1 = running, 2 = results ready, 3 = logged */
typedef struct { uintptr_t *addr, *val; int n, max; } Coll;
static int hit_coll(uintptr_t addr, uintptr_t val, void *c) {
    Coll *k = c;
    for (int i = 0; i < k->n; i++) if (k->addr[i] == addr) return 0;
    if (k->n < k->max) { k->addr[k->n] = addr; k->val[k->n] = val; k->n++; }
    return k->n >= k->max;
}
static int cmp_up(const void *a, const void *b) { uintptr_t x = *(const uintptr_t *)a, y = *(const uintptr_t *)b; return x < y ? -1 : x > y; }
static int hit_gs(uintptr_t addr, uintptr_t val, void *c) {
    Hits *h = c; (void)val;
    if (gs_plausible(addr) && h->n < MAXC) h->out[h->n++] = addr;
    return h->n >= MAXC;
}
static int hit_ref(uintptr_t addr, uintptr_t val, void *c) {
    Hits *h = c; (void)val;
    uintptr_t obj = addr - 8, ee = rp(obj);     /* referrer field at +8 */
    if (ee_in_image(ee) && h->n < MAXC) h->out[h->n++] = obj;
    return h->n >= MAXC;
}
/* reverse trace: holder <- (array element) <- handle/slot <- slot array <- ... (pointers to +0..+56 too) */
static void trace_holder(ScanCtx *sc, uintptr_t holder, uint8_t *buf, size_t bufSize, size_t *scanned) {
    memset(&g_tr, 0, sizeof g_tr); g_tr.holder = holder;
    ULONGLONG t0 = GetTickCount64();
    Coll A = { g_tr.a, g_tr.arr, 0, TR };
    scan_memory(sc, &holder, 1, hit_coll, &A, buf, bufSize, scanned, t0 + 30000); g_tr.nA = A.n;
    uintptr_t want[TR * 8]; int nw = 0;
    for (int i = 0; i < A.n; i++) {               /* is the reference an element of an array? */
        g_tr.arr[i] = 0; g_tr.idx[i] = -1;
        for (int k = 0; k < 256; k++) {
            uintptr_t a = g_tr.a[i] - 16 - 8 * (uintptr_t)k, ee = rp(a); int len = ri(a + 8);
            if (ee_in_image(ee) && len > k && len < 1000000) { g_tr.arr[i] = a; g_tr.idx[i] = k; break; }
        }
        uintptr_t t = g_tr.arr[i] ? g_tr.arr[i] : g_tr.a[i];
        int dup = 0; for (int j = 0; j < nw; j++) if (want[j] == t) dup = 1;
        if (!dup && nw < TR) want[nw++] = t;
    }
    if (!nw || sc->stop) return;
    qsort(want, nw, sizeof *want, cmp_up);
    Coll B = { g_tr.b, g_tr.bv, 0, TR };
    scan_memory(sc, want, nw, hit_coll, &B, buf, bufSize, scanned, GetTickCount64() + 30000); g_tr.nB = B.n;
    nw = 0;                                       /* level C: pointers to any of the B slots or up to 7 slots before them */
    for (int i = 0; i < B.n && nw < TR * 8; i++) for (int k = 0; k < 8 && nw < TR * 8; k++) want[nw++] = g_tr.b[i] - 8 * (uintptr_t)k;
    if (!nw || sc->stop) return;
    qsort(want, nw, sizeof *want, cmp_up);
    Coll Cc = { g_tr.c, g_tr.cv, 0, TR };
    scan_memory(sc, want, nw, hit_coll, &Cc, buf, bufSize, scanned, GetTickCount64() + 30000); g_tr.nC = Cc.n;
    nw = 0;
    for (int i = 0; i < Cc.n && nw < TR * 8; i++) for (int k = 0; k < 8 && nw < TR * 8; k++) want[nw++] = g_tr.c[i] - 8 * (uintptr_t)k;
    if (!nw || sc->stop) return;
    qsort(want, nw, sizeof *want, cmp_up);
    Coll D = { g_tr.d, g_tr.dv, 0, TR };
    scan_memory(sc, want, nw, hit_coll, &D, buf, bufSize, scanned, GetTickCount64() + 30000); g_tr.nD = D.n;
}
static DWORD WINAPI scan_thread(LPVOID param) {
    ScanCtx *sc = param;
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);   /* low CPU, I/O and memory priority */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    size_t bufSize = 256 << 10;
    uint8_t *buf = VirtualAlloc(NULL, bufSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) return 0;
    while (!sc->stop) {
        WaitForSingleObject(sc->evt, INFINITE);
        if (sc->stop) break;
        InterlockedExchange(&g_busy, 1);
        ULONGLONG t0 = GetTickCount64(); size_t scanned = 0;
        uintptr_t gs[MAXC], hold[MAXC]; Hits h1 = { gs, 0 }, h2 = { hold, 0 };
        uintptr_t want = (uintptr_t)EE_GS;
        scan_memory(sc, &want, 1, hit_gs, &h1, buf, bufSize, &scanned, t0 + 20000);
        if (h1.n && !sc->stop) scan_memory(sc, gs, h1.n, hit_ref, &h2, buf, bufSize, &scanned, t0 + 40000);
        if (sc->stop) break;
        EnterCriticalSection(&g_cs);
        memcpy(g_resGS, gs, sizeof gs); g_nResGS = h1.n; memcpy(g_resHolder, hold, sizeof hold); g_nResHolder = h2.n;
        LeaveCriticalSection(&g_cs);
        LOGI("search #%d: %zu MB scanned in %llu ms, %d GameState object(s), %d object(s) pointing at one from +8",
             ++g_scans, scanned >> 20, GetTickCount64() - t0, h1.n, h2.n);
        InterlockedExchange(&g_lastSearchN, h1.n); g_lastSearchDone = GetTickCount64();
        InterlockedExchange(&g_resReady, 1);
        /* diagnostics, once per session: trace who points at the first statics-block-like referrer */
        if (C.diagnostics && g_trState == 0) {
            for (int i = 0; i < h2.n; i++) {
                uintptr_t ee = rp(hold[i]); const char *sec = meta_section_of(&M, (const void *)ee);
                if ((ee & 7) || !sec || !strcmp(sec, ".managed") || !strcmp(sec, ".text")) continue;
                InterlockedExchange(&g_trState, 1);
                trace_holder(sc, hold[i], buf, bufSize, &scanned);
                InterlockedExchange(&g_trState, sc->stop ? 0 : 2);
                break;
            }
        }
        InterlockedExchange(&g_busy, 0);
    }
    VirtualFree(buf, 0, MEM_RELEASE);
    InterlockedExchange(&g_busy, 0);
    return 0;
}
/* Ask the search thread to end. Waits at most waitMs (0 = don't wait); if it is still busy it is
 * abandoned: it only reads memory and exits by itself at its next check. */
static void stop_scanner(DWORD waitMs, const char *why) {
    if (!g_thread) return;
    InterlockedExchange(&g_ctx->stop, 1); SetEvent(g_ctx->evt);
    DWORD r = waitMs ? WaitForSingleObject(g_thread, waitMs) : WAIT_TIMEOUT;
    LOGI("search thread %s (%s)", r == WAIT_OBJECT_0 ? "stopped" : waitMs ? "still finishing a read; abandoned, it exits by itself" : "told to stop", why);
    CloseHandle(g_thread); g_thread = NULL; g_ctx = NULL;
}
static void enum_threads(ULONGLONG now);
static void request_scan(const char *why) {
    if (!g_ok || !g_wanted || g_busy || g_stopping) return;
    ULONGLONG now = GetTickCount64();
    if (g_lastScanReq && now - g_lastScanReq < (g_scans < 3 ? 4000u : g_scans < 20 ? 8000u : 30000u)) return;
    g_lastScanReq = now;
    if (!g_csInit) { InitializeCriticalSection(&g_cs); g_csInit = 1; }
    if (!g_thread) {
        ScanCtx *sc = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *sc);
        if (sc) sc->evt = CreateEventW(NULL, FALSE, FALSE, NULL);
        g_thread = sc && sc->evt ? CreateThread(NULL, 0, scan_thread, sc, 0, NULL) : NULL;
        if (!g_thread) { LOGE("could not start the search thread (%lu)", GetLastError()); g_dead = 1; add_fail("search thread"); return; }
        g_ctx = sc;
        LOGI("Started the background search for the game state (read-only, background priority).");
    }
    enum_threads(now);                     /* fresh thread-stack list for the scanner (it is idle now) */
    LOGD("search requested: %s", why);
    InterlockedExchange(&g_busy, 1);
    SetEvent(g_ctx->evt);
}
void state_set_wanted(int w) { InterlockedExchange(&g_wanted, w ? 1 : 0); }

/* v1.1 stage 4 (Normal only, v1.1-run3 Diva fight): a stale GameState copy (frames frozen at 6849) kept winning the
   "most frames" pick over the live one (576) after every rescan, so the bots flapped between them every ~6 s. Copies
   that stopped updating are remembered with their frozen frame count and skipped while it has not moved. */
static struct { uintptr_t a; int f; } g_staleGS[8]; static int g_nStaleGS;
static int stale_gs(uintptr_t a, int f) { for (int i = 0; i < g_nStaleGS; i++) if (g_staleGS[i].a == a && g_staleGS[i].f == f) return 1; return 0; }
static uintptr_t online_obj(void) { return g_onlineCell ? cell_object(g_onlineCell) : 0; }
static void adopt_results(void) {
    if (!g_resReady) return;
    InterlockedExchange(&g_resReady, 0);
    uintptr_t gs[MAXC], hold[MAXC]; int ng, nh;
    EnterCriticalSection(&g_cs);
    ng = g_nResGS; nh = g_nResHolder; memcpy(gs, g_resGS, sizeof gs); memcpy(hold, g_resHolder, sizeof hold);
    LeaveCriticalSection(&g_cs);
    uintptr_t on = online_obj();
    for (int i = 0; i < ng && (C.diagnostics || i < 4); i++) {
        uintptr_t l = rp(gs[i] + O.gs_levels);
        LOGI("  GameState candidate %p: frames %d, levels %d", (void *)gs[i], ri(gs[i] + O.gs_frames), l ? ri(l + 16) : -1);
    }
    uintptr_t best = 0;
    for (int i = 0; i < nh; i++) {
        uintptr_t o = hold[i], ee = rp(o);
        uint32_t td = meta_type_for_eetype(&M, (const void *)ee);
        char nm[160] = "(statics block)";
        if (td) meta_type_name(&M, td, nm, sizeof nm);
        int isOnline = o == on;
        uintptr_t second = rp(o + 16), sEE = second ? rp(second) : 0;
        const char *sec = meta_section_of(&M, (const void *)ee);
        int eeData = !(ee & 7) && sec && strcmp(sec, ".managed") && strcmp(sec, ".text");   /* a code address = stack slot */
        int plausible = !td && !isOnline && eeData && (!second || sEE == (uintptr_t)EE_CHR || ee_in_image(sEE));
        /* m2.4: the m2.3 log adopted holders whose [+8] was already empty when we looked (another
           thread's copy of game's thread statics, set only for a moment): the bot then saw "no level"
           for a minute while the stage ran. A holder must hold a GameState with a level right now. */
        uintptr_t now8 = rp(o + 8), nl = obj_is(now8, EE_GS) ? rp(now8 + O.gs_levels) : 0;
        int holdsNow = obj_is(now8, EE_GS) && nl && ri(nl + 16) > 0;
        if (C.diagnostics || i < 4)
            LOGI("  referrer %p ee rva 0x%x %s%s, [+16] -> %s, [+8] now %s%s", (void *)o, (unsigned)(ee - (uintptr_t)M.base), nm,
                 isOnline ? " (online statics)" : "", !second ? "null" : sEE == (uintptr_t)EE_CHR ? "Character" : "other",
                 !now8 ? "empty" : holdsNow ? "a GameState with a level" : obj_is(now8, EE_GS) ? "a GameState without a level" : "something else",
                 !plausible ? (!eeData ? "  (not an object: code address, ignored)" : "") :
                 holdsNow ? "  <- candidate for game's thread statics" : "  (statics-like, but not holding a level now: ignored)");
        if (plausible && holdsNow && !best) best = o;
    }
    if (best) {
        g_holder = best; g_holderEE = rp(best); g_holderEmpty = 0; g_direct = 0;
        LOGI("Found game.state holder at %p; reading [holder+8] every frame from now on.", (void *)best);
        if (g_trState != 1) stop_scanner(0, "root found; no more searching unless it is lost");
    } else if (ng) {
        int bi = -1, bf = -1;
        int skipped = 0;
        for (int i = 0; i < ng; i++) { uintptr_t l = rp(gs[i] + O.gs_levels); int f = ri(gs[i] + O.gs_frames);
            if (C.botSkill == 1 && stale_gs(gs[i], f)) { skipped++; continue; }   /* v1.1 stage 4 (Normal only) */
            if (l && ri(l + 16) > 0 && f > bf) { bf = f; bi = i; } }
        if (skipped) LOGI("  skipped %d GameState cop%s that stopped updating earlier (still frozen)", skipped, skipped == 1 ? "y" : "ies");
        if (bi >= 0) { g_direct = gs[bi]; LOGI("No holder found; using GameState %p directly (most frames: %d).", (void *)g_direct, bf); }
    }
}


/* ---------- thread-static root: game.state through the thread's own TLS ----------
 * game.state is [ThreadStatic]. NativeAOT keeps thread statics like this:
 *   TEB -> ThreadLocalStoragePointer[tls index of SOR4.exe] -> TLS block (holds the runtime's Thread)
 *   Thread.m_pThreadLocalModuleStatics[moduleIndex] -> (handle ->) object[] storage
 *   storage[typeTlsIndex] -> statics object of `game`;  [+8] = game.state
 * moduleIndex/typeTlsIndex come from the helper the game itself calls (lea r8,[sym]; mov rcx,[r8];
 * mov rdx,[r8+8]). Every hop is a reference the GC keeps up to date, so unlike a raw heap address
 * this survives garbage collections that move objects. We find which TLS-block offset and which of
 * three plausible shapes holds the chain once, then re-walk it every frame. */
typedef LONG (WINAPI *NtQIT_t)(HANDLE, int, PVOID, ULONG, PULONG);
typedef struct { LONG ExitStatus; PVOID TebBaseAddress; PVOID UniqueProcess, UniqueThread; ULONG_PTR Affinity; LONG Priority, BasePriority; } TBI;
static struct {
    int ready; uintptr_t sym; int ti, mi; DWORD tlsIndex; size_t blockSize;
    int nthr; DWORD tid[MAXSTK]; uintptr_t teb[MAXSTK];
    int locked; DWORD ltid; uintptr_t lteb; int off, shape, inner, lmi; uintptr_t sEE;
    ULONGLONG lastTry, lastEnum; int tries, fails;
    uintptr_t testBlock;     /* tests only */
} T;
void state_test_tls(uintptr_t block, int mi, int ti) { T.testBlock = block; T.mi = mi; T.ti = ti; T.ready = 1; T.blockSize = 0x200; }
static void tls_setup(const uint8_t *h, const void *exeBase) {
    uint8_t b[14];
    if (T.testBlock) return;
    if (!rd((uintptr_t)h, b, 14) || b[0] != 0x4C || b[1] != 0x8D || b[2] != 0x05 || b[7] != 0x49 || b[8] != 0x8B || b[9] != 0x08 ||
        b[10] != 0x49 || b[11] != 0x8B || b[12] != 0x50 || b[13] != 0x08) { LOGI("thread-static root: helper has an unexpected shape; using the fallback search"); return; }
    T.sym = (uintptr_t)h + 7 + (intptr_t)g32(b, 3);
    T.ti = ri(T.sym + 8);
    const uint8_t *base = exeBase;
    IMAGE_NT_HEADERS64 nt; IMAGE_DOS_HEADER dos;
    if (!rd((uintptr_t)base, &dos, sizeof dos) || !rd((uintptr_t)base + dos.e_lfanew, &nt, sizeof nt)) return;
    IMAGE_DATA_DIRECTORY dd = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    IMAGE_TLS_DIRECTORY64 td;
    if (!dd.VirtualAddress || !rd((uintptr_t)base + dd.VirtualAddress, &td, sizeof td)) { LOGI("thread-static root: game has no TLS directory; using the fallback search"); return; }
    T.tlsIndex = (DWORD)ri((uintptr_t)td.AddressOfIndex);
    T.blockSize = (size_t)(td.EndAddressOfRawData - td.StartAddressOfRawData) + td.SizeOfZeroFill;
    if (T.blockSize < 64 || T.blockSize > 0x4000) T.blockSize = 0x1000;
    T.ready = T.ti >= 0 && T.ti < 100000;
    LOGI("thread-static root: game statics index %d (from rva 0x%x), TLS index %lu, TLS block %zu bytes%s", T.ti,
         (unsigned)(T.sym - (uintptr_t)M.base), T.tlsIndex, T.blockSize, T.ready ? "" : " - implausible, not used");
}
static int module_index(void) {
    uintptr_t tms = rp(T.sym);
    if (!tms) return -1;
    int mi = ri(tms + 8);
    return mi >= 0 && mi < 64 ? mi : -1;
}
static void enum_threads(ULONGLONG now) {
    if (T.testBlock || (T.lastEnum && now - T.lastEnum < 2000)) return;
    T.lastEnum = now; T.nthr = 0;
    uintptr_t stk[MAXSTK][2]; int nstk = 0;
    static NtQIT_t q; if (!q) q = (NtQIT_t)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
    if (!q) return;
    HANDLE sn = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (sn == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te; te.dwSize = sizeof te; DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(sn, &te); ok && T.nthr < MAXSTK; ok = Thread32Next(sn, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!th) continue;
        TBI bi; memset(&bi, 0, sizeof bi);
        if (q(th, 0, &bi, sizeof bi, NULL) == 0 && bi.TebBaseAddress) {
            uintptr_t teb = (uintptr_t)bi.TebBaseAddress;
            T.tid[T.nthr] = te.th32ThreadID; T.teb[T.nthr++] = teb;
            uintptr_t sb = rp(teb + 0x08), dealloc = rp(teb + 0x1478);
            if (sb && dealloc && dealloc < sb && nstk < MAXSTK) { stk[nstk][0] = dealloc; stk[nstk][1] = sb; nstk++; }
        }
        CloseHandle(th);
    }
    CloseHandle(sn);
    if (!g_busy) { memcpy(g_stk, stk, sizeof stk); g_nstk = nstk; }   /* the scanner reads this list; only touch it while it is idle */
}
static uintptr_t tls_block(uintptr_t teb) {
    if (T.testBlock) return T.testBlock;
    uintptr_t arr = rp(teb + 0x58);
    return arr ? rp(arr + 8 * (uintptr_t)T.tlsIndex) : 0;
}
/* shapes: 0 = slot holds object[] storage directly, 1 = array of object[] by module, 2 = array of handles */
static uintptr_t walk_chain(uintptr_t n, int shape, int mi, uintptr_t *sOut) {
    uintptr_t a = 0;
    if (!n) return 0;
    if (shape == 0) a = n;
    else if (shape == 1) a = rp(n + 8 * (uintptr_t)mi);
    else { uintptr_t hnd = rp(n + 8 * (uintptr_t)mi); a = hnd ? rp(hnd) : 0; }
    if (!a || (a & 7) || !ee_in_image(rp(a))) return 0;
    int len = ri(a + 8);
    if (len <= T.ti || len > 1000000) return 0;
    uintptr_t st = rp(a + 16 + 8 * (uintptr_t)T.ti);
    if (!st || (st & 7) || !ee_in_image(rp(st))) return 0;
    *sOut = st;
    return st;
}
static uintptr_t tls_try_block(uintptr_t blk, int *offOut, int *shapeOut, int *innerOut, int *miOut, uintptr_t *sOut, int deep) {
    int mis[4], nmi = 0, m = module_index();
    if (T.testBlock) mis[nmi++] = T.mi; else if (m >= 0) mis[nmi++] = m; else { mis[nmi++] = 0; mis[nmi++] = 1; mis[nmi++] = 2; }
    uint8_t buf[0x4000]; size_t n = T.blockSize;
    if (!blk || !rd(blk, buf, n)) return 0;
    for (size_t off = 0; off + 8 <= n; off += 8) {
        uintptr_t v = g64(buf, (int)off);
        if (v < 0x10000 || (v & 7)) continue;
        for (int k = 0; k < nmi; k++) for (int shape = 0; shape < 3; shape++) {
            uintptr_t st = 0;
            if (walk_chain(v, shape, mis[k], &st)) {
                uintptr_t gs = rp(st + 8);
                if (obj_is(gs, EE_GS) && gs_plausible(gs) && !meta_type_for_eetype(&M, (const void *)rp(st))) { *offOut = (int)off; *shapeOut = shape; *innerOut = -1; *miOut = mis[k]; *sOut = st; return gs; }
            }
        }
        if (!deep) continue;
        /* the Thread may be pointed to rather than inline: look one level deeper */
        uint8_t t2[0x400];
        if (!rd(v, t2, sizeof t2)) continue;
        for (int in = 0; in + 8 <= (int)sizeof t2; in += 8) {
            uintptr_t w = g64(t2, in);
            if (w < 0x10000 || (w & 7)) continue;
            for (int k = 0; k < nmi; k++) for (int shape = 0; shape < 3; shape++) {
                uintptr_t st = 0;
                if (walk_chain(w, shape, mis[k], &st)) {
                    uintptr_t gs = rp(st + 8);
                    if (obj_is(gs, EE_GS) && gs_plausible(gs) && !meta_type_for_eetype(&M, (const void *)rp(st))) { *offOut = (int)off; *shapeOut = shape; *innerOut = in; *miOut = mis[k]; *sOut = st; return gs; }
                }
            }
        }
    }
    return 0;
}
static void tls_discover(ULONGLONG now) {
    if (!T.ready || T.locked || (T.lastTry && now - T.lastTry < (T.tries < 30 ? 2000u : 30000u))) return;   /* m3: slower after 30 tries */
    T.lastTry = now; T.tries++;
    enum_threads(now);
    int nthr = T.testBlock ? 1 : T.nthr;
    uintptr_t bestGs = 0; int bestFrames = -1;
    int bo = 0, bs = 0, bi = -1, bm = 0, bt = -1; uintptr_t bst = 0;
    for (int pass = 0; pass < 2 && !bestGs; pass++)
        for (int i = 0; i < nthr; i++) {
            int deep = pass == 1;
            if (deep && !T.testBlock && T.tid[i] != GetCurrentThreadId() && T.tries % 3) continue;   /* deep look: our thread every time, others now and then */
            int off, shape, inner, mi; uintptr_t st;
            uintptr_t gs = tls_try_block(tls_block(T.testBlock ? 0 : T.teb[i]), &off, &shape, &inner, &mi, &st, deep);
            if (!gs) continue;
            int f = ri(gs + O.gs_frames);
            if (f > bestFrames) { bestFrames = f; bestGs = gs; bo = off; bs = shape; bi = inner; bm = mi; bst = st; bt = i; }
        }
    if (!bestGs) {
        if (T.tries == 1 || T.tries % 30 == 0) LOGI("thread-static root: game.state not found in any thread's TLS yet (try %d, %d threads, module index %d); %s", T.tries,
                                                  nthr, module_index(), T.tries == 1 ? "normal (game.state is only set while the game updates), will keep looking" : "still looking, every 30 s");
        return;
    }
    T.locked = 1; T.off = bo; T.shape = bs; T.inner = bi; T.lmi = bm; T.sEE = rp(bst);
    T.ltid = T.testBlock ? 0 : T.tid[bt]; T.lteb = T.testBlock ? 0 : T.teb[bt];
    static const char *const shapes[] = { "storage array", "per-module array", "per-module handle" };
    LOGI("Found game.state through thread %lu's TLS (%s thread): block+0x%x%s -> %s[%d] -> statics[%d] -> +8 (GameState %p, frame %d). "
         "Reading this chain every frame; it follows the game's garbage collections.",
         T.ltid, T.ltid == GetCurrentThreadId() || T.testBlock ? "our/main" : "another", T.off, T.inner >= 0 ? " -> Thread" : "",
         shapes[T.shape], T.lmi, T.ti, (void *)bestGs, bestFrames);
    if (T.inner >= 0) LOGI("  (Thread object pointed to from the TLS block, field +0x%x)", T.inner);
    stop_scanner(0, "thread-static root found");
    g_holder = 0; g_direct = 0;
}
/* 1 = chain intact (gs may be 0 = no game state right now); 0 = chain broken */
static int tls_read(uintptr_t *gsOut) {
    *gsOut = 0;
    if (!T.locked) return 0;
    uintptr_t blk = tls_block(T.lteb);
    uintptr_t v = blk ? rp(blk + (uintptr_t)T.off) : 0;
    if (T.inner >= 0) v = v ? rp(v + (uintptr_t)T.inner) : 0;
    uintptr_t st = 0;
    if (!walk_chain(v, T.shape, T.lmi, &st)) return T.fails++ < 30;      /* tolerate a few torn reads during a GC */
    if (rp(st) != T.sEE) return T.fails++ < 30;
    T.fails = 0;
    uintptr_t gs = rp(st + 8);
    if (gs && !obj_is(gs, EE_GS)) return 1;                               /* mid-update; treat as empty this frame */
    *gsOut = gs;
    return 1;
}

static int g_staleOn[4]; static int g_staleOnFrames[4];
static int g_menuQuiet;     /* m3: a menu that never has a level (title, character select, results...) is on screen */
static uintptr_t current_gs(const char **src) {
    ULONGLONG now = GetTickCount64();
    if (T.locked) {
        uintptr_t gs;
        if (tls_read(&gs)) { *src = gs ? "game.state" : "game.state (empty)"; return gs; }
        LOGI("thread-static chain to game.state broke (thread ended or the game changed it); looking again.");
        T.locked = 0; T.lastTry = 0;
    }
    tls_discover(now);
    if (T.locked) { uintptr_t gs; if (tls_read(&gs)) { *src = gs ? "game.state" : "game.state (empty)"; return gs; } }
    /* fallbacks (only while the thread-static chain is unknown) */
    if (g_csInit) adopt_results();
    int holderEmpty = 0;
    if (g_holder) {
        if (rp(g_holder) != g_holderEE) { LOGI("game.state holder moved or was freed; searching again."); g_holder = 0; }
        else {
            uintptr_t gs = rp(g_holder + 8);
            if (obj_is(gs, EE_GS)) { g_holderEmpty = 0; *src = "game.state (heap holder)"; return gs; }
            if (gs) { LOGI("game.state holder no longer holds a GameState; searching again."); g_holder = 0; }
            else {
                /* empty: a menu, or this holder is not the main thread's. Don't trust "empty" alone:
                   look at the other sources too, and drop the holder if they show a running level. */
                holderEmpty = 1;
                if (++g_holderEmpty == 30) LOGI("game.state holder has been empty for about 0.5 s; also checking the other sources.");
            }
        }
    }
    uintptr_t fb = 0; const char *fbSrc = NULL;
    if (g_direct) {
        if (obj_is(g_direct, EE_GS) && gs_plausible(g_direct)) { fb = g_direct; fbSrc = "GameState (direct)"; }   /* m3: no more searches while it works (a stale copy triggers one) */
        else g_direct = 0;
    }
    uintptr_t on = online_obj();
    if (!fb && on) for (int i = 0; i < 3; i++) if (g_onOff[i] > 0) {
        uintptr_t gs = rp(on + g_onOff[i]);
        if (!obj_is(gs, EE_GS) || !gs_plausible(gs)) continue;
        if (g_staleOn[i] && ri(gs + O.gs_frames) == g_staleOnFrames[i]) continue;    /* a frozen copy (offline: level-start snapshot) */
        g_staleOn[i] = 0;
        fb = gs; fbSrc = k_onNames[i]; if (!holderEmpty && g_scans < 3) request_scan("holder unknown");
        break;
    }
    if (holderEmpty) {
        uintptr_t l = fb ? rp(fb + O.gs_levels) : 0;
        static uintptr_t fbLast; static int fbFrames, fbAdv;      /* is the other source's level actually running? */
        int f = fb ? ri(fb + O.gs_frames) : 0;
        if (fb != fbLast) { fbLast = fb; fbFrames = f; fbAdv = 0; } else if (f != fbFrames) { fbFrames = f; fbAdv++; }
        if (fb && l && ri(l + 16) > 0 && g_holderEmpty >= 30 && fbAdv >= 10) {
            LOGI("game.state holder stayed empty while %s shows a level: it is not the game's main thread; dropping it and searching again.", fbSrc);
            g_holder = 0; g_holderEmpty = 0; g_lastScanReq = 0; request_scan("holder was another thread's");
            *src = fbSrc; return fb;
        }
        *src = "game.state (empty)"; return 0;
    }
    if (fb) { *src = fbSrc; return fb; }
    *src = "none";
    if (!g_menuQuiet) request_scan("no GameState yet");   /* m3: not while a known menu screen is up */
    return 0;
}
/* a source whose frame counter stopped: drop it if it's a fallback copy (stale after the GC moved the
   real object), keep it if it's the thread-static chain (then the game is simply paused) */
static void drop_stale_source(const char *src, int frames) {
    if (!strcmp(src, "GameState (direct)") && C.botSkill == 1 && g_direct) {   /* v1.1 stage 4 (Normal only): remember the frozen copy */
        int k = g_nStaleGS < 8 ? g_nStaleGS++ : (int)((unsigned)frames % 8u);
        g_staleGS[k].a = g_direct; g_staleGS[k].f = frames;
    }
    if (!strcmp(src, "GameState (direct)")) { LOGI("GameState copy stopped updating (frame %d): it was a stale copy; searching again.", frames); g_direct = 0; g_lastScanReq = 0; request_scan("stale copy"); return; }
    if (!strcmp(src, "game.state (heap holder)")) { LOGI("heap holder stopped updating; searching again."); g_holder = 0; return; }
    for (int i = 0; i < 3; i++) if (!strcmp(src, k_onNames[i])) { g_staleOn[i] = 1; g_staleOnFrames[i] = frames; LOGI("online.%s is a frozen copy (frame %d); not using it while it stays frozen.", k_onNames[i], frames); }
}

static int read_list(uintptr_t list, uintptr_t *out, int max) {
    uintptr_t h[3];
    if (!list || !rd(list, h, 24) || !ee_in_image(h[0]) || !h[1]) return 0;
    int size = (int)(int32_t)h[2], len = ri(h[1] + 8);
    if (size <= 0 || size > len) return 0;
    if (size > max) size = max;
    return rd(h[1] + 16, out, (size_t)size * 8) ? size : 0;
}
static struct { uintptr_t ee; int cs; } g_stCache[48]; static int g_nst;
static int classify_state(uintptr_t ee) {
    if (!ee) return CS_NONE;
    for (int i = 0; i < CS_COUNT; i++) if (ee == (uintptr_t)EE_ST[i]) return i;
    for (int i = 0; i < g_nst; i++) if (g_stCache[i].ee == ee) return g_stCache[i].cs;
    if (g_nst < 48) {
        char nm[160] = "?"; uint32_t td = meta_type_for_eetype(&M, (const void *)ee);
        if (td) meta_type_name(&M, td, nm, sizeof nm);
        LOGI("new character state type seen: %s (ee rva 0x%x)", nm, ee_in_image(ee) ? (unsigned)(ee - (uintptr_t)M.base) : 0);
        g_stCache[g_nst].ee = ee; g_stCache[g_nst++].cs = CS_OTHER;
    }
    return CS_OTHER;
}

/* main thread: log the reverse trace collected by the search thread (diagnostics, once) */
static void where_is(uintptr_t a, char *out, size_t n) {
    out[0] = 0;
    for (int i = 0; i < (T.testBlock ? 1 : T.nthr); i++) {
        uintptr_t blk = tls_block(T.testBlock ? 0 : T.teb[i]);
        if (!blk) continue;
        if (a >= blk && a < blk + T.blockSize) { snprintf(out, n, " = thread %lu TLS block +0x%x", T.testBlock ? 0 : T.tid[i], (unsigned)(a - blk)); return; }
        for (size_t off = 0; off + 8 <= T.blockSize; off += 8) {
            uintptr_t v = rp(blk + off);
            if (v >= 0x10000 && a >= v && a < v + 0x400) { snprintf(out, n, " = thread %lu: [TLS block +0x%x] +0x%x", T.testBlock ? 0 : T.tid[i], (unsigned)off, (unsigned)(a - v)); return; }
        }
    }
}
static void trace_log(void) {
    InterlockedExchange(&g_trState, 3);
    char wh[128];
    LOGI("statics trace (diagnostic): holder %p; module index guess %d, statics index %d, TLS index %lu, %d threads",
         (void *)g_tr.holder, module_index(), T.ti, T.tlsIndex, T.nthr);
    for (int i = 0; i < g_tr.nA; i++) {
        uintptr_t ae = g_tr.arr[i] ? rp(g_tr.arr[i]) : 0;
        where_is(g_tr.a[i], wh, sizeof wh);
        LOGI("  A%d: %p points at the holder%s%s", i, (void *)g_tr.a[i], wh,
             g_tr.arr[i] ? "" : " (not inside an array)");
        if (g_tr.arr[i]) LOGI("      inside array %p (ee rva 0x%x, length %d) at index %d", (void *)g_tr.arr[i],
                              ee_in_image(ae) ? (unsigned)(ae - (uintptr_t)M.base) : 0, ri(g_tr.arr[i] + 8), g_tr.idx[i]);
    }
    for (int i = 0; i < g_tr.nB; i++) { where_is(g_tr.b[i], wh, sizeof wh); LOGI("  B%d: %p -> %p%s", i, (void *)g_tr.b[i], (void *)g_tr.bv[i], wh); }
    for (int i = 0; i < g_tr.nC; i++) { where_is(g_tr.c[i], wh, sizeof wh); LOGI("  C%d: %p -> %p%s", i, (void *)g_tr.c[i], (void *)g_tr.cv[i], wh); }
    for (int i = 0; i < g_tr.nD; i++) { where_is(g_tr.d[i], wh, sizeof wh); LOGI("  D%d: %p -> %p%s", i, (void *)g_tr.d[i], (void *)g_tr.dv[i], wh); }
    if (!g_tr.nA) LOGI("  nothing points at the holder any more (it moved or was freed during the trace)");
}
void state_want_menu_check(void) {
    ULONGLONG now = GetTickCount64();
    if (T.locked || g_busy || !g_ok) return;
    if (g_lastSearchDone && now - g_lastSearchDone < 4000) return;
    if (g_lastScanReq && now - g_lastScanReq < 4000) return;
    g_lastScanReq = 0; request_scan("menu check");
}
/* ---------- m3: menus, character select, roster ---------- */
static void short_type_name(uintptr_t ee, char *out, size_t n) {
    out[0] = 0;
    if (!ee_in_image(ee)) return;
    static struct { uintptr_t ee; char nm[48]; } cache[32]; static int nc;
    for (int i = 0; i < nc; i++) if (cache[i].ee == ee) { snprintf(out, n, "%s", cache[i].nm); return; }
    char full[200] = ""; uint32_t td = meta_type_for_eetype(&M, (const void *)ee);
    if (td) meta_type_name(&M, td, full, sizeof full);
    const char *p = full, *q;
    for (q = full; *q; q++) if (*q == '.' || *q == '+') p = q + 1;
    snprintf(out, n, "%s", td ? p : "?");
    if (nc < 32) { cache[nc].ee = ee; snprintf(cache[nc].nm, sizeof cache[nc].nm, "%s", out); nc++; }
}
static int read_str(uintptr_t s, char *out, size_t n) {          /* .NET string -> ASCII (non-ASCII -> '?') */
    out[0] = 0;
    if (!s || (s & 7)) return 0;
    int len = ri(s + 8); if (len < 0 || len > 200) return 0;
    uint16_t w[64]; int m = len < 63 ? len : 63;
    if (m && !rd(s + 12, w, (size_t)m * 2)) return 0;
    size_t k = 0;
    for (int i = 0; i < m && k + 1 < n; i++) out[k++] = w[i] >= 32 && w[i] < 127 ? (char)w[i] : '?';
    out[k] = 0; return 1;
}
/* screens in a ScreenManager: count, and the short type name of the last (top) one */
static int screen_info(uintptr_t sm, char *top, size_t n) {
    if (top) top[0] = 0;
    if (!obj_is(sm, EE_SM) || O.sm_list < 0) return -1;
    uintptr_t list = rp(sm + O.sm_list), h[3];
    if (!list || !rd(list, h, 24) || !h[1]) return -1;
    int size = (int)(int32_t)h[2];
    if (size < 0 || size > 64) return -1;
    if (size && top) short_type_name(rp(rp(h[1] + 16 + 8 * (uintptr_t)(size - 1))), top, n);
    return size;
}
static uintptr_t cell_obj(const uint8_t *cell) { return cell ? cell_object(cell) : 0; }
static void read_menu(World *w, uintptr_t on) {
    w->menuRead = 0; w->menuName[0] = 0; w->topScreen[0] = 0; w->mainScreens = w->pauseScreens = -1; w->charSel = 0; w->ncs = 0;
    uintptr_t prog = cell_obj(g_progCell);
    if (prog && g_offMainMenu > 0) w->mainScreens = screen_info(rp(prog + g_offMainMenu), w->topScreen, sizeof w->topScreen);
    if (prog && g_offPauseMenu > 0) w->pauseScreens = screen_info(rp(prog + g_offPauseMenu), NULL, 0);
    g_menuQuiet = 0;
    if (!on || g_onGlobal < 0 || !EE_MGS || O.mgs_menu < 0) return;
    uintptr_t mgs = rp(on + g_onGlobal);
    if (!obj_is(mgs, EE_MGS)) return;
    w->menuRead = 1;
    if (O.mgs_ff >= 0) { uint8_t ff = 0; if (rd(mgs + O.mgs_ff, &ff, 1)) w->friendlyFire = ff != 0; }
    uintptr_t menu = rp(mgs + O.mgs_menu);
    if (menu) short_type_name(rp(menu), w->menuName, sizeof w->menuName);
    int isCs = menu && EE_MENU_CS && rp(menu) == (uintptr_t)EE_MENU_CS;
    /* per player: confirmed character and color (globalState.playerArray), cursor (slot user data) */
    uintptr_t arr = O.mgs_players >= 0 ? rp(mgs + O.mgs_players) : 0, aee = arr ? rp(arr) : 0; uint16_t comp = 0;
    uintptr_t alt = isCs && O.cs_altConfirmed >= 0 ? rp(menu + O.cs_altConfirmed) : 0; int altN = alt ? ri(alt + 8) : 0;
    if (aee && rd(aee, &comp, 2) && comp >= 8 && comp <= 512) {
        int n = ri(arr + 8);
        for (int i = 0; i < n && i < 8; i++) {
            int32_t v[2] = { 0, 0 };
            if (!rd(arr + 16 + (uintptr_t)i * comp + (O.ps_playable >= 0 ? O.ps_playable : 0), &v[0], 4)) break;
            if (O.ps_palette >= 0) rd(arr + 16 + (uintptr_t)i * comp + O.ps_palette, &v[1], 4);
            w->cs[i].playable = v[0]; w->cs[i].palette = v[1]; w->cs[i].cursor = -1; w->cs[i].haveUser = 0;
            uint8_t b = 0; w->cs[i].altConfirmed = i < altN && rd(alt + 16 + (uintptr_t)i, &b, 1) ? b : -1;
            w->ncs = i + 1;
        }
    }
    if (on && g_onSlots > 0 && O.sl_user >= 0 && O.ud_entry >= 0) {
        uintptr_t sa = rp(on + g_onSlots); int n = sa ? ri(sa + 8) : 0;
        for (int i = 0; i < n && i < w->ncs; i++) {
            uintptr_t s = rp(sa + 16 + 8 * (uintptr_t)i), ud = obj_is(s, EE_SLOT) ? rp(s + O.sl_user) : 0;
            if (obj_is(ud, EE_UD)) { w->cs[i].cursor = ri(ud + O.ud_entry); w->cs[i].haveUser = 1; }
        }
    }
    static const char *const quiet[] = { "CharacterSelection", "MainMenu", "StageSummary", "Video", "Credits", "SurvivalResult" };
    int q = 0;
    for (int i = 0; i < 6; i++) if (!strcmp(w->menuName, quiet[i])) q = 1;
    q = q && w->mainScreens > 0 && w->pauseScreens <= 0;
    g_menuQuiet = q;
    static char lastMenu[40] = "-", lastTop[48] = "-";
    if (strcmp(lastMenu, w->menuName) || strcmp(lastTop, w->topScreen)) {
        snprintf(lastMenu, sizeof lastMenu, "%s", w->menuName); snprintf(lastTop, sizeof lastTop, "%s", w->topScreen);
        LOGI("Screen: menu '%s', top screen '%s' (%d menu screens, %d pause screens)%s", w->menuName[0] ? w->menuName : "none",
             w->topScreen[0] ? w->topScreen : "none", w->mainScreens, w->pauseScreens, q ? "; no searching here" : "");
    }
    w->charSel = isCs && w->mainScreens > 0 && EE_CSSCREEN && !strcmp(w->topScreen, "CharacterSelectionScreen") && w->pauseScreens <= 0;
}
static RosterEntry g_roster[ROSTER_MAX]; static int g_nroster; static ULONGLONG g_rosterTry;
static void nav_build(void);
static int roster_read(void) {
    uintptr_t mc = cell_obj(g_mainCell);
    if (!mc || g_offCfgRef < 0 || O.mc_chars < 0 || O.pc_name < 0) return 0;
    uintptr_t cfg = rp(mc + g_offCfgRef + 8);                 /* AssetRef_s: path +0, strongRef +8, version +16 */
    if (!obj_is(cfg, EE_MCD)) return 0;
    uintptr_t arr = rp(cfg + O.mc_chars); int n = arr ? ri(arr + 8) : 0;
    if (n <= 0 || n > ROSTER_MAX) return 0;
    RosterEntry r[ROSTER_MAX]; memset(r, 0, sizeof r);
    for (int i = 0; i < n; i++) {
        uintptr_t pc = rp(arr + 16 + 8 * (uintptr_t)i);
        if (!obj_is(pc, EE_PC)) return 0;
        if (!read_str(rp(pc + O.pc_name), r[i].name, sizeof r[i].name) || !r[i].name[0]) snprintf(r[i].name, sizeof r[i].name, "Fighter %d", i + 1);
        uintptr_t sk = O.pc_skins >= 0 ? rp(pc + O.pc_skins) : 0;
        r[i].palettes = (sk ? ri(sk + 8) : 0) + 1;            /* validate_palette: alternateSkinArray.Length + 1 */
        if (r[i].palettes < 1 || r[i].palettes > 64) r[i].palettes = 1;
        r[i].menuId = O.pc_id >= 0 ? ri(pc + O.pc_id) : i;
        r[i].left = O.pc_left >= 0 ? ri(pc + O.pc_left) : -1; r[i].right = O.pc_right >= 0 ? ri(pc + O.pc_right) : -1;
        r[i].up = O.pc_up >= 0 ? ri(pc + O.pc_up) : -1; r[i].down = O.pc_down >= 0 ? ri(pc + O.pc_down) : -1;
        uint8_t f[2] = { 0, 0 };
        if (O.pc_adam >= 0) rd(pc + O.pc_adam, &f[0], 1);
        if (O.pc_roo >= 0) rd(pc + O.pc_roo, &f[1], 1);
        r[i].flags = (f[0] ? 1 : 0) | (f[1] ? 2 : 0);
    }
    memcpy(g_roster, r, sizeof r); g_nroster = n;
    char b[2400]; int k = 0;
    for (int i = 0; i < n && k < (int)sizeof b - 80; i++)
        k += snprintf(b + k, sizeof b - k, "%s#%d %s (%d color%s, menu id %d, links L%d R%d U%d D%d%s%s)", i ? "; " : "", i, r[i].name, r[i].palettes,
                      r[i].palettes == 1 ? "" : "s", r[i].menuId, r[i].left, r[i].right, r[i].up, r[i].down, r[i].flags & 1 ? ", SoR4 Adam" : "", r[i].flags & 2 ? ", SoR3 Roo" : "");
    LOGI("Character roster (%d): %s", n, b);
    nav_build();
    return n;
}
int state_roster(const RosterEntry **out) {
    *out = g_roster;
    if (!g_nroster && state_available()) {
        ULONGLONG now = GetTickCount64();
        if (!g_rosterTry || now - g_rosterTry >= 3000) { g_rosterTry = now; roster_read(); }
    }
    return g_nroster;
}
int state_roster_find(const char *name) {
    const RosterEntry *r; int n = state_roster(&r);
    if (!name || !name[0]) return -1;
    for (int i = 0; i < n; i++) if (!_stricmp(r[i].name, name)) return i;
    return -1;
}
/* ---- m7.1: navigation graph ---- */
static int g_nav[ROSTER_MAX][4], g_learn[ROSTER_MAX][4], g_noEff[ROSTER_MAX][4], g_navIdMode;
static const char *const k_navDir[4] = { "up", "down", "left", "right" };
static int link_of(const RosterEntry *e, int d) { return d == 0 ? e->up : d == 1 ? e->down : d == 2 ? e->left : e->right; }
static void nav_build(void) {
    int n = g_nroster; const RosterEntry *r = g_roster;
    /* which meaning do the links have? m3 tests: unique menu ids and every link names a menu id -> menu-id links.
       The real game (m7 log): menuId is a group number (0 main row, 1 SoR1/SoR2 grid, 2 SoR3 grid), links are indices. */
    int unique = 1, allIds = 1, any = 0;
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (r[i].menuId == r[j].menuId) unique = 0;
    for (int i = 0; i < n; i++) for (int d = 0; d < 4; d++) {
        int v = link_of(&r[i], d); if (v < 0) continue; any = 1;
        int f = 0; for (int j = 0; j < n; j++) if (r[j].menuId == v) f = 1;
        if (!f) allIds = 0;
    }
    g_navIdMode = unique && any && allIds;
    for (int i = 0; i < n; i++) for (int d = 0; d < 4; d++) {
        int v = link_of(&r[i], d), to = -1;
        if (g_navIdMode) { if (v >= 0) for (int j = 0; j < n; j++) if (r[j].menuId == v) to = j; }
        else if (v >= 0 && v < n && v != i) to = v;
        else {
            int opp = d ^ 1, best = -1;                         /* reverse of an explicit link: j's opposite link points at i */
            for (int j = 0; j < n; j++) if (j != i && link_of(&r[j], opp) == i) {
                if (best < 0 || (d >= 2 && r[j].up < 0 && r[best].up >= 0)) best = j;      /* several: the top-row one */
            }
            to = best;
            if (to < 0 && d >= 2) {                             /* left/right: next entry of the same menu group */
                for (int k = d == 2 ? i - 1 : i + 1; k >= 0 && k < n; k += d == 2 ? -1 : 1)
                    if (r[k].menuId == r[i].menuId) { to = k; break; }
            }
        }
        g_nav[i][d] = to; g_learn[i][d] = -2; g_noEff[i][d] = 0;
    }
    char b[2400]; int k = 0;
    for (int i = 0; i < n && k < (int)sizeof b - 80; i++)
        k += snprintf(b + k, sizeof b - k, "%s%s: L %s R %s U %s D %s", i ? "; " : "", r[i].name,
                      g_nav[i][2] >= 0 ? r[g_nav[i][2]].name : "-", g_nav[i][3] >= 0 ? r[g_nav[i][3]].name : "-",
                      g_nav[i][0] >= 0 ? r[g_nav[i][0]].name : "-", g_nav[i][1] >= 0 ? r[g_nav[i][1]].name : "-");
    LOGI("Character select navigation (%s links; game links + inferred): %s", g_navIdMode ? "menu-id" : "index", b);
}
void state_roster_set(const RosterEntry *r, int n) {
    if (n > ROSTER_MAX) n = ROSTER_MAX;
    memcpy(g_roster, r, sizeof *r * (size_t)n); g_nroster = n; nav_build();
}
int state_roster_nav_static(int from, int dir) { return from >= 0 && from < g_nroster && dir >= 0 && dir < 4 ? g_nav[from][dir] : -1; }
int state_roster_noeffect(int from, int dir) { return from >= 0 && from < g_nroster && dir >= 0 && dir < 4 ? g_noEff[from][dir] : 0; }
int state_roster_nav(int from, int dir) {
    if (from < 0 || from >= g_nroster || dir < 0 || dir > 3) return -1;
    if (g_noEff[from][dir] >= 2) return -1;
    if (g_learn[from][dir] >= 0) return g_learn[from][dir] == from ? -1 : g_learn[from][dir];
    return g_nav[from][dir];
}
void state_roster_observe(int from, int dir, int to) {
    if (from < 0 || from >= g_nroster || dir < 0 || dir > 3 || to < 0 || to >= g_nroster) return;
    if (to == from) {
        g_noEff[from][dir]++;
        LOGI("Character select: %s from %s did nothing (%d time(s))%s.", k_navDir[dir], g_roster[from].name, g_noEff[from][dir],
             g_noEff[from][dir] >= 2 ? "; not using that move again this visit" : "");
        return;
    }
    g_noEff[from][dir] = 0;
    if (g_learn[from][dir] != to) {
        int st = g_nav[from][dir];
        LOGI("Character select: seen %s -%s-> %s (%s).", g_roster[from].name, k_navDir[dir], g_roster[to].name,
             st == to ? "as expected" : st >= 0 ? "the table said something else; using what the game did" : "the table had no move here; learned");
        g_learn[from][dir] = to;
    }
}
void state_roster_nav_new_visit(void) { memset(g_noEff, 0, sizeof g_noEff); }
int state_roster_path(int from, int to, int *firstDir) {
    *firstDir = -1;
    if (from < 0 || to < 0 || from >= g_nroster || to >= g_nroster) return -1;
    if (from == to) return 0;
    int dist[ROSTER_MAX], first[ROSTER_MAX], q[ROSTER_MAX], qh = 0, qt = 0;
    static const int order[4] = { 2, 3, 0, 1 };                /* ties: left/right before up/down */
    for (int i = 0; i < g_nroster; i++) dist[i] = -1;
    dist[from] = 0; first[from] = -1; q[qt++] = from;
    while (qh < qt) {
        int u = q[qh++];
        for (int k = 0; k < 4; k++) {
            int d = order[k], v = state_roster_nav(u, d);
            if (v < 0 || dist[v] >= 0) continue;
            dist[v] = dist[u] + 1; first[v] = u == from ? d : first[u]; q[qt++] = v;
            if (v == to) { *firstDir = first[v]; return dist[v]; }
        }
    }
    return -1;
}

/* ---- m4: caches for asset data (CharacterData / PickupData objects live as long as the level) ---- */
static int g_cacheScene = -12345;
static struct { uintptr_t p; int boss, canGuard, shieldUser; float maxStamina, maxLife; uint64_t blockMask; char nm[24]; } g_cd[64]; static int g_ncd;
static int read_str(uintptr_t s, char *out, size_t n);
/* m6.7: block-armor moves (shields) and stamina of one CharacterData, cached with the boss flag */
static void char_data_shield(uintptr_t cd, Chr *c) {
    c->maxStamina = -1; c->shieldUser = 0; c->shieldMove = 0; c->dname[0] = 0; c->dataMaxLife = -1;
    if (!cd) return;
    int i = 0; for (; i < g_ncd; i++) if (g_cd[i].p == cd) break;
    if (i == g_ncd) return;                      /* char_data_boss adds it first */
    if (g_cd[i].maxStamina == -2) {              /* not read yet */
        g_cd[i].maxStamina = OFF_cd_maxSt >= 0 ? fx(ri(cd + OFF_cd_maxSt)) : -1;
        g_cd[i].nm[0] = 0;
        /* v1.1 stage 3: _name never read (name=? in every log); displayName (a localization key) only when it is a string */
        if (OFF_cd_dname >= 0) { uintptr_t sp = rp(cd + OFF_cd_dname); if (obj_is(sp, EE_STRING)) read_str(sp, g_cd[i].nm, sizeof g_cd[i].nm); }
        if (!g_cd[i].nm[0] && OFF_cd_name >= 0) { uintptr_t sp = rp(cd + OFF_cd_name); if (!EE_STRING || obj_is(sp, EE_STRING)) read_str(sp, g_cd[i].nm, sizeof g_cd[i].nm); }
        g_cd[i].maxLife = OFF_cd_maxLife >= 0 ? fx(ri(cd + OFF_cd_maxLife)) : -1;
        if (g_cd[i].maxLife < 0 || g_cd[i].maxLife > 100000) g_cd[i].maxLife = -1;
        uint64_t mask = 0; uintptr_t arr = OFF_cd_moves >= 0 ? rp(cd + OFF_cd_moves) : 0; int n = arr ? ri(arr + 8) : 0;
        if (n > 0 && n <= 256 && OFF_mv_block >= 0) {
            uintptr_t mv[64]; int m = n < 64 ? n : 64;
            if (rd(arr + 16, mv, (size_t)m * 8)) for (int k = 0; k < m; k++) { uint8_t v = 0; if (mv[k] && rd(mv[k] + OFF_mv_block, &v, 1) && v) mask |= 1ull << k; }
        }
        g_cd[i].blockMask = mask;
        char lo[24]; int k = 0; for (; g_cd[i].nm[k] && k < 23; k++) lo[k] = (char)(g_cd[i].nm[k] >= 'A' && g_cd[i].nm[k] <= 'Z' ? g_cd[i].nm[k] + 32 : g_cd[i].nm[k]); lo[k] = 0;
        g_cd[i].shieldUser = mask != 0 || strstr(lo, "murphy") || strstr(lo, "shield");
        LOGI("character data %s: maxLife %.1f maxStamina %.1f, block-armor moves 0x%llx%s", g_cd[i].nm[0] ? g_cd[i].nm : "?", g_cd[i].maxLife, g_cd[i].maxStamina, (unsigned long long)mask, g_cd[i].shieldUser ? " (shield user)" : "");
    }
    c->maxStamina = g_cd[i].maxStamina; c->shieldUser = g_cd[i].shieldUser; snprintf(c->dname, sizeof c->dname, "%s", g_cd[i].nm); c->dataMaxLife = g_cd[i].maxLife;
    c->shieldMove = c->moveId >= 0 && c->moveId < 64 && (g_cd[i].blockMask >> c->moveId & 1);
}
static struct { uintptr_t p; int kind, starGain, onlyThrow, score, wtype, knife; float lifeGain, maxLife; char wname[24]; } g_pd[64]; static int g_npd;
static int read_str(uintptr_t s, char *out, size_t n);
static int name_is_knife(const char *n) {
    char l[32]; size_t i = 0; for (; n[i] && i < sizeof l - 1; i++) l[i] = (char)(n[i] >= 'A' && n[i] <= 'Z' ? n[i] + 32 : n[i]); l[i] = 0;
    return strstr(l, "knife") || strstr(l, "kunai") || strstr(l, "dagger") || strstr(l, "shuriken") || strstr(l, "couteau");
}
static int char_data_boss(uintptr_t cd, int *canGuard) {
    *canGuard = 0;
    if (!cd) return 0;
    for (int i = 0; i < g_ncd; i++) if (g_cd[i].p == cd) { *canGuard = g_cd[i].canGuard; return g_cd[i].boss; }
    uint8_t v = 0; if (O.cd_boss >= 0 && !rd(cd + O.cd_boss, &v, 1)) return 0;
    int gc = O.cd_guardCrush >= 0 ? ri(cd + O.cd_guardCrush) > 0 : 0;
    if (g_ncd < 64) { g_cd[g_ncd].p = cd; g_cd[g_ncd].canGuard = gc; g_cd[g_ncd].maxStamina = -2; g_cd[g_ncd++].boss = v != 0; }
    *canGuard = gc;
    return v != 0;
}
static void pickup_data(uintptr_t pd, Pick *p) {
    if (!pd) return;
    int i = 0;
    for (; i < g_npd; i++) if (g_pd[i].p == pd) break;
    if (i == g_npd) {
        int kind = PK_OTHER, star = 0, only = 0, score = 0, wtype = -1, knife = 0; float life = 0, maxLife = -1; char wname[24] = "";
        if (O.pd_score >= 0) score = ri(pd + O.pd_score);
        uintptr_t wd = O.pd_weapon >= 0 ? rp(pd + O.pd_weapon) : 0;
        if (O.pd_life >= 0) life = fx(ri(pd + O.pd_life));
        if (O.pd_star >= 0) star = ri(pd + O.pd_star);
        if (wd) {
            kind = PK_WEAPON;
            if (O.wd_onlyThrow >= 0) { uint8_t b = 0; rd(wd + O.wd_onlyThrow, &b, 1); only = b != 0; }
            if (O.wd_explDur >= 0 && ri(wd + O.wd_explDur) > 0) kind = PK_GRENADE;
            if (O.wd_maxLife >= 0) maxLife = fx(ri(wd + O.wd_maxLife));
            if (O.wd_type >= 0) wtype = ri(wd + O.wd_type);
            if (O.wd_name >= 0) read_str(rp(wd + O.wd_name), wname, sizeof wname);
            knife = name_is_knife(wname);
        } else if (life > 0) kind = PK_FOOD;
        else if (star > 0) kind = PK_STAR;
        else if (score > 0) kind = PK_SCORE;
        if (g_npd >= 64) { p->kind = kind; p->lifeGain = life; p->starGain = star; p->onlyThrow = only; p->scoreGain = score; p->maxLife = maxLife; p->wtype = wtype; p->knife = knife; snprintf(p->wname, sizeof p->wname, "%s", wname); return; }
        g_pd[i].p = pd; g_pd[i].kind = kind; g_pd[i].lifeGain = life; g_pd[i].starGain = star; g_pd[i].onlyThrow = only; g_pd[i].score = score;
        g_pd[i].maxLife = maxLife; g_pd[i].wtype = wtype; g_pd[i].knife = knife; snprintf(g_pd[i].wname, sizeof g_pd[i].wname, "%s", wname); g_npd++;
        if (kind == PK_WEAPON || kind == PK_GRENADE) LOGI("Weapon data: \"%s\" type %d maxLife %.1f onlyThrow %d%s", wname[0] ? wname : "?", wtype, maxLife, only, knife ? " (knife: thrown at range)" : "");
    }
    p->kind = g_pd[i].kind; p->lifeGain = g_pd[i].lifeGain; p->starGain = g_pd[i].starGain; p->onlyThrow = g_pd[i].onlyThrow; p->scoreGain = g_pd[i].score;
    p->maxLife = g_pd[i].maxLife; p->wtype = g_pd[i].wtype; p->knife = g_pd[i].knife; snprintf(p->wname, sizeof p->wname, "%s", g_pd[i].wname);
}
/* m7.2: DestroyableObjectInstance.dataRef has no offset in the metadata; its own fields leave a 24-byte gap at
   144..167 (an AssetRef_s, strongRef at +8). Each candidate is checked against the DestroyableObjectData type. */
static struct { uintptr_t d; int explosive, blowTime; float maxLife, radX, radZ; char name[24]; } g_dod[32]; static int g_ndod, g_dodOff = -1;
static int read_str(uintptr_t s, char *out, size_t n);
static void destroyable_data(uintptr_t inst, Brk *b) {
    b->explosive = -1; b->blowTime = 0; b->maxLife = -1; b->radX = b->radZ = 0; b->name[0] = 0;
    if (!EE_DOD) return;
    uintptr_t d = 0;
    if (g_dodOff >= 0) { d = rp(inst + g_dodOff); if (!obj_is(d, EE_DOD)) d = 0; }
    if (!d) { static const int cand[] = { 152, 144, 160, 168, 176 };
        for (int k = 0; k < 5 && !d; k++) { uintptr_t p = rp(inst + cand[k]); if (obj_is(p, EE_DOD)) { d = p; if (g_dodOff != cand[k]) LOGI("Breakable data: DestroyableObjectInstance.dataRef found at +%d", cand[k]); g_dodOff = cand[k]; } } }
    if (!d) return;
    int i = 0; for (; i < g_ndod; i++) if (g_dod[i].d == d) break;
    if (i == g_ndod) {
        int bt = O.dod_blowTime >= 0 ? ri(d + O.dod_blowTime) : 0;
        uintptr_t dmg = O.dod_dmg >= 0 ? rp(d + O.dod_dmg + 8) : 0, hz = O.dod_haz >= 0 ? rp(d + O.dod_haz + 8) : 0, pj = O.dod_proj >= 0 ? rp(d + O.dod_proj + 8) : 0;
        float rx = 0, rz = 0; char nm[24] = "";
        if (dmg && EE_DAD && obj_is(dmg, EE_DAD) && O.dad_rad >= 0) { rx = fx(ri(dmg + O.dad_rad)); rz = fx(ri(dmg + O.dad_rad + 4)); } else if (dmg && !obj_is(dmg, EE_DAD)) dmg = 0;
        if (O.dod_name >= 0) read_str(rp(d + O.dod_name), nm, sizeof nm);
        char l[24]; size_t k = 0; for (; nm[k] && k < sizeof l - 1; k++) l[k] = (char)(nm[k] >= 'A' && nm[k] <= 'Z' ? nm[k] + 32 : nm[k]); l[k] = 0;
        int byName = strstr(l, "explos") != NULL;   /* m7.3: not "barrel" (m7.2-run1: OBJ_BARREL / OBJ_BARREL_TOXIC are ordinary; OBJ_BARREL_EXPLOSIVE has willBlowStateTime 60) */
        int ex = bt > 0 || dmg != 0 || pj != 0 || byName;   /* hazard-on-destroy alone is only logged */
        if (g_ndod < 32) { i = g_ndod++; } else i = 31;
        g_dod[i].d = d; g_dod[i].explosive = ex; g_dod[i].blowTime = bt; g_dod[i].maxLife = O.dod_maxLife >= 0 ? fx(ri(d + O.dod_maxLife)) : -1;
        g_dod[i].radX = rx > 0 && rx < 20 ? rx : 0; g_dod[i].radZ = rz > 0 && rz < 20 ? rz : 0; snprintf(g_dod[i].name, sizeof g_dod[i].name, "%s", nm);
        LOGI("Breakable data: \"%s\" maxLife %.1f willBlowStateTime %d damageArea %s (radius %.1f x %.1f) hazard %s projectile %s -> %s",
             nm[0] ? nm : "?", g_dod[i].maxLife, bt, dmg ? "yes" : "no", rx, rz, hz ? "yes" : "no", pj ? "yes" : "no", ex ? "EXPLOSIVE (bots keep away once it is hit)" : "ordinary");
    }
    b->explosive = g_dod[i].explosive; b->blowTime = g_dod[i].blowTime; b->maxLife = g_dod[i].maxLife; b->radX = g_dod[i].radX; b->radZ = g_dod[i].radZ;
    snprintf(b->name, sizeof b->name, "%s", g_dod[i].name);
}
/* projectiles and thrown pickups; velocity from the previous read of the same uid */
static struct { uint32_t uid; V3 pos; int seen; } g_pjPrev[W_MAXPJ]; static int g_npjPrev;
static struct { uint32_t uid; V3 pos; } g_rpPrev[W_MAXRP]; static int g_nrpPrev;   /* m7.4 */
/* m7.4: LevelState.robotList -> RobotInstance (drivers, parts, flags) and RobotData (part names / life, heal, damage factor) */
static int read_str(uintptr_t s, char *out, size_t n);
static struct { uintptr_t d; int n; char nm[12][20]; float life[12]; float heal, dmg; } g_rdat[4]; static int g_nrdat;
static int robot_data(uintptr_t d) {
    if (!d || !obj_is(d, EE_RDATA)) return -1;
    for (int i = 0; i < g_nrdat; i++) if (g_rdat[i].d == d) return i;
    int i = g_nrdat < 4 ? g_nrdat++ : 3; memset(&g_rdat[i], 0, sizeof g_rdat[i]); g_rdat[i].d = d;
    g_rdat[i].heal = OFF_rd_heal >= 0 ? fx(ri(d + OFF_rd_heal)) : -1; g_rdat[i].dmg = OFF_rd_dmg >= 0 ? fx(ri(d + OFF_rd_dmg)) : -1;
    uintptr_t arr = OFF_rd_parts >= 0 ? rp(d + OFF_rd_parts) : 0; int n = arr ? ri(arr + 8) : 0; if (n < 0 || n > 64) n = 0;
    char line[600]; int L = snprintf(line, sizeof line, "Robot data: %d part(s), healLife %.1f, hitDamageFactor %.2f:", n, g_rdat[i].heal, g_rdat[i].dmg);
    for (int k = 0; k < n && k < 12; k++) {
        uintptr_t pd = rp(arr + 16 + 8 * (uintptr_t)k); if (!pd) continue;
        uintptr_t sp = rp(pd + 8);                     /* <Name>k__BackingField has no offset in the metadata: first field (+8), checked as a string */
        if (sp && (!EE_STRING || obj_is(sp, EE_STRING))) read_str(sp, g_rdat[i].nm[k], sizeof g_rdat[i].nm[k]);
        g_rdat[i].life[k] = OFF_rpd_life >= 0 ? (float)ri(pd + OFF_rpd_life) : -1;
        if (L < (int)sizeof line - 40) L += snprintf(line + L, sizeof line - L, " [%d] %s life %.0f;", k, g_rdat[i].nm[k][0] ? g_rdat[i].nm[k] : "?", g_rdat[i].life[k]);
    }
    g_rdat[i].n = n < 12 ? n : 12;
    LOGI("%s", line);
    return i;
}
static void read_robots(World *w, uintptr_t list) {
    uintptr_t rl[W_MAXRB]; int n = read_list(list, rl, W_MAXRB);
    for (int i = 0; i < n && w->nrb < W_MAXRB; i++) {
        if (!obj_is(rl[i], EE_ROBOT)) continue;
        Robot *r = &w->rb[w->nrb++]; memset(r, 0, sizeof *r);
        uint8_t v = 0;
        r->uid = OFF_rb_uid >= 0 ? ri(rl[i] + OFF_rb_uid) : -1;
        r->active = OFF_rb_active >= 0 && rd(rl[i] + OFF_rb_active, &v, 1) ? v != 0 : -1;
        r->broken = OFF_rb_broken >= 0 && rd(rl[i] + OFF_rb_broken, &v, 1) ? v != 0 : -1;
        r->firstDriver = OFF_rb_first >= 0 && rd(rl[i] + OFF_rb_first, &v, 1) ? v != 0 : -1;
        r->animId = OFF_rb_anim >= 0 ? ri(rl[i] + OFF_rb_anim) : -1;
        if (OFF_rb_pos >= 0) { r->pos.x = fx(ri(rl[i] + OFF_rb_pos)); r->pos.y = fx(ri(rl[i] + OFF_rb_pos + 4)); r->pos.z = fx(ri(rl[i] + OFF_rb_pos + 8)); }
        uintptr_t da = OFF_rb_drivers >= 0 ? rp(rl[i] + OFF_rb_drivers) : 0; int nd = da ? ri(da + 8) : 0;
        if (nd > 0 && nd <= 8) { r->nDrivers = nd < 4 ? nd : 4; for (int k = 0; k < r->nDrivers; k++) r->drivers[k] = (uint32_t)ri(da + 16 + 4 * (uintptr_t)k); }
        uintptr_t pa = OFF_rb_parts >= 0 ? rp(rl[i] + OFF_rb_parts) : 0; int np = pa ? ri(pa + 8) : 0; r->nParts = np >= 0 && np <= 64 ? np : -1;
        int di = OFF_rb_data >= 0 ? robot_data(rp(rl[i] + OFF_rb_data + 8)) : -1;     /* AssetRef_s: strongRef +8 */
        if (di < 0 && OFF_rb_data >= 0) di = robot_data(rp(rl[i] + OFF_rb_data));
        r->healLife = di >= 0 ? g_rdat[di].heal : -1; r->dmgFactor = di >= 0 ? g_rdat[di].dmg : -1;
        for (int k = 0; k < r->nDrivers; k++) for (int c = 0; c < w->nch; c++) if (w->ch[c].uid == r->drivers[k] && r->drivers[k]) w->ch[c].robotDriver = 1;
        for (int p = 0; p < w->nrp; p++) if (w->rp[p].robotId == r->uid && di >= 0 && w->rp[p].partId >= 0 && w->rp[p].partId < g_rdat[di].n) {
            snprintf(w->rp[p].name, sizeof w->rp[p].name, "%s", g_rdat[di].nm[w->rp[p].partId]); w->rp[p].dataLife = g_rdat[di].life[w->rp[p].partId]; }
        /* changes, so a run log shows the driver swaps / activation (capped) */
        static struct { int uid, active, broken, first; uint32_t d0, d1; } prev[W_MAXRB]; static int nprev, logs;
        int k = 0; for (; k < nprev; k++) if (prev[k].uid == r->uid) break;
        if (k == nprev && nprev < W_MAXRB) { nprev++; prev[k].uid = r->uid; prev[k].active = -2; }
        if (k < W_MAXRB && (prev[k].active != r->active || prev[k].broken != r->broken || prev[k].first != r->firstDriver || prev[k].d0 != r->drivers[0] || prev[k].d1 != r->drivers[1]) && logs++ < 300) {
            LOGI("robot %d: active %d broken %d firstDriver %d drivers %u,%u (%d) parts %d anim %d at (%.1f, %.1f, %.1f)", r->uid, r->active, r->broken, r->firstDriver,
                 r->drivers[0], r->nDrivers > 1 ? r->drivers[1] : 0, r->nDrivers, r->nParts, r->animId, r->pos.x, r->pos.y, r->pos.z);
            prev[k].active = r->active; prev[k].broken = r->broken; prev[k].first = r->firstDriver; prev[k].d0 = r->drivers[0]; prev[k].d1 = r->drivers[1];
        }
    }
}
static void read_projectiles(World *w, uintptr_t list) {
    uintptr_t ol[256]; int n = read_list(list, ol, 256);
    uint8_t hb[64], sb[512];
    for (int i = 0; i < n; i++) {
        if (!rd(ol[i], hb, 48)) continue;
        uintptr_t ee = g64(hb, 0); int kind;
        if (EE_DESTR && ee == (uintptr_t)EE_DESTR) {          /* m5: breakable object */
            if (w->nbr >= W_MAXBR) continue;
            uintptr_t spr = rp(ol[i] + O.pob_sprite);
            if (!spr || !rd(spr, sb, O.sprRead)) continue;
            Brk *b = &w->br[w->nbr++]; b->addr = ol[i];
            b->uid = O.pob_uid >= 0 ? (uint32_t)g32(hb, O.pob_uid) : 0;
            b->life = O.pob_life >= 0 ? fx(ri(ol[i] + O.pob_life)) : 0;
            b->pos.x = fx(g32(sb, O.spr_pos)); b->pos.y = fx(g32(sb, O.spr_pos + 4)); b->pos.z = fx(g32(sb, O.spr_pos + 8));
            b->entered = -1; if (O.do_entered >= 0) { uint8_t v = 0; if (rd(ol[i] + O.do_entered, &v, 1)) b->entered = v != 0; }
            destroyable_data(ol[i], b);                                                     /* m7.2 */
            b->blowTimer = O.do_blow >= 0 ? ri(ol[i] + O.do_blow) : 0;
            continue;
        }
        if ((EE_HAZ && ee == (uintptr_t)EE_HAZ) || (EE_DMGA && ee == (uintptr_t)EE_DMGA)) {   /* m6.7: floor traps */
            if (w->nhz >= W_MAXHZ) continue;
            uintptr_t spr = rp(ol[i] + O.pob_sprite);
            if (!spr || !rd(spr, sb, O.sprRead)) continue;
            Haz *z = &w->hz[w->nhz++]; memset(z, 0, sizeof *z);
            z->uid = O.pob_uid >= 0 ? (uint32_t)g32(hb, O.pob_uid) : 0; z->kind = ee == (uintptr_t)EE_DMGA;
            z->pos.x = fx(g32(sb, O.spr_pos)); z->pos.y = fx(g32(sb, O.spr_pos + 4)); z->pos.z = fx(g32(sb, O.spr_pos + 8));
            z->anim = z->timer = z->cooldown = -1;
            if (!z->kind) {
                if (OFF_hz_anim >= 0) z->anim = ri(ol[i] + OFF_hz_anim);
                if (OFF_hz_timer >= 0) z->timer = ri(ol[i] + OFF_hz_timer);
                if (OFF_hz_cool >= 0) z->cooldown = ri(ol[i] + OFF_hz_cool);
                uintptr_t hd = OFF_hz_data >= 0 ? rp(ol[i] + OFF_hz_data + 8) : 0;      /* AssetRef_s: strongRef +8 */
                uintptr_t pa = hd && OFF_hd_zone >= 0 ? rp(hd + OFF_hd_zone) : 0;      /* Zone_s.pointArray (FixVec2[]) */
                int np = pa ? ri(pa + 8) : 0;
                if (np >= 3 && np <= 32) {
                    int32_t pt[64]; float a0 = 1e30f, a1 = -1e30f, b0 = 1e30f, b1 = -1e30f;
                    if (rd(pa + 16, pt, (size_t)np * 8)) {
                        for (int k = 0; k < np; k++) { float px = fx(pt[2 * k]), pz = fx(pt[2 * k + 1]);
                            if (px < a0) a0 = px; if (px > a1) a1 = px; if (pz < b0) b0 = pz; if (pz > b1) b1 = pz; }
                        if (a1 - a0 > 0.05f && a1 - a0 < 30.0f && b1 - b0 > 0.05f && b1 - b0 < 15.0f && a0 > -30.0f && a0 < 30.0f && b0 > -15.0f && b0 < 15.0f) {
                            z->haveZone = 1; z->x0 = z->pos.x + a0; z->x1 = z->pos.x + a1; z->z0 = z->pos.z + b0; z->z1 = z->pos.z + b1;
                        }
                    }
                }
                /* anim changes, so a run log shows each trap's on/off cycle (capped) */
                static struct { uint32_t uid; int anim; } hzPrev[W_MAXHZ * 2]; static int nHzPrev, hzLogs; int k;
                for (k = 0; k < nHzPrev; k++) if (hzPrev[k].uid == z->uid) break;
                if (k == nHzPrev) { if (nHzPrev < W_MAXHZ * 2) { hzPrev[nHzPrev].uid = z->uid; hzPrev[nHzPrev++].anim = z->anim; } else { hzPrev[k = (int)(z->uid % (W_MAXHZ * 2))].uid = z->uid; hzPrev[k].anim = z->anim; } }
                else if (hzPrev[k].anim != z->anim) {
                    if (hzLogs++ < 600) LOGI("hazard uid %u at (%.1f, %.1f): anim %d -> %d (timer %d, cooldown %d)", z->uid, z->pos.x, z->pos.z, hzPrev[k].anim, z->anim, z->timer, z->cooldown);
                    hzPrev[k].anim = z->anim;
                }
            } else if (OFF_da_radius >= 0) {
                float rx = fx(ri(ol[i] + OFF_da_radius)), rz = fx(ri(ol[i] + OFF_da_radius + 4));
                if (rx > 0.05f && rx < 20.0f && rz > 0.0f && rz < 10.0f) { z->haveZone = 1; z->x0 = z->pos.x - rx; z->x1 = z->pos.x + rx; z->z0 = z->pos.z - rz; z->z1 = z->pos.z + rz; }
            }
            continue;
        }
        if (EE_RPART && ee == (uintptr_t)EE_RPART) {           /* m7.4: robot boss part (legs, body...) */
            if (w->nrp >= W_MAXRP) continue;
            uintptr_t spr = rp(ol[i] + O.pob_sprite);
            if (!spr || !rd(spr, sb, O.sprRead)) continue;
            RPart *q = &w->rp[w->nrp++]; memset(q, 0, sizeof *q);
            q->uid = O.pob_uid >= 0 ? (uint32_t)g32(hb, O.pob_uid) : 0;
            q->team = O.pob_team >= 0 && O.pob_team + 4 <= 48 ? g32(hb, O.pob_team) : -1;
            q->plane = O.pob_plane >= 0 ? ri(ol[i] + O.pob_plane) : -1;
            q->life = O.pob_life >= 0 ? fx(ri(ol[i] + O.pob_life)) : -1;
            q->hitFreeze = OFF_pob_freeze >= 0 ? fx(ri(ol[i] + OFF_pob_freeze)) : -1;
            q->partId = OFF_rp_partId >= 0 ? ri(ol[i] + OFF_rp_partId) : -1; q->robotId = OFF_rp_robotId >= 0 ? ri(ol[i] + OFF_rp_robotId) : -1;
            q->pos.x = fx(g32(sb, O.spr_pos)); q->pos.y = fx(g32(sb, O.spr_pos + 4)); q->pos.z = fx(g32(sb, O.spr_pos + 8));
            q->dataLife = -1;
            for (int k = 0; k < g_nrpPrev; k++) if (g_rpPrev[k].uid == q->uid && q->uid) { q->vel.x = q->pos.x - g_rpPrev[k].pos.x; q->vel.y = q->pos.y - g_rpPrev[k].pos.y; q->vel.z = q->pos.z - g_rpPrev[k].pos.z; break; }
            continue;
        }
        if (EE_PROJ && ee == (uintptr_t)EE_PROJ) kind = 0; else if (EE_THROWN && ee == (uintptr_t)EE_THROWN) kind = 1;
        else {                                                  /* m6.7: name every other object type once (trap hunting) */
            static uintptr_t seenEE[32]; static int nSeen; int k;
            for (k = 0; k < nSeen; k++) if (seenEE[k] == ee) break;
            if (k == nSeen && nSeen < 32) { char nm[48]; seenEE[nSeen++] = ee; short_type_name(ee, nm, sizeof nm); LOGI("physical object type seen: %s (uid %u)", nm[0] ? nm : "?", O.pob_uid >= 0 ? (uint32_t)g32(hb, O.pob_uid) : 0); }
            continue;
        }
        if (w->npj >= W_MAXPJ) continue;
        uintptr_t spr = rp(ol[i] + O.pob_sprite);
        if (!spr || !rd(spr, sb, O.sprRead)) continue;
        Proj *p = &w->pj[w->npj++];
        p->uid = O.pob_uid >= 0 ? (uint32_t)g32(hb, O.pob_uid) : 0; p->team = O.pob_team >= 0 && O.pob_team + 4 <= 48 ? g32(hb, O.pob_team) : 0; p->kind = kind;
        p->pos.x = fx(g32(sb, O.spr_pos)); p->pos.y = fx(g32(sb, O.spr_pos + 4)); p->pos.z = fx(g32(sb, O.spr_pos + 8));
        p->vel.x = p->vel.y = p->vel.z = 0;
        for (int k = 0; k < g_npjPrev; k++) if (g_pjPrev[k].uid == p->uid && p->uid) {
            p->vel.x = p->pos.x - g_pjPrev[k].pos.x; p->vel.y = p->pos.y - g_pjPrev[k].pos.y; p->vel.z = p->pos.z - g_pjPrev[k].pos.z; break;
        }
    }
    g_nrpPrev = w->nrp; for (int i = 0; i < w->nrp; i++) { g_rpPrev[i].uid = w->rp[i].uid; g_rpPrev[i].pos = w->rp[i].pos; }
    g_npjPrev = w->npj;
    for (int i = 0; i < w->npj; i++) { g_pjPrev[i].uid = w->pj[i].uid; g_pjPrev[i].pos = w->pj[i].pos; }
}
static int g_adv, g_same;
int state_read(World *w) {
    w->ok = 0; w->nch = w->npk = w->npj = w->nbr = w->nhz = w->nslot = w->nps = 0; w->nrp = w->nrb = 0; w->bossMode = 0; w->haveWall = 0; w->haveCam = 0; w->source = "off"; w->frozen = 0; w->frozenMs = 0;
    w->live = 0; w->paused = 0; w->pausedSlot = -1; w->pausedStatus = 0; w->noGameState = 0; w->noGsWhy = "";
    w->friendlyFire = -1;
    w->menuRead = 0; w->charSel = 0; w->ncs = 0; w->mainScreens = w->pauseScreens = -1; w->menuName[0] = w->topScreen[0] = 0;
    if (!state_available()) return 0;
    w->seq++;
    if (g_trState == 2) trace_log();
    uintptr_t gs = current_gs(&w->source);
    /* player slots (who is P1..P4 and which controller each uses) are useful even in menus */
    uintptr_t on = online_obj();
    if (on && g_onSlots > 0 && EE_SLOT) {
        uintptr_t arr = rp(on + g_onSlots); int n = arr ? ri(arr + 8) : 0;
        for (int i = 0; i < n && i < 8; i++) {
            uintptr_t s = rp(arr + 16 + 8 * i); SlotInfo *si = &w->slot[w->nslot++];
            if (!obj_is(s, EE_SLOT)) { si->ctrl = si->type = si->status = -99; continue; }
            si->ctrl = ri(s + O.sl_ctrl); si->type = ri(s + O.sl_type); si->status = ri(s + O.sl_status);
        }
    }
    read_menu(w, on);
    {   /* m7.2: friendly fire (Options / character select); keep the last value seen when globalState is not readable */
        static int lastFF = -1;
        if (w->friendlyFire >= 0) { if (w->friendlyFire != lastFF) LOGI("Friendly fire is %s (MetaGameGlobalState.friendlyFireEnabled).", w->friendlyFire ? "ON" : "OFF"); lastFF = w->friendlyFire; }
        else w->friendlyFire = lastFF;
    }
    if (w->charSel && !g_nroster) { const RosterEntry *r_; state_roster(&r_); }
    /* m2.4: an occupied slot with _gameplayStatus != 0 = the game is paused (pause menu, options...).
       In the m2.3 log status went 0 -> 1 for every player exactly when the frame counter froze, and back
       to 0 when it moved again. */
    for (int i = 0; i < w->nslot; i++) {
        const SlotInfo *si = &w->slot[i];
        if (si->ctrl >= 0 && si->type != 0 && si->status != 0) { w->paused = 1; w->pausedSlot = i; w->pausedStatus = si->status; break; }
    }
    if (!w->paused && w->pauseScreens > 0) { w->paused = 1; w->pausedSlot = -1; w->pausedStatus = 100 + w->pauseScreens; }   /* program.pauseMenu has a screen open */
    if (!gs) {
        g_adv = 0; g_same = 0;
        /* positive evidence that no GameState exists at all (character select has none) */
        int onAny = 0;
        if (on) for (int i = 0; i < 3; i++) if (g_onOff[i] > 0 && obj_is(rp(on + g_onOff[i]), EE_GS)) onAny = 1;
        ULONGLONG now = GetTickCount64(), done = g_lastSearchDone;
        if (T.locked && !strcmp(w->source, "game.state (empty)")) { w->noGameState = 1; w->noGsWhy = "the game's own game.state is empty"; }
        else if (onAny) w->noGsWhy = "an online.* copy still holds a GameState";
        else if (g_holder || g_direct) w->noGsWhy = "a GameState source is still set";
        else if (!done) w->noGsWhy = "no search has finished yet";
        else if (g_lastSearchN != 0) w->noGsWhy = "the last search found GameState objects";
        else if (now - done > 12000) w->noGsWhy = "the last search is too old";
        else { w->noGameState = 1; w->noGsWhy = "the last search found no GameState objects"; }
        return 0;
    }
    uint8_t g[512]; if (!rd(gs, g, O.gsRead)) return 0;
    w->frames = O.gs_frames >= 0 ? g32(g, O.gs_frames) : 0;
    {   /* liveness: is the frame counter moving? counted in our reads (one per frame, >= 12 ms apart).
           g_adv = frame advances since the source changed or last stalled (>= 6 reads unchanged). */
        static int lastFrames = -1; static const char *lastSrc; static uintptr_t lastGs;
        int newSrc = w->source != lastSrc || (gs != lastGs && strcmp(w->source, "game.state"));
        if (newSrc) { lastFrames = w->frames; lastSrc = w->source; lastGs = gs; g_same = 0; g_adv = 0; }
        else if (w->frames != lastFrames) { lastFrames = w->frames; lastGs = gs; g_same = 0; g_adv++; }
        else { g_same++; if (g_same >= 6) g_adv = 0; }
        w->frozenMs = g_same * 16;
        w->frozen = g_same >= 36;                                   /* ~0.6 s */
        /* a frozen fallback copy is dropped as stale, but not while the game is paused: in the m2.3 log
           the live GameState was dropped and re-found over and over during every pause, and each time
           the bot thought a stage had ended. */
        if (g_same >= 90 && strcmp(w->source, "game.state") && !w->paused) { drop_stale_source(w->source, w->frames); g_same = 0; g_adv = 0; }
    }
    w->mode = O.gs_mode >= 0 ? g32(g, O.gs_mode) : 0;
    w->gameOver = O.gs_over >= 0 ? g[O.gs_over] : 0;
    w->bossMode = O.gs_boss >= 0 ? g[O.gs_boss] : 0;
    if (O.gs_players >= 0) {
        uintptr_t arr = g64(g, O.gs_players), aee = arr ? rp(arr) : 0;
        uint16_t comp = 0;
        if (aee && rd(aee, &comp, 2) && comp >= 16 && comp <= 512) {
            int n = ri(arr + 8); uint8_t e[512];
            for (int i = 0; i < n && i < 8; i++) {
                if (!rd(arr + 16 + (uintptr_t)i * comp, e, comp)) break;
                PlayerStateInfo *p = &w->ps[w->nps++];
                p->playable = O.ps_playable >= 0 ? g32(e, O.ps_playable) : 0; p->continues = O.ps_continues >= 0 ? g32(e, O.ps_continues) : 0;
                p->life = O.ps_life >= 0 ? fx(g32(e, O.ps_life)) : 0; p->fury = O.ps_fury >= 0 ? fx(g32(e, O.ps_fury)) : 0;
                p->score = O.ps_score >= 0 ? g32(e, O.ps_score) : 0; p->allyId = O.ps_ally >= 0 ? g32(e, O.ps_ally) : 0;
                p->stars = O.ps_star >= 0 && O.ps_star + 4 <= comp ? g32(e, O.ps_star) : -1;
                p->banked = O.ps_banked >= 0 && O.ps_banked + 4 <= comp ? fx(g32(e, O.ps_banked)) : -1;
                p->weaponLife = O.ps_wlife >= 0 && O.ps_wlife + 4 <= comp ? fx(g32(e, O.ps_wlife)) : -1;
                p->starBtn = O.ps_starBtn >= 0 && O.ps_starBtn < comp ? e[O.ps_starBtn] : -1;
                p->starYB = O.ps_starYB >= 0 && O.ps_starYB < comp ? e[O.ps_starYB] : -1;
                for (int k = 0; k < 5; k++) p->alt[k] = O.ps_alt[k] >= 0 && O.ps_alt[k] < comp ? (e[O.ps_alt[k]] ? 1 : 0) : -1;   /* v1.1 stage 4 */
            }
        }
    }
    uintptr_t levels[64]; int nl = read_list(g64(g, O.gs_levels), levels, 64);
    w->nLevels = nl;
    if (nl <= 0) return 0;
    uintptr_t ls = levels[nl - 1];
    uint8_t l[512];
    if (!obj_is(ls, EE_LS) || !rd(ls, l, O.lsRead)) return 0;
    w->scene = O.ls_scene >= 0 ? g32(l, O.ls_scene) : 0;
    if (w->scene != g_cacheScene) { g_cacheScene = w->scene; g_ncd = g_npd = 0; }
    w->haveWall = O.ls_wall >= 0; w->leftWallX = O.ls_wall >= 0 ? fx(g32(l, O.ls_wall)) : 0;

    uintptr_t cl[W_MAXCH]; int nc = read_list(g64(l, O.ls_chars), cl, W_MAXCH);
    uint8_t cb[1024], sb[512];
    for (int i = 0; i < nc; i++) {
        if (!rd(cl[i], cb, O.chrRead) || g64(cb, 0) != (uintptr_t)EE_CHR) continue;
        uintptr_t spr = g64(cb, O.pob_sprite);
        if (!spr || !rd(spr, sb, O.sprRead)) continue;
        Chr *c = &w->ch[w->nch++]; memset(c, 0, sizeof *c);
        c->addr = cl[i];
        c->uid = O.pob_uid >= 0 ? (uint32_t)g32(cb, O.pob_uid) : (uint32_t)i;
        c->team = g32(cb, O.pob_team); c->player = g32(cb, O.pob_player);
        c->life = fx(g32(cb, O.pob_life));
        c->maxLife = O.ch_maxlife >= 0 ? fx(g32(cb, O.ch_maxlife)) : 0;
        c->pos.x = fx(g32(sb, O.spr_pos)); c->pos.y = fx(g32(sb, O.spr_pos + 4)); c->pos.z = fx(g32(sb, O.spr_pos + 8));
        c->facing = sb[O.spr_flip] ? -1 : 1;
        c->state = O.ch_state >= 0 ? classify_state(rp(g64(cb, O.ch_state))) : CS_NONE;
        c->hitStun = O.ch_stun >= 0 ? g32(cb, O.ch_stun) : 0; c->invincible = O.ch_inv >= 0 ? g32(cb, O.ch_inv) : 0;
        c->scripted = O.ch_scripted >= 0 ? cb[O.ch_scripted] : 0; c->moveId = O.ch_move >= 0 ? g32(cb, O.ch_move) : 0;
        c->ctrlKind = CTRL_NONE; c->stickX = c->stickY = 0;
        c->lineOfSight = c->btRoot = -1;
        c->plane = O.pob_plane >= 0 ? g32(cb, O.pob_plane) : -1;
        c->entered = O.ch_entered >= 0 ? cb[O.ch_entered] != 0 : -1;
        c->collidable = O.ch_coll >= 0 ? (cb[O.ch_coll] != 0 && !(O.ch_neverColl >= 0 && cb[O.ch_neverColl])) : -1;
        uintptr_t ctl = O.ch_ctrl >= 0 ? g64(cb, O.ch_ctrl) : 0;
        uint8_t kb[512];
        if (ctl && rd(ctl, kb, O.ctlRead)) {
            uintptr_t cee = g64(kb, 0);
            c->ctrlKind = cee == (uintptr_t)EE_PCTRL ? CTRL_PLAYER : cee == (uintptr_t)EE_ACTRL ? CTRL_AI : CTRL_OTHER;
            if (O.cc_analog >= 0) { c->stickX = fx(g32(kb, O.cc_analog)); c->stickY = fx(g32(kb, O.cc_analog + 4)); }
            c->guard = O.cc_guard >= 0 ? kb[O.cc_guard] != 0 : 0;
            if (c->ctrlKind == CTRL_AI) {
                c->lineOfSight = O.ai_los >= 0 ? kb[O.ai_los] != 0 : -1;
                c->btRoot = O.ai_root >= 0 ? g32(kb, O.ai_root) : -1;
                c->aiAttacking = O.ai_attacking >= 0 ? kb[O.ai_attacking] != 0 : 0;
                c->aiTarget = O.ai_target >= 0 ? (uint32_t)g32(kb, O.ai_target) : 0;     /* PhysicalObjectRef_s: uid is its only field */
            }
        }
        /* m4 */
        if (O.ch_grabList >= 0) {
            uintptr_t gl = g64(cb, O.ch_grabList), h[3];
            if (gl && rd(gl, h, 24) && h[1]) { int n = (int)(int32_t)h[2]; if (n > 0 && n < 16) { c->grabbing = n; c->grabUid = (uint32_t)ri(h[1] + 16); } }
        }
        if (c->state == CS_STANDHIT && O.sh_grabSrc >= 0) c->grabbedBy = (uint32_t)ri(g64(cb, O.ch_state) + O.sh_grabSrc);
        c->heldAddr = O.ch_held >= 0 ? g64(cb, O.ch_held) : 0;
        c->heldLife = c->heldMaxLife = -1; c->heldOnlyThrow = 0; c->heldWType = -1; c->heldKnife = 0; c->heldName[0] = 0;
        if (c->heldAddr && EE_PICK && obj_is(c->heldAddr, EE_PICK)) {          /* m7.2: the weapon in hand */
            Pick hp; memset(&hp, 0, sizeof hp); hp.maxLife = -1; hp.wtype = -1;
            if (O.pk_life >= 0) c->heldLife = fx(ri(c->heldAddr + O.pk_life));
            if (O.pk_data >= 0) pickup_data(rp(c->heldAddr + O.pk_data + 8), &hp);
            c->heldMaxLife = hp.maxLife; c->heldOnlyThrow = hp.onlyThrow; c->heldWType = hp.wtype; c->heldKnife = hp.knife;
            snprintf(c->heldName, sizeof c->heldName, "%s", hp.wname);
        }
        c->superArmor = O.ch_armor >= 0 ? g32(cb, O.ch_armor) > 0 : 0;
        c->untargettable = O.ch_untarget >= 0 ? g32(cb, O.ch_untarget) > 0 : 0;
        c->cantBeGrabbed = O.ch_noGrabbed >= 0 ? g32(cb, O.ch_noGrabbed) > 0 : 0;
        c->noQuickRecover = O.ch_noQR >= 0 ? cb[O.ch_noQR] : 0;
        c->airRecoverTimer = O.ch_airRec >= 0 ? g32(cb, O.ch_airRec) : 0;
        c->airTime = O.ch_airTime >= 0 ? g32(cb, O.ch_airTime) : 0;
        if (O.ch_data >= 0 && c->player < 0) c->boss = char_data_boss(g64(cb, O.ch_data + 8), &c->canGuard);   /* AssetRef_s: strongRef +8 */
        c->stamina = OFF_ch_stamina >= 0 ? fx(g32(cb, OFF_ch_stamina)) : -1; c->maxStamina = -1;
        c->staminaBroken = OFF_ch_stBroken >= 0 ? g32(cb, OFF_ch_stBroken) > 0 : 0;
        c->guardHits = OFF_ch_guardHits >= 0 ? g32(cb, OFF_ch_guardHits) : 0;
        c->lastHitGuarded = OFF_ch_lastGuard >= 0 ? cb[OFF_ch_lastGuard] != 0 : 0;
        c->hitSrc = OFF_ch_hitSrc >= 0 ? (uint32_t)ri(cl[i] + OFF_ch_hitSrc) : 0;          /* m7.4 */
        c->robotDriver = 0; c->robotPart = 0;
        if (O.ch_data >= 0 && c->player < 0) char_data_shield(g64(cb, O.ch_data + 8), c);
        if (c->state == CS_STANDGUARD || c->state == CS_STANDGUARDHIT || c->state == CS_CROUCHGUARD || c->state == CS_CROUCHGUARDHIT) c->guard = 1;
    }
    if (O.ls_picks >= 0 && EE_PICK && O.pk_pos >= 0) {
        uintptr_t pl[W_MAXPK]; int np = read_list(g64(l, O.ls_picks), pl, W_MAXPK); uint8_t pb[512];
        for (int i = 0; i < np; i++) {
            if (!rd(pl[i], pb, O.pkRead) || g64(pb, 0) != (uintptr_t)EE_PICK) continue;
            Pick *p = &w->pk[w->npk++]; p->addr = pl[i];
            p->pos.x = fx(g32(pb, O.pk_pos)); p->pos.y = fx(g32(pb, O.pk_pos + 4)); p->pos.z = fx(g32(pb, O.pk_pos + 8));
            p->life = O.pk_life >= 0 ? fx(g32(pb, O.pk_life)) : 0;
            p->team = O.pk_team >= 0 ? g32(pb, O.pk_team) : 0; p->player = O.pk_player >= 0 ? g32(pb, O.pk_player) : 0;
            p->kind = PK_OTHER; p->lifeGain = 0; p->starGain = p->ticking = p->onlyThrow = p->held = 0; p->maxLife = -1; p->wtype = -1; p->knife = 0; p->wname[0] = 0;
            if (O.pk_data >= 0) pickup_data(g64(pb, O.pk_data + 8), p);              /* AssetRef_s: strongRef +8 */
            if (O.pk_expl >= 0 && g32(pb, O.pk_expl) > 0) p->ticking = 1;
            for (int k = 0; k < w->nch; k++) if (w->ch[k].heldAddr == p->addr) p->held = 1;
        }
    }
    if (O.ls_objs >= 0 && (EE_PROJ || EE_THROWN || EE_DESTR || EE_HAZ || EE_DMGA || EE_RPART)) read_projectiles(w, g64(l, O.ls_objs));
    if (OFF_ls_robots >= 0 && EE_ROBOT) read_robots(w, rp(ls + OFF_ls_robots));                                /* m7.4 */
    if (O.ls_cams >= 0 && EE_CAM && O.cam_pos >= 0) {
        uintptr_t cams[4]; int ncam = read_list(g64(l, O.ls_cams), cams, 4);
        int32_t v[3];
        if (ncam > 0 && obj_is(cams[0], EE_CAM) && rd(cams[0] + O.cam_pos, v, 12)) {
            w->haveCam = 1; w->cam.x = fx(v[0]); w->cam.y = fx(v[1]); w->cam.z = fx(v[2]);
            w->camSizeY = O.cam_sizeY >= 0 ? fx(ri(cams[0] + O.cam_sizeY)) : 0;
        }
    }
    w->ok = 1;
    w->live = g_adv >= 10 && g_same < 6;
    return 1;
}

void state_log_slots(const World *w, const char *why) {
    char b[400]; int k = 0;
    for (int i = 0; i < w->nslot; i++) k += snprintf(b + k, sizeof b - k, " P%d{controller %d, type %d, status %d}", i + 1, w->slot[i].ctrl, w->slot[i].type, w->slot[i].status);
    LOGI("player slots (%s):%s", why, w->nslot ? b : " not readable yet");
    k = 0;
    for (int i = 0; i < w->nps; i++) k += snprintf(b + k, sizeof b - k, " P%d{fighter %d, life %.1f, continues %d, score %d}", i + 1, w->ps[i].playable, w->ps[i].life, w->ps[i].continues, w->ps[i].score);
    if (w->nps) LOGI("player states:%s", b);
}
void state_dump(const World *w, const char *tag) {
    LOGI("DUMP %s: src=%s frame=%d mode=%d levels=%d scene=%d chars=%d pickups=%d cam=(%.1f,%.1f,%.1f) camSizeY=%.2f",
         tag, w->source, w->frames, w->mode, w->nLevels, w->scene, w->nch, w->npk, w->cam.x, w->cam.y, w->cam.z, w->camSizeY);
    for (int i = 0; i < w->nch; i++) {
        const Chr *c = &w->ch[i];
        LOGI("  chr uid=%u team=%d player=%d life=%.1f max=%.1f pos=(%.1f,%.1f,%.1f) face=%+d state=%s ctrl=%s stick=(%.2f,%.2f) stun=%d inv=%d script=%d move=%d",
             c->uid, c->team, c->player, c->life, c->maxLife, c->pos.x, c->pos.y, c->pos.z, c->facing, state_name(c->state),
             c->ctrlKind == CTRL_PLAYER ? "player" : c->ctrlKind == CTRL_AI ? "ai" : c->ctrlKind == CTRL_OTHER ? "other" : "none",
             c->stickX, c->stickY, c->hitStun, c->invincible, c->scripted, c->moveId);
        if (c->grabbing || c->grabbedBy || c->heldAddr || c->superArmor || c->boss || c->aiAttacking || c->airTime || c->noQuickRecover)
            LOGI("    m4: grabbing=%d(uid %u) grabbedBy=%u weapon=%d armor=%d untarget=%d noGrab=%d airRec=%d airTime=%d noQR=%d boss=%d aiAttack=%d aiTarget=%u",
                 c->grabbing, c->grabUid, c->grabbedBy, c->heldAddr != 0, c->superArmor, c->untargettable, c->cantBeGrabbed,
                 c->airRecoverTimer, c->airTime, c->noQuickRecover, c->boss, c->aiAttacking, c->aiTarget);
        if (c->heldAddr) LOGI("    m7.2: uid %u holds weapon \"%s\" life=%.1f/%.1f type=%d onlyThrow=%d knife=%d", c->uid, c->heldName[0] ? c->heldName : "?", c->heldLife, c->heldMaxLife, c->heldWType, c->heldOnlyThrow, c->heldKnife);
        LOGI("    m5: plane=%d entered=%d collidable=%d guard=%d canGuard=%d btRoot=%d lineOfSight=%d", c->plane, c->entered, c->collidable, c->guard, c->canGuard, c->btRoot, c->lineOfSight);
        if (c->hitSrc || c->robotDriver) LOGI("    m7.4: hitSrc=%u driver=%d", c->hitSrc, c->robotDriver);
        if (c->player < 0) LOGI("    m6.7: name=%s stamina=%.1f/%.1f broken=%d guardHits=%d lastGuarded=%d shieldUser=%d shieldMove=%d dataMaxLife=%.0f", c->dname[0] ? c->dname : "?",
                                c->stamina, c->maxStamina, c->staminaBroken, c->guardHits, c->lastHitGuarded, c->shieldUser, c->shieldMove, c->dataMaxLife);
    }
    for (int i = 0; i < w->nbr; i++)
        LOGI("  breakable uid=%u pos=(%.1f,%.1f,%.1f) life=%.1f entered=%d explosive=%d blowTimer=%d name=%s", w->br[i].uid, w->br[i].pos.x, w->br[i].pos.y, w->br[i].pos.z, w->br[i].life, w->br[i].entered,
             w->br[i].explosive, w->br[i].blowTimer, w->br[i].name[0] ? w->br[i].name : "?");
    for (int i = 0; i < w->nhz; i++)
        LOGI("  hazard uid=%u kind=%s pos=(%.1f,%.1f,%.1f) anim=%d timer=%d cooldown=%d zone=%d (x %.1f..%.1f, z %.1f..%.1f)", w->hz[i].uid, w->hz[i].kind ? "damageArea" : "trap",
             w->hz[i].pos.x, w->hz[i].pos.y, w->hz[i].pos.z, w->hz[i].anim, w->hz[i].timer, w->hz[i].cooldown, w->hz[i].haveZone, w->hz[i].x0, w->hz[i].x1, w->hz[i].z0, w->hz[i].z1);
    for (int i = 0; i < w->nrb; i++) {
        const Robot *r = &w->rb[i];
        LOGI("  robot id=%d active=%d broken=%d firstDriver=%d anim=%d pos=(%.1f,%.1f,%.1f) drivers=%u,%u (%d) parts=%d healLife=%.1f hitDamageFactor=%.2f", r->uid, r->active, r->broken, r->firstDriver,
             r->animId, r->pos.x, r->pos.y, r->pos.z, r->drivers[0], r->nDrivers > 1 ? r->drivers[1] : 0, r->nDrivers, r->nParts, r->healLife, r->dmgFactor);
    }
    for (int i = 0; i < w->nrp; i++) {
        const RPart *q = &w->rp[i];
        LOGI("  robot part uid=%u robot=%d part=%d pos=(%.2f,%.2f,%.2f) vel=(%.3f,%.3f,%.3f) life=%.1f dataLife=%.0f team=%d hitFreeze=%.1f plane=%d name=%s", q->uid, q->robotId, q->partId,
             q->pos.x, q->pos.y, q->pos.z, q->vel.x, q->vel.y, q->vel.z, q->life, q->dataLife, q->team, q->hitFreeze, q->plane, q->name[0] ? q->name : "?");
    }
    static const char *const kk[] = { "other", "food", "weapon", "star", "grenade", "money" };
    for (int i = 0; i < w->npk; i++)
        LOGI("  pickup pos=(%.1f,%.1f,%.1f) life=%.1f team=%d player=%d kind=%s lifeGain=%.1f star=%d ticking=%d onlyThrow=%d held=%d score=%d", w->pk[i].pos.x, w->pk[i].pos.y, w->pk[i].pos.z,
             w->pk[i].life, w->pk[i].team, w->pk[i].player, kk[w->pk[i].kind % 6], w->pk[i].lifeGain, w->pk[i].starGain, w->pk[i].ticking, w->pk[i].onlyThrow, w->pk[i].held, w->pk[i].scoreGain);
    for (int i = 0; i < w->npj; i++)
        LOGI("  %s uid=%u team=%d pos=(%.1f,%.1f,%.1f) vel=(%.2f,%.2f,%.2f)", w->pj[i].kind ? "thrown" : "projectile", w->pj[i].uid, w->pj[i].team,
             w->pj[i].pos.x, w->pj[i].pos.y, w->pj[i].pos.z, w->pj[i].vel.x, w->pj[i].vel.y, w->pj[i].vel.z);
    for (int i = 0; i < w->nps; i++)
        LOGI("  player %d: playable=%d life=%.1f banked=%.1f stars=%d starButton=%d starYB=%d weaponInHandLife=%.1f alt=%d%d%d%d%d", i + 1, w->ps[i].playable, w->ps[i].life, w->ps[i].banked, w->ps[i].stars, w->ps[i].starBtn, w->ps[i].starYB, w->ps[i].weaponLife,
             w->ps[i].alt[0] < 0 ? 9 : w->ps[i].alt[0], w->ps[i].alt[1] < 0 ? 9 : w->ps[i].alt[1], w->ps[i].alt[2] < 0 ? 9 : w->ps[i].alt[2], w->ps[i].alt[3] < 0 ? 9 : w->ps[i].alt[3], w->ps[i].alt[4] < 0 ? 9 : w->ps[i].alt[4]);
    if (w->bossMode || w->haveWall) LOGI("  bossMode=%d leftWallX=%.1f", w->bossMode, w->leftWallX);
}
/* v1.1 stage 3: per-frame test events (called by the host every frame while DumpState=1) */
static struct { uint32_t uid; float maxSeen; int move, state; } g_evH[32]; static int g_nevH;
static uint32_t g_evPj[48]; static int g_nevPj;
static struct { uint32_t uid; float life; } g_evP[8]; static int g_nevP;
void state_events(const World *w) {
    if (!w || !w->ok) return;
    for (int i = 0; i < w->nch; i++) {
        const Chr *c = &w->ch[i];
        if (c->player >= 0 || c->team == 0 || c->life <= 0) continue;
        int k = 0; for (; k < g_nevH; k++) if (g_evH[k].uid == c->uid) break;
        if (k == g_nevH) {
            if (!(c->boss || c->dataMaxLife >= 300 || c->life >= 300)) continue;
            k = g_nevH < 32 ? g_nevH++ : (int)(c->uid % 32);
            g_evH[k].uid = c->uid; g_evH[k].maxSeen = 0; g_evH[k].move = -2; g_evH[k].state = -2;
            LOGI("EVENT heavy uid=%u name=%s life=%.0f dataMaxLife=%.0f boss=%d: watching its moves", c->uid, c->dname[0] ? c->dname : "?", c->life, c->dataMaxLife, c->boss);
        }
        if (c->life > g_evH[k].maxSeen) g_evH[k].maxSeen = c->life;
        if (c->moveId != g_evH[k].move || (c->state != g_evH[k].state && (c->state == CS_AIRMOVE || c->state == CS_JUMP || c->state == CS_DASH))) {
            const Chr *np = NULL; float nd = 1e30f;
            for (int j = 0; j < w->nch; j++) { const Chr *p = &w->ch[j];
                if (p->player < 0 || p->life <= 0) continue;
                float ax = p->pos.x - c->pos.x, az = p->pos.z - c->pos.z; float d = (ax < 0 ? -ax : ax) + 2.0f * (az < 0 ? -az : az); if (d < nd) { nd = d; np = p; } }
            if (c->moveId >= 0 || g_evH[k].move >= 0)
                LOGI("EVENT heavy uid=%u move %d->%d state=%s pos=(%.2f,%.2f,%.2f) face=%+d aiAttack=%d aiTarget=%u weapon=%d armor=%d inv=%d life=%.0f | nearest player uid=%u dx=%.2f dz=%.2f %s",
                     c->uid, g_evH[k].move, c->moveId, state_name(c->state), c->pos.x, c->pos.y, c->pos.z, c->facing, c->aiAttacking, c->aiTarget, c->heldAddr != 0, c->superArmor, c->invincible, c->life,
                     np ? np->uid : 0u, np ? np->pos.x - c->pos.x : 0.0f, np ? np->pos.z - c->pos.z : 0.0f, np ? state_name(np->state) : "-");
            g_evH[k].move = c->moveId; g_evH[k].state = c->state;
        }
    }
    for (int i = 0; i < w->npj; i++) {
        const Proj *p = &w->pj[i];
        if (p->team == 0 || !p->uid) continue;
        int k = 0; for (; k < g_nevPj; k++) if (g_evPj[k] == p->uid) break;
        if (k < g_nevPj) continue;
        if (g_nevPj < 48) g_evPj[g_nevPj++] = p->uid; else { memmove(g_evPj, g_evPj + 1, 47 * sizeof *g_evPj); g_evPj[47] = p->uid; }
        LOGI("EVENT %s uid=%u team=%d appears at (%.2f,%.2f,%.2f) vel=(%.3f,%.3f,%.3f)", p->kind ? "thrown" : "projectile", p->uid, p->team, p->pos.x, p->pos.y, p->pos.z, p->vel.x, p->vel.y, p->vel.z);
    }
    for (int i = 0; i < w->nch; i++) {
        const Chr *c = &w->ch[i];
        if (c->player < 0 || c->player >= 8) continue;
        int k = 0; for (; k < g_nevP; k++) if (g_evP[k].uid == c->uid) break;
        if (k == g_nevP) { k = g_nevP < 8 ? g_nevP++ : c->player; g_evP[k].uid = c->uid; g_evP[k].life = c->life; continue; }
        if (c->life < g_evP[k].life - 0.05f) {
            const Chr *s = NULL; for (int j = 0; j < w->nch; j++) if (c->hitSrc && w->ch[j].uid == c->hitSrc) s = &w->ch[j];
            const Proj *sp = NULL; for (int j = 0; j < w->npj; j++) if (c->hitSrc && w->pj[j].uid == c->hitSrc) sp = &w->pj[j];
            char src[160] = "";
            if (s) snprintf(src, sizeof src, "uid=%u %s move=%d state=%s pos=(%.2f,%.2f,%.2f) face=%+d aiAttack=%d dx=%.2f dz=%.2f", s->uid, s->dname[0] ? s->dname : "?", s->moveId, state_name(s->state),
                             s->pos.x, s->pos.y, s->pos.z, s->facing, s->aiAttacking, c->pos.x - s->pos.x, c->pos.z - s->pos.z);
            else if (sp) snprintf(src, sizeof src, "%s uid=%u pos=(%.2f,%.2f,%.2f) vel=(%.3f,%.3f,%.3f)", sp->kind ? "thrown" : "projectile", sp->uid, sp->pos.x, sp->pos.y, sp->pos.z, sp->vel.x, sp->vel.y, sp->vel.z);
            else {
                const Haz *hz = NULL; for (int j = 0; j < w->nhz; j++) if (c->hitSrc && w->hz[j].uid == c->hitSrc) hz = &w->hz[j];
                if (hz) snprintf(src, sizeof src, "hazard uid=%u (%s) pos=(%.2f,%.2f,%.2f) anim=%d dx=%.2f dz=%.2f", hz->uid, hz->kind ? "damage area" : "floor trap", hz->pos.x, hz->pos.y, hz->pos.z, hz->anim, c->pos.x - hz->pos.x, c->pos.z - hz->pos.z);
                else snprintf(src, sizeof src, "uid=%u (not a fighter or object on the list)", c->hitSrc);
            }
            /* v1.1 stage 4 (log only): life lost in our own special move is its health cost, whatever the (sticky) last hit source says */
            if ((c->state == CS_STANDMOVE || c->state == CS_AIRMOVE) && c->moveId >= 0 && !(s && s->player < 0 && (c->pos.x - s->pos.x < 6.0f && s->pos.x - c->pos.x < 6.0f))) {
                char tmp[160]; memcpy(tmp, src, sizeof tmp);
                int axel = c->dname[0] && (strstr(c->dname, "AXEL") || strstr(c->dname, "Axel") || strstr(c->dname, "axel"));
                const char *mv = !axel ? "" : c->moveId == 4 ? ", Dragon Dive" : c->moveId == 8 ? ", Dragon Smash" : c->moveId == 10 ? ", Dragon Wing" : "";
                snprintf(src, sizeof src, "own special cost (move %d%s; last hit source: %.100s)", c->moveId, mv, tmp);
            }
            LOGI("EVENT player %d uid=%u life %.1f->%.1f state=%s pos=(%.2f,%.2f,%.2f) move=%d | hit by %s", c->player + 1, c->uid, g_evP[k].life, c->life, state_name(c->state), c->pos.x, c->pos.y, c->pos.z, c->moveId, src);
        }
        g_evP[k].life = c->life;
    }
}
void state_shutdown(void) {           /* SDL_Quit, main thread, never under the loader lock */
    InterlockedExchange(&g_stopping, 1);
    stop_scanner(150, "game is quitting");
}
