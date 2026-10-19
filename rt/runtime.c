/* TMNT 2003 host runtime: guest address space, image loader, dispatch, CPU probes, guest heap and the entry point. */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "guest.h"
#include "runtime.h"

uint8_t *guest_membase;
uint32_t g_guest_fs_base_cached;

extern const uint32_t guest_func_rvas[];
extern const guest_fn guest_func_ptrs[];
extern const uint32_t guest_func_count;
extern const uint32_t guest_image_entry;
extern const uint32_t guest_image_size;

typedef struct { uint32_t slot_rva; const char *name; } guest_import_slot;
extern const guest_import_slot guest_import_slots[];
extern const uint32_t guest_import_count;

__thread const rt_thunk *rt_cur_thunk;
static pthread_mutex_t heap_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
static int trace_imports;

void rt_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

void guest_fault(CPU *__restrict c, uint32_t addr, const char *what)
{
    rt_log("[fault] rva %08x: %s (eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x)\n",
           addr, what, c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi);
    rt_backtrace(c);
    exit(3);
}

void guest_int3(CPU *__restrict c, uint32_t addr)
{
    guest_fault(c, addr, "int3");
}

int guest_stack_violation(CPU *__restrict c, uint32_t pc, uint32_t kind, uint32_t address, uint32_t size)
{
    rt_log("[stack] violation kind %u at %08x size %u pc %08x\n", kind, address, size, pc);
    guest_fault(c, pc, "stack violation");
    return 0;
}

int guest_stack_owner_violation(CPU *__restrict c, uint32_t pc)
{
    guest_fault(c, pc, "stack owner violation");
    return 0;
}

/* A plain Pentium II: FPU, TSC, CX8, CMOV; no MMX/SSE/3DNow!, so RenderWare and the CRT take their x87 paths. */
void guest_cpuid(CPU *__restrict c)
{
    switch (c->eax) {
    case 0:
        c->eax = 1;
        c->ebx = 0x756e6547U; c->edx = 0x49656e69U; c->ecx = 0x6c65746eU;
        break;
    case 1:
        c->eax = 0x00000652U; c->ebx = 0; c->ecx = 0; c->edx = 0x00008111U;
        break;
    default:
        c->eax = 0; c->ebx = 0; c->ecx = 0; c->edx = 0;
        break;
    }
}

void guest_xgetbv(CPU *__restrict c)
{
    c->eax = 0; c->edx = 0;
}

uint64_t guest_rdtsc(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 500000000ULL + (uint64_t)ts.tv_nsec / 2U;
}

uint32_t guest_fs_base(CPU *__restrict c)
{
    (void)c;
    return rt_current_teb();
}

/* ------------------------------------------------------------- tracing --- */

volatile int guest_trace_on;
static uint32_t trace_rvas[64];
static int trace_count;

static void trace_init(void)
{
    const char *s = getenv("TMNT_TRACE_FUNCS");
    while (s && *s && trace_count < 64) {
        uint32_t v = (uint32_t)strtoul(s, (char **)&s, 16);
        trace_rvas[trace_count++] = v >= GUEST_IMAGE_BASE ? v - GUEST_IMAGE_BASE : v;
        while (*s == ',' || *s == ' ') s++;
    }
    guest_trace_on = trace_count > 0;
}

void guest_trace_enter(CPU *c, uint32_t rva)
{
    for (int i = 0; i < trace_count; i++) {
        if (trace_rvas[i] == rva) {
            rt_log("[call:%u] %08x(%08x, %08x, %08x, %08x) eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x from %08x\n", rt_current_tid(), rva + GUEST_IMAGE_BASE,
                   ld32(c->esp + 4), ld32(c->esp + 8), ld32(c->esp + 12), ld32(c->esp + 16), c->eax, c->ebx, c->ecx, c->edx, c->esi, c->edi, ld32(c->esp));
            for (int k = 0; k < 2; k++) {
                uint32_t a = ld32(c->esp + 4U + 4U * (uint32_t)k);
                if (a < GUEST_IMAGE_BASE || a >= GUEST_SPACE - 64) continue;
                char s[48];
                int n = 0;
                while (n < 47 && ld8(a + (uint32_t)n) >= 0x20 && ld8(a + (uint32_t)n) < 0x7f) { s[n] = (char)ld8(a + (uint32_t)n); n++; }
                s[n] = 0;
                if (n >= 3) rt_log("[call]   arg%d \"%s\"\n", k, s);
            }
            return;
        }
    }
}

