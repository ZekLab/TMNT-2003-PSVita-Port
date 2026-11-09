/* The game's memory image made from TMNT_EN.EXE at start-up, as recomp/gen_tables.py writes image.bin: the PE laid out by RVA plus CODE_PATCHES. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pe_image.h"

#define EXE_SIZE 2150400U    /* TMNT_EN.EXE, English/Russian release */
#define EXE_CRC32 0x43ad59b9U  /* the same file; TMNT_RU.EXE has the same size and layout but other code */

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    uint32_t c = 0xffffffffU;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320U & (0U - (c & 1U)));
    }
    return ~c;
}

/* recomp/tmnt_image.py CODE_PATCHES: RVA, bytes expected, bytes written. */
static const struct { uint32_t rva; uint8_t old[4], now[4]; } patches[] = {
    { 0x9865a, { 0x08, 0x07, 0x00, 0x00 }, { 0xff, 0xff, 0xff, 0xff } },   /* no attract demo on the PRESS START screen */
};

int pe_image_load(const char *path, uint8_t *dst, uint32_t image_size, uint32_t entry, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "%s not found", path);
        return 0;
    }
    uint8_t *file = malloc(EXE_SIZE + 1);
    size_t n = file ? fread(file, 1, EXE_SIZE + 1, f) : 0;
    fclose(f);
    int ok = 0;
    uint32_t pe = n >= 0x40 ? rd32(file + 0x3c) : 0;
    if (n != EXE_SIZE || file[0] != 'M' || file[1] != 'Z' || pe > n - 0xf8 || memcmp(file + pe, "PE\0\0", 4) != 0 || crc32_of(file, n) != EXE_CRC32) {
        snprintf(err, errlen, "%s is not the supported TMNT_EN.EXE (%u bytes, expected %u)", path, (unsigned)n, EXE_SIZE);
        goto out;
    }
    const uint8_t *coff = file + pe + 4, *opt = coff + 20;
    uint16_t nsect = rd16(coff + 2), optsize = rd16(coff + 16);
    uint32_t exe_entry = rd32(opt + 16), size_of_image = rd32(opt + 56), size_of_headers = rd32(opt + 60);
    if (exe_entry != entry || size_of_image != image_size || size_of_headers > image_size) {
        snprintf(err, errlen, "%s does not match tmnt_code.so (entry %x, size %x)", path, exe_entry, size_of_image);
        goto out;
    }
    memset(dst, 0, image_size);
    memcpy(dst, file, size_of_headers);
    const uint8_t *sh = opt + optsize;
    for (uint16_t i = 0; i < nsect; i++, sh += 40) {
        uint32_t vsize = rd32(sh + 8), rva = rd32(sh + 12), raw_size = rd32(sh + 16), raw_ptr = rd32(sh + 20);
        if (raw_ptr > n) raw_ptr = (uint32_t)n;
        uint32_t len = raw_ptr + raw_size > n ? (uint32_t)n - raw_ptr : raw_size;
        if (vsize && vsize < len) len = vsize;
        if (rva > image_size || len > image_size - rva) {
            snprintf(err, errlen, "%s: section %u lies outside the image", path, i);
            goto out;
        }
        memcpy(dst + rva, file + raw_ptr, len);
    }
    for (size_t i = 0; i < sizeof patches / sizeof patches[0]; i++) {
        if (memcmp(dst + patches[i].rva, patches[i].old, 4) != 0) {
            snprintf(err, errlen, "%s: unexpected bytes at %x", path, patches[i].rva);
            goto out;
        }
        memcpy(dst + patches[i].rva, patches[i].now, 4);
    }
    ok = 1;
out:
    free(file);
    return ok;
}
