/* TMNT 2003 host runtime: guest address space, image loader, dispatch, CPU probes, guest heap and the entry point. */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __vita__
#include <psp2/io/stat.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/power.h>
#else
#include <sys/mman.h>
#endif

#include "guest.h"
#include "osdep.h"
#include "runtime.h"
#include "par.h"

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

static uint32_t prof_rdtsc;
/* rdtsc at a nominal 500 MHz. The game reads it thousands of times a frame; on the Vita the 32-bit process clock is the cheapest
 * (~0.4 us against ~1 us for the 64-bit one), extended here by counting wraps (every 71 minutes). */
#ifdef __vita__
/* The game reads rdtsc 7-10k times a frame in fights; even the cheapest system clock call made that ~10% of the frame. A thread
 * on cores 1-2 refreshes a 32-bit microsecond copy every 100 us and readers only load it (wraps are counted, every 71 minutes);
 * each read still advances by at least one tick, so time never stands still for code that measures short spans. */
static volatile uint32_t rdtsc_cache_lo;
static void *rdtsc_ticker(void *arg)
{
    (void)arg;
    rt_thread_place_self(0, 15);
    plat_thread_label("rdtsc");
    for (;;) {
        rdtsc_cache_lo = sceKernelGetProcessTimeLow();
        sceKernelDelayThread(100);
    }
    return NULL;
}
#endif

uint64_t guest_rdtsc(void)
{
    prof_rdtsc++;
#ifdef __vita__
    static uint32_t last_lo, hi;
    static uint64_t last;
    uint32_t lo = rdtsc_cache_lo;
    if (lo < last_lo) hi++;
    last_lo = lo;
    uint64_t t = (((uint64_t)hi << 32) | lo) * 500U;
    if (t <= last) t = last + 1U;
    last = t;
    return t;
#else
    return rt_now_us() * 500U;
#endif
}

uint32_t guest_fs_base(CPU *__restrict c)
{
    return rt_current_teb(c);
}

/* ------------------------------------------------------------- tracing --- */

volatile int guest_trace_on;
static uint32_t trace_rvas[64];
static int trace_count;
static int watch_functions;

/* TMNT_PROF=1: where the main thread's time goes, reported with the frame line (rt_prof_report). Host API time per import/COM
 * method, and a sampling profile of the translated function the main thread last entered (sampled every 500 us on another core). */
static int prof_on;
/* TMNT_FNSET=1: every translated function the main thread enters is marked; each report window (rt_prof_report) is written to
 * fnset_<frame>.txt, one RVA per line. Diffing two windows finds the code of one screen (e.g. a menu page). */
static int fnset_on;
static uint8_t fnset_bits[0x200000 / 8];
static CPU *prof_cpu;
static volatile uint32_t prof_cur_fn;
/* Exact-profile build (rt/guest.h, GUEST_PROF_EXCL): the function running on the profiled thread; 0x0FFFFFFE inside a host API. */
struct CPU *guest_prof_cpu;
volatile uint32_t guest_prof_fn;
#ifdef GUEST_PROF_EXCL
int tmnt_phase(void);   /* game_tmnt.c: 1 logic, 2 draw, 3 present + pace */
#define PROF_HOST 0x0FFFFFFEU
#endif
static uint64_t prof_thunk_us[RT_MAX_THUNKS];
static uint32_t prof_thunk_n[RT_MAX_THUNKS];
#define PROF_SLOTS 8192
static uint32_t prof_fn_rva[PROF_SLOTS], prof_fn_hits[PROF_SLOTS], prof_samples;
void rt_note_function(CPU *c, uint32_t rva);
void rt_note_import(CPU *c, const char *name, int stage);
extern int rt_watchdog_on;

static void trace_init(void)
{
    const char *s = getenv("TMNT_TRACE_FUNCS");
    while (s && *s && trace_count < 64) {
        uint32_t v = (uint32_t)strtoul(s, (char **)&s, 16);
        trace_rvas[trace_count++] = v >= GUEST_IMAGE_BASE ? v - GUEST_IMAGE_BASE : v;
        while (*s == ',' || *s == ' ') s++;
    }
    watch_functions = getenv("TMNT_WATCHDOG") && atoi(getenv("TMNT_WATCHDOG"));
    prof_on = getenv("TMNT_PROF") && atoi(getenv("TMNT_PROF"));
    fnset_on = getenv("TMNT_FNSET") && atoi(getenv("TMNT_FNSET"));
    guest_trace_on = trace_count > 0 || watch_functions || prof_on || fnset_on;
}

