/* OpenGL backend of the Direct3D 9 device: fixed-function state, vertex formats, textures, draws, present. */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

#include "d3d9.h"
#include "movie.h"
#include "par.h"
#include "game_hooks.h"
#include "overlay_font.h"

/* GL state cache. The draw path re-specifies the whole fixed-function state for every D3D draw; vitaGL marks its pipeline dirty
 * (and re-selects a shader) on each such call even when nothing changed. Every simple state call in this file goes through these
 * wrappers, which forward a call only when the value differs from the last one sent. Texture binds and lights are not cached
 * (deleted names and light positions depend on more than the arguments). All GL calls come from the render thread. */
static GLenum sc_unit;   /* active texture unit, 0..3 */
static GLenum sc_client_unit = 0xFFFFFFFFU;
typedef struct { GLenum cap; GLenum unit; int on; } sc_cap;
static sc_cap sc_caps[48];
static int sc_ncaps;
static void sc_enable(GLenum cap, int on)
{
    int per_unit = cap == GL_TEXTURE_2D;
#ifdef GL_TEXTURE_GEN_S
    per_unit |= cap == GL_TEXTURE_GEN_S || cap == GL_TEXTURE_GEN_T || cap == GL_TEXTURE_GEN_R;
#endif
    GLenum unit = per_unit ? sc_unit : 0;
    for (int i = 0; i < sc_ncaps; i++)
        if (sc_caps[i].cap == cap && sc_caps[i].unit == unit) {
            if (sc_caps[i].on == on) return;
            sc_caps[i].on = on;
            if (on) glEnable(cap); else glDisable(cap);
            return;
        }
    if (sc_ncaps < 48) { sc_caps[sc_ncaps].cap = cap; sc_caps[sc_ncaps].unit = unit; sc_caps[sc_ncaps].on = on; sc_ncaps++; }
    if (on) glEnable(cap); else glDisable(cap);
}
static void sc_active_texture(GLenum t)
{
    GLenum u = t - GL_TEXTURE0;
    if (u == sc_unit) return;
    sc_unit = u;
    glActiveTexture(t);
}
static void sc_client_active_texture(GLenum t)
{
    if (t == sc_client_unit) return;
    sc_client_unit = t;
    glClientActiveTexture(t);
}
/* Texture environment, per unit: integer parameters by name, the scales and the constant colour. */
typedef struct { GLenum pname; GLint v; } sc_env;
static sc_env sc_envs[4][24];
static int sc_nenv[4];
static float sc_env_scale[4][2] = { { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 } };
static float sc_env_color[4][4] = { { -1, -1, -1, -1 }, { -1, -1, -1, -1 }, { -1, -1, -1, -1 }, { -1, -1, -1, -1 } };
static void sc_tex_envi(GLenum target, GLenum pname, GLint v)
{
    int u = (int)sc_unit & 3;
    for (int i = 0; i < sc_nenv[u]; i++)
        if (sc_envs[u][i].pname == pname) {
            if (sc_envs[u][i].v == v) return;
            sc_envs[u][i].v = v;
            glTexEnvi(target, pname, v);
            return;
        }
    if (sc_nenv[u] < 24) { sc_envs[u][sc_nenv[u]].pname = pname; sc_envs[u][sc_nenv[u]].v = v; sc_nenv[u]++; }
    glTexEnvi(target, pname, v);
}
static void sc_tex_envf(GLenum target, GLenum pname, GLfloat v)
{
    int u = (int)sc_unit & 3, k = pname == GL_RGB_SCALE ? 0 : pname == GL_ALPHA_SCALE ? 1 : -1;
    if (k >= 0 && sc_env_scale[u][k] == v) return;
    if (k >= 0) sc_env_scale[u][k] = v;
    glTexEnvf(target, pname, v);
}
static void sc_tex_envfv(GLenum target, GLenum pname, GLfloat *v)
{
    int u = (int)sc_unit & 3;
    if (pname == GL_TEXTURE_ENV_COLOR) {
        if (memcmp(sc_env_color[u], v, sizeof sc_env_color[u]) == 0) return;
        memcpy(sc_env_color[u], v, sizeof sc_env_color[u]);
    }
    glTexEnvfv(target, pname, v);
}
/* Single-valued states: the last arguments, compared as a key of up to four words. */
enum { SC_DEPTHMASK, SC_DEPTHFUNC, SC_ALPHAFUNC, SC_BLENDFUNC, SC_BLENDEQ, SC_CULLFACE, SC_FRONTFACE, SC_COLORMASK, SC_SHADE,
       SC_POLYMODE, SC_FOGMODE, SC_FOGSTART, SC_FOGEND, SC_FOGDENSITY, SC_FOGCOLOR, SC_COLOR, SC_NORMAL, SC_COUNT };
static uint32_t sc_key[SC_COUNT][4];
static uint8_t sc_valid[SC_COUNT];
static int sc_same(int k, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    if (sc_valid[k] && sc_key[k][0] == a && sc_key[k][1] == b && sc_key[k][2] == c && sc_key[k][3] == d) return 1;
    sc_valid[k] = 1; sc_key[k][0] = a; sc_key[k][1] = b; sc_key[k][2] = c; sc_key[k][3] = d;
    return 0;
}
static uint32_t sc_f(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void sc_depth_mask(GLboolean m) { if (!sc_same(SC_DEPTHMASK, m, 0, 0, 0)) glDepthMask(m); }
static void sc_depth_func(GLenum f) { if (!sc_same(SC_DEPTHFUNC, f, 0, 0, 0)) glDepthFunc(f); }
static void sc_alpha_func(GLenum f, GLfloat r) { if (!sc_same(SC_ALPHAFUNC, f, sc_f(r), 0, 0)) glAlphaFunc(f, r); }
static void sc_blend_func(GLenum a, GLenum b) { if (!sc_same(SC_BLENDFUNC, a, b, 0, 0)) glBlendFunc(a, b); }
static void sc_blend_equation(GLenum e) { if (!sc_same(SC_BLENDEQ, e, 0, 0, 0)) glBlendEquation(e); }
static void sc_cull_face(GLenum m) { if (!sc_same(SC_CULLFACE, m, 0, 0, 0)) glCullFace(m); }
static void sc_front_face(GLenum m) { if (!sc_same(SC_FRONTFACE, m, 0, 0, 0)) glFrontFace(m); }
static void sc_color_mask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { if (!sc_same(SC_COLORMASK, r, g, b, a)) glColorMask(r, g, b, a); }
static void sc_shade_model(GLenum m) { if (!sc_same(SC_SHADE, m, 0, 0, 0)) glShadeModel(m); }
static void sc_polygon_mode(GLenum f, GLenum m) { if (!sc_same(SC_POLYMODE, f, m, 0, 0)) glPolygonMode(f, m); }
static void sc_fogi(GLenum p, GLint v) { if (p != GL_FOG_MODE || !sc_same(SC_FOGMODE, (uint32_t)v, 0, 0, 0)) glFogi(p, v); }
static void sc_fogf(GLenum p, GLfloat v)
{
    int k = p == GL_FOG_START ? SC_FOGSTART : p == GL_FOG_END ? SC_FOGEND : p == GL_FOG_DENSITY ? SC_FOGDENSITY : -1;
    if (k < 0 || !sc_same(k, sc_f(v), 0, 0, 0)) glFogf(p, v);
}
static void sc_fogfv(GLenum p, const GLfloat *v)
{
    if (p != GL_FOG_COLOR || !sc_same(SC_FOGCOLOR, sc_f(v[0]), sc_f(v[1]), sc_f(v[2]), sc_f(v[3]))) glFogfv(p, v);
}
/* glEnableClientState marks the fixed-function shaders dirty even when nothing changed, so vitaGL picks a shader again on every draw. */
static signed char sc_client_on[7] = { -1, -1, -1, -1, -1, -1, -1 };
static void sc_client_state(GLenum a, int on)
{
    int i = a == GL_VERTEX_ARRAY ? 0 : a == GL_NORMAL_ARRAY ? 1 : a == GL_COLOR_ARRAY ? 2
          : 3 + (sc_client_unit == 0xFFFFFFFFU ? 0 : (int)(sc_client_unit - GL_TEXTURE0));
    if (sc_client_on[i] == on) return;
    sc_client_on[i] = (signed char)on;
    if (on) glEnableClientState(a); else glDisableClientState(a);
}
static GLuint sc_array_buf, sc_element_buf;
static void sc_bind_buffer(GLenum t, GLuint b)
{
    GLuint *cur = t == GL_ARRAY_BUFFER ? &sc_array_buf : &sc_element_buf;
    if (*cur == b) return;
    *cur = b;
    glBindBuffer(t, b);
}
static void sc_color4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
    if (!sc_same(SC_COLOR, sc_f(r), sc_f(g), sc_f(b), sc_f(a))) glColor4f(r, g, b, a);
}
static void sc_normal3f(GLfloat x, GLfloat y, GLfloat z)
{
    if (!sc_same(SC_NORMAL, sc_f(x), sc_f(y), sc_f(z), 0)) glNormal3f(x, y, z);
}
#define glEnable(cap) sc_enable((cap), 1)
#define glDisable(cap) sc_enable((cap), 0)
#define glActiveTexture sc_active_texture
#define glClientActiveTexture sc_client_active_texture
#define glTexEnvi sc_tex_envi
#define glTexEnvf sc_tex_envf
#define glTexEnvfv sc_tex_envfv
#define glDepthMask sc_depth_mask
#define glDepthFunc sc_depth_func
#define glAlphaFunc sc_alpha_func
#define glBlendFunc sc_blend_func
#define glBlendEquation sc_blend_equation
#define glCullFace sc_cull_face
#define glFrontFace sc_front_face
#define glColorMask sc_color_mask
#define glShadeModel sc_shade_model
#define glPolygonMode sc_polygon_mode
#define glFogi sc_fogi
#define glFogf sc_fogf
#define glFogfv sc_fogfv
#define glEnableClientState(a) sc_client_state((a), 1)
#define glDisableClientState(a) sc_client_state((a), 0)
#define glBindBuffer sc_bind_buffer
#define glColor4f sc_color4f
#define glNormal3f sc_normal3f

static GLuint white_tex;
static int tc_enabled[4];   /* texture coordinate arrays left enabled per unit (draw path and overlay) */
static uint8_t *colors, *speculars;
static float *positions;
static size_t scratch_vertices;
static int shot_frame = -1, shot_every;
/* Texture coordinate generation on the CPU: always on the Vita (vitaGL has no glTexGen), TMNT_CPU_TEXGEN=1 elsewhere for comparison. */
static int cpu_texgen = -1;
static int dump_frame = -2;
static uint64_t last_present_us;

/* Transform, viewport and texture-matrix memory: vitaGL re-uploads a matrix or viewport on every set, so the draw path sets them
 * only when they change. Code that loads other matrices or viewports (pretransformed draws, the overlay, setup) clears it. */
static int xf_valid;
static float xf_last[3][16];
static GLint vp_last[6] = { -1, -1, -1, -1, -1, -1 };
static int texm_state[4];   /* 0 unknown, 1 identity, 2 the stage's matrix (texm_last) */
static float texm_last[4][16];
static void xf_forget(void) { xf_valid = 0; vp_last[2] = -1; memset(texm_state, 0, sizeof texm_state); }
/* Frame time split, logged with the fps line: draw calls, texture uploads (part of the draws), the buffer swap; the rest is the game. */
static uint64_t prof_draw_us, prof_upload_us, prof_swap_us, prof_upload_bytes, prof_frame_start;
static uint32_t prof_draws, prof_uploads;
static int map_fbw, map_fbh;
static void update_mapping(d3d_device *d);

static void ensure_scratch(size_t n)
{
    if (n <= scratch_vertices) return;
    scratch_vertices = n + 1024;
    colors = realloc(colors, scratch_vertices * 4);
    speculars = realloc(speculars, scratch_vertices * 4);
    positions = realloc(positions, scratch_vertices * 16);
}

void gl_init_device(d3d_device *d)
{
    glGenTextures(1, &white_tex);
    glBindTexture(GL_TEXTURE_2D, white_tex);
    uint32_t px = 0xFFFFFFFFU;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    update_mapping(d);
    glViewport(0, 0, map_fbw, map_fbh);
    xf_forget();
    const char *s = getenv("TMNT_SHOT_FRAME");
    if (s) shot_frame = atoi(s);
    s = getenv("TMNT_SHOT_EVERY");
    if (s) shot_every = atoi(s);
}


/* The game draws in its own resolution (800x600); every viewport, scissor and clear rectangle is mapped onto the real
 * framebuffer, either stretched to fill it (default, as in the Rogue Squadron port) or kept at 4:3 with black bars
 * (TMNT_ASPECT=4:3). Rectangles are D3D-style (top-left origin) and come out in GL window coordinates. */
static float map_sx = 1.0f, map_sy = 1.0f;
static int map_ox, map_oy;

