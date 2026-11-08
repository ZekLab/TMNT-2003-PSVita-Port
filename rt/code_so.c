/* Vita: loads tmnt_code.so (the translated game code the user made from their TMNT_EN.EXE) into executable memory and links it to the runtime. */
#ifdef TMNT_CODE_SO
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <psp2/kernel/sysmem.h>

#include "code_so.h"
#include "game_hooks.h"
#include "plat.h"
#include "runtime.h"

#define VM_BLOCK_MAX (16U << 20)   /* the system limit for one executable block */

const tmnt_code_exports *tmnt_code_mod;

/* Helpers the compiler may call (ARM EABI, libgcc and newlib), declared only to take their address. */
#define EABI_HELPERS(X) \
    X(__aeabi_idiv) X(__aeabi_uidiv) X(__aeabi_idivmod) X(__aeabi_uidivmod) X(__aeabi_ldivmod) X(__aeabi_uldivmod) \
    X(__aeabi_d2lz) X(__aeabi_d2ulz) X(__aeabi_f2lz) X(__aeabi_f2ulz) X(__aeabi_l2d) X(__aeabi_ul2d) X(__aeabi_l2f) X(__aeabi_ul2f) \
    X(__aeabi_llsl) X(__aeabi_llsr) X(__aeabi_lasr) X(__aeabi_lcmp) X(__aeabi_ulcmp) \
    X(__aeabi_memcpy) X(__aeabi_memcpy4) X(__aeabi_memcpy8) X(__aeabi_memmove) X(__aeabi_memmove4) X(__aeabi_memmove8) \
    X(__aeabi_memset) X(__aeabi_memset4) X(__aeabi_memset8) X(__aeabi_memclr) X(__aeabi_memclr4) X(__aeabi_memclr8) \
    X(__popcountsi2) X(__paritysi2) X(__clzdi2) X(__ctzdi2)
#define EABI_DECL(name) extern void name(void);
EABI_HELPERS(EABI_DECL)
#undef EABI_DECL

/* Everything else the translated code may import: libc, libm and the runtime (guest.h, game_hooks.h). */
#define CODE_IMPORTS(X) \
    X(memcpy) X(memmove) X(memset) X(memcmp) X(strlen) X(wcslen) \
    X(sin) X(cos) X(tan) X(atan) X(atan2) X(sqrt) X(fabs) X(trunc) X(floor) X(ceil) X(round) X(rint) X(nearbyint) X(remainder) \
    X(fmod) X(ldexp) X(frexp) X(log) X(log2) X(log10) X(exp) X(exp2) X(pow) \
    X(sinf) X(cosf) X(atan2f) X(sqrtf) X(fabsf) X(truncf) X(floorf) X(ceilf) X(roundf) X(fmodf) \
    X(guest_membase) X(guest_trace_on) X(guest_call) X(guest_cpuid) X(guest_fault) X(guest_fs_base) X(guest_int3) X(guest_rdtsc) \
    X(guest_trace_enter) \
    X(tmnt_fast_anim_curve) X(tmnt_fast_getptd) X(tmnt_fast_hanim_attach) X(tmnt_fast_rw_flush_states) X(tmnt_fast_rw_invsqrt) \
    X(tmnt_fast_rw_matrix_multiply) X(tmnt_fast_rw_v3d_length) X(tmnt_fast_rw_xform_point) X(tmnt_fast_rw_xform_points) \
    X(tmnt_fast_sfd_skip) X(tmnt_fast_sfd_yuv_argb) X(tmnt_fast_skin1) X(tmnt_fast_skin2) X(tmnt_fast_skin4) X(tmnt_fast_sphere_tri) \
    X(tmnt_fast_ticks) X(tmnt_fast_toon_ink_quad) X(tmnt_fast_toon_ink_vertex) \
    X(tmnt_hook_draw) X(tmnt_hook_logic) X(tmnt_hook_pad_settings) X(tmnt_hook_present) X(tmnt_hook_set_next_area) \
    X(tmnt_hook_sfd_frame) X(tmnt_hook_skip_check) X(tmnt_hook_start_area) X(tmnt_hook_title) \
    X(tmnt_region_pad_label) X(tmnt_region_pad_label_check) X(tmnt_region_toon_cam_vectors) X(tmnt_region_toon_cam_vectors_check) \
    X(tmnt_region_toon_copy_nrm) X(tmnt_region_toon_copy_nrm_check) X(tmnt_region_toon_copy_pos) X(tmnt_region_toon_copy_pos_check) \
    X(tmnt_region_toon_edges) X(tmnt_region_toon_edges_check) X(tmnt_region_toon_face_normals) \
    X(tmnt_region_toon_face_normals_check) X(tmnt_region_toon_facing) X(tmnt_region_toon_facing_check)