/* ------------------------------------------------------------ self test --- */

static uint32_t call_cdecl(CPU *c, uint32_t va, int n, const uint32_t *args)
{
    uint32_t saved = c->esp;
    for (int i = n - 1; i >= 0; i--) { c->esp -= 4; st32(c->esp, args[i]); }
    c->esp -= 4;
    st32(c->esp, 0xFFFFFFF0U);
    guest_call(c, va);
    c->esp = saved;
    return c->eax;
}

/* TMNT_SELFTEST=1: runs the CRT string routines (hand-written x86 assembly) against libc on fixed inputs. */
static void selftest(CPU *c)
{
    static const char *strs[] = { "fonts/font.met", "./data/x", "a", "", "abcdefgh:ij", "texture/pause/pc/pause.txd", "Mixed:Case" };
    uint32_t buf = rt_alloc(4096);
    int bad = 0;
    for (unsigned i = 0; i < sizeof strs / sizeof strs[0]; i++) {
        for (unsigned off = 0; off < 4; off++) {
            uint32_t a = buf + off, b = buf + 1024 + ((off + 1) & 3);
            strcpy((char *)G2H(a), strs[i]);
            strcpy((char *)G2H(b), strs[(i + 1) % 7]);
            uint32_t args[3];
            args[0] = a; args[1] = ':';
            uint32_t r = call_cdecl(c, 0x567060, 2, args);
            const char *e = strchr(strs[i], ':');
            uint32_t want = e ? a + (uint32_t)(e - strs[i]) : 0;
            if (r != want) { bad++; rt_log("[selftest] strchr(\"%s\"+%u) = %08x, want %08x\n", strs[i], off, r, want); }
            args[0] = a; args[1] = a;
            r = call_cdecl(c, 0x565cc0, 2, args);
            if (r != 0) { bad++; rt_log("[selftest] strcmp(same \"%s\"+%u) = %d\n", strs[i], off, (int)r); }
            args[0] = a; args[1] = b;
            int sr = (int)call_cdecl(c, 0x565cc0, 2, args), se = strcmp(strs[i], strs[(i + 1) % 7]);
            if ((sr < 0) != (se < 0) || (sr == 0) != (se == 0)) { bad++; rt_log("[selftest] strcmp(\"%s\", \"%s\") = %d, want sign of %d\n", strs[i], strs[(i + 1) % 7], sr, se); }
            args[0] = a; args[1] = b; args[2] = 7;
            sr = (int)call_cdecl(c, 0x566e00, 3, args); se = strncmp(strs[i], strs[(i + 1) % 7], 7);
            if ((sr < 0) != (se < 0) || (sr == 0) != (se == 0)) { bad++; rt_log("[selftest] strncmp(\"%s\", \"%s\", 7) = %d, want sign of %d\n", strs[i], strs[(i + 1) % 7], sr, se); }
        }
    }
    rt_log("[selftest] %d failures\n", bad);
    rt_free(buf);
}


