/* The translated game code as a separate module (tmnt_code.so, made by the user from their TMNT_EN.EXE): what it hands the runtime. */
#ifndef TMNT_CODE_SO_H
#define TMNT_CODE_SO_H

#include <stdint.h>

#include "guest.h"

#define TMNT_CODE_MAGIC 0x434d4e54U   /* "TNMC" */
#define TMNT_CODE_VERSION 1

typedef struct { uint32_t slot_rva; const char *name; } guest_import_slot;

/* Translated functions the runtime calls by name (rt/game_tmnt.c). */
#define TMNT_CODE_DIRECT(X) \
    X(sub_000a64aa) X(sub_000a72cb) X(sub_000b8d10) X(sub_000b8de0) X(sub_000cc050) X(sub_000ef9f0) X(sub_000f52f0) X(sub_000f5880) \
    X(sub_000f5960) X(sub_000fca50) X(sub_000fd570) X(sub_00118cd0) X(sub_00185000) X(sub_001850ef) X(sub_001852a0)

typedef struct {
    uint32_t magic, version;
    const uint32_t *func_rvas;
    const guest_fn *func_ptrs;
    const uint32_t *func_count, *image_entry, *image_size, *import_count;
    const guest_import_slot *import_slots;
#define TMNT_CODE_FIELD(name) guest_fn name;
    TMNT_CODE_DIRECT(TMNT_CODE_FIELD)
#undef TMNT_CODE_FIELD
} tmnt_code_exports;

#ifdef TMNT_CODE_SO
extern const tmnt_code_exports *tmnt_code_mod;
/* Loads tmnt_code.so from the data folder; on failure shows why and closes the app. */
const tmnt_code_exports *code_so_load(const char *path);
#endif

#endif
