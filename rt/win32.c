/* Win32 imports of TMNT_EN.EXE served by the host: kernel objects, files, heap, locale, time, window stubs. */
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "guest.h"
#include "runtime.h"
#include "movie.h"
#include "plat.h"
#include "osdep.h"

#define ERROR_FILE_NOT_FOUND 2U
#define ERROR_INVALID_HANDLE 6U
#define INVALID_HANDLE 0xFFFFFFFFU
#define MODULE_HANDLE 0x00400000U
#define KERNEL32_HANDLE 0x7C800000U
#define FAKE_DIR "C:\\TMNT\\"

/* ------------------------------------------------------------ threads --- */

typedef struct rt_thread_ctx {
    CPU cpu;
    uint32_t teb;
    uint32_t tid;
    int suspend;
    rt_cond wake;
    const char *last_import;      /* watchdog: the last API the thread called, and how many so far */
    volatile uint32_t import_calls;
    int cs_held;                  /* critical sections entered and not yet left */
    volatile uint32_t fn_calls, fn_ring[8];   /* watchdog: translated functions entered, the last 8 */
    volatile int stage;           /* watchdog: 1 = entering an API call, 2 = past the suspend check, 3 = returned */
    volatile uint32_t waiting_cs; /* watchdog: critical section being entered */
    int win_prio;                 /* SetThreadPriority value, applied when the host thread starts */
    const rt_thunk *thunk;        /* the API being called (COM dispatch reads it) */
    volatile int os_id;           /* host thread id once started (Vita), 0 before */
} rt_thread_ctx;


static rt_thread_ctx main_ctx;

/* Every guest thread context. */
#define MAX_CTX 64
static rt_thread_ctx *all_ctx[MAX_CTX];
static int ctx_count;

static CPU *main_cpu;

/* The context of the thread whose CPU is c: every API call carries its CPU, so no thread-local storage is needed (on the Vita it is
 * emulated by library calls, and threads hung inside them). A context's CPU is its first member. */
static rt_thread_ctx *ctx_of_cpu(CPU *c)
{
    if (c == main_cpu) return &main_ctx;
    for (int i = 0; i < ctx_count; i++)
        if (&all_ctx[i]->cpu == c) return all_ctx[i];
    return NULL;
}
static pthread_mutex_t big = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
static uint32_t next_tid = 1001;

uint32_t rt_current_teb(CPU *c) { rt_thread_ctx *x = ctx_of_cpu(c); return x ? x->teb : GUEST_TEB; }
uint32_t rt_current_tid(CPU *c) { rt_thread_ctx *x = ctx_of_cpu(c); return x ? x->tid : 0; }

/* ------------------------------------------------------------ handles --- */

enum { H_FREE, H_FILE, H_EVENT, H_MUTEX, H_THREAD, H_MAPPING };

typedef struct rt_handle {
    int type;
    FILE *f;
    rt_cond wake;
    int wake_ready;
    int signaled, manual;
    uint32_t owner, count, pulses;
    pthread_t thread;
    rt_thread_ctx *tctx;
    uint32_t start, param, suspended;
    int movie;      /* TMNT_V0x.DAT: x + 1, so movie.c knows which movie Sofdec is reading */
} rt_handle;

#define MAX_HANDLES 1024
static rt_handle handles[MAX_HANDLES];

static uint32_t h_new(int type)
{
    pthread_mutex_lock(&big);
    for (uint32_t i = 1; i < MAX_HANDLES; i++) {
        if (handles[i].type == H_FREE) {
            /* The kernel lock/condition of a slot is created once and kept across reuse. */
            rt_cond wake = handles[i].wake;
            int wake_ready = handles[i].wake_ready;
            memset(&handles[i], 0, sizeof handles[i]);
            handles[i].wake = wake;
            handles[i].wake_ready = wake_ready;
            if (!handles[i].wake_ready) { rt_cond_init(&handles[i].wake); handles[i].wake_ready = 1; }
            handles[i].type = type;
            pthread_mutex_unlock(&big);
            return 0x1000U + i * 4U;
        }
    }
    pthread_mutex_unlock(&big);
    rt_log("[w32] out of handles\n");
    return 0;
}

static rt_handle *h_get(uint32_t h, int type)
{
    uint32_t i = (h - 0x1000U) / 4U;
    if (h < 0x1000U || i >= MAX_HANDLES || handles[i].type == H_FREE)
        return NULL;
    if (type != H_FREE && handles[i].type != type)
        return NULL;
    return &handles[i];
}

static uint32_t std_handles[3] = { 0x10U, 0x14U, 0x18U };

/* --------------------------------------------------------------- paths --- */

/* Guest paths are relative to the game directory or under the fake module directory C:\TMNT\. */
static void host_path(const char *g, char *out, size_t n)
{
    if (strncasecmp(g, FAKE_DIR, strlen(FAKE_DIR)) == 0)
        g += strlen(FAKE_DIR);
    else if (strncasecmp(g, ".\\", 2) == 0)
        g += 2;
    size_t i = 0;
    for (; *g && i + 1 < n; g++)
        out[i++] = (*g == '\\') ? '/' : *g;
    out[i] = 0;
}

/* ------------------------------------------------------------ helpers --- */


#define ret_err(v, e) (c->last_error = (e), (v))

static uint32_t guest_strdup(const char *s)
{
    uint32_t n = (uint32_t)strlen(s) + 1, p = rt_alloc(n);
    memcpy(G2H(p), s, n);
    return p;
}

uint32_t rt_call_guest(CPU *__restrict c, uint32_t fn, int nargs, const uint32_t *args)
{
    uint32_t saved = c->esp;
    for (int i = nargs - 1; i >= 0; i--)
        gpush_generated(c, args[i]);
    gpush_generated(c, 0xFFFFFFF0U);
    guest_call(c, fn);
    c->esp = saved;
    return c->eax;
}

/* -------------------------------------------------------------- kernel --- */

static uint32_t cmdline_ptr, env_ptr, envw_ptr;

