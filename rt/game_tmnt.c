/* TMNT-specific additions: area checkpoints inside a story stage.
 *
 * The game saves only when a whole stage is cleared. Areas are records of 56 bytes at 0x5e0560 ({u16 stage, u16 area, name, ...});
 * StartArea(state, index) at 0x411d93 starts one (story mode and the attract demos alike); SetNextArea at 0x4047d7 tells them
 * apart by its caller: stage select (0x48c92d, 0x4a1f96) or area cleared (0x49475f). The furthest area reached in each stage is
 * kept in TMNTCheckpoint.dat (next to SaveGame.dat); starting that stage from its first area jumps there, and clearing the
 * stage removes it. */
#include <math.h>
#include <stdio.h>
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif
#include <stdlib.h>
#include <string.h>

#include "game_hooks.h"
#include "plat.h"
#include "guest.h"
#include "osdep.h"
#include "runtime.h"
#include "movie.h"
#include "par.h"

#define AREA_TABLE 0x005e0560U
#define AREA_SIZE 56U
#define MAX_AREAS 128U
#define GAME_STATE_PTR 0x0060a438U
#define NEXT_AREA_FIELD 0x13cU
#define CHECKPOINT_FILE "TMNTCheckpoint.dat"
#define MAX_STAGES 16

static int checkpoint[MAX_STAGES];   /* area index to resume, 0 = none */
static int loaded;
static int last_index = -1;

static uint32_t area_stage(uint32_t i) { return ld16(AREA_TABLE + i * AREA_SIZE); }
static uint32_t area_number(uint32_t i) { return ld16(AREA_TABLE + i * AREA_SIZE + 2); }
static const char *area_name(uint32_t i) { return (const char *)G2H(AREA_TABLE + i * AREA_SIZE + 4); }

static void load_checkpoints(void)
{
    loaded = 1;
    FILE *f = fopen(CHECKPOINT_FILE, "r");
    if (!f) return;
    int s, idx;
    while (fscanf(f, "%d %d", &s, &idx) == 2)
        if (s > 0 && s < MAX_STAGES && idx > 0 && (uint32_t)idx < MAX_AREAS) checkpoint[s] = idx;
    fclose(f);
}

static void save_checkpoints(void)
{
    FILE *f = fopen(CHECKPOINT_FILE ".tmp", "w");
    if (!f) return;
    for (int s = 1; s < MAX_STAGES; s++)
        if (checkpoint[s]) fprintf(f, "%d %d\n", s, checkpoint[s]);
    fclose(f);
    rename(CHECKPOINT_FILE ".tmp", CHECKPOINT_FILE);
}

/* Highest area number of a stage in the table. */
/* SetNextArea callers (RVAs of the return address) that classify the next StartArea. */
#define FROM_STAGE_SELECT_A 0x0008c92dU
#define FROM_STAGE_SELECT_B 0x000a1f96U
#define FROM_AREA_CLEARED   0x0009475fU

static uint32_t next_area_origin;

void tmnt_hook_set_next_area(CPU *c)
{
    uint32_t from = ld32(c->esp);
    if (from == FROM_STAGE_SELECT_A || from == FROM_STAGE_SELECT_B || from == FROM_AREA_CLEARED)
        next_area_origin = from;
}

/* StartArea(state, area index, ...) at 0x411d93: the area index is the second argument. */
void tmnt_hook_start_area(CPU *c)
{
    uint32_t origin = next_area_origin;
    next_area_origin = 0;
    uint32_t idx = ld32(c->esp + 8);
    if (idx == 0 || idx >= MAX_AREAS || getenv("TMNT_NO_CHECKPOINTS")) return;
    uint32_t stage = area_stage(idx), area = area_number(idx);
    rt_log("[checkpoint] StartArea(%u: stage %u area %u %s), origin %08x\n", idx, stage, area, area_name(idx), origin);
    if (stage == 0 || stage >= MAX_STAGES || origin == 0) return;
    if (!loaded) load_checkpoints();

    if (origin == FROM_AREA_CLEARED) {
        /* Clearing the stage's last area ends the stage: the next StartArea is another stage or a menu area. */
        if (last_index > 0) {
            uint32_t ls = area_stage((uint32_t)last_index);
            if (ls != stage && ls < MAX_STAGES && checkpoint[ls]) {
                rt_log("[checkpoint] stage %u cleared, checkpoint removed\n", ls);
                checkpoint[ls] = 0;
                save_checkpoints();
            }
        }
        if (area > 1 && (!checkpoint[stage] || area_number((uint32_t)checkpoint[stage]) < area)) {
            checkpoint[stage] = (int)idx;
            save_checkpoints();
            rt_log("[checkpoint] stage %u: area %u (%s) saved\n", stage, area, area_name(idx));
        }
    } else if (area == 1 && checkpoint[stage] && area_stage((uint32_t)checkpoint[stage]) == stage) {
        uint32_t resume = (uint32_t)checkpoint[stage];
        rt_log("[checkpoint] stage %u: resuming at area %u (%s)\n", stage, area_number(resume), area_name(resume));
        st32(c->esp + 8, resume);
        uint32_t gs = ld32(GAME_STATE_PTR);
        if (gs) st32(gs + NEXT_AREA_FIELD, resume);
        idx = resume;
    }
    last_index = (int)idx;
}

/* The title screen's update runs every frame while it is shown; the Vita's Start button is Enter there and Esc (pause) elsewhere. */
static volatile uint64_t title_seen_us;

void tmnt_hook_title(CPU *c)
{
    (void)c;
    title_seen_us = rt_now_us();
}

int tmnt_title_active(void)
{
    uint64_t t = title_seen_us;
    return t && rt_now_us() - t < 200000U;
}

/* CRT _getptd (0x56abe9): GetLastError, TlsGetValue(index at 0x600850) through the pointer at 0x830024, allocation on a miss,
 * SetLastError. When that pointer is still the TlsGetValue import (no FLS) and the slot is set, the result is the slot itself:
 * read it from the TEB the way k_TlsGetValue does and return; otherwise the translated body runs. */
#define GETPTD_TLS_INDEX 0x00600850U
#define GETPTD_GET_FN 0x00830024U
#define IAT_TLSGETVALUE 0x0058b1b4U

int tmnt_fast_getptd(CPU *c)
{
    if (ld32(GETPTD_GET_FN) != ld32(IAT_TLSGETVALUE)) return 0;
    uint32_t idx = ld32(GETPTD_TLS_INDEX);
    if (idx >= 64U) return 0;
    uint32_t ptd = ld32(rt_current_teb(c) + 0xE10U + 4U * idx);
    if (!ptd) return 0;
    c->eax = ptd;
    c->esp += 4U;   /* the function's `ret` */
    return 1;
}

/* 0x4b3354: `call [0x69f838]; sub eax, [0x69f848]; sbb edx, [0x69f84c]; ret`. The pointer holds 0x4b33a9, a stack-framed rdtsc
 * read; the game calls this ~14500 times a frame in fights. Same result straight from the host clock. */
#define TICKS_CLOCK_FN 0x0069f838U
#define TICKS_BASE 0x0069f848U
#define RDTSC_READER 0x004b33a9U

int tmnt_fast_ticks(CPU *c)
{
    if (ld32(TICKS_CLOCK_FN) != RDTSC_READER) return 0;
    uint64_t base = (uint64_t)ld32(TICKS_BASE) | ((uint64_t)ld32(TICKS_BASE + 4U) << 32);
    uint64_t t = guest_rdtsc() - base;
    c->eax = (uint32_t)t;
    c->edx = (uint32_t)(t >> 32);
    c->esp += 4U;   /* the function's `ret` */
    return 1;
}

static volatile uint64_t pad_settings_seen_us;

void tmnt_hook_pad_settings(CPU *c)
{
    (void)c;
    pad_settings_seen_us = rt_now_us();
}

int tmnt_pad_settings_active(void)
{
    uint64_t t = pad_settings_seen_us;
    return t && rt_now_us() - t < 200000U;
}

/* TMNT_PERF=1: time spent in the native replacements (skinning, toon loops, other), logged with the tick timing. */
static uint64_t hle_us[3];
#define HLE_TIME(k, stmt) do { if (perf_on > 0) { uint64_t _t0 = rt_now_us(); stmt; hle_us[k] += rt_now_us() - _t0; } else { stmt; } } while (0)

/* Tick timing at the phase entries (three clock reads per tick). It drives the 30 FPS cadence below; TMNT_PERF=1 also logs it
 * every 2 s: logic and draw time, busy wait and the game's frameskip level. */
#define SKIP_LEVEL 0x0069f554U
#define SKIP_INDEX 0x0069f558U
#define SKIP_TABLE 0x005bd648U
#define SPEED_PERCENT 0x0069f518U

static int perf_on = -1, perf_phase;   /* phase: 1 logic, 2 draw, 3 present + pace */
static uint64_t perf_t, perf_window_t, perf_logic, perf_draw, perf_wait, perf_other;
static uint32_t perf_ticks, perf_drawn;
static int perf_checked;   /* the draw loops over views and checks once per view; count the tick once */

/* 30 FPS cadence: when a 2 s window costs more than 14 ms per tick (logic plus a drawn frame: a level, not a menu), the game's
 * frameskip level is kept at 6 or above, every second tick drawn, instead of the uneven 7-11 of 12 patterns of levels 1-5. Two
 * windows under 8 ms give 60 FPS back (menus). TMNT_30FPS=0 turns it off, TMNT_30FPS=2 keeps it everywhere. */
#define CAP_LEVEL 6U
static int cap_on = -1, cap_heavy, cap_light;

static int perf_begin(uint64_t *now)
{
    if (perf_on < 0) {
        perf_on = getenv("TMNT_PERF") && atoi(getenv("TMNT_PERF"));
        cap_on = getenv("TMNT_30FPS") ? atoi(getenv("TMNT_30FPS")) : 1;
    }
    *now = rt_now_us();
    return 1;
}

