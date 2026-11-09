/* Settings changed in the game (on the Vita: Select + Start) and kept in tmnt_env.txt. */
#ifndef TMNT_SETTINGS_H
#define TMNT_SETTINGS_H

#include <stdint.h>

/* Items from SET_FIRST_DEV on are for development and show only in builds with TMNT_DEV_MENU (not in release VPKs). */
enum { SET_FPS_LOCK, SET_VOLUME, SET_START_AREA, SET_PIPELINE, SET_COUNT, SET_FIRST_DEV = SET_PIPELINE };
#ifdef TMNT_DEV_MENU
#define SET_VISIBLE SET_COUNT
#else
#define SET_VISIBLE SET_FIRST_DEV
#endif

/* Nonzero while the menu is on screen; the game gets no input then. */
extern volatile int settings_open;
extern volatile int settings_cursor;

int settings_value(int id);
void settings_set(int id, int value);
/* game_tmnt.c: the areas "Start area" offers. */
int tmnt_area_choice_count(void);
const char *tmnt_area_choice_name(int k);
/* For the menu: label and current value as text, and whether a change waits for the next start. */
const char *settings_label(int id);
const char *settings_value_text(int id);
int settings_needs_restart(int id);
/* Buttons pressed this frame (SETTINGS_BTN_*), held ones in `held`; returns 1 while the menu takes the input. */
int settings_input(uint32_t pressed, uint32_t held);

enum { SETTINGS_BTN_UP = 1, SETTINGS_BTN_DOWN = 2, SETTINGS_BTN_LEFT = 4, SETTINGS_BTN_RIGHT = 8, SETTINGS_BTN_CLOSE = 16,
       SETTINGS_BTN_START = 32, SETTINGS_BTN_SELECT = 64 };

#endif