static void update_mapping(d3d_device *d)
{
    int fw = (int)d->width, fh = (int)d->height;
    plat_screen_size(&fw, &fh);
    map_fbw = fw;
    map_fbh = fh;
    const char *a = getenv("TMNT_ASPECT");
    if (a && strcmp(a, "4:3") == 0 && fw * 3 != fh * 4) {
        int w = fw, h = fh;
        if (fw * 3 > fh * 4) w = fh * 4 / 3; else h = fw * 3 / 4;
        map_ox = (fw - w) / 2;
        map_oy = (fh - h) / 2;
        map_sx = (float)w / (float)d->width;
        map_sy = (float)h / (float)d->height;
    } else {
        map_ox = map_oy = 0;
        map_sx = (float)fw / (float)d->width;
        map_sy = (float)fh / (float)d->height;
    }
}

static void map_rect(d3d_device *d, uint32_t x, uint32_t y, uint32_t w, uint32_t h, GLint out[4])
{
    (void)d;
    int x0 = map_ox + (int)((float)x * map_sx + 0.5f), x1 = map_ox + (int)((float)(x + w) * map_sx + 0.5f);
    int y0 = map_oy + (int)((float)y * map_sy + 0.5f), y1 = map_oy + (int)((float)(y + h) * map_sy + 0.5f);
    out[0] = x0;
    out[1] = map_fbh - y1;
    out[2] = x1 - x0;
    out[3] = y1 - y0;
}

/* ------------------------------------------------------------ textures --- */

static void expand_level(uint32_t fmt, const uint8_t *src, uint32_t pitch, uint32_t w, uint32_t h, uint8_t *dst)
{
#if defined(__ARM_NEON)
    /* 32-bit ARGB, the common case (and the movie frames): swap red and blue 16 pixels at a time. */
    if (fmt == D3DFMT_A8R8G8B8 || fmt == D3DFMT_X8R8G8B8) {
        for (uint32_t y = 0; y < h; y++) {
            const uint8_t *s = src + (size_t)y * pitch;
            uint8_t *o = dst + (size_t)y * w * 4;
            uint32_t x = 0;
            for (; x + 16 <= w; x += 16) {
                uint8x16x4_t v = vld4q_u8(s + x * 4), r;
                r.val[0] = v.val[2]; r.val[1] = v.val[1]; r.val[2] = v.val[0];
                r.val[3] = fmt == D3DFMT_X8R8G8B8 ? vdupq_n_u8(255) : v.val[3];
                vst4q_u8(o + x * 4, r);
            }
            for (; x < w; x++) {
                o[x * 4] = s[x * 4 + 2]; o[x * 4 + 1] = s[x * 4 + 1]; o[x * 4 + 2] = s[x * 4];
                o[x * 4 + 3] = fmt == D3DFMT_X8R8G8B8 ? 255 : s[x * 4 + 3];
            }
        }
        return;
    }
#endif
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *s = src + (size_t)y * pitch;
        uint8_t *o = dst + (size_t)y * w * 4;
        for (uint32_t x = 0; x < w; x++, o += 4) {
            uint32_t v;
            switch (fmt) {
            case D3DFMT_A8R8G8B8: o[0] = s[x * 4 + 2]; o[1] = s[x * 4 + 1]; o[2] = s[x * 4]; o[3] = s[x * 4 + 3]; break;
            case D3DFMT_X8R8G8B8: o[0] = s[x * 4 + 2]; o[1] = s[x * 4 + 1]; o[2] = s[x * 4]; o[3] = 255; break;
            case D3DFMT_A8B8G8R8: memcpy(o, s + x * 4, 4); break;
            case D3DFMT_R5G6B5:
                v = (uint32_t)(s[x * 2] | (s[x * 2 + 1] << 8));
                o[0] = (uint8_t)(((v >> 11) & 31) * 255 / 31); o[1] = (uint8_t)(((v >> 5) & 63) * 255 / 63); o[2] = (uint8_t)((v & 31) * 255 / 31); o[3] = 255;
                break;
            case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5:
                v = (uint32_t)(s[x * 2] | (s[x * 2 + 1] << 8));
                o[0] = (uint8_t)(((v >> 10) & 31) * 255 / 31); o[1] = (uint8_t)(((v >> 5) & 31) * 255 / 31); o[2] = (uint8_t)((v & 31) * 255 / 31);
                o[3] = (fmt == D3DFMT_X1R5G5B5 || (v & 0x8000)) ? 255 : 0;
                break;
            case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4:
                v = (uint32_t)(s[x * 2] | (s[x * 2 + 1] << 8));
                o[0] = (uint8_t)(((v >> 8) & 15) * 17); o[1] = (uint8_t)(((v >> 4) & 15) * 17); o[2] = (uint8_t)((v & 15) * 17);
                o[3] = fmt == D3DFMT_X4R4G4B4 ? 255 : (uint8_t)(((v >> 12) & 15) * 17);
                break;
            case D3DFMT_L8: o[0] = o[1] = o[2] = s[x]; o[3] = 255; break;
            case D3DFMT_A8: o[0] = o[1] = o[2] = 255; o[3] = s[x]; break;
            case D3DFMT_A8L8: o[0] = o[1] = o[2] = s[x * 2]; o[3] = s[x * 2 + 1]; break;
            default: o[0] = 255; o[1] = 0; o[2] = 255; o[3] = 255; break;
            }
        }
    }
}

static void upload_texture(d3d_tex *t);
void gl_upload_texture(d3d_tex *t)
{
    if (!t->dirty && t->gl) {
        glBindTexture(GL_TEXTURE_2D, t->gl_movie ? t->gl_movie : t->gl);
#ifdef __vita__
        if (t->gl_movie_hw) movie_hw_bound_frame[t->gl_movie_hw - 1] = vglGetFrameNumber();
#endif
        return;
    }
    uint64_t t0 = plat_ticks_us();
    upload_texture(t);
    prof_upload_us += plat_ticks_us() - t0;
    prof_uploads++;
    prof_upload_bytes += (uint64_t)t->w * t->h * 4;
}

#ifdef __vita__
/* Movie frames on the Vita: glTexSubImage2D on a texture drawn in the last frames makes vitaGL copy the whole texture to new
   memory first (2 MB per movie frame), so frames go round three textures of their own and are written straight into them. */
#define MOVIE_RING 3
static GLuint movie_ring[MOVIE_RING];
static uint32_t movie_ring_w, movie_ring_h, movie_ring_next;

static void upload_movie_frame(d3d_tex *t, const uint8_t *guest_pixels)
{
    if (movie_ring_w != t->w || movie_ring_h != t->h) {
        for (int i = 0; i < MOVIE_RING; i++) {
            if (movie_ring[i]) gl_defer_delete(movie_ring[i]);
            glGenTextures(1, &movie_ring[i]);
            glBindTexture(GL_TEXTURE_2D, movie_ring[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)t->w, (GLsizei)t->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, guest_pixels);
        }
        movie_ring_w = t->w;
        movie_ring_h = t->h;
    }
    GLuint g = movie_ring[movie_ring_next];
    movie_ring_next = (movie_ring_next + 1) % MOVIE_RING;
    glBindTexture(GL_TEXTURE_2D, g);
    uint8_t *dst = vglGetTexDataPointer(GL_TEXTURE_2D);
    /* vitaGL rows are padded to 8 pixels. */
    size_t stride = (size_t)((t->w + 7U) & ~7U) * 4, row = (size_t)movie_pin_w * 4;
    for (int y = 0; dst && y < movie_pin_h; y++) memcpy(dst + y * stride, movie_pin + y * row, row);
    if (t->gl_movie != g) { t->gl_movie = g; t->sampler_key = 0; }
    t->gl_movie_hw = 0;
}

/* A picture of the hardware decoder (movie.h) as the movie texture: its video memory becomes a GL texture once (vitaGL then owns
   it; these textures are never deleted), and binding it is all an "upload" takes. */
static GLuint movie_hw_gl[MOVIE_HW_SLOTS];
/* In the vitaGL we build (build/vitagl), not yet in the SDK's vitaGL.h. */
void vglSetupTexture(GLenum target, void *data, SceGxmTextureFormat format, SceGxmTextureType type, GLsizei width, GLsizei height,
                     GLsizei mip_count);

static void bind_movie_hw(d3d_tex *t, int i)
{
    if (!movie_hw_gl[i]) {
        glGenTextures(1, &movie_hw_gl[i]);
        glBindTexture(GL_TEXTURE_2D, movie_hw_gl[i]);
        vglSetupTexture(GL_TEXTURE_2D, movie_hw_mem[i], SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, SCE_GXM_TEXTURE_LINEAR,
                        (GLsizei)movie_hw_w, (GLsizei)movie_hw_h, 1);
    } else {
        glBindTexture(GL_TEXTURE_2D, movie_hw_gl[i]);
    }
    movie_hw_bound_frame[i] = vglGetFrameNumber();
    if (t->gl_movie != movie_hw_gl[i]) { t->gl_movie = movie_hw_gl[i]; t->sampler_key = 0; }
    t->gl_movie_hw = i + 1;
}
#endif

static void upload_texture(d3d_tex *t)
{
    if (!t->gl) {
        glGenTextures(1, &t->gl);
        glBindTexture(GL_TEXTURE_2D, t->gl);
#ifndef __vita__
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)t->levels - 1);
#endif
    } else {
        glBindTexture(GL_TEXTURE_2D, t->gl);
    }
    if (!t->dirty) return;
    static uint8_t *tmp;
    static size_t tmp_size;
    for (uint32_t l = 0; l < t->levels; l++) {
        if (!(t->dirty & (1U << l)) && t->uploaded) continue;
        uint32_t w = t->w >> l ? t->w >> l : 1, h = t->h >> l ? t->h >> l : 1;
        const uint8_t *src = G2H(t->mem[l]);
        if (d3d_format_is_dxt(t->fmt)) {
            GLenum f = t->fmt == D3DFMT_DXT1 ? GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
                     : (t->fmt == D3DFMT_DXT2 || t->fmt == D3DFMT_DXT3) ? GL_COMPRESSED_RGBA_S3TC_DXT3_EXT : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
            glCompressedTexImage2D(GL_TEXTURE_2D, (GLint)l, f, (GLsizei)w, (GLsizei)h, 0, (GLsizei)t->bytes[l], src);
        } else {
            const uint8_t *pixels = src;
            int is_movie = l == 0 && t->fmt == D3DFMT_A8R8G8B8 && movie_rgba_mem && t->mem[0] == movie_rgba_mem && t->pitch[0] == w * 4;
#ifdef __vita__
            /* Movie frame from the hardware decoder: already in video memory, laid out like this texture. */
            if (is_movie && movie_pin_slot >= 0 && t->w == movie_hw_w && t->h == movie_hw_h) {
                bind_movie_hw(t, movie_pin_slot);
                continue;
            }
            static int hw_mismatch_logged;
            if (is_movie && movie_pin_slot >= 0 && !hw_mismatch_logged) {
                rt_log("[movie] movie texture %ux%u differs from the hardware pictures %ux%u: not shown\n", t->w, t->h, movie_hw_w,
                       movie_hw_h);
                hw_mismatch_logged = 1;
            }
#endif
            /* Movie frame: only the picture's rectangle, straight from the decoder's slot (rt/movie.c). */
            if (is_movie && movie_pin && (uint32_t)movie_pin_w <= w && (uint32_t)movie_pin_h <= h) {
#ifdef __vita__
                upload_movie_frame(t, src);
#else
                if (!t->uploaded) glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, src);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, movie_pin_w, movie_pin_h, GL_RGBA, GL_UNSIGNED_BYTE, movie_pin);
#endif
                continue;
            }
            /* Any other upload of this texture uses its own GL texture again. */
            if (l == 0 && t->gl_movie) { t->gl_movie = 0; t->gl_movie_hw = 0; t->sampler_key = 0; glBindTexture(GL_TEXTURE_2D, t->gl); }
            /* A movie frame is already RGBA (rt/movie.c); everything else is converted. */
            if (!is_movie) {
                size_t need = (size_t)w * h * 4;
                if (need > tmp_size) { tmp_size = need; tmp = realloc(tmp, tmp_size); }
                expand_level(t->fmt, src, t->pitch[l], w, h, tmp);
                pixels = tmp;
            }
            /* Re-uploads (movies, animated textures) reuse the texture storage instead of specifying it again. */
            if (t->uploaded) glTexSubImage2D(GL_TEXTURE_2D, (GLint)l, 0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            else glTexImage2D(GL_TEXTURE_2D, (GLint)l, GL_RGBA, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        }
    }
    t->dirty = 0;
    t->uploaded = 1;
}

static GLint gl_wrap(uint32_t a)
{
    switch (a) {
    case D3DTADDRESS_MIRROR: return GL_MIRRORED_REPEAT;
    case D3DTADDRESS_CLAMP: case D3DTADDRESS_BORDER: return GL_CLAMP_TO_EDGE;
    default: return GL_REPEAT;
    }
}