/* TMNT_SELFTEST_POW=1: checks the translated CRT _CIpow once the CRT is initialised (called from Direct3DCreate9). */
void rt_selftest_pow(CPU *c)
{
    int bad = 0;
    int saved_top = c->st_top;
    double saved_st[8];
    memcpy(saved_st, c->st, sizeof saved_st);
    /* _CIpow: x in ST(1), y in ST(0), result in ST(0) (x87 path while __sse2_available is 0). */
    static const double pv[][2] = { { 0.5, 1.0 }, { 0.25, 0.5 }, { 2.0, 10.0 }, { 0.1, 2.2 }, { 0.7, 1.0 / 2.2 }, { 3.0, 0.5 } };
    for (unsigned i = 0; i < sizeof pv / sizeof pv[0]; i++) {
        uint32_t a[4];
        memcpy(a, &pv[i][0], 8);
        memcpy(a + 2, &pv[i][1], 8);
        int top = c->st_top;
        call_cdecl(c, 0x566180, 4, a);
        double r = *fst(c, 0), want = pow(pv[i][0], pv[i][1]);
        if (c->st_top == top) rt_log("[selftest] pow left the x87 stack unchanged\n");
        else (void)fpop(c);
        if (fabs(r - want) > 1e-9 * fabs(want) + 1e-12) { bad++; rt_log("[selftest] pow(%g, %g) = %.12g, want %.12g\n", pv[i][0], pv[i][1], r, want); }
    }
    /* One-argument CRT math entry points (cdecl double, result in ST(0)). */
    static const struct { uint32_t va; const char *name; double (*ref)(double); } fns[] = {
        { 0x565e94, "cos", cos }, { 0x566074, "sin", sin }, { 0x566854, "sqrt", sqrt },
        { 0x567204, "acos?", acos }, { 0x567134, "asin?", asin },
    };
    static const double xs[] = { 0.0, 0.5, -0.3, 1.0, 0.999, 2.5, -7.0, 100.0 };
    for (unsigned f = 0; f < sizeof fns / sizeof fns[0]; f++) {
        for (unsigned k = 0; k < sizeof xs / sizeof xs[0]; k++) {
            uint32_t a[2];
            memcpy(a, &xs[k], 8);
            int top = c->st_top;
            call_cdecl(c, fns[f].va, 2, a);
            double r = c->st_top != top ? fpop(c) : NAN, want = fns[f].ref(xs[k]);
            int same = (r != r && want != want) || fabs(r - want) <= 1e-9 * fabs(want) + 1e-12;
            if (!same) { bad++; rt_log("[selftest] %s(%g) = %.12g, want %.12g\n", fns[f].name, xs[k], r, want); }
        }
    }
    rt_log("[selftest] pow: %d failures\n", bad);
    memcpy(c->st, saved_st, sizeof saved_st);
    c->st_top = saved_top;
}

/* ------------------------------------------------------------ dispatch --- */

static guest_fn lookup(uint32_t rva)
{
    uint32_t lo = 0, hi = guest_func_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (guest_func_rvas[mid] < rva)
            lo = mid + 1;
        else
            hi = mid;
    }
    return (lo < guest_func_count && guest_func_rvas[lo] == rva) ? guest_func_ptrs[lo] : NULL;
}

#define CALLSTACK_DEPTH 64
static __thread uint32_t callstack[CALLSTACK_DEPTH];
static __thread int callstack_top;

void rt_backtrace(CPU *__restrict c)
{
    (void)c;
    int n = callstack_top < CALLSTACK_DEPTH ? callstack_top : CALLSTACK_DEPTH;
    rt_log("[fault] last indirect calls:");
    for (int i = 1; i <= n && i <= 12; i++)
        rt_log(" %08x", callstack[(callstack_top - i) & (CALLSTACK_DEPTH - 1)]);
    rt_log("\n");
}

void guest_call(CPU *__restrict c, uint32_t addr)
{
    callstack[callstack_top++ & (CALLSTACK_DEPTH - 1)] = addr;
    if (addr >= RT_THUNK_BASE) {
        rt_thunk_call(c, addr);
        return;
    }
    guest_fn fn = (addr >= GUEST_IMAGE_BASE) ? lookup(addr - GUEST_IMAGE_BASE) : NULL;
    if (fn == NULL) {
        char what[64];
        snprintf(what, sizeof what, "call to untranslated address %08x", addr);
        guest_fault(c, addr, what);
        return;
    }
    fn(c);
}

/* Imports and host callbacks are thunk addresses above RT_THUNK_BASE, 16 bytes apart. */
static const rt_thunk *thunks[RT_MAX_THUNKS];
static uint32_t thunk_count;

uint32_t rt_add_thunk(const rt_thunk *t)
{
    if (thunk_count >= RT_MAX_THUNKS) {
        rt_log("[rt] out of thunks\n");
        exit(2);
    }
    thunks[thunk_count] = t;
    return RT_THUNK_BASE + 16U * thunk_count++;
}

void rt_thunk_call(CPU *__restrict c, uint32_t addr)
{
    uint32_t i = (addr - RT_THUNK_BASE) / 16U;
    if (i >= thunk_count || thunks[i] == NULL) {
        guest_fault(c, addr, "call to an unknown thunk");
        return;
    }
    const rt_thunk *t = thunks[i];
    if (t->fn == NULL) {
        char what[160];
        snprintf(what, sizeof what, "unimplemented import %s", t->name);
        guest_fault(c, addr, what);
        return;
    }
    rt_cur_thunk = t;
    rt_safepoint();
    if (trace_imports)
        rt_log("[imp:%u] %s(%08x, %08x, %08x, %08x) from %08x\n", rt_current_tid(), t->name, ARG(0), ARG(1), ARG(2), ARG(3), ld32(c->esp));
    uint32_t ret = t->fn(c);
    c->eax = ret;
    c->esp += 4U + 4U * (uint32_t)t->nargs;
}

