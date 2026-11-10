/* Read-only view of Streets of Rage 4's live game state, from inside the proxy DLL. */
#pragma once
#include <stdint.h>

typedef struct { float x, y, z; } V3;           /* x = left/right, y = height (0 on the floor), z = depth (lane) */

enum { CS_AIRHIT, CS_AIRLIGHTHIT, CS_AIRMOVE, CS_CROUCH, CS_CROUCHGUARD, CS_CROUCHGUARDHIT, CS_CROUCHHIT,
       CS_CROUCHMOVE, CS_DASH, CS_DISAPEAR, CS_GROUNDHIT, CS_JUMP, CS_ONGROUND, CS_STANDDIE, CS_STANDGUARD,
       CS_STANDGUARDHIT, CS_STANDHIT, CS_STANDMOVE, CS_STANDWALK, CS_COUNT, CS_OTHER = 100, CS_NONE = -1 };
extern const char *const k_state_names[CS_COUNT];
const char *state_name(int cs);
int state_is_hurt(int cs);      /* being hit, knocked down, dying or gone */
int state_is_down(int cs);      /* on the floor / dying / gone: not worth targeting */

enum { CTRL_NONE, CTRL_PLAYER, CTRL_AI, CTRL_OTHER };
typedef struct {
    uintptr_t addr; uint32_t uid; int team, player;
    float life, maxLife; V3 pos; int facing;          /* facing +1 right, -1 left */
    int state, ctrlKind; float stickX, stickY;
    int hitStun, invincible, scripted, moveId;
    /* m4 (all 0 when unreadable) */
    int grabbing;                 /* grabbedCharacterUidList.Count: we hold this many characters */
    uint32_t grabUid;             /* first uid in that list */
    uint32_t grabbedBy;           /* state is StandHit and StandHit.grabSourceUid != 0: held by that uid */
    uintptr_t heldAddr;           /* heldPickup (weapon in hand), 0 = none */
    int superArmor, untargettable, cantBeGrabbed;     /* timers > 0 */
    int airRecoverTimer, airTime, noQuickRecover;     /* AirRecoverTimer, TimeHitInAirSinceRelaunch, quickRecoverDeactivated */
    int boss;                     /* CharacterData.isBoss (via dataRef) */
    int aiAttacking;              /* AiCharacterController.isAttacking */
    uint32_t aiTarget;            /* AiCharacterController.targetCharacterRef.uid (0 = none) */
    /* m5 (-1 / 0 when unreadable) */
    int plane;                    /* PhysicalObjectBase.CurrentPlane (enum; compared with our fighter's, -1 unknown) */
    int entered;                  /* Character.hasEnteredTheScreenOnce (-1 unknown) */
    int collidable;               /* Collidable && !NeverCollidable (-1 unknown) */
    int guard;                    /* guarding now: controller Guard flag or a guard state */
    int canGuard;                 /* CharacterData.guardCrushDuration > 0 (a fighter that blocks, e.g. shield cops) */
    int btRoot;                   /* AiCharacterController.currentBtRoot (-1 unknown) */
    int lineOfSight;              /* AiCharacterController.lineOfSightToTarget (-1 unknown) */
    /* m6.7: shields (stage 2 Murphy cops). The m5 guard flag never read 1 and canGuard is set for every enemy;
       the shield is a move whose super armor blocks (CharacterData+Move.superArmorIsBlock), and it breaks when
       Character.stamina runs out (staminaBrokenTimer > 0 while broken) */
    float stamina, maxStamina;    /* Character.stamina / CharacterData.maxStamina (-1 unknown) */
    int staminaBroken;            /* Character.staminaBrokenTimer > 0: guard/shield broken */
    int guardHits;                /* Character.TakenHitInGuardCounter */
    int lastHitGuarded;           /* Character.LastHitReceivedWasInGuard */
    int shieldUser;               /* its CharacterData has a block-armor move (or the name says Murphy / shield) */
    int shieldMove;               /* the current move is one of those (shield raised) */
    char dname[24];               /* CharacterData._name (short) */
    /* m7.2: the weapon in hand (Character.heldPickup -> PickupInstance.life, PickupData.weapon -> WeaponData) */
    float heldLife, heldMaxLife;  /* PickupInstance.life / WeaponData.maxLife (-1 = not readable / no weapon) */
    int heldOnlyThrow, heldWType; /* WeaponData.onlyThrow, WeaponData.type (WeaponTypeEnum value; names unknown) */
    int heldKnife;                /* WeaponData.displayName looks like a knife/kunai/dagger/shuriken */
    char heldName[24];            /* WeaponData.displayName (may be a localization key) */
    /* m7.4 */
    uint32_t hitSrc;              /* Character.lastHitReceivedSourceUid (0 = none / unreadable) */
    int robotDriver;              /* uid is in a RobotInstance.driverUids (seated when also scripted) */
    int robotPart;                /* brain only: a pseudo fighter made from a grounded RobotPartInstance */
} Chr;
enum { PK_OTHER, PK_FOOD, PK_WEAPON, PK_STAR, PK_GRENADE, PK_SCORE };   /* m5: PK_SCORE = money (PickupData.scoreGain > 0) */
typedef struct { uintptr_t addr; V3 pos; float life; int team, player;
                 int kind; float lifeGain; int starGain, ticking, onlyThrow, held, scoreGain;
                 float maxLife; int wtype, knife; char wname[24]; } Pick;   /* m7.2: WeaponData.maxLife / type / displayName */