static void apply_sampler(d3d_device *d, int s, d3d_tex *t)
{
    uint32_t *ss = d->ss[s];
    uint32_t key = ss[D3DSAMP_ADDRESSU] | (ss[D3DSAMP_ADDRESSV] << 4) | (ss[D3DSAMP_MINFILTER] << 8) | (ss[D3DSAMP_MAGFILTER] << 12) | (ss[D3DSAMP_MIPFILTER] << 16) | 0x80000000U;
    if (t->sampler_key == key) return;
    t->sampler_key = key;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gl_wrap(ss[D3DSAMP_ADDRESSU]));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gl_wrap(ss[D3DSAMP_ADDRESSV]));
    int lin_mag = ss[D3DSAMP_MAGFILTER] >= D3DTEXF_LINEAR, lin_min = ss[D3DSAMP_MINFILTER] >= D3DTEXF_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, lin_mag ? GL_LINEAR : GL_NEAREST);
    GLint minf;
    if (t->levels <= 1 || ss[D3DSAMP_MIPFILTER] == D3DTEXF_NONE) minf = lin_min ? GL_LINEAR : GL_NEAREST;
    else if (ss[D3DSAMP_MIPFILTER] == D3DTEXF_POINT) minf = lin_min ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
    else minf = lin_min ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minf);
}

/* ---------------------------------------------------- texture stages --- */

static void color4(uint32_t argb, float out[4])
{
    out[0] = (float)((argb >> 16) & 255) / 255.0f; out[1] = (float)((argb >> 8) & 255) / 255.0f;
    out[2] = (float)(argb & 255) / 255.0f; out[3] = (float)(argb >> 24) / 255.0f;
}

/* Set per draw by apply_stages: the texture factor is fed as the primary colour (see there). */
static int tf_as_primary;

static GLint arg_source(uint32_t a, int stage)
{
    switch (a & 0xF) {
    case D3DTA_DIFFUSE: return GL_PRIMARY_COLOR;
    case D3DTA_CURRENT: return stage == 0 ? GL_PRIMARY_COLOR : GL_PREVIOUS;
    case D3DTA_TEXTURE: return GL_TEXTURE;
    case D3DTA_TFACTOR: return tf_as_primary ? GL_PRIMARY_COLOR : GL_CONSTANT;
    default: return GL_PRIMARY_COLOR;
    }
}

static GLint arg_operand(uint32_t a, int alpha)
{
    int comp = (a & D3DTA_COMPLEMENT) != 0, rep = (a & D3DTA_ALPHAREPLICATE) != 0;
    if (alpha || rep) return comp ? GL_ONE_MINUS_SRC_ALPHA : GL_SRC_ALPHA;
    return comp ? GL_ONE_MINUS_SRC_COLOR : GL_SRC_COLOR;
}

#ifndef GL_SOURCE0_RGB   /* vitaGL uses the GL 1.5 names (same values) */
#define GL_SOURCE0_RGB GL_SRC0_RGB
#define GL_SOURCE1_RGB GL_SRC1_RGB
#define GL_SOURCE2_RGB GL_SRC2_RGB
#define GL_SOURCE0_ALPHA GL_SRC0_ALPHA
#define GL_SOURCE1_ALPHA GL_SRC1_ALPHA
#define GL_SOURCE2_ALPHA GL_SRC2_ALPHA
#endif

#ifdef __vita__
/* vitaGL lights: glLightfv only and no spot lights (spot parameters are dropped). */
#define GL_SPOT_EXPONENT 0x1205
#define GL_SPOT_CUTOFF 0x1206
#define GL_SPOT_DIRECTION 0x1204
static void vita_light_f(GLenum l, GLenum p, GLfloat v) { if (p != GL_SPOT_CUTOFF && p != GL_SPOT_EXPONENT) glLightfv(l, p, &v); }
#define glLightf vita_light_f
#define glLightfv(l, p, v) ((p) == GL_SPOT_DIRECTION ? (void)0 : glLightfv((l), (p), (v)))
#endif

static void combine(int alpha, uint32_t op, uint32_t a1, uint32_t a2, int stage)
{
    GLenum CMB = alpha ? GL_COMBINE_ALPHA : GL_COMBINE_RGB;
    GLenum S0 = alpha ? GL_SOURCE0_ALPHA : GL_SOURCE0_RGB, S1 = alpha ? GL_SOURCE1_ALPHA : GL_SOURCE1_RGB, S2 = alpha ? GL_SOURCE2_ALPHA : GL_SOURCE2_RGB;
    GLenum O0 = alpha ? GL_OPERAND0_ALPHA : GL_OPERAND0_RGB, O1 = alpha ? GL_OPERAND1_ALPHA : GL_OPERAND1_RGB, O2 = alpha ? GL_OPERAND2_ALPHA : GL_OPERAND2_RGB;
    GLenum SCALE = alpha ? GL_ALPHA_SCALE : GL_RGB_SCALE;
    GLint mode = GL_MODULATE;
    float scale = 1.0f;
    uint32_t x1 = a1, x2 = a2;
    switch (op) {
    case D3DTOP_SELECTARG1: mode = GL_REPLACE; break;
    case D3DTOP_SELECTARG2: mode = GL_REPLACE; x1 = a2; break;
    case D3DTOP_MODULATE: break;
    case D3DTOP_MODULATE2X: scale = 2.0f; break;
    case D3DTOP_MODULATE4X: scale = 4.0f; break;
    case D3DTOP_ADD: mode = GL_ADD; break;
    case D3DTOP_ADDSIGNED: mode = GL_ADD_SIGNED; break;
    case D3DTOP_ADDSIGNED2X: mode = GL_ADD_SIGNED; scale = 2.0f; break;
    case D3DTOP_SUBTRACT: mode = GL_SUBTRACT; break;
    case D3DTOP_BLENDDIFFUSEALPHA: case D3DTOP_BLENDTEXTUREALPHA: case D3DTOP_BLENDFACTORALPHA: case D3DTOP_BLENDCURRENTALPHA:
        mode = GL_INTERPOLATE;
        break;
    case D3DTOP_DOTPRODUCT3: if (!alpha) mode = GL_DOT3_RGB; break;
    default: break;
    }
    glTexEnvi(GL_TEXTURE_ENV, CMB, mode);
    glTexEnvi(GL_TEXTURE_ENV, S0, arg_source(x1, stage));
    glTexEnvi(GL_TEXTURE_ENV, O0, arg_operand(x1, alpha));
    glTexEnvi(GL_TEXTURE_ENV, S1, arg_source(x2, stage));
    glTexEnvi(GL_TEXTURE_ENV, O1, arg_operand(x2, alpha));
    if (mode == GL_INTERPOLATE) {
        uint32_t src = op == D3DTOP_BLENDDIFFUSEALPHA ? D3DTA_DIFFUSE : op == D3DTOP_BLENDTEXTUREALPHA ? D3DTA_TEXTURE : op == D3DTOP_BLENDFACTORALPHA ? D3DTA_TFACTOR : D3DTA_CURRENT;
        glTexEnvi(GL_TEXTURE_ENV, S2, arg_source(src, stage));
        glTexEnvi(GL_TEXTURE_ENV, O2, GL_SRC_ALPHA);
    }
    glTexEnvf(GL_TEXTURE_ENV, SCALE, scale);
}

static int uses_texture(uint32_t op, uint32_t a1, uint32_t a2)
{
    if (op == D3DTOP_DISABLE) return 0;
    if (op == D3DTOP_BLENDTEXTUREALPHA) return 1;
    if ((a1 & 0xF) == D3DTA_TEXTURE && op != D3DTOP_SELECTARG2) return 1;
    return (a2 & 0xF) == D3DTA_TEXTURE && op != D3DTOP_SELECTARG1;
}

/* Returns the number of texture units set up. */
/* Whether the enabled stages read the diffuse colour (directly, as CURRENT on stage 0, through an op that blends with it,
 * or through a source the combiner maps to the primary colour). Mirrors the argument choice in apply_stages. */
static int stages_read_diffuse(d3d_device *d)
{
    for (int s = 0; s < MAX_STAGES && s < 4; s++) {
        uint32_t *t = d->tss[s];
        if (t[D3DTSS_COLOROP] == D3DTOP_DISABLE) return s == 0;   /* no stages: the output is the diffuse colour itself */
        for (int alpha = 0; alpha < 2; alpha++) {
            uint32_t op = t[alpha ? D3DTSS_ALPHAOP : D3DTSS_COLOROP];
            uint32_t a1 = t[alpha ? D3DTSS_ALPHAARG1 : D3DTSS_COLORARG1], a2 = t[alpha ? D3DTSS_ALPHAARG2 : D3DTSS_COLORARG2];
            if (op == D3DTOP_DISABLE) { if (alpha && s == 0) return 1; continue; }
            if (!d->tex[s] && op == D3DTOP_SELECTARG1 && (a1 & 0xF) == D3DTA_TEXTURE) op = D3DTOP_SELECTARG2;
            if (op == D3DTOP_BLENDDIFFUSEALPHA || (op == D3DTOP_BLENDCURRENTALPHA && s == 0)) return 1;
            for (int k = 0; k < 2; k++) {
                uint32_t a = k ? a2 : a1;
                if ((k == 0 && op == D3DTOP_SELECTARG2) || (k == 1 && op == D3DTOP_SELECTARG1)) continue;
                uint32_t src = a & 0xF;
                if (src == D3DTA_DIFFUSE || (src == D3DTA_CURRENT && s == 0)) return 1;
                if (src != D3DTA_CURRENT && src != D3DTA_TEXTURE && src != D3DTA_TFACTOR) return 1;
            }
        }
    }
    return 0;
}

/* Returns the number of texture units set up. no_vertex_colour: the draw has no colour array and no lighting, so the primary
 * colour is a constant. vitaGL's combiner ignores GL_CONSTANT (TFACTOR): the turtles' black outline hulls (SELECTARG2 TFACTOR)
 * came out white. When the stages never read the diffuse colour, the factor goes in as the primary colour instead. */
static int apply_stages(d3d_device *d, int texcoords, int no_vertex_colour)
{
    float tf[4];
    color4(d->rs[D3DRS_TEXTUREFACTOR], tf);
    tf_as_primary = no_vertex_colour && !stages_read_diffuse(d);
    if (tf_as_primary) glColor4f(tf[0], tf[1], tf[2], tf[3]);
    int unit = 0;
    for (int s = 0; s < MAX_STAGES && s < 4; s++) {
        uint32_t *t = d->tss[s];
        if (t[D3DTSS_COLOROP] == D3DTOP_DISABLE) break;
        d3d_tex *tex = d->tex[s];
        glActiveTexture(GL_TEXTURE0 + (GLenum)s);
        glClientActiveTexture(GL_TEXTURE0 + (GLenum)s);
        glEnable(GL_TEXTURE_2D);
        if (tex) {
            gl_upload_texture(tex);
            apply_sampler(d, s, tex);
        } else {
            glBindTexture(GL_TEXTURE_2D, white_tex);
        }
        uint32_t cop = t[D3DTSS_COLOROP], aop = t[D3DTSS_ALPHAOP];
        uint32_t c1 = t[D3DTSS_COLORARG1], c2 = t[D3DTSS_COLORARG2], a1 = t[D3DTSS_ALPHAARG1], a2 = t[D3DTSS_ALPHAARG2];
        if (!tex) {
            /* A stage without a texture reads the texture as opaque white; pick the non-texture argument when the op only selects. */
            if (cop == D3DTOP_SELECTARG1 && (c1 & 0xF) == D3DTA_TEXTURE) cop = D3DTOP_SELECTARG2;
            if (aop == D3DTOP_SELECTARG1 && (a1 & 0xF) == D3DTA_TEXTURE) aop = D3DTOP_SELECTARG2;
        }
        if (aop == D3DTOP_DISABLE) { aop = D3DTOP_SELECTARG1; a1 = D3DTA_CURRENT; }
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
        glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, tf);
        combine(0, cop, c1, c2, s);
        combine(1, aop, a1, a2, s);
        uint32_t tci = t[D3DTSS_TEXCOORDINDEX], gen = tci & 0xFFFF0000U;
        if (cpu_texgen < 0) {
#ifdef __vita__
            cpu_texgen = 1;
#else
            cpu_texgen = getenv("TMNT_CPU_TEXGEN") && atoi(getenv("TMNT_CPU_TEXGEN"));
#endif
        }
        /* CPU-generated coordinates already include the texture transform. Loaded only when it changes (vitaGL re-uploads a matrix
         * on every load). */
        int use_texm = (t[D3DTSS_TEXTURETRANSFORMFLAGS] & 0xFF) && !(gen && cpu_texgen);
        if (texm_state[s] != (use_texm ? 2 : 1) || (use_texm && memcmp(texm_last[s], d->texm[s], sizeof texm_last[s]))) {
            glMatrixMode(GL_TEXTURE);
            if (use_texm) { glLoadMatrixf(d->texm[s]); memcpy(texm_last[s], d->texm[s], sizeof texm_last[s]); } else glLoadIdentity();
            glMatrixMode(GL_MODELVIEW);
            texm_state[s] = use_texm ? 2 : 1;
        }
#ifndef __vita__
        if (gen && !cpu_texgen) {
            GLint mode = gen == D3DTSS_TCI_CAMERASPACENORMAL ? GL_NORMAL_MAP : gen == D3DTSS_TCI_SPHEREMAP ? GL_SPHERE_MAP
                       : gen == D3DTSS_TCI_CAMERASPACEPOSITION ? GL_EYE_LINEAR : GL_REFLECTION_MAP;
            glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, mode);
            glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, mode);
            glEnable(GL_TEXTURE_GEN_S);
            glEnable(GL_TEXTURE_GEN_T);
            if (mode != GL_SPHERE_MAP) { glTexGeni(GL_R, GL_TEXTURE_GEN_MODE, mode); glEnable(GL_TEXTURE_GEN_R); }
        } else {
            glDisable(GL_TEXTURE_GEN_S); glDisable(GL_TEXTURE_GEN_T); glDisable(GL_TEXTURE_GEN_R);
        }
