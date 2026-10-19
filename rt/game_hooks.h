/* Host hooks called at the entry of selected translated functions (recomp/gen_c.py HOOKS). */
#ifndef TMNT_GAME_HOOKS_H
#define TMNT_GAME_HOOKS_H

struct CPU;

/* StartArea(state, ?, area index) at 0x411d93 and SetNextArea(area index) at 0x4047d7: area checkpoints inside a stage. */
void tmnt_hook_start_area(struct CPU *c);
void tmnt_hook_set_next_area(struct CPU *c);

#endif
