#include "sor4meta.h"
#include <string.h>
#include <stdio.h>
#include <windows.h>

/* ---------- native-format primitives (CoreRT NativeFormatReader) ---------- */
static uint32_t du(const uint8_t *b, uint32_t o, uint32_t *v) {
    uint32_t x = b[o];
    if (!(x & 1)) { *v = x >> 1; return o + 1; }
    if (!(x & 2)) { *v = (x >> 2) | ((uint32_t)b[o+1] << 6); return o + 2; }
    if (!(x & 4)) { *v = (x >> 3) | ((uint32_t)b[o+1] << 5) | ((uint32_t)b[o+2] << 13); return o + 3; }
    if (!(x & 8)) { *v = (x >> 4) | ((uint32_t)b[o+1] << 4) | ((uint32_t)b[o+2] << 12) | ((uint32_t)b[o+3] << 20); return o + 4; }
    if (!(x & 16)) { memcpy(v, b + o + 1, 4); return o + 5; }
    *v = 0; return o + 1;
}
static uint32_t ds(const uint8_t *b, uint32_t o, int32_t *v) {
    int32_t x = b[o];
    if (!(x & 1)) { *v = ((int8_t)b[o]) >> 1; return o + 1; }
    if (!(x & 2)) { *v = (x >> 2) | ((int32_t)(int8_t)b[o+1] << 6); return o + 2; }
    if (!(x & 4)) { *v = (x >> 3) | ((int32_t)b[o+1] << 5) | ((int32_t)(int8_t)b[o+2] << 13); return o + 3; }
    if (!(x & 8)) { *v = (x >> 4) | ((int32_t)b[o+1] << 4) | ((int32_t)b[o+2] << 12) | ((int32_t)(int8_t)b[o+3] << 20); return o + 4; }
    if (!(x & 16)) { memcpy(v, b + o + 1, 4); return o + 5; }
    *v = 0; return o + 1;
}
static uint32_t skipu(const uint8_t *b, uint32_t o) { uint32_t v; return du(b, o, &v); }
static uint32_t skipcoll(const uint8_t *b, uint32_t o) { uint32_t n; o = du(b, o, &n); while (n--) o = skipu(b, o); return o; }

typedef int (*entry_cb)(const uint8_t *b, uint32_t entryOff, void *ctx);   /* return 1 to stop */
static void hashtable_each(const uint8_t *b, uint32_t size, entry_cb cb, void *ctx) {
    if (!b || size < 2) return;
    uint8_t hdr = b[0]; uint32_t base = 1, nb = 1u << (hdr >> 2), isz = hdr & 3;
    for (uint32_t k = 0; k < nb; k++) {
        uint32_t s, e;
        if (isz == 0) { s = b[base + k]; e = b[base + k + 1]; }
        else if (isz == 1) { uint16_t t[2]; memcpy(t, b + base + 2*k, 4); s = t[0]; e = t[1]; }
        else { uint32_t t[2]; memcpy(t, b + base + 4*k, 8); s = t[0]; e = t[1]; }
        uint32_t o = base + s, end = base + e;
        if (end > size) return;
        while (o < end) {
            o++;                                   /* low hashcode byte */
            uint32_t pos = o; int32_t d; o = ds(b, o, &d);
            if (cb(b, pos + (uint32_t)d, ctx)) return;
        }
    }
}

/* ---------- image ---------- */
int meta_in_image(MetaImage *im, const void *p) {
    return (const uint8_t *)p >= im->base && (const uint8_t *)p < im->base + im->size;
}
const uint8_t *meta_fix_ptr(MetaImage *im, uint64_t v) {
    uint64_t b = (uint64_t)(uintptr_t)im->base;
    if (v >= b && v < b + im->size) return (const uint8_t *)(uintptr_t)v;
    if (v >= im->prefBase && v < im->prefBase + im->size) return im->base + (v - im->prefBase);
    return NULL;
}
const char *meta_section_of(MetaImage *im, const void *p) {
    if (!meta_in_image(im, p)) return "outside";
    uint32_t rva = (uint32_t)((const uint8_t *)p - im->base);
    for (int i = 0; i < im->nsec; i++) if (rva >= im->sec[i].rva && rva < im->sec[i].rva + im->sec[i].vsize) return im->sec[i].name;
    return "?";
}
static const uint8_t *extref(MetaImage *im, int blobId, uint32_t idx) {
    const uint8_t *t = im->blob[blobId];
    if (!t || (uint64_t)idx * 4 + 4 > im->blobSize[blobId]) return NULL;
    int32_t rel; memcpy(&rel, t + idx * 4, 4);
    return t + idx * 4 + rel;
}