typedef struct { uint32_t uid; V3 pos, vel; int team, kind; } Proj;   /* kind 0 = projectile (bullet...), 1 = thrown pickup */
typedef struct { int ctrl, type, status; } SlotInfo;
typedef struct { int playable, continues, allyId; float life, fury; int score;
                 int stars; float banked; int starBtn, starYB; float weaponLife; } PlayerStateInfo;   /* m7.2: weaponInHandLife (-1 unknown) */   /* m4: starCount, bankedLife, star-move control options (-1 unknown) */

#define W_MAXCH 64
#define W_MAXPK 32
#define W_MAXPJ 24
#define W_MAXBR 24
typedef struct { uintptr_t addr; uint32_t uid; V3 pos; float life; int entered;   /* m5: DestroyableObjectInstance (sinks, crates...) */
                 /* m7.2: explosive barrels. explosive: 1 = its DestroyableObjectData blows up (willBlowStateTime > 0,
                    a damage area / projectile / hazard when destroyed, or an explosive name), 0 = no, -1 = data not read.
                    blowTimer: DestroyableObjectInstance.willBlowTimer (> 0 = lit fuse). radX/radZ: the damage area's
                    ellipseRadiusMax (0 = unknown). */
                 int explosive, blowTimer, blowTime; float maxLife, radX, radZ; char name[24]; } Brk;
/* m7.4: the giant robot boss (m7.3-run2): RobotPartInstance objects in physicalObjectList and
   LevelState.robotList. Leg parts plant at the back edge of the floor; hits on them drain the driver. */
#define W_MAXRP 24
#define W_MAXRB 4
typedef struct { uint32_t uid; V3 pos, vel; float life, dataLife, hitFreeze; int team, partId, robotId, plane; char name[20]; } RPart;
typedef struct { int uid, active, broken, animId, firstDriver, nDrivers, nParts; uint32_t drivers[4]; V3 pos; float healLife, dmgFactor; } Robot;
#define W_MAXHZ 16
/* m6.7: floor traps and damage areas in LevelState.physicalObjectList: HazardInstance (kind 0: curAnimId, timer,
   cooldownTimer; zone = HazardData.zone bounding box around pos, if it reads plausibly) and DamageAreaInstance (kind 1:
   radius) */
