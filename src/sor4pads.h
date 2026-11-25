/* v1.2: counting the HUMAN controllers behind the SDL device list (Steam Deck / Steam Input / remappers).
 * One physical pad can show up as several SDL devices: through two SDL drivers (XInput + HIDAPI / RawInput /
 * WGI), as the physical device plus a Steam Input virtual pad (VID 28DE PID 11FF), or twice with the same device
 * path or serial. The bots must never take a human's slot, so this only merges devices that are provably the
 * same pad and never two separate identical pads on the same driver:
 *   1. same non-empty path, or same non-empty serial  -> one pad
 *   2. same vendor:product seen through different SDL drivers -> per vendor:product, the most seen on one driver
 *   3. Steam Input virtual pads (28DE:11FF): each stands for one physical pad Steam took over, so the count is
 *      max(Steam virtual pads, physical pads) rather than their sum.
 * Pure C, no SDL: sor4bots.c fills PadDesc from SDL; pads_test.c tests it offline. */
#pragma once
#include <string.h>
typedef struct { unsigned short vid, pid; char drv; const char *path, *serial; int skip; } PadDesc;   /* skip: ours, virtual or hidden */
static int pad_is_steam_virtual(const PadDesc *d) { return d->vid == 0x28DE && d->pid == 0x11FF; }
static int pads_count_humans(const PadDesc *d, int n, int *merged) {
    int ng = 0; unsigned short gv[32], gp[32]; int cnt[32][8]; char drvs[32][8]; int nd[32];
    int dupOf[32]; int nSteam = 0, nPhys = 0, nIn = 0;
    if (n > 32) n = 32;
    for (int i = 0; i < n; i++) {
        dupOf[i] = -1;
        if (d[i].skip) continue;
        nIn++;
        for (int j = 0; j < i && dupOf[i] < 0; j++) {
            if (d[j].skip || dupOf[j] >= 0) continue;
            if (d[i].path && d[i].path[0] && d[j].path && !strcmp(d[i].path, d[j].path)) dupOf[i] = j;
            else if (d[i].serial && d[i].serial[0] && d[j].serial && !strcmp(d[i].serial, d[j].serial)) dupOf[i] = j;
        }
    }
    for (int i = 0; i < n; i++) {
        if (d[i].skip || dupOf[i] >= 0) continue;
        if (pad_is_steam_virtual(&d[i])) { nSteam++; continue; }
        if (!d[i].vid) { nPhys++; continue; }          /* unknown ids: counted on its own */
        int g = -1; for (int k = 0; k < ng; k++) if (gv[k] == d[i].vid && gp[k] == d[i].pid) g = k;
        if (g < 0) { if (ng >= 32) { nPhys++; continue; } g = ng++; gv[g] = d[i].vid; gp[g] = d[i].pid; nd[g] = 0; }
        int k = 0; for (; k < nd[g]; k++) if (drvs[g][k] == d[i].drv) break;
        if (k == nd[g]) { if (nd[g] >= 8) { nPhys++; continue; } drvs[g][k] = d[i].drv; cnt[g][k] = 0; nd[g]++; }
        cnt[g][k]++;
    }
    for (int g = 0; g < ng; g++) { int m = 0; for (int k = 0; k < nd[g]; k++) if (cnt[g][k] > m) m = cnt[g][k]; nPhys += m; }
    int real = nSteam > nPhys ? nSteam : nPhys;
    if (merged) *merged = nIn - real;
    return real;
}