int meta_open(MetaImage *im, const void *moduleBase, char *err, size_t errlen) {
    memset(im, 0, sizeof *im);
    const uint8_t *b = (const uint8_t *)moduleBase;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)b;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { snprintf(err, errlen, "no MZ header"); return 0; }
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(b + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) { snprintf(err, errlen, "not a PE32+ image"); return 0; }
    im->base = b; im->size = nt->OptionalHeader.SizeOfImage; im->prefBase = nt->OptionalHeader.ImageBase;
    im->timestamp = nt->FileHeader.TimeDateStamp;
    const IMAGE_SECTION_HEADER *sh = IMAGE_FIRST_SECTION(nt);
    const uint8_t *rdata = NULL; uint32_t rdataSize = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections && im->nsec < 24; i++) {
        im->sec[im->nsec].rva = sh[i].VirtualAddress; im->sec[im->nsec].vsize = sh[i].Misc.VirtualSize;
        memcpy(im->sec[im->nsec].name, sh[i].Name, 8); im->sec[im->nsec].name[8] = 0;
        if (!memcmp(sh[i].Name, ".rdata", 6)) { rdata = b + sh[i].VirtualAddress; rdataSize = sh[i].Misc.VirtualSize; }
        im->nsec++;
    }
    if (!rdata) { snprintf(err, errlen, "no .rdata section"); return 0; }
    /* ReadyToRun header: "RTR\0", u16 major, u16 minor, u32 flags, u16 count, u8 entrySize(24), u8 entryType */
    for (uint32_t o = 0; o + 16 < rdataSize; o += 4) {
        const uint8_t *p = rdata + o;
        if (memcmp(p, "RTR\0", 4)) continue;
        uint16_t maj, mn, cnt; memcpy(&maj, p + 4, 2); memcpy(&mn, p + 6, 2); memcpy(&cnt, p + 12, 2);
        if (p[14] != 24 || cnt < 4 || cnt > 128 || maj == 0 || maj > 30) continue;
        int ok = 1;
        for (int k = 0; k < cnt && ok; k++) {
            uint32_t id; uint64_t s, e; memcpy(&id, p + 16 + k*24, 4); memcpy(&s, p + 24 + k*24, 8); memcpy(&e, p + 32 + k*24, 8);
            const uint8_t *ps = meta_fix_ptr(im, s), *pe = meta_fix_ptr(im, e);
            if (!ps || (e && (!pe || pe < ps))) { ok = 0; break; }
            if (!e) pe = ps;                               /* start-only entries */
            if (id >= 300 && id < 364) { im->blob[id - 300] = ps; im->blobSize[id - 300] = (uint32_t)(pe - ps); }
            if (id == 201) { im->gcStaticRegion = ps; im->gcStaticRegionSize = (uint32_t)(pe - ps); }
        }
        if (!ok) { memset(im->blob, 0, sizeof im->blob); continue; }
        im->rtr = p; im->rtrMajor = maj; im->rtrMinor = mn;
        break;
    }
    if (!im->rtr) { snprintf(err, errlen, "ReadyToRun header not found"); return 0; }
    im->md = im->blob[13]; im->mdSize = im->blobSize[13];
    uint32_t sig = 0; if (im->md) memcpy(&sig, im->md, 4);
    if (sig != 0xDEADDFFDu) { snprintf(err, errlen, "embedded metadata missing or unknown format (sig %08X)", sig); return 0; }
    if (!im->blob[1] || !im->blob[8] || !im->blob[9] || !im->blob[6]) { snprintf(err, errlen, "mapping tables missing (TypeMap/FieldAccessMap/InvokeMap)"); return 0; }
    return 1;
}

