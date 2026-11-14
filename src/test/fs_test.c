/* v1.1 stage 3: ForceBorderless check. Loads the proxy SDL2.dll (with SDL2_real.dll next to it), creates a window
   asking for exclusive fullscreen, toggles windowed / fullscreen, prints the real window flags. */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
typedef void *(*CW)(const char *, int, int, int, int, uint32_t); typedef int (*SWF)(void *, uint32_t); typedef uint32_t (*GWF)(void *);
typedef int (*INIT)(uint32_t); typedef void (*GWS)(void *, int *, int *); typedef const char *(*GE)(void); typedef void (*DW)(void *); typedef void (*QU)(void);
int main(void) {
    HMODULE m = LoadLibraryA("SDL2.dll"); if (!m) { printf("load failed %lu\n", GetLastError()); return 1; }
    INIT in = (INIT)(void *)GetProcAddress(m, "SDL_Init"); CW cw = (CW)(void *)GetProcAddress(m, "SDL_CreateWindow"); SWF swf = (SWF)(void *)GetProcAddress(m, "SDL_SetWindowFullscreen");
    GWF gwf = (GWF)(void *)GetProcAddress(m, "SDL_GetWindowFlags"); GWS gws = (GWS)(void *)GetProcAddress(m, "SDL_GetWindowSize"); GE ge = (GE)(void *)GetProcAddress(m, "SDL_GetError");
    DW dw = (DW)(void *)GetProcAddress(m, "SDL_DestroyWindow"); QU qu = (QU)(void *)GetProcAddress(m, "SDL_Quit");
    if (in(0x20) != 0) { printf("SDL_Init(VIDEO) failed: %s\n", ge()); return 2; }
    int w, h, bad = 0;
    void *win = cw("fs_test", 0x2FFF0000, 0x2FFF0000, 800, 600, 0x1 | 0x4); if (!win) { printf("create failed: %s\n", ge()); return 3; }
    uint32_t f = gwf(win); gws(win, &w, &h); printf("created fullscreen request: flags 0x%08X (%s) size %dx%d\n", f, (f & 0x1001) == 0x1001 ? "desktop" : (f & 1) ? "EXCLUSIVE" : "windowed", w, h);
    swf(win, 0); f = gwf(win); gws(win, &w, &h); printf("toggle windowed: flags 0x%08X (%s) size %dx%d\n", f, (f & 1) ? "fullscreen" : "windowed", w, h); if (f & 1) bad++;
    swf(win, 0x1); f = gwf(win); gws(win, &w, &h); printf("toggle fullscreen: flags 0x%08X (%s) size %dx%d\n", f, (f & 0x1001) == 0x1001 ? "desktop" : (f & 1) ? "EXCLUSIVE" : "windowed", w, h);
    swf(win, 0); f = gwf(win); gws(win, &w, &h); printf("toggle windowed again: flags 0x%08X size %dx%d\n", f, w, h); if (f & 1) bad++;
    dw(win);
    void *w2 = cw("fs_test2", 0x2FFF0000, 0x2FFF0000, 1280, 720, 0x4); f = gwf(w2); gws(w2, &w, &h); printf("windowed create: flags 0x%08X size %dx%d\n", f, w, h); if ((f & 1) || w != 1280 || h != 720) bad++;
    dw(w2); qu();
    printf("fs_test: %s\n", bad ? "FAIL" : "done"); return bad;
}
