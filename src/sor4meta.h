/* In-process reader for the game's NativeAOT/CoreRT reflection data (RTR header, embedded metadata,
 * TypeMap, FieldAccessMap, InvokeMap). Works on the live, mapped SOR4.exe image. Read-only. */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    const uint8_t *base; size_t size; uint64_t prefBase; uint32_t timestamp;
    struct { uint32_t rva, vsize; char name[9]; } sec[24]; int nsec;
    const uint8_t *rtr; int rtrMajor, rtrMinor;
    const uint8_t *blob[64]; uint32_t blobSize[64];    /* ReflectionMapBlob id -> data */
    const uint8_t *gcStaticRegion; uint32_t gcStaticRegionSize;
    const uint8_t *md; uint32_t mdSize;
} MetaImage;

int  meta_open(MetaImage *im, const void *moduleBase, char *err, size_t errlen);
/* full name like "BeatThemAll.Gameplay.Character" or "CommonLib.online+PlayerSlot"; returns metadata offset or 0 */
uint32_t meta_find_type(MetaImage *im, const char *fullName);
const uint8_t *meta_eetype(MetaImage *im, uint32_t typeDefOffset);           /* NULL if none */
uint32_t meta_type_for_eetype(MetaImage *im, const void *eetype);            /* reverse lookup, 0 if none */
int  meta_field_offset(MetaImage *im, uint32_t typeDefOffset, const char *field); /* -1 if not found */
const uint8_t *meta_method_entry(MetaImage *im, uint32_t typeDefOffset, const char *method);
/* static field: *cookie = GC static cell / non-GC data address, *offset inside the statics object, *storage 1..3 */
int  meta_static_field(MetaImage *im, uint32_t typeDefOffset, const char *field, const uint8_t **cookie, int *offset, int *storage);
int  meta_type_name(MetaImage *im, uint32_t typeDefOffset, char *out, size_t n);
const uint8_t *meta_fix_ptr(MetaImage *im, uint64_t v);   /* rebases if the image is not relocated */
int  meta_in_image(MetaImage *im, const void *p);
const char *meta_section_of(MetaImage *im, const void *p);
