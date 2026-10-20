/* TMNT-specific additions: area checkpoints inside a story stage.
 *
 * The game saves only when a whole stage is cleared. Areas are records of 56 bytes at 0x5e0560 ({u16 stage, u16 area, name, ...});
 * StartArea(state, index) at 0x411d93 starts one (story mode and the attract demos alike); SetNextArea at 0x4047d7 tells them
 * apart by its caller: stage select (0x48c92d, 0x4a1f96) or area cleared (0x49475f). The furthest area reached in each stage is
 * kept in TMNTCheckpoint.dat (next to SaveGame.dat); starting that stage from its first area jumps there, and clearing the
 * stage removes it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game_hooks.h"
#include "guest.h"
#include "osdep.h"
#include "runtime.h"

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