#endif
        (void)texcoords;
        unit = s + 1;
    }
    for (int s = unit; s < 4; s++) {
        glActiveTexture(GL_TEXTURE0 + (GLenum)s);
        glDisable(GL_TEXTURE_2D);
#ifndef __vita__
        glDisable(GL_TEXTURE_GEN_S); glDisable(GL_TEXTURE_GEN_T); glDisable(GL_TEXTURE_GEN_R);
#endif
    }
    glActiveTexture(GL_TEXTURE0);
    return unit;
}

/* ----------------------------------------------------------- render states --- */

static GLenum gl_cmp(uint32_t f)
{
    static const GLenum m[] = { GL_ALWAYS, GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS };
    return f <= 8 ? m[f] : GL_ALWAYS;
}

static GLenum gl_blend(uint32_t b)
{
    switch (b) {
    case D3DBLEND_ZERO: return GL_ZERO;
    case D3DBLEND_ONE: return GL_ONE;
    case D3DBLEND_SRCCOLOR: return GL_SRC_COLOR;
    case D3DBLEND_INVSRCCOLOR: return GL_ONE_MINUS_SRC_COLOR;
    case D3DBLEND_SRCALPHA: return GL_SRC_ALPHA;
    case D3DBLEND_INVSRCALPHA: return GL_ONE_MINUS_SRC_ALPHA;
    case D3DBLEND_DESTALPHA: return GL_DST_ALPHA;
    case D3DBLEND_INVDESTALPHA: return GL_ONE_MINUS_DST_ALPHA;
    case D3DBLEND_DESTCOLOR: return GL_DST_COLOR;
    case D3DBLEND_INVDESTCOLOR: return GL_ONE_MINUS_DST_COLOR;
    case D3DBLEND_SRCALPHASAT: return GL_SRC_ALPHA_SATURATE;
#ifdef __vita__
    /* vitaGL has no constant blend colour; logged once if the game ever asks for it. */
    case D3DBLEND_BLENDFACTOR: case D3DBLEND_INVBLENDFACTOR: {
        static int logged;
        if (!logged) { logged = 1; rt_log("[gl] blend factor %u unsupported on the Vita\n", b); }
        return b == D3DBLEND_BLENDFACTOR ? GL_ONE : GL_ZERO;
    }
#else
    case D3DBLEND_BLENDFACTOR: return GL_CONSTANT_COLOR;
    case D3DBLEND_INVBLENDFACTOR: return GL_ONE_MINUS_CONSTANT_COLOR;
#endif
    default: return GL_ONE;
    }
}

static GLenum gl_stencil_op(uint32_t o)
{
    switch (o) {
    case D3DSTENCILOP_ZERO: return GL_ZERO;
    case D3DSTENCILOP_REPLACE: return GL_REPLACE;
    case D3DSTENCILOP_INCRSAT: return GL_INCR;
    case D3DSTENCILOP_DECRSAT: return GL_DECR;
    case D3DSTENCILOP_INVERT: return GL_INVERT;
    case D3DSTENCILOP_INCR: return GL_INCR_WRAP;
    case D3DSTENCILOP_DECR: return GL_DECR_WRAP;
    default: return GL_KEEP;
    }
}

static void apply_render_states(d3d_device *d, int pretransformed)
{
    uint32_t *rs = d->rs;
    if (rs[D3DRS_ZENABLE]) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthMask(rs[D3DRS_ZWRITEENABLE] ? GL_TRUE : GL_FALSE);
    glDepthFunc(gl_cmp(rs[D3DRS_ZFUNC]));
    if (rs[D3DRS_ALPHATESTENABLE]) {
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(gl_cmp(rs[D3DRS_ALPHAFUNC]), (float)(rs[D3DRS_ALPHAREF] & 255) / 255.0f);
    } else {
        glDisable(GL_ALPHA_TEST);
    }
    if (rs[D3DRS_ALPHABLENDENABLE]) {
        glEnable(GL_BLEND);
        glBlendFunc(gl_blend(rs[D3DRS_SRCBLEND]), gl_blend(rs[D3DRS_DESTBLEND]));
        uint32_t op = rs[D3DRS_BLENDOP];
        glBlendEquation(op == D3DBLENDOP_SUBTRACT ? GL_FUNC_SUBTRACT : op == D3DBLENDOP_REVSUBTRACT ? GL_FUNC_REVERSE_SUBTRACT
                        : op == D3DBLENDOP_MIN ? GL_MIN : op == D3DBLENDOP_MAX ? GL_MAX : GL_FUNC_ADD);
    } else {
        glDisable(GL_BLEND);
    }
    if (rs[D3DRS_CULLMODE] == D3DCULL_NONE) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(rs[D3DRS_CULLMODE] == D3DCULL_CCW ? GL_CW : GL_CCW);
    }
    uint32_t cw = rs[D3DRS_COLORWRITEENABLE];
    glColorMask(cw & 1 ? GL_TRUE : GL_FALSE, cw & 2 ? GL_TRUE : GL_FALSE, cw & 4 ? GL_TRUE : GL_FALSE, cw & 8 ? GL_TRUE : GL_FALSE);
    glShadeModel(rs[D3DRS_SHADEMODE] == D3DSHADE_FLAT ? GL_FLAT : GL_SMOOTH);
    glPolygonMode(GL_FRONT_AND_BACK, rs[D3DRS_FILLMODE] == D3DFILL_WIREFRAME ? GL_LINE : rs[D3DRS_FILLMODE] == D3DFILL_POINT ? GL_POINT : GL_FILL);
    if (rs[D3DRS_STENCILENABLE]) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(gl_cmp(rs[D3DRS_STENCILFUNC]), (GLint)rs[D3DRS_STENCILREF], rs[D3DRS_STENCILMASK]);
        glStencilOp(gl_stencil_op(rs[D3DRS_STENCILFAIL]), gl_stencil_op(rs[D3DRS_STENCILZFAIL]), gl_stencil_op(rs[D3DRS_STENCILPASS]));
        glStencilMask(rs[D3DRS_STENCILWRITEMASK]);
    } else {
        glDisable(GL_STENCIL_TEST);
    }
    float bias = u2f(rs[D3DRS_DEPTHBIAS]), slope = u2f(rs[D3DRS_SLOPESCALEDEPTHBIAS]);
    if (bias != 0.0f || slope != 0.0f) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(slope, bias * 16777215.0f);
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
    if (rs[D3DRS_SCISSORTESTENABLE]) {
        glEnable(GL_SCISSOR_TEST);
        GLint r[4];
        map_rect(d, d->scissor[0], d->scissor[1], d->scissor[2] - d->scissor[0], d->scissor[3] - d->scissor[1], r);
        glScissor(r[0], r[1], r[2], r[3]);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
    if (rs[D3DRS_FOGENABLE]) {
        float fc[4];
        color4(rs[D3DRS_FOGCOLOR], fc);
        uint32_t mode = rs[D3DRS_FOGTABLEMODE] != D3DFOG_NONE ? rs[D3DRS_FOGTABLEMODE] : rs[D3DRS_FOGVERTEXMODE];
        if (mode == D3DFOG_NONE || pretransformed) {
            glDisable(GL_FOG);
        } else {
            glEnable(GL_FOG);
            glFogfv(GL_FOG_COLOR, fc);
            glFogi(GL_FOG_MODE, mode == D3DFOG_EXP ? GL_EXP : mode == D3DFOG_EXP2 ? GL_EXP2 : GL_LINEAR);
            glFogf(GL_FOG_START, u2f(rs[D3DRS_FOGSTART]));
            glFogf(GL_FOG_END, u2f(rs[D3DRS_FOGEND]));
            glFogf(GL_FOG_DENSITY, u2f(rs[D3DRS_FOGDENSITY]));
        }
    } else {
        glDisable(GL_FOG);
    }
}

static void apply_lighting(d3d_device *d, int has_normal, int has_diffuse, int has_specular)
{
    uint32_t *rs = d->rs;
    if (!rs[D3DRS_LIGHTING]) {
        glDisable(GL_LIGHTING);
        return;
    }
    glEnable(GL_LIGHTING);
    if (rs[D3DRS_NORMALIZENORMALS]) glEnable(GL_NORMALIZE); else glDisable(GL_NORMALIZE);
    /* Material, lights and their view matrix sent only when they change: vitaGL re-uploads its lighting uniforms on every set. */
    static struct {
        uint32_t valid, rsv[7], has_normal, has_diffuse;
        float material[17], view[16];
        d3d_light lights[8];
    } last, now;
    memset(&now, 0, sizeof now);
    now.valid = 1;
    now.rsv[0] = rs[D3DRS_AMBIENT]; now.rsv[1] = rs[D3DRS_LOCALVIEWER]; now.rsv[2] = rs[D3DRS_SPECULARENABLE];
    now.rsv[3] = rs[D3DRS_COLORVERTEX]; now.rsv[4] = rs[D3DRS_DIFFUSEMATERIALSOURCE]; now.rsv[5] = rs[D3DRS_AMBIENTMATERIALSOURCE];
    now.rsv[6] = rs[D3DRS_EMISSIVEMATERIALSOURCE];
    now.has_normal = (uint32_t)has_normal; now.has_diffuse = (uint32_t)has_diffuse;
    memcpy(now.material, d->material, sizeof now.material);
    memcpy(now.view, d->view, sizeof now.view);
    for (int i = 0; i < 8; i++)
        if (d->lights[i].enabled) now.lights[i] = d->lights[i];
    if (!memcmp(&now, &last, sizeof now)) return;
    last = now;
    float amb[4];
    color4(rs[D3DRS_AMBIENT], amb);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb);
#ifndef __vita__
    glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, rs[D3DRS_LOCALVIEWER] ? GL_TRUE : GL_FALSE);
#endif
    const float *m = d->material;
    glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, m);
    glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, m + 4);
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, rs[D3DRS_SPECULARENABLE] ? m + 8 : (const float[4]){ 0, 0, 0, 0 });
    glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, m + 12);
    glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, m[16] > 128.0f ? 128.0f : m[16]);
    if (rs[D3DRS_COLORVERTEX] && has_diffuse) {
        uint32_t ds = rs[D3DRS_DIFFUSEMATERIALSOURCE], as = rs[D3DRS_AMBIENTMATERIALSOURCE], es = rs[D3DRS_EMISSIVEMATERIALSOURCE];
        GLenum mode = 0;
        if (ds == D3DMCS_COLOR1 && as == D3DMCS_COLOR1) mode = GL_AMBIENT_AND_DIFFUSE;
        else if (ds == D3DMCS_COLOR1) mode = GL_DIFFUSE;
        else if (as == D3DMCS_COLOR1) mode = GL_AMBIENT;
        else if (es == D3DMCS_COLOR1) mode = GL_EMISSION;
        if (mode) { glColorMaterial(GL_FRONT_AND_BACK, mode); glEnable(GL_COLOR_MATERIAL); } else glDisable(GL_COLOR_MATERIAL);
    } else {
        glDisable(GL_COLOR_MATERIAL);
    }
    (void)has_specular;
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadMatrixf(d->view);
    for (int i = 0; i < 8; i++) {
        d3d_light *l = &d->lights[i];
        GLenum L = GL_LIGHT0 + (GLenum)i;
        if (!l->enabled || !has_normal) { glDisable(L); continue; }
        const float *f = (const float *)l->raw;
        uint32_t type = l->raw[0];
        glEnable(L);
        glLightfv(L, GL_DIFFUSE, f + 1);
        glLightfv(L, GL_SPECULAR, f + 5);
        glLightfv(L, GL_AMBIENT, f + 9);
        if (type == D3DLIGHT_DIRECTIONAL) {
            float p[4] = { -f[16], -f[17], -f[18], 0.0f };
            glLightfv(L, GL_POSITION, p);
            glLightf(L, GL_SPOT_CUTOFF, 180.0f);
            glLightf(L, GL_CONSTANT_ATTENUATION, 1.0f); glLightf(L, GL_LINEAR_ATTENUATION, 0.0f); glLightf(L, GL_QUADRATIC_ATTENUATION, 0.0f);
        } else {
            float p[4] = { f[13], f[14], f[15], 1.0f };
            glLightfv(L, GL_POSITION, p);
            glLightf(L, GL_CONSTANT_ATTENUATION, f[21]); glLightf(L, GL_LINEAR_ATTENUATION, f[22]); glLightf(L, GL_QUADRATIC_ATTENUATION, f[23]);
            if (type == D3DLIGHT_SPOT) {
                glLightfv(L, GL_SPOT_DIRECTION, f + 16);
                glLightf(L, GL_SPOT_CUTOFF, f[25] * 0.5f * 57.29578f);
                glLightf(L, GL_SPOT_EXPONENT, f[20]);
            } else {
                glLightf(L, GL_SPOT_CUTOFF, 180.0f);
            }
        }
    }
    glPopMatrix();
}