static void cap_window(double logic_ms, double draw_ms)
{
    double cost = logic_ms + draw_ms;
    if (cost > 14.0) { cap_heavy = 1; cap_light = 0; }
    else if (cost < 8.0) { if (++cap_light >= 2) cap_heavy = 0; }
    else cap_light = 0;
}

void tmnt_hook_logic(CPU *c)
{
    uint64_t now;
    (void)c;
    if (!perf_begin(&now)) return;
    if (perf_phase == 3) perf_other += now - perf_t;
    if (!perf_window_t) perf_window_t = now;
    if (now - perf_window_t >= 2000000U && perf_ticks) {
        double s = (double)(now - perf_window_t) / 1e6, t = perf_ticks, d = perf_drawn ? perf_drawn : 1;
        cap_window(perf_logic / t / 1000.0, perf_draw / d / 1000.0);
        if (perf_on) rt_log("[perf] %u ticks in %.2f s (%.1f/s), %u drawn (%.1f fps); logic %.1f ms/tick, draw %.1f ms/drawn, wait %.1f ms/tick, "
               "present+other %.1f ms/tick; level %u, speed %.0f%%; native skin %.1f toon %.1f other %.1f ms/tick\n", perf_ticks, s,
               t / s, perf_drawn, perf_drawn / s, perf_logic / t / 1000.0, perf_draw / d / 1000.0, perf_wait / t / 1000.0,
               perf_other / t / 1000.0, ld32(SKIP_LEVEL), ldd(SPEED_PERCENT), hle_us[0] / t / 1000.0, hle_us[1] / t / 1000.0,
               hle_us[2] / t / 1000.0);
        if (perf_on) {
            int arm, gpu, battery;
            plat_power_info(&arm, &gpu, &battery);
            if (arm) rt_log("[power] arm %d gpu %d MHz, battery %d%%\n", arm, gpu, battery);
        }
        memset(hle_us, 0, sizeof hle_us);
        perf_logic = perf_draw = perf_wait = perf_other = 0;
        perf_ticks = perf_drawn = 0;
        perf_window_t = now;
    }
    perf_ticks++;
    perf_checked = 0;
    perf_phase = 1;
    perf_t = now;
}

void tmnt_hook_draw(CPU *c)
{
    uint64_t now;
    (void)c;
    if (!perf_begin(&now) || perf_phase != 1) return;
    perf_logic += now - perf_t;
    perf_phase = 2;
    perf_t = now;
}

void tmnt_hook_present(CPU *c)
{
    uint64_t now;
    (void)c;
    if (!perf_begin(&now) || perf_phase != 2) return;
    perf_draw += now - perf_t;
    perf_phase = 3;
    perf_t = now;
}

/* 0x4af87e is called by the draw (skip this frame?) and by 0x4b000b right after its busy wait. */
void tmnt_hook_skip_check(CPU *c)
{
    uint64_t now;
    (void)c;
    if (!perf_begin(&now)) return;
    if (perf_phase == 2) {
        if (perf_checked++) return;
        if ((cap_on == 2 || (cap_on && cap_heavy)) && ld32(SKIP_LEVEL) < CAP_LEVEL) st32(SKIP_LEVEL, CAP_LEVEL);
        if (!ld32(SKIP_TABLE + 4U * (ld32(SKIP_LEVEL) * 12U + ld32(SKIP_INDEX)))) perf_drawn++;
    } else if (perf_phase == 3) {
        perf_wait += now - perf_t;
        perf_t = now;
    }
}

/* RpSkin generic x87 skinning in native float. 0x4f52f0 (up to 4 bones) and 0x4f5960 (2 bones) take (count, weights, indices,
 * matrices, out_pos, in_pos, out_normals, in_normals, stride); 0x4f5880 (one bone for the whole mesh) takes (count, matrix, out_pos,
 * in_pos, out_normals, in_normals, stride). Matrices are 64 bytes, 3 rows of 4; a first weight >= 1.0 uses bone 0 only, bones 2 and
 * 3 count only when their weight is above zero. Odd cases (unaligned data, normals given on one side only, no vertices) run the
 * translated body. */
void sub_000f52f0(CPU *__restrict c);
void sub_000f5960(CPU *__restrict c);
void sub_000f5880(CPU *__restrict c);

static int hle_check = -1, hle_check_running;

static void skin_xform(const float *m, const float *v, float *r)
{
    r[0] = m[0] * v[0] + m[1] * v[1] + m[2] * v[2] + m[3];
    r[1] = m[4] * v[0] + m[5] * v[1] + m[6] * v[2] + m[7];
    r[2] = m[8] * v[0] + m[9] * v[1] + m[10] * v[2] + m[11];
}

static void skin_rotate(const float *m, const float *v, float *r)
{
    r[0] = m[0] * v[0] + m[1] * v[1] + m[2] * v[2];
    r[1] = m[4] * v[0] + m[5] * v[1] + m[6] * v[2];
    r[2] = m[8] * v[0] + m[9] * v[1] + m[10] * v[2];
}

static void skin_add(float *acc, const float *r, float w)
{
    acc[0] += r[0] * w;
    acc[1] += r[1] * w;
    acc[2] += r[2] * w;
}

typedef struct {
    const char *name;
    void (*body)(CPU *__restrict c);
    int nargs, bones;                 /* bones 0: one matrix for the mesh (0x4f5880) */
    int count, out_pos, in_pos, out_nrm, in_nrm, stride;   /* argument indices */
    uint32_t calls, verts, bad;
    double maxerr;
} skin_kind;

static skin_kind skin4 = { "skin4", sub_000f52f0, 9, 4, 0, 4, 5, 6, 7, 8 };
static skin_kind skin2 = { "skin2", sub_000f5960, 9, 2, 0, 4, 5, 6, 7, 8 };
static skin_kind skin1 = { "skin1", sub_000f5880, 7, 0, 0, 2, 3, 4, 5, 6 };

#ifdef __ARM_NEON
/* NEON skinning: each bone matrix transposed once per call into columns, so a transformed vertex is three multiply-adds on
 * (x, y, z, -) vectors. Same results as the scalar code up to float rounding (the Mac check compares against the x87 body). */
static float32x4_t skin_cols[2][256][4];   /* per thread of par_for */
static uint8_t skin_have[2][256];

static const float32x4_t *skin_bone(int worker, const float *mats, uint32_t i)
{
    if (!skin_have[worker][i]) {
        const float *m = mats + 16U * i;
        float c[4][4] = { { m[0], m[4], m[8], 0 }, { m[1], m[5], m[9], 0 }, { m[2], m[6], m[10], 0 }, { m[3], m[7], m[11], 0 } };
        for (int j = 0; j < 4; j++) skin_cols[worker][i][j] = vld1q_f32(c[j]);
        skin_have[worker][i] = 1;
    }
    return skin_cols[worker][i];
}

static void skin_store(uint8_t *dst, float32x4_t v)
{
    float t[4];
    vst1q_f32(t, v);
    memcpy(dst, t, 12);
}

typedef struct { const skin_kind *k; const uint32_t *a; } skin_job;

static void skin_range(void *ctx, uint32_t from, uint32_t to, int worker)
{
    const skin_kind *k = ((const skin_job *)ctx)->k;
    const uint32_t *a = ((const skin_job *)ctx)->a;
    int32_t count = (int32_t)to;
    uint32_t stride = a[k->stride];
    const float *p = (const float *)G2H(a[k->in_pos]) + 3U * from, *n = a[k->in_nrm] ? (const float *)G2H(a[k->in_nrm]) + 3U * from : NULL;
    uint8_t *op = (uint8_t *)G2H(a[k->out_pos]) + (size_t)from * stride, *on = a[k->out_nrm] ? (uint8_t *)G2H(a[k->out_nrm]) + (size_t)from * stride : NULL;
    const float *mats = (const float *)G2H(k->bones ? a[3] : a[1]);
    const float *w = (const float *)G2H(a[1]);
    const uint32_t *wbits = (const uint32_t *)G2H(a[1]), *idx = (const uint32_t *)G2H(a[2]);
    for (int32_t i = (int32_t)from; i < count; i++, p += 3) {
        float32x4_t pos, nrm = vdupq_n_f32(0.0f);
        if (!k->bones || (int32_t)wbits[4 * i] >= 0x3f800000) {
            const float32x4_t *c = skin_bone(worker, mats, k->bones ? idx[i] & 0xffU : 0);
            pos = vmlaq_n_f32(vmlaq_n_f32(vmlaq_n_f32(c[3], c[0], p[0]), c[1], p[1]), c[2], p[2]);
            if (on) nrm = vmlaq_n_f32(vmlaq_n_f32(vmulq_n_f32(c[0], n[0]), c[1], n[1]), c[2], n[2]);
        } else {
            uint32_t b = idx[i];
            pos = vdupq_n_f32(0.0f);
            for (int j = 0; j < k->bones; j++) {
                if (j >= 2 && (int32_t)wbits[4 * i + j] <= 0) break;
                const float32x4_t *c = skin_bone(worker, mats, (b >> (8 * j)) & 0xffU);
                float wj = w[4 * i + j];
                float32x4_t r = vmlaq_n_f32(vmlaq_n_f32(vmlaq_n_f32(c[3], c[0], p[0]), c[1], p[1]), c[2], p[2]);
                pos = vmlaq_n_f32(pos, r, wj);
                if (n) nrm = vmlaq_n_f32(nrm, vmlaq_n_f32(vmlaq_n_f32(vmulq_n_f32(c[0], n[0]), c[1], n[1]), c[2], n[2]), wj);
            }
        }
        skin_store(op, pos);
        op += stride;
        if (on) { skin_store(on, nrm); on += stride; n += 3; }
    }
}
#endif

