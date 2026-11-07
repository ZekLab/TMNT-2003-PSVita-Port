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

/* Gamepad settings page (Options > Controller settings > Game pad, drawn by 0x49e126): the Vita legend overlay is shown while
 * tmnt_pad_settings_active() is true. */
void tmnt_hook_pad_settings(struct CPU *c);
int tmnt_pad_settings_active(void);

/* TMNT_PERF=1 timing: entries of logic (0x4aca5f), draw (0x4ac384), present + pace (0x4b000b) and the skip check (0x4af87e). */
void tmnt_hook_logic(struct CPU *c);
void tmnt_hook_draw(struct CPU *c);
void tmnt_hook_present(struct CPU *c);
void tmnt_hook_skip_check(struct CPU *c);

/* CRT _getptd (0x56abe9) fast path: the thread's data block straight from the emulated TLS slot when it exists. */
int tmnt_fast_getptd(struct CPU *c);
/* RpSkin skinning in native float: 4 bones (0x4f52f0), 2 bones (0x4f5960), one bone per mesh (0x4f5880); TMNT_HLE_CHECK=1 also
 * runs the translated body and compares. */
int tmnt_fast_skin4(struct CPU *c);
int tmnt_fast_skin2(struct CPU *c);
int tmnt_fast_skin1(struct CPU *c);
/* RpToon brute silhouette (0x4fc170) loops in native code (recomp/gen_c.py REGIONS); *_check runs at each region's end. */
int tmnt_region_toon_face_normals(struct CPU *c);
void tmnt_region_toon_face_normals_check(struct CPU *c);
int tmnt_region_toon_cam_vectors(struct CPU *c);
void tmnt_region_toon_cam_vectors_check(struct CPU *c);
int tmnt_region_toon_facing(struct CPU *c);
void tmnt_region_toon_facing_check(struct CPU *c);
int tmnt_region_toon_edges(struct CPU *c);
void tmnt_region_toon_edges_check(struct CPU *c);
int tmnt_region_toon_copy_pos(struct CPU *c);
void tmnt_region_toon_copy_pos_check(struct CPU *c);
int tmnt_region_toon_copy_nrm(struct CPU *c);
void tmnt_region_toon_copy_nrm_check(struct CPU *c);
int tmnt_region_pad_label(struct CPU *c);
void tmnt_region_pad_label_check(struct CPU *c);
/* Silhouette edge leaves in native code: RwV3dTransformPoint (0x5850ef), RW table invsqrt (0x4b8de0), ink vertex (0x4fca50),
 * ink quad (0x4fd570). */
int tmnt_fast_rw_xform_point(struct CPU *c);
int tmnt_fast_rw_invsqrt(struct CPU *c);
int tmnt_fast_toon_ink_vertex(struct CPU *c);
int tmnt_fast_toon_ink_quad(struct CPU *c);
/* RenderWare math in native code: RwV3dLength (0x4b8d10), RwV3dTransformPoints (0x585000), RwMatrixMultiply (0x5852a0). */
int tmnt_fast_rw_v3d_length(struct CPU *c);
int tmnt_fast_rw_xform_points(struct CPU *c);
int tmnt_fast_rw_matrix_multiply(struct CPU *c);
/* The game's animation curve (0x4a64aa): key search and cubic interpolation in native code. */
int tmnt_fast_anim_curve(struct CPU *c);
int tmnt_fast_rw_flush_states(struct CPU *c);
/* RpHAnim hierarchy attach callback (0x4ef9f0), recursive over the frame tree, in native code. */
int tmnt_fast_hanim_attach(struct CPU *c);
/* Sphere/triangle collision test (0x518cd0): its early rejection in native code. */
int tmnt_fast_sphere_tri(struct CPU *c);
int tmnt_fast_sfd_yuv_argb(struct CPU *c);
int tmnt_fast_sfd_skip(struct CPU *c);
void tmnt_hook_sfd_frame(struct CPU *c);
/* Elapsed-ticks helper (0x4b3354): rdtsc minus the base at 0x69f848, when its clock pointer is the rdtsc reader. */
int tmnt_fast_ticks(struct CPU *c);

#endif
