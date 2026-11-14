/*
 * Rage Bots (formerly SoR4Bots) F7 menu.
 *
 * How it is drawn: a separate top-most, layered, click-through window that never activates
 * (WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW) placed over the
 * game's client area and drawn with UpdateLayeredWindow and per-pixel alpha (premultiplied BGRA), so the
 * game shows through the dark panel. It runs on its own thread and only redraws when the menu changes.
 * Why not inside the game's frame: SoR4 renders through FNA3D with Direct3D 11 by default (Vulkan or
 * OpenGL possible). Drawing into its frame from an SDL proxy is only possible for OpenGL
 * (SDL_GL_SwapWindow is an SDL export); D3D11/Vulkan would need hooking IDXGISwapChain::Present or
 * vkQueuePresentKHR (patching code in the game's process), which is fragile and against the "no patching"
 * rule. FNA's fullscreen is borderless (desktop) fullscreen, so a top-most window shows above it.
 *
 * Input never goes to this window: the game keeps keyboard focus, and sor4bots.c reads keys from the
 * game's SDL events and the pads through SDL, swallowing them while the menu is open.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "sor4common.h"
#include "sor4menu.h"

/* ---------------- model ---------------- */
int menu_color_count(const MenuModel *m, int bot) {
    const char *c = m->bot[bot].character;
    int mx = 1;
    for (int i = 0; i < m->nroster; i++) {
        if (c[0] && !_stricmp(c, m->roster[i])) return m->palettes[i] > 0 ? m->palettes[i] : 1;
        if (m->palettes[i] > mx) mx = m->palettes[i];
    }
    return m->nroster ? mx : 8;          /* Auto / unknown character: the largest count (wraps in the game) */
}
static int roster_pos(const MenuModel *m, const char *name) {
    if (!name[0] || !_stricmp(name, "Auto")) return -1;
    for (int i = 0; i < m->nroster; i++) if (!_stricmp(name, m->roster[i])) return i;
    return -2;                             /* a name from the ini that the roster doesn't have (yet) */
}
void menu_update_locks(MenuModel *m, const int joined[MENU_BOTS]) {
    m->locked[0] = 0; m->lockWhy[0][0] = 0;
    for (int i = 1; i < MENU_BOTS; i++) {
        m->lockWhy[i][0] = 0; m->locked[i] = 0;
        if (m->bot[i].on) continue;                                  /* never lock a bot that is already on */
        if (!m->bot[i - 1].on) { m->locked[i] = 1; snprintf(m->lockWhy[i], sizeof m->lockWhy[i], "(locked: turn on P%d bot first)", i + 1); }
        else if (!joined[i - 1]) { m->locked[i] = 1; snprintf(m->lockWhy[i], sizeof m->lockWhy[i], "(locked: P%d bot must join first)", i + 1); }
    }
    while (m->cursor < MROW_SKILL && m->cursor >= MROW_PER_BOT && m->locked[m->cursor / MROW_PER_BOT])
        m->cursor = (m->cursor / MROW_PER_BOT - 1) * MROW_PER_BOT;      /* the selected row got locked: back to the bot before it */
}
int menu_cascade_off(MenuModel *m, int bot) {
    int bits = 0;
    for (int j = bot + 1; j < MENU_BOTS; j++) if (m->bot[j].on) { m->bot[j].on = 0; bits |= 1 << j; }
    m->cascaded = bits;
    return bits;
}
static int row_locked(const MenuModel *m, int row) { return row < MROW_SKILL && m->locked[row / MROW_PER_BOT]; }
static void change(MenuModel *m, int row, int dir) {
    if (row == MROW_SKILL) { m->skill = !m->skill; m->changed = 1; return; }   /* v1.1: Easy <-> Normal */
    if (row >= MROW_CLOSE) return;
    BotChoice *b = &m->bot[row / MROW_PER_BOT];
    if (row_locked(m, row)) return;                                  /* m7: locked bot: nothing changes */
    switch (row % MROW_PER_BOT) {
    case 0: b->on = !b->on; m->cascaded = 0; if (!b->on) menu_cascade_off(m, row / MROW_PER_BOT); break;
    case 1: {
        int n = m->nroster, p = roster_pos(m, b->character);
        if (!n) { snprintf(b->character, sizeof b->character, "Auto"); break; }
        if (p == -2) p = -1;
        p += dir;                          /* -1 = Auto, 0..n-1 = roster */
        if (p < -1) p = n - 1;
        if (p >= n) p = -1;
        snprintf(b->character, sizeof b->character, "%s", p < 0 ? "Auto" : m->roster[p]);
        int cc = menu_color_count(m, row / MROW_PER_BOT);
        if (b->color > cc) b->color = 0;
        break; }
    case 2: {
        int n = menu_color_count(m, row / MROW_PER_BOT);
        b->color += dir;
        if (b->color < 0) b->color = n;
        if (b->color > n) b->color = 0;
        break; }
    }
    m->changed = 1;
}
int menu_nav(MenuModel *m, int key) {
    switch (key) {
    case MK_UP: for (int k = 0; k < MROW_COUNT; k++) { m->cursor = (m->cursor + MROW_COUNT - 1) % MROW_COUNT; if (!row_locked(m, m->cursor)) break; } break;
    case MK_DOWN: for (int k = 0; k < MROW_COUNT; k++) { m->cursor = (m->cursor + 1) % MROW_COUNT; if (!row_locked(m, m->cursor)) break; } break;
    case MK_LEFT: change(m, m->cursor, -1); break;
    case MK_RIGHT: change(m, m->cursor, +1); break;
    case MK_OK: if (m->cursor == MROW_CLOSE) return 1; change(m, m->cursor, +1); break;
    case MK_CLOSE: return 1;
    }
    return 0;
}
int menu_lines(const MenuModel *m, char lines[][96], int colors[], int max) {
    int n = 0;
#define ADD(col, ...) do { if (n < max) { snprintf(lines[n], 96, __VA_ARGS__); colors[n] = (col); n++; } } while (0)
    ADD(MC_TITLE, "RAGE BOTS v1.1");   /* v1.1 release */
    ADD(MC_TEXT, "By: MrRobotoDomoArigato");
    ADD(MC_GREY, "F7 close, arrows, Enter");
    ADD(MC_TEXT, "Where: %s", m->where[0] ? m->where : "-");
    if (m->slots[0]) ADD(MC_TEXT, "Slots: %s", m->slots);
    for (int i = 0; i < MENU_BOTS; i++) {
        ADD(MC_TEXT, "%s", "");
        const BotChoice *b = &m->bot[i]; int r = i * MROW_PER_BOT, lk = m->locked[i];
        char col[16];
        if (b->color > 0) snprintf(col, sizeof col, "%d", b->color); else snprintf(col, sizeof col, "Default");   /* m7.1: was "Auto" */
        const char *nt = lk ? m->lockWhy[i] : m->note[i];
        ADD(lk ? MC_GREY : m->cursor == r ? MC_SEL : MC_TEXT, "%sP%d bot: %s%s%s", m->cursor == r ? "> " : "  ", i + 2, b->on ? "ON" : "OFF",
            nt[0] ? "  " : "", nt);
        ADD(m->cursor == r + 1 ? MC_SEL : b->on && !lk ? MC_TEXT : MC_GREY, "    %sCharacter: %s", m->cursor == r + 1 ? "> " : "  ",
            b->character[0] ? b->character : "Auto");
        ADD(m->cursor == r + 2 ? MC_SEL : b->on && !lk ? MC_TEXT : MC_GREY, "    %sColor: %s", m->cursor == r + 2 ? "> " : "  ", col);
        if (b->on && m->live[i][0]) ADD(MC_GREY, "      Game: %s", m->live[i]);
    }
    ADD(MC_TEXT, "%s", "");
    ADD(m->cursor == MROW_SKILL ? MC_SEL : MC_TEXT, "%sBot skill: %s", m->cursor == MROW_SKILL ? "> " : "  ", m->skill ? "Normal" : "Easy");   /* v1.1 */
    ADD(m->cursor == MROW_CLOSE ? MC_SEL : MC_TEXT, "%sClose", m->cursor == MROW_CLOSE ? "> " : "  ");
#undef ADD
    return n;
}