static void skin_native(const skin_kind *k, const uint32_t *a)
{
#ifdef __ARM_NEON
    skin_job job = { k, a };
    memset(skin_have, 0, sizeof skin_have);
    par_for(skin_range, &job, a[k->count], 192);
    return;
#endif
    int32_t count = (int32_t)a[k->count];
    uint32_t stride = a[k->stride];
    const float *p = (const float *)G2H(a[k->in_pos]), *n = a[k->in_nrm] ? (const float *)G2H(a[k->in_nrm]) : NULL;
    uint8_t *op = (uint8_t *)G2H(a[k->out_pos]), *on = a[k->out_nrm] ? (uint8_t *)G2H(a[k->out_nrm]) : NULL;
    if (!k->bones) {
        const float *m = (const float *)G2H(a[1]);
        for (int32_t i = 0; i < count; i++, p += 3) {
            float pos[3], nrm[3];
            skin_xform(m, p, pos);
            memcpy(op, pos, 12);
            op += stride;
            if (on) { skin_rotate(m, n, nrm); memcpy(on, nrm, 12); on += stride; n += 3; }
        }
        return;
    }
    const float *w = (const float *)G2H(a[1]);
    const uint32_t *wbits = (const uint32_t *)G2H(a[1]);
    const uint32_t *idx = (const uint32_t *)G2H(a[2]);
    const float *mats = (const float *)G2H(a[3]);
    for (int32_t i = 0; i < count; i++, w += 4, wbits += 4, p += 3) {
        uint32_t b = idx[i];
        float pos[3] = { 0, 0, 0 }, nrm[3] = { 0, 0, 0 }, r[3];
        const float *m = mats + 16U * (b & 0xffU);
        if ((int32_t)wbits[0] >= 0x3f800000) {
            skin_xform(m, p, pos);
            if (on) skin_rotate(m, n, nrm);
        } else {
            for (int j = 0; j < k->bones; j++) {
                if (j >= 2 && (int32_t)wbits[j] <= 0) break;
                m = mats + 16U * ((b >> (8 * j)) & 0xffU);
                skin_xform(m, p, r);
                skin_add(pos, r, w[j]);
                if (n) { skin_rotate(m, n, r); skin_add(nrm, r, w[j]); }
            }
        }
        memcpy(op, pos, 12);
        op += stride;
        if (on) { memcpy(on, nrm, 12); on += stride; n += 3; }
    }
}

/* TMNT_HLE_CHECK=1: the native result is kept, memory restored, the translated body run, and both outputs compared. */
static void skin_compare(skin_kind *k, const uint32_t *a, const uint8_t *np, const uint8_t *nn)
{
    uint32_t stride = a[k->stride];
    for (int32_t i = 0; i < (int32_t)a[k->count]; i++) {
        for (int s = 0; s < (nn ? 2 : 1); s++) {
            const float *x = (const float *)((s ? nn : np) + (size_t)i * stride);
            const float *y = (const float *)G2H(a[s ? k->out_nrm : k->out_pos] + (uint32_t)i * stride);
            for (int j = 0; j < 3; j++) {
                double e = fabs((double)x[j] - (double)y[j]);
                if (e > k->maxerr) k->maxerr = e;
                if (e > 1e-4 + 1e-4 * fabs((double)y[j])) k->bad++;
            }
        }
    }
    k->verts += a[k->count];
    if (++k->calls % 300U == 0)
        rt_log("[hle] %s: %u calls, %u vertices, %u mismatches, max error %g\n", k->name, k->calls, k->verts, k->bad, k->maxerr);
}

static int skin_fast(CPU *c, skin_kind *k)
{
    uint32_t a[9], or = 0;
    if (hle_check_running) return 0;
    for (int i = 0; i < k->nargs; i++) a[i] = ld32(c->esp + 4U + 4U * (uint32_t)i);
    for (int i = 1; i < k->nargs; i++) or |= a[i];
    if ((or & 3U) || !a[k->out_nrm] != !a[k->in_nrm] || (int32_t)a[k->count] <= 0) return 0;
    if (hle_check < 0) hle_check = getenv("TMNT_HLE_CHECK") && atoi(getenv("TMNT_HLE_CHECK"));
    if (!hle_check) {
        HLE_TIME(0, skin_native(k, a));
        c->esp += 4U;   /* the function's `ret` (cdecl: the caller removes the arguments) */
        return 1;
    }
    uint32_t op = a[k->out_pos], on = a[k->out_nrm];
    size_t span = (size_t)(a[k->count] - 1U) * a[k->stride] + 12U;
    uint8_t *save_p = malloc(span), *save_n = on ? malloc(span) : NULL, *nat_p = malloc(span), *nat_n = on ? malloc(span) : NULL;
    memcpy(save_p, G2H(op), span);
    if (on) memcpy(save_n, G2H(on), span);
    skin_native(k, a);
    memcpy(nat_p, G2H(op), span);
    if (on) memcpy(nat_n, G2H(on), span);
    memcpy(G2H(op), save_p, span);
    if (on) memcpy(G2H(on), save_n, span);
    hle_check_running = 1;
    k->body(c);   /* the translated body, `ret` included */
    hle_check_running = 0;
    skin_compare(k, a, nat_p, nat_n);
    free(save_p); free(save_n); free(nat_p); free(nat_n);
    return 1;
}

int tmnt_fast_skin4(CPU *c) { return skin_fast(c, &skin4); }
int tmnt_fast_skin2(CPU *c) { return skin_fast(c, &skin2); }
int tmnt_fast_skin1(CPU *c) { return skin_fast(c, &skin1); }

/* RpToon brute silhouette (0x4fc170): its three per-face/per-vertex loops in native code (recomp/gen_c.py REGIONS). Each one
 * computes in double in the translated code's operation order, so results match it bit for bit, and leaves the registers and
 * stack locals the loop leaves. TMNT_HLE_CHECK=1: the native result goes to a side buffer, the translated loop runs, and the
 * end-of-region hook compares memory, registers and locals. */
#define TOON_TMP_PTR 0x006a3a18U      /* per-vertex scratch, 24 bytes: camera minus vertex xyz, ink xy, ink owner */
#define TOON_FACING_PTR 0x006a3a1cU   /* per-face byte: 1 when the face points at the camera */

typedef struct {
    uint32_t eax, ecx, edx, edi;
    int nloc;
    uint32_t loc_off[4], loc_val[4];
} region_out;

typedef struct {
    int pending;
    region_out want;
    uint32_t dst, size;   /* guest bytes the loop writes, and their native values in `buf` */
    uint8_t *buf;
    uint32_t calls, bad;
} region_check;

static int hle_checking(void)
{
    if (hle_check < 0) hle_check = getenv("TMNT_HLE_CHECK") && atoi(getenv("TMNT_HLE_CHECK"));
    return hle_check;
}

static void region_apply(CPU *c, const region_out *o)
{
    c->eax = o->eax; c->ecx = o->ecx; c->edx = o->edx; c->edi = o->edi;
    for (int i = 0; i < o->nloc; i++) st32(c->esp + o->loc_off[i], o->loc_val[i]);
}

/* Runs `fill` into guest memory (normal) or into the side buffer (check mode, returns 0 so the translated loop runs too). */
static int region_run(CPU *c, region_check *k, uint32_t dst, uint32_t size, void (*fill)(CPU *, uint8_t *, region_out *))
{
    region_out o;
    if (!hle_checking()) {
        HLE_TIME(1, fill(c, (uint8_t *)G2H(dst), &o));
        region_apply(c, &o);
        return 1;
    }
    free(k->buf);
    k->buf = malloc(size ? size : 1U);
    memcpy(k->buf, G2H(dst), size);   /* bytes the loop skips keep their current value */
    fill(c, k->buf, &o);
    k->want = o;
    k->dst = dst;
    k->size = size;
    k->pending = 1;
    return 0;
}

static void region_compare(CPU *c, region_check *k, const char *name)
{
    if (!k->pending) return;
    k->pending = 0;
    int bad = memcmp(k->buf, G2H(k->dst), k->size) != 0 || c->eax != k->want.eax || c->ecx != k->want.ecx ||
              c->edx != k->want.edx || c->edi != k->want.edi;
    for (int i = 0; i < k->want.nloc; i++) bad |= ld32(c->esp + k->want.loc_off[i]) != k->want.loc_val[i];
    if (bad && k->bad++ < 5)
        rt_log("[hle] %s mismatch: eax %08x/%08x ecx %08x/%08x edx %08x/%08x edi %08x/%08x\n", name, c->eax, k->want.eax, c->ecx,
               k->want.ecx, c->edx, k->want.edx, c->edi, k->want.edi);
    if (++k->calls % 3000U == 0) rt_log("[hle] %s: %u checks, %u mismatches\n", name, k->calls, k->bad);
}

static double toon_f(uint32_t a) { float v; memcpy(&v, G2H(a), 4); return v; }
/* toon_f through a function-local copy of the memory base (the hot native loops). */
#define TF(a) toon_fb(mb, (a))
static inline double toon_fb(const uint8_t *mb, uint32_t a) { float v; memcpy(&v, mb + a, 4); return v; }
static void toon_store(uint8_t *p, double v) { float f = (float)v; memcpy(p, &f, 4); }

/* 0x4fc1ef..0x4fc2ac: if the geometry has faces, normal = (v1 - v0) x (v2 - v0) per face into [esi+0x6c] (12 bytes each).
 * b.x and b.y go through float stack slots [esp+0x34]/[esp+0x38], b.z stays on the x87 stack. */
static region_check toon_normals_check;

typedef struct { const uint8_t *mb; uint32_t a, b, c; uint8_t *dst; double cx, cy, cz; } toon_job;

static void toon_normals_range(void *ctx, uint32_t from, uint32_t to, int worker)
{
    const toon_job *j = ctx;
    const uint8_t *mb = j->mb;
    uint32_t faces = j->a + 6U * from, verts = j->b;
    uint8_t *dst = j->dst + 12U * from;
    (void)worker;
    for (uint32_t i = from; i < to; i++, faces += 6U, dst += 12) {
        uint32_t p0 = verts + 12U * ld16(faces), p1 = verts + 12U * ld16(faces + 2U), p2 = verts + 12U * ld16(faces + 4U);
        double ax = TF(p1) - TF(p0), ay = TF(p1 + 4U) - TF(p0 + 4U), az = TF(p1 + 8U) - TF(p0 + 8U);
        float bxs = (float)(TF(p2) - TF(p0)), bys = (float)(TF(p2 + 4U) - TF(p0 + 4U));
        double bz = TF(p2 + 8U) - TF(p0 + 8U), bx = bxs, by = bys;
        toon_store(dst, bz * ay - by * az);
        toon_store(dst + 4, bx * az - bz * ax);
        toon_store(dst + 8, by * ax - bx * ay);
    }
}