typedef struct { uint32_t uid; V3 pos; int kind, anim, timer, cooldown, haveZone; float x0, x1, z0, z1; } Haz;
typedef struct {
    int ok;                       /* a level is running and was read this frame */
    uint64_t seq;
    int frames, mode, gameOver, nLevels, scene;
    int nch; Chr ch[W_MAXCH];
    int npk; Pick pk[W_MAXPK];
    int npj; Proj pj[W_MAXPJ];    /* m4: projectiles and thrown pickups (LevelState.physicalObjectList) */
    int nbr; Brk br[W_MAXBR];     /* m5: breakable objects (LevelState.physicalObjectList) */
    int nhz; Haz hz[W_MAXHZ];
    int nrp; RPart rp[W_MAXRP];   /* m7.4: robot parts (LevelState.physicalObjectList) */
    int nrb; Robot rb[W_MAXRB];   /* m7.4: robots (LevelState.robotList) */     /* m6.7: hazards (floor traps) and damage areas (LevelState.physicalObjectList) */
    int bossMode;                 /* m4: GameState.bossMode */
    int friendlyFire;             /* m7.2: MetaGameGlobalState.friendlyFireEnabled (-1 = not readable) */
    float leftWallX; int haveWall;
    int haveCam; V3 cam; float camSizeY;
    int nslot; SlotInfo slot[8];
    int nps; PlayerStateInfo ps[8];
    const char *source;           /* how the GameState was found */
    int frozen, frozenMs;         /* frame counter not moving (paused, or a stale copy) */
    /* m2.4 safety signals */
    int live;                     /* level readable AND its frame counter has been advancing (>= 10 reads, no 100 ms stall) */
    int paused, pausedSlot, pausedStatus;  /* an occupied player slot has _gameplayStatus != 0 (pause menu) */
    int noGameState;              /* positive evidence that no GameState exists (character select / menus) */
    const char *noGsWhy;          /* why noGameState is (or isn't) set, for the log */
    /* m3: menus, read from online.globalState (MetaGameGlobalState) and program.mainMenu / pauseMenu */
    int menuRead;                 /* online.globalState was readable this frame */
    char menuName[40];            /* short type name of globalState.currentMenu ("CharacterSelection", "MainMenu", ...; "" = none) */
    char topScreen[48];           /* short type name of the top screen in program.mainMenu ("" = no screen / unreadable) */
    int mainScreens, pauseScreens;/* screens open in program.mainMenu / program.pauseMenu (-1 = unreadable) */
    int charSel;                  /* PROVEN character select: currentMenu is CharacterSelection, the top menu screen is
                                     CharacterSelectionScreen, no pause screen, no running stage */
    int ncs;                      /* per player (index = player slot): character select state */
    struct { int cursor, playable, palette, altConfirmed, haveUser; } cs[8];
                                  /* cursor = PlayerUserData.menuEntryId (index into the roster), playable =
                                     innerPlayableIndex (0 = not confirmed, else roster index + 1), palette = color
                                     (0 = default), altConfirmed = special-moves menu confirmed (-1 unknown) */
} World;

/* playable characters, from main.metaGameConfigRef -> MetaGameConfigData.playableCharacterArray */
#define ROSTER_MAX 40
typedef struct { char name[32]; int palettes, menuId, left, right, up, down, flags; } RosterEntry;  /* flags: 1 = SoR4 Adam, 2 = SoR3 Roo */
int  state_roster(const RosterEntry **out);   /* number of entries (0 = not readable yet); re-read at most every 3 s until it works */
int  state_roster_find(const char *name);     /* index by name (case-insensitive), -1 = none */
int  state_roster_path(int from, int to, int *firstDir);   /* menu steps from -> to over the navigation graph (BFS, shortest); dir 0..3 = up, down, left, right; -1 = no path */
/* m7.1 character-select navigation graph. Static part: the game's own menu links where set (in the real game they are
   ROSTER INDICES; -1 = not set) plus inferred edges for the unset ones (left/right: the reverse of another entry's link,
   else the next entry of the same menu group; up/down: the reverse link only). Learned part: every observed press
   (from, dir) -> to overrides the static edge for this session; a press with no effect twice blocks that edge until
   the next character-select visit. */
int  state_roster_nav(int from, int dir);                  /* neighbour used for planning, -1 = none/blocked */
int  state_roster_nav_static(int from, int dir);           /* static (game + inferred) neighbour */
int  state_roster_noeffect(int from, int dir);             /* presses from 'from' in 'dir' that did nothing (this visit) */
void state_roster_observe(int from, int dir, int to);      /* record what a press did */
void state_roster_nav_new_visit(void);                     /* forget "no effect" blocks (learned moves stay) */
void state_roster_set(const RosterEntry *r, int n);        /* tests: install a roster and build its graph */

int  state_init(const void *exeBase);   /* 1 = resolver OK (layouts found) */
int  state_available(void);             /* resolver OK and not permanently failed */
const char *state_failure(void);        /* why not, for the log */
int  state_read(World *w);              /* main thread, once per frame; 1 = world->ok */
void state_set_wanted(int wanted);      /* allow background searches (bots on, or diagnostics) */
void state_dump(const World *w, const char *tag);
void state_log_slots(const World *w, const char *why);
void state_shutdown(void);
void state_want_menu_check(void);       /* bots want proof of "no GameState anywhere": run a search soon */
void state_test_tls(uintptr_t block, int moduleIndex, int typeIndex);   /* tests only */
