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