static uint32_t k_GetModuleFileNameA(CPU *c)
{
    const char *p = FAKE_DIR "TMNT.EXE";
    uint32_t n = ARG(2), len = (uint32_t)strlen(p);
    if (len >= n) len = n - 1;
    memcpy(G2H(ARG(1)), p, len);
    st8(ARG(1) + len, 0);
    return len;
}
static uint32_t k_GetModuleHandleA(CPU *c)
{
    if (!ARG(0)) return MODULE_HANDLE;
    if (strncasecmp(GSTR(ARG(0)), "kernel32", 8) == 0) return KERNEL32_HANDLE;
    rt_log("[w32] GetModuleHandleA(%s) -> 0\n", GSTR(ARG(0)));
    return ret_err(0, ERROR_FILE_NOT_FOUND);
}
static uint32_t k_GetCommandLineA(CPU *c) { (void)c; return cmdline_ptr; }
static uint32_t k_GetEnvironmentStrings(CPU *c) { (void)c; return env_ptr; }
static uint32_t k_GetEnvironmentStringsW(CPU *c) { (void)c; return envw_ptr; }
static uint32_t k_Free1(CPU *c) { (void)c; return 1; }
static uint32_t k_GetStartupInfoA(CPU *c)
{
    memset(G2H(ARG(0)), 0, 68);
    st32(ARG(0), 68);
    return 0;
}
static uint32_t k_GetStdHandle(CPU *c)
{
    int32_t n = (int32_t)ARG(0);
    return (n >= -12 && n <= -10) ? std_handles[-10 - n] : INVALID_HANDLE;
}
static uint32_t k_SetStdHandle(CPU *c) { (void)c; return 1; }
static uint32_t k_GetFileType(CPU *c)
{
    uint32_t h = ARG(0);
    if (h == 0x10U || h == 0x14U || h == 0x18U) return 2;
    return h_get(h, H_FILE) ? 1 : 0;
}
static uint32_t k_SetHandleCount(CPU *c) { return ARG(0); }
static uint32_t k_GetVersionExA(CPU *c)
{
    uint32_t p = ARG(0);
    st32(p + 4, 5); st32(p + 8, 1); st32(p + 12, 2600); st32(p + 16, 2);
    memset(G2H(p + 20), 0, 128);
    strcpy((char *)G2H(p + 20), "Service Pack 3");
    return 1;
}
static uint32_t k_GetCurrentThread(CPU *c) { (void)c; return 0xFFFFFFFEU; }
static uint32_t k_GetCurrentThreadId(CPU *c) { rt_thread_ctx *x = ctx_of_cpu(c); return x ? x->tid : 1000; }
static uint32_t k_GetCurrentProcessId(CPU *c) { (void)c; return 999; }
static uint32_t k_GetCurrentProcess(CPU *c) { (void)c; return 0xFFFFFFFFU; }
static uint32_t k_GetLastError(CPU *c) { return c->last_error; }
static uint32_t k_SetLastError(CPU *c) { c->last_error = ARG(0); return 0; }
static uint32_t k_SetCurrentDirectoryA(CPU *c) { rt_log("[w32] SetCurrentDirectoryA(%s)\n", GSTR(ARG(0))); return 1; }
static uint32_t k_GetSystemInfo(CPU *c)
{
    uint32_t p = ARG(0);
    memset(G2H(p), 0, 36);
    st32(p + 4, 4096); st32(p + 8, 0x10000); st32(p + 12, 0x7FFEFFFF); st32(p + 16, 1); st32(p + 20, 1);
    st32(p + 24, 586); st32(p + 28, 0x10000); st16(p + 32, 6);
    return 0;
}
static uint32_t k_IsProcessorFeaturePresent(CPU *c) { (void)c; return 0; }
static uint32_t k_GetSystemTimeAsFileTime(CPU *c)
{
    uint64_t ft = ((uint64_t)time(NULL) + 11644473600ULL) * 10000000ULL;
    st32(ARG(0), (uint32_t)ft); st32(ARG(0) + 4, (uint32_t)(ft >> 32));
    return 0;
}
static uint32_t k_GetLocalTime(CPU *c)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    uint32_t p = ARG(0);
    st16(p, (uint16_t)(tm.tm_year + 1900)); st16(p + 2, (uint16_t)(tm.tm_mon + 1)); st16(p + 4, (uint16_t)tm.tm_wday);
    st16(p + 6, (uint16_t)tm.tm_mday); st16(p + 8, (uint16_t)tm.tm_hour); st16(p + 10, (uint16_t)tm.tm_min);
    st16(p + 12, (uint16_t)tm.tm_sec); st16(p + 14, 0);
    return 0;
}
static uint64_t now_us(void) { return rt_now_us(); }
static uint32_t k_GetTickCount(CPU *c) { (void)c; return (uint32_t)(now_us() / 1000U); }
static uint32_t k_QueryPerformanceCounter(CPU *c) { uint64_t v = now_us(); st32(ARG(0), (uint32_t)v); st32(ARG(0) + 4, (uint32_t)(v >> 32)); return 1; }
static uint32_t k_QueryPerformanceFrequency(CPU *c) { st32(ARG(0), 1000000U); st32(ARG(0) + 4, 0); return 1; }
static uint32_t k_Sleep(CPU *c) { uint32_t ms = ARG(0); if (ms) rt_sleep_us(ms * 1000U); else rt_yield(); return 0; }

/* heap: every HeapCreate heap is the one guest heap */
static uint32_t k_HeapCreate(CPU *c) { (void)c; return 0x00520000U; }
static uint32_t k_HeapDestroy(CPU *c) { (void)c; return 1; }
static uint32_t k_HeapAlloc(CPU *c)
{
    pthread_mutex_lock(&big);
    uint32_t p = rt_alloc(ARG(2) ? ARG(2) : 1);
    pthread_mutex_unlock(&big);
    if (p && (ARG(1) & 8U)) memset(G2H(p), 0, ARG(2));
    return p;
}
static uint32_t k_HeapFree(CPU *c) { pthread_mutex_lock(&big); rt_free(ARG(2)); pthread_mutex_unlock(&big); return 1; }
static uint32_t k_HeapSize(CPU *c) { return rt_alloc_size(ARG(2)); }
static uint32_t k_HeapReAlloc(CPU *c)
{
    pthread_mutex_lock(&big);
    uint32_t old = rt_alloc_size(ARG(2)), p = rt_realloc(ARG(2), ARG(3));
    pthread_mutex_unlock(&big);
    if (p && (ARG(1) & 8U) && ARG(3) > old) memset(G2H(p + old), 0, ARG(3) - old);
    return p;
}
static uint32_t k_VirtualAlloc(CPU *c)
{
    if (ARG(0) && !(ARG(2) & 0x2000U)) return ARG(0);
    pthread_mutex_lock(&big);
    uint32_t p = rt_alloc(ARG(1) + 0xFFFU);
    pthread_mutex_unlock(&big);
    if (!p) return 0;
    uint32_t a = (p + 0xFFFU) & ~0xFFFU;
    memset(G2H(a), 0, ARG(1));
    return a;
}
static uint32_t k_VirtualFree(CPU *c) { (void)c; return 1; }
static uint32_t k_VirtualProtect(CPU *c) { if (ARG(3)) st32(ARG(3), 4); return 1; }
static uint32_t k_VirtualQuery(CPU *c)
{
    uint32_t p = ARG(1), a = ARG(0) & ~0xFFFU;
    st32(p, a); st32(p + 4, a); st32(p + 8, 4); st32(p + 12, 0x1000); st32(p + 16, 0x1000); st32(p + 20, 4); st32(p + 24, 0x20000);
    return 28;
}
static uint32_t k_IsBad(CPU *c) { (void)c; return 0; }

/* critical sections: DebugInfo holds a host recursive mutex index + 1 */
#define MAX_CS 512
static pthread_mutex_t cs_locks[MAX_CS];
static uint32_t cs_count;
static pthread_mutex_t *cs_of(uint32_t p)
{
    uint32_t id = ld32(p);
    if (id == 0 || id > MAX_CS) {
        pthread_mutex_lock(&big);
        id = ld32(p);
        if (id == 0 || id > MAX_CS) {
            pthread_mutexattr_t a;
            pthread_mutexattr_init(&a);
            pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
            pthread_mutex_init(&cs_locks[cs_count], &a);
            id = ++cs_count;
            st32(p, id);
        }
        pthread_mutex_unlock(&big);
    }
    return &cs_locks[id - 1];
}
static uint32_t k_InitializeCriticalSectionAndSpinCount(CPU *c) { memset(G2H(ARG(0)), 0, 24); st32(ARG(0) + 4, 0xFFFFFFFFU); (void)cs_of(ARG(0)); return 1; }
static uint32_t k_InitializeCriticalSection(CPU *c) { memset(G2H(ARG(0)), 0, 24); st32(ARG(0) + 4, 0xFFFFFFFFU); (void)cs_of(ARG(0)); return 0; }
static uint32_t k_DeleteCriticalSection(CPU *c) { (void)c; return 0; }
static uint32_t cs_owner[MAX_CS], cs_depth[MAX_CS];   /* watchdog: owning tid and recursion depth */
static uint32_t k_EnterCriticalSection(CPU *c)
{
    pthread_mutex_t *m = cs_of(ARG(0));
    uint32_t id = (uint32_t)(m - cs_locks);
    rt_thread_ctx *x = ctx_of_cpu(c);
    if (x) x->waiting_cs = ARG(0);
    pthread_mutex_lock(m);
    cs_owner[id] = x ? x->tid : 0;
    cs_depth[id]++;
    if (x) { x->waiting_cs = 0; x->cs_held++; }
    return 0;
}
static uint32_t k_LeaveCriticalSection(CPU *c)
{
    pthread_mutex_t *m = cs_of(ARG(0));
    uint32_t id = (uint32_t)(m - cs_locks);
    rt_thread_ctx *x = ctx_of_cpu(c);
    if (x && x->cs_held > 0) x->cs_held--;
    if (cs_depth[id] > 0 && --cs_depth[id] == 0) cs_owner[id] = 0;
    pthread_mutex_unlock(m);
    return 0;
}
static uint32_t k_InterlockedExchange(CPU *c)
{
    pthread_mutex_lock(&big);
    uint32_t old = ld32(ARG(0));
    st32(ARG(0), ARG(1));
    pthread_mutex_unlock(&big);
    return old;
}