static void toon_normals_fill(CPU *c, uint8_t *dst, region_out *o)
{    const uint8_t *mb = guest_membase;   /* local copy: stores cannot alias it, so it stays in a register */

    uint32_t esi = c->esi, n = ld32(esi + 4U), faces = ld32(esi + 8U), verts = ld32(c->ebp + 0xcU), out = ld32(esi + 0x6cU);
    memset(o, 0, sizeof *o);
    if ((int32_t)n <= 0) {
        o->eax = n; o->ecx = out; o->edx = faces; o->edi = c->edi;
        return;
    }
    float bxs = 0, bys = 0;
    uint32_t last = 0, start = n - 1U;   /* the last face leaves the registers and stack slots */
    toon_job j = { mb, faces, verts, 0, dst };
    par_for(toon_normals_range, &j, start, 256);
    faces += 6U * start; dst += 12U * start;
    for (uint32_t i = start; i < n; i++, faces += 6U, dst += 12) {
        uint32_t p0 = verts + 12U * ld16(faces), p1 = verts + 12U * ld16(faces + 2U), p2 = verts + 12U * ld16(faces + 4U);
        double ax = TF(p1) - TF(p0), ay = TF(p1 + 4U) - TF(p0 + 4U), az = TF(p1 + 8U) - TF(p0 + 8U);
        bxs = (float)(TF(p2) - TF(p0));
        bys = (float)(TF(p2 + 4U) - TF(p0 + 4U));
        double bz = TF(p2 + 8U) - TF(p0 + 8U), bx = bxs, by = bys;
        toon_store(dst, bz * ay - by * az);
        toon_store(dst + 4, bx * az - bz * ax);
        toon_store(dst + 8, by * ax - bx * ay);
        last = p2;
    }
    o->eax = 0; o->ecx = last; o->edx = faces; o->edi = verts;
    o->nloc = 4;
    o->loc_off[0] = 0x1cU; o->loc_val[0] = 0;
    o->loc_off[1] = 0x20U; o->loc_val[1] = out + 8U + 12U * n;
    o->loc_off[2] = 0x34U; memcpy(&o->loc_val[2], &bxs, 4);
    o->loc_off[3] = 0x38U; memcpy(&o->loc_val[3], &bys, 4);
}

int tmnt_region_toon_face_normals(CPU *c)
{
    uint32_t n = ld32(c->esi + 4U);
    return region_run(c, &toon_normals_check, ld32(c->esi + 0x6cU), (int32_t)n > 0 ? 12U * n : 0U, toon_normals_fill);
}

void tmnt_region_toon_face_normals_check(CPU *c) { region_compare(c, &toon_normals_check, "toon face normals"); }

/* 0x4fc322..0x4fc380: per vertex, camera position ([esp+0x34..0x3c]) minus vertex into the scratch record, ink owner cleared. */
static region_check toon_cam_check;

static void toon_cam_range(void *ctx, uint32_t from, uint32_t to, int worker)
{
    const toon_job *j = ctx;
    const uint8_t *mb = j->mb;
    uint8_t *dst = j->dst + 24U * from;
    (void)worker;
    for (uint32_t v = from; v < to; v++, dst += 24) {
        uint32_t p = j->b + 12U * v;
        toon_store(dst, j->cx - TF(p));
        toon_store(dst + 4, j->cy - TF(p + 4U));
        toon_store(dst + 8, j->cz - TF(p + 8U));
        memset(dst + 20, 0, 4);
    }
}

static void toon_cam_fill(CPU *c, uint8_t *dst, region_out *o)
{    const uint8_t *mb = guest_membase;   /* local copy: stores cannot alias it, so it stays in a register */

    uint32_t n = ld32(c->esi), verts = ld32(c->ebp + 0xcU);
    double cx = TF(c->esp + 0x34U), cy = TF(c->esp + 0x38U), cz = TF(c->esp + 0x3cU);
    memset(o, 0, sizeof *o);
    if ((int32_t)n <= 0) {
        o->eax = n; o->ecx = c->ecx; o->edx = 0; o->edi = c->edi;
        return;
    }
    toon_job j = { mb, 0, verts, 0, dst, cx, cy, cz };
    par_for(toon_cam_range, &j, n, 256);
    o->eax = 24U * n; o->ecx = verts + 8U + 12U * n; o->edx = n; o->edi = n;
}

int tmnt_region_toon_cam_vectors(CPU *c)
{
    uint32_t n = ld32(c->esi);
    return region_run(c, &toon_cam_check, ld32(TOON_TMP_PTR), (int32_t)n > 0 ? 24U * n : 0U, toon_cam_fill);
}

void tmnt_region_toon_cam_vectors_check(CPU *c) { region_compare(c, &toon_cam_check, "toon camera vectors"); }

/* 0x4fc427..0x4fc49d (no SSE): facing[f] = dot(normal f, scratch vector of the face's first vertex) > 0, the dot rounded to
 * float at [esp+0x28] and tested as an integer. */
static region_check toon_facing_check;

static void toon_facing_range(void *ctx, uint32_t from, uint32_t to, int worker)
{
    const toon_job *j = ctx;
    const uint8_t *mb = j->mb;
    (void)worker;
    for (uint32_t f = from; f < to; f++) {
        uint32_t t = j->c + 24U * ld16(j->a + 6U * f), nm = j->b + 12U * f, bits;
        double d = TF(nm + 4U) * TF(t + 4U) + TF(nm) * TF(t);
        d += TF(nm + 8U) * TF(t + 8U);
        float fd = (float)d;
        memcpy(&bits, &fd, 4);
        j->dst[f] = (int32_t)bits > 0;
    }
}

static void toon_facing_fill(CPU *c, uint8_t *dst, region_out *o)
{    const uint8_t *mb = guest_membase;   /* local copy: stores cannot alias it, so it stays in a register */

    uint32_t esi = c->esi, n = ld32(esi + 4U), faces = ld32(esi + 8U), normals = ld32(esi + 0x6cU), tmp = ld32(TOON_TMP_PTR);
    memset(o, 0, sizeof *o);
    if ((int32_t)n <= 0) {
        o->eax = n; o->ecx = c->ecx; o->edx = 0; o->edi = c->edi;
        return;
    }
    uint32_t bits = 0;
    toon_job j = { mb, faces, normals, tmp, dst };
    par_for(toon_facing_range, &j, n - 1U, 256);   /* the last face leaves the stack slot */
    for (uint32_t f = n - 1U; f < n; f++) {
        uint32_t t = tmp + 24U * ld16(faces + 6U * f), nm = normals + 12U * f;
        double d = TF(nm + 4U) * TF(t + 4U) + TF(nm) * TF(t);
        d += TF(nm + 8U) * TF(t + 8U);
        float fd = (float)d;
        memcpy(&bits, &fd, 4);
        dst[f] = (int32_t)bits > 0;
    }
    o->eax = n; o->ecx = ld32(TOON_FACING_PTR); o->edx = n; o->edi = 12U * n;
    o->nloc = 3;
    o->loc_off[0] = 0x1cU; o->loc_val[0] = 6U * n;
    o->loc_off[1] = 0x20U; o->loc_val[1] = 12U * n;
    o->loc_off[2] = 0x28U; o->loc_val[2] = bits;
}

int tmnt_region_toon_facing(CPU *c)
{
    uint32_t n = ld32(c->esi + 4U);
    return region_run(c, &toon_facing_check, ld32(TOON_FACING_PTR), (int32_t)n > 0 ? n : 0U, toon_facing_fill);
}

void tmnt_region_toon_facing_check(CPU *c) { region_compare(c, &toon_facing_check, "toon facing"); }

/* Leaf functions of the silhouette edge loop in native code, as FASTPATHS. They compute in double in the translated code's
 * operation order (bit-exact) and leave registers, x87 stack and memory as the original does. TMNT_HLE_CHECK=1: the native run
 * is captured, undone, the translated body run, and registers, x87 stack and the written memory compared. */
void sub_001850ef(CPU *__restrict c);
void sub_000b8de0(CPU *__restrict c);
void sub_000fca50(CPU *__restrict c);
void sub_000fd570(CPU *__restrict c);

#define RW_XFORM_POINT 0x005850efU        /* RwV3dTransformPoint, x87 version (the pointer at 0x6a3a28 without SSE) */
#define TOON_XFORM_FN 0x006a3a28U
#define TOON_INK_SCALE 0x006a3a20U
#define TOON_INK_OFFSET 0x006a3a24U
#define TOON_LEN_MIN 0x005c3494U
#define SSE_FLAG 0x006a04ecU
#define TOON_QUAD_COUNT 0x006a3a40U
#define TOON_QUAD_CAP 0x006a3a48U
#define TOON_QUAD_PTR 0x006a3a50U

typedef struct { uint32_t addr, len; } mem_span;
typedef struct { const char *name; uint32_t calls, bad; } fast_stat;

static void fpu_push(CPU *c, double v)
{
    c->st_top = (c->st_top - 1) & 7;
    c->st[c->st_top] = v;
}

