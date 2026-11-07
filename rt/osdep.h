/* Host primitives that differ between the Mac and the Vita: waitable lock + condition, sleeping, monotonic time.
 * On the Vita, VitaSDK's pthread condition variables lose wake-ups (seen in the Rogue Squadron port), so the condition
 * used by the Win32 events, mutexes, thread joins and suspensions is a kernel mutex + kernel condition there. */
#ifndef TMNT_OSDEP_H
#define TMNT_OSDEP_H

#include <stdint.h>

/* Registers the calling thread for the [cpu] line (plat_vita.c; nothing on the Mac), with a name when given. */
void plat_thread_label(const char *name);

#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>

typedef struct { SceUID mutex, cond; } rt_cond;

static inline void rt_cond_init(rt_cond *c)
{
    c->mutex = sceKernelCreateMutex("rt_cond_m", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);
    c->cond = sceKernelCreateCond("rt_cond_c", 0, c->mutex, NULL);
}
static inline void rt_cond_lock(rt_cond *c) { sceKernelLockMutex(c->mutex, 1, NULL); }
static inline void rt_cond_unlock(rt_cond *c) { sceKernelUnlockMutex(c->mutex, 1); }
static inline void rt_cond_broadcast(rt_cond *c) { sceKernelSignalCondAll(c->cond); }
/* Waits with the lock held; ms == 0xFFFFFFFF waits forever. Returns 0 on timeout. */
static inline int rt_cond_wait_ms(rt_cond *c, uint32_t ms)
{
    if (ms == 0xFFFFFFFFU)
        return sceKernelWaitCond(c->cond, NULL) >= 0;
    SceUInt us = ms * 1000U;
    return sceKernelWaitCond(c->cond, &us) >= 0;
}
static inline void rt_sleep_us(uint64_t us) { sceKernelDelayThread((SceUInt)us); }
static inline void rt_yield(void) { sceKernelDelayThread(0); }
static inline uint64_t rt_now_us(void) { return sceKernelGetProcessTimeWide(); }

/* Thread placement. The Vita does not share a core between threads of equal priority: a guest thread spinning on a flag
 * starves every other thread on its core (seen with CRI ADX: the main thread polls while the timer that feeds it never runs).
 * The game's main thread gets core 0 alone; every other thread shares cores 1-2. Win32 priorities map onto Vita ones
 * (64 highest .. 191): normal 160, 5 per step, time critical 130, idle 190. */
static inline int rt_thread_os_id(void) { return sceKernelGetThreadId(); }
static inline void rt_thread_set_priority(int os_id, int win_prio)
{
    sceKernelChangeThreadPriority(os_id, win_prio >= 15 ? 130 : win_prio <= -15 ? 190 : 160 - 5 * win_prio);
}
static inline void rt_thread_place_self(int main_core, int win_prio)
{
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), main_core ? 0x00010000 : 0x00060000);  /* user core 0 / cores 1-2 */
    rt_thread_set_priority(sceKernelGetThreadId(), win_prio);
    plat_thread_label(NULL);
}

#else
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>

typedef struct { pthread_mutex_t mutex; pthread_cond_t cond; } rt_cond;

static inline void rt_cond_init(rt_cond *c)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&c->mutex, &a);
    pthread_cond_init(&c->cond, NULL);
}
static inline void rt_cond_lock(rt_cond *c) { pthread_mutex_lock(&c->mutex); }
static inline void rt_cond_unlock(rt_cond *c) { pthread_mutex_unlock(&c->mutex); }
static inline void rt_cond_broadcast(rt_cond *c) { pthread_cond_broadcast(&c->cond); }
static inline int rt_cond_wait_ms(rt_cond *c, uint32_t ms)
{
    if (ms == 0xFFFFFFFFU)
        return pthread_cond_wait(&c->cond, &c->mutex) == 0;
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    uint64_t ns = (uint64_t)dl.tv_nsec + (uint64_t)ms * 1000000ULL;
    dl.tv_sec += (time_t)(ns / 1000000000ULL);
    dl.tv_nsec = (long)(ns % 1000000000ULL);
    return pthread_cond_timedwait(&c->cond, &c->mutex, &dl) != ETIMEDOUT;
}
static inline void rt_sleep_us(uint64_t us) { usleep((useconds_t)us); }
static inline void rt_yield(void) { sched_yield(); }
static inline uint64_t rt_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000U;
}
/* The Mac's scheduler time-slices threads: placement and priorities are left to it. */
static inline int rt_thread_os_id(void) { return 0; }
static inline void rt_thread_set_priority(int os_id, int win_prio) { (void)os_id; (void)win_prio; }
static inline void rt_thread_place_self(int main_core, int win_prio) { (void)main_core; (void)win_prio; }
#endif

#endif
