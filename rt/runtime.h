/* TMNT 2003 host runtime: guest memory map, thunks and the Win32 layer interface. */
#ifndef TMNT_RUNTIME_H
#define TMNT_RUNTIME_H

#include <stdint.h>
#include "guest.h"

#define GUEST_SPACE (512U << 20)
#define GUEST_TEB 0x00010000U
#define GUEST_PEB 0x00011000U
#define GUEST_SCRATCH 0x00012000U
#undef GUEST_IMAGE_BASE
#define GUEST_IMAGE_BASE 0x00400000U
#define GUEST_STACK_TOP 0x01000000U
#define GUEST_STACK_BYTES (6U << 20)
#define GUEST_HEAP_LO 0x01000000U

/* Thunks: guest-callable addresses served by host code (imports, COM methods, callbacks). */
#define RT_THUNK_BASE 0xF0000000U
#define RT_MAX_THUNKS 8192

typedef struct rt_thunk {
    const char *name;
    uint32_t (*fn)(CPU *__restrict c);
    int nargs;
} rt_thunk;

/* Argument n of a stdcall/cdecl call, read from the guest stack above the return address. */
#define ARG(n) ld32(c->esp + 4U + 4U * (uint32_t)(n))
#define GSTR(a) ((const char *)G2H(a))

void rt_log(const char *fmt, ...);
void rt_backtrace(CPU *__restrict c);
uint32_t rt_add_thunk(const rt_thunk *t);
void rt_thunk_call(CPU *__restrict c, uint32_t addr);
const rt_thunk *rt_find_import(const char *name);
void rt_win32_init(CPU *__restrict c);
uint32_t rt_current_teb(void);
void rt_safepoint(void);
uint32_t rt_current_tid(void);
uint32_t rt_call_guest(CPU *__restrict c, uint32_t fn, int nargs, const uint32_t *args);

uint32_t rt_alloc(uint32_t n);
void rt_free(uint32_t p);
uint32_t rt_realloc(uint32_t p, uint32_t n);
uint32_t rt_alloc_size(uint32_t p);
uint32_t rt_heap_used(void);

#endif