static int fast_checked(CPU *c, int (*native)(CPU *), void (*body)(CPU *__restrict), const mem_span *sp, int n, fast_stat *s)
{
    uint8_t *save[4], *got[4];
    CPU before = *c, after;
    for (int i = 0; i < n; i++) { save[i] = malloc(sp[i].len); memcpy(save[i], G2H(sp[i].addr), sp[i].len); }
    int done = native(c);
    if (done) {
        after = *c;
        for (int i = 0; i < n; i++) { got[i] = malloc(sp[i].len); memcpy(got[i], G2H(sp[i].addr), sp[i].len); }
        for (int i = 0; i < n; i++) memcpy(G2H(sp[i].addr), save[i], sp[i].len);
        *c = before;
        hle_check_running = 1;
        body(c);
        hle_check_running = 0;
        /* x87: popped slots keep whatever the translated body computed there (empty registers nobody reads); compare the top
         * and a value the function returns on the stack. */
        int bad = memcmp(c->r, after.r, sizeof c->r) != 0 || c->st_top != after.st_top ||
                  (after.st_top != before.st_top && memcmp(&c->st[c->st_top & 7], &after.st[after.st_top & 7], sizeof(double)) != 0);
        for (int i = 0; i < n; i++) bad |= memcmp(got[i], G2H(sp[i].addr), sp[i].len) != 0;
        if (bad && s->bad++ < 5)
            rt_log("[hle] %s mismatch: eax %08x/%08x ecx %08x/%08x edx %08x/%08x esp %08x/%08x top %d/%d\n", s->name, c->eax, after.eax,
                   c->ecx, after.ecx, c->edx, after.edx, c->esp, after.esp, c->st_top, after.st_top);
        if (++s->calls % 30000U == 0) rt_log("[hle] %s: %u checks, %u mismatches\n", s->name, s->calls, s->bad);
        for (int i = 0; i < n; i++) free(got[i]);
    }
    for (int i = 0; i < n; i++) free(save[i]);
    return done;
}

/* x = ((m0*vx + m4*vy) + m8*vz) + m12, y and z likewise with columns 1 and 2 (RwMatrix right/up/at/pos rows). */
static void rw_xform_point(uint32_t out, uint32_t in, uint32_t m)
{    const uint8_t *mb = guest_membase;   /* local copy: stores cannot alias it, so it stays in a register */

    double vx = TF(in), vy = TF(in + 4U), vz = TF(in + 8U);
    double x = TF(m) * vx, y = TF(m + 4U) * vx, z = vx * TF(m + 8U);
    x += TF(m + 0x10U) * vy; y += TF(m + 0x14U) * vy; z += vy * TF(m + 0x18U);
    x += TF(m + 0x20U) * vz; x += TF(m + 0x30U);
    y += TF(m + 0x24U) * vz; y += TF(m + 0x34U);
    z += vz * TF(m + 0x28U); z += TF(m + 0x38U);
    toon_store(G2H(out), x);
    toon_store(G2H(out + 4U), y);
    toon_store(G2H(out + 8U), z);
}

/* 0x5850ef: (out, in, matrix); saves and restores eax/ebx/edx, returns `in` in eax. */
static int native_xform_point(CPU *c)
{
    rw_xform_point(ld32(c->esp + 4U), ld32(c->esp + 8U), ld32(c->esp + 12U));
    c->eax = ld32(c->esp + 8U);
    c->esp += 4U;
    return 1;
}

static fast_stat stat_xform = { "rw transform point" };

int tmnt_fast_rw_xform_point(CPU *c)
{
    if (hle_check_running) return 0;
    if (!hle_checking()) return native_xform_point(c);
    mem_span sp[1] = { { ld32(c->esp + 4U), 12U } };
    return fast_checked(c, native_xform_point, sub_001850ef, sp, 1, &stat_xform);
}

/* 0x4b8de0: RW's table reciprocal square root of the float argument, returned on the x87 stack and in the argument slot. */
static uint32_t rw_invsqrt_bits(uint32_t bits, uint32_t *ecx, uint32_t *edx)
{
    uint32_t table = ld32(ld32(0x0069fe00U) + ld32(0x0069fdfcU) + 4U), b = bits + 0x800U;
    *edx = (b >> 12) & 0xfffU;
    *ecx = (~b >> 1) & 0x3fc00000U;
    return ld32(table + 4U * *edx) + *ecx;
}

static int native_invsqrt(CPU *c)
{
    uint32_t bits = ld32(c->esp + 4U), r = 0;
    if (bits) {
        r = rw_invsqrt_bits(bits, &c->ecx, &c->edx);
        st32(c->esp + 4U, r);
    }
    c->eax = r;
    float f;
    memcpy(&f, &r, 4);
    fpu_push(c, f);
    c->esp += 4U;
    return 1;
}

static fast_stat stat_invsqrt = { "rw invsqrt" };

int tmnt_fast_rw_invsqrt(CPU *c)
{
    if (hle_check_running) return 0;
    if (!hle_checking()) return native_invsqrt(c);
    mem_span sp[1] = { { c->esp + 4U, 4U } };
    return fast_checked(c, native_invsqrt, sub_000b8de0, sp, 1, &stat_invsqrt);
}

/* 0x4fca50 (rec, out, vertex, normal, matrix): rec = matrix * vertex; the normal's screen x/y (columns 0 and 1); ink width
 * ((rec.z * scale) + offset) * rec.z, over the normal's x/y length (RW table invsqrt) when that is above the minimum; out.xy =
 * rec.xy + normal.xy * width. The original reuses its vertex and matrix argument slots for the length and the width. */
static int ink_vertex_ready(void) { return ld32(TOON_XFORM_FN) == RW_XFORM_POINT && !ld32(SSE_FLAG); }

static void ink_vertex_core(uint32_t rec, uint32_t out, uint32_t v, uint32_t n, uint32_t m, uint32_t *lbits_out, uint32_t *wbits_out,
                            uint32_t *ecx, uint32_t *edx)
{    const uint8_t *mb = guest_membase;   /* local copy: stores cannot alias it, so it stays in a register */

    rw_xform_point(rec, v, m);
    double nx = TF(n), ny = TF(n + 4U), nz = TF(n + 8U);
    float sx = (float)((TF(m + 0x20U) * nz + TF(m + 0x10U) * ny) + nx * TF(m));
    float sy = (float)((TF(m + 0x24U) * nz + TF(m + 0x14U) * ny) + nx * TF(m + 4U));
    double rz = TF(rec + 8U);
    float w = (float)((rz * TF(TOON_INK_SCALE) + TF(TOON_INK_OFFSET)) * rz);
    double len = (double)sy * sy + (double)sx * sx;
    float lenf = (float)len;
    uint32_t lbits;
    memcpy(&lbits, &lenf, 4);
    if (len > TF(TOON_LEN_MIN)) {
        *ecx = lbits;
        uint32_t r = lbits ? rw_invsqrt_bits(lbits, ecx, edx) : 0;
        float rf;
        memcpy(&rf, &r, 4);
        w = (float)((double)rf * w);
    }
    *lbits_out = lbits;
    memcpy(wbits_out, &w, 4);
    toon_store(G2H(out), sx * (double)w + TF(rec));
    toon_store(G2H(out + 4U), sy * (double)w + TF(rec + 4U));
}

static int native_ink_vertex(CPU *c)
{
    uint32_t sp = c->esp, lbits, wbits, ecx = c->ecx, edx = c->edx;
    if (!ink_vertex_ready()) return 0;
    ink_vertex_core(ld32(sp + 4U), ld32(sp + 8U), ld32(sp + 12U), ld32(sp + 16U), ld32(sp + 20U), &lbits, &wbits, &ecx, &edx);
    st32(sp + 12U, lbits);
    st32(sp + 20U, wbits);
    c->eax = ld32(sp + 8U);
    c->ecx = ecx;
    c->edx = edx;
    c->esp += 4U;
    return 1;
}

static fast_stat stat_ink = { "toon ink vertex" };

int tmnt_fast_toon_ink_vertex(CPU *c)
{
    if (hle_check_running) return 0;
    if (!hle_checking()) return native_ink_vertex(c);
    mem_span sp[3] = { { ld32(c->esp + 4U), 12U }, { ld32(c->esp + 8U), 8U }, { c->esp + 4U, 20U } };
    return fast_checked(c, native_ink_vertex, sub_000fca50, sp, 3, &stat_ink);
}

/* 0x4fd570 (a, b, a_ink, b_ink): appends the quad a, b, (a_ink.xy, a.z), (b_ink.xy, b.z) to the ink vertex buffer; a full buffer
 * (flush and relock) runs the translated body. */
static int ink_quad_core(uint32_t a, uint32_t b, uint32_t ai, uint32_t bi, uint32_t *count_out, uint32_t *vb_out)
{
    uint32_t count = ld32(TOON_QUAD_COUNT);
    if (count >= ld32(TOON_QUAD_CAP)) return 0;
    uint32_t vb = ld32(TOON_QUAD_PTR);
    uint32_t q[12] = { ld32(a), ld32(a + 4U), ld32(a + 8U), ld32(b), ld32(b + 4U), ld32(b + 8U),
                       ld32(ai), ld32(ai + 4U), ld32(a + 8U), ld32(bi), ld32(bi + 4U), ld32(b + 8U) };
    for (int i = 0; i < 12; i++) st32(vb + 4U * (uint32_t)i, q[i]);
    st32(TOON_QUAD_PTR, vb + 48U);
    st32(TOON_QUAD_COUNT, count + 4U);
    *count_out = count + 4U;
    *vb_out = vb;
    return 1;
}

static int native_ink_quad(CPU *c)
{
    uint32_t count, vb;
    if (!ink_quad_core(ld32(c->esp + 4U), ld32(c->esp + 8U), ld32(c->esp + 12U), ld32(c->esp + 16U), &count, &vb)) return 0;
    c->eax = count;
    c->ecx = vb + 48U;
    c->edx = vb + 36U;
    c->esp += 4U;
    return 1;
}

static fast_stat stat_quad = { "toon ink quad" };

int tmnt_fast_toon_ink_quad(CPU *c)
{
    if (hle_check_running) return 0;
    if (!hle_checking()) return native_ink_quad(c);
    mem_span sp[2] = { { ld32(TOON_QUAD_PTR), 48U }, { TOON_QUAD_COUNT, 20U } };
    return fast_checked(c, native_ink_quad, sub_000fd570, sp, 2, &stat_quad);
}

/* RenderWare math used by the game logic and the draw, in native code (bit-exact, same checks as above). */
void sub_000b8d10(CPU *__restrict c);
void sub_00185000(CPU *__restrict c);
void sub_001852a0(CPU *__restrict c);

/* 0x4b8d10 RwV3dLength: (x*x + y*y) + z*z rounded to float, square root from RW's table, returned on the x87 stack and in the
 * argument slot; eax/ecx/edx keep the table lookup's values. */
