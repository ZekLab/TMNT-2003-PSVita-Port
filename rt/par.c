/* Chunks are claimed one at a time, so the caller never waits for a helper that has not woken: it just takes the rest. */
#include "par.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "osdep.h"
#include "plat.h"
#include "runtime.h"

#define PAR_CHUNKS 8U
#define PAR_SPIN_US 1000U   /* jobs come in bursts while a frame's characters are drawn */

/* state = generation << 8 | next chunk; a claim is a compare-and-swap, so it fails if the job changed since it was read. */
static uint32_t job_state = PAR_CHUNKS, job_done;
static par_fn *job_fn;
static void *job_ctx;
static uint32_t job_n;
static int par_on, par_busy;

static int claim_and_run(int worker)
{
    uint32_t s = __atomic_load_n(&job_state, __ATOMIC_ACQUIRE);
    if ((s & 0xFFU) >= PAR_CHUNKS) return 0;
    par_fn *fn = job_fn;
    void *ctx = job_ctx;
    uint32_t n = job_n;
    if (!__atomic_compare_exchange_n(&job_state, &s, s + 1U, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 1;
    uint32_t chunk = s & 0xFFU;
    fn(ctx, (uint32_t)((uint64_t)n * chunk / PAR_CHUNKS), (uint32_t)((uint64_t)n * (chunk + 1U) / PAR_CHUNKS), worker);
    __atomic_add_fetch(&job_done, 1U, __ATOMIC_ACQ_REL);
    return 1;
}

static void *par_helper(void *arg)
{
    int worker = (int)(intptr_t)arg;
    rt_thread_place_self(0, -1);   /* below the game's normal threads: it only spends idle time */
    plat_thread_label(worker == 1 ? "par" : "par 2");
    uint64_t last = plat_ticks_us();
    for (;;) {
        if (claim_and_run(worker)) { last = plat_ticks_us(); continue; }
        if (plat_ticks_us() - last > PAR_SPIN_US) usleep(200);
    }
    return NULL;
}

void par_init(void)
{
    const char *e = getenv("TMNT_PAR");
    pthread_t t;
    /* TMNT_PAR=0 off, =1 one helper, default PAR_HELPERS (one per free core). */
    int helpers = e ? atoi(e) : PAR_HELPERS;
    if (helpers > PAR_HELPERS) helpers = PAR_HELPERS;
    int started = 0;
    for (int w = 1; w <= helpers; w++) {
        if (pthread_create(&t, NULL, par_helper, (void *)(intptr_t)w)) break;
        pthread_detach(t);
        started++;
    }
    par_on = started > 0;
    rt_log("[par] %d helper thread%s\n", started, started == 1 ? "" : "s");
}

void par_for(par_fn *fn, void *ctx, uint32_t n, uint32_t min_n)
{
    if (!par_on || n < min_n) { fn(ctx, 0, n, 0); return; }
    /* One job at a time: the game thread and the render thread may both ask; the one that finds it taken works alone. */
    if (__sync_lock_test_and_set(&par_busy, 1)) { fn(ctx, 0, n, 0); return; }
    uint32_t s = __atomic_load_n(&job_state, __ATOMIC_RELAXED);
    job_fn = fn; job_ctx = ctx; job_n = n;
    __atomic_store_n(&job_done, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&job_state, (((s >> 8) + 1U) << 8), __ATOMIC_RELEASE);
    while (claim_and_run(0)) { }
    while (__atomic_load_n(&job_done, __ATOMIC_ACQUIRE) < PAR_CHUNKS) { }
    __sync_lock_release(&par_busy);
}
