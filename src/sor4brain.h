#pragma once
#include <stdint.h>
#include "sor4state.h"
#include "sor4common.h"

typedef struct {
    int16_t axis[AX_COUNT]; uint8_t btn[BTN_COUNT];
} PadOut;

/* who we are in the game: kept across F7 off/on, cleared only when the bot pad is unplugged */
typedef struct {
    int ctrlId;                 /* the game's controllerId for our pad (-1 = not known) */
    char how[64];               /* how we learned it */
    int priorFna, priorSdl;     /* guesses from device order (FNA first-free slot, SDL device index) */
    int preKnown, npre, pre[8]; /* controllerIds already in player slots before our pad existed = humans */
    int nlast, last[8];         /* slots last frame, to see changes */
    int rejected[8], nrej;      /* controllerIds proven not ours */
    int verifiedSlot;           /* stick cross-check passed for this slot (-1 = not yet) */
    int ckSlot, ckSamples, ckAgree, ckFollow, ckRev, ckLastSx; uint64_t ckRetryAt; int ckFails;
    int pvPending, pvSamples, pvFollow, pvAgree;   /* m6: silent check while playing (no wiggle) */
    uint64_t attachAt;
    int loggedPre, loggedNoSlot;
    uint64_t lastSeen;          /* when ident_update last looked at the slots */
} BotIdent;

/* m3/m4: per-character profile. Specials and moves per character plug in here. */
enum { GS_DEFAULT, GS_FLOYD, GS_MAX };                       /* what to do while holding an enemy */
typedef struct {
    const char *name;            /* roster name it applies to ("" = default) */
    int attackButton, specialButton, jumpButton;     /* -1 = use the ini's [Brain] buttons */
    float rangeMul;              /* attack range multiplier (1 = default) */
    /* m4 */
    int comboLen;                /* X presses in a full string; the last one launches / knocks away (0 = ini ComboHits) */
    int run;                     /* fwd,fwd and hold = run; X while running = running attack (Cherry) */
    int dash;                    /* fwd,fwd = dash (Adam) */
    int juggle;                  /* jump-cancel after the launcher, then air X x4 (Shiva) */
    int pogo;                    /* jump, then air down+X onto heads (Cherry) */
    int grabStyle;               /* GS_*: Floyd walks into a 2nd enemy (double grab) and uses Y in the grab; Max jumps (piledriver) */
    int fwdSpecial;              /* fwd+Y is worth it at mid range: Floyd pulls the enemy in, Shiva auto-adjusts distance */
    int stunGrenade;             /* neutral Y throws a stun grenade (Estel): open with it, then the full string */
    int blitzThenGrab;           /* the blitz stuns: walk in and grab after it (Max) */
    int blitz;                   /* fwd,fwd,X at distance (1 for everybody we know) */
    int jcCombo;                 /* m6: X x4 -> A (jump cancel) -> air down+X -> air X (Blaze) */
} CharProfile;

/* m4: short input sequences (blitz, throws, specials...), played back over a few frames */
enum { SB_X = 1, SB_A = 2, SB_Y = 4, SB_B = 8, SB_STAR = 16, SB_RT = 32 };
typedef struct { uint16_t ms; int8_t x, y; uint8_t btn; } SeqStep;   /* x: +1 forward, -1 back (relative to dir); y: +1 = stick down */
typedef struct { SeqStep s[20]; int n, i; uint64_t at; int dir; int kind; int interruptible; } Seq;

/* m4: decision types, counted per stage and logged as a summary when the stage ends */
enum { D_COMBO, D_LAUNCHER, D_BLITZ, D_BACKATTACK, D_GRABTRY, D_GRAB, D_GRABHIT, D_THROW, D_VAULT, D_GRABSPECIAL,
       D_DEFSPECIAL, D_DODGEJUMP, D_GRABESCAPE, D_SPECIALRECOVER, D_QUICKRECOVER, D_FWDSPECIAL, D_STAR,
       D_FOOD, D_FOODLEFT, D_WEAPONPICK, D_WEAPONTHROW, D_GRENADEBACK, D_GRENADEAVOID,
       D_RUN, D_DASH, D_POGO, D_JUGGLE, D_PULL, D_DOUBLEGRAB, D_STUNGRENADE, D_RETREAT,
       D_UNREACHABLE, D_DETOUR, D_SHIELDFLANK, D_SHIELDGRAB, D_JUMPATTACK, D_BREAK, D_MONEY, D_CHAIN,
       D_JUMPIN, D_AIRATTACK, D_JCCOMBO, D_AIRSPECIAL, D_CROWDKICK, D_ANTIAIR, D_YIELD, D_BREAKALIGN, D_BREAKMISS, D_FOODSHARE, D_SPREAD, D_WAITDOWN, D_SLAMEVADE, D_SHIELDTAC, D_TRAP, D_CAGED, D_BARREL, D_OFFFLOOR, D_ROBOTPART, D_PARTEVADE, D_RETALIATE,
       /* v1.1 Normal skill only (internal name Mania; never counted with BotSkill=0) */
       D_M_REPOS, D_M_WALL, D_M_FOLLOWUP, D_M_STEPAWAY, D_M_PANIC, D_M_ROWDODGE, D_M_GU, D_M_GUCATCH, D_M_THROW, D_M_SMASH, D_M_PUSH, D_M_DIVE,
       D_M_HEAVYLANE, D_M_OBJROW, D_M_DIAG, D_M_BAIT, D_M_LOWLIFE, D_M_UNSTACK,   /* v1.1 stage 3 */
       D_M_FWDSP, D_M_LEAP, D_M_PIPE, D_M_TRAPSRC, D_M_PICKTRAP, D_M_GREEN, D_COUNT };   /* v1.1 stage 4 */