static int native_v3d_length(CPU *c)
{
    uint32_t v = ld32(c->esp + 4U);
    double x = toon_f(v), y = toon_f(v + 4U), z = toon_f(v + 8U);
    float len2 = (float)((x * x + y * y) + z * z);
    uint32_t bits, r = 0;
    memcpy(&bits, &len2, 4);
    if (bits) {
        uint32_t table = ld32(ld32(0x0069fe00U) + ld32(0x0069fdfcU)), b = bits + 0x800U;
        c->ecx = (b >> 12) & 0xfffU;
        c->eax = (b >> 1) & 0x3fc00000U;
        r = ld32(table + 4U * c->ecx) + c->eax;
        c->edx = r;
    } else {
        c->eax = 0;
    }
    st32(c->esp + 4U, r);
    float f;
    memcpy(&f, &r, 4);
    fpu_push(c, f);
    c->esp += 4U;
    return 1;
}

static fast_stat stat_len = { "rw v3d length" };

int tmnt_fast_rw_v3d_length(CPU *c)
{
    if (hle_check_running) return 0;
    if (!hle_checking()) return native_v3d_length(c);
    mem_span sp[1] = { { c->esp + 4U, 4U } };
    return fast_checked(c, native_v3d_length, sub_000b8d10, sp, 1, &stat_len);
}

/* 0x585000 RwV3dTransformPoints (out, in, count, matrix), x87 version: a do-while over count; registers restored, eax = in. */
static int native_xform_points(CPU *c)
{
    uint32_t out = ld32(c->esp + 4U), in = ld32(c->esp + 8U), n = ld32(c->esp + 12U), m = ld32(c->esp + 16U);
    if ((int32_t)n <= 0) return 0;
    for (uint32_t i = 0; i < n; i++, in += 12U, out += 12U) rw_xform_point(out, in, m);
    c->eax = ld32(c->esp + 8U);
    c->esp += 4U;
    return 1;
}

static fast_stat stat_points = { "rw transform points" };

int tmnt_fast_rw_xform_points(CPU *c)
{
    if (hle_check_running) return 0;
    if (!hle_checking()) return native_xform_points(c);
    uint32_t n = ld32(c->esp + 12U);
    mem_span sp[1] = { { ld32(c->esp + 4U), (int32_t)n > 0 ? 12U * n : 4U } };
    return fast_checked(c, native_xform_points, sub_00185000, sp, 1, &stat_points);
}

/* 0x5852a0 RwMatrixMultiply (out, a, b): each row of a times b's 3x3, ((b0*x + b4*y) + b8*z), plus b's position for the position
 * row; registers restored. Overlapping matrices (the x87 code interleaves loads and stores) run the translated body. */
static int native_matrix_multiply(CPU *c)
{
    uint32_t out = ld32(c->esp + 4U), a = ld32(c->esp + 8U), b = ld32(c->esp + 12U);
    if ((out < a + 0x3cU && a < out + 0x3cU) || (out < b + 0x3cU && b < out + 0x3cU)) return 0;
    double bm[16];
    for (int i = 0; i < 16; i++) bm[i] = toon_f(b + 4U * (uint32_t)i);
    for (uint32_t row = 0; row < 4; row++) {
        uint32_t r = a + 0x10U * row, o = out + 0x10U * row;
        double x = toon_f(r), y = toon_f(r + 4U), z = toon_f(r + 8U);
        for (int k = 0; k < 3; k++) {
            double s = (bm[k] * x + bm[4 + k] * y) + bm[8 + k] * z;
            if (row == 3) s += bm[12 + k];
            toon_store(G2H(o + 4U * (uint32_t)k), s);
        }
    }
    c->esp += 4U;
    return 1;
}

static fast_stat stat_matmul = { "rw matrix multiply" };

int tmnt_fast_rw_matrix_multiply(CPU *c)
{
    if (hle_check_running) return 0;
    if (!hle_checking()) return native_matrix_multiply(c);
    mem_span sp[1] = { { ld32(c->esp + 4U), 0x3cU } };
    return fast_checked(c, native_matrix_multiply, sub_001852a0, sp, 1, &stat_matmul);
}

/* 0x4a64aa (out, curve, t): the game's animation curve. Keys of 28 bytes at curve+0x28 ({p.xyz, tangent.xyz, time}), count at
 * curve+0x24; binary search for the last key not after t (NaN times count as after), then per axis the cubic of the x87 code:
 * ((((p1 - p0) * inv - (m0 * K2 + m1) * dt) + ((m1 - m0) * inv * u + m0 * K1) * u) * u) + p0, with dt = t1 - t0, u = t - t0,
 * inv = 1 / dt. Returns 1 in eax; ecx 0 and edx the search's last index, as the original leaves them. */
void sub_000a64aa(CPU *__restrict c);

#define CURVE_K1 0x0058b458U
#define CURVE_K2 0x0058b508U

static int native_anim_curve(CPU *c)
{
    uint32_t out = ld32(c->esp + 4U), curve = ld32(c->esp + 8U), keys = curve + 0x28U, edx = c->edx;
    double t = toon_f(c->esp + 12U);
    int32_t lo = 0, hi = (int32_t)ld32(curve + 0x24U) - 1;
    while (lo < hi) {
        int32_t mid = (lo + hi) / 2;
        if (toon_f(keys + 28U * (uint32_t)mid + 0x18U) < t) { lo = mid + 1; edx = (uint32_t)lo; }
        else { hi = mid; edx = (uint32_t)mid; }
    }
    if (lo > 0) lo--;
    uint32_t a = keys + 28U * (uint32_t)lo, b = a + 28U;
    double dt = toon_f(b + 0x18U) - toon_f(a + 0x18U), u = t - toon_f(a + 0x18U), inv = 1.0 / dt;
    double k1 = toon_f(CURVE_K1), k2 = toon_f(CURVE_K2);
    for (uint32_t k = 0; k < 3; k++) {
        double p0 = toon_f(a + 4U * k), p1 = toon_f(b + 4U * k), m0 = toon_f(a + 12U + 4U * k), m1 = toon_f(b + 12U + 4U * k);
        double cc = (p1 - p0) * inv - (m0 * k2 + m1) * dt;
        double g = (m1 - m0) * inv * u + m0 * k1;
        toon_store(G2H(out + 4U * k), (cc + g * u) * u + p0);
    }
    c->eax = 1;
    c->ecx = 0;
    c->edx = edx;
    c->esp += 4U;
    return 1;
}

static fast_stat stat_curve = { "anim curve" };

int tmnt_fast_anim_curve(CPU *c)
{
    if (hle_check_running) return 0;
    if (!hle_checking()) { int r; HLE_TIME(2, r = native_anim_curve(c)); return r; }
    mem_span sp[1] = { { ld32(c->esp + 4U), 12U } };
    return fast_checked(c, native_anim_curve, sub_000a64aa, sp, 1, &stat_curve);
}

/* 0x4fc50d..0x4fc610: the silhouette edge loop. Edges of 16 bytes at [esi+0x2c] ({v0, v1, f0, f1}, f1 0xffff for an open edge);
 * an edge whose two faces differ in facing gets ink vertices for its ends (once per vertex: the scratch record's owner at +0x14
 * set to the ink at [esp+0x20]) and a quad. A full ink buffer calls the translated 0x4fd570 (flush, draw, relock). Leaves
 * eax = facing buffer, ecx = 16 * edges, edx = edi = edges, the loop counters and the last edge's records at [esp+0x10..0x1c].
 * TMNT_HLE_CHECK=1: a run that needs no flush is captured, undone, the translated loop run, and memory, registers and locals
 * compared at the region's end. */
static void guest_call_cdecl(CPU *c, void (*fn)(CPU *__restrict), uint32_t ret, int n, const uint32_t *args)
{
    for (int i = n - 1; i >= 0; i--) { c->esp -= 4U; st32(c->esp, args[i]); }
    c->esp -= 4U;
    st32(c->esp, ret);
    fn(c);                          /* the callee's `ret` pops the return address */
    c->esp += 4U * (uint32_t)n;     /* cdecl: the caller removes the arguments */
}

static int edge_dry_run, edge_needs_flush;

static void toon_edges_run(CPU *c)
{
    uint32_t esi = c->esi, ebp = c->ebp, sp = c->esp, n = ld32(esi + 0x24U), facing = ld32(TOON_FACING_PTR), ink = ld32(sp + 0x20U);
    uint32_t verts = ld32(ebp + 0xcU), norms = ld32(ebp + 0x10U), m = ld32(ebp + 0x1cU), r0 = 0, r1 = 0, scratch[4];
    for (uint32_t i = ld32(sp + 0x10U); i < n; i++) {
        uint32_t e = ld32(esi + 0x2cU) + 16U * i, f1 = ld16(e + 6U);
        if (f1 == 0xffffU || ld8(facing + ld16(e + 4U)) == ld8(facing + f1)) continue;
        uint32_t v0 = ld16(e), v1 = ld16(e + 2U), tmp = ld32(TOON_TMP_PTR);
        r0 = tmp + 24U * v0;
        r1 = tmp + 24U * v1;
        if (!ld32(r0 + 0x14U)) {
            st32(r0 + 0x14U, ink);
            ink_vertex_core(r0, r0 + 0xcU, verts + 12U * v0, norms + 12U * v0, m, &scratch[0], &scratch[1], &scratch[2], &scratch[3]);
        }
        if (!ld32(r1 + 0x14U)) {
            st32(r1 + 0x14U, ink);
            ink_vertex_core(r1, r1 + 0xcU, verts + 12U * v1, norms + 12U * v1, m, &scratch[0], &scratch[1], &scratch[2], &scratch[3]);
        }
        uint32_t cnt, vb;
        if (!ink_quad_core(r0, r1, r0 + 0xcU, r1 + 0xcU, &cnt, &vb)) {
            if (edge_dry_run) { edge_needs_flush = 1; return; }
            uint32_t args[4] = { r0, r1, r0 + 0xcU, r1 + 0xcU };
            guest_call_cdecl(c, sub_000fd570, 0x004fc5e9U, 4, args);
        }
        st32(sp + 0x18U, r1);
        st32(sp + 0x1cU, r0);
    }
    c->eax = ld32(TOON_FACING_PTR);
    c->ecx = 16U * n;
    c->edx = n;
    c->edi = n;
    st32(sp + 0x10U, n);
    st32(sp + 0x14U, 16U * n);
}