/* ------------------------------------------------------- vertex formats --- */

typedef struct vfmt {
    int pos_off, pos_rhw, normal_off, diffuse_off, specular_off;
    int tex_off[8], tex_size[8], tex_count;
    int pos_stream, normal_stream, diffuse_stream, specular_stream, tex_stream[8];
    int pos_comps;
} vfmt;

static int fvf_tex_size(uint32_t fvf, int i)
{
    switch ((fvf >> (16 + i * 2)) & 3) {
    case 1: return 3;
    case 2: return 4;
    case 3: return 1;
    default: return 2;
    }
}

static void parse_fvf(uint32_t fvf, vfmt *v)
{
    memset(v, 0, sizeof *v);
    v->normal_off = v->diffuse_off = v->specular_off = -1;
    int off = 0;
    uint32_t pos = fvf & D3DFVF_POSITION_MASK;
    v->pos_off = 0;
    v->pos_comps = 3;
    if (pos == D3DFVF_XYZRHW) { v->pos_rhw = 1; off = 16; }
    else if (pos == D3DFVF_XYZW) { v->pos_comps = 4; off = 16; }
    else if (pos >= D3DFVF_XYZB1 && pos <= D3DFVF_XYZB5) off = 12 + 4 * (int)((pos - D3DFVF_XYZ) / 2);
    else off = 12;
    if (fvf & D3DFVF_NORMAL) { v->normal_off = off; off += 12; }
    if (fvf & D3DFVF_PSIZE) off += 4;
    if (fvf & D3DFVF_DIFFUSE) { v->diffuse_off = off; off += 4; }
    if (fvf & D3DFVF_SPECULAR) { v->specular_off = off; off += 4; }
    v->tex_count = (int)((fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT);
    for (int i = 0; i < v->tex_count && i < 8; i++) {
        v->tex_off[i] = off;
        v->tex_size[i] = fvf_tex_size(fvf, i);
        off += 4 * v->tex_size[i];
    }
}

static void parse_decl(const d3d_decl *dc, vfmt *v)
{
    memset(v, 0, sizeof *v);
    v->normal_off = v->diffuse_off = v->specular_off = v->pos_off = -1;
    for (int i = 0; i < dc->count; i++) {
        const d3d_decl_elem *e = &dc->e[i];
        int size = e->type <= D3DDECLTYPE_FLOAT4 ? (int)e->type + 1 : 4;
        switch (e->usage) {
        case D3DDECLUSAGE_POSITION: if (e->index == 0) { v->pos_off = e->offset; v->pos_stream = e->stream; v->pos_comps = size >= 4 ? 4 : 3; } break;
        case D3DDECLUSAGE_POSITIONT: v->pos_off = e->offset; v->pos_stream = e->stream; v->pos_rhw = 1; break;
        case D3DDECLUSAGE_NORMAL: if (e->index == 0) { v->normal_off = e->offset; v->normal_stream = e->stream; } break;
        case D3DDECLUSAGE_COLOR:
            if (e->index == 0) { v->diffuse_off = e->offset; v->diffuse_stream = e->stream; }
            else if (e->index == 1) { v->specular_off = e->offset; v->specular_stream = e->stream; }
            break;
        case D3DDECLUSAGE_TEXCOORD:
            if (e->index < 8) {
                v->tex_off[e->index] = e->offset;
                v->tex_size[e->index] = size;
                v->tex_stream[e->index] = e->stream;
                if (v->tex_count < e->index + 1) v->tex_count = e->index + 1;
            }
            break;
        default: break;
        }
    }
    if (v->pos_off < 0) v->pos_off = 0;
}

/* --------------------------------------------------------------- draws --- */

static GLenum gl_prim(uint32_t t)
{
    switch (t) {
    case D3DPT_POINTLIST: return GL_POINTS;
    case D3DPT_LINELIST: return GL_LINES;
    case D3DPT_LINESTRIP: return GL_LINE_STRIP;
    case D3DPT_TRIANGLESTRIP: return GL_TRIANGLE_STRIP;
    case D3DPT_TRIANGLEFAN: return GL_TRIANGLE_FAN;
    default: return GL_TRIANGLES;
    }
}

static void bgra_to_rgba(const uint8_t *src, uint32_t stride, uint32_t n, uint8_t *dst)
{
    for (uint32_t i = 0; i < n; i++, src += stride, dst += 4) {
        dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = src[3];
    }
}

static void load_projection(d3d_device *d)
{
    if (xf_valid == 1 && !memcmp(xf_last[0], d->proj, 64) && !memcmp(xf_last[1], d->view, 64) && !memcmp(xf_last[2], d->world, 64)) return;
    memcpy(xf_last[0], d->proj, 64); memcpy(xf_last[1], d->view, 64); memcpy(xf_last[2], d->world, 64);
    xf_valid = 1;
    glMatrixMode(GL_PROJECTION);
    static const float zfix[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 2, 0, 0, 0, -1, 1 };
    glLoadMatrixf(zfix);
    glMultMatrixf(d->proj);
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(d->view);
    glMultMatrixf(d->world);
}

static void set_viewport(d3d_device *d)
{
    uint32_t *vp = d->viewport;
    GLint r[4];
    map_rect(d, vp[0], vp[1], vp[2], vp[3], r);
    if (memcmp(r, vp_last, sizeof r)) { glViewport(r[0], r[1], r[2], r[3]); memcpy(vp_last, r, sizeof r); }
    if (vp[4] != vp_last[4] || vp[5] != vp_last[5]) { glDepthRange(u2f(vp[4]), u2f(vp[5])); vp_last[4] = (GLint)vp[4]; vp_last[5] = (GLint)vp[5]; }
}

/* D3D texture coordinate generation done on the CPU for stage s: the camera-space normal (world * view, normalised when
 * D3DRS_NORMALIZENORMALS is set) as (x, y, z, 1), through the stage's texture transform when one is set; the turtles' cel shading
 * uses it. Other modes, or a draw without normals, get (0, 0) so the unit still has coordinates (vitaGL crashed without them). */
static float *texgen_buf[4];
static size_t texgen_cap[4];
typedef struct {
    float wv[9], m[16];
    int xf, norm;
    const uint8_t *base;
    uint32_t st;
    float *out;
} texgen_job;

static void texgen_range(void *ctx, uint32_t from, uint32_t to, int worker)
{
    const texgen_job *j = ctx;
    const float *wv = j->wv, *m = j->m;
    int xf = j->xf, norm = j->norm;
    uint32_t st = j->st, i = from;
    const uint8_t *p = j->base + (size_t)from * st;
    float *out = j->out;
    (void)worker;
#ifdef __ARM_NEON
    /* Vita: four vertices at a time; reciprocal square root estimate with two Newton steps (~1e-7 relative). */
    {
        float32x4_t w0 = vdupq_n_f32(wv[0]), w1 = vdupq_n_f32(wv[1]), w2 = vdupq_n_f32(wv[2]), w3 = vdupq_n_f32(wv[3]);
        float32x4_t w4 = vdupq_n_f32(wv[4]), w5 = vdupq_n_f32(wv[5]), w6 = vdupq_n_f32(wv[6]), w7 = vdupq_n_f32(wv[7]);
        float32x4_t w8 = vdupq_n_f32(wv[8]);
        float tm[6] = { xf ? m[0] : 1.0f, xf ? m[4] : 0.0f, xf ? m[8] : 0.0f, xf ? m[1] : 0.0f, xf ? m[5] : 1.0f, xf ? m[9] : 0.0f };
        float32x4_t u0 = vdupq_n_f32(xf ? m[12] : 0.0f), v0 = vdupq_n_f32(xf ? m[13] : 0.0f);
        for (; i + 4 <= to; i += 4, p += 4 * st) {
            float nx[4], ny[4], nz[4];
            for (int k = 0; k < 4; k++) { float n[3]; memcpy(n, p + k * st, 12); nx[k] = n[0]; ny[k] = n[1]; nz[k] = n[2]; }
            float32x4_t x = vld1q_f32(nx), y = vld1q_f32(ny), z = vld1q_f32(nz);
            float32x4_t c0 = vmlaq_f32(vmlaq_f32(vmulq_f32(x, w0), y, w3), z, w6);
            float32x4_t c1 = vmlaq_f32(vmlaq_f32(vmulq_f32(x, w1), y, w4), z, w7);
            float32x4_t c2 = vmlaq_f32(vmlaq_f32(vmulq_f32(x, w2), y, w5), z, w8);
            if (norm) {
                float32x4_t l = vmlaq_f32(vmlaq_f32(vmulq_f32(c0, c0), c1, c1), c2, c2);
                uint32x4_t pos = vcgtq_f32(l, vdupq_n_f32(0.0f));
                float32x4_t r = vrsqrteq_f32(l);
                r = vmulq_f32(r, vrsqrtsq_f32(vmulq_f32(l, r), r));
                r = vmulq_f32(r, vrsqrtsq_f32(vmulq_f32(l, r), r));
                r = vbslq_f32(pos, r, vdupq_n_f32(1.0f));
                c0 = vmulq_f32(c0, r); c1 = vmulq_f32(c1, r); c2 = vmulq_f32(c2, r);
            }
            float32x4x2_t uv;
            uv.val[0] = vmlaq_n_f32(vmlaq_n_f32(vmlaq_n_f32(u0, c0, tm[0]), c1, tm[1]), c2, tm[2]);
            uv.val[1] = vmlaq_n_f32(vmlaq_n_f32(vmlaq_n_f32(v0, c0, tm[3]), c1, tm[4]), c2, tm[5]);
            vst2q_f32(out + i * 2, uv);
        }
    }
#endif
    for (; i < to; i++, p += st) {
        float n[3];
        memcpy(n, p, 12);
        float c0 = n[0] * wv[0] + n[1] * wv[3] + n[2] * wv[6];
        float c1 = n[0] * wv[1] + n[1] * wv[4] + n[2] * wv[7];
        float c2 = n[0] * wv[2] + n[1] * wv[5] + n[2] * wv[8];
        if (norm) {
            float l = c0 * c0 + c1 * c1 + c2 * c2;
            if (l > 0.0f) { l = 1.0f / sqrtf(l); c0 *= l; c1 *= l; c2 *= l; }
        }
        out[i * 2] = xf ? c0 * m[0] + c1 * m[4] + c2 * m[8] + m[12] : c0;
        out[i * 2 + 1] = xf ? c0 * m[1] + c1 * m[5] + c2 * m[9] + m[13] : c1;
    }
}

static const float *cpu_texgen_coords(d3d_device *d, int s, uint32_t gen, const vfmt *v, const uint8_t *const sbase[4],
                                      const uint32_t sstride[4], uint32_t total)
{
    if (texgen_cap[s] < total) {
        texgen_cap[s] = total + 1024;
        texgen_buf[s] = realloc(texgen_buf[s], texgen_cap[s] * 8);
    }
    float *out = texgen_buf[s];
    if (gen != D3DTSS_TCI_CAMERASPACENORMAL || v->normal_off < 0 || v->pos_rhw) {
        static int logged;
        if (!logged++) rt_log("[gl] texgen %08x without CPU support (normals %d): constant coordinates\n", gen, v->normal_off >= 0);
        memset(out, 0, (size_t)total * 8);
        return out;
    }
    float a[9], wv[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            a[i * 3 + j] = d->world[i * 4] * d->view[j] + d->world[i * 4 + 1] * d->view[4 + j] + d->world[i * 4 + 2] * d->view[8 + j];
    /* Normals go through the inverse transpose of world * view (as in D3D and GL): cofactors over the determinant. With a scaled
     * model the plain matrix scales the normal, the ramp coordinate runs off the end and wraps (white outlines). */
    wv[0] = a[4] * a[8] - a[5] * a[7]; wv[1] = a[5] * a[6] - a[3] * a[8]; wv[2] = a[3] * a[7] - a[4] * a[6];
    wv[3] = a[2] * a[7] - a[1] * a[8]; wv[4] = a[0] * a[8] - a[2] * a[6]; wv[5] = a[1] * a[6] - a[0] * a[7];
    wv[6] = a[1] * a[5] - a[2] * a[4]; wv[7] = a[2] * a[3] - a[0] * a[5]; wv[8] = a[0] * a[4] - a[1] * a[3];
    float det = a[0] * wv[0] + a[1] * wv[1] + a[2] * wv[2];
    if (det != 0.0f) for (int i = 0; i < 9; i++) wv[i] /= det;
    texgen_job j;
    memcpy(j.wv, wv, sizeof j.wv);
    memcpy(j.m, d->texm[s], sizeof j.m);
    j.xf = (d->tss[s][D3DTSS_TEXTURETRANSFORMFLAGS] & 0xFF) != 0;
    j.norm = d->rs[D3DRS_NORMALIZENORMALS] != 0;
    j.base = sbase[v->normal_stream] + v->normal_off;
    j.st = sstride[v->normal_stream];
    j.out = out;
    par_for(texgen_range, &j, total, 256);
    return out;
}

/* Vita names on the game's Game pad page (game_tmnt.c): while the game draws a "BUTTON n" label (n 1..8) its draws only record
 * their screen box (framebuffer pixels, top left origin); the overlay writes the Vita button there. */
int ovl_capture_slot = -1;
static float ovl_box[9][4];
static uint32_t ovl_box_frame[9];

static void ovl_capture(d3d_device *d, const vfmt *v, const uint8_t *const sbase[4], const uint32_t sstride[4], uint32_t first,
                        uint32_t total)
{
    float wv[16], m[16], *box = ovl_box[ovl_capture_slot];
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) {
            wv[r * 4 + k] = 0;
            for (int i = 0; i < 4; i++) wv[r * 4 + k] += d->world[r * 4 + i] * d->view[i * 4 + k];
        }
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) {
            m[r * 4 + k] = 0;
            for (int i = 0; i < 4; i++) m[r * 4 + k] += wv[r * 4 + i] * d->proj[i * 4 + k];
        }
    if (ovl_box_frame[ovl_capture_slot] != d->frame) {
        box[0] = box[1] = 1e9f; box[2] = box[3] = -1e9f;
        ovl_box_frame[ovl_capture_slot] = d->frame;
    }
    const uint32_t *vp = d->viewport;
    for (uint32_t i = first; i < total; i++) {
        float p[4] = { 0, 0, 0, 1 }, sx, sy;
        memcpy(p, sbase[v->pos_stream] + (size_t)i * sstride[v->pos_stream] + v->pos_off, (size_t)(v->pos_rhw ? 16 : 4 * v->pos_comps));
        if (v->pos_rhw) {
            sx = p[0]; sy = p[1];
        } else {
            float cx = p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12], cy = p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13];
            float cw = p[0] * m[3] + p[1] * m[7] + p[2] * m[11] + m[15];
            if (cw == 0.0f) continue;
            sx = (float)vp[0] + (cx / cw + 1.0f) * 0.5f * (float)vp[2];
            sy = (float)vp[1] + (1.0f - cy / cw) * 0.5f * (float)vp[3];
        }
        float fx = (float)map_ox + sx * map_sx, fy = (float)map_oy + sy * map_sy;
        if (fx < box[0]) box[0] = fx;
        if (fy < box[1]) box[1] = fy;
        if (fx > box[2]) box[2] = fx;
        if (fy > box[3]) box[3] = fy;
    }
}

