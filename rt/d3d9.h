/* Direct3D 9 fixed-function device over OpenGL: shared types between the object layer and the GL backend. */
#ifndef TMNT_D3D9_H
#define TMNT_D3D9_H

#include <stdint.h>
#include <string.h>
#include "com.h"
#include "dx_consts.h"
#include "plat.h"

enum { K_D3D = 1, K_DEVICE, K_TEXTURE, K_SURFACE, K_VB, K_IB, K_DECL, K_STATEBLOCK, K_SWAPCHAIN, K_QUERY };

#define MAX_LEVELS 13
#define MAX_STAGES 8

typedef struct d3d_tex d3d_tex;

typedef struct d3d_surf {
    int kind;
    uint32_t guest, refs;
    d3d_tex *parent;
    int level;
    uint32_t fmt, w, h, usage, pool;
    uint32_t mem, pitch;
    int is_backbuffer, is_depth;
    GLuint fbo;
} d3d_surf;

struct d3d_tex {
    int kind;
    uint32_t guest, refs;
    uint32_t fmt, w, h, levels, usage, pool;
    uint32_t mem[MAX_LEVELS], pitch[MAX_LEVELS], bytes[MAX_LEVELS];
    d3d_surf *surf[MAX_LEVELS];
    GLuint gl;
    GLuint gl_movie;   /* Vita: the ring texture holding the current movie frame (d3d9_gl), bound instead of gl */
    int gl_movie_hw;   /* Vita: 1 + the hardware-decoded picture gl_movie shows (movie.h), 0 for none */
    uint32_t dirty;             /* levels to upload; GL side only */
    uint32_t dirty_pending;     /* levels written by the game (any thread, atomic or); the GL side moves them into dirty */
    int uploaded;
    uint32_t sampler_key;
};

typedef struct d3d_buf {
    int kind;
    uint32_t guest, refs;
    uint32_t size, mem, usage, pool, fvf, fmt;
    uint32_t version, lock_frame;            /* writes (non read-only locks) and the frame of the last one */
    uint32_t gl_buf, gl_version, gl_key;     /* GL copy of a static buffer (d3d9_gl.c), its version and colour layout */
    int gl_bad;                              /* drawn with two colour layouts: never cached */
    uint8_t *gl_check;                       /* TMNT_VBO_CHECK=1: the bytes uploaded, to spot writes without a Lock */
} d3d_buf;

typedef struct d3d_decl_elem {
    uint16_t stream, offset;
    uint8_t type, method, usage, index;
} d3d_decl_elem;

typedef struct d3d_decl {
    int kind;
    uint32_t guest, refs;
    int count;
    d3d_decl_elem e[32];
} d3d_decl;

typedef struct d3d_light {
    uint32_t raw[26];
    int enabled;
} d3d_light;

typedef struct d3d_device {
    int kind;
    uint32_t guest, refs;
    uint32_t d3d_guest;
    uint32_t width, height;
    uint32_t rs[256];
    uint32_t tss[MAX_STAGES][33];
    uint32_t ss[MAX_STAGES][14];
    float world[16], view[16], proj[16], texm[MAX_STAGES][16];
    uint32_t viewport[6];
    float material[17];
    d3d_light lights[8];
    d3d_tex *tex[MAX_STAGES];
    struct { d3d_buf *vb; uint32_t offset, stride; } stream[4];
    d3d_buf *ib;
    uint32_t fvf;
    d3d_decl *decl;
    uint32_t scissor[4];
    d3d_surf *backbuffer, *depth, *rt0, *ds;
    uint32_t frame;
    int in_scene;
} d3d_device;

d3d_device *d3d_dev(void);
/* Render thread (d3d9_gl.c). Mode 0: the game thread makes the GL calls itself. Mode 1: it records them, and a render thread
 * replays the previous frame meanwhile (TMNT_RENDER_THREAD=1, the Vita's default). Mode 2: recorded and replayed on the game
 * thread at Present (TMNT_RENDER_THREAD=2, tests the recording on the Mac). The render side keeps a copy of the device: every
 * change to a device field must go through DEV_TOUCH so the copy gets it (TMNT_RQ_CHECK=1 compares the two and logs misses). */
extern int gl_rq_mode;
void gl_dev_touch(d3d_device *d, const void *field, uint32_t size);
#define DEV_TOUCH(d, field) gl_dev_touch((d), &(d)->field, (uint32_t)sizeof((d)->field))
/* Textures and buffers whose last reference went: freed once the render side has drawn every frame that used them. */
void gl_texture_release(d3d_tex *t);
void gl_buffer_release(d3d_buf *b);
/* Native calls into the device (d3d9.c), for replacements of the game's RenderWare D3D9 code. */
int d3d9_is_device(uint32_t guest);
void d3d9_set_render_state(uint32_t state, uint32_t value);
void d3d9_set_texture_stage_state(uint32_t stage, uint32_t type, uint32_t value);
size_t d3d9_states_size(void);
void d3d9_states_save(uint8_t *buf);
void d3d9_states_load(const uint8_t *buf);
void gl_init_device(d3d_device *d);
void gl_clear(d3d_device *d, uint32_t flags, uint32_t color, float z, uint32_t stencil, uint32_t count, uint32_t rects);
void gl_draw(d3d_device *d, uint32_t prim, uint32_t vbase, uint32_t stride0, uint32_t first_vertex, uint32_t num_vertices,
             const void *indices, int index32, uint32_t index_count);
void gl_upload_texture(d3d_tex *t);
void gl_set_render_target(d3d_device *d, d3d_surf *s);
void gl_present(d3d_device *d);
void gl_defer_delete_buffer(uint32_t buf);
extern uint32_t d3d_frame_now;   /* frames presented so far (buffer write tracking) */
extern d3d_buf *draw_vb_obj, *draw_ib_obj;   /* buffers behind the current draw; NULL for the UP draws */
void gl_defer_delete(GLuint tex);
uint32_t d3d_format_bpp(uint32_t fmt);
int d3d_format_is_dxt(uint32_t fmt);

static inline float u2f(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }

#endif
