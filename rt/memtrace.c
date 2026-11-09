/* Mac memtrace build (make VARIANT=memtrace, see the Makefile): which guest words the game thread's logic writes and its draw
 * reads, and the other way round, for the draw/logic pipeline. TMNT_MEMTRACE=start_s,length_s: after start_s seconds, one window
 * of length_s collects four bitmaps (logic/draw x read/write); then a second window of the same length counts, per function, the
 * accesses to the words both phases touch. Reports: memtrace.txt and memtrace_fn.txt in the game directory. */
#ifdef GUEST_CHECKED_MEMORY
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest.h"
#include "osdep.h"
#include "runtime.h"

int tmnt_phase(void);

#define WORDS (0x10000000U >> 2)   /* the 256 MB guest window, 4-byte words */
#define FN_SLOTS 8192U

enum { LR, LW, DR, DW, MAPS };
static uint8_t *maps[MAPS], *conflict;
static pthread_t game_thread;
static int have_thread, state;   /* 0 off/waiting, 1 collecting, 2 attributing, 3 done */
static uint64_t t_start, t_len, t_next;
static struct { uint32_t fn, hits[4]; } fns[FN_SLOTS];

void memtrace_note_game_thread(void)
{
    if (have_thread) return;
    game_thread = pthread_self();
    have_thread = 1;
    const char *e = getenv("TMNT_MEMTRACE");
    if (!e) return;
    t_start = (uint64_t)strtoul(e, (char **)&e, 10) * 1000000U;
    t_len = (*e == ',' ? strtoul(e + 1, NULL, 10) : 20) * 1000000U;
    for (int i = 0; i < MAPS; i++) maps[i] = calloc(WORDS / 8, 1);
    conflict = calloc(WORDS / 8, 1);
    t_next = 0;   /* set when an area starts: t_start counts from there */
    rt_log("[memtrace] armed: start %llu s after the next area starts, windows of %llu s\n", (unsigned long long)(t_start / 1000000U),
           (unsigned long long)(t_len / 1000000U));
}

static int bit(const uint8_t *m, uint32_t w) { return (m[w >> 3] >> (w & 7)) & 1; }

static const char *region(uint32_t a)
{
    if (a >= GUEST_IMAGE_BASE && a < GUEST_IMAGE_BASE + 0x55dda0U) return a < 0x580000U ? "image code/rdata" : "image data/bss";
    if (a >= GUEST_HEAP_LO) return "heap";
    return "stack/teb/other";
}

static void report_ranges(FILE *f, const char *title, int a, int b)
{
    uint64_t total = 0;
    uint32_t start = 0, n = 0;
    fprintf(f, "\n== %s ==\n", title);
    for (uint32_t w = 0; w <= WORDS; w++) {
        int on = w < WORDS && bit(maps[a], w) && bit(maps[b], w);
        if (on) {
            conflict[w >> 3] |= (uint8_t)(1U << (w & 7));
            if (!n) start = w;
            n++;
            total++;
        } else if (n) {
            fprintf(f, "%08x-%08x %6u bytes  %s\n", start << 2, (start + n) << 2, n * 4, region(start << 2));
            n = 0;
        }
    }
    fprintf(f, "total %llu words (%llu KB)\n", (unsigned long long)total, (unsigned long long)(total * 4 / 1024));
}

static void finish_window(void)
{
    if (state == 1) {
        FILE *f = fopen("memtrace.txt", "w");
        if (!f) return;
        uint64_t count[MAPS] = { 0 };
        for (int i = 0; i < MAPS; i++)
            for (uint32_t w = 0; w < WORDS / 8; w++) count[i] += (uint64_t)__builtin_popcount(maps[i][w]);
        fprintf(f, "words: logic read %llu, logic write %llu, draw read %llu, draw write %llu\n", (unsigned long long)count[LR],
                (unsigned long long)count[LW], (unsigned long long)count[DR], (unsigned long long)count[DW]);
        report_ranges(f, "logic writes, draw reads (snapshot or keep)", LW, DR);
        report_ranges(f, "draw writes, logic reads (per-thread copies)", DW, LR);
        report_ranges(f, "both write", LW, DW);
        fclose(f);
        rt_log("[memtrace] window done: memtrace.txt written, attributing functions\n");
        state = 2;
    } else if (state == 2) {
        FILE *f = fopen("memtrace_fn.txt", "w");
        if (!f) return;
        static const char *kind[4] = { "logic read", "logic write", "draw read", "draw write" };
        for (int k = 0; k < 4; k++) {
            fprintf(f, "\n== %s of shared words ==\n", kind[k]);
            for (int r = 0; r < 40; r++) {
                uint32_t best = FN_SLOTS;
                for (uint32_t i = 0; i < FN_SLOTS; i++)
                    if (fns[i].fn && fns[i].hits[k] && (best == FN_SLOTS || fns[i].hits[k] > fns[best].hits[k])) best = i;
                if (best == FN_SLOTS) break;
                fprintf(f, "%08x %10u\n", fns[best].fn + GUEST_IMAGE_BASE, fns[best].hits[k]);
                fns[best].hits[k] = 0;
            }
        }
        fclose(f);
        rt_log("[memtrace] memtrace_fn.txt written\n");
        state = 3;
    }
}

void guest_check(uint32_t addr, uint32_t size, int write)
{
    if (!conflict || state == 3 || !pthread_equal(pthread_self(), game_thread)) return;
    int phase = tmnt_phase();
    if (phase != 1 && phase != 2) return;
    extern uint64_t tmnt_area_started_us;
    if (!t_next) {
        if (!tmnt_area_started_us) return;
        t_next = tmnt_area_started_us + t_start;
    }
    static uint32_t calls;
    uint64_t now = (++calls & 0xfffffU) ? 0 : rt_now_us();   /* the clock only every million accesses */
    if (now && now >= t_next) {
        if (state == 0) { state = 1; rt_log("[memtrace] collecting\n"); }
        else finish_window();
        t_next = now + t_len;
        if (state == 3) return;
    }
    if (state == 0) return;
    int k = (phase == 2 ? 2 : 0) + (write ? 1 : 0);
    for (uint32_t w = addr >> 2, end = (addr + size - 1) >> 2; w <= end && w < WORDS; w++) {
        if (state == 1) {
            maps[k][w >> 3] |= (uint8_t)(1U << (w & 7));
        } else if (bit(conflict, w)) {
            uint32_t fn = guest_prof_fn, h = (fn * 2654435761U) >> 19;
            for (uint32_t i = 0; i < FN_SLOTS; i++, h = (h + 1) & (FN_SLOTS - 1))
                if (fns[h].fn == fn || !fns[h].fn) { fns[h].fn = fn; fns[h].hits[k]++; break; }
            break;
        }
    }
}
#endif