/* Static vertex and index buffers drawn from GL buffers: uploaded once (colours already RGBA), so neither the colour conversion
 * nor vitaGL's per-draw copy of the arrays runs for them. A buffer qualifies when it is not D3DUSAGE_DYNAMIC and has not been
 * written for two frames; the draw must read it alone (one stream, no RHW positions, no CPU texture coordinates). A buffer written
 * again goes into a new GL buffer, the old one deleted after the frame (refilling one the GPU may still read flickered breakable
 * barrels and crates on the Vita); one drawn with another layout leaves the cache for good. Everything else keeps the client
 * arrays. TMNT_VBO=0 turns it off. */
uint32_t d3d_frame_now;
d3d_buf *draw_vb_obj, *draw_ib_obj;
static uint64_t dt_up_count, dt_up_bytes, dt_up_us;   /* GL buffer uploads, logged with the draw timing */
static int vbo_on = -1;

static int vbo_check = -1;

static int buf_settled(const d3d_buf *b)
{
    return !(b->usage & D3DUSAGE_DYNAMIC) && !b->gl_bad && d3d_frame_now - b->lock_frame >= 2U;
}

static GLuint vbo_for(d3d_buf *b, int diffuse_off, uint32_t stride, uint32_t start)
{
    uint32_t key = diffuse_off >= 0 ? (stride << 16) | ((start + (uint32_t)diffuse_off) % stride + 1U) : stride << 16;
    if (b->gl_bad) return 0;
    if (b->gl_buf && b->gl_version == b->version) {
        if (b->gl_key == key) return b->gl_buf;
        if (vbo_check > 0) rt_log("[vbo] buffer %08x (%u bytes, fvf %x) drawn with another layout: not cached\n", b->mem, b->size, b->fvf);
        b->gl_bad = 1;
        return 0;
    }
    if (!buf_settled(b)) return 0;
    if (b->gl_buf) { gl_defer_delete_buffer(b->gl_buf); b->gl_buf = 0; }   /* written again: a fresh buffer */
    uint64_t t0 = plat_ticks_us();
    uint8_t *tmp = malloc(b->size ? b->size : 4);
    memcpy(tmp, G2H(b->mem), b->size);
    if (vbo_check > 0) { free(b->gl_check); b->gl_check = malloc(b->size ? b->size : 4); memcpy(b->gl_check, tmp, b->size); }
    if (diffuse_off >= 0)
        for (uint32_t o = (start + (uint32_t)diffuse_off) % stride; o + 4 <= b->size; o += stride) {
            uint8_t t = tmp[o]; tmp[o] = tmp[o + 2]; tmp[o + 2] = t;
        }
    if (!b->gl_buf) { GLuint g; glGenBuffers(1, &g); b->gl_buf = g; }
    glBindBuffer(GL_ARRAY_BUFFER, b->gl_buf);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)b->size, tmp, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    free(tmp);
    b->gl_version = b->version;
    b->gl_key = key;
    dt_up_count++; dt_up_bytes += b->size; dt_up_us += plat_ticks_us() - t0;
    return b->gl_buf;
}

static GLuint ibo_for(d3d_buf *b)
{
    if (b->gl_buf && b->gl_version == b->version) return b->gl_buf;
    if (!buf_settled(b)) return 0;
    if (b->gl_buf) { gl_defer_delete_buffer(b->gl_buf); b->gl_buf = 0; }   /* written again: a fresh buffer */
    uint64_t t0 = plat_ticks_us();
    if (!b->gl_buf) { GLuint g; glGenBuffers(1, &g); b->gl_buf = g; }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b->gl_buf);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)b->size, G2H(b->mem), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    b->gl_version = b->version;
    dt_up_count++; dt_up_bytes += b->size; dt_up_us += plat_ticks_us() - t0;
    return b->gl_buf;
}

/* CPU texture coordinates in any enabled stage (they change with the camera): such a draw keeps the client arrays. */
static int draw_needs_texgen(d3d_device *d)
{
    for (int s = 0; s < 4 && d->tss[s][D3DTSS_COLOROP] != D3DTOP_DISABLE; s++)
        if (d->tss[s][D3DTSS_TEXCOORDINDEX] & 0xFFFF0000U) return 1;
    return 0;
}

/* TMNT_PERF=1: where a draw's host time goes (states and positions, colours, stages and texture coordinates, the vitaGL call)
 * and how many vertices it converts; logged with the frame line. draw_min_index is the D3D MinIndex of an indexed draw. */
static int draw_timing = -1;
static uint64_t dt_us[4], dt_vertices, dt_prefix, dt_indices, dt_cached_vertices, dt_cached_draws;
uint32_t draw_min_index;

static void draw(d3d_device *d, uint32_t prim, uint32_t vbase, uint32_t stride0, uint32_t first_vertex, uint32_t num_vertices,
                 const void *indices, int index32, uint32_t index_count);
void gl_draw(d3d_device *d, uint32_t prim, uint32_t vbase, uint32_t stride0, uint32_t first_vertex, uint32_t num_vertices,
             const void *indices, int index32, uint32_t index_count)
{
    uint64_t t0 = plat_ticks_us();
    draw(d, prim, vbase, stride0, first_vertex, num_vertices, indices, index32, index_count);
    prof_draw_us += plat_ticks_us() - t0;
    prof_draws++;
}

