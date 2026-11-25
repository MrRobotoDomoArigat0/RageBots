/* v1.2: pad dedupe (sor4pads.h). Build: x86_64-w64-mingw32-gcc -O2 -o pads_test.exe pads_test.c */
#include <stdio.h>
#include "../sor4pads.h"
static int fails;
#define CHECK(cond, ...) do { int ok_ = !!(cond); printf("%s: ", ok_ ? "OK  " : "FAIL"); printf(__VA_ARGS__); putchar('\n'); if (!ok_) fails++; } while (0)
int main(void) {
    int m;
    { PadDesc d[] = { { 0x28DE, 0x1205, 'h', "\\\\?\\HID#VID_28DE&PID_1205#1", NULL, 0 }, { 0x28DE, 0x11FF, 'x', "XInput#0", NULL, 0 } };
      CHECK(pads_count_humans(d, 2, &m) == 1 && m == 1, "Steam Deck: built-in pad + Steam virtual pad = 1 human (merged %d)", m); }
    { PadDesc d[] = { { 0x054C, 0x09CC, 'h', "hidA", NULL, 0 }, { 0x054C, 0x09CC, 'r', "rawA", NULL, 0 } };
      CHECK(pads_count_humans(d, 2, &m) == 1, "one DS4 through HIDAPI and RawInput = 1 (merged %d)", m); }
    { PadDesc d[] = { { 0x045E, 0x02FF, 'x', "XInput#0", NULL, 0 }, { 0x045E, 0x02FF, 'x', "XInput#1", NULL, 0 } };
      CHECK(pads_count_humans(d, 2, &m) == 2 && m == 0, "two identical Xbox pads on XInput stay 2 (never merged)"); }
    { PadDesc d[] = { { 0x045E, 0x02FF, 'x', "XInput#0", NULL, 0 }, { 0x045E, 0x02FF, 'x', "XInput#1", NULL, 0 },
                      { 0x045E, 0x02FF, 'w', "wgi0", NULL, 0 }, { 0x045E, 0x02FF, 'w', "wgi1", NULL, 0 } };
      CHECK(pads_count_humans(d, 4, &m) == 2 && m == 2, "two Xbox pads each seen via XInput and WGI = 2 (merged %d)", m); }
    { PadDesc d[] = { { 0x054C, 0x09CC, 'h', "same", NULL, 0 }, { 0x1234, 0x5678, 'r', "same", NULL, 0 } };
      CHECK(pads_count_humans(d, 2, &m) == 1, "same device path = 1"); }
    { PadDesc d[] = { { 0x054C, 0x09CC, 'h', "p1", "SER1", 0 }, { 0x1234, 0x5678, 'r', "p2", "SER1", 0 } };
      CHECK(pads_count_humans(d, 2, &m) == 1, "same serial = 1"); }
    { PadDesc d[] = { { 0x054C, 0x09CC, 'h', "p1", NULL, 0 }, { 0x28DE, 0x11FF, 'x', "v1", NULL, 0 }, { 0x045E, 0x02FF, 'x', "x1", NULL, 0 } };
      CHECK(pads_count_humans(d, 3, &m) == 2, "DS4 under Steam Input + Xbox pad outside it = 2"); }
    { PadDesc d[] = { { 0x28DE, 0x11FF, 'x', "v1", NULL, 0 }, { 0x28DE, 0x11FF, 'x', "v2", NULL, 0 }, { 0x28DE, 0x1205, 'h', "deck", NULL, 0 } };
      CHECK(pads_count_humans(d, 3, &m) == 2, "two Steam virtual pads (Deck + paired pad hidden by Steam) = 2"); }
    { PadDesc d[] = { { 0x045E, 0x028E, 'v', "bot", NULL, 1 }, { 0x045E, 0x028E, 'x', "XInput#0", NULL, 0 } };
      CHECK(pads_count_humans(d, 2, &m) == 1, "bot pads / hidden duplicates are skipped"); }
    { PadDesc d[] = { { 0, 0, 0, NULL, NULL, 0 }, { 0, 0, 0, NULL, NULL, 0 } };
      CHECK(pads_count_humans(d, 2, &m) == 2, "unknown ids, no path: each counted"); }
    printf("%s (%d failure(s))\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