typedef struct { uint32_t addr, len; uint8_t *want; } edge_span;
static edge_span edge_spans[3];
static CPU edge_want;
static uint32_t edge_want_loc[4];
static int edge_pending;
static uint32_t edge_checks, edge_bad, edge_skipped;

int tmnt_region_toon_edges(CPU *c)
{
    if (!ink_vertex_ready()) return 0;
    if (!hle_checking()) { HLE_TIME(1, toon_edges_run(c)); return 1; }
    uint32_t cap = ld32(TOON_QUAD_CAP), cnt = ld32(TOON_QUAD_COUNT);
    edge_span sp[3] = { { ld32(TOON_TMP_PTR), 24U * ld32(c->esi), NULL }, { ld32(TOON_QUAD_PTR), cnt < cap ? 12U * (cap - cnt) : 4U, NULL },
                        { TOON_QUAD_COUNT, 20U, NULL } };
    uint8_t *save[3];
    uint32_t save_loc[4];
    CPU before = *c;
    for (int i = 0; i < 3; i++) { save[i] = malloc(sp[i].len); memcpy(save[i], G2H(sp[i].addr), sp[i].len); }
    for (int i = 0; i < 4; i++) save_loc[i] = ld32(c->esp + 0x10U + 4U * (uint32_t)i);
    edge_dry_run = 1;
    edge_needs_flush = 0;
    toon_edges_run(c);
    edge_dry_run = 0;
    if (!edge_needs_flush) {
        edge_want = *c;
        for (int i = 0; i < 4; i++) edge_want_loc[i] = ld32(c->esp + 0x10U + 4U * (uint32_t)i);
        for (int i = 0; i < 3; i++) {
            free(edge_spans[i].want);
            edge_spans[i] = sp[i];
            edge_spans[i].want = malloc(sp[i].len);
            memcpy(edge_spans[i].want, G2H(sp[i].addr), sp[i].len);
        }
        edge_pending = 1;
    } else {
        edge_skipped++;
    }
    for (int i = 0; i < 3; i++) { memcpy(G2H(sp[i].addr), save[i], sp[i].len); free(save[i]); }
    for (int i = 0; i < 4; i++) st32(before.esp + 0x10U + 4U * (uint32_t)i, save_loc[i]);
    *c = before;
    return 0;
}

void tmnt_region_toon_edges_check(CPU *c)
{
    if (!edge_pending) return;
    edge_pending = 0;
    int bad = c->eax != edge_want.eax || c->ecx != edge_want.ecx || c->edx != edge_want.edx || c->edi != edge_want.edi;
    for (int i = 0; i < 4; i++) bad |= ld32(c->esp + 0x10U + 4U * (uint32_t)i) != edge_want_loc[i];
    for (int i = 0; i < 3; i++) bad |= memcmp(edge_spans[i].want, G2H(edge_spans[i].addr), edge_spans[i].len) != 0;
    if (bad && edge_bad++ < 5)
        rt_log("[hle] toon edges mismatch: eax %08x/%08x ecx %08x/%08x edx %08x/%08x\n", c->eax, edge_want.eax, c->ecx, edge_want.ecx,
               c->edx, edge_want.edx);
    if (++edge_checks % 3000U == 0) rt_log("[hle] toon edges: %u checks, %u mismatches, %u skipped (buffer flush)\n", edge_checks, edge_bad, edge_skipped);
}

/* 0x4f86d0 (toon/skin atomic setup): two strided copies of 12-byte vertices into packed arrays, do-while over a count. */
static region_check toon_copy_pos_check, toon_copy_nrm_check;

/* 0x4f88dd..0x4f8907: eax = [esp+0x28] + edx, count [esp+0x24], stride [esp+0x1c], packed destination edi. */
static void toon_copy_pos_fill(CPU *c, uint8_t *dst, region_out *o)
{
    uint32_t src = ld32(c->esp + 0x28U) + c->edx, n = ld32(c->esp + 0x24U), stride = ld32(c->esp + 0x1cU), d = c->edi;
    for (uint32_t i = 0; i < n; i++, src += stride, dst += 12)
        for (uint32_t k = 0; k < 12U; k += 4U) { uint32_t v = ld32(src + k); memcpy(dst + k, &v, 4); }
    memset(o, 0, sizeof *o);
    o->eax = src; o->ecx = 0; o->edx = d + 12U * (n - 1U); o->edi = d + 12U * n;
    o->nloc = 1;
    o->loc_off[0] = 0x24U; o->loc_val[0] = 0;
}

int tmnt_region_toon_copy_pos(CPU *c)
{
    uint32_t n = ld32(c->esp + 0x24U);
    if ((int32_t)n <= 0) return 0;
    int done = region_run(c, &toon_copy_pos_check, c->edi, 12U * n, toon_copy_pos_fill);
    if (done) c->esi = ld32(c->esp + 0x1cU);   /* the stride, as the loop leaves it */
    return done;
}

void tmnt_region_toon_copy_pos_check(CPU *c) { region_compare(c, &toon_copy_pos_check, "toon copy positions"); }

/* 0x4f894f..0x4f8979: eax = [esp+0x28] + ecx, count edi, stride [esp+0x1c], packed destination [esp+0x14]. */
static void toon_copy_nrm_fill(CPU *c, uint8_t *dst, region_out *o)
{
    uint32_t src = ld32(c->esp + 0x28U) + c->ecx, n = c->edi, stride = ld32(c->esp + 0x1cU), d = ld32(c->esp + 0x14U);
    for (uint32_t i = 0; i < n; i++, src += stride, dst += 12)
        for (uint32_t k = 0; k < 12U; k += 4U) { uint32_t v = ld32(src + k); memcpy(dst + k, &v, 4); }
    memset(o, 0, sizeof *o);
    o->eax = src; o->ecx = d + 12U * (n - 1U); o->edx = d + 12U * n; o->edi = 0;
    o->nloc = 1;
    o->loc_off[0] = 0x14U; o->loc_val[0] = d + 12U * n;
}

int tmnt_region_toon_copy_nrm(CPU *c)
{
    uint32_t n = c->edi;
    if ((int32_t)n <= 0) return 0;
    int done = region_run(c, &toon_copy_nrm_check, ld32(c->esp + 0x14U), 12U * n, toon_copy_nrm_fill);
    if (done) c->esi = ld32(c->esp + 0x1cU);
    return done;
}

void tmnt_region_toon_copy_nrm_check(CPU *c) { region_compare(c, &toon_copy_nrm_check, "toon copy normals"); }

/* 0x4ef9f0 (frame, hierarchy): RpHAnim's attach callback, recursive through RwFrameForAllChildren (0x4bd4b0, children at +0x98,
 * siblings at +0x9c): every hierarchy node (16 bytes at [hier+0x10], count [hier+4]) whose id equals the frame's HAnim id
 * (plugin offset at 0x838100) gets the frame at +0xc; then RwFrameUpdateObjects (0x4bd230) and the children. The game runs
 * it for every character every tick. Returns the frame; ecx/edx are what the last RwFrameUpdateObjects left. */
void sub_000ef9f0(CPU *__restrict c);
#define HANIM_ID_OFFSET 0x00838100U
#define RW_ENGINE_PTR 0x0069fdfcU

/* 0x4bd230 RwFrameUpdateObjects: the root (+0xa0) joins the engine's dirty list (head at engine+0xbc, links at root+8/+0xc)
 * unless already flagged, root flags |= 3, frame flags |= 0xc (byte +3). */
static void rw_frame_update_objects(uint32_t f, uint32_t *ecx, uint32_t *edx)
{
    uint32_t root = ld32(f + 0xa0U);
    uint8_t rf = ld8(root + 3U);
    if (!(rf & 3U)) {
        uint32_t eng = ld32(RW_ENGINE_PTR);
        st32(root + 8U, ld32(eng + 0xbcU));
        st32(root + 0xcU, ld32(RW_ENGINE_PTR) + 0xbcU);
        st32(ld32(ld32(RW_ENGINE_PTR) + 0xbcU) + 4U, ld32(f + 0xa0U) + 8U);
        st32(ld32(RW_ENGINE_PTR) + 0xbcU, ld32(f + 0xa0U) + 8U);
    }
    st8(ld32(f + 0xa0U) + 3U, (uint8_t)(rf | 3U));
    uint8_t ff = (uint8_t)(ld8(f + 3U) | 0xcU);
    st8(f + 3U, ff);
    *ecx = ff;
    *edx = ld32(f + 0xa0U);
}

static void hanim_attach(uint32_t f, uint32_t hier, uint32_t *ecx, uint32_t *edx)
{
    uint32_t id = ld32(ld32(HANIM_ID_OFFSET) + f);
    for (int32_t i = 0; i < (int32_t)ld32(hier + 4U); i++) {
        uint32_t node = ld32(hier + 0x10U) + 16U * (uint32_t)i;
        if (ld32(ld32(HANIM_ID_OFFSET) + f) == ld32(node)) st32(node + 0xcU, f);
    }
    (void)id;
    rw_frame_update_objects(f, ecx, edx);
    for (uint32_t ch = ld32(f + 0x98U); ch; ) {
        uint32_t next = ld32(ch + 0x9cU);
        hanim_attach(ch, hier, ecx, edx);
        ch = next;
    }
}

static int native_hanim_attach(CPU *c)
{
    uint32_t f = ld32(c->esp + 4U);
    hanim_attach(f, ld32(c->esp + 8U), &c->ecx, &c->edx);
    c->eax = f;
    c->esp += 4U;
    return 1;
}

/* Check: every frame of the subtree (0xb0 bytes), each root reached, the node array and the engine's dirty-list head. */
static fast_stat stat_attach = { "hanim attach" };