/* ---------------------------------------------------------- guest heap --- */
/* Boundary-tag allocator over guest memory: 8-byte header {prev size, size|used}, payload 8-aligned, free blocks in 32 size bins. */

#define HDR 8U
#define USED 1U
#define MIN_BLOCK 24U
#define NBINS 32
static uint32_t heap_lo, heap_hi, heap_top;
static uint32_t bins[NBINS];

static uint32_t bsize(uint32_t b) { return ld32(b + 4) & ~7U; }
static int bused(uint32_t b) { return (ld32(b + 4) & USED) != 0; }
static void bset(uint32_t b, uint32_t size, int used) { st32(b + 4, size | (used ? USED : 0U)); if (b + size + 4U <= heap_hi) st32(b + size, size); }

static int bin_of(uint32_t size)
{
    int i = 0;
    size >>= 4;
    while (size > 1 && i < NBINS - 1) { size >>= 1; i++; }
    return i;
}

static void bin_remove(uint32_t b)
{
    uint32_t next = ld32(b + 8), prev = ld32(b + 12);
    if (prev) st32(prev + 8, next); else bins[bin_of(bsize(b))] = next;
    if (next) st32(next + 12, prev);
}

static void bin_insert(uint32_t b)
{
    int i = bin_of(bsize(b));
    st32(b + 8, bins[i]);
    st32(b + 12, 0);
    if (bins[i]) st32(bins[i] + 12, b);
    bins[i] = b;
}

static void heap_init(uint32_t lo, uint32_t hi)
{
    heap_lo = lo; heap_hi = hi; heap_top = lo;
    memset(bins, 0, sizeof bins);
}

static uint32_t heap_alloc(uint32_t n)
{
    uint32_t need = (n + HDR + 7U) & ~7U;
    if (need < MIN_BLOCK) need = MIN_BLOCK;
    for (int i = bin_of(need); i < NBINS; i++) {
        for (uint32_t b = bins[i]; b; b = ld32(b + 8)) {
            uint32_t sz = bsize(b);
            if (sz < need)
                continue;
            bin_remove(b);
            if (sz - need >= MIN_BLOCK) {
                bset(b, need, 1);
                uint32_t r = b + need;
                st32(r, need);
                bset(r, sz - need, 0);
                bin_insert(r);
            } else {
                bset(b, sz, 1);
            }
            return b + HDR;
        }
    }
    if (heap_top + need > heap_hi) {
        rt_log("[heap] out of guest memory (%u bytes requested, %u MB used)\n", n, (heap_top - heap_lo) >> 20);
        return 0;
    }
    uint32_t b = heap_top;
    if (b == heap_lo) st32(b, 0);
    heap_top += need;
    bset(b, need, 1);
    return b + HDR;
}

static void heap_free(uint32_t p)
{
    if (!p) return;
    uint32_t b = p - HDR, sz = bsize(b);
    uint32_t next = b + sz;
    if (next < heap_top && !bused(next)) {
        bin_remove(next);
        sz += bsize(next);
    }
    if (b > heap_lo) {
        uint32_t prev = b - ld32(b);
        if (!bused(prev)) {
            bin_remove(prev);
            sz += bsize(prev);
            b = prev;
        }
    }
    if (b + sz == heap_top) {
        heap_top = b;
        return;
    }
    bset(b, sz, 0);
    bin_insert(b);
}

uint32_t rt_alloc_size(uint32_t p)
{
    return p ? bsize(p - HDR) - HDR : 0;
}

uint32_t rt_alloc(uint32_t n)
{
    pthread_mutex_lock(&heap_lock);
    uint32_t p = heap_alloc(n);
    pthread_mutex_unlock(&heap_lock);
    return p;
}

void rt_free(uint32_t p)
{
    pthread_mutex_lock(&heap_lock);
    heap_free(p);
    pthread_mutex_unlock(&heap_lock);
}