/* TLS */
static uint32_t tls_used;
static uint32_t k_TlsAlloc(CPU *c) { (void)c; pthread_mutex_lock(&big); uint32_t i = tls_used < 64 ? tls_used++ : 0xFFFFFFFFU; pthread_mutex_unlock(&big); return i; }
static uint32_t k_TlsFree(CPU *c) { (void)c; return 1; }
static uint32_t k_TlsSetValue(CPU *c) { st32(rt_current_teb(c) + 0xE10U + 4U * ARG(0), ARG(1)); return 1; }
static uint32_t k_TlsGetValue(CPU *c) { c->last_error = 0; return ld32(rt_current_teb(c) + 0xE10U + 4U * ARG(0)); }

/* locale: code page 1252, LCID 0x409 */
static uint32_t k_GetACP(CPU *c) { (void)c; return 1252; }
static uint32_t k_GetOEMCP(CPU *c) { (void)c; return 437; }
static uint32_t k_GetUserDefaultLCID(CPU *c) { (void)c; return 0x409; }
static uint32_t k_IsValidCodePage(CPU *c) { return ARG(0) == 1252 || ARG(0) == 437; }
static uint32_t k_IsValidLocale(CPU *c) { return ARG(0) == 0x409; }
static uint32_t k_GetCPInfo(CPU *c)
{
    uint32_t p = ARG(1);
    memset(G2H(p), 0, 20);
    st32(p, 1); st8(p + 4, '?');
    return 1;
}
static uint32_t k_MultiByteToWideChar(CPU *c)
{
    uint32_t src = ARG(2), dst = ARG(4), dn = ARG(5);
    int32_t n = (int32_t)ARG(3);
    if (n < 0) n = (int32_t)strlen(GSTR(src)) + 1;
    if (dn == 0) return (uint32_t)n;
    if ((uint32_t)n > dn) n = (int32_t)dn;
    for (int32_t i = 0; i < n; i++) st16(dst + 2U * (uint32_t)i, ld8(src + (uint32_t)i));
    return (uint32_t)n;
}
static uint32_t k_WideCharToMultiByte(CPU *c)
{
    uint32_t src = ARG(2), dst = ARG(4), dn = ARG(5);
    int32_t n = (int32_t)ARG(3);
    if (n < 0) { n = 0; while (ld16(src + 2U * (uint32_t)n)) n++; n++; }
    if (dn == 0) return (uint32_t)n;
    if ((uint32_t)n > dn) n = (int32_t)dn;
    for (int32_t i = 0; i < n; i++) { uint16_t w = ld16(src + 2U * (uint32_t)i); st8(dst + (uint32_t)i, w < 256 ? (uint8_t)w : '?'); }
    if (ARG(7)) st32(ARG(7), 0);
    return (uint32_t)n;
}
static uint32_t k_GetStringTypeA(CPU *c)
{
    uint32_t src = ARG(2), out = ARG(4);
    int32_t n = (int32_t)ARG(3);
    if (n < 0) n = (int32_t)strlen(GSTR(src)) + 1;
    for (int32_t i = 0; i < n; i++) {
        int ch = ld8(src + (uint32_t)i);
        uint16_t t = 0;
        if (isupper(ch)) t |= 1; if (islower(ch)) t |= 2; if (isdigit(ch)) t |= 4; if (isspace(ch)) t |= 8;
        if (ispunct(ch)) t |= 0x10; if (iscntrl(ch)) t |= 0x20; if (ch == ' ' || ch == '\t') t |= 0x40;
        if (isxdigit(ch)) t |= 0x80; if (isalpha(ch)) t |= 0x100;
        st16(out + 2U * (uint32_t)i, t);
    }
    return 1;
}
static uint32_t k_GetStringTypeW(CPU *c)
{
    uint32_t src = ARG(1), out = ARG(3);
    int32_t n = (int32_t)ARG(2);
    if (n < 0) { n = 0; while (ld16(src + 2U * (uint32_t)n)) n++; n++; }
    for (int32_t i = 0; i < n; i++) {
        int ch = ld16(src + 2U * (uint32_t)i);
        uint16_t t = 0;
        if (ch < 128) {
            if (isupper(ch)) t |= 1; if (islower(ch)) t |= 2; if (isdigit(ch)) t |= 4; if (isspace(ch)) t |= 8;
            if (ispunct(ch)) t |= 0x10; if (iscntrl(ch)) t |= 0x20; if (ch == ' ' || ch == '\t') t |= 0x40;
            if (isxdigit(ch)) t |= 0x80; if (isalpha(ch)) t |= 0x100;
        }
        st16(out + 2U * (uint32_t)i, t);
    }
    return 1;
}
static uint32_t k_LCMapStringA(CPU *c)
{
    uint32_t flags = ARG(1), src = ARG(2), dst = ARG(4), dn = ARG(5);
    int32_t n = (int32_t)ARG(3);
    if (n < 0) n = (int32_t)strlen(GSTR(src)) + 1;
    if (dn == 0) return (uint32_t)n;
    for (int32_t i = 0; i < n && (uint32_t)i < dn; i++) {
        int ch = ld8(src + (uint32_t)i);
        st8(dst + (uint32_t)i, (uint8_t)((flags & 0x100U) ? tolower(ch) : (flags & 0x200U) ? toupper(ch) : ch));
    }
    return (uint32_t)n;
}
static uint32_t k_LCMapStringW(CPU *c)
{
    uint32_t flags = ARG(1), src = ARG(2), dst = ARG(4), dn = ARG(5);
    int32_t n = (int32_t)ARG(3);
    if (n < 0) { n = 0; while (ld16(src + 2U * (uint32_t)n)) n++; n++; }
    if (dn == 0) return (uint32_t)n;
    for (int32_t i = 0; i < n && (uint32_t)i < dn; i++) {
        int ch = ld16(src + 2U * (uint32_t)i);
        if (ch < 128) ch = (flags & 0x100U) ? tolower(ch) : (flags & 0x200U) ? toupper(ch) : ch;
        st16(dst + 2U * (uint32_t)i, (uint16_t)ch);
    }
    return (uint32_t)n;
}
static uint32_t k_GetLocaleInfoA(CPU *c)
{
    char buf[16];
    snprintf(buf, sizeof buf, "%u", 1252U);
    uint32_t n = (uint32_t)strlen(buf) + 1;
    if (ARG(3) == 0) return n;
    memcpy(G2H(ARG(2)), buf, n);
    return n;
}
static uint32_t k_GetLocaleInfoW(CPU *c)
{
    const char *buf = "1252";
    uint32_t n = 5;
    if (ARG(3) == 0) return n;
    for (uint32_t i = 0; i < n; i++) st16(ARG(2) + 2U * i, (uint16_t)buf[i]);
    return n;
}
static uint32_t k_EnumSystemLocalesA(CPU *c) { (void)c; return 1; }
static uint32_t k_lstrlenA(CPU *c) { return ARG(0) ? (uint32_t)strlen(GSTR(ARG(0))) : 0; }
static uint32_t k_lstrcmpiA(CPU *c) { return (uint32_t)strcasecmp(GSTR(ARG(0)), GSTR(ARG(1))); }

