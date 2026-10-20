/* Host hooks called at the entry of selected translated functions (recomp/gen_c.py HOOKS). */
#ifndef TMNT_GAME_HOOKS_H
#define TMNT_GAME_HOOKS_H

struct CPU;

/* StartArea(state, ?, area index) at 0x411d93 and SetNextArea(area index) at 0x4047d7: area checkpoints inside a stage. */
void tmnt_hook_start_area(struct CPU *c);
void tmnt_hook_set_next_area(struct CPU *c);

/* Title screen update (0x498466, the "PRESS START" screen): tmnt_title_active() is true while the game runs it. */
void tmnt_hook_title(struct CPU *c);
int tmnt_title_active(void);

/* CRT _getptd (0x56abe9) fast path: the thread's data block straight from the emulated TLS slot when it exists. */
int tmnt_fast_getptd(struct CPU *c);
/* Elapsed-ticks helper (0x4b3354): rdtsc minus the base at 0x69f848, when its clock pointer is the rdtsc reader. */
int tmnt_fast_ticks(struct CPU *c);

#endif