static void draw(d3d_device *d, uint32_t prim, uint32_t vbase, uint32_t stride0, uint32_t first_vertex, uint32_t num_vertices,
             const void *indices, int index32, uint32_t index_count)
{
    vfmt v;
    if (d->decl) parse_decl(d->decl, &v); else parse_fvf(d->fvf, &v);
    if (dump_frame == -2) dump_frame = getenv("TMNT_DUMP_FRAME") ? atoi(getenv("TMNT_DUMP_FRAME")) : -1;
    if ((int)d->frame == dump_frame) {
        d3d_tex *t0 = d->tex[0];
        rt_log("[draw] prim %u n %u idx %u stride %u %s fvf %x rhw %d tex0 %s%ux%u fmt %x op %u/%u vp %u,%u %ux%u light %u zen %u blend %u atest %u cull %u\n",
               prim, num_vertices, index_count, stride0, d->decl ? "decl" : "fvf", d->fvf, v.pos_rhw, t0 ? "" : "none ", t0 ? t0->w : 0,
               t0 ? t0->h : 0, t0 ? t0->fmt : 0, d->tss[0][D3DTSS_COLOROP], d->tss[0][D3DTSS_ALPHAOP], d->viewport[0], d->viewport[1],
               d->viewport[2], d->viewport[3], d->rs[D3DRS_LIGHTING], d->rs[D3DRS_ZENABLE], d->rs[D3DRS_ALPHABLENDENABLE],
               d->rs[D3DRS_ALPHATESTENABLE], d->rs[D3DRS_CULLMODE]);
        const float *mt = d->material;
        rt_log("[draw]   stages: 0 %u(%x,%x)/%u(%x,%x) tci %x  1 %u(%x,%x)/%u  tex1 %s; tfactor %08x colorvertex %u diffuse src %u ambient src %u "
               "emissive src %u fog %u; material diffuse %.2f %.2f %.2f %.2f ambient %.2f %.2f %.2f emissive %.2f %.2f %.2f; normals %d colors %d\n",
               d->tss[0][D3DTSS_COLOROP], d->tss[0][D3DTSS_COLORARG1], d->tss[0][D3DTSS_COLORARG2], d->tss[0][D3DTSS_ALPHAOP],
               d->tss[0][D3DTSS_ALPHAARG1], d->tss[0][D3DTSS_ALPHAARG2], d->tss[0][D3DTSS_TEXCOORDINDEX], d->tss[1][D3DTSS_COLOROP],
               d->tss[1][D3DTSS_COLORARG1], d->tss[1][D3DTSS_COLORARG2], d->tss[1][D3DTSS_ALPHAOP], d->tex[1] ? "yes" : "no",
               d->rs[D3DRS_TEXTUREFACTOR], d->rs[D3DRS_COLORVERTEX], d->rs[D3DRS_DIFFUSEMATERIALSOURCE], d->rs[D3DRS_AMBIENTMATERIALSOURCE],
               d->rs[D3DRS_EMISSIVEMATERIALSOURCE], d->rs[D3DRS_FOGENABLE], mt[0], mt[1], mt[2], mt[3], mt[4], mt[5], mt[6], mt[12], mt[13], mt[14],
               v.normal_off >= 0, v.diffuse_off >= 0);
        for (int st = 0; st < 2; st++) {
            const float *tm = d->texm[st];
            rt_log("[draw]   stage %d: tci %x ttff %x addr %u/%u filter %u/%u/%u tex %s%ux%u fmt %x texm %.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f\n",
                   st, d->tss[st][D3DTSS_TEXCOORDINDEX], d->tss[st][D3DTSS_TEXTURETRANSFORMFLAGS], d->ss[st][D3DSAMP_ADDRESSU],
                   d->ss[st][D3DSAMP_ADDRESSV], d->ss[st][D3DSAMP_MINFILTER], d->ss[st][D3DSAMP_MAGFILTER], d->ss[st][D3DSAMP_MIPFILTER],
                   d->tex[st] ? "" : "none ", d->tex[st] ? d->tex[st]->w : 0, d->tex[st] ? d->tex[st]->h : 0, d->tex[st] ? d->tex[st]->fmt : 0,
                   tm[0], tm[1], tm[2], tm[4], tm[5], tm[6], tm[8], tm[9], tm[10], tm[12], tm[13], tm[14]);
        }
        static int dumped1;
        d3d_tex *t1 = d->tex[1];
        if (t1 && !d3d_format_is_dxt(t1->fmt) && dumped1 < 4 && (d->tss[1][D3DTSS_TEXCOORDINDEX] & 0xFFFF0000U)) {
            uint8_t *px = malloc((size_t)t1->w * t1->h * 4);
            expand_level(t1->fmt, G2H(t1->mem[0]), t1->pitch[0], t1->w, t1->h, px);
            char name[64];
            snprintf(name, sizeof name, "tex1_%02d_%ux%u.png", dumped1++, t1->w, t1->h);
            plat_write_rgba_png(name, px, (int)t1->w, (int)t1->h);
            free(px);
        }
        if (v.pos_rhw && !indices) {
            const uint8_t *vb = G2H(vbase);
            for (uint32_t k = first_vertex; k < first_vertex + num_vertices && k < first_vertex + 4; k++) {
                float f[4], uv[2] = { 0, 0 };
                uint32_t col = v.diffuse_off >= 0 ? *(const uint32_t *)(vb + k * stride0 + (uint32_t)v.diffuse_off) : 0;
                memcpy(f, vb + k * stride0 + (uint32_t)v.pos_off, 16);
                if (v.tex_count) memcpy(uv, vb + k * stride0 + (uint32_t)v.tex_off[0], 8);
                rt_log("[draw]   v%u pos %.1f %.1f %.3f rhw %.3f col %08x uv %.3f %.3f\n", k, f[0], f[1], f[2], f[3], col, uv[0], uv[1]);
            }
        }
        static int dumped;
        if (t0 && !d3d_format_is_dxt(t0->fmt) && dumped < 12) {
            uint8_t *px = malloc((size_t)t0->w * t0->h * 4);
            expand_level(t0->fmt, G2H(t0->mem[0]), t0->pitch[0], t0->w, t0->h, px);
            char name[64];
            snprintf(name, sizeof name, "tex_%02d_%ux%u.png", dumped++, t0->w, t0->h);
            plat_write_rgba_png(name, px, (int)t0->w, (int)t0->h);
            free(px);
        }
    }
    if (stride0 == 0) return;
    const uint8_t *base0 = (const uint8_t *)G2H(vbase);
    const uint8_t *sbase[4] = { base0, base0, base0, base0 };
    uint32_t sstride[4] = { stride0, stride0, stride0, stride0 };
    if (d->decl) {
        for (int s = 1; s < 4; s++)
            if (d->stream[s].vb) { sbase[s] = (const uint8_t *)G2H(d->stream[s].vb->mem + d->stream[s].offset); sstride[s] = d->stream[s].stride; }
    }
    /* A non-indexed draw from the middle of a buffer starts every stream at its first vertex, so only the drawn vertices are
     * converted (colours, positions, generated coordinates) and handed to GL; dynamic buffers otherwise cost their whole prefix. */
    if (!indices && first_vertex) {
        for (int s = 0; s < 4; s++) sbase[s] += (size_t)first_vertex * sstride[s];
        first_vertex = 0;
    }
    uint32_t total = first_vertex + num_vertices;
    if (ovl_capture_slot > 0) { ovl_capture(d, &v, sbase, sstride, first_vertex, total); return; }
    ensure_scratch(total);
    if (draw_timing < 0) draw_timing = getenv("TMNT_PERF") && atoi(getenv("TMNT_PERF"));
    uint64_t ta = draw_timing ? plat_ticks_us() : 0, tb = 0, tc = 0;
    if (draw_timing) { dt_vertices += total; dt_prefix += draw_min_index; dt_indices += index_count; }
    /* GL buffers for a static vertex buffer (and index buffer) read alone; pbase is then the offset into the GL buffer. */
    if (vbo_on < 0) {
        vbo_on = getenv("TMNT_VBO") ? atoi(getenv("TMNT_VBO")) : 1;
        vbo_check = getenv("TMNT_VBO_CHECK") && atoi(getenv("TMNT_VBO_CHECK"));
    }
    GLuint vb_gl = 0, ib_gl = 0;
    const uint8_t *pbase[4] = { sbase[0], sbase[1], sbase[2], sbase[3] };
    if (vbo_on && draw_vb_obj && !v.pos_rhw && sbase[1] == sbase[0] && sbase[2] == sbase[0] && sbase[3] == sbase[0] &&
        sstride[1] == sstride[0] && sstride[2] == sstride[0] && sstride[3] == sstride[0] && !draw_needs_texgen(d)) {
        uint32_t start = (uint32_t)(sbase[0] - (const uint8_t *)G2H(draw_vb_obj->mem));
        vb_gl = vbo_for(draw_vb_obj, v.diffuse_off >= 0 ? v.diffuse_off : -1, sstride[0], start);
        if (vb_gl && vbo_check > 0 && draw_vb_obj->gl_check) {
            uint32_t len = total * sstride[0];
            if (start + len > draw_vb_obj->size) len = draw_vb_obj->size - start;
            if (memcmp(draw_vb_obj->gl_check + start, G2H(draw_vb_obj->mem + start), len)) {
                static int logged;
                if (logged++ < 20)
                    rt_log("[vbo] buffer %08x (%u bytes, usage %x, fvf %x, version %u) changed without a Lock: bytes %u..%u, stride %u\n",
                           draw_vb_obj->mem, draw_vb_obj->size, draw_vb_obj->usage, draw_vb_obj->fvf, draw_vb_obj->version, start,
                           start + len, sstride[0]);
                memcpy(draw_vb_obj->gl_check + start, G2H(draw_vb_obj->mem + start), len);
            }
        }
        if (vb_gl) {
            for (int k = 0; k < 4; k++) pbase[k] = (const uint8_t *)(uintptr_t)start;
            if (indices && draw_ib_obj) ib_gl = ibo_for(draw_ib_obj);
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, vb_gl);
    set_viewport(d);
    apply_render_states(d, v.pos_rhw);
    if (v.pos_rhw) {
        static uint32_t rhw_vp[4];
        if (xf_valid != 2 || memcmp(rhw_vp, d->viewport, sizeof rhw_vp)) {
            xf_valid = 2;
            memcpy(rhw_vp, d->viewport, sizeof rhw_vp);
            glMatrixMode(GL_PROJECTION);
            glLoadIdentity();
            glOrtho(0, d->viewport[2], d->viewport[3], 0, 0, -1);
            glTranslatef(0.5f - (float)d->viewport[0], 0.5f - (float)d->viewport[1], 0);
            glMatrixMode(GL_MODELVIEW);
            glLoadIdentity();
        }
        const uint8_t *p = sbase[v.pos_stream] + v.pos_off;
        for (uint32_t i = 0; i < total; i++, p += sstride[v.pos_stream]) {
            float f[4];
            memcpy(f, p, 16);
            float w = f[3] != 0.0f ? 1.0f / f[3] : 1.0f;
            positions[i * 4 + 0] = f[0] * w; positions[i * 4 + 1] = f[1] * w; positions[i * 4 + 2] = f[2] * w; positions[i * 4 + 3] = w;
        }
        glVertexPointer(4, GL_FLOAT, 16, positions);
        glDisable(GL_LIGHTING);
    } else {
        load_projection(d);
        glVertexPointer(v.pos_comps, GL_FLOAT, (GLsizei)sstride[v.pos_stream], pbase[v.pos_stream] + v.pos_off);
        apply_lighting(d, v.normal_off >= 0, v.diffuse_off >= 0, v.specular_off >= 0);
    }
    glEnableClientState(GL_VERTEX_ARRAY);
    if (v.normal_off >= 0 && !v.pos_rhw) {
        glEnableClientState(GL_NORMAL_ARRAY);
        glNormalPointer(GL_FLOAT, (GLsizei)sstride[v.normal_stream], pbase[v.normal_stream] + v.normal_off);
        sc_valid[SC_NORMAL] = 0;
    } else {
        glDisableClientState(GL_NORMAL_ARRAY);
        glNormal3f(0, 0, 0);
    }
    if (draw_timing) tb = plat_ticks_us();
    if (v.diffuse_off >= 0 && vb_gl) {
        glEnableClientState(GL_COLOR_ARRAY);
        glColorPointer(4, GL_UNSIGNED_BYTE, (GLsizei)sstride[v.diffuse_stream], pbase[v.diffuse_stream] + v.diffuse_off);
        sc_valid[SC_COLOR] = 0;
    } else if (v.diffuse_off >= 0) {
        bgra_to_rgba(sbase[v.diffuse_stream] + v.diffuse_off, sstride[v.diffuse_stream], total, colors);
        glEnableClientState(GL_COLOR_ARRAY);
        glColorPointer(4, GL_UNSIGNED_BYTE, 4, colors);
        sc_valid[SC_COLOR] = 0;
    } else {
        glDisableClientState(GL_COLOR_ARRAY);
        glColor4f(1, 1, 1, 1);
    }
    if (draw_timing) tc = plat_ticks_us();
    int units = apply_stages(d, v.tex_count, v.diffuse_off < 0 && (v.pos_rhw || !d->rs[D3DRS_LIGHTING]));
    /* Texture coordinate arrays: only units in use, plus those left enabled by an earlier draw. vitaGL has fewer client units than 4,
     * and glClientActiveTexture on a missing one switched off unit 0's coordinates (every textured draw then read one texel: white screen). */
    for (int s = 0; s < 4; s++) {
        uint32_t gen = s < units ? d->tss[s][D3DTSS_TEXCOORDINDEX] & 0xFFFF0000U : 0;
        const float *gen_uv = gen && cpu_texgen ? cpu_texgen_coords(d, s, gen, &v, sbase, sstride, total) : NULL;
        int set = s < units ? (int)(d->tss[s][D3DTSS_TEXCOORDINDEX] & 0xFFFF) : -1;
        int want = gen_uv || (set >= 0 && set < v.tex_count && !gen);
        if (!want && !tc_enabled[s]) continue;
        glClientActiveTexture(GL_TEXTURE0 + (GLenum)s);
        if (gen_uv) {
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glTexCoordPointer(2, GL_FLOAT, 8, gen_uv);
        } else if (want) {
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glTexCoordPointer(v.tex_size[set], GL_FLOAT, (GLsizei)sstride[v.tex_stream[set]], pbase[v.tex_stream[set]] + v.tex_off[set]);
        } else {
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        }
        tc_enabled[s] = want;
    }
    glClientActiveTexture(GL_TEXTURE0);
    uint64_t td = draw_timing ? plat_ticks_us() : 0;
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib_gl);
    if (ib_gl) {
        glDrawElements(gl_prim(prim), (GLsizei)index_count, index32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT,
                       (const void *)(uintptr_t)((const uint8_t *)indices - (const uint8_t *)G2H(draw_ib_obj->mem)));
    } else if (indices) {
        glDrawElements(gl_prim(prim), (GLsizei)index_count, index32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, indices);
    } else {
        glDrawArrays(gl_prim(prim), (GLint)first_vertex, (GLsizei)num_vertices);
    }
    if (draw_timing && vb_gl) { dt_cached_vertices += total; dt_cached_draws++; }
    if (draw_timing) {
        uint64_t te = plat_ticks_us();
        dt_us[0] += tb - ta; dt_us[1] += tc - tb; dt_us[2] += td - tc; dt_us[3] += te - td;
    }
}

void gl_clear(d3d_device *d, uint32_t flags, uint32_t color, float z, uint32_t stencil, uint32_t count, uint32_t rects)
{
    (void)count; (void)rects;
    GLbitfield m = 0;
    set_viewport(d);
    glDisable(GL_SCISSOR_TEST);
    if (flags & D3DCLEAR_TARGET) {
        float c[4];
        color4(color, c);
        glClearColor(c[0], c[1], c[2], c[3]);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        m |= GL_COLOR_BUFFER_BIT;
    }
    if (flags & D3DCLEAR_ZBUFFER) { glClearDepth(z); glDepthMask(GL_TRUE); m |= GL_DEPTH_BUFFER_BIT; }
    if (flags & D3DCLEAR_STENCIL) { glClearStencil((GLint)stencil); glStencilMask(0xFF); m |= GL_STENCIL_BUFFER_BIT; }
    uint32_t *vp = d->viewport;
    glEnable(GL_SCISSOR_TEST);
    GLint r[4];
    map_rect(d, vp[0], vp[1], vp[2], vp[3], r);
    glScissor(r[0], r[1], r[2], r[3]);
    glClear(m);
    glDisable(GL_SCISSOR_TEST);
}

void gl_set_render_target(d3d_device *d, d3d_surf *s)
{
    static int warned;
    if (s && !s->is_backbuffer && !warned++)
        rt_log("[d3d] render targets other than the back buffer are not implemented yet\n");
    d->rt0 = s;
}

static GLuint *pending_deletes;
static size_t pending_count, pending_cap;
static pthread_mutex_t pending_lock = PTHREAD_MUTEX_INITIALIZER;

static GLuint *pending_buf_deletes;
static size_t pending_buf_count, pending_buf_cap;

void gl_defer_delete_buffer(uint32_t buf)
{
    pthread_mutex_lock(&pending_lock);
    if (pending_buf_count == pending_buf_cap) {
        pending_buf_cap = pending_buf_cap ? pending_buf_cap * 2 : 64;
        pending_buf_deletes = realloc(pending_buf_deletes, pending_buf_cap * sizeof *pending_buf_deletes);
    }
    pending_buf_deletes[pending_buf_count++] = buf;
    pthread_mutex_unlock(&pending_lock);
}

void gl_defer_delete(GLuint tex)
{
    pthread_mutex_lock(&pending_lock);
    if (pending_count == pending_cap) {
        pending_cap = pending_cap ? pending_cap * 2 : 256;
        pending_deletes = realloc(pending_deletes, pending_cap * sizeof *pending_deletes);
    }
    pending_deletes[pending_count++] = tex;
    pthread_mutex_unlock(&pending_lock);
}