/* files */
static uint32_t k_CreateFileA(CPU *c)
{
    char p[1024];
    host_path(GSTR(ARG(0)), p, sizeof p);
    uint32_t access = ARG(1), disp = ARG(4);
    const char *mode = "rb";
    if (access & 0x40000000U) {
        if (disp == 2 || disp == 1) mode = (access & 0x80000000U) ? "w+b" : "wb";
        else if (disp == 4) { FILE *t = fopen(p, "ab"); if (t) fclose(t); mode = "r+b"; }
        else mode = "r+b";
    }
    FILE *f = fopen(p, mode);
    rt_log("[w32] CreateFileA(%s, %s, flags %08x) -> %s\n", GSTR(ARG(0)), mode, ARG(5), f ? "ok" : "missing");
    if (!f) return ret_err(INVALID_HANDLE, ERROR_FILE_NOT_FOUND);
    uint32_t h = h_new(H_FILE);
    h_get(h, H_FILE)->f = f;
    const char *base = strrchr(p, '/');
    base = base ? base + 1 : p;
    if (strncasecmp(base, "TMNT_V0", 7) == 0 && base[7] >= '0' && base[7] <= '2') {
        h_get(h, H_FILE)->movie = base[7] - '0' + 1;
        movie_note_open(base[7] - '0', p);
    }
    return h;
}
static uint32_t k_ReadFile(CPU *c)
{
    rt_handle *h = h_get(ARG(0), H_FILE);
    if (!h) return ret_err(0, ERROR_INVALID_HANDLE);
    if (h->movie) movie_note_read(h->movie - 1, (uint64_t)ftello(h->f));
    size_t got = fread(G2H(ARG(1)), 1, ARG(2), h->f);
    if (ARG(3)) st32(ARG(3), (uint32_t)got);
    return 1;
}
static uint32_t k_WriteFile(CPU *c)
{
    uint32_t hv = ARG(0);
    if (hv == 0x14U || hv == 0x18U) {
        fwrite(G2H(ARG(1)), 1, ARG(2), stderr);
        if (ARG(3)) st32(ARG(3), ARG(2));
        return 1;
    }
    rt_handle *h = h_get(hv, H_FILE);
    if (!h) return ret_err(0, ERROR_INVALID_HANDLE);
    size_t put = fwrite(G2H(ARG(1)), 1, ARG(2), h->f);
    if (ARG(3)) st32(ARG(3), (uint32_t)put);
    return 1;
}
static uint32_t k_SetFilePointer(CPU *c)
{
    rt_handle *h = h_get(ARG(0), H_FILE);
    if (!h) return ret_err(INVALID_HANDLE, ERROR_INVALID_HANDLE);
    int64_t off = (int32_t)ARG(1);
    if (ARG(2)) off = (int64_t)(((uint64_t)ld32(ARG(2)) << 32) | ARG(1));
    fseeko(h->f, off, ARG(3) == 0 ? SEEK_SET : ARG(3) == 1 ? SEEK_CUR : SEEK_END);
    int64_t pos = ftello(h->f);
    if (ARG(2)) st32(ARG(2), (uint32_t)(pos >> 32));
    return (uint32_t)pos;
}
static uint32_t k_GetFileSize(CPU *c)
{
    rt_handle *h = h_get(ARG(0), H_FILE);
    if (!h) return ret_err(INVALID_HANDLE, ERROR_INVALID_HANDLE);
    struct stat st;
    fstat(fileno(h->f), &st);
    if (ARG(1)) st32(ARG(1), (uint32_t)((uint64_t)st.st_size >> 32));
    return (uint32_t)st.st_size;
}
static uint32_t k_SetEndOfFile(CPU *c)
{
    rt_handle *h = h_get(ARG(0), H_FILE);
    if (!h) return 0;
    fflush(h->f);
    return ftruncate(fileno(h->f), ftello(h->f)) == 0;
}
static uint32_t k_FlushFileBuffers(CPU *c) { rt_handle *h = h_get(ARG(0), H_FILE); if (h) fflush(h->f); return 1; }
static uint32_t k_GetFullPathNameA(CPU *c)
{
    const char *n = GSTR(ARG(0));
    char buf[512];
    if (n[1] == ':') snprintf(buf, sizeof buf, "%s", n); else snprintf(buf, sizeof buf, FAKE_DIR "%s", n);
    uint32_t len = (uint32_t)strlen(buf);
    if (len + 1 > ARG(1)) return len + 1;
    memcpy(G2H(ARG(2)), buf, len + 1);
    if (ARG(3)) { char *s = strrchr(buf, '\\'); st32(ARG(3), ARG(2) + (uint32_t)(s ? s + 1 - buf : 0)); }
    return len;
}

static uint32_t k_CloseHandle(CPU *c)
{
    rt_handle *h = h_get(ARG(0), H_FREE);
    if (!h) return 1;
    if (h->type == H_FILE && h->f) fclose(h->f);
    if (h->type == H_THREAD) return 1;
    h->type = H_FREE;
    return 1;
}

/* events, mutexes, threads */
static uint32_t k_CreateEventA(CPU *c)
{
    uint32_t h = h_new(H_EVENT);
    rt_handle *e = h_get(h, H_EVENT);
    e->manual = ARG(1) != 0;
    e->signaled = ARG(2) != 0;
    return h;
}
static uint32_t k_SetEvent_impl(rt_handle *e, int pulse)
{
    rt_cond_lock(&e->wake);
    if (pulse) e->pulses++; else e->signaled = 1;
    rt_cond_broadcast(&e->wake);
    rt_cond_unlock(&e->wake);
    return 1;
}
static uint32_t k_PulseEvent(CPU *c) { rt_handle *e = h_get(ARG(0), H_EVENT); return e ? k_SetEvent_impl(e, 1) : 0; }
static uint32_t k_CreateMutexA(CPU *c)
{
    uint32_t h = h_new(H_MUTEX);
    rt_handle *m = h_get(h, H_MUTEX);
    if (ARG(1)) { m->owner = k_GetCurrentThreadId(c); m->count = 1; }
    return h;
}
static uint32_t k_ReleaseMutex(CPU *c)
{
    rt_handle *m = h_get(ARG(0), H_MUTEX);
    if (!m) return 0;
    rt_cond_lock(&m->wake);
    if (m->count && --m->count == 0) { m->owner = 0; rt_cond_broadcast(&m->wake); }
    rt_cond_unlock(&m->wake);
    return 1;
}
static int wait_until(rt_handle *h, uint32_t ms, int (*ready)(rt_handle *, uint32_t), uint32_t tid)
{
    uint64_t deadline = rt_now_us() + (uint64_t)ms * 1000U;
    rt_cond_lock(&h->wake);
    uint32_t pulses = h->pulses;
    /* A PulseEvent releases the threads already waiting, without leaving the event signaled. */
    while (!ready(h, tid) && h->pulses == pulses) {
        if (ms == 0) { rt_cond_unlock(&h->wake); return 0; }
        if (ms == 0xFFFFFFFFU) {
            rt_cond_wait_ms(&h->wake, 0xFFFFFFFFU);
            continue;
        }
        uint64_t now = rt_now_us();
        if (now >= deadline) { rt_cond_unlock(&h->wake); return 0; }
        uint32_t left = (uint32_t)((deadline - now + 999U) / 1000U);
        rt_cond_wait_ms(&h->wake, left ? left : 1);
    }
    rt_cond_unlock(&h->wake);
    return 1;
}
static int ev_ready(rt_handle *h, uint32_t tid) { (void)tid; if (!h->signaled) return 0; if (!h->manual) h->signaled = 0; return 1; }
static int mx_ready(rt_handle *h, uint32_t tid) { if (h->count && h->owner != tid) return 0; h->owner = tid; h->count++; return 1; }
static int th_ready(rt_handle *h, uint32_t tid) { (void)tid; return h->signaled; }
static uint32_t k_WaitForSingleObject(CPU *c)
{
    rt_handle *h = h_get(ARG(0), H_FREE);
    if (!h) return 0xFFFFFFFFU;
    int ok = h->type == H_EVENT ? wait_until(h, ARG(1), ev_ready, 0)
           : h->type == H_MUTEX ? wait_until(h, ARG(1), mx_ready, k_GetCurrentThreadId(c))
           : h->type == H_THREAD ? wait_until(h, ARG(1), th_ready, 0) : 1;
    return ok ? 0 : 0x102U;
}