extern const char *const k_dec_names[D_COUNT];

/* m3: driving the character select with the bot's own pad, one visit at a time */
enum { CSP_START, CSP_NAV, CSP_COLOR, CSP_CONFIRM, CSP_ALT, CSP_DONE };
typedef struct {
    int phase, target, origEntry, triedOrig, pulses, noChange, colorPresses, colorNoChange, taps, altTaps;
    int pressing, before; uint64_t pressAt, startAt;
    uint64_t unavail;            /* roster entries that would not confirm this visit (bit per index) */
    int doneLogged;
    /* m7 live mode (F7 menu open at character select): follow the menu's pick without confirming */
    int liveTarget, liveColor, navRounds, unconfTries, liveLogged; uint64_t retryAt;
} CsDrive;

typedef struct {
    int index, nbots, slot;                /* bot number (0..), how many bots, configured player slot */
    char name[32];
    uint64_t activeSince;
    int attacks;                           /* test-brain counter */
    /* identity */
    uint32_t uid; int identified; uint64_t identSince;
    float score[W_MAXCH], pen[W_MAXCH]; uint32_t scoreUid[W_MAXCH]; int nscore; int samples;
    int prevLx;                            /* stick x we sent last frame (-1/0/+1) */
    /* calibration */
    int xSign, zSign, zVotes; uint64_t zFlipAt;   /* m6.5 */
    float lastX, lastZ; int lastLy;
    /* brain */
    float maxLife; uint32_t targetUid; uint64_t comboStart, lastSpecial, lastProgress, lastJump, attackSince;
    int comboTap, hitsSeen, misses; float rangeScale, laneScale; V3 progressPos;
    int mode; uint64_t modeSince;          /* for logging transitions */
    int fallbackLogged;
    int missingFrames;
    char why[160]; uint64_t lastDecisionLog;
    BotIdent id; int playerSlot;           /* player slot index (0 = P1) our controller sits in, -1 none */
    uint64_t retreatSince, retreatCooldownUntil;
    int hadLevel, menuPrevTap, menuDone, menuStartStatus, menuTaps, menuLoggedEnd; uint64_t menuSince;   /* menu / character-select confirm */
    /* m2.4 safety gate */
    uint64_t joinAt;                       /* when the current join attempt started (0 = none yet) */
    int sawLevel;                          /* a running stage was seen since F7: no more menu taps until F7 off/on */
    uint64_t lastPauseAt;                  /* last frame the game was paused (resume grace) */
    int gateCat, gateLoggedCat; uint64_t gateCatSince; char gateWhy[120];
    int stuckJumps; uint32_t ignoreUid; uint64_t ignoreUntil;   /* unreachable target handling */
    /* m5 */
    struct { uint32_t uid; int fails; uint64_t until; } unreach[12];   /* escalating ignore: 4 s, 15 s, 60 s */
    uint64_t detourUntil; float detourX, detourZ; int detours, detourFlip;   /* walking around an obstacle */
    uint64_t lastJumpAttack, flankUntil, lastShieldGrab; uint32_t shieldUid;
    uint32_t breakUid; int breakFrozen;
    uint64_t lastApproachAt, approachSince; int filterFallbackLogged; uint64_t breakReachUntil;
    int introDone;   /* m6: the stage intro gate passed once this level */
    /* m6 */
    float p1PrevX, p1PrevZ, p1Vx, p1Vz; uint64_t p1PrevAt; uint32_t p1Uid;      /* P1 movement (for making way) */
    int breakSide, breakFails; float breakLife, breakStand; uint64_t breakSwingSince, breakAlignSince;
    float breakZOff, breakZPrev; uint64_t breakStart, breakZAt, breakNudgeUntil, breakLastHit; int breakInPos, breakZBlocked;   /* m6.1 */
    float breakXPrev; uint64_t breakXAt;   /* m6.3 */
    float breakZLim;   /* m6.5: the reachable depth when blocked (back wall) */
    int wlx; uint64_t wlxAt; uint32_t sideUid; int sideK; uint64_t sideAt; int waitArrived; uint32_t closeOkUid; uint64_t closeOkUntil;   /* m6.7 anti-jitter */
    uint32_t slamUid, punishUid, slamNoted; uint64_t slamSince, punishUntil;   /* m6.6: jump slams */
    uint32_t shBrokenUid, shTacUid; int shTacKind; uint64_t lastShieldSpecial, lastShieldStar, shTacAt, lastShBackoff;   /* m6.7: shield cops */
    uint64_t trapNoteAt, tzWaitSince;   /* m6.7: floor traps */
    int retreatSideK; uint64_t retreatSideAt; uint64_t enemyNearAt;   /* m6.7 */
    uint64_t jumpTimes[8]; int jumpIdx; uint64_t lastJumpMove, lastJumpIn, lastAirAttack, lastAirSpecial, lastCrowdKick, jumpBanUntil;
    int jumpMoveKind, airFails; uint64_t airFailWindow; uint32_t antiAirUid; uint64_t antiAirUntil;
    uint64_t yieldUntil; float yieldZ; int jcToggle; uint64_t lastSpread; uint32_t waitDownUid; uint64_t breakClaimAt;   /* m6.2: breakable claim refreshed while we work on it */
    /* m3 */
    char wantChar[32];                     /* character to pick at character select ("" / "Auto" = keep the game's) */
    int wantColor;                         /* 0 = Auto, else color number 1..N */
    int joinTaps;                          /* A taps in the current join attempt */
    int joinAttempts;                      /* join attempts at this character-select visit */
    int prevCharSel; CsDrive cs;
    /* m7: live character select and backing out */
    int live, prevLive;                    /* set by the host every frame: the F7 menu is open */
    int backout, backoutDone, boPresses;   /* host: bot turned OFF at character select -> press B until our slot is free */
    uint64_t boSince; char boWhy[96];
    int csInSlot, rejoins; uint64_t rejoinAt;   /* we were in a slot at this visit (and left it), rejoin taps */
    char liveStatus[56];                   /* what the game shows for us (for the menu's "Game:" line) */
    const CharProfile *prof; int profFor, profNamed;  /* profile for innerPlayableIndex profFor (profNamed: the roster name was known) */
    /* m4 */
    int aggression;                        /* 0 low, 1 normal, 2 high ([Bots] PnAggression) */
    Seq seq; uint64_t lastThink;
    int dec[D_COUNT], decTotal[D_COUNT];   /* per stage / since the bot was plugged in */
    int inLevel; uint64_t noLevelSince;
    /* intro wait before the stick check */
    uint64_t levelSince; int introMode, introModeLogged; uint64_t introModeSince; uint64_t stickQuietSince; const char *introWhy;
    /* defense */
    uint64_t grabbedSince, lastMash, lastDodge, lastDefSpecial, lastRecoverTap, airHitSince; int dodgeCount; uint64_t dodgeWindow;
    uint32_t grabbedByUid; int grabEscapeCounted, quickRecCounted, hitsTaken; uint64_t lastHitAt; int prevState;
    /* health budget */
    uint64_t noSpecialUntil, recoverUntil, specialAt; float bankedAtSpecial, lifeAtSpecial; int greenKnown, greenChecks, greenNoChange;
    uint64_t lastStar;
    /* offense */
    uint64_t lastBlitz, lastBack, lastGrabTry, grabStart, lastGrabHit, lastFwdSpecial, lastRun, lastDash, lastPogo, lastCombo, comboEnd;
    int grabHits, grabTries, comboCount, wantJuggle, starYB; uint64_t nextComboAt; uint32_t grabTarget; uint64_t doubleGrabUntil;
    /* pickups */
    uint64_t pickNearAt;   /* m6.3 */
    uintptr_t goalPick; int goalKind, goalTries; uint64_t goalSince, lastPickPress; uintptr_t badPick[8]; uint64_t badPickUntil[8];
    uintptr_t leftFor[8]; int nleftFor; int heldOnlyThrow; uintptr_t heldAddr;
    /* m7.2: melee weapon throws */
    uint32_t barrelSkipUid, barrelSeen[8]; int nBarrelSeen;   /* m7.2/m7.3 */
    uint64_t lastMeleeThrowAt, meleeNearAt; int heldKnifeOk, throwsDone; uint32_t rng;   /* m7.3 */
    uint64_t fightSince, fightLastAt;   /* m7.2: first frame of the current run of fight() frames */
    uint64_t cagedNoteAt; int cagedN; uint32_t cagedSum;   /* m7.2: not-yet-released enemies */
    uint64_t heldSince, heldLastSwingAt, wtNoteAt; int heldSwings, heldDurOk, wtNoteKind; float heldLastLife, heldLastDrop;
    /* m7.4: off-floor enemies, robot parts, friendly-fire retaliation */
    uint32_t offNoted[8]; int nOffNoted; struct { uint32_t uid; uint64_t since, seen; } offT[4];
    uint32_t partNoted, partTgtNoted; uint64_t partNoteAt, partEvadeAt, lastPartJump, partLastAt;
    uint64_t rlNextAt, rlSwingUntil; int rlSwingSlot, rlSwings, rlSkipNoted;
    /* v1.1 (m8): Mania skill state (only read and written when C.botSkill == 1) */
    uint64_t mSpecSeen, mFuUntil, mStepUntil, mLastPanic, mLastSmash, mLastGU;
    float mStepZ; int mFuDone, mLogged;
    uint64_t mReposSince, mReposUntil, mReposCool; float mReposX, mReposZ; int mReposKind;
    uint64_t mRowUntil, mRowCool; uint32_t mRowUid; float mRowZ;
    uint32_t mPushUid, mPushNoteUid; uint64_t mPushSince, mPushOkAt; int mPushSide;
    uint64_t mLastDive;   /* v1.1 stage 2: air special (Axel's Dragon Dive) */
    /* v1.1 stage 3 (Normal only) */
    uint64_t mGuardUntil; uint32_t mGuardUid; float mGuardX, mGuardZ; int mGuardKind; uint64_t mGuardEndAt; float mGuardFX, mGuardFZ; float mLeapBX, mLeapBZ; uint64_t mLeapAt;   /* out of a heavy's lane / a flying object's row */
    uint32_t mDiagUid; int mDiagSign, mDiagOn;                                          /* diagonal approach */
    uint64_t mRetSince, mRetRestUntil, mRetSideAt; int mRetSideK, mRetNoted; uint64_t mRetLastEnd, mRetDetUntil; float mRetDetZ; int mRetDetHold;            /* low-life retreat */
    uint32_t mBaitUid, mUnstackNoteUid; uint64_t mBaitNoteAt;
    /* v1.1 stage 4 (Normal only) */
    float mLifeTop;                                                                     /* highest life seen (never reset per section) */
    uint32_t mLeapUid, mLeapNoted; float mLeapX, mLeapZ;                                 /* out of a heavy's leap landing */
    uint64_t mLastFwdGap, mGreenNoteAt, mPickTrapNoteAt; uint32_t mPipeNoted;
    uint64_t mEdgeWaitSince, mEdgeWaitLast;                                              /* waiting at the edge of a danger lane / zone */
    int mAlt[5], mAltLogged;                                                             /* equipped alternative moves (-1 unknown) */
} BotBrain;
/* m7.4 test / replay hooks */
extern int g_brainLearnWalls;                  /* 0 in the replay (frozen snapshots look like walls) */
int brain_retal_debt(int botSlot, int humanSlot);
int brain_retal_ff_seen(void);
void brain_retal_reset(void);
int brain_floor(float *lo, float *hi);         /* 1 when the walkable depth is known */
void brain_floor_reset(void);
void brain_parts_reset(void);
void brain_part_stats(int *n, uint32_t *uid, int *st, uint64_t *plantedMs, uint64_t *raisedMs, int *hits, uint64_t *botMs, uint64_t *botJumps);

void brain_reset(BotBrain *b, int index, int nbots, int slot, const char *name, uint64_t now);   /* keeps b->id */
/* the bot pad was just plugged in: forget identity, remember which controllerIds were humans */
void brain_attached(BotBrain *b, int priorFna, int priorSdl, const World *w, uint64_t now);
int brain_ctrl_is_bot(const BotBrain *all, int nall, int ctrlId);
/* w may be NULL or !ok. stateUsable = the state reader works at all (else: test pattern) */
/* the game window has keyboard focus (sor4bots.c tells the brain every frame; default: focused) */
void brain_set_focus(int focused, uint64_t now);
/* the character name of roster index i changed for this bot (hook for per-character specials) */
void brain_set_character(BotBrain *b, int rosterIndex, const char *name);
/* a fresh, unattached brain: no controller, no slot */
void brain_init_detached(BotBrain *b, int index, const char *name);
/* m4: log the per-stage decision counts (if any) and reset them */
void brain_summary(BotBrain *b, const char *why);
void brain_think(BotBrain *b, BotBrain *all, int nall, const World *w, int stateUsable, uint64_t now, PadOut *out);