void guest_trace_enter(CPU *c, uint32_t rva)
{
    if (prof_on && c == prof_cpu) prof_cur_fn = rva;
    if (fnset_on && c == prof_cpu && rva < 0x200000U) fnset_bits[rva >> 3] |= (uint8_t)(1U << (rva & 7U));
    if (watch_functions) rt_note_function(c, rva);
    for (int i = 0; i < trace_count; i++) {
        if (trace_rvas[i] == rva) {
            rt_log("[call:%u] %08x(%08x, %08x, %08x, %08x) eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x from %08x\n", rt_current_tid(c), rva + GUEST_IMAGE_BASE,
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

/* TMNT_SELFTEST_BLOCKS=rva,in.bin,out.bin: runs a translated cdecl (block, n) function on every record of in.bin
   (int32 n + 64 int16 coefficients) and writes the blocks it leaves; compared offline with the x86 run in Unicorn. */
void rt_selftest_blocks(CPU *c, const char *spec)
{
    char in[512], out[512];
    unsigned rva;
    if (sscanf(spec, "%x,%511[^,],%511s", &rva, in, out) != 3) { rt_log("[selftest] bad TMNT_SELFTEST_BLOCKS\n"); return; }
    FILE *fi = fopen(in, "rb"), *fo = fopen(out, "wb");
    if (!fi || !fo) { rt_log("[selftest] cannot open %s / %s\n", in, out); return; }
    uint32_t buf = rt_alloc(128);
    int32_t n;
    int16_t blk[64];
    int count = 0;
    while (fread(&n, 4, 1, fi) == 1 && fread(blk, 2, 64, fi) == 64) {
        memcpy(G2H(buf), blk, 128);
        uint32_t a[2] = { buf, (uint32_t)n };
        call_cdecl(c, rva, 2, a);
        memcpy(blk, G2H(buf), 128);
        fwrite(blk, 2, 64, fo);
        count++;
    }
    fclose(fi); fclose(fo);
    rt_free(buf);
    rt_log("[selftest] blocks: %d records through %08x\n", count, rva);
}

/* TMNT_SELFTEST_MC=out.bin: Sofdec's plain motion compensation (struct: +0x18 dst, +0x20 pitch, +0x24 src A, +0x28 src B) on a
   64 KB arena of xorshift bytes, for every source alignment; each record is (rva, align, arena address) + the arena after the
   call, replayed in Unicorn by build/analysis/idct/uc_mc.py. */
void rt_selftest_mc(CPU *c, const char *path)
{
    static const uint32_t fns[] = { 0x586ee0, 0x5872f0, 0x5873c0, 0x587480 };
    FILE *fo = fopen(path, "wb");
    if (!fo) return;
    uint32_t arena = rt_alloc(0x10000);
    for (unsigned f = 0; f < sizeof fns / sizeof fns[0]; f++)
        for (uint32_t align = 0; align < 8; align++) {
            uint32_t x = 0x9e3779b9U ^ (f * 8 + align);
            uint8_t *m = G2H(arena);
            for (int i = 0; i < 0x10000; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; m[i] = (uint8_t)x; }
            st32(arena + 0x18, arena + 0x6000); st32(arena + 0x20, 0x40);
            st32(arena + 0x24, arena + 0x1000 + align); st32(arena + 0x28, arena + 0x3000 + align);
            uint32_t a[1] = { arena };
            call_cdecl(c, fns[f], 1, a);
            uint32_t hdr[3] = { fns[f], align, arena };
            fwrite(hdr, 4, 3, fo);
            fwrite(G2H(arena), 1, 0x10000, fo);
        }
    fclose(fo);
    rt_free(arena);
    rt_log("[selftest] mc: wrote %s\n", path);
}

/* ------------------------------------------------------------ dispatch --- */

/* Indirect calls: RVA -> translated function through an open-addressing table filled once before any guest code runs (read-only
 * afterwards, so guest threads share it without locks); about one probe per call against ~14 for the binary search. */
#define LOOKUP_BITS 15
static struct { uint32_t rva; guest_fn fn; } lookup_tab[1U << LOOKUP_BITS];

static uint32_t lookup_slot(uint32_t rva) { return (rva * 2654435761U) >> (32 - LOOKUP_BITS); }

static guest_fn lookup(uint32_t rva)
{
    for (uint32_t h = lookup_slot(rva); lookup_tab[h].rva; h = (h + 1) & ((1U << LOOKUP_BITS) - 1))
        if (lookup_tab[h].rva == rva) return lookup_tab[h].fn;
    return NULL;
}

/* The sorted-table search the hash table replaced; TMNT_SELFTEST=1 checks both agree. */
static guest_fn lookup_sorted(uint32_t rva)
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

static void lookup_init(void)
{
    for (uint32_t i = 0; i < guest_func_count; i++) {
        uint32_t h = lookup_slot(guest_func_rvas[i]);
        while (lookup_tab[h].rva) h = (h + 1) & ((1U << LOOKUP_BITS) - 1);
        lookup_tab[h].rva = guest_func_rvas[i];
        lookup_tab[h].fn = guest_func_ptrs[i];
    }
    if (getenv("TMNT_SELFTEST")) {
        uint32_t bad = 0;
        for (uint32_t i = 0; i < guest_func_count; i++) {
            uint32_t r = guest_func_rvas[i];
            if (lookup(r) != lookup_sorted(r)) bad++;
            if (lookup(r + 1U) != lookup_sorted(r + 1U)) bad++;   /* mostly addresses inside a function: both must miss */
        }
        rt_log("[selftest] dispatch table: %u functions, %u mismatches\n", guest_func_count, bad);
    }
}

#define CALLSTACK_DEPTH 64
/* Last indirect calls, for fault reports. On the Vita __thread is emulated (a library call per access), so it is shared there. */
#ifdef __vita__
static uint32_t callstack[CALLSTACK_DEPTH];
static int callstack_top;
#else
static __thread uint32_t callstack[CALLSTACK_DEPTH];
static __thread int callstack_top;
#endif

uint32_t rt_last_guest_call(void) { return callstack[(callstack_top - 1) & (CALLSTACK_DEPTH - 1)]; }

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
    if (rt_watchdog_on) rt_note_import(c, t->name, 1);
    if (t->fn == NULL) {
        char what[160];
        snprintf(what, sizeof what, "unimplemented import %s", t->name);
        guest_fault(c, addr, what);
        return;
    }
    rt_safepoint(c, t);
    if (rt_watchdog_on) rt_note_import(c, t->name, 2);
    if (trace_imports)
        rt_log("[imp:%u] %s(%08x, %08x, %08x, %08x) from %08x\n", rt_current_tid(c), t->name, ARG(0), ARG(1), ARG(2), ARG(3), ld32(c->esp));
    uint32_t ret;
    if (prof_on && c == prof_cpu) {
        uint64_t t0 = rt_now_us();
#ifdef GUEST_PROF_EXCL
        uint32_t keep = guest_prof_fn;
        guest_prof_fn = PROF_HOST;
        ret = t->fn(c);
        guest_prof_fn = keep;
#else
        ret = t->fn(c);
#endif
        prof_thunk_us[i] += rt_now_us() - t0;
        prof_thunk_n[i]++;
    } else {
        ret = t->fn(c);
    }
    if (rt_watchdog_on) rt_note_import(c, t->name, 3);
    c->eax = ret;
    c->esp += 4U + 4U * (uint32_t)t->nargs;
}

static void *prof_sampler(void *arg)
{
    (void)arg;
    rt_thread_place_self(0, 15);
    plat_thread_label("prof");
    for (;;) {
        rt_sleep_us(500);
#ifdef GUEST_PROF_EXCL
        uint32_t rva = guest_prof_fn | ((uint32_t)tmnt_phase() << 28), h = (rva * 2654435761U) >> 19;
#else
        uint32_t rva = prof_cur_fn, h = (rva * 2654435761U) >> 19;
#endif
        for (uint32_t k = 0; k < PROF_SLOTS; k++, h = (h + 1) & (PROF_SLOTS - 1)) {
            if (prof_fn_rva[h] == rva || prof_fn_hits[h] == 0) { prof_fn_rva[h] = rva; prof_fn_hits[h]++; break; }
        }
        prof_samples++;
    }
    return NULL;
}

/* Logs the heaviest host API calls (per frame over `frames`) and translated functions, then starts a new window. */
static uint32_t fnset_window;
static void fnset_dump(void)
{
    char name[48];
    snprintf(name, sizeof name, "fnset_%05u.txt", ++fnset_window);
    FILE *f = fopen(name, "w");
    for (uint32_t i = 0; f && i < sizeof fnset_bits * 8U; i++)
        if (fnset_bits[i >> 3] & (1U << (i & 7U))) fprintf(f, "%08x\n", i + GUEST_IMAGE_BASE);
    if (f) fclose(f);
    memset(fnset_bits, 0, sizeof fnset_bits);
}

void rt_prof_report(uint32_t frames)
{
    if (fnset_on && frames) fnset_dump();
    if (!prof_on || !frames) return;
    for (int r = 0; r < 10; r++) {
        uint32_t best = 0;
        for (uint32_t i = 1; i < thunk_count; i++) if (prof_thunk_us[i] > prof_thunk_us[best]) best = i;
        if (!prof_thunk_us[best]) break;
        rt_log("[prof] api %-40s %6.2f ms/frame, %5u calls/frame\n", thunks[best]->name, (double)prof_thunk_us[best] / frames / 1000.0,
               prof_thunk_n[best] / frames);
        prof_thunk_us[best] = 0;
    }
    rt_log("[prof] rdtsc %u/frame\n", prof_rdtsc / frames);
    prof_rdtsc = 0;
    memset(prof_thunk_us, 0, sizeof prof_thunk_us);
    memset(prof_thunk_n, 0, sizeof prof_thunk_n);
    uint32_t total = prof_samples;
#ifdef GUEST_PROF_EXCL
    /* Per phase: its share of the samples, then its functions by share of the phase (host = inside host APIs). */
    static const char *phase_name[4] = { "other", "logic", "draw", "pace" };
    uint32_t phase_total[4] = { 0, 0, 0, 0 };
    for (uint32_t h = 0; h < PROF_SLOTS; h++) if (prof_fn_hits[h]) phase_total[(prof_fn_rva[h] >> 28) & 3U] += prof_fn_hits[h];
    for (uint32_t ph = 0; ph < 4 && total; ph++) {
        if (!phase_total[ph]) continue;
        rt_log("[prof] phase %s %.1f%% of the samples\n", phase_name[ph], 100.0 * phase_total[ph] / total);
        for (int r = 0; r < 25; r++) {
            uint32_t best = PROF_SLOTS;
            for (uint32_t h = 0; h < PROF_SLOTS; h++)
                if (prof_fn_hits[h] && ((prof_fn_rva[h] >> 28) & 3U) == ph && (best == PROF_SLOTS || prof_fn_hits[h] > prof_fn_hits[best])) best = h;
            if (best == PROF_SLOTS) break;
            uint32_t rva = prof_fn_rva[best] & 0x0FFFFFFFU;
            if (rva == PROF_HOST) rt_log("[prof]   %s host APIs %5.1f%%\n", phase_name[ph], 100.0 * prof_fn_hits[best] / phase_total[ph]);
            else rt_log("[prof]   %s fn %08x %5.1f%%\n", phase_name[ph], rva + GUEST_IMAGE_BASE, 100.0 * prof_fn_hits[best] / phase_total[ph]);
            prof_fn_hits[best] = 0;
        }
    }
    total = 0;
#endif
    for (int r = 0; r < 15 && total; r++) {
        uint32_t best = 0;
        for (uint32_t h = 1; h < PROF_SLOTS; h++) if (prof_fn_hits[h] > prof_fn_hits[best]) best = h;
        if (!prof_fn_hits[best]) break;
        rt_log("[prof] fn %08x %5.1f%%\n", prof_fn_rva[best] + GUEST_IMAGE_BASE, 100.0 * prof_fn_hits[best] / total);
        prof_fn_hits[best] = 0;
    }
    memset(prof_fn_rva, 0, sizeof prof_fn_rva);
    memset(prof_fn_hits, 0, sizeof prof_fn_hits);
    prof_samples = 0;
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

static uint64_t heap_live;

uint32_t rt_alloc(uint32_t n)
{
    pthread_mutex_lock(&heap_lock);
    uint32_t p = heap_alloc(n);
    if (p) heap_live += bsize(p - HDR);
    pthread_mutex_unlock(&heap_lock);
    return p;
}

void rt_free(uint32_t p)
{
    pthread_mutex_lock(&heap_lock);
    if (p) heap_live -= bsize(p - HDR);
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
uint32_t rt_heap_live(void) { return (uint32_t)heap_live; }

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

#ifdef __vita__
/* Newlib heap for host allocations; the guest space is its own memory block. */
int _newlib_heap_size_user = 48 * 1024 * 1024;
#define VITA_DATA_DIR "ux0:data/TMNT2003"
/* Translated code nests every guest call on the native stack: the game runs on a thread with a large one. */
#define VITA_GAME_STACK (8U << 20)

static void *alloc_guest_space(void)
{
    SceUID b = sceKernelAllocMemBlock("tmnt_guest", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, GUEST_SPACE, NULL);
    void *m = NULL;
    if (b < 0 || sceKernelGetMemBlockBase(b, &m) < 0) return NULL;
    return m;
}

/* tmnt_env.txt in the data folder: NAME=value lines, the Vita's stand-in for environment variables. */
static void load_env_file(void)
{
    FILE *f = fopen("tmnt_env.txt", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        setenv(line, eq + 1, 1);
    }
    fclose(f);
}
#else
static void *alloc_guest_space(void)
{
    void *m = mmap(NULL, GUEST_SPACE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) return NULL;
    mprotect(m, 0x10000, PROT_NONE);
    return m;
}
#endif

static int game_main(int argc, char **argv)
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
    void *m = alloc_guest_space();
    if (!m) {
        rt_log("[rt] cannot reserve %u MB of guest space\n", GUEST_SPACE >> 20);
        return 2;
    }
    guest_membase = m;
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
    prof_cpu = c;
    if (prof_on) guest_prof_cpu = c;
#ifdef __vita__
    rdtsc_cache_lo = sceKernelGetProcessTimeLow();
    {
        pthread_t tk;
        pthread_create(&tk, NULL, rdtsc_ticker, NULL);
        pthread_detach(tk);
    }
#endif
    if (prof_on) {
        pthread_t ps;
        pthread_create(&ps, NULL, prof_sampler, NULL);
        pthread_detach(ps);
    }
    lookup_init();
    par_init();
    if (getenv("TMNT_SELFTEST")) { selftest(c); return 0; }

    gpush_generated(c, 0xFFFFFFF0U);
    rt_log("[rt] guest space %u MB at %p, image %u bytes, %u functions, %u imports; entry %08x\n", GUEST_SPACE >> 20, (void *)guest_membase,
           guest_image_size, guest_func_count, guest_import_count, guest_image_entry + GUEST_IMAGE_BASE);
    guest_call(c, guest_image_entry + GUEST_IMAGE_BASE);
    rt_log("[rt] entry point returned eax=%08x\n", c->eax);
    return (int)c->eax;
}

#ifdef __vita__
static int game_exit;

static void *game_thread(void *arg)
{
    (void)arg;
    rt_thread_place_self(1, 0);
    plat_thread_label("main");
    char *argv[] = { "tmnt", NULL };
    game_exit = game_main(1, argv);
    return NULL;
}

int main(void)
{
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(VITA_DATA_DIR, 0777);
    chdir(VITA_DATA_DIR);
    freopen("log.txt", "w", stderr);
    setvbuf(stderr, NULL, _IOLBF, 0);
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    char cwd[256];
    rt_log("[vita] cwd %s, clocks arm %d gpu %d MHz\n", getcwd(cwd, sizeof cwd) ? cwd : "?", scePowerGetArmClockFrequency(),
           scePowerGetGpuClockFrequency());
    load_env_file();
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, VITA_GAME_STACK);
    pthread_t t;
    pthread_create(&t, &a, game_thread, NULL);
    pthread_join(t, NULL);
    rt_log("[vita] game exited (%d)\n", game_exit);
    return game_exit;
}
#else
int main(int argc, char **argv)
{
    return game_main(argc, argv);
}
#endif
