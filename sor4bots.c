/*
 * SoR4Bots - milestone 4
 * Proxy SDL2.dll for Streets of Rage 4 (Steam, Windows x64).
 *
 * Every export of the game's original SDL2.dll is re-exported with the same name and ordinal. All but
 * eight are PE forwarders to SDL2_real.dll (see SDL2.def). The eight we implement here:
 *   SDL_GameControllerGetButton / GetAxis - call through; return "not pressed" for the human pads while
 *                                   the F7 menu is open (and for buttons held when it closes, until released)
 *   SDL_Init / SDL_InitSubSystem  - call through; log input drivers, hints, Steam Input signals
 *   SDL_PollEvent                 - call through; per-frame state read + bot brains; device logging,
 *                                   bot attach policy, re-ordering for late human pads; F7
 *   SDL_Quit / SDL_QuitSubSystem  - release our pads, stop the search thread, then call through
 *   SDL_GetVersion                - report 2.0.9 (what the stock DLL reports)
 *
 * Bot pads are SDL virtual joysticks (SDL >= 2.24). The game state is only ever read (sor4state.c),
 * never written; no game code is called or patched.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include "sor4common.h"
#include "sor4state.h"
#include "sor4brain.h"
#include "sor4menu.h"

#define SOR4BOTS_VERSION "RageBots v1.0"   /* internal build m7.5 */

/* ---- minimal SDL2 ABI (from SDL 2.32 headers; stable across SDL2) ---- */
typedef uint8_t Uint8; typedef int16_t Sint16; typedef uint16_t Uint16; typedef uint32_t Uint32;
typedef struct { Uint8 major, minor, patch; } SDL_version;
typedef struct SDL_Joystick SDL_Joystick;
typedef struct { Uint8 data[16]; } SDL_JoystickGUID;
typedef union { Uint32 type; Uint8 padding[56]; } SDL_Event;
typedef struct {
    Uint16 version, type, naxes, nbuttons, nhats, vendor_id, product_id, padding;
    Uint32 button_mask, axis_mask;
    const char *name;
    void *userdata;
    void (*Update)(void *userdata);
    void (*SetPlayerIndex)(void *userdata, int player_index);
    int (*Rumble)(void *userdata, Uint16 lo, Uint16 hi);
    int (*RumbleTriggers)(void *userdata, Uint16 l, Uint16 r);
    int (*SetLED)(void *userdata, Uint8 r, Uint8 g, Uint8 b);
    int (*SendEffect)(void *userdata, const void *data, int size);
} SDL_VirtualJoystickDesc;

#define SDL_INIT_JOYSTICK       0x00000200u
#define SDL_INIT_GAMECONTROLLER 0x00002000u
#define SDL_KEYDOWN             0x300
#define SDL_KEYUP               0x301
#define SDL_JOYDEVICEADDED      0x605
#define SDL_JOYDEVICEREMOVED    0x606
#define SDL_CONTROLLERDEVICEADDED   0x653
#define SDL_CONTROLLERDEVICEREMOVED 0x654
#define SDL_CONTROLLERDEVICEREMAPPED 0x655
#define SDL_SCANCODE_F7         64
#define SDL_TEXTEDITING         0x302
#define SDL_TEXTINPUT           0x303
#define SDL_JOYAXISMOTION       0x600
#define SDL_JOYBALLMOTION       0x601
#define SDL_JOYHATMOTION        0x602
#define SDL_JOYBUTTONDOWN       0x603
#define SDL_CONTROLLERAXISMOTION 0x650
#define SDL_CONTROLLERBUTTONDOWN 0x651
typedef struct SDL_GameController SDL_GameController;
typedef Uint8 (*PFN_GCGetButton)(SDL_GameController *, int);
typedef Sint16 (*PFN_GCGetAxis)(SDL_GameController *, int);
typedef SDL_GameController *(*PFN_GCFromID)(int);
typedef SDL_Joystick *(*PFN_GCGetJoystick)(SDL_GameController *);
#define SDL_JOYSTICK_TYPE_GAMECONTROLLER 1
#define SDL_JOYSTICK_AXIS_MIN   (-32768)

typedef int  (*PFN_Init)(Uint32);
typedef void (*PFN_Quit)(void);
typedef void (*PFN_QuitSub)(Uint32);
typedef int  (*PFN_PollEvent)(SDL_Event *);
typedef void (*PFN_GetVersion)(SDL_version *);
typedef int  (*PFN_AttachVirtualEx)(const SDL_VirtualJoystickDesc *);
typedef int  (*PFN_IntInt)(int);
typedef SDL_Joystick *(*PFN_JoystickOpen)(int);
typedef void (*PFN_JoystickClose)(SDL_Joystick *);
typedef int  (*PFN_SetVAxis)(SDL_Joystick *, int, Sint16);
typedef int  (*PFN_SetVButton)(SDL_Joystick *, int, Uint8);
typedef int  (*PFN_Int)(void);
typedef const char *(*PFN_StrInt)(int);
typedef SDL_JoystickGUID (*PFN_GetDeviceGUID)(int);
typedef void (*PFN_GUIDString)(SDL_JoystickGUID, char *, int);
typedef int  (*PFN_AddMapping)(const char *);
typedef const char *(*PFN_Str)(void);
typedef const char *(*PFN_GetHint)(const char *);
typedef const Uint8 *(*PFN_GetKeyboardState)(int *);
typedef void *(*PFN_GetKeyboardFocus)(void);
typedef Uint32 (*PFN_GetWindowFlags)(void *);
typedef int  (*PFN_JoystickInstanceID)(SDL_Joystick *);
typedef Uint16 (*PFN_U16Int)(int);
typedef int (*PFN_SetHint)(const char *, const char *);

static struct {
    HMODULE mod;
    PFN_Init Init, InitSubSystem; PFN_Quit Quit; PFN_QuitSub QuitSubSystem; PFN_PollEvent PollEvent;
    PFN_GetVersion GetVersion; PFN_AttachVirtualEx AttachVirtualEx; PFN_IntInt DetachVirtual;
    PFN_JoystickOpen JoystickOpen; PFN_JoystickClose JoystickClose;
    PFN_SetVAxis SetVirtualAxis; PFN_SetVButton SetVirtualButton;
    PFN_Int NumJoysticks; PFN_IntInt IsGameController, JoystickIsVirtual, DevInstanceID, DevType, DevPlayerIndex;
    PFN_StrInt JoystickNameForIndex, JoystickPathForIndex, ControllerNameForIndex;
    PFN_GetDeviceGUID GetDeviceGUID; PFN_GUIDString GUIDString; PFN_AddMapping AddMapping;
    PFN_Str GetError, CurrentVideoDriver; PFN_GetHint GetHint; PFN_GetKeyboardState GetKeyboardState; PFN_GetKeyboardFocus GetKeyboardFocus; PFN_GetWindowFlags GetWindowFlags; PFN_JoystickInstanceID JoystickInstanceID;
    PFN_U16Int DevVendor, DevProduct;
    PFN_SetHint SetHint;
    PFN_GCGetButton GCGetButton; PFN_GCGetAxis GCGetAxis; PFN_GCFromID GCFromID; PFN_GCGetJoystick GCGetJoystick;
} R;

Config C;
static int g_testAssumeFocus;

/* install.ps1 looks for this text to recognise an older SoR4Bots build when upgrading */
__attribute__((used)) static const char g_marker[] = "SOR4BOTS_PROXY_SDL2";

static wchar_t g_dir[MAX_PATH];

/* ---- logging ----
 * Opened exactly once per process (rotation SoR4Bots.log -> SoR4Bots.prev.log happens at most once),
 * never reopened, never truncated: CreateFile(FILE_APPEND_DATA, OPEN_ALWAYS, share read+write).
 * Plain WriteFile, no CRT FILE streams, so no CRT stream lock can be left held by a thread that the
 * OS killed at exit. A named mutex detects a second process using the same folder; that process
 * writes SoR4Bots.pid<N>.log instead of rotating/truncating the first one's log.
 * Every line also goes to OutputDebugString (visible in DebugView). */
