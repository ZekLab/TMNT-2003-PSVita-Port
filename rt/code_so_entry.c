/* Part of tmnt_code.so: the table the Vita runtime reads from the module (its ELF entry point). */
#include "code_so.h"

extern const uint32_t guest_func_rvas[];
extern const guest_fn guest_func_ptrs[];
extern const uint32_t guest_func_count;
extern const uint32_t guest_image_entry;
extern const uint32_t guest_image_size;
extern const guest_import_slot guest_import_slots[];
extern const uint32_t guest_import_count;

#define TMNT_CODE_DECL(name) void name(CPU *__restrict c);
TMNT_CODE_DIRECT(TMNT_CODE_DECL)
#undef TMNT_CODE_DECL

__attribute__((visibility("default"))) const tmnt_code_exports tmnt_code = {
    TMNT_CODE_MAGIC, TMNT_CODE_VERSION, guest_func_rvas, guest_func_ptrs, &guest_func_count, &guest_image_entry, &guest_image_size,
    &guest_import_count, guest_import_slots,
#define TMNT_CODE_INIT(name) name,
    TMNT_CODE_DIRECT(TMNT_CODE_INIT)
#undef TMNT_CODE_INIT
};