static void hanim_collect(uint32_t f, uint32_t *list, int *n, int max)
{
    if (*n < max) list[(*n)++] = f;
    if (*n < max) list[(*n)++] = ld32(f + 0xa0U);
    for (uint32_t ch = ld32(f + 0x98U); ch; ch = ld32(ch + 0x9cU)) hanim_collect(ch, list, n, max);
}

int tmnt_fast_hanim_attach(CPU *c)
{
    if (hle_check_running) return 0;
    if (!hle_checking()) { int r; HLE_TIME(2, r = native_hanim_attach(c)); return r; }
    uint32_t list[512], hier = ld32(c->esp + 8U);
    int n = 0;
    hanim_collect(ld32(c->esp + 4U), list, &n, 510);
    list[n++] = ld32(RW_ENGINE_PTR) + 0xbcU - 0x10U;   /* the dirty-list head, in a 0xb0 window */
    uint32_t nodes = ld32(hier + 0x10U), nn = 16U * ld32(hier + 4U);
    uint8_t *save = malloc((size_t)n * 0xb0U + nn + 4U), *got = malloc((size_t)n * 0xb0U + nn + 4U);
    for (int i = 0; i < n; i++) memcpy(save + 0xb0 * i, G2H(list[i]), 0xb0);
    memcpy(save + 0xb0 * n, G2H(nodes), nn);
    CPU before = *c, after;
    native_hanim_attach(c);
    after = *c;
    for (int i = 0; i < n; i++) memcpy(got + 0xb0 * i, G2H(list[i]), 0xb0);
    memcpy(got + 0xb0 * n, G2H(nodes), nn);
    for (int i = n - 1; i >= 0; i--) memcpy(G2H(list[i]), save + 0xb0 * i, 0xb0);
    memcpy(G2H(nodes), save + 0xb0 * n, nn);
    *c = before;
    hle_check_running = 1;
    sub_000ef9f0(c);
    hle_check_running = 0;
    int bad = memcmp(c->r, after.r, sizeof c->r) != 0;
    for (int i = 0; i < n; i++) bad |= memcmp(got + 0xb0 * i, G2H(list[i]), 0xb0) != 0;
    bad |= memcmp(got + 0xb0 * n, G2H(nodes), nn) != 0;
    if (bad && stat_attach.bad++ < 5)
        rt_log("[hle] hanim attach mismatch: eax %08x/%08x ecx %08x/%08x edx %08x/%08x\n", c->eax, after.eax, c->ecx, after.ecx, c->edx, after.edx);
    if (++stat_attach.calls % 1000U == 0) rt_log("[hle] hanim attach: %u checks, %u mismatches\n", stat_attach.calls, stat_attach.bad);
    free(save); free(got);
    return 1;
}

/* 0x518cd0 (sphere {centre, radius}, v0, v1, v2, out normal): sphere/triangle test. Natively only its early rejection: per axis,
 * when the three vertex offsets (rounded to float) lie on one side and all beyond the radius, it returns 0, having written the
 * axis' three offsets minus (or plus) the radius into the sphere, v0 and v1 argument slots; ecx holds their combined bits.
 * Anything else runs the translated body from the start. */
void sub_00118cd0(CPU *__restrict c);

static uint32_t f2bits(double v) { float f = (float)v; uint32_t b; memcpy(&b, &f, 4); return b; }

static int native_sphere_tri_reject(CPU *c)
{
    uint32_t sp = c->esp, s = ld32(sp + 4U), v[3] = { ld32(sp + 8U), ld32(sp + 12U), ld32(sp + 16U) };
    float d[3][3];
    for (int i = 0; i < 3; i++)
        for (uint32_t a = 0; a < 3; a++) d[i][a] = (float)(toon_f(v[i] + 4U * a) - toon_f(s + 4U * a));
    double r = toon_f(s + 0xcU);
    for (int a = 0; a < 3; a++) {
        uint32_t b[3], t[3];
        int32_t neg = 0;
        for (int i = 0; i < 3; i++) { memcpy(&b[i], &d[i][a], 4); neg += (int32_t)b[i] >> 31; }
        uint32_t ecx;
        int reject;
        if (neg == 0) {
            for (int i = 0; i < 3; i++) t[i] = f2bits((double)d[i][a] - r);
            ecx = t[2] | t[1] | t[0];
            reject = (int32_t)ecx >= 0;
        } else if (neg == -3) {
            for (int i = 0; i < 3; i++) t[i] = f2bits(r + (double)d[i][a]);
            ecx = t[2] & t[1];
            reject = (int32_t)(ecx & t[0]) < 0;
        } else {
            continue;
        }
        if (!reject) continue;
        st32(sp + 0xcU, t[0]);
        st32(sp + 4U, t[1]);
        st32(sp + 8U, t[2]);
        c->eax = 0;
        c->ecx = ecx;
        c->edx = v[1];
        c->esp += 4U;
        return 1;
    }
    return 0;
}

static fast_stat stat_sphere = { "sphere/triangle reject" };

int tmnt_fast_sphere_tri(CPU *c)
{
    if (hle_check_running) return 0;
    if (!hle_checking()) return native_sphere_tri_reject(c);
    mem_span sp[1] = { { c->esp + 4U, 12U } };
    return fast_checked(c, native_sphere_tri_reject, sub_00118cd0, sp, 1, &stat_sphere);
}

/* Game pad page (0x4b1a0e loop): the call that draws one "BUTTON n" label (0x4b1a4e, Rt2d text through 0x4a72cb, arguments already
 * pushed). For n 1..8 the draw only records its screen box (d3d9_gl.c ovl_capture_slot) and the overlay writes the Vita button
 * there; other labels draw as usual. */
void sub_000a72cb(CPU *__restrict c);
extern int ovl_capture_slot;

int tmnt_region_pad_label(CPU *c)
{
    char s[16];
    uint32_t str = ld32(c->esp);
    for (int i = 0; i < 15; i++) s[i] = (char)ld8(str + (uint32_t)i);
    s[15] = 0;
    int n = 0;
    if (sscanf(s, "BUTTON %d", &n) != 1 || n < 1 || n > 8) return 0;
    ovl_capture_slot = n;
    guest_call_cdecl(c, sub_000a72cb, 0x004b1a53U, 0, NULL);
    ovl_capture_slot = -1;
    return 1;
}

void tmnt_region_pad_label_check(CPU *c) { (void)c; }

/* 0x5593b0 Sofdec 4:2:0 YUV -> ARGB8888 (planes, out, tables). The game picks an SSE (0x558910) or MMX (0x558e20) version and has
   no plain one, while we report neither, so movie frames stayed black. Same lookup tables and arithmetic as the MMX version:
   per channel (Y[y] + U[u] + V[v]) as 16-bit words, arithmetic shift by 6, saturated to a byte; two rows per chroma row. */
static inline void sfd_pixel(uint8_t *o, const int16_t *base, const int16_t *ty)
{
    for (int k = 0; k < 4; k++) {
        int v = (int16_t)(base[k] + ty[k]) >> 6;
        o[k] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
}

int tmnt_fast_sfd_yuv_argb(CPU *c)
{
    uint32_t pl = ld32(c->esp + 4U), od = ld32(c->esp + 8U), tab = ld32(c->esp + 12U);
    uint32_t pitch = ld32(od + 0xcU);
    /* The original stores the row step of the output before converting. */
    st32(od + 0x10U, pitch);
    st32(od + 0x14U, pitch * 2U);
    const uint8_t *Y = G2H(ld32(pl)), *U = G2H(ld32(pl + 4U)), *V = G2H(ld32(pl + 8U));
    uint32_t ypitch = ld32(pl + 0xcU), upitch = ld32(pl + 0x10U), vpitch = ld32(pl + 0x14U);
    uint8_t *out = G2H(ld32(od));
    int w2 = (int32_t)ld32(od + 4U) / 2, h2 = (int32_t)ld32(od + 8U) / 2;
    /* The picture comes from pl_mpeg when the movie is known (rt/movie.c); Sofdec's own decode has artifacts. */
    if (movie_frame_rgba(out, pitch, w2 * 2, h2 * 2)) {
        movie_rgba_mem = ld32(od);
        c->eax = 0;
        c->esp += 4U;
        return 1;
    }
    const int16_t *T = (const int16_t *)G2H(tab);
    for (int j = 0; j < h2; j++) {
        const uint8_t *y0 = Y + (size_t)(2 * j) * ypitch, *y1 = y0 + ypitch, *u = U + (size_t)j * upitch, *v = V + (size_t)j * vpitch;
        uint8_t *o0 = out + (size_t)(2 * j) * pitch, *o1 = o0 + pitch;
        for (int i = 0; i < w2; i++) {
            const int16_t *tu = T + 0x400 + u[i] * 4, *tv = T + 0x800 + v[i] * 4;
            int16_t base[4];
            for (int k = 0; k < 4; k++) base[k] = (int16_t)(tu[k] + tv[k]);
            sfd_pixel(o0 + 8 * i, base, T + y0[2 * i] * 4);
            sfd_pixel(o0 + 8 * i + 4, base, T + y0[2 * i + 1] * 4);
            sfd_pixel(o1 + 8 * i, base, T + y1[2 * i] * 4);
            sfd_pixel(o1 + 8 * i + 4, base, T + y1[2 * i + 1] * 4);
        }
    }
    c->eax = 0;
    c->esp += 4U;
    return 1;
}

/* 0x53ed10 mwPlyFxCnvFrmARGB8888(handle, frame info, out): the frame number Sofdec shows (+0x20), for rt/movie.c. */
void tmnt_hook_sfd_frame(CPU *c)
{
    uint32_t fi = ld32(c->esp + 8U);
    if (fi) movie_set_frame((int32_t)ld32(fi + 0x20U));
}

/* Sofdec IDCT / motion compensation (cdecl, results only in its own frame buffers): skipped while pl_mpeg draws the movie. */
int tmnt_fast_sfd_skip(CPU *c)
{
    c->esp += 4U;
    return 1;
}