/* ---------------- overlay window ---------------- */
#define OV_MAXL 32
static struct {
    CRITICAL_SECTION cs; int csInit;
    HANDLE thread, evt; DWORD tid;
    HWND hwnd, game;
    int want, quit, dirty;                  /* want = 1 shown, 0 hidden */
    char lines[OV_MAXL][96]; int colors[OV_MAXL], n;
    int failed;
} OV;

/* draws the panel into a new premultiplied BGRA buffer (malloc'd; caller frees). gameH = game client height */
uint32_t *overlay_compose(char lines[][96], const int *colors, int n, int gameH, int *outW, int *outH) {
    int gh = gameH > 100 ? gameH : 720;
    int fh = gh / 34;
    int fitFh = (gh * 9 / 10) * 3 / (4 * (n > 0 ? n : 1) + 6);      /* m7: more lines; keep the panel within 90% of the game's height */
    if (fh > fitFh) fh = fitFh;
    if (fh < 14) fh = 14; if (fh > 42) fh = 42;
    HDC screen = GetDC(NULL), dc = CreateCompatibleDC(screen);
    HFONT font = CreateFontW(-fh, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    HGDIOBJ oldF = SelectObject(dc, font);
    int tw = 0, lh = fh + fh / 3;
    for (int i = 0; i < n; i++) { SIZE sz; GetTextExtentPoint32A(dc, lines[i], (int)strlen(lines[i]), &sz); if (sz.cx > tw) tw = sz.cx; }
    int pad = fh, W = tw + 2 * pad, H = n * lh + 2 * pad - fh / 3;
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL; uint32_t *out = NULL;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (bmp && bits && (out = (uint32_t *)malloc((size_t)W * H * 4))) {
        HGDIOBJ oldB = SelectObject(dc, bmp);
        uint32_t *px = (uint32_t *)bits;
        memset(px, 0, (size_t)W * H * 4);
        /* text: white on black, then used as coverage (grayscale antialiasing, so all channels agree) */
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
        for (int i = 0; i < n; i++) TextOutA(dc, pad, pad + i * lh, lines[i], (int)strlen(lines[i]));
        GdiFlush();
        static const uint8_t pal[4][3] = { { 255, 200, 40 }, { 236, 236, 236 }, { 255, 236, 64 }, { 128, 128, 128 } };   /* title gold, text, selected yellow, grey */
        const int bgA = 158;                       /* ~62% dark panel: the game shows through */
        for (int y = 0; y < H; y++) {
            int li = y < pad ? 0 : (y - pad) / lh; if (li >= n) li = n - 1; if (li < 0) li = 0;
            const uint8_t *c = pal[colors[li] & 3];
            for (int x = 0; x < W; x++) {
                uint32_t v = px[y * W + x];
                int cov = (int)(v & 0xff), g = (int)((v >> 8) & 0xff), r = (int)((v >> 16) & 0xff);
                if (g > cov) cov = g;
                if (r > cov) cov = r;
                int border = x < 2 || y < 2 || x >= W - 2 || y >= H - 2, a, R, G, B;
                if (border) { a = 235; R = 255 * a / 255; G = 200 * a / 255; B = 40 * a / 255; }   /* thin gold border */
                else { a = cov + bgA * (255 - cov) / 255; R = c[0] * cov / 255; G = c[1] * cov / 255; B = c[2] * cov / 255; }
                out[y * W + x] = (uint32_t)a << 24 | (uint32_t)R << 16 | (uint32_t)G << 8 | (uint32_t)B;
            }
        }
        SelectObject(dc, oldB);
    }
    if (bmp) DeleteObject(bmp);
    SelectObject(dc, oldF); DeleteObject(font); DeleteDC(dc); ReleaseDC(NULL, screen);
    *outW = W; *outH = H; return out;
}
static void ov_render(void) {
    char lines[OV_MAXL][96]; int colors[OV_MAXL], n; HWND game;
    EnterCriticalSection(&OV.cs);
    memcpy(lines, OV.lines, sizeof lines); memcpy(colors, OV.colors, sizeof colors); n = OV.n; game = OV.game; OV.dirty = 0;
    LeaveCriticalSection(&OV.cs);
    RECT cr = { 0, 0, 1280, 720 }; POINT org = { 0, 0 };
    if (game && IsWindow(game)) { GetClientRect(game, &cr); ClientToScreen(game, &org); }
    int W, H; uint32_t *img = overlay_compose(lines, colors, n, cr.bottom - cr.top, &W, &H);
    if (!img) return;
    HDC screen = GetDC(NULL), dc = CreateCompatibleDC(screen);
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL; HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (bmp && bits) {
        memcpy(bits, img, (size_t)W * H * 4);
        HGDIOBJ oldB = SelectObject(dc, bmp);
        int fh = (cr.bottom - cr.top) / 34; if (fh < 14) fh = 14; if (fh > 42) fh = 42;
        POINT dst = { org.x + fh, org.y + fh }, src = { 0, 0 }; SIZE size = { W, H };
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        if (!UpdateLayeredWindow(OV.hwnd, screen, &dst, &size, dc, &src, 0, &bf, ULW_ALPHA) && !OV.failed) {
            OV.failed = 1; LOGE("menu overlay: UpdateLayeredWindow failed (%lu)", GetLastError());
        }
        SetWindowPos(OV.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SelectObject(dc, oldB);
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(dc); ReleaseDC(NULL, screen); free(img);
}
static LRESULT CALLBACK ov_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, msg, wp, lp);
}
static DWORD WINAPI ov_thread(LPVOID p) {
    (void)p;
    WNDCLASSW wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = ov_proc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"SoR4BotsMenuOverlay";
    RegisterClassW(&wc);
    OV.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                              wc.lpszClassName, L"Rage Bots menu", WS_POPUP, 0, 0, 1, 1, NULL, NULL, wc.hInstance, NULL);
    if (!OV.hwnd) { LOGE("menu overlay: could not create the window (%lu); the menu still works but is not drawn.", GetLastError()); return 0; }
    LOGI("menu overlay window ready (translucent, top-most, click-through, never takes focus).");
    int shown = 0; RECT lastRc = { 0 };
    for (;;) {
        DWORD r = MsgWaitForMultipleObjects(1, &OV.evt, FALSE, shown ? 250 : INFINITE, QS_ALLINPUT);
        MSG msg; while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        (void)r;
        if (OV.quit) break;
        int want = OV.want;
        if (!want && shown) { ShowWindow(OV.hwnd, SW_HIDE); shown = 0; continue; }
        if (!want) continue;
        RECT rc = { 0 }; POINT o = { 0, 0 };
        if (OV.game && IsWindow(OV.game)) { GetClientRect(OV.game, &rc); ClientToScreen(OV.game, &o); OffsetRect(&rc, o.x, o.y); }
        if (OV.dirty || !shown || memcmp(&rc, &lastRc, sizeof rc)) {
            lastRc = rc; ov_render();
            if (!shown) { ShowWindow(OV.hwnd, SW_SHOWNOACTIVATE); shown = 1; }
        }
    }
    DestroyWindow(OV.hwnd); OV.hwnd = NULL;
    return 0;
}
void overlay_show(void *gameHwnd, const MenuModel *m) {
    if (!OV.csInit) { InitializeCriticalSection(&OV.cs); OV.csInit = 1; }
    char lines[OV_MAXL][96]; int colors[OV_MAXL]; int n = menu_lines(m, lines, colors, OV_MAXL);
    EnterCriticalSection(&OV.cs);
    int same = n == OV.n && !memcmp(lines, OV.lines, sizeof lines) && !memcmp(colors, OV.colors, sizeof colors) && OV.game == (HWND)gameHwnd && OV.want;
    if (!same) { memcpy(OV.lines, lines, sizeof lines); memcpy(OV.colors, colors, sizeof colors); OV.n = n; if (gameHwnd) OV.game = (HWND)gameHwnd; OV.dirty = 1; }
    OV.want = 1;
    LeaveCriticalSection(&OV.cs);
    if (!OV.thread) {
        OV.evt = CreateEventW(NULL, FALSE, FALSE, NULL);
        OV.thread = OV.evt ? CreateThread(NULL, 0, ov_thread, NULL, 0, &OV.tid) : NULL;
        if (!OV.thread) { LOGE("menu overlay: could not start its thread (%lu)", GetLastError()); return; }
    }
    if (!same) SetEvent(OV.evt);
}
void overlay_hide(void) {
    if (!OV.thread) return;
    OV.want = 0; SetEvent(OV.evt);
}
void overlay_shutdown(void) {
    if (!OV.thread) return;
    OV.quit = 1; SetEvent(OV.evt);
    if (OV.tid) PostThreadMessageW(OV.tid, WM_NULL, 0, 0);
    WaitForSingleObject(OV.thread, 300);       /* never block the game's exit for long */
    CloseHandle(OV.thread); OV.thread = NULL;
}