typedef struct { const char *name; const void *addr; } code_import;
#define IMPORT_ENTRY(name) { #name, (const void *)&name },
static const code_import imports[] = { EABI_HELPERS(IMPORT_ENTRY) CODE_IMPORTS(IMPORT_ENTRY) };
#undef IMPORT_ENTRY

/* The runtime's calls into the translated code go through the module's table. */
#define CODE_TRAMPOLINE(name) void name(CPU *__restrict c) { tmnt_code_mod->name(c); }
TMNT_CODE_DIRECT(CODE_TRAMPOLINE)
#undef CODE_TRAMPOLINE

typedef struct { uint8_t ident[16]; uint16_t type, machine; uint32_t version, entry, phoff, shoff, flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } elf_ehdr;
typedef struct { uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align; } elf_phdr;
typedef struct { uint32_t name, value, size; uint8_t info, other; uint16_t shndx; } elf_sym;
typedef struct { int32_t tag; uint32_t val; } elf_dyn;
typedef struct { uint32_t offset, info; } elf_rel;

enum { PT_LOAD = 1, PT_DYNAMIC = 2 };
enum { DT_NULL = 0, DT_PLTRELSZ = 2, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7, DT_REL = 17, DT_RELSZ = 18, DT_JMPREL = 23, DT_RELR = 36 };
enum { R_ARM_ABS32 = 2, R_ARM_GLOB_DAT = 21, R_ARM_JUMP_SLOT = 22, R_ARM_RELATIVE = 23 };

static int vm_open;

static void fail(const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (vm_open) sceKernelCloseVMDomain();
    plat_fatal(msg);
}

static const void *find_import(const char *name)
{
    for (size_t i = 0; i < sizeof imports / sizeof imports[0]; i++)
        if (strcmp(imports[i].name, name) == 0) return imports[i].addr;
    return NULL;
}

static void relocate(uint8_t *base, uint32_t span, const elf_rel *rel, uint32_t bytes, const elf_sym *syms, const char *strs)
{
    for (uint32_t i = 0; i < bytes / sizeof *rel; i++) {
        uint32_t type = rel[i].info & 0xff, s = rel[i].info >> 8;
        if (rel[i].offset > span - 4) fail("tmnt_code.so is damaged (relocation outside the module).\n\nMake it again on your computer.");
        uint32_t *where = (uint32_t *)(base + rel[i].offset), value = 0;
        if (type != R_ARM_RELATIVE) {
            const elf_sym *sym = &syms[s];
            if (sym->shndx) {
                value = (uint32_t)base + sym->value;
            } else {
                const void *p = find_import(strs + sym->name);
                if (!p)
                    fail("tmnt_code.so needs \"%s\", which this version of the port does not provide.\n\n"
                         "Make tmnt_code.so with the tools of the same release as the VPK.", strs + sym->name);
                value = (uint32_t)p;
            }
        }
        switch (type) {
        case R_ARM_RELATIVE: *where += (uint32_t)base; break;
        case R_ARM_ABS32: *where += value; break;
        case R_ARM_GLOB_DAT:
        case R_ARM_JUMP_SLOT: *where = value; break;
        default: fail("tmnt_code.so uses relocation type %u, which the loader does not handle.\n\nMake it with the tools of the same release as the VPK.", type);
        }
    }
}