static void *thread_main(void *arg)
{
    rt_handle *h = arg;
    rt_thread_ctx *x = h->tctx;
    rt_thread_place_self(0, x->win_prio);
    x->os_id = rt_thread_os_id();
    char label[24];
    snprintf(label, sizeof label, "guest %u", x->tid);
    plat_thread_label(label);
    CPU *c = &x->cpu;
    uint32_t a[1] = { h->param };
    rt_call_guest(c, h->start, 1, a);
    rt_cond_lock(&h->wake);
    h->signaled = 1;
    rt_cond_broadcast(&h->wake);
    rt_cond_unlock(&h->wake);
    return NULL;
}
/* Translated code keeps every guest call on the native stack, so host threads get a large one. */
#define HOST_THREAD_STACK (4U << 20)
static void start_host_thread(pthread_t *th, void *(*fn)(void *), void *arg)
{
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, HOST_THREAD_STACK);
    pthread_create(th, &a, fn, arg);
    pthread_attr_destroy(&a);
}
static void start_guest_thread(rt_handle *t) { start_host_thread(&t->thread, thread_main, t); }

/* A guest thread context: CPU, guest stack and TEB. */
static rt_thread_ctx *new_thread_ctx(uint32_t stack)
{
    rt_thread_ctx *t = calloc(1, sizeof *t);
    rt_cond_init(&t->wake);
    pthread_mutex_lock(&big);
    uint32_t base = rt_alloc(stack + 0x1000U);
    uint32_t teb = rt_alloc(0x1000U);
    t->tid = ++next_tid;
    if (ctx_count < MAX_CTX) all_ctx[ctx_count++] = t;
    pthread_mutex_unlock(&big);
    memset(G2H(teb), 0, 0x1000);
    st32(teb + 0x00, 0xFFFFFFFFU); st32(teb + 0x04, base + stack); st32(teb + 0x08, base);
    st32(teb + 0x18, teb); st32(teb + 0x24, t->tid); st32(teb + 0x30, GUEST_PEB);
    t->teb = teb;
    CPU *nc = &t->cpu;
    nc->fcw = 0x027F;
    nc->esp = (base + stack) & ~15U;
    nc->stack_owner = nc; nc->stack_floor = base; nc->stack_ceiling = nc->esp; nc->stack_low_water = nc->esp;
    return t;
}

static uint32_t k_CreateThread(CPU *c)
{
    uint32_t stack = ARG(1) ? ARG(1) : (1U << 20);
    if (stack < (256U << 10)) stack = 256U << 10;
    uint32_t h = h_new(H_THREAD);
    rt_handle *t = h_get(h, H_THREAD);
    t->tctx = new_thread_ctx(stack);
    t->start = ARG(2);
    t->param = ARG(3);
    if (ARG(5)) st32(ARG(5), t->tctx->tid);
    rt_log("[w32] CreateThread(start %08x, param %08x, flags %x) -> tid %u\n", t->start, t->param, ARG(4), t->tctx->tid);
    if (ARG(4) & 4U) { t->suspended = 1; t->tctx->suspend = 1; }
    else start_guest_thread(t);
    return h;
}
/* Suspend counts as on Windows: a thread suspended by itself blocks at once, one suspended by another stops at its next API call
 * (rt_safepoint) made outside every critical section. Windows stops it on the spot; stopping later while it holds a lock would
 * deadlock whoever waits for that lock (seen on the Vita: CRI ADX's timer waiting on a suspended worker's critical section). */
static rt_thread_ctx *ctx_of(CPU *c, uint32_t h, rt_handle **th)
{
    *th = NULL;
    if (h == 0xFFFFFFFEU) return ctx_of_cpu(c);
    rt_handle *t = h_get(h, H_THREAD);
    *th = t;
    return t ? t->tctx : NULL;
}
static void block_while_suspended(rt_thread_ctx *x)
{
    rt_cond_lock(&x->wake);
    while (x->suspend > 0) rt_cond_wait_ms(&x->wake, 0xFFFFFFFFU);
    rt_cond_unlock(&x->wake);
}
static const rt_thunk *no_ctx_thunk;
/* Called on every API call: records the thunk being called (COM dispatch reads it) and stops a suspended thread. */
void rt_safepoint(CPU *c, const rt_thunk *t)
{
    rt_thread_ctx *x = ctx_of_cpu(c);
    if (!x) { no_ctx_thunk = t; return; }
    x->thunk = t;
    if (x->suspend > 0 && x->cs_held == 0) block_while_suspended(x);
}
const rt_thunk *rt_current_thunk(CPU *c)
{
    rt_thread_ctx *x = ctx_of_cpu(c);
    return x ? x->thunk : no_ctx_thunk;
}
static uint32_t k_ResumeThread(CPU *c)
{
    rt_handle *t;
    rt_thread_ctx *x = ctx_of(c, ARG(0), &t);
    if (!x) return 0xFFFFFFFFU;
    rt_cond_lock(&x->wake);
    int prev = x->suspend;
    if (x->suspend > 0) x->suspend--;
    int run = x->suspend == 0;
    rt_cond_broadcast(&x->wake);
    rt_cond_unlock(&x->wake);
    if (run && t && t->suspended) { t->suspended = 0; start_guest_thread(t); }
    return (uint32_t)prev;
}
static uint32_t k_SuspendThread(CPU *c)
{
    rt_handle *t;
    rt_thread_ctx *x = ctx_of(c, ARG(0), &t);
    if (!x) return 0xFFFFFFFFU;
    rt_cond_lock(&x->wake);
    int prev = x->suspend++;
    rt_cond_unlock(&x->wake);
    if (x == ctx_of_cpu(c)) {
        c->eax = (uint32_t)prev;
        block_while_suspended(x);
    }
    return (uint32_t)prev;
}
static uint32_t k_ExitThread(CPU *c) { (void)c; pthread_exit(NULL); return 0; }
static uint32_t k_SetThreadPriority(CPU *c)
{
    rt_handle *t;
    rt_thread_ctx *x = ctx_of(c, ARG(0), &t);
    if (!x) return 0;
    x->win_prio = (int)ARG(1);
    if (x->os_id) rt_thread_set_priority(x->os_id, x->win_prio);
    return 1;
}
static uint32_t k_GetThreadPriority(CPU *c)
{
    rt_handle *t;
    rt_thread_ctx *x = ctx_of(c, ARG(0), &t);
    return x ? (uint32_t)x->win_prio : 0;
}
static uint32_t k_SetThreadAffinityMask(CPU *c) { (void)c; return 1; }
static uint32_t k_SetThreadPriorityBoost(CPU *c) { (void)c; return 1; }