uint32_t rt_realloc(uint32_t p, uint32_t n)
{
    if (!p) return rt_alloc(n);
    uint32_t old = rt_alloc_size(p);
    if (n <= old) return p;
    uint32_t q = rt_alloc(n);
    if (q) {
        memcpy(G2H(q), G2H(p), old);
        rt_free(p);
    }
    return q;
}

uint32_t rt_heap_used(void) { return heap_top - heap_lo; }

/* -------------------------------------------------------------- loader --- */

static void load_image(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        rt_log("[rt] cannot open %s\n", path);
        exit(2);
    }
    size_t got = fread(guest_membase + GUEST_IMAGE_BASE, 1, guest_image_size, f);
    fclose(f);
    if (got != guest_image_size) {
        rt_log("[rt] short image %s: %zu of %u bytes\n", path, got, guest_image_size);
        exit(2);
    }
    for (uint32_t i = 0; i < guest_import_count; i++) {
        const rt_thunk *t = rt_find_import(guest_import_slots[i].name);
        st32(GUEST_IMAGE_BASE + guest_import_slots[i].slot_rva, rt_add_thunk(t));
    }
}

static void init_teb(CPU *c)
{
    uint32_t teb_addr = GUEST_TEB;
    memset(G2H(teb_addr), 0, 0x1000);
    st32(teb_addr + 0x00, 0xFFFFFFFFU);
    st32(teb_addr + 0x04, GUEST_STACK_TOP);
    st32(teb_addr + 0x08, GUEST_STACK_TOP - GUEST_STACK_BYTES);
    st32(teb_addr + 0x18, teb_addr);
    st32(teb_addr + 0x20, 1000);
    st32(teb_addr + 0x24, 1001);
    st32(teb_addr + 0x2C, teb_addr + 0x800);
    st32(teb_addr + 0x30, GUEST_PEB);
    (void)c;
}

#ifndef __vita__
#include <execinfo.h>
#include <signal.h>
/* Host crash report: the faulting address as a guest address when it lies in the guest block, plus the native stack (sub_RVA names). */
static void crash_handler(int sig, siginfo_t *info, void *ctx)
{
    (void)ctx;
    uintptr_t a = (uintptr_t)info->si_addr, base = (uintptr_t)guest_membase;
    char line[160];
    int n = snprintf(line, sizeof line, "[crash] signal %d at host %p (guest %08lx%s)\n", sig, info->si_addr,
                     (unsigned long)(a - base), a >= base && a < base + GUEST_SPACE ? "" : ", outside the guest block");
    write(2, line, (size_t)n);
    void *frames[48];
    int count = backtrace(frames, 48);
    backtrace_symbols_fd(frames, count, 2);
    _exit(128 + sig);
}
#endif

int main(int argc, char **argv)
{
#ifndef __vita__
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
#endif
    const char *image = (argc > 1) ? argv[1] : "image.bin";
    trace_imports = getenv("TMNT_TRACE_IMPORTS") && atoi(getenv("TMNT_TRACE_IMPORTS"));
    trace_init();
    void *m = mmap(NULL, GUEST_SPACE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) {
        rt_log("[rt] cannot reserve %u MB of guest space\n", GUEST_SPACE >> 20);
        return 2;
    }
    guest_membase = m;
    mprotect(guest_membase, 0x10000, PROT_NONE);
    heap_init(GUEST_HEAP_LO, GUEST_SPACE);
    load_image(image);

    static CPU cpu;
    CPU *c = &cpu;
    c->fcw = 0x027F;
    c->esp = GUEST_STACK_TOP;
    c->stack_owner = c;
    c->stack_floor = GUEST_STACK_TOP - GUEST_STACK_BYTES;
    c->stack_ceiling = GUEST_STACK_TOP;
    c->stack_low_water = GUEST_STACK_TOP;
    init_teb(c);
    rt_win32_init(c);
    if (getenv("TMNT_SELFTEST")) { selftest(c); return 0; }

    gpush_generated(c, 0xFFFFFFF0U);
    rt_log("[rt] guest space %u MB at %p, image %u bytes, %u functions, %u imports; entry %08x\n", GUEST_SPACE >> 20, (void *)guest_membase,
           guest_image_size, guest_func_count, guest_import_count, guest_image_entry + GUEST_IMAGE_BASE);
    guest_call(c, guest_image_entry + GUEST_IMAGE_BASE);
    rt_log("[rt] entry point returned eax=%08x\n", c->eax);
    return (int)c->eax;
}