/* ---------- metadata records ---------- */
static int md_str_eq(MetaImage *im, uint32_t strOff, const char *s, size_t n) {
    if (!strOff || strOff >= im->mdSize) return n == 0;
    uint32_t len, o = du(im->md, strOff, &len);
    return len == n && !memcmp(im->md + o, s, n);
}
static int md_str_copy(MetaImage *im, uint32_t strOff, char *out, size_t cap) {
    if (!strOff || !cap) { if (cap) out[0] = 0; return 0; }
    uint32_t len, o = du(im->md, strOff, &len);
    if (len >= cap) len = (uint32_t)cap - 1;
    memcpy(out, im->md + o, len); out[len] = 0; return (int)len;
}
typedef struct { uint32_t name, enclosing, nestedOff, methodsOff, fieldsOff; } TypeDefInfo;
static void md_typedef(MetaImage *im, uint32_t off, TypeDefInfo *t) {
    const uint8_t *b = im->md; uint32_t v, o = off;
    o = skipu(b, o); o = skipu(b, o);                    /* flags, baseType */
    o = skipu(b, o);                                     /* namespaceDefinition */
    o = du(b, o, &t->name);
    o = skipu(b, o); o = skipu(b, o);                    /* size, packingSize */
    o = du(b, o, &t->enclosing);
    t->nestedOff = o; o = skipcoll(b, o);
    t->methodsOff = o; o = skipcoll(b, o);
    t->fieldsOff = o; (void)v;
}
static uint32_t md_typedef_ns(MetaImage *im, uint32_t off) {
    uint32_t v, o = skipu(im->md, skipu(im->md, off)); du(im->md, o, &v); return v;
}
typedef struct { uint32_t parent, name, typesOff, nsOff; } NsInfo;
static void md_ns(MetaImage *im, uint32_t off, NsInfo *n) {
    const uint8_t *b = im->md; uint32_t o = off;
    o = du(b, o, &n->parent); o = du(b, o, &n->name);
    n->typesOff = o; o = skipcoll(b, o); o = skipcoll(b, o); n->nsOff = o;
}
static uint32_t md_scope_root(MetaImage *im, uint32_t off) {
    const uint8_t *b = im->md; uint32_t o = off, n, v;
    for (int i = 0; i < 7; i++) o = skipu(b, o);         /* flags,name,hashAlg,maj,min,build,rev */
    o = du(b, o, &n); o += n;                            /* publicKey bytes */
    o = skipu(b, o);                                     /* culture */
    du(b, o, &v); return v;
}
static uint32_t coll_find(MetaImage *im, uint32_t collOff, int (*match)(MetaImage *, uint32_t, const void *), const void *ctx) {
    uint32_t n, o = du(im->md, collOff, &n), v;
    while (n--) { o = du(im->md, o, &v); if (match(im, v, ctx)) return v; }
    return 0;
}
typedef struct { const char *s; size_t n; } Span;
static int match_ns(MetaImage *im, uint32_t off, const void *c) { NsInfo n; md_ns(im, off, &n); const Span *s = c; return md_str_eq(im, n.name, s->s, s->n); }
static int match_td(MetaImage *im, uint32_t off, const void *c) { TypeDefInfo t; md_typedef(im, off, &t); const Span *s = c; return md_str_eq(im, t.name, s->s, s->n); }