/* Vita button names drawn over the game's Controller settings > Game pad page in place of its "BUTTON n" labels (the game numbers
 * joystick buttons from 1): the PlayStation symbols in their colours for 1-4, L, SELECT, R and L + R for 5-8. */
static GLuint ovl_tex;

static void ovl_quads(const float *pos, const float *uv, int n_quads)
{
    static float p[6 * 2 * 64], t[6 * 2 * 64];
    for (int q = 0; q < n_quads; q++) {
        static const int order[6] = { 0, 1, 2, 2, 1, 3 };
        for (int k = 0; k < 6; k++) {
            int v = order[k];
            p[(q * 6 + k) * 2] = pos[q * 8 + v * 2]; p[(q * 6 + k) * 2 + 1] = pos[q * 8 + v * 2 + 1];
            if (uv) { t[(q * 6 + k) * 2] = uv[q * 8 + v * 2]; t[(q * 6 + k) * 2 + 1] = uv[q * 8 + v * 2 + 1]; }
        }
    }
    glVertexPointer(2, GL_FLOAT, 0, p);
    if (uv) glTexCoordPointer(2, GL_FLOAT, 0, t);
    glDrawArrays(GL_TRIANGLES, 0, n_quads * 6);
}

static void ovl_rounded_rect(float x, float y, float w, float h, float r)
{
    /* Centre and side bands as quads, corners as fans of thin triangles. */
    float bands[3 * 8] = { x + r, y, x + w - r, y, x + r, y + h, x + w - r, y + h,
                           x, y + r, x + r, y + r, x, y + h - r, x + r, y + h - r,
                           x + w - r, y + r, x + w, y + r, x + w - r, y + h - r, x + w, y + h - r };
    ovl_quads(bands, NULL, 3);
    static float fan[4][3 * 2 * 8];
    const float cx[4] = { x + r, x + w - r, x + r, x + w - r }, cy[4] = { y + r, y + r, y + h - r, y + h - r };
    const float a0[4] = { 3.14159f, 4.71239f, 1.5708f, 0.0f };
    for (int c = 0; c < 4; c++) {
        for (int i = 0; i < 8; i++) {
            float t0 = a0[c] + 1.5708f * i / 8.0f, t1 = a0[c] + 1.5708f * (i + 1) / 8.0f;
            float *f = fan[c] + i * 6;
            f[0] = cx[c]; f[1] = cy[c];
            f[2] = cx[c] + r * cosf(t0); f[3] = cy[c] + r * sinf(t0);
            f[4] = cx[c] + r * cosf(t1); f[5] = cy[c] + r * sinf(t1);
        }
        glVertexPointer(2, GL_FLOAT, 0, fan[c]);
        glDrawArrays(GL_TRIANGLES, 0, 24);
    }
}

/* Text: printable ASCII; '\1'..'\4' are the cross, circle, square and triangle symbols in their PlayStation colours. */
static float ovl_text(float x, float y, float scale, const char *s, const float rgba[4])
{
    static const float sym[4][3] = { { 0.49f, 0.70f, 0.98f }, { 1.0f, 0.40f, 0.40f }, { 0.95f, 0.55f, 0.85f }, { 0.35f, 0.88f, 0.69f } };
    float cw = OVL_CELL_W * scale, ch = OVL_CELL_H * scale;
    for (; *s; s++) {
        int g = (unsigned char)*s >= 1 && (unsigned char)*s <= 4 ? 94 + (unsigned char)*s : (unsigned char)*s - 32;
        float adv = cw * (g >= 95 ? 1.1f : 0.6f);   /* Menlo glyphs are ~0.6 of the cell wide */
        if (g < 0 || g > 98 || *s == ' ') { x += adv; continue; }
        if (g >= 95) glColor4f(sym[g - 95][0], sym[g - 95][1], sym[g - 95][2], rgba[3]);
        else glColor4f(rgba[0], rgba[1], rgba[2], rgba[3]);
        float u0 = (float)(g % OVL_COLS * OVL_CELL_W) / 256.0f, v0 = (float)(g / OVL_COLS * OVL_CELL_H) / 256.0f;
        float u1 = u0 + (float)OVL_CELL_W / 256.0f, v1 = v0 + (float)OVL_CELL_H / 256.0f;
        float pos[8] = { x, y, x + cw, y, x, y + ch, x + cw, y + ch }, uv[8] = { u0, v0, u1, v0, u0, v1, u1, v1 };
        ovl_quads(pos, uv, 1);
        x += adv;
    }
    return x;
}

static void draw_vita_overlay(uint32_t frame)
{
    if (!ovl_tex) {
        static uint8_t rgba[256 * 256 * 4];
        for (int y = 0; y < OVL_FONT_H; y++)
            for (int x = 0; x < OVL_FONT_W; x++) {
                uint8_t *q = rgba + ((size_t)y * 256 + (size_t)x) * 4;
                q[0] = q[1] = q[2] = 255; q[3] = ovl_font[y * OVL_FONT_W + x];
            }
        glGenTextures(1, &ovl_tex);
        glBindTexture(GL_TEXTURE_2D, ovl_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    float W = (float)map_fbw, H = (float)map_fbh, scale = H / 544.0f;
    glViewport(0, 0, map_fbw, map_fbh);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_ALPHA_TEST); glDisable(GL_FOG); glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE); glDisable(GL_STENCIL_TEST); glDisable(GL_POLYGON_OFFSET_FILL);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glBlendEquation(GL_FUNC_ADD);
    xf_forget();
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, W, H, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    for (int u = 1; u < 4; u++) { glActiveTexture(GL_TEXTURE0 + (GLenum)u); glDisable(GL_TEXTURE_2D); }
    glActiveTexture(GL_TEXTURE0);
    glMatrixMode(GL_TEXTURE); glLoadIdentity(); glMatrixMode(GL_MODELVIEW);
    glEnableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_NORMAL_ARRAY);
    for (int u = 1; u < 4; u++)
        if (tc_enabled[u]) { glClientActiveTexture(GL_TEXTURE0 + (GLenum)u); glDisableClientState(GL_TEXTURE_COORD_ARRAY); tc_enabled[u] = 0; }
    glClientActiveTexture(GL_TEXTURE0);

    /* The game's "BUTTON n" labels (captured above) become the Vita buttons, at the label's place and height. */
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, ovl_tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    static const float white[4] = { 1, 1, 1, 1 };
    static const char *names[9] = { "", "\1", "\2", "\3", "\4", "L", "SELECT", "R", "L + R" };
    (void)scale;
    for (int n = 1; n <= 8; n++) {
        const float *box = ovl_box[n];
        if (ovl_box_frame[n] != frame || box[2] <= box[0]) continue;
        float h = box[3] - box[1], sc = h / (float)OVL_CELL_H * (n <= 4 ? 1.6f : 1.25f);   /* the symbols draw smaller than letters */
        ovl_text(box[0], box[1] - h * (n <= 4 ? 0.35f : 0.15f), sc, names[n], white);
    }

    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    tc_enabled[0] = 0;
    glColor4f(1, 1, 1, 1);
}

void gl_present(d3d_device *d)
{
    pthread_mutex_lock(&pending_lock);
    if (pending_count) {
        glDeleteTextures((GLsizei)pending_count, pending_deletes);
        pending_count = 0;
    }
    if (pending_buf_count) {
        glBindBuffer(GL_ARRAY_BUFFER, 0);   /* vitaGL keeps the bound one by address */
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glDeleteBuffers((GLsizei)pending_buf_count, pending_buf_deletes);
        pending_buf_count = 0;
    }
    pthread_mutex_unlock(&pending_lock);
    d3d_frame_now = d->frame;
    /* A file dump.req in the game directory asks the running game for a screenshot now and a draw dump of the next frame. */
    if (d->frame % 30 == 0 && access("dump.req", F_OK) == 0) {
        remove("dump.req");
        shot_frame = (int)d->frame;
        dump_frame = (int)d->frame + 1;
        rt_log("[d3d] dump requested at frame %u (render target %s)\n", d->frame, d->rt0 && !d->rt0->is_backbuffer ? "offscreen" : "back buffer");
        for (int r = 0; r < 4; r++)
            rt_log("[d3d]   view %9.3f %9.3f %9.3f %9.3f   proj %9.3f %9.3f %9.3f %9.3f   world %9.3f %9.3f %9.3f %9.3f\n",
                   d->view[r * 4], d->view[r * 4 + 1], d->view[r * 4 + 2], d->view[r * 4 + 3], d->proj[r * 4], d->proj[r * 4 + 1],
                   d->proj[r * 4 + 2], d->proj[r * 4 + 3], d->world[r * 4], d->world[r * 4 + 1], d->world[r * 4 + 2], d->world[r * 4 + 3]);
    }
    /* TMNT_MEMDUMP=f1,f2,...: the guest's global data (0x580000-0x900000) saved as mem_<frame>.bin at those frames. */
    {
        static int parsed;
        static uint32_t dump_at[8], ndump;
        if (!parsed) {
            parsed = 1;
            const char *e = getenv("TMNT_MEMDUMP");
            while (e && *e && ndump < 8) { dump_at[ndump++] = (uint32_t)strtoul(e, (char **)&e, 10); if (*e == ',') e++; }
        }
        for (uint32_t i = 0; i < ndump; i++)
            if (dump_at[i] == d->frame) {
                char name[40];
                snprintf(name, sizeof name, "mem_%05u.bin", d->frame);
                FILE *f = fopen(name, "wb");
                if (f) { fwrite(G2H(0x580000U), 1, 0x380000U, f); fclose(f); }
            }
    }
    if (tmnt_pad_settings_active()) draw_vita_overlay(d->frame);
    if ((int)d->frame == shot_frame || (shot_every && d->frame % (uint32_t)shot_every == 0)) {
        char name[64];
        snprintf(name, sizeof name, "frame_%04u.png", d->frame);
        plat_screenshot(name);
    }
    uint64_t swap0 = plat_ticks_us();
    plat_swap();
    prof_swap_us += plat_ticks_us() - swap0;
    if (map_ox || map_oy) {
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    d->frame++;
    uint64_t now = plat_ticks_us();
    if (d->frame % 300 == 0) {
        extern uint64_t d3d_tex_bytes, d3d_buf_bytes;
        extern uint32_t d3d_tex_count;
        rt_log("[d3d] frame %u, %.1f fps, guest heap %u MB, live %u MB (textures %u: %llu MB, buffers %llu MB)\n", d->frame,
               last_present_us ? 1e6 / (double)(now - last_present_us) : 0.0, rt_heap_used() >> 20, rt_heap_live() >> 20, d3d_tex_count,
               (unsigned long long)(d3d_tex_bytes >> 20), (unsigned long long)(d3d_buf_bytes >> 20));
        double frames = 300.0, total = prof_frame_start ? (double)(now - prof_frame_start) / frames / 1000.0 : 0.0;
        double dr = (double)prof_draw_us / frames / 1000.0, sw = (double)prof_swap_us / frames / 1000.0;
        rt_log("[prof] per frame: %.1f ms = draws %.1f ms (%u draws, uploads %.1f ms, %u textures %llu KB) + swap %.1f ms + game %.1f ms\n",
               total, dr, prof_draws / 300U, (double)prof_upload_us / frames / 1000.0, prof_uploads / 300U,
               (unsigned long long)(prof_upload_bytes / 300U / 1024U), sw, total - dr - sw);
        rt_prof_report(300);
        if (draw_timing > 0) {
            rt_log("[draw] per frame: states+positions %.2f ms, colours %.2f ms, stages+texcoords %.2f ms, vitaGL %.2f ms; %llu vertices "
                   "(%llu before MinIndex, %llu from GL buffers in %llu draws), %llu indices\n", dt_us[0] / 300.0 / 1000.0,
                   dt_us[1] / 300.0 / 1000.0, dt_us[2] / 300.0 / 1000.0, dt_us[3] / 300.0 / 1000.0, (unsigned long long)(dt_vertices / 300U),
                   (unsigned long long)(dt_prefix / 300U), (unsigned long long)(dt_cached_vertices / 300U),
                   (unsigned long long)(dt_cached_draws / 300U), (unsigned long long)(dt_indices / 300U));
            rt_log("[draw] GL buffer uploads per frame: %.2f (%.1f KB, %.2f ms)\n", dt_up_count / 300.0, dt_up_bytes / 300.0 / 1024.0,
                   dt_up_us / 300.0 / 1000.0);
            dt_up_count = dt_up_bytes = dt_up_us = 0;
            memset(dt_us, 0, sizeof dt_us);
            dt_vertices = dt_prefix = dt_indices = dt_cached_vertices = dt_cached_draws = 0;
        }
        prof_draw_us = prof_upload_us = prof_swap_us = prof_upload_bytes = 0;
        prof_draws = prof_uploads = 0;
        prof_frame_start = now;
    }
    last_present_us = now;
    GLenum e = glGetError();
    if (e != GL_NO_ERROR) {
        static int n;
        if (n++ < 20) rt_log("[gl] error 0x%x before frame %u\n", e, d->frame);
    }
    if (!plat_pump()) {
        rt_log("[plat] quit requested\n");
        exit(0);
    }
}