static uint32_t k_ExitProcess(CPU *c) { rt_log("[w32] ExitProcess(%u)\n", ARG(0)); exit((int)ARG(0)); return 0; }
static uint32_t k_TerminateProcess(CPU *c) { rt_log("[w32] TerminateProcess(%u)\n", ARG(1)); exit((int)ARG(1)); return 0; }
static uint32_t k_SetUnhandledExceptionFilter(CPU *c) { (void)c; return 0; }
static uint32_t k_UnhandledExceptionFilter(CPU *c) { (void)c; return 0; }
static uint32_t k_RaiseException(CPU *c) { rt_log("[w32] RaiseException(%08x)\n", ARG(0)); guest_fault(c, 0, "RaiseException"); return 0; }
static uint32_t k_RtlUnwind(CPU *c)
{
    /* Pop SEH registrations down to the target frame; the return value is passed through. */
    uint32_t teb = rt_current_teb(c), frame = ARG(0);
    if (frame) st32(teb, frame); else st32(teb, 0xFFFFFFFFU);
    c->eax = ARG(3);
    return ARG(3);
}

/* modules */
static uint32_t k_LoadLibraryA(CPU *c) { rt_log("[w32] LoadLibraryA(%s)\n", GSTR(ARG(0))); return 0x10000000U; }
static uint32_t k_GetProcAddress(CPU *c)
{
    char name[128];
    const char *n = GSTR(ARG(1));
    if (ARG(1) < 0x10000U) {
        rt_log("[w32] GetProcAddress by ordinal %u\n", ARG(1));
        return 0;
    }
    snprintf(name, sizeof name, "%s!%s", ARG(0) == 0x10000000U ? "DINPUT8" : ARG(0) == KERNEL32_HANDLE ? "KERNEL32" : "UNKNOWN", n);
    const rt_thunk *t = rt_find_import(name);
    rt_log("[w32] GetProcAddress(%s) -> %s\n", name, t->fn ? "host" : "missing");
    return t->fn ? rt_add_thunk(t) : 0;
}

/* registry: nothing is installed */
static uint32_t a_RegOpenKeyA(CPU *c) { rt_log("[w32] RegOpenKeyA(%s)\n", ARG(1) ? GSTR(ARG(1)) : ""); return 2; }
static uint32_t a_RegCloseKey(CPU *c) { (void)c; return 0; }
static uint32_t a_RegQueryValueExA(CPU *c) { (void)c; return 2; }

/* ------------------------------------------------------------- winmm --- */
static uint32_t w_timeGetTime(CPU *c) { (void)c; return (uint32_t)(now_us() / 1000U); }
static uint32_t w_timeBeginPeriod(CPU *c) { (void)c; return 0; }
static uint32_t w_timeGetDevCaps(CPU *c) { st32(ARG(0), 1); st32(ARG(0) + 4, 1000000); return 0; }

/* timeSetEvent: one persistent host thread per timer slot, woken when the slot is armed; it signals the event or calls the guest
 * TimeProc on its own guest context. The game re-arms a one-shot timer every ~15 ms: a new host thread each time was slow on the
 * Vita and hung there at the new thread's first thread-local access. */
typedef struct rt_timer {
    uint32_t id, delay, cb, user, flags;
    volatile int active, running;
    int started;
    pthread_t thread;
    rt_thread_ctx *ctx;
    rt_cond arm;
} rt_timer;
#define MAX_TIMERS 16
static rt_timer timers[MAX_TIMERS];
static uint32_t timer_arms;

static void *timer_main(void *arg)
{
    rt_timer *t = arg;
    /* Windows runs multimedia timer callbacks at time-critical priority. */
    rt_thread_place_self(0, 15);
    char label[24];
    snprintf(label, sizeof label, "timer %d", (int)(t - timers));
    plat_thread_label(label);
    for (;;) {
        rt_cond_lock(&t->arm);
        while (!t->running) rt_cond_wait_ms(&t->arm, 0xFFFFFFFFU);
        rt_cond_unlock(&t->arm);
        uint64_t next = now_us();
        do {
            next += (uint64_t)t->delay * 1000U;
            uint64_t now = now_us();
            if (next > now) rt_sleep_us(next - now);
            else next = now;
            if (!t->active) break;
            if (t->flags & 0x30U) {
                rt_handle *e = h_get(t->cb, H_EVENT);
                if (e) k_SetEvent_impl(e, (t->flags & 0x20U) != 0);
            } else {
                uint32_t a[5] = { t->id, 0, t->user, 0, 0 };
                rt_call_guest(&t->ctx->cpu, t->cb, 5, a);
            }
        } while (t->active && (t->flags & 1U));
        rt_cond_lock(&t->arm);
        t->active = 0;
        t->running = 0;
        rt_cond_unlock(&t->arm);
    }
    return NULL;
}
static uint32_t w_timeSetEvent(CPU *c)
{
    pthread_mutex_lock(&big);
    for (uint32_t i = 0; i < MAX_TIMERS; i++) {
        rt_timer *t = &timers[i];
        if (t->active || t->running) continue;
        if (!t->ctx) { t->ctx = new_thread_ctx(256U << 10); rt_cond_init(&t->arm); }
        rt_cond_lock(&t->arm);
        t->id = i + 1; t->delay = ARG(0) ? ARG(0) : 1; t->cb = ARG(2); t->user = ARG(3); t->flags = ARG(4);
        t->active = 1;
        t->running = 1;
        rt_cond_broadcast(&t->arm);
        rt_cond_unlock(&t->arm);
        if (!t->started) {
            t->started = 1;
            start_host_thread(&t->thread, timer_main, t);
            pthread_detach(t->thread);
        }
        pthread_mutex_unlock(&big);
        /* The game re-arms every frame or so: only the first arms are logged. */
        if (++timer_arms <= 8)
            rt_log("[w32] timeSetEvent(%u ms, %s %08x, flags %x) -> %u\n", t->delay, (t->flags & 0x30U) ? "event" : "proc", t->cb, t->flags, t->id);
        return t->id;
    }
    pthread_mutex_unlock(&big);
    (void)c;
    return 0;
}
static uint32_t w_timeKillEvent(CPU *c)
{
    uint32_t id = ARG(0);
    if (id == 0 || id > MAX_TIMERS) return 97;
    timers[id - 1].active = 0;
    return 0;
}