const tmnt_code_exports *code_so_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        fail("The game code (tmnt_code.so) is missing from ux0:data/TMNT2003/.\n\n"
             "Make it on your computer from your copy of the game with tools/make_vita_files.py (see the README), then copy it there.");
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = size > (long)sizeof(elf_ehdr) ? malloc((size_t)size) : NULL;
    int ok = file && fread(file, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    const elf_ehdr *eh = (const elf_ehdr *)file;
    if (!ok || memcmp(eh->ident, "\x7f" "ELF\x01\x01", 6) != 0 || eh->type != 3 || eh->machine != 40 ||
        eh->phoff + (uint32_t)eh->phnum * sizeof(elf_phdr) > (uint32_t)size)
        fail("tmnt_code.so is not a Vita game module (or could not be read).\n\nMake it again on your computer.");

    const elf_phdr *ph = (const elf_phdr *)(file + eh->phoff);
    uint32_t span = 0, dyn = 0;
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type == PT_LOAD) {
            if (ph[i].offset + ph[i].filesz > (uint32_t)size || ph[i].filesz > ph[i].memsz) fail("tmnt_code.so is damaged.\n\nMake it again on your computer.");
            if (ph[i].vaddr + ph[i].memsz > span) span = ph[i].vaddr + ph[i].memsz;
        } else if (ph[i].type == PT_DYNAMIC) {
            dyn = ph[i].vaddr;
        }
    }
    uint32_t block = (span + 0xfffffU) & ~0xfffffU;
    if (!dyn || !span || block > VM_BLOCK_MAX) fail("tmnt_code.so has an unexpected layout (%u KB).\n\nMake it with the tools of the same release as the VPK.", span >> 10);

    SceUID uid = sceKernelAllocMemBlockForVM("tmnt_code", block);
    uint8_t *base = NULL;
    if (uid < 0 || sceKernelGetMemBlockBase(uid, (void **)&base) < 0) fail("Not enough memory for the game code (error %08x).", (unsigned)uid);

    sceKernelOpenVMDomain();
    vm_open = 1;
    memset(base, 0, block);
    for (int i = 0; i < eh->phnum; i++)
        if (ph[i].type == PT_LOAD) memcpy(base + ph[i].vaddr, file + ph[i].offset, ph[i].filesz);
    uint32_t rel = 0, relsz = 0, jmprel = 0, pltrelsz = 0, symtab = 0, strtab = 0;
    for (const elf_dyn *d = (const elf_dyn *)(base + dyn); d->tag != DT_NULL; d++) {
        switch (d->tag) {
        case DT_REL: rel = d->val; break;
        case DT_RELSZ: relsz = d->val; break;
        case DT_JMPREL: jmprel = d->val; break;
        case DT_PLTRELSZ: pltrelsz = d->val; break;
        case DT_SYMTAB: symtab = d->val; break;
        case DT_STRTAB: strtab = d->val; break;
        case DT_RELA:
        case DT_RELR: fail("tmnt_code.so uses a relocation format the loader does not handle.\n\nMake it with the tools of the same release as the VPK."); break;
        }
    }
    const elf_sym *syms = (const elf_sym *)(base + symtab);
    const char *strs = (const char *)(base + strtab);
    if (rel) relocate(base, span, (const elf_rel *)(base + rel), relsz, syms, strs);
    if (jmprel) relocate(base, span, (const elf_rel *)(base + jmprel), pltrelsz, syms, strs);
    sceKernelCloseVMDomain();
    vm_open = 0;
    sceKernelSyncVMDomain(uid, base, block);

    const tmnt_code_exports *x = (const tmnt_code_exports *)(base + eh->entry);
    if (eh->entry > span - sizeof *x || x->magic != TMNT_CODE_MAGIC || x->version != TMNT_CODE_VERSION)
        fail("tmnt_code.so does not match this version of the port.\n\nMake it with the tools of the same release as the VPK.");
    free(file);
    rt_log("[code] %s: %u KB at %p, %u relocations, %u functions\n", path, span >> 10, (void *)base, (relsz + pltrelsz) / 8,
           *x->func_count);
    tmnt_code_mod = x;
    return x;
}
#endif
