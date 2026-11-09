/* In-game settings: read from the environment (tmnt_env.txt) at start-up, changed in the menu, written back to tmnt_env.txt. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime.h"
#include "settings.h"

#define ENV_FILE "tmnt_env.txt"

/* names: shown in the menu; saved: written to tmnt_env.txt, in the form the rest of the runtime reads. */
typedef struct {
    const char *key, *label;
    int value, count, restart;
    const char *names[11], *saved[11];
} setting;

#define NUMBERS { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10" }
static setting items[SET_COUNT] = {
    [SET_FPS_LOCK] = { "TMNT_FPS_LOCK", "30 FPS lock", 1, 2, 0, { "Off", "On" }, { "0", "1" } },
    [SET_VOLUME] = { "TMNT_VOLUME", "Sound volume", 10, 11, 0, NUMBERS, NUMBERS },
    [SET_START_AREA] = { NULL, "Start area", 0, 0, 0, { "Checkpoint" }, { "" } },   /* list from game_tmnt.c, not saved */
    [SET_PIPELINE] = { "TMNT_PIPELINE", "Pipeline (test)", 0, 2, 0, { "Off", "On" }, { "0", "1" } },
};
static int loaded, start_value[SET_COUNT];

volatile int settings_open;
volatile int settings_cursor;

static void load(void)
{
    if (loaded) return;
    loaded = 1;
    const char *e;
    if ((e = getenv("TMNT_FPS_LOCK")) && *e) items[SET_FPS_LOCK].value = atoi(e) ? 1 : 0;
    if ((e = getenv("TMNT_PIPELINE")) && *e) items[SET_PIPELINE].value = atoi(e) ? 1 : 0;
    if ((e = getenv("TMNT_START_AREA")) && *e) items[SET_START_AREA].value = atoi(e);   /* scripted tests: position in the area list */
    if ((e = getenv("TMNT_VOLUME")) && *e) {
        int v = atoi(e);
        items[SET_VOLUME].value = v < 0 ? 0 : v > 10 ? 10 : v;
    }
    for (int i = 0; i < SET_COUNT; i++) start_value[i] = items[i].value;
}

int settings_value(int id)
{
    load();
    return items[id].value;
}

void settings_set(int id, int value) { load(); items[id].value = value; }
const char *settings_label(int id) { return items[id].label; }
const char *settings_value_text(int id)
{
    load();
    if (id == SET_START_AREA) return items[id].value ? tmnt_area_choice_name(items[id].value - 1) : "Checkpoint";
    return items[id].names[items[id].value];
}
int settings_needs_restart(int id) { load(); return items[id].restart && items[id].value != start_value[id]; }

/* Rewrites tmnt_env.txt with the menu's keys updated or added; every other line stays as it was. */
static void save(void)
{
    char *lines[256];
    int n = 0, found[SET_COUNT] = { 0 };
    FILE *f = fopen(ENV_FILE, "r");
    char buf[512];
    while (f && n < 256 && fgets(buf, sizeof buf, f)) {
        for (int i = 0; i < SET_COUNT; i++) {
            if (!items[i].key) continue;
            size_t k = strlen(items[i].key);
            if (strncmp(buf, items[i].key, k) == 0 && buf[k] == '=') {
                snprintf(buf, sizeof buf, "%s=%s\n", items[i].key, items[i].saved[items[i].value]);
                found[i] = 1;
            }
        }
        lines[n++] = strdup(buf);
    }
    if (f) fclose(f);
    f = fopen(ENV_FILE ".tmp", "w");
    if (!f) { rt_log("[settings] cannot write %s\n", ENV_FILE); goto out; }
    for (int i = 0; i < n; i++) {
        fputs(lines[i], f);
        if (lines[i][0] && lines[i][strlen(lines[i]) - 1] != '\n') fputc('\n', f);
    }
    for (int i = 0; i < SET_COUNT; i++)
        if (items[i].key && !found[i]) fprintf(f, "%s=%s\n", items[i].key, items[i].saved[items[i].value]);
    fclose(f);
    remove(ENV_FILE);
    if (rename(ENV_FILE ".tmp", ENV_FILE) != 0) rt_log("[settings] cannot replace %s\n", ENV_FILE);
    else rt_log("[settings] saved: fps lock %d, volume %d, pipeline %d\n", items[SET_FPS_LOCK].value, items[SET_VOLUME].value,
                items[SET_PIPELINE].value);
out:
    for (int i = 0; i < n; i++) free(lines[i]);
}

int settings_input(uint32_t pressed, uint32_t held)
{
    load();
    if (!settings_open) {
        if ((pressed & SETTINGS_BTN_START) && (held & SETTINGS_BTN_SELECT)) {
            settings_open = 1;
            rt_log("[settings] opened\n");
            return 1;
        }
        return 0;
    }
    int c = settings_cursor;
    if (pressed & SETTINGS_BTN_UP) settings_cursor = (c + SET_VISIBLE - 1) % SET_VISIBLE;
    if (pressed & SETTINGS_BTN_DOWN) settings_cursor = (c + 1) % SET_VISIBLE;
    if (pressed & (SETTINGS_BTN_LEFT | SETTINGS_BTN_RIGHT)) {
        setting *s = &items[c];
        int step = (pressed & SETTINGS_BTN_RIGHT) ? 1 : -1;
        if (c == SET_START_AREA) {   /* Checkpoint, then the reached areas; wraps around */
            int n = 1 + tmnt_area_choice_count();
            s->value = (s->value + step + n) % n;
        } else {
            s->value = s->count == 2 ? !s->value : (s->value + step < 0 ? 0 : s->value + step >= s->count ? s->count - 1 : s->value + step);
        }
    }
    if (pressed & (SETTINGS_BTN_CLOSE | SETTINGS_BTN_START)) {
        settings_open = 0;
        save();
    }
    return 1;
}