/* ---------------------------------------------------- user32 and gdi32 --- */
static uint32_t wndproc;
static uint32_t u_RegisterClassA(CPU *c) { wndproc = ld32(ARG(0) + 4); rt_log("[w32] RegisterClassA(%s) wndproc %08x\n", GSTR(ld32(ARG(0) + 36)), wndproc); return 0xC001; }
static uint32_t u_CreateWindowExA(CPU *c) { rt_log("[w32] CreateWindowExA(%s, %ux%u)\n", ARG(2) ? GSTR(ARG(2)) : "", ARG(6), ARG(7)); return 0x00020000U; }
static uint32_t u_One(CPU *c) { (void)c; return 1; }
static uint32_t u_Zero(CPU *c) { (void)c; return 0; }
static uint32_t u_GetSystemMetrics(CPU *c) { return ARG(0) == 0 ? 1024 : ARG(0) == 1 ? 768 : 0; }
static uint32_t u_Rect(CPU *c)
{
    int w = 640, h = 480;
    plat_game_size(&w, &h);
    if (w <= 0 || h <= 0) { w = 640; h = 480; }
    st32(ARG(1), 0); st32(ARG(1) + 4, 0); st32(ARG(1) + 8, (uint32_t)w); st32(ARG(1) + 12, (uint32_t)h);
    return 1;
}
static uint32_t u_SetRect(CPU *c) { st32(ARG(0), ARG(1)); st32(ARG(0) + 4, ARG(2)); st32(ARG(0) + 8, ARG(3)); st32(ARG(0) + 12, ARG(4)); return 1; }
static uint32_t u_MessageBoxA(CPU *c) { rt_log("[w32] MessageBoxA(%s: %s)\n", ARG(2) ? GSTR(ARG(2)) : "", ARG(1) ? GSTR(ARG(1)) : ""); return 1; }
/* A small posted-message queue for the game window; DispatchMessageA calls the guest WndProc. */
typedef struct { uint32_t hwnd, msg, wparam, lparam; } rt_msg;
static rt_msg msgq[64];
static int msgq_head, msgq_count;
static int activated;

static void post_msg(uint32_t msg, uint32_t w, uint32_t l)
{
    if (msgq_count >= 64) return;
    msgq[(msgq_head + msgq_count++) % 64] = (rt_msg){ 0x00020000U, msg, w, l };
}

static uint32_t u_PeekMessageA(CPU *c)
{
    uint32_t p = ARG(0);
    if (!plat_pump()) post_msg(0x12U, 0, 0);
    if (!msgq_count) return 0;
    rt_msg *m = &msgq[msgq_head];
    memset(G2H(p), 0, 28);
    st32(p, m->hwnd); st32(p + 4, m->msg); st32(p + 8, m->wparam); st32(p + 12, m->lparam);
    st32(p + 16, (uint32_t)(now_us() / 1000U));
    if (ARG(4) & 1U) { msgq_head = (msgq_head + 1) % 64; msgq_count--; }
    return 1;
}
static uint32_t u_DispatchMessageA(CPU *c)
{
    uint32_t p = ARG(0);
    if (!wndproc || ld32(p + 4) == 0x12U) return 0;
    uint32_t a[4] = { ld32(p), ld32(p + 4), ld32(p + 8), ld32(p + 12) };
    return rt_call_guest(c, wndproc, 4, a);
}
static uint32_t u_ShowWindow(CPU *c)
{
    if (ARG(1) && !activated) {
        activated = 1;
        post_msg(0x001CU, 1, 0);   /* WM_ACTIVATEAPP */
        post_msg(0x0006U, 1, 0);   /* WM_ACTIVATE, WA_ACTIVE */
        post_msg(0x0007U, 0, 0);   /* WM_SETFOCUS */
    }
    return 1;
}
uint32_t dinput8_create(CPU *c, uint32_t pp);
static uint32_t di_DirectInput8Create(CPU *c) { rt_log("[w32] DirectInput8Create(version %x)\n", ARG(1)); return dinput8_create(c, ARG(3)); }
static uint32_t u_DefWindowProcA(CPU *c) { (void)c; return 0; }
static uint32_t u_PostQuitMessage(CPU *c) { rt_log("[w32] PostQuitMessage(%u)\n", ARG(0)); return 0; }
static uint32_t u_FindWindowA(CPU *c) { (void)c; return 0; }
static uint32_t u_LoadRes(CPU *c) { (void)c; return 0x00030000U; }
static uint32_t g_GetStockObject(CPU *c) { return 0x00040000U + ARG(0); }

/* ------------------------------------------------- not yet implemented --- */
uint32_t d3d9_create(void);
void rt_selftest_pow(CPU *c);
static uint32_t d3d_Direct3DCreate9(CPU *c)
{
    rt_log("[w32] Direct3DCreate9(%u)\n", ARG(0));
    if (getenv("TMNT_SELFTEST_POW")) rt_selftest_pow(c);
    if (getenv("TMNT_SELFTEST_BLOCKS")) rt_selftest_blocks(c, getenv("TMNT_SELFTEST_BLOCKS"));
    if (getenv("TMNT_SELFTEST_MC")) rt_selftest_mc(c, getenv("TMNT_SELFTEST_MC"));
    return d3d9_create();
}
uint32_t dsound_create(uint32_t pp);
static uint32_t ds_DirectSoundCreate(CPU *c) { rt_log("[w32] DirectSoundCreate\n"); return dsound_create(ARG(1)); }

/* --------------------------------------------------------------- table --- */

