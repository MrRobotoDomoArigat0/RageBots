/* Shared declarations for the SoR4Bots proxy modules. */
#pragma once
#include <stdint.h>
enum { LV_ERROR = 0, LV_INFO = 1, LV_DEBUG = 2 };
void logf_(int lv, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define LOGE(...) logf_(LV_ERROR, __VA_ARGS__)
#define LOGI(...) logf_(LV_INFO, __VA_ARGS__)
#define LOGD(...) logf_(LV_DEBUG, __VA_ARGS__)

enum { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_BACK, BTN_GUIDE, BTN_START, BTN_LSTICK, BTN_RSTICK,
       BTN_LB, BTN_RB, BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_COUNT };
enum { AX_LX, AX_LY, AX_RX, AX_RY, AX_LT, AX_RT, AX_COUNT };

typedef struct {
    /* [General] */
    int logLevel, inputFallbackSeconds, diagnostics;
    /* [Bots] */
    int botOn[3], botColor[3]; char botChar[3][32];      /* m3: P2..P4 bot choices from the F7 menu (saved to the ini) */
    int botNormalized;                                   /* m7: bits of bots forced OFF at load (P3 on without P2, ...) */
    int overlay;                                          /* [Debug] Overlay=0 turns the drawn menu off (tests) */
    int startActive, attachDelayMs, attachOnF7, reorderLatePads, settleMs;
    /* [Input] */
    int legacyDrivers, hideDuplicatePads;
    /* [Brain] */
    int fallbackTest, retreatMaxMs, menuConfirmMs, menuConfirmMaxTaps, focusGraceMs, resumeGraceMs;
    int smart, attackButton, jumpButton, specialButton, useSpecials, specialMinLifePct, retreatLifePct;
    float attackRange, laneTolerance, followDistance;     /* 0 = automatic */
    int comboHits, comboTapMs, comboPauseMs;
    /* m4 */
    int botAggr[3];                                       /* [Bots] PnAggression: 0 Low, 1 Normal, 2 High */
    int defSpecialMinLifePct, starButton, starMode;       /* starMode 0 = auto (game's control option), 1 = StarButton, 2 = Y+B */
    int backAttack;                                       /* 0 off, 1 RT, 2 back+X */
    int useStars, usePickups, useGrabs, foodLifePct;
    int itemHeadStartMs, breakObjects;   /* m5: PlayerItemHeadStartSec, BreakObjects */
    int jumpAttacks;                     /* m6: JumpAttacks (jump-ins, air hits, jump-cancel combos, air special) */
    int friendlyRetaliate;               /* m7.4: FriendlyRetaliate (friendly fire on: a bot hits a human back once per hit taken) */
    int weaponThrows;                    /* m7.2: WeaponThrows (throw held melee weapons at range / before they break) */
    /* [TestBrain] (also the fallback when the game state can't be read) */
    int joinButton, joinPresses, testAttackButton, attackIntervalMs, walkMs, jumpEvery;
    /* [Debug] */
    int dumpState, dumpEveryNFrames, brainLogMs, p1WatchMs;   /* m6: P1 watch line every N ms (0 = off) */
} Config;
extern Config C;