static HANDLE g_logH = INVALID_HANDLE_VALUE;
static LONG g_logState;                 /* 0 not opened yet, 1 open, 2 failed, 3 closed (detach) */
static CRITICAL_SECTION g_logLock;
static wchar_t g_logPath[MAX_PATH];
static void log_raw(const char *s, DWORD n) {
    if (g_logH != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(g_logH, s, n, &w, NULL); }
}
static void log_open(void) {                /* under g_logLock */
    if (g_logState) return;
    g_logState = 2;
    wchar_t prev[MAX_PATH], mname[96]; char hdr[600];
    uint32_t hsh = 2166136261u;
    for (const wchar_t *p = g_dir; *p; p++) hsh = (hsh ^ (uint32_t)towlower(*p)) * 16777619u;
    swprintf(mname, 96, L"Local\\SoR4Bots-log-%08x", hsh);
    HANDLE m = CreateMutexW(NULL, FALSE, mname);           /* kept until the process ends */
    int secondary = m && GetLastError() == ERROR_ALREADY_EXISTS;
    DWORD rotErr = 0; const char *rot;
    if (!secondary) {
        swprintf(g_logPath, MAX_PATH, L"%lsSoR4Bots.log", g_dir);
        swprintf(prev, MAX_PATH, L"%lsSoR4Bots.prev.log", g_dir);
        if (GetFileAttributesW(g_logPath) == INVALID_FILE_ATTRIBUTES) rot = "no previous log";
        else if (MoveFileExW(g_logPath, prev, MOVEFILE_REPLACE_EXISTING)) rot = "previous log moved to SoR4Bots.prev.log";
        else { rotErr = GetLastError(); rot = "could not move the previous log (appending to it)"; }
    } else {
        swprintf(g_logPath, MAX_PATH, L"%lsSoR4Bots.pid%lu.log", g_dir, GetCurrentProcessId());
        rot = "ANOTHER PROCESS is already logging to SoR4Bots.log; this process uses its own file";
    }
    g_logH = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_logH == INVALID_HANDLE_VALUE) {
        snprintf(hdr, sizeof hdr, "SoR4Bots: could not open log %ls (error %lu)\n", g_logPath, GetLastError());
        OutputDebugStringA(hdr); return;
    }
    g_logState = 1;
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
    int n = snprintf(hdr, sizeof hdr, "==== Rage Bots (SoR4Bots) log opened once for process %lu (%ls); %s%s",
                     GetCurrentProcessId(), exe, rot, rotErr ? "" : "\r\n");
    if (rotErr) n += snprintf(hdr + n, sizeof hdr - n, ", error %lu\r\n", rotErr);
    log_raw(hdr, (DWORD)strlen(hdr)); OutputDebugStringA(hdr);
}
void logf_(int lv, const char *fmt, ...) {
    if (lv > C.logLevel || g_logState >= 2) return;
    char line[2048]; SYSTEMTIME t; GetLocalTime(&t);
    int n = snprintf(line, sizeof line, "%02d:%02d:%02d.%03d [%s] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                     lv == LV_ERROR ? "ERR" : lv == LV_INFO ? "INF" : "DBG");
    va_list ap; va_start(ap, fmt); int m = vsnprintf(line + n, sizeof line - n - 3, fmt, ap); va_end(ap);
    n = m < 0 ? n : n + m < (int)sizeof line - 3 ? n + m : (int)sizeof line - 3;
    line[n++] = '\r'; line[n++] = '\n'; line[n] = 0;
    EnterCriticalSection(&g_logLock);
    if (!g_logState) log_open();
    if (g_logState == 1) log_raw(line, (DWORD)n);
    LeaveCriticalSection(&g_logLock);
    OutputDebugStringA(line);
}
static void log_flush(void) { if (g_logState == 1) FlushFileBuffers(g_logH); }

static int parse_button(const wchar_t *s, int def) {
    static const wchar_t *names[BTN_COUNT] = { L"A", L"B", L"X", L"Y", L"Back", L"Guide", L"Start",
        L"LeftStick", L"RightStick", L"LB", L"RB", L"Up", L"Down", L"Left", L"Right" };
    for (int i = 0; i < BTN_COUNT; i++) if (!_wcsicmp(s, names[i])) return i;
    return def;
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static float ini_float(const wchar_t *sec, const wchar_t *key, const wchar_t *ini) {
    wchar_t buf[64]; GetPrivateProfileStringW(sec, key, L"0", buf, 64, ini);
    float v = (float)_wtof(buf); return v > 0 ? v : 0;
}
static void load_config(void) {
    wchar_t ini[MAX_PATH], buf[128];
    swprintf(ini, MAX_PATH, L"%lsSoR4Bots.ini", g_dir);
    GetPrivateProfileStringW(L"General", L"LogLevel", L"Info", buf, 128, ini);
    C.logLevel = !_wcsicmp(buf, L"Debug") ? LV_DEBUG : !_wcsicmp(buf, L"Error") ? LV_ERROR : LV_INFO;
    C.inputFallbackSeconds = clampi(GetPrivateProfileIntW(L"General", L"InputFallbackSeconds", 5, ini), 0, 120);
    C.diagnostics = GetPrivateProfileIntW(L"General", L"Diagnostics", 1, ini) != 0;

    /* m3: one bot per player slot P2..P4, each OFF unless the F7 menu turned it on (saved here) */
    for (int i = 0; i < 3; i++) {
        wchar_t k[32];
        swprintf(k, 32, L"P%dBot", i + 2); C.botOn[i] = GetPrivateProfileIntW(L"Bots", k, 0, ini) != 0;
        swprintf(k, 32, L"P%dCharacter", i + 2); GetPrivateProfileStringW(L"Bots", k, L"Auto", buf, 128, ini);
        snprintf(C.botChar[i], sizeof C.botChar[i], "%ls", buf);
        swprintf(k, 32, L"P%dAggression", i + 2); GetPrivateProfileStringW(L"Bots", k, L"Normal", buf, 128, ini);
        C.botAggr[i] = !_wcsicmp(buf, L"Low") || !wcscmp(buf, L"0") ? 0 : !_wcsicmp(buf, L"High") || !wcscmp(buf, L"2") ? 2 : 1;
        swprintf(k, 32, L"P%dColor", i + 2); GetPrivateProfileStringW(L"Bots", k, L"Default", buf, 128, ini);
        C.botColor[i] = _wcsicmp(buf, L"Auto") && _wcsicmp(buf, L"Default") ? clampi(_wtoi(buf), 0, 64) : 0;   /* m7.1: "Default" (old inis: "Auto") */
    }
    /* m7 sequential locking: P3 needs P2 ON, P4 needs P3 ON (an older ini could say otherwise) */
    for (int i = 1; i < 3; i++) if (C.botOn[i] && !C.botOn[i - 1]) { C.botOn[i] = 0; C.botNormalized |= 1 << i; }
    C.overlay = GetPrivateProfileIntW(L"Debug", L"Overlay", 1, ini) != 0;
    C.startActive = GetPrivateProfileIntW(L"Bots", L"StartActive", 0, ini) != 0;
    GetPrivateProfileStringW(L"Bots", L"AttachMode", C.startActive ? L"Auto" : L"OnF7", buf, 128, ini);
    C.attachOnF7 = _wcsicmp(buf, L"Auto") != 0;
    C.attachDelayMs = clampi(GetPrivateProfileIntW(L"Bots", L"AttachDelayMs", 2000, ini), 0, 60000);
    C.settleMs = clampi(GetPrivateProfileIntW(L"Bots", L"SettleMs", 1500, ini), 0, 30000);
    C.reorderLatePads = GetPrivateProfileIntW(L"Bots", L"ReorderLatePads", 0, ini) != 0;
    C.legacyDrivers = GetPrivateProfileIntW(L"Input", L"LegacyInputDrivers", 1, ini) != 0;
    C.hideDuplicatePads = GetPrivateProfileIntW(L"Input", L"HideDuplicatePads", 1, ini) != 0;

    GetPrivateProfileStringW(L"Brain", L"Mode", L"Smart", buf, 128, ini);
    C.smart = _wcsicmp(buf, L"Test") != 0;
    GetPrivateProfileStringW(L"Brain", L"AttackButton", L"X", buf, 128, ini); C.attackButton = parse_button(buf, BTN_X);
    GetPrivateProfileStringW(L"Brain", L"JumpButton", L"A", buf, 128, ini); C.jumpButton = parse_button(buf, BTN_A);
    GetPrivateProfileStringW(L"Brain", L"SpecialButton", L"Y", buf, 128, ini); C.specialButton = parse_button(buf, BTN_Y);
    C.useSpecials = GetPrivateProfileIntW(L"Brain", L"UseSpecials", 1, ini) != 0;
    C.specialMinLifePct = clampi(GetPrivateProfileIntW(L"Brain", L"SpecialMinLifePercent", 60, ini), 0, 101);
    C.retreatLifePct = clampi(GetPrivateProfileIntW(L"Brain", L"RetreatLifePercent", 30, ini), 0, 100);
    C.attackRange = ini_float(L"Brain", L"AttackRange", ini);
    C.laneTolerance = ini_float(L"Brain", L"LaneTolerance", ini);
    C.followDistance = ini_float(L"Brain", L"FollowDistance", ini);
    C.comboHits = clampi(GetPrivateProfileIntW(L"Brain", L"ComboHits", 4, ini), 1, 10);
    C.comboTapMs = clampi(GetPrivateProfileIntW(L"Brain", L"ComboTapMs", 110, ini), 50, 1000);
    C.comboPauseMs = clampi(GetPrivateProfileIntW(L"Brain", L"ComboPauseMs", 350, ini), 0, 5000);
    /* m4 */
    C.defSpecialMinLifePct = clampi(GetPrivateProfileIntW(L"Brain", L"DefensiveSpecialMinLifePercent", 40, ini), 0, 101);
    GetPrivateProfileStringW(L"Brain", L"StarButton", L"Auto", buf, 128, ini);
    if (!_wcsicmp(buf, L"Y+B") || !_wcsicmp(buf, L"YB")) { C.starMode = 2; C.starButton = BTN_RB; }
    else if (!_wcsicmp(buf, L"Auto")) { C.starMode = 0; C.starButton = BTN_RB; }
    else { C.starMode = 1; C.starButton = parse_button(buf, BTN_RB); }
    if (C.starButton == BTN_BACK || C.starButton == BTN_GUIDE || C.starButton == BTN_START) C.starButton = BTN_RB;
    GetPrivateProfileStringW(L"Brain", L"BackAttack", L"RT", buf, 128, ini);
    C.backAttack = !_wcsicmp(buf, L"Off") || !wcscmp(buf, L"0") ? 0 : !_wcsicmp(buf, L"BackX") ? 2 : 1;
    C.useStars = GetPrivateProfileIntW(L"Brain", L"UseStarMoves", 1, ini) != 0;
    C.usePickups = GetPrivateProfileIntW(L"Brain", L"UsePickups", 1, ini) != 0;
    C.useGrabs = GetPrivateProfileIntW(L"Brain", L"UseGrabs", 1, ini) != 0;
    C.foodLifePct = clampi(GetPrivateProfileIntW(L"Brain", L"FoodLifePercent", 75, ini), 0, 100);
    C.itemHeadStartMs = 1000 * clampi(GetPrivateProfileIntW(L"Brain", L"PlayerItemHeadStartSec", 10, ini), 0, 600);   /* m5 */
    C.breakObjects = GetPrivateProfileIntW(L"Brain", L"BreakObjects", 1, ini) != 0;
    C.weaponThrows = GetPrivateProfileIntW(L"Brain", L"WeaponThrows", 1, ini) != 0;   /* m7.2 */
    C.friendlyRetaliate = GetPrivateProfileIntW(L"Brain", L"FriendlyRetaliate", 1, ini) != 0;   /* m7.4 */
    C.jumpAttacks = GetPrivateProfileIntW(L"Brain", L"JumpAttacks", 1, ini) != 0;

    GetPrivateProfileStringW(L"TestBrain", L"JoinButton", L"A", buf, 128, ini); C.joinButton = parse_button(buf, BTN_A);
    C.joinPresses = clampi(GetPrivateProfileIntW(L"TestBrain", L"JoinPresses", 2, ini), 0, 10);
    GetPrivateProfileStringW(L"TestBrain", L"AttackButton", L"X", buf, 128, ini); C.testAttackButton = parse_button(buf, BTN_X);
    C.attackIntervalMs = clampi(GetPrivateProfileIntW(L"TestBrain", L"AttackIntervalMs", 700, ini), 150, 10000);
    C.walkMs = clampi(GetPrivateProfileIntW(L"TestBrain", L"WalkMs", 1500, ini), 100, 20000);
    C.jumpEvery = clampi(GetPrivateProfileIntW(L"TestBrain", L"JumpEveryNAttacks", 0, ini), 0, 100);

    C.dumpState = GetPrivateProfileIntW(L"Debug", L"DumpState", 1, ini) != 0;
    C.dumpEveryNFrames = clampi(GetPrivateProfileIntW(L"Debug", L"DumpEveryNFrames", 120, ini), 1, 100000);
    C.brainLogMs = clampi(GetPrivateProfileIntW(L"Debug", L"BrainLogEveryMs", 1000, ini), 0, 600000);
    C.p1WatchMs = clampi(GetPrivateProfileIntW(L"Debug", L"P1WatchLogEveryMs", 1000, ini), 0, 600000);
    C.fallbackTest = GetPrivateProfileIntW(L"Brain", L"FallbackToTestPattern", 0, ini) ? 1 : 0;
    C.menuConfirmMs = clampi(GetPrivateProfileIntW(L"Brain", L"MenuConfirmSeconds", 10, ini), 0, 120) * 1000;
    C.menuConfirmMaxTaps = clampi(GetPrivateProfileIntW(L"Brain", L"MenuConfirmMaxTaps", 6, ini), 0, 20);
    C.focusGraceMs = clampi(GetPrivateProfileIntW(L"Brain", L"FocusGraceMs", 1500, ini), 0, 10000);
    C.resumeGraceMs = clampi(GetPrivateProfileIntW(L"Brain", L"ResumeGraceMs", 750, ini), 0, 10000);
    g_testAssumeFocus = GetPrivateProfileIntW(L"Debug", L"TestAssumeFocus", 0, ini) != 0;
    C.retreatMaxMs = clampi(GetPrivateProfileIntW(L"Brain", L"RetreatMaxMs", 2500, ini), 0, 60000);
}

/* ---------------- real SDL ---------------- */
static int load_real(void) {
    if (R.mod) return 1;
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%lsSDL2_real.dll", g_dir);
    R.mod = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);   /* full path only */
    if (!R.mod) { LOGE("Could not load %ls (error %lu). The mod is not installed correctly.", path, GetLastError()); return 0; }
#define GP(field, name) R.field = (void *)GetProcAddress(R.mod, name)
    GP(Init, "SDL_Init"); GP(InitSubSystem, "SDL_InitSubSystem"); GP(Quit, "SDL_Quit");
    GP(QuitSubSystem, "SDL_QuitSubSystem"); GP(PollEvent, "SDL_PollEvent"); GP(GetVersion, "SDL_GetVersion");
    GP(AttachVirtualEx, "SDL_JoystickAttachVirtualEx"); GP(DetachVirtual, "SDL_JoystickDetachVirtual");
    GP(JoystickOpen, "SDL_JoystickOpen"); GP(JoystickClose, "SDL_JoystickClose");
    GP(SetVirtualAxis, "SDL_JoystickSetVirtualAxis"); GP(SetVirtualButton, "SDL_JoystickSetVirtualButton");
    GP(NumJoysticks, "SDL_NumJoysticks"); GP(IsGameController, "SDL_IsGameController");
    GP(JoystickIsVirtual, "SDL_JoystickIsVirtual"); GP(DevInstanceID, "SDL_JoystickGetDeviceInstanceID");
    GP(DevType, "SDL_JoystickGetDeviceType"); GP(DevPlayerIndex, "SDL_JoystickGetDevicePlayerIndex");
    GP(JoystickNameForIndex, "SDL_JoystickNameForIndex"); GP(JoystickPathForIndex, "SDL_JoystickPathForIndex");
    GP(ControllerNameForIndex, "SDL_GameControllerNameForIndex");
    GP(GetDeviceGUID, "SDL_JoystickGetDeviceGUID"); GP(GUIDString, "SDL_JoystickGetGUIDString");
    GP(AddMapping, "SDL_GameControllerAddMapping"); GP(GetError, "SDL_GetError");
    GP(CurrentVideoDriver, "SDL_GetCurrentVideoDriver"); GP(GetHint, "SDL_GetHint"); GP(GetKeyboardState, "SDL_GetKeyboardState"); GP(GetKeyboardFocus, "SDL_GetKeyboardFocus"); GP(GetWindowFlags, "SDL_GetWindowFlags");
    GP(JoystickInstanceID, "SDL_JoystickInstanceID");
    GP(GCGetButton, "SDL_GameControllerGetButton"); GP(GCGetAxis, "SDL_GameControllerGetAxis");
    GP(GCFromID, "SDL_GameControllerFromInstanceID"); GP(GCGetJoystick, "SDL_GameControllerGetJoystick");
    GP(DevVendor, "SDL_JoystickGetDeviceVendor"); GP(SetHint, "SDL_SetHint"); GP(DevProduct, "SDL_JoystickGetDeviceProduct");
#undef GP
    SDL_version v = {0};
    if (R.GetVersion) R.GetVersion(&v);
    LOGI("Real SDL loaded: %ls, version %d.%d.%d", path, v.major, v.minor, v.patch);
    return 1;
}
static int virtual_supported(void) {
    return R.AttachVirtualEx && R.DetachVirtual && R.JoystickOpen && R.JoystickClose && R.SetVirtualAxis &&
           R.SetVirtualButton && R.NumJoysticks && R.IsGameController && R.DevInstanceID;
}

/* ---------------- bots ----------------
 * m3: up to three bots, one per player row P2, P3, P4 of the F7 menu. Bot i always uses g_bots[i] and
 * g_brains[i]. Its pad is plugged in only while it is ON (and after the first F7 of the session); it
 * joins through the game's own "press A on a free controller" rule, so it gets the first FREE player
 * slot and never a slot a human already has. */
#define NBOT 3
typedef struct {
    int slot, instanceId, sdlIndex;
    SDL_Joystick *joy;
    PadOut out;
    char name[32];
    int keptLogged;
} Bot;
static Bot g_bots[NBOT];
static BotBrain g_brains[NBOT];
static int g_armed, g_joyReady, g_warnedNoVirtual, g_menuOpen;
static ULONGLONG g_joyInitAt, g_lastHumanAddAt, g_reattachAt, g_lastTick, g_lastFallbackWarn, g_nextAttachAt;
static int g_humanPadsSeen, g_steamVirtualSeen, g_keyboardSeen, g_reattachPending, g_noRoomLogged[NBOT];
static SDL_Event g_held; static int g_holding, g_heldInst; static ULONGLONG g_holdSince;
static int g_pendingRemove[NBOT], g_nPendingRemove;  /* bot instances whose removal the game hasn't seen yet */
static int g_fnaSlot[4] = { -1, -1, -1, -1 };      /* predicted FNA gamepad index -> SDL instance id */
static World g_world; static uint64_t g_frame; static int g_stateInit;
/* every controller the game has been told about (dedupe + phantom-duplicate filter) */
typedef struct { int inst, delivered, hidden, ours; Uint16 vid, pid; char drv; ULONGLONG at; } DevRec;
static DevRec g_dev[32]; static int g_ndev;
static SlotInfo g_lastSlots[8]; static int g_lastNSlot = -1;

static int is_bot_instance(int inst) {
    for (int i = 0; i < NBOT; i++) if (g_bots[i].joy && g_bots[i].instanceId == inst) return i;
    return -1;
}
static int bots_attached(void) { int n = 0; for (int i = 0; i < NBOT; i++) n += g_bots[i].joy != NULL; return n; }
/* m7: with the F7 menu open, bots act only at the game's character select (live picking, see sor4brain.c);
   a bot that was turned OFF there may still press B to leave its slot (back-out) */
static int bot_may_act(int i) { return g_armed && ((C.botOn[i] && (!g_menuOpen || g_world.charSel)) || g_brains[i].backout); }
static void neutral_out(PadOut *o) { memset(o, 0, sizeof *o); o->axis[AX_LT] = o->axis[AX_RT] = SDL_JOYSTICK_AXIS_MIN; }

static void apply_out(Bot *b) {
    if (!b->joy) return;
    for (int i = 0; i < BTN_COUNT; i++) R.SetVirtualButton(b->joy, i, b->out.btn[i]);
    for (int i = 0; i < AX_COUNT; i++) R.SetVirtualAxis(b->joy, i, b->out.axis[i]);
}
static void neutral_all(const char *why) {      /* release every bot button and stick right now */
    int n = 0;
    for (int i = 0; i < NBOT; i++) if (g_bots[i].joy) { neutral_out(&g_bots[i].out); apply_out(&g_bots[i]); n++; }
    if (why && n) LOGD("bot pads set to neutral: %s", why);
}
static int g_focusNow = 1;
static void vj_update(void *userdata) {         /* SDL main thread, from SDL_JoystickUpdate */
    Bot *b = (Bot *)userdata;
    if (!b->joy) return;
    /* watchdog: if our per-frame tick hasn't run for 250 ms (stall, loading, minimised), don't keep
       holding whatever the brain last pressed; never press anything while the game window is not focused,
       while the F7 menu is open, or while this bot is off */
    if (!bot_may_act((int)(b - g_bots)) || !g_focusNow || GetTickCount64() - g_lastTick > 250) neutral_out(&b->out);
    for (int i = 0; i < BTN_COUNT; i++) R.SetVirtualButton(b->joy, i, b->out.btn[i]);
    for (int i = 0; i < AX_COUNT; i++) R.SetVirtualAxis(b->joy, i, b->out.axis[i]);
}

static const char *guid_driver(const SDL_JoystickGUID *g) {
    switch (g->data[14]) {
    case 'h': return "HIDAPI"; case 'r': return "RawInput"; case 'v': return "virtual"; case 'w': return "WGI";
    case 'x': return "XInput"; default: return "DirectInput/other";
    }
}
static void describe_device(int idx, const char *what) {
    char guid[64] = "?";
    SDL_JoystickGUID g; memset(&g, 0, sizeof g);
    if (R.GetDeviceGUID && R.GUIDString) { g = R.GetDeviceGUID(idx); R.GUIDString(g, guid, sizeof guid); }
    int inst = R.DevInstanceID ? R.DevInstanceID(idx) : -1;
    Uint16 vid = R.DevVendor ? R.DevVendor(idx) : 0, pid = R.DevProduct ? R.DevProduct(idx) : 0;
    const char *nm = R.JoystickNameForIndex ? R.JoystickNameForIndex(idx) : NULL;
    const char *cn = R.ControllerNameForIndex ? R.ControllerNameForIndex(idx) : NULL;
    const char *path = R.JoystickPathForIndex ? R.JoystickPathForIndex(idx) : NULL;
    int ours = is_bot_instance(inst) >= 0;
    LOGI("%s: index %d, instance %d, \"%s\" (controller name \"%s\"), VID/PID %04X:%04X, GUID %s, driver %s, type %d, "
         "SDL player index %d, path %s, game controller %s, virtual %s, %s",
         what, idx, inst, nm ? nm : "?", cn ? cn : "-", vid, pid, guid, guid_driver(&g), R.DevType ? R.DevType(idx) : -1,
         R.DevPlayerIndex ? R.DevPlayerIndex(idx) : -1, path ? path : "-", R.IsGameController(idx) ? "yes" : "no",
         R.JoystickIsVirtual && R.JoystickIsVirtual(idx) ? "yes" : "no", ours ? "OURS (bot pad)" : "not ours (human/other)");
    if (vid == 0x28DE && !g_steamVirtualSeen) { g_steamVirtualSeen = 1; LOGI("Steam Input virtual controller seen: Steam Input is active for this game."); }
}
static void log_fna_slots(const char *why) {
    char b[200]; int k = 0;
    for (int i = 0; i < 4; i++) {
        int bi = g_fnaSlot[i] >= 0 ? is_bot_instance(g_fnaSlot[i]) : -1;
        k += snprintf(b + k, sizeof b - k, " [%d]=%s", i, g_fnaSlot[i] < 0 ? "free" : bi >= 0 ? g_bots[bi].name : "human pad");
        if (g_fnaSlot[i] >= 0 && bi < 0) k += snprintf(b + k, sizeof b - k, "(inst %d)", g_fnaSlot[i]);
    }
    LOGI("game pad slots (predicted from FNA's first-free rule, %s):%s", why, b);
}

static int count_real_controllers(void) {
    int n = R.NumJoysticks(), real = 0;
    for (int i = 0; i < n; i++) {
        int isVirt = R.JoystickIsVirtual ? R.JoystickIsVirtual(i) : 0;
        if (!isVirt && R.IsGameController(i)) real++;
    }
    return real;
}

static int attach_one(int i, const char *why) {
    if (!virtual_supported()) {
        if (!g_warnedNoVirtual) LOGE("SDL2_real.dll has no virtual joystick support (needs SDL 2.24+). Bots disabled; the game runs normally.");
        g_warnedNoVirtual = 1; return 0;
    }
    Bot *b = &g_bots[i];
    if (b->joy) return 1;
    int real = count_real_controllers(), mine = bots_attached();
    if (real + mine >= 4) {
        if (!g_noRoomLogged[i]) { g_noRoomLogged[i] = 1; LOGI("P%d bot: not plugging in a pad: %d real controller(s) and %d bot pad(s) already use all 4 player slots.", i + 2, real, mine); }
        return 0;
    }
    g_noRoomLogged[i] = 0;
    memset(b, 0, sizeof *b);
    b->slot = i + 2;
    snprintf(b->name, sizeof b->name, "SoR4Bots P%d", b->slot);
    neutral_out(&b->out);
    SDL_VirtualJoystickDesc d; memset(&d, 0, sizeof d);
    d.version = 1; d.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    d.naxes = AX_COUNT; d.nbuttons = BTN_COUNT; d.nhats = 0;
    d.vendor_id = 0x045E; d.product_id = 0x028E;           /* looks like an Xbox 360 pad */
    d.button_mask = (1u << BTN_COUNT) - 1; d.axis_mask = (1u << AX_COUNT) - 1;
    d.name = b->name; d.userdata = b; d.Update = vj_update;
    int idx = R.AttachVirtualEx(&d);
    if (idx < 0) { LOGE("Attach failed for %s: %s", b->name, R.GetError ? R.GetError() : "?"); return 0; }
    b->joy = R.JoystickOpen(idx);
    if (!b->joy) { LOGE("Open failed for %s: %s", b->name, R.GetError ? R.GetError() : "?"); R.DetachVirtual(idx); return 0; }
    b->instanceId = R.JoystickInstanceID ? R.JoystickInstanceID(b->joy) : R.DevInstanceID(idx);
    b->sdlIndex = idx;
    for (int k = 0; k < BTN_COUNT; k++) R.SetVirtualButton(b->joy, k, 0);
    for (int k = 0; k < AX_COUNT; k++) R.SetVirtualAxis(b->joy, k, b->out.axis[k]);
    if (!R.IsGameController(idx) && R.GetDeviceGUID && R.GUIDString && R.AddMapping) {
        char guid[64], map[512];
        R.GUIDString(R.GetDeviceGUID(idx), guid, sizeof guid);
        snprintf(map, sizeof map, "%s,%s,a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,start:b6,leftstick:b7,"
                 "rightstick:b8,leftshoulder:b9,rightshoulder:b10,dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,"
                 "leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,", guid, b->name);
        LOGI("Adding controller mapping for %s (result %d)", b->name, R.AddMapping(map));
    }
    LOGI("Attached %s (%s) as device index %d, instance %d (controller=%d); %d real controller(s) present.", b->name, why, idx, b->instanceId, R.IsGameController(idx), real);
    int fna = -1, pending = 0;            /* first FNA slot not taken and not promised to another new bot pad */
    for (int k = 0; k < NBOT; k++) if (k != i && g_bots[k].joy) { int seen = 0; for (int s = 0; s < 4; s++) seen |= g_fnaSlot[s] == g_bots[k].instanceId; if (!seen) pending++; }
    for (int s = 0, f = 0; s < 4; s++) if (g_fnaSlot[s] < 0) { if (f++ == pending) { fna = s; break; } }
    ULONGLONG t = GetTickCount64();
    brain_summary(&g_brains[i], "bot plugged in again");
    brain_reset(&g_brains[i], i, NBOT, b->slot, b->name, t);
    brain_attached(&g_brains[i], fna, idx, g_stateInit ? &g_world : NULL, t);
    return 1;
}
static void detach_one(int i, const char *why) {
    Bot *b = &g_bots[i];
    if (!b->joy) return;
    LOGI("Removing %s: %s", b->name, why);
    int n = R.NumJoysticks(), idx = -1;
    for (int k = 0; k < n; k++) if (R.DevInstanceID(k) == b->instanceId) { idx = k; break; }
    brain_summary(&g_brains[i], "bot unplugged");
    R.JoystickClose(b->joy); b->joy = NULL;
    if (idx >= 0 && R.DetachVirtual(idx) == 0 && g_nPendingRemove < NBOT) g_pendingRemove[g_nPendingRemove++] = b->instanceId;
    brain_init_detached(&g_brains[i], i, b->name);
}
static void detach_bots(const char *why) { for (int i = NBOT - 1; i >= 0; i--) detach_one(i, why); }
static void arm(const char *why) {
    if (g_armed) return;
    g_armed = 1;
    LOGI("Bots armed (%s): bots that are ON in the F7 menu may now plug in and play.", why);
    state_set_wanted(1);
}
/* every frame: plug in pads of bots that are ON, unplug pads of bots that are OFF when that is safe */
static ULONGLONG g_boStart[NBOT];
static void manage_bots(ULONGLONG now) {
    if (!g_joyReady || g_holding || g_warnedNoVirtual) return;
    if (g_reattachPending && now < g_reattachAt) return;
    g_reattachPending = 0;
    for (int i = 0; i < NBOT; i++) {
        Bot *b = &g_bots[i]; BotBrain *br = &g_brains[i];
        int want = g_armed && C.botOn[i];
        if (want && br->backout) { LOGI("%s: turned on again; back-out stopped.", b->name); br->backout = br->backoutDone = 0; br->boSince = 0; }
        if (want && !b->joy) {
            if (now < g_nextAttachAt) continue;
            g_nextAttachAt = now + 400;                      /* one pad at a time */
            attach_one(i, "bot turned on");
            continue;
        }
        if (want || !b->joy) { b->keptLogged = 0; continue; }
        /* OFF but still plugged in. Unplugging frees its player slot for a real player; the game handles
           that cleanly at character select (an unplugged pad leaves the select), and when the bot never
           got a slot. During a stage the pad stays (neutral) so the game doesn't pause for a lost pad. */
        int inSlot = 0;
        for (int s = 0; s < g_world.nslot; s++) if (br->id.ctrlId >= 0 && g_world.slot[s].ctrl == br->id.ctrlId) inSlot = 1;
        /* m7: at character select a bot in P2..P4 first presses B to leave its slot (top-down: P4, then P3,
           then P2), at most 5 s; then (or if B did nothing) its pad is unplugged, which frees the slot too */
        if (g_world.charSel && g_world.menuRead && inSlot && br->playerSlot > 0 && state_available()) {
            if (!br->backout) {
                int later = 0;
                for (int j = i + 1; j < NBOT; j++) if (g_bots[j].joy && !C.botOn[j]) later = 1;
                if (later) continue;                             /* a later bot backs out first */
                br->backout = 1; br->backoutDone = 0; br->boSince = 0; g_boStart[i] = now;
            }
            if (!br->backoutDone && now - g_boStart[i] < 5000) continue;
            detach_one(i, br->backoutDone ? "bot turned off at character select (backed out)" : "bot turned off at character select (back-out timed out)");
            b->keptLogged = 0; continue;
        }
        const char *why = br->id.ctrlId < 0 ? "bot turned off (it had not joined)" : !inSlot && state_available() ? "bot turned off (not in a player slot)" :
                          g_world.charSel ? "bot turned off at character select (its player slot is free again)" :
                          g_world.menuRead && !g_world.ok && !strcmp(g_world.menuName, "MainMenu") ? "bot turned off (main menu)" : NULL;
        if (why) { detach_one(i, why); b->keptLogged = 0; }
        else if (!b->keptLogged) { b->keptLogged = 1; LOGI("%s: turned off; its pad stays plugged in (idle) until character select or the main menu, so the game doesn't pause for a lost controller.", b->name); }
    }
}

static void log_input_environment(void) {
    static const char *hints[] = { "SDL_JOYSTICK_HIDAPI", "SDL_JOYSTICK_HIDAPI_XBOX", "SDL_JOYSTICK_HIDAPI_XBOX_ONE",
        "SDL_JOYSTICK_HIDAPI_PS4", "SDL_JOYSTICK_HIDAPI_PS5", "SDL_JOYSTICK_HIDAPI_SWITCH", "SDL_JOYSTICK_HIDAPI_STEAM",
        "SDL_JOYSTICK_RAWINPUT", "SDL_JOYSTICK_RAWINPUT_CORRELATE_XINPUT", "SDL_JOYSTICK_WGI", "SDL_XINPUT_ENABLED",
        "SDL_DIRECTINPUT_ENABLED", "SDL_JOYSTICK_THREAD", "SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS",
        "SDL_GAMECONTROLLER_IGNORE_DEVICES", "SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT", "SDL_GAMECONTROLLER_USE_BUTTON_LABELS",
        "SDL_GAMECONTROLLERCONFIG", "SDL_GAMECONTROLLERCONFIG_FILE" };
    for (size_t i = 0; i < sizeof hints / sizeof *hints; i++) {
        const char *v = R.GetHint ? R.GetHint(hints[i]) : NULL;
        LOGI("hint %s = %s", hints[i], v ? v : "(default)");
    }
    static const char *env[] = { "SteamVirtualGamepadInfo", "SDL_GAMECONTROLLER_IGNORE_DEVICES", "SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT",
        "SDL_GAMECONTROLLERCONFIG", "SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD", "SteamAppId", "SteamGameId",
        "SteamClientLaunch", "SteamOverlayGameId", "SteamTenfoot", "SteamDeck", "SteamEnv" };
    int steamInput = 0;
    for (size_t i = 0; i < sizeof env / sizeof *env; i++) {
        char v[512]; DWORD n = GetEnvironmentVariableA(env[i], v, sizeof v);
        if (n >= sizeof v) { strcpy(v + sizeof v - 5, "..."); }
        LOGI("env %s = %s", env[i], n ? v : "(not set)");
        if (n && (!strcmp(env[i], "SteamVirtualGamepadInfo") || !strcmp(env[i], "SDL_GAMECONTROLLER_IGNORE_DEVICES"))) steamInput = 1;
    }
    LOGI("Steam Input: %s", steamInput ? "looks ACTIVE (Steam set its controller variables for this game)" : "no sign of it in the environment");
    LOGI("input drivers in SDL 2.32 on Windows: HIDAPI, RawInput, Windows.Gaming.Input (WGI), XInput, DirectInput; "
         "each device line below shows which one reported it.");
}

static void after_init(Uint32 flags, int rc) {
    if (rc != 0 || !(flags & (SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER)) || g_joyReady) return;
    g_joyReady = 1; g_joyInitAt = GetTickCount64();
    log_input_environment();
    int on = C.botOn[0] + C.botOn[1] + C.botOn[2];
    LOGI("Press F7 in the game to open the bots menu. Bots ON from last time: %d (P2 %s, P3 %s, P4 %s).", on,
         C.botOn[0] ? "on" : "off", C.botOn[1] ? "on" : "off", C.botOn[2] ? "on" : "off");
    if (C.attachOnF7) LOGI("Bot pads are plugged in only after you first press F7 in this session (AttachMode=OnF7), so your own controllers always come first.");
    else LOGI("AttachMode=Auto: bots that are ON plug in once real controllers have settled (%d ms after the last one appears, at least %d ms after start), or after %d s with none.",
              C.settleMs, C.attachDelayMs, C.inputFallbackSeconds);
}
static void auto_attach_check(ULONGLONG now) {
    if (!g_joyReady || g_armed || C.attachOnF7 || g_warnedNoVirtual || g_holding) return;
    if (now < g_joyInitAt + (ULONGLONG)C.attachDelayMs) return;
    int settled = g_humanPadsSeen && now - g_lastHumanAddAt >= (ULONGLONG)C.settleMs;
    int timeout = !g_humanPadsSeen && (C.inputFallbackSeconds == 0 || now >= g_joyInitAt + (ULONGLONG)C.inputFallbackSeconds * 1000);
    if (!settled && !timeout) return;
    arm(settled ? "AttachMode=Auto, real controllers settled" : "AttachMode=Auto, no real controller appeared");
}

/* ---------------- F7 menu ----------------
 * F7 opens/closes the menu (seen two ways, each press handled once: the game's key events through our
 * SDL_PollEvent hook, and the keyboard state polled once per frame while the game window has focus).
 * While it is open: every bot pad is neutral, the human pads read as "nothing pressed" by the game
 * (SDL_GameControllerGetButton/GetAxis hooks, controller events dropped), and key presses go to the menu
 * instead of the game (key-down and text events dropped; key-up events always pass). */
static MenuModel g_menu;
static void *g_gameHwnd;
static int g_f7By, g_f7PrevDown, g_f7PollLogged;     /* g_f7By: 0 = no press open, 1 = event toggled, 2 = poll toggled, 3 = both saw it */
/* human pads: nav repeat while open; buttons and sticks held at close stay hidden from the game until released */
typedef struct { SDL_GameController *gc; uint32_t btnLatch; uint8_t axLatch; } PadLatch;
static PadLatch g_latch[8]; static int g_nlatch; static volatile int g_latchAny;
static int g_navHeld[MK_CLOSE + 1]; static ULONGLONG g_navNext[MK_CLOSE + 1];
static uint8_t g_keySwallowed[512];                  /* key-downs eaten by the menu: their repeats stay eaten until key-up */

static int human_pads(SDL_GameController **out, int max) {
    int n = 0;
    if (!R.GCFromID) return 0;
    for (int i = 0; i < g_ndev; i++) {
        int inst = g_dev[i].inst;
        if (!g_dev[i].delivered || g_dev[i].hidden || inst < 0 || is_bot_instance(inst) >= 0) continue;
        SDL_GameController *gc = R.GCFromID(inst);
        if (gc && n < max) out[n++] = gc;
    }
    return n;
}
static void menu_sync_from_config(void) {
    for (int i = 0; i < MENU_BOTS; i++) {
        g_menu.bot[i].on = C.botOn[i]; g_menu.bot[i].color = C.botColor[i];
        snprintf(g_menu.bot[i].character, sizeof g_menu.bot[i].character, "%s", C.botChar[i][0] ? C.botChar[i] : "Auto");
    }
}
static void menu_apply_to_config(void) {
    for (int i = 0; i < MENU_BOTS; i++) {
        if (C.botOn[i] != g_menu.bot[i].on) LOGI("Menu: P%d bot %s.", i + 2, g_menu.bot[i].on ? "ON" : "OFF");
        if (strcmp(C.botChar[i], g_menu.bot[i].character)) LOGI("Menu: P%d bot character %s.", i + 2, g_menu.bot[i].character);
        if (C.botColor[i] != g_menu.bot[i].color) { if (g_menu.bot[i].color) LOGI("Menu: P%d bot color %d.", i + 2, g_menu.bot[i].color); else LOGI("Menu: P%d bot color Default.", i + 2); }
        C.botOn[i] = g_menu.bot[i].on; C.botColor[i] = g_menu.bot[i].color;
        snprintf(C.botChar[i], sizeof C.botChar[i], "%s", g_menu.bot[i].character);
        snprintf(g_brains[i].wantChar, sizeof g_brains[i].wantChar, "%s", C.botChar[i]);
        g_brains[i].wantColor = C.botColor[i];
    }
}
static void save_choices(void) {
    wchar_t ini[MAX_PATH], k[32], v[64]; int ok = 1;
    swprintf(ini, MAX_PATH, L"%lsSoR4Bots.ini", g_dir);
    for (int i = 0; i < 3; i++) {
        swprintf(k, 32, L"P%dBot", i + 2); ok &= WritePrivateProfileStringW(L"Bots", k, C.botOn[i] ? L"1" : L"0", ini) != 0;
        swprintf(k, 32, L"P%dCharacter", i + 2); swprintf(v, 64, L"%hs", C.botChar[i][0] ? C.botChar[i] : "Auto");
        ok &= WritePrivateProfileStringW(L"Bots", k, v, ini) != 0;
        swprintf(k, 32, L"P%dColor", i + 2);
        if (C.botColor[i]) swprintf(v, 64, L"%d", C.botColor[i]); else swprintf(v, 64, L"Default");
        ok &= WritePrivateProfileStringW(L"Bots", k, v, ini) != 0;
    }
    if (ok) LOGI("Menu choices saved to SoR4Bots.ini."); else LOGE("Could not save the menu choices to SoR4Bots.ini (error %lu).", GetLastError());
}
static const char *where_text(void) {
    static char buf[64];
    if (!state_available()) return "unknown (game data not readable)";
    if (g_world.paused) return "paused";
    if (g_world.ok && g_world.nch > 0) return "in a stage";
    if (g_world.charSel) return "character select";
    const char *m = g_world.menuName;
    if (!g_world.menuRead) return "menu";
    if (!m[0]) return g_world.ok ? "in a stage" : "loading";
    if (!strcmp(m, "MainMenu")) return "main menu";
    if (!strcmp(m, "CharacterSelection")) return "character select (opening)";
    if (!strcmp(m, "Loading")) return "loading";
    if (!strcmp(m, "Cutscene")) return "cutscene";
    if (!strcmp(m, "StageSummary")) return "stage results";
    snprintf(buf, sizeof buf, "menu (%s)", m); return buf;
}
static void menu_locks(void) {
    int joined[MENU_BOTS];
    for (int i = 0; i < MENU_BOTS; i++) joined[i] = g_bots[i].joy && g_brains[i].playerSlot >= 0;
    int before[MENU_BOTS]; for (int i = 0; i < MENU_BOTS; i++) before[i] = g_menu.locked[i];
    menu_update_locks(&g_menu, joined);
    for (int i = 1; i < MENU_BOTS; i++) if (before[i] != g_menu.locked[i]) LOGI("Menu: P%d bot %s.", i + 2, g_menu.locked[i] ? g_menu.lockWhy[i] : "unlocked");
}
/* "P1 human  P2 bot  P3 free  P4 free" from the game's player slots */
static void slots_text(char *out, size_t n) {
    out[0] = 0;
    if (!state_available() || g_world.nslot <= 0) return;
    size_t k = 0;
    for (int s = 0; s < g_world.nslot && s < 4; s++) {
        int c = g_world.slot[s].ctrl; const char *who = "free";
        if (c >= 0) { who = "human"; for (int i = 0; i < NBOT; i++) if (g_bots[i].joy && g_brains[i].id.ctrlId == c) who = "bot"; }
        k += (size_t)snprintf(out + k, k < n ? n - k : 0, "%sP%d %s", s ? "  " : "", s + 1, who);
        if (k >= n) break;
    }
}
static void menu_refresh(void) {
    if (!g_menuOpen) return;
    menu_locks();
    slots_text(g_menu.slots, sizeof g_menu.slots);
    const RosterEntry *r; int n = state_roster(&r);
    g_menu.nroster = n;
    for (int i = 0; i < n; i++) { snprintf(g_menu.roster[i], sizeof g_menu.roster[i], "%s", r[i].name); g_menu.palettes[i] = r[i].palettes; }
    snprintf(g_menu.where, sizeof g_menu.where, "%s", where_text());
    for (int i = 0; i < MENU_BOTS; i++) {
        const BotBrain *b = &g_brains[i]; char *nt = g_menu.note[i]; size_t nn = sizeof g_menu.note[i];
        nt[0] = 0;
        if (g_menu.bot[i].on) {
            if (!g_armed) snprintf(nt, nn, "(starts when the menu closes)");
            else if (!g_bots[i].joy) snprintf(nt, nn, g_noRoomLogged[i] ? "(no free player slot)" : "(starting)");
            else if (b->playerSlot >= 0) { if (b->playerSlot != i + 1) snprintf(nt, nn, "(playing as P%d)", b->playerSlot + 1); else snprintf(nt, nn, "(playing)"); }
            else snprintf(nt, nn, "(waiting to join)");
        } else if (g_bots[i].joy) snprintf(nt, nn, g_brains[i].backout ? "(leaving its slot)" : "(leaves at character select)");
        snprintf(g_menu.live[i], sizeof g_menu.live[i], "%s", g_world.charSel && g_bots[i].joy ? b->liveStatus : "");
    }
    if (C.overlay) overlay_show(g_gameHwnd, &g_menu);
}
static void log_renderer_once(void) {
    static int done; if (done) return; done = 1;
    LOGI("Renderer modules loaded: d3d11 %s, vulkan %s, opengl %s (FNA3D picks one; the menu is a separate overlay window either way).",
         GetModuleHandleW(L"d3d11.dll") ? "yes" : "no", GetModuleHandleW(L"vulkan-1.dll") ? "yes" : "no", GetModuleHandleW(L"opengl32.dll") ? "yes" : "no");
}
static void latch_human_pads(void) {
    SDL_GameController *gcs[8]; int n = human_pads(gcs, 8);
    g_nlatch = 0;
    for (int i = 0; i < n; i++) {
        PadLatch *l = &g_latch[g_nlatch++]; l->gc = gcs[i]; l->btnLatch = 0; l->axLatch = 0;
        for (int k = 0; k < BTN_COUNT; k++) if (R.GCGetButton(gcs[i], k)) l->btnLatch |= 1u << k;
        for (int k = 0; k < AX_COUNT; k++) { int v = R.GCGetAxis(gcs[i], k); if (v > 8000 || v < -8000) l->axLatch |= (uint8_t)(1u << k); }
    }
    g_latchAny = g_nlatch > 0;
}
static void menu_open(const char *how) {
    if (g_menuOpen) return;
    g_menuOpen = 1;
    menu_sync_from_config(); g_menu.changed = 0;
    neutral_all("menu opened");
    memset(g_navHeld, 0, sizeof g_navHeld);
    /* pad buttons already held when the menu opens must be released before they count */
    SDL_GameController *gcs[8]; int n = human_pads(gcs, 8);
    for (int i = 0; i < n; i++) {
        if (R.GCGetButton(gcs[i], BTN_A)) g_navHeld[MK_OK] = 1;
        if (R.GCGetButton(gcs[i], BTN_B)) g_navHeld[MK_CLOSE] = 1;
    }
    LOGI("Bots menu opened (%s). Game input is paused while it is open%s.", how, g_world.charSel ? "; at character select the bots follow the menu live" : "");
    log_renderer_once();
    if (!g_armed) arm("F7 menu opened");
    menu_refresh();
}
static void menu_close(const char *how) {
    if (!g_menuOpen) return;
    menu_apply_to_config();
    g_menuOpen = 0;
    latch_human_pads();
    if (C.overlay) overlay_hide();
    LOGI("Bots menu closed (%s).", how);
    if (g_menu.changed) save_choices();
    g_menu.changed = 0;
}
static void menu_key(int mk) {
    if (!g_menuOpen || mk == MK_NONE) return;
    menu_locks();                        /* m7: locked rows can't be selected or changed */
    g_menu.cascaded = 0;
    if (menu_nav(&g_menu, mk)) { menu_close(mk == MK_CLOSE ? "closed from the menu" : "Close"); return; }
    if (g_menu.cascaded) LOGI("Menu: turning a bot off also turned off the later bot(s):%s%s.", g_menu.cascaded & 2 ? " P3" : "", g_menu.cascaded & 4 ? " P4" : "");
    menu_apply_to_config();              /* ON/OFF and picks take effect at once */
    menu_refresh();
}
static void f7_toggle(const char *how) {
    if (g_menuOpen) menu_close(how); else menu_open(how);
}
static void f7_event(void) {
    if (g_f7By == 2) { g_f7By = 3; return; }          /* the keyboard-state poll already handled this press */
    g_f7By = 1; f7_toggle("F7");                       /* new press (a missed key-up must not swallow it) */
}
static void f7_poll(void) {
    if (!R.GetKeyboardState || !R.GetKeyboardFocus) return;
    int n = 0; const Uint8 *ks = R.GetKeyboardState(&n);
    int down = ks && n > SDL_SCANCODE_F7 && ks[SDL_SCANCODE_F7] && R.GetKeyboardFocus() != NULL;
    if (down && !g_f7PrevDown) {
        if (g_f7By == 0) {
            if (!g_f7PollLogged) { g_f7PollLogged = 1; LOGI("F7 noticed from the keyboard state before (or without) a key event."); }
            g_f7By = 2; f7_toggle("F7, keyboard state");
        } else if (g_f7By == 1) g_f7By = 3;
    }
    if (!down && g_f7PrevDown) g_f7By = 0;
    g_f7PrevDown = down;
}
/* menu keys from the game's key events: arrows, Enter/Space, Esc/Backspace */
static int key_to_mk(int sc) {
    switch (sc) {
    case 82: return MK_UP; case 81: return MK_DOWN; case 80: return MK_LEFT; case 79: return MK_RIGHT;
    case 40: case 88: case 44: return MK_OK; case 41: case 42: return MK_CLOSE;
    default: return MK_NONE;
    }
}
/* menu from the human pads: d-pad / left stick (repeat 400 ms, then every 120 ms), A = change/select, B or Start = close */
static void menu_pad_poll(ULONGLONG now) {
    SDL_GameController *gcs[8]; int n = human_pads(gcs, 8);
    int held[MK_CLOSE + 1] = { 0 };
    for (int i = 0; i < n; i++) {
        SDL_GameController *g = gcs[i];
        int lx = R.GCGetAxis(g, AX_LX), ly = R.GCGetAxis(g, AX_LY);
        held[MK_UP] |= R.GCGetButton(g, BTN_UP) || ly < -20000;
        held[MK_DOWN] |= R.GCGetButton(g, BTN_DOWN) || ly > 20000;
        held[MK_LEFT] |= R.GCGetButton(g, BTN_LEFT) || lx < -20000;
        held[MK_RIGHT] |= R.GCGetButton(g, BTN_RIGHT) || lx > 20000;
        held[MK_OK] |= R.GCGetButton(g, BTN_A);
        held[MK_CLOSE] |= R.GCGetButton(g, BTN_B) || R.GCGetButton(g, BTN_START);
    }
    for (int k = MK_UP; k <= MK_CLOSE && g_menuOpen; k++) {
        int fire = 0;
        if (held[k] && !g_navHeld[k]) { fire = 1; g_navNext[k] = now + 400; }
        else if (held[k] && k <= MK_RIGHT && now >= g_navNext[k]) { fire = 1; g_navNext[k] = now + 120; }
        g_navHeld[k] = held[k];
        if (fire) menu_key(k);
    }
}

/* ---------------- focus ----------------
 * Focused = SDL says our window has keyboard/input focus and is not minimised, and Windows' foreground
 * window belongs to this process. (SoR4 pauses itself when it loses focus; when you come back the pause
 * menu is open, which is where the m2.3 bot was tapping.) */
typedef void *(WINAPI *PFN_GetFg)(void); typedef DWORD (WINAPI *PFN_GetWTPid)(void *, DWORD *);
/* g_testAssumeFocus ([Debug] TestAssumeFocus=1): only for the windowless Wine host test */
static int game_focused(void) {
    if (g_testAssumeFocus || !R.GetKeyboardFocus) return 1;
    void *win = R.GetKeyboardFocus();
    if (!win) return 0;
    if (R.GetWindowFlags) { Uint32 f = R.GetWindowFlags(win); if (!(f & 0x200u) || (f & 0x40u)) return 0; }  /* INPUT_FOCUS, MINIMIZED */
    static PFN_GetFg fg; static PFN_GetWTPid wtp; static int tried;
    if (!tried) { tried = 1; HMODULE u = GetModuleHandleW(L"user32.dll"); if (u) { fg = (PFN_GetFg)(void *)GetProcAddress(u, "GetForegroundWindow"); wtp = (PFN_GetWTPid)(void *)GetProcAddress(u, "GetWindowThreadProcessId"); } }
    if (fg && wtp) { void *h = fg(); DWORD pid = 0; if (!h) return 0; wtp(h, &pid); if (pid != GetCurrentProcessId()) return 0; }
    return 1;
}

/* ---------------- per-frame work ---------------- */
static void frame_tick(ULONGLONG now) {
    static int wasOk, focusLogged = -1;
    if (g_joyReady) f7_poll();
    g_focusNow = game_focused();
    if (g_focusNow != focusLogged) {
        if (focusLogged >= 0 || !g_focusNow)
            LOGI(g_focusNow ? "Game window has focus again; bots wait %d ms before pressing anything." : "Game window lost focus; bot pads held neutral until it is back.", C.focusGraceMs);
        focusLogged = g_focusNow;
    }
    brain_set_focus(g_focusNow, now);
    if (g_focusNow) {          /* the game's window, for placing the menu overlay */
        typedef void *(WINAPI *PFN_Fg)(void); static PFN_Fg fg; static int t;
        if (!t) { t = 1; HMODULE u = GetModuleHandleW(L"user32.dll"); if (u) fg = (PFN_Fg)(void *)GetProcAddress(u, "GetForegroundWindow"); }
        if (fg && !g_testAssumeFocus) g_gameHwnd = fg();
    }
    if (g_menuOpen) {
        menu_pad_poll(now);
        if (!g_focusNow && C.overlay) overlay_hide();     /* alt-tab: don't float over other programs; back when focused */
    }
    if (!g_stateInit) return;
    if (state_available()) state_read(&g_world); else g_world.ok = 0;
    if (g_world.ok != wasOk) {
        wasOk = g_world.ok;
        if (wasOk) { LOGI("Level running: game state readable (via %s).", g_world.source); state_dump(&g_world, "level start"); }
        else LOGI("No level running (menu, loading or cutscene).");
    }
    g_frame++;
    /* who is P1..P4 and which controller each uses: log whenever it changes */
    if (g_world.nslot != g_lastNSlot || memcmp(g_world.slot, g_lastSlots, sizeof(SlotInfo) * (size_t)g_world.nslot)) {
        for (int i = 0; i < g_world.nslot; i++) {
            const SlotInfo *a = i < g_lastNSlot ? &g_lastSlots[i] : NULL, *b = &g_world.slot[i];
            if (!a || memcmp(a, b, sizeof *b))
                LOGI("SLOT CHANGE P%d: controllerId %d -> %d, slotType %d -> %d, status %d -> %d", i + 1,
                     a ? a->ctrl : -99, b->ctrl, a ? a->type : -99, b->type, a ? a->status : -99, b->status);
        }
        g_lastNSlot = g_world.nslot; memcpy(g_lastSlots, g_world.slot, sizeof g_lastSlots);
        state_log_slots(&g_world, "changed");
        log_fna_slots("player slots changed");
    }
    /* which fighter belongs to which player id, and who drives it */
    if (g_world.ok) {
        static uint32_t lastUid[8]; static int lastKind[8], seen[8];
        for (int i = 0; i < g_world.nch; i++) {
            const Chr *c = &g_world.ch[i];
            if (c->player < 0 || c->player >= 8) continue;
            if (!seen[c->player] || lastUid[c->player] != c->uid || lastKind[c->player] != c->ctrlKind) {
                const char *who = "human or unknown";
                for (int k = 0; k < NBOT; k++) if (g_brains[k].identified && g_brains[k].uid == c->uid) who = g_bots[k].name;
                LOGI("PLAYER CHANGE player id %d: fighter uid %u -> %u, controller %s, driven by %s", c->player,
                     seen[c->player] ? lastUid[c->player] : 0, c->uid, c->ctrlKind == CTRL_PLAYER ? "player" : c->ctrlKind == CTRL_AI ? "AI" : "other/none", who);
                seen[c->player] = 1; lastUid[c->player] = c->uid; lastKind[c->player] = c->ctrlKind;
            }
        }
    }
    if (C.dumpState && g_world.ok && g_frame % (uint64_t)C.dumpEveryNFrames == 0) state_dump(&g_world, "periodic");
    {   /* pause / menu transitions, logged once each (the brains log why they block) */
        static int lastPaused = -1, lastProof = -1;
        if (g_world.paused != lastPaused) {
            if (lastPaused >= 0 || g_world.paused) LOGI(g_world.paused ? "Game paused (P%d slot status %d): bot pads held neutral." : "Game no longer paused.", g_world.pausedSlot + 1, g_world.pausedStatus);
            lastPaused = g_world.paused;
        }
        if (!g_world.noGsWhy) g_world.noGsWhy = "";
        int proof = !g_world.ok && g_world.noGameState;
        if (proof != lastProof && g_armed && !g_world.menuRead && state_available()) { LOGI("Menu check: %s (%s).", proof ? "no GameState exists (character select or another menu)" : "not proven to be a menu without a GameState", g_world.noGsWhy[0] ? g_world.noGsWhy : g_world.ok ? "a stage is running" : "-"); lastProof = proof; }
    }
    manage_bots(now);
    for (int i = 0; i < NBOT; i++) {
        if (!g_bots[i].joy) continue;
        snprintf(g_brains[i].wantChar, sizeof g_brains[i].wantChar, "%s", C.botChar[i]);
        g_brains[i].wantColor = C.botColor[i];
        g_brains[i].aggression = C.botAggr[i];
        g_brains[i].live = g_menuOpen;
        if (!bot_may_act(i) || !g_focusNow) {
            /* menu open, bot off or game unfocused: the brain doesn't run (a half-done character-select
               press is forgotten and redone; anyone who joins meanwhile is treated as human) */
            neutral_out(&g_bots[i].out);
            g_brains[i].cs.pressing = 0;
            continue;
        }
        brain_think(&g_brains[i], g_brains, NBOT, &g_world, state_available(), now, &g_bots[i].out);
        /* brain output is rebuilt from neutral every frame; when the world isn't readable it stays neutral */
    }
    if (g_armed && bots_attached() && !state_available() && now - g_lastFallbackWarn > 30000) {
        g_lastFallbackWarn = now; LOGI("State reading unavailable (%s); bots stay neutral (or use the test pattern if enabled).", state_failure());
    }
    if (g_menuOpen && g_focusNow) menu_refresh();
}

/* ---- every controller the game has been told about (dedupe + phantom-duplicate filter) ---- */
static DevRec *dev_find(int inst) { for (int i = 0; i < g_ndev; i++) if (g_dev[i].inst == inst) return &g_dev[i]; return NULL; }
static void dev_forget(int inst) { for (int i = 0; i < g_ndev; i++) if (g_dev[i].inst == inst) { g_dev[i] = g_dev[--g_ndev]; return; } }
/* 1 = pass the event to the game, 0 = drop it */
static int device_filter(SDL_Event *ev, ULONGLONG now) {
    int32_t which; memcpy(&which, ev->padding + 8, 4);
    if (ev->type == SDL_CONTROLLERDEVICEADDED) {
        int inst = R.DevInstanceID ? R.DevInstanceID(which) : -1;
        DevRec *d = dev_find(inst);
        if (d && d->delivered) { LOGI("DROPPED duplicate CONTROLLERDEVICEADDED for instance %d (the game already has it)", inst); return 0; }
        if (!d && g_ndev < 32) { d = &g_dev[g_ndev++]; memset(d, 0, sizeof *d); d->inst = inst; }
        if (!d) return 1;
        SDL_JoystickGUID g; memset(&g, 0, sizeof g); if (R.GetDeviceGUID) g = R.GetDeviceGUID(which);
        d->vid = R.DevVendor ? R.DevVendor(which) : 0; d->pid = R.DevProduct ? R.DevProduct(which) : 0;
        d->drv = (char)g.data[14]; d->at = now; d->ours = is_bot_instance(inst) >= 0;
        int virt = R.JoystickIsVirtual && R.JoystickIsVirtual(which);
        if (!d->ours && !virt && d->vid) {
            for (int i = 0; i < g_ndev; i++) {
                DevRec *o = &g_dev[i];
                if (o == d || !o->delivered || o->ours || o->vid != d->vid || o->pid != d->pid || o->drv == d->drv) continue;
                if (now - o->at > 3000 && d->at - o->at > 3000) continue;
                LOGI("WARNING: instance %d (%04X:%04X via %c) looks like the same physical pad as instance %d (via %c) seen through a second SDL driver.",
                     inst, d->vid, d->pid, d->drv ? d->drv : '?', o->inst, o->drv ? o->drv : '?');
                if (C.hideDuplicatePads) { d->hidden = 1; LOGI("DROPPED: hiding instance %d from the game (HideDuplicatePads=1) so one pad doesn't become two players.", inst); return 0; }
            }
        }
        d->delivered = 1;
        return 1;
    }
    if (ev->type == SDL_CONTROLLERDEVICEREMOVED) {
        DevRec *d = dev_find(which);
        if (d && d->hidden) { LOGI("DROPPED CONTROLLERDEVICEREMOVED for hidden duplicate instance %d", which); dev_forget(which); return 0; }
        if (d && !d->delivered) { LOGI("DROPPED CONTROLLERDEVICEREMOVED for instance %d (never given to the game)", which); dev_forget(which); return 0; }
        if (!d) LOGI("note: CONTROLLERDEVICEREMOVED for instance %d that we never saw added", which);
        dev_forget(which);
        return 1;
    }
    return 1;
}

static void handle_device_event(SDL_Event *ev, ULONGLONG now) {
    int32_t which; memcpy(&which, ev->padding + 8, 4);
    switch (ev->type) {
    case SDL_JOYDEVICEADDED: describe_device(which, "JOYDEVICEADDED"); break;
    case SDL_CONTROLLERDEVICEADDED: {
        describe_device(which, "CONTROLLERDEVICEADDED");
        int inst = R.DevInstanceID ? R.DevInstanceID(which) : -1;
        if (is_bot_instance(inst) < 0) { g_humanPadsSeen++; g_lastHumanAddAt = now; }
        int have = 0; for (int i = 0; i < 4; i++) have |= g_fnaSlot[i] == inst;
        for (int i = 0; i < 4 && !have; i++) if (g_fnaSlot[i] < 0) { g_fnaSlot[i] = inst; have = 1; }
        log_fna_slots("device added");
        break; }
    case SDL_JOYDEVICEREMOVED: LOGI("JOYDEVICEREMOVED: instance %d%s", which, is_bot_instance(which) >= 0 ? " (ours)" : ""); break;
    case SDL_CONTROLLERDEVICEREMOVED:
        LOGI("CONTROLLERDEVICEREMOVED: instance %d", which);
        for (int i = 0; i < g_nPendingRemove; i++) if (g_pendingRemove[i] == which) g_pendingRemove[i--] = g_pendingRemove[--g_nPendingRemove];
        for (int i = 0; i < 4; i++) if (g_fnaSlot[i] == which) g_fnaSlot[i] = -1;
        log_fna_slots("device removed");
        break;
    case SDL_CONTROLLERDEVICEREMAPPED: LOGI("CONTROLLERDEVICEREMAPPED: instance %d", which); break;
    }
}

/* ---------------- exported wrappers ---------------- */
static void init_state_once(void) {
    if (g_stateInit) return;
    g_stateInit = 1;
    HMODULE exe = GetModuleHandleW(NULL);
    wchar_t path[MAX_PATH]; GetModuleFileNameW(exe, path, MAX_PATH);
    LOGI("Host program: %ls", path);
    state_init(exe);
    state_set_wanted(C.diagnostics && C.dumpState);
}
static void set_input_hints(void) {
    static int done; if (done) return; done = 1;
    if (!C.legacyDrivers || !R.SetHint) { LOGI("LegacyInputDrivers=0: SDL 2.32 uses all its controller drivers."); return; }
    /* Closer to the game's own SDL 2.0.9: Xbox pads through XInput only. RawInput and
       Windows.Gaming.Input did not exist in 2.0.9 and can report the same pad a second time. */
    static const char *h[][2] = { { "SDL_JOYSTICK_RAWINPUT", "0" }, { "SDL_JOYSTICK_WGI", "0" }, { "SDL_JOYSTICK_HIDAPI_XBOX", "0" } };
    for (int i = 0; i < 3; i++) LOGI("LegacyInputDrivers=1: set hint %s=%s (result %d; an environment variable would still win)", h[i][0], h[i][1], R.SetHint(h[i][0], h[i][1]));
}
int Proxy_SDL_Init(Uint32 flags) {
    if (!load_real() || !R.Init) return -1;
    if (flags & (SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER)) set_input_hints();
    int rc = R.Init(flags);
    LOGD("SDL_Init(0x%08X) = %d", flags, rc);
    if (R.CurrentVideoDriver && R.CurrentVideoDriver()) LOGD("video driver: %s", R.CurrentVideoDriver());
    init_state_once();
    after_init(flags, rc);
    return rc;
}
int Proxy_SDL_InitSubSystem(Uint32 flags) {
    if (!load_real() || !R.InitSubSystem) return -1;
    if (flags & (SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER)) set_input_hints();
    int rc = R.InitSubSystem(flags);
    LOGD("SDL_InitSubSystem(0x%08X) = %d", flags, rc);
    init_state_once();
    after_init(flags, rc);
    return rc;
}
void Proxy_SDL_QuitSubSystem(Uint32 flags) {
    if (!load_real() || !R.QuitSubSystem) return;
    if (flags & (SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER)) { detach_bots("SDL_QuitSubSystem"); g_joyReady = 0; }
    R.QuitSubSystem(flags);
}
void Proxy_SDL_Quit(void) {
    if (!load_real() || !R.Quit) return;
    ULONGLONG t0 = GetTickCount64();
    LOGI("SDL_Quit: game is shutting down.");
    detach_bots("SDL_Quit");
    overlay_shutdown();
    state_shutdown();                  /* never blocks more than ~150 ms */
    R.Quit();
    LOGI("SDL_Quit done in %llu ms.", GetTickCount64() - t0);
    log_flush();
}
/* m3: the game reads pads by polling these two. While the menu is open the human pads read as idle;
   after it closes, buttons/sticks that were held stay idle until released (so the A or B that closed
   the menu doesn't also act in the game). Bot pads are never filtered. */
static int gc_is_bot(SDL_GameController *gc) {
    SDL_Joystick *j = R.GCGetJoystick ? R.GCGetJoystick(gc) : NULL;
    return j && R.JoystickInstanceID && is_bot_instance(R.JoystickInstanceID(j)) >= 0;
}
static PadLatch *latch_for(SDL_GameController *gc) {
    for (int i = 0; i < g_nlatch; i++) if (g_latch[i].gc == gc) return &g_latch[i];
    return NULL;
}
static void latch_update_any(void) {
    int any = 0; for (int i = 0; i < g_nlatch; i++) any |= g_latch[i].btnLatch || g_latch[i].axLatch;
    g_latchAny = any; if (!any) g_nlatch = 0;
}
Uint8 Proxy_SDL_GameControllerGetButton(SDL_GameController *gc, int button) {
    if (!load_real() || !R.GCGetButton) return 0;
    Uint8 v = R.GCGetButton(gc, button);
    if (!g_menuOpen && !g_latchAny) return v;
    if (!gc || gc_is_bot(gc)) return v;
    if (g_menuOpen) return 0;
    PadLatch *l = latch_for(gc);
    if (l && button >= 0 && button < 32 && (l->btnLatch >> button & 1)) {
        if (v) return 0;
        l->btnLatch &= ~(1u << button); latch_update_any();
    }
    return v;
}
Sint16 Proxy_SDL_GameControllerGetAxis(SDL_GameController *gc, int axis) {
    if (!load_real() || !R.GCGetAxis) return 0;
    Sint16 v = R.GCGetAxis(gc, axis);
    if (!g_menuOpen && !g_latchAny) return v;
    if (!gc || gc_is_bot(gc)) return v;
    if (g_menuOpen) return 0;
    PadLatch *l = latch_for(gc);
    if (l && axis >= 0 && axis < 8 && (l->axLatch >> axis & 1)) {
        if (v > 8000 || v < -8000) return 0;
        l->axLatch &= (uint8_t)~(1u << axis); latch_update_any();
    }
    return v;
}
void Proxy_SDL_GetVersion(SDL_version *v) {
    if (v) { v->major = 2; v->minor = 0; v->patch = 9; }   /* same answer as the stock SDL2.dll */
}
int Proxy_SDL_PollEvent(SDL_Event *ev) {
    if (!load_real() || !R.PollEvent) return 0;
    ULONGLONG now = GetTickCount64();
    if (now - g_lastTick >= 12) { g_lastTick = now; frame_tick(now); }
    auto_attach_check(now);
    for (;;) {
        int rc = R.PollEvent(ev);
        if (rc != 1 || !ev) {
            if (g_holding && ev && (g_nPendingRemove == 0 || now > g_holdSince + 1000)) {
                if (g_nPendingRemove) LOGI("removal of our pads not confirmed after 1 s; releasing the held controller anyway");
                /* removal events for our pads have been delivered; now hand the game the human pad,
                   with its device index re-looked-up (it moved down when our pads left) */
                *ev = g_held; g_holding = 0;
                int n = R.NumJoysticks(), idx = -1;
                for (int k = 0; k < n; k++) if (R.DevInstanceID(k) == g_heldInst) { idx = k; break; }
                if (idx < 0) { LOGI("held controller (instance %d) went away; dropping its event", g_heldInst); return rc; }
                memcpy(ev->padding + 8, &idx, 4);
                if (!device_filter(ev, now)) return rc;
                handle_device_event(ev, now);
                g_reattachAt = now + 1500; g_reattachPending = 1;
                return 1;
            }
            return rc;
        }
        switch (ev->type) {
        case SDL_CONTROLLERDEVICEADDED: {
            int32_t which; memcpy(&which, ev->padding + 8, 4);
            int inst = R.DevInstanceID ? R.DevInstanceID(which) : -1;
            int nbBefore = bots_attached(), anyActing = 0;
            for (int i = 0; i < NBOT; i++) anyActing |= g_bots[i].joy && g_brains[i].id.ctrlId >= 0;
            if (is_bot_instance(inst) < 0 && nbBefore && C.reorderLatePads && !anyActing && !g_holding) {
                describe_device(which, "late real controller (held back for a moment)");
                LOGI("A real controller appeared after the bot pads. Unplugging the idle bot pads first so it gets the lower slot.");
                g_held = *ev; g_heldInst = inst; g_holding = 1; g_holdSince = now; g_nPendingRemove = 0;
                detach_bots("re-ordering for a late real controller");
                continue;          /* deliver the removal events first */
            }
            if (is_bot_instance(inst) < 0 && nbBefore)
                LOGI("A real controller appeared after the bot pads; it gets the next free slot after them (ReorderLatePads=0).");
            if (!device_filter(ev, now)) { describe_device(which, "CONTROLLERDEVICEADDED (dropped)"); continue; }
            handle_device_event(ev, now);
            break; }
        case SDL_CONTROLLERDEVICEREMOVED:
            if (!device_filter(ev, now)) continue;
            handle_device_event(ev, now); break;
        case SDL_JOYDEVICEADDED: case SDL_JOYDEVICEREMOVED: case SDL_CONTROLLERDEVICEREMAPPED:
            handle_device_event(ev, now); break;
        case SDL_KEYDOWN: {
            Uint8 repeat = ev->padding[13];
            int32_t scancode; memcpy(&scancode, ev->padding + 16, 4);
            if (!g_keyboardSeen) { g_keyboardSeen = 1; LOGI("Keyboard input seen (first key press)."); }
            if (scancode == SDL_SCANCODE_F7) { if (!repeat) f7_event(); continue; }     /* F7 is ours; the game never sees it */
            if (g_menuOpen) {
                int mk = key_to_mk(scancode);
                if (mk != MK_NONE && (!repeat || mk <= MK_RIGHT)) menu_key(mk);
                if (scancode >= 0 && scancode < 512) g_keySwallowed[scancode] = 1;
                continue;                                  /* the game doesn't see keys while the menu is open */
            }
            if (scancode >= 0 && scancode < 512 && g_keySwallowed[scancode]) continue;   /* still held from the menu */
            break; }
        case SDL_KEYUP: {
            int32_t scancode; memcpy(&scancode, ev->padding + 16, 4);
            if (scancode == SDL_SCANCODE_F7 && !g_f7PrevDown) g_f7By = 0;
            if (scancode >= 0 && scancode < 512) g_keySwallowed[scancode] = 0;
            break; }                                       /* key-ups always pass (the game never keeps a stuck key) */
        case SDL_TEXTINPUT: case SDL_TEXTEDITING:
            if (g_menuOpen) continue;
            break;
        case SDL_JOYAXISMOTION: case SDL_JOYBALLMOTION: case SDL_JOYHATMOTION: case SDL_JOYBUTTONDOWN:
        case SDL_CONTROLLERAXISMOTION: case SDL_CONTROLLERBUTTONDOWN:
            if (g_menuOpen) { int32_t which; memcpy(&which, ev->padding + 8, 4); if (is_bot_instance(which) < 0) continue; }
            break;
        }
        return rc;
    }
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        InitializeCriticalSection(&g_logLock);
        DWORD n = GetModuleFileNameW(h, g_dir, MAX_PATH);
        while (n > 0 && g_dir[n - 1] != L'\\' && g_dir[n - 1] != L'/') n--;
        g_dir[n] = 0;
        load_config();          /* kernel32 only; real SDL is loaded later, outside the loader lock */
        LOGI("%s loaded from %ls (files keep the SoR4Bots names)", SOR4BOTS_VERSION, g_dir);
        if (C.botNormalized) LOGI("SoR4Bots.ini: %s%s turned OFF at load (a bot needs the one before it ON: P3 needs P2, P4 needs P3).", C.botNormalized & 2 ? "P3 " : "", C.botNormalized & 4 ? "P4 " : "");
        LOGI("Config: P2 bot %s (%s, color %d), P3 bot %s (%s, color %d), P4 bot %s (%s, color %d) [color 0 = Auto]; AttachMode=%s StartActive=%d AttachDelayMs=%d SettleMs=%d "
             "ReorderLatePads=%d InputFallbackSeconds=%d LegacyInputDrivers=%d HideDuplicatePads=%d Diagnostics=%d Brain=%s DumpState=%d DumpEveryNFrames=%d BrainLogEveryMs=%d Overlay=%d",
             C.botOn[0] ? "ON" : "off", C.botChar[0], C.botColor[0], C.botOn[1] ? "ON" : "off", C.botChar[1], C.botColor[1], C.botOn[2] ? "ON" : "off", C.botChar[2], C.botColor[2],
             C.attachOnF7 ? "OnF7" : "Auto", C.startActive, C.attachDelayMs, C.settleMs, C.reorderLatePads, C.inputFallbackSeconds, C.legacyDrivers, C.hideDuplicatePads, C.diagnostics,
             C.smart ? "Smart" : "Test", C.dumpState, C.dumpEveryNFrames, C.brainLogMs, C.overlay);
        static const char *const ag[] = { "Low", "Normal", "High" };
        LOGI("Fighting: aggression P2 %s P3 %s P4 %s; specials %d (offensive >= %d%% life, defensive >= %d%%), star moves %d (%s), back attack %s, "
             "pickups %d (food below %d%%, money/weapons after a %d s head start for players), grabs %d, break objects %d, jump attacks %d, weapon throws %d, friendly-fire retaliation %d, retreat below %d%%; P1 watch log every %d ms",
             ag[C.botAggr[0]], ag[C.botAggr[1]], ag[C.botAggr[2]], C.useSpecials, C.specialMinLifePct, C.defSpecialMinLifePct, C.useStars,
             C.starMode == 2 ? "Y+B" : C.starMode == 0 ? "auto: the game's control option, else RB" : "button", C.backAttack == 0 ? "off" : C.backAttack == 2 ? "back+X" : "RT",
             C.usePickups, C.foodLifePct, C.itemHeadStartMs / 1000, C.useGrabs, C.breakObjects, C.jumpAttacks, C.weaponThrows, C.friendlyRetaliate, C.retreatLifePct, C.p1WatchMs);
        for (int i = 0; i < NBOT; i++) { char nm[32]; snprintf(nm, sizeof nm, "SoR4Bots P%d", i + 2); brain_init_detached(&g_brains[i], i, nm); }
    } else if (reason == DLL_PROCESS_DETACH) {
        /* No waits, no locks, no CRT here (loader lock held; at process exit other threads are
           already gone and may have died holding locks). Stop logging; never reopen. */
        g_logState = 3;
        if (!r && g_logH != INVALID_HANDLE_VALUE) CloseHandle(g_logH);   /* FreeLibrary only */
    }
    return TRUE;
}