#define K(n, args) { "KERNEL32!" #n, k_##n, args }
static const rt_thunk table[] = {
    K(GetModuleFileNameA, 3), K(GetModuleHandleA, 1), K(CloseHandle, 1), K(ReleaseMutex, 1), K(WaitForSingleObject, 2),
    K(CreateMutexA, 3), K(GetProcAddress, 2), K(LoadLibraryA, 1), K(WriteFile, 5), K(CreateFileA, 7), K(ReadFile, 5),
    K(GetLocalTime, 1), K(SetThreadPriority, 2), K(GetThreadPriority, 1), K(GetCurrentThread, 0), K(GetVersionExA, 1),
    K(GetCurrentThreadId, 0), K(GetCurrentProcessId, 0), K(SetCurrentDirectoryA, 1), K(GetSystemInfo, 1),
    K(IsProcessorFeaturePresent, 1), K(EnterCriticalSection, 1), K(LeaveCriticalSection, 1), K(InterlockedExchange, 2),
    K(ResumeThread, 1), K(SuspendThread, 1), K(CreateEventA, 4), K(QueryPerformanceCounter, 1), K(QueryPerformanceFrequency, 1),
    K(InitializeCriticalSection, 1), K(InitializeCriticalSectionAndSpinCount, 2), K(DeleteCriticalSection, 1), K(PulseEvent, 1), K(SetThreadAffinityMask, 2),
    K(SetThreadPriorityBoost, 2), K(Sleep, 1), K(ExitThread, 1), K(CreateThread, 6), K(GetLastError, 0),
    { "KERNEL32!GetOverlappedResult", NULL, 4 }, K(SetFilePointer, 4), K(GetFileSize, 2), K(GetFullPathNameA, 4),
    K(lstrlenA, 1), K(HeapAlloc, 3), K(HeapCreate, 3), K(HeapDestroy, 1), K(HeapFree, 3), K(lstrcmpiA, 2), K(GetLocaleInfoW, 4),
    K(SetEndOfFile, 1), K(HeapSize, 3), { "KERNEL32!IsBadCodePtr", k_IsBad, 1 }, { "KERNEL32!IsBadReadPtr", k_IsBad, 2 },
    K(SetUnhandledExceptionFilter, 1), K(SetStdHandle, 2), K(VirtualProtect, 4), K(IsValidCodePage, 1), K(IsValidLocale, 2),
    K(EnumSystemLocalesA, 2), K(GetLocaleInfoA, 4), K(GetUserDefaultLCID, 0), K(GetStringTypeW, 4), K(GetStringTypeA, 5),
    K(VirtualQuery, 3), K(FlushFileBuffers, 1), K(GetFileType, 1), K(SetHandleCount, 1), K(GetEnvironmentStringsW, 0),
    { "KERNEL32!FreeEnvironmentStringsW", k_Free1, 1 }, K(GetEnvironmentStrings, 0), { "KERNEL32!FreeEnvironmentStringsA", k_Free1, 1 },
    K(UnhandledExceptionFilter, 1), K(GetStdHandle, 1), K(GetCurrentProcess, 0), K(TerminateProcess, 2), K(LCMapStringW, 6),
    K(MultiByteToWideChar, 6), K(WideCharToMultiByte, 8), K(LCMapStringA, 6), K(GetCPInfo, 2), K(GetOEMCP, 0), K(GetACP, 0),
    K(RaiseException, 4), { "KERNEL32!MapViewOfFile", NULL, 5 }, { "KERNEL32!CreateFileMappingA", NULL, 6 },
    { "KERNEL32!CreateFileW", NULL, 7 }, { "KERNEL32!UnmapViewOfFile", NULL, 1 }, K(ExitProcess, 1), K(HeapReAlloc, 4),
    K(GetStartupInfoA, 1), K(GetCommandLineA, 0), K(GetSystemTimeAsFileTime, 1), K(RtlUnwind, 4), K(GetTickCount, 0),
    K(TlsAlloc, 0), K(SetLastError, 1), K(TlsFree, 1), K(TlsSetValue, 2), K(TlsGetValue, 1), K(VirtualFree, 3), K(VirtualAlloc, 4),
    { "KERNEL32!IsBadWritePtr", k_IsBad, 2 },
    { "ADVAPI32!RegOpenKeyA", a_RegOpenKeyA, 3 }, { "ADVAPI32!RegCloseKey", a_RegCloseKey, 1 }, { "ADVAPI32!RegQueryValueExA", a_RegQueryValueExA, 6 },
    { "GDI32!DeleteObject", u_One, 1 }, { "GDI32!GetStockObject", g_GetStockObject, 1 },
    { "D3D9!Direct3DCreate9", d3d_Direct3DCreate9, 1 }, { "COMCTL32!#17", u_Zero, 0 }, { "DSOUND!#1", ds_DirectSoundCreate, 3 },
    { "USER32!IsIconic", u_Zero, 1 }, { "USER32!GetWindowLongA", u_Zero, 2 }, { "USER32!GetMenu", u_Zero, 1 },
    { "USER32!GetWindowRect", u_Rect, 2 }, { "USER32!SystemParametersInfoA", u_One, 4 }, { "USER32!MessageBoxA", u_MessageBoxA, 4 },
    { "USER32!WaitMessage", u_One, 0 }, { "USER32!PeekMessageA", u_PeekMessageA, 5 }, { "USER32!TranslateMessage", u_One, 1 },
    { "USER32!DispatchMessageA", u_DispatchMessageA, 1 }, { "USER32!InvalidateRect", u_One, 3 }, { "USER32!ShowWindow", u_ShowWindow, 2 },
    { "USER32!LoadIconA", u_LoadRes, 2 }, { "USER32!LoadCursorA", u_LoadRes, 2 }, { "USER32!FindWindowA", u_FindWindowA, 2 },
    { "USER32!SetForegroundWindow", u_One, 1 }, { "USER32!RegisterClassA", u_RegisterClassA, 1 },
    { "USER32!CreateWindowExA", u_CreateWindowExA, 12 }, { "USER32!DefWindowProcA", u_DefWindowProcA, 4 },
    { "USER32!SetCursor", u_Zero, 1 }, { "USER32!PostQuitMessage", u_PostQuitMessage, 1 }, { "USER32!BeginPaint", u_Zero, 2 },
    { "USER32!EndPaint", u_One, 2 }, { "USER32!UpdateWindow", u_One, 1 }, { "USER32!SetRect", u_SetRect, 5 },
    { "USER32!AdjustWindowRectEx", u_One, 4 }, { "USER32!GetSystemMetrics", u_GetSystemMetrics, 1 },
    { "USER32!SetWindowLongA", u_Zero, 3 }, { "USER32!SetWindowPos", u_One, 7 }, { "USER32!DestroyWindow", u_One, 1 },
    { "USER32!GetClientRect", u_Rect, 2 },
    { "DINPUT8!DirectInput8Create", di_DirectInput8Create, 5 },
    { "WINMM!timeKillEvent", w_timeKillEvent, 1 }, { "WINMM!timeSetEvent", w_timeSetEvent, 5 }, { "WINMM!timeGetDevCaps", w_timeGetDevCaps, 2 },
    { "WINMM!timeBeginPeriod", w_timeBeginPeriod, 1 }, { "WINMM!timeEndPeriod", w_timeBeginPeriod, 1 }, { "WINMM!timeGetTime", w_timeGetTime, 0 },
};

const rt_thunk *rt_find_import(const char *name)
{
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if (strcasecmp(table[i].name, name) == 0)
            return &table[i];
    rt_thunk *t = calloc(1, sizeof *t);
    t->name = strdup(name);
    return t;
}

int rt_watchdog_on;
void rt_note_function(CPU *c, uint32_t rva)
{
    rt_thread_ctx *x = ctx_of_cpu(c);
    if (!x) return;
    x->fn_ring[x->fn_calls & 7U] = rva + GUEST_IMAGE_BASE;
    x->fn_calls++;
}
void rt_note_import(CPU *c, const char *name, int stage)
{
    rt_thread_ctx *x = ctx_of_cpu(c);
    if (!x) return;
    if (stage == 1) { x->last_import = name; x->import_calls++; }
    x->stage = stage;
}

/* TMNT_WATCHDOG=1: every 2 s, each guest thread's last API call, call count and suspend count (finds hangs where no log is written). */
extern uint32_t rt_last_guest_call(void);
static void *watchdog_main(void *arg)
{
    (void)arg;
    rt_thread_place_self(0, 15);
    for (;;) {
        rt_sleep_us(2000000);
        rt_log("[watchdog] last indirect call %08x\n", rt_last_guest_call());
        for (int i = 0; i < ctx_count; i++) {
            rt_thread_ctx *x = all_ctx[i];
            uint32_t n = x->fn_calls, w = x->waiting_cs, wid = w ? ld32(w) : 0;
            rt_log("[watchdog] tid %u: %u calls, last %s (stage %d), suspend %d, critical sections %d, waiting cs %08x (owner %u), esp %08x; "
                   "%u functions, last %08x %08x %08x %08x\n",
                   x->tid, x->import_calls, x->last_import ? x->last_import : "-", x->stage, x->suspend, x->cs_held, w,
                   wid && wid <= MAX_CS ? cs_owner[wid - 1] : 0, x == &main_ctx && main_cpu ? main_cpu->esp : x->cpu.esp, n,
                   x->fn_ring[(n - 1) & 7U], x->fn_ring[(n - 2) & 7U], x->fn_ring[(n - 3) & 7U], x->fn_ring[(n - 4) & 7U]);
        }
    }
    return NULL;
}

void rt_win32_init(CPU *__restrict c)
{
    main_cpu = c;
    rt_watchdog_on = getenv("TMNT_WATCHDOG") && atoi(getenv("TMNT_WATCHDOG"));
    main_ctx.tid = 1000;
    main_ctx.teb = GUEST_TEB;
    rt_cond_init(&main_ctx.wake);
    all_ctx[ctx_count++] = &main_ctx;
    if (getenv("TMNT_WATCHDOG") && atoi(getenv("TMNT_WATCHDOG"))) {
        pthread_t w;
        start_host_thread(&w, watchdog_main, NULL);
        pthread_detach(w);
    }
    cmdline_ptr = guest_strdup("TMNT.EXE");
    env_ptr = rt_alloc(4);
    st32(env_ptr, 0);
    envw_ptr = rt_alloc(4);
    st32(envw_ptr, 0);
}