uint32_t meta_find_type(MetaImage *im, const char *full) {
    const char *plus = strchr(full, '+');
    const char *lastDot = NULL;
    for (const char *p = full; *p && (!plus || p < plus); p++) if (*p == '.') lastDot = p;
    uint32_t nscope, o = du(im->md, 4, &nscope);
    for (uint32_t i = 0; i < nscope; i++) {
        uint32_t scope; o = du(im->md, o, &scope);
        uint32_t ns = md_scope_root(im, scope);
        const char *p = full;
        while (ns && lastDot && p < lastDot) {           /* walk namespace components */
            const char *q = p; while (q < lastDot && *q != '.') q++;
            Span s = { p, (size_t)(q - p) }; NsInfo ni; md_ns(im, ns, &ni);
            ns = coll_find(im, ni.nsOff, match_ns, &s);
            p = q + 1;
        }
        if (!ns) continue;
        const char *name = lastDot ? lastDot + 1 : full;
        NsInfo ni; md_ns(im, ns, &ni);
        const char *q = name; while (*q && *q != '+') q++;
        Span s = { name, (size_t)(q - name) };
        uint32_t td = coll_find(im, ni.typesOff, match_td, &s);
        while (td && *q == '+') {
            name = q + 1; q = name; while (*q && *q != '+') q++;
            Span s2 = { name, (size_t)(q - name) }; TypeDefInfo t; md_typedef(im, td, &t);
            td = coll_find(im, t.nestedOff, match_td, &s2);
        }
        if (td) return td;
    }
    return 0;
}

int meta_type_name(MetaImage *im, uint32_t td, char *out, size_t n) {
    char tmp[256]; TypeDefInfo t; md_typedef(im, td, &t);
    md_str_copy(im, t.name, tmp, sizeof tmp);
    if (t.enclosing) {
        char outer[256]; meta_type_name(im, t.enclosing, outer, sizeof outer);
        return snprintf(out, n, "%s+%s", outer, tmp);
    }
    char ns[256] = ""; uint32_t nso = md_typedef_ns(im, td); int depth = 0;
    char parts[8][64]; int np = 0;
    while (nso && depth++ < 8) {
        NsInfo ni; md_ns(im, nso, &ni);
        if (ni.name && np < 8) md_str_copy(im, ni.name, parts[np++], 64);
        if ((ni.parent & 0xff) != 0x31) break;           /* parent is another NamespaceDefinition? */
        nso = ni.parent >> 8;
    }
    for (int i = np - 1; i >= 0; i--) if (parts[i][0]) { strncat(ns, parts[i], sizeof ns - strlen(ns) - 2); strcat(ns, "."); }
    return snprintf(out, n, "%s%s", ns, tmp);
}

/* ---------- TypeMap ---------- */
typedef struct { MetaImage *im; uint32_t handle; const uint8_t *ee; const void *want; uint32_t foundTd; } TmCtx;
static int tm_cb(const uint8_t *b, uint32_t e, void *c) {
    TmCtx *x = c; uint32_t idx, h; e = du(b, e, &idx); du(b, e, &h);
    if (x->want) {
        if (extref(x->im, 8, idx) == x->want && (h >> 24) == 0x3c) { x->foundTd = h & 0xffffff; return 1; }
        return 0;
    }
    if (h == x->handle) { x->ee = extref(x->im, 8, idx); return 1; }
    return 0;
}
const uint8_t *meta_eetype(MetaImage *im, uint32_t td) {
    if (!td) return NULL;
    TmCtx c = { im, (0x3cu << 24) | td, NULL, NULL, 0 };
    hashtable_each(im->blob[1], im->blobSize[1], tm_cb, &c);
    return c.ee;
}
uint32_t meta_type_for_eetype(MetaImage *im, const void *ee) {
    TmCtx c = { im, 0, NULL, ee, 0 };
    hashtable_each(im->blob[1], im->blobSize[1], tm_cb, &c);
    return c.foundTd;
}

