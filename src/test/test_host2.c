/* Test host 2: loads the proxy SDL2.dll like FNA, logs controller add/remove events in order.
 * arg "f7"   : push F7 at frame 30, expect bot pad to attach and move (test pattern; no game here)
 * arg "late" : AttachMode=Auto; after the bot pad is in, plug a fake "human" pad; expect re-ordering
 * arg "menu" : m3/m7 F7 menu (locked P3/P4 rows skipped): human pad and keys swallowed while open, menu driven by the human pad and keys,
 *              buttons held at close stay hidden until released, choice saved to SoR4Bots.ini */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
typedef union { uint32_t type; uint8_t b[56]; } Ev;
typedef struct { uint16_t version, type, naxes, nbuttons, nhats, vendor_id, product_id, padding; uint32_t button_mask, axis_mask;
  const char *name; void *userdata; void (*Update)(void *); void *a, *b2, *c, *d, *e; } VDesc;
#define P(T, n) typedef T; n##_t n = (n##_t)(void*)GetProcAddress(h, #n); if(!n){printf("missing %s\n", #n); return 1;}
int main(int argc, char **argv){
  int late = argc > 1 && !strcmp(argv[1], "late"), menu = argc > 1 && !strcmp(argv[1], "menu");
  HMODULE h = LoadLibraryA("SDL2.dll"); if(!h){printf("load fail %lu\n",GetLastError());return 1;}
  P(int(*SDL_Init_t)(uint32_t), SDL_Init) P(int(*SDL_PollEvent_t)(Ev*), SDL_PollEvent) P(int(*SDL_PushEvent_t)(Ev*), SDL_PushEvent)
  P(void*(*SDL_GameControllerOpen_t)(int), SDL_GameControllerOpen) P(uint8_t(*SDL_GameControllerGetButton_t)(void*,int), SDL_GameControllerGetButton)
  P(int16_t(*SDL_GameControllerGetAxis_t)(void*,int), SDL_GameControllerGetAxis) P(const char*(*SDL_GameControllerNameForIndex_t)(int), SDL_GameControllerNameForIndex)
  P(void(*SDL_Quit_t)(void), SDL_Quit)   P(int(*SDL_GameControllerAddMapping_t)(const char*), SDL_GameControllerAddMapping)
  printf("SDL_Init=%d\n", SDL_Init(0x2000|0x200|0x4000));
  typedef int(*AV_t)(const VDesc*); AV_t SDL_JoystickAttachVirtualEx = (AV_t)(void*)GetProcAddress(GetModuleHandleA("SDL2_real.dll"), "SDL_JoystickAttachVirtualEx");
  typedef int(*SVB_t)(void*,int,uint8_t); typedef int(*SVA_t)(void*,int,int16_t); typedef void*(*GJ_t)(void*);
  HMODULE hr = GetModuleHandleA("SDL2_real.dll"); SVB_t SetVB = (SVB_t)(void*)GetProcAddress(hr, "SDL_JoystickSetVirtualButton"); SVA_t SetVA = (SVA_t)(void*)GetProcAddress(hr, "SDL_JoystickSetVirtualAxis");
  GJ_t GetJoy = (GJ_t)(void*)GetProcAddress(h, "SDL_GameControllerGetJoystick");
  if (menu) late = 0;
  void *human = NULL; int keysSeen = 0, keysAfter = 0, fails = 0;
#define KEY(sc) do { Ev k; memset(&k,0,sizeof k); k.type=0x300; k.b[12]=1; int32_t sc_=(sc); memcpy(k.b+16,&sc_,4); SDL_PushEvent(&k); k.type=0x301; k.b[12]=0; SDL_PushEvent(&k); } while (0)
#define HB(btn, v) SetVB(GetJoy(human), (btn), (v))
#define EXPECT(cond, what) do { int c_ = (cond); printf("frame %d %s: %s\n", frame, what, c_ ? "OK" : "FAIL"); fails += !c_; } while (0)
  void *pads[8] = {0}; char names[8][64] = {{0}}; int npads = 0, f7sent = 0, humanAdded = 0, presses = 0, last[15] = {0}; int16_t minx = 0, maxx = 0;
  for (int frame = 0; frame < 60*10; frame++) {
    Ev e;
    while (SDL_PollEvent(&e)) {
      int32_t which; memcpy(&which, e.b+8, 4);
      if (e.type == 0x653) { const char *nm = SDL_GameControllerNameForIndex(which); void *p = SDL_GameControllerOpen(which);
        printf("frame %d CONTROLLERDEVICEADDED index %d \"%s\"\n", frame, which, nm ? nm : "?");
        if (p && npads < 8) { pads[npads] = p; snprintf(names[npads], 64, "%s", nm ? nm : "?"); npads++; } }
      if (e.type == 0x654) printf("frame %d CONTROLLERDEVICEREMOVED instance %d\n", frame, which);
      if (e.type == 0x300) { if (frame < 118) keysSeen++; else keysAfter++; }
    }
    if (menu) {
      if (frame == 10) { static VDesc d; memset(&d, 0, sizeof d); d.version = 1; d.type = 1; d.naxes = 6; d.nbuttons = 15; d.vendor_id = 0x045E; d.product_id = 0x02FF;
        d.button_mask = 0x7fff; d.axis_mask = 0x3f; d.name = "Human Test Pad"; printf("frame %d host plugged \"Human Test Pad\" -> index %d\n", frame, SDL_JoystickAttachVirtualEx(&d)); }
      for (int p = 0; p < npads && !human; p++) if (strstr(names[p], "Human")) human = pads[p];
      if (human) {
        if (frame == 20) HB(0, 1);
        if (frame == 22) EXPECT(SDL_GameControllerGetButton(human, 0) == 1, "menu closed: human A reaches the game");
        if (frame == 24) HB(0, 0);
        if (frame == 30) { KEY(64); printf("frame %d pushed F7 (open menu)\n", frame); }
        if (frame == 35) { HB(2, 1); SetVA(GetJoy(human), 2, 30000); }      /* X and the right stick: the menu ignores them */
        if (frame == 37) EXPECT(SDL_GameControllerGetButton(human, 2) == 0 && SDL_GameControllerGetAxis(human, 2) == 0, "menu open: human X and right stick hidden from the game");
        if (frame == 39) { HB(2, 0); SetVA(GetJoy(human), 2, 0); }
        if (frame == 45) KEY(81);                                      /* Down: cursor 1 (keyboard) */
        if (frame == 50) HB(12, 1); if (frame == 53) HB(12, 0);        /* d-pad down: cursor 2 */
        if (frame == 56) HB(12, 1); if (frame == 59) HB(12, 0);        /* m7: P3/P4 rows are locked (P2 OFF): skipped -> Bot skill (v1.1) */
        if (frame == 60) KEY(81);                                      /* v1.1: down: Bot skill -> Close */
        if (frame == 62) KEY(81);                                      /* down: wraps to row 0 = P2 bot */
        if (frame == 70) HB(0, 1); if (frame == 73) HB(0, 0);          /* A: P2 bot ON */
        if (frame == 100) HB(2, 1);
        if (frame == 102) { KEY(64); printf("frame %d pushed F7 (close menu) with X held\n", frame); }
        if (frame == 106) EXPECT(SDL_GameControllerGetButton(human, 2) == 0, "menu closed, X still held from the menu: hidden");
        if (frame == 108) HB(2, 0);
        if (frame == 110) EXPECT(SDL_GameControllerGetButton(human, 2) == 0, "X released");
        if (frame == 112) HB(2, 1);
        if (frame == 114) EXPECT(SDL_GameControllerGetButton(human, 2) == 1, "new X press after the menu reaches the game");
        if (frame == 116) HB(2, 0);
        if (frame == 120) KEY(81);
      }
      if (frame == 200) break;
    }
    if (!menu && !late && !f7sent && frame == 30) { Ev k; memset(&k,0,sizeof k); k.type=0x300; k.b[12]=1; int32_t sc=64; memcpy(k.b+16,&sc,4); SDL_PushEvent(&k); f7sent=1; printf("frame %d pushed F7\n", frame); }
    static int offAt = -1;
    if (!menu && !late && frame == 45) { Ev k; memset(&k,0,sizeof k); k.type=0x300; k.b[12]=1; int32_t sc=64; memcpy(k.b+16,&sc,4); SDL_PushEvent(&k); printf("frame %d pushed F7 (close menu)\n", frame); }
    if (!menu && !late && frame == 300) { Ev k; memset(&k,0,sizeof k); k.type=0x300; k.b[12]=1; int32_t sc=64; memcpy(k.b+16,&sc,4); SDL_PushEvent(&k); offAt = frame; printf("frame %d pushed F7 (menu open again)\n", frame); }
    if (!menu && !late && offAt > 0 && frame == offAt + 2) for (int p = 0; p < npads; p++) if (pads[p] && strstr(names[p], "SoR4Bots")) {
      int held = 0; for (int i=0;i<15;i++) held += SDL_GameControllerGetButton(pads[p],i);
      printf("frame %d with the menu open: bot buttons held %d, leftx %d -> %s\n", frame, held, SDL_GameControllerGetAxis(pads[p],0), (!held && !SDL_GameControllerGetAxis(pads[p],0)) ? "NEUTRAL OK" : "NOT NEUTRAL"); }
    if (late && !humanAdded && frame == 150) {
      static VDesc d; memset(&d, 0, sizeof d); d.version = 1; d.type = 1; d.naxes = 6; d.nbuttons = 15; d.vendor_id = 0x045E; d.product_id = 0x02FF;
      d.button_mask = 0x7fff; d.axis_mask = 0x3f; d.name = "Human Test Pad";
      int idx = SDL_JoystickAttachVirtualEx(&d); humanAdded = 1; printf("frame %d host plugged \"Human Test Pad\" -> index %d\n", frame, idx);
    }
    for (int p = 0; p < npads; p++) if (pads[p] && strstr(names[p], "SoR4Bots")) {
      for (int i=0;i<15;i++){int s=SDL_GameControllerGetButton(pads[p],i); if(s&&!last[i]) presses++; last[i]=s;}
      int16_t x=SDL_GameControllerGetAxis(pads[p],0); if(x<minx)minx=x; if(x>maxx)maxx=x; }
    Sleep(16);
  }
  if (menu) {
    int p3 = GetPrivateProfileIntA("Bots", "P3Bot", -1, ".\\SoR4Bots.ini"), p2 = GetPrivateProfileIntA("Bots", "P2Bot", -1, ".\\SoR4Bots.ini"), p2pad = 0;
    for (int p = 0; p < npads; p++) if (strstr(names[p], "SoR4Bots P2")) p2pad = 1;
    printf("keys reaching the game while the menu was open: %d (expect 0); after it closed: %d (expect 1)\n", keysSeen, keysAfter);
    printf("ini after the menu: P2Bot=%d P3Bot=%d (expect 1 and 0: the locked P3 rows were skipped); P2 bot pad plugged in: %s\n", p2, p3, p2pad ? "yes" : "no");
    fails += keysSeen != 0 || keysAfter != 1 || p3 != 0 || p2 != 1 || !p2pad || !human;
    printf("menu test: %s\n", fails ? "FAIL" : "PASS");
  }
  printf("bot button presses %d, bot leftx range %d..%d\n", presses, minx, maxx);
  SDL_Quit(); printf("quit ok\n"); return 0;
}