/* ---------- fields / methods ---------- */
static uint32_t md_member_named(MetaImage *im, uint32_t collOff, int skipBeforeName, const char *name) {
    uint32_t n, o = du(im->md, collOff, &n), v; size_t len = strlen(name);
    while (n--) {
        o = du(im->md, o, &v);
        uint32_t p = v, nm;
        for (int i = 0; i < skipBeforeName; i++) p = skipu(im->md, p);
        du(im->md, p, &nm);
        if (md_str_eq(im, nm, name, len)) return v;
    }
    return 0;
}
typedef struct { MetaImage *im; const uint8_t *ee; uint32_t fieldMd; int offset; } FamCtx;
static int fam_cb(const uint8_t *b, uint32_t e, void *c) {
    FamCtx *x = c; uint32_t fl, idx, fm, off;
    e = du(b, e, &fl); e = du(b, e, &idx);
    if (!(fl & 0x08)) return 0;
    e = du(b, e, &fm);
    if (fm != x->fieldMd) return 0;
    if (x->ee && extref(x->im, 8, idx) != x->ee) return 0;     /* Field records are shared between types */
    if ((fl & 3) != 0 || (fl & 0x44) || !(fl & 0x20)) return 0;   /* instance, non-canonical, direct offset */
    du(b, e, &off); x->offset = (int)off; return 1;
}
int meta_field_offset(MetaImage *im, uint32_t td, const char *field) {
    if (!td) return -1;
    TypeDefInfo t; md_typedef(im, td, &t);
    uint32_t f = md_member_named(im, t.fieldsOff, 1, field);       /* Field: flags, name */
    if (!f) return -1;
    FamCtx c = { im, meta_eetype(im, td), f, -1 };
    hashtable_each(im->blob[9], im->blobSize[9], fam_cb, &c);
    return c.offset;
}
typedef struct { MetaImage *im; uint32_t methodMd; const uint8_t *ep; } ImCtx;
static int im_cb(const uint8_t *b, uint32_t e, void *c) {
    ImCtx *x = c; uint32_t fl, mh, dt, ei;
    e = du(b, e, &fl); e = du(b, e, &mh); e = du(b, e, &dt);
    if (!(fl & 0x04) || !(fl & 0x20) || mh != x->methodMd) return 0;
    du(b, e, &ei); x->ep = extref(x->im, 8, ei); return 1;
}
const uint8_t *meta_method_entry(MetaImage *im, uint32_t td, const char *method) {
    if (!td) return NULL;
    TypeDefInfo t; md_typedef(im, td, &t);
    uint32_t m = md_member_named(im, t.methodsOff, 2, method);     /* Method: flags, implFlags, name */
    if (!m) return NULL;
    ImCtx c = { im, m, NULL };
    hashtable_each(im->blob[6], im->blobSize[6], im_cb, &c);
    return c.ep;
}

typedef struct { MetaImage *im; const uint8_t *ee; uint32_t fieldMd; const uint8_t *cookie; int offset, storage, found; } SfCtx;
static int sf_cb(const uint8_t *b, uint32_t e, void *c) {
    SfCtx *x = c; uint32_t fl, idx, fm, v;
    e = du(b, e, &fl); e = du(b, e, &idx);
    if (!(fl & 0x08) || (fl & 3) == 0 || (fl & 0x44) || (fl & 0x20)) return 0;
    e = du(b, e, &fm);
    if (fm != x->fieldMd || (x->ee && extref(x->im, 8, idx) != x->ee)) return 0;
    e = du(b, e, &v); x->cookie = extref(x->im, 8, v); x->storage = (int)(fl & 3);
    x->offset = 0;
    /* this compiler version marks GC statics as storage 1 + flag 0x10; newer CoreRT uses storage 2 */
    if ((fl & 0x10) || (fl & 3) == 2 || (fl & 3) == 3) { du(b, e, &v); x->offset = (int)v; if (fl & 0x10) x->storage = 2; }
    x->found = 1; return 1;
}
int meta_static_field(MetaImage *im, uint32_t td, const char *field, const uint8_t **cookie, int *offset, int *storage) {
    if (!td) return 0;
    TypeDefInfo t; md_typedef(im, td, &t);
    uint32_t f = md_member_named(im, t.fieldsOff, 1, field);
    if (!f) return 0;
    SfCtx c = { im, meta_eetype(im, td), f, NULL, 0, 0, 0 };
    hashtable_each(im->blob[9], im->blobSize[9], sf_cb, &c);
    if (!c.found) return 0;
    *cookie = c.cookie; *offset = c.offset; *storage = c.storage; return 1;
}
