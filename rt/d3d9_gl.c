/* OpenGL backend of the Direct3D 9 device: fixed-function state, vertex formats, textures, draws, present. */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "d3d9.h"

static GLuint white_tex;
static uint8_t *colors, *speculars;
static float *positions;
static size_t scratch_vertices;
static int shot_frame = -1, shot_every;
static int dump_frame = -2;
static uint64_t last_present_us;

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
    glViewport(0, 0, (GLsizei)d->width, (GLsizei)d->height);
    const char *s = getenv("TMNT_SHOT_FRAME");
    if (s) shot_frame = atoi(s);
    s = getenv("TMNT_SHOT_EVERY");
    if (s) shot_every = atoi(s);
}

/* ------------------------------------------------------------ textures --- */

static void expand_level(uint32_t fmt, const uint8_t *src, uint32_t pitch, uint32_t w, uint32_t h, uint8_t *dst)
{
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

void gl_upload_texture(d3d_tex *t)
{
    if (!t->gl) {
        glGenTextures(1, &t->gl);
        glBindTexture(GL_TEXTURE_2D, t->gl);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)t->levels - 1);
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
            size_t need = (size_t)w * h * 4;
            if (need > tmp_size) { tmp_size = need; tmp = realloc(tmp, tmp_size); }
            expand_level(t->fmt, src, t->pitch[l], w, h, tmp);
            glTexImage2D(GL_TEXTURE_2D, (GLint)l, GL_RGBA, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
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

static GLint arg_source(uint32_t a, int stage)
{
    switch (a & 0xF) {
    case D3DTA_DIFFUSE: return GL_PRIMARY_COLOR;
    case D3DTA_CURRENT: return stage == 0 ? GL_PRIMARY_COLOR : GL_PREVIOUS;
    case D3DTA_TEXTURE: return GL_TEXTURE;
    case D3DTA_TFACTOR: return GL_CONSTANT;
    default: return GL_PRIMARY_COLOR;
    }
}

static GLint arg_operand(uint32_t a, int alpha)
{
    int comp = (a & D3DTA_COMPLEMENT) != 0, rep = (a & D3DTA_ALPHAREPLICATE) != 0;
    if (alpha || rep) return comp ? GL_ONE_MINUS_SRC_ALPHA : GL_SRC_ALPHA;
    return comp ? GL_ONE_MINUS_SRC_COLOR : GL_SRC_COLOR;
}

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
static int apply_stages(d3d_device *d, int texcoords)
{
    float tf[4];
    color4(d->rs[D3DRS_TEXTUREFACTOR], tf);
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
        glMatrixMode(GL_TEXTURE);
        if (t[D3DTSS_TEXTURETRANSFORMFLAGS] & 0xFF) glLoadMatrixf(d->texm[s]); else glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
        if (gen) {
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
        (void)texcoords;
        unit = s + 1;
    }
    for (int s = unit; s < 4; s++) {
        glActiveTexture(GL_TEXTURE0 + (GLenum)s);
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_TEXTURE_GEN_S); glDisable(GL_TEXTURE_GEN_T); glDisable(GL_TEXTURE_GEN_R);
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
    case D3DBLEND_BLENDFACTOR: return GL_CONSTANT_COLOR;
    case D3DBLEND_INVBLENDFACTOR: return GL_ONE_MINUS_CONSTANT_COLOR;
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
        glScissor((GLint)d->scissor[0], (GLint)(d->height - d->scissor[3]), (GLsizei)(d->scissor[2] - d->scissor[0]), (GLsizei)(d->scissor[3] - d->scissor[1]));
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
    float amb[4];
    color4(rs[D3DRS_AMBIENT], amb);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb);
    glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, rs[D3DRS_LOCALVIEWER] ? GL_TRUE : GL_FALSE);
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
    glViewport((GLint)vp[0], (GLint)(d->height - vp[1] - vp[3]), (GLsizei)vp[2], (GLsizei)vp[3]);
    glDepthRange(u2f(vp[4]), u2f(vp[5]));
}

void gl_draw(d3d_device *d, uint32_t prim, uint32_t vbase, uint32_t stride0, uint32_t first_vertex, uint32_t num_vertices,
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
    uint32_t total = first_vertex + num_vertices;
    ensure_scratch(total);
    const uint8_t *base0 = (const uint8_t *)G2H(vbase);
    const uint8_t *sbase[4] = { base0, base0, base0, base0 };
    uint32_t sstride[4] = { stride0, stride0, stride0, stride0 };
    if (d->decl) {
        for (int s = 1; s < 4; s++)
            if (d->stream[s].vb) { sbase[s] = (const uint8_t *)G2H(d->stream[s].vb->mem + d->stream[s].offset); sstride[s] = d->stream[s].stride; }
    }
    set_viewport(d);
    apply_render_states(d, v.pos_rhw);
    if (v.pos_rhw) {
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, d->viewport[2], d->viewport[3], 0, 0, -1);
        glTranslatef(0.5f - (float)d->viewport[0], 0.5f - (float)d->viewport[1], 0);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
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
        glVertexPointer(v.pos_comps, GL_FLOAT, (GLsizei)sstride[v.pos_stream], sbase[v.pos_stream] + v.pos_off);
        apply_lighting(d, v.normal_off >= 0, v.diffuse_off >= 0, v.specular_off >= 0);
    }
    glEnableClientState(GL_VERTEX_ARRAY);
    if (v.normal_off >= 0 && !v.pos_rhw) {
        glEnableClientState(GL_NORMAL_ARRAY);
        glNormalPointer(GL_FLOAT, (GLsizei)sstride[v.normal_stream], sbase[v.normal_stream] + v.normal_off);
    } else {
        glDisableClientState(GL_NORMAL_ARRAY);
        glNormal3f(0, 0, 0);
    }
    if (v.diffuse_off >= 0) {
        bgra_to_rgba(sbase[v.diffuse_stream] + v.diffuse_off, sstride[v.diffuse_stream], total, colors);
        glEnableClientState(GL_COLOR_ARRAY);
        glColorPointer(4, GL_UNSIGNED_BYTE, 4, colors);
    } else {
        glDisableClientState(GL_COLOR_ARRAY);
        glColor4f(1, 1, 1, 1);
    }
    int units = apply_stages(d, v.tex_count);
    for (int s = 0; s < 4; s++) {
        glClientActiveTexture(GL_TEXTURE0 + (GLenum)s);
        int set = s < units ? (int)(d->tss[s][D3DTSS_TEXCOORDINDEX] & 0xFFFF) : -1;
        if (set >= 0 && set < v.tex_count && !(d->tss[s][D3DTSS_TEXCOORDINDEX] & 0xFFFF0000U)) {
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glTexCoordPointer(v.tex_size[set], GL_FLOAT, (GLsizei)sstride[v.tex_stream[set]], sbase[v.tex_stream[set]] + v.tex_off[set]);
        } else {
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        }
    }
    glClientActiveTexture(GL_TEXTURE0);
    if (indices)
        glDrawElements(gl_prim(prim), (GLsizei)index_count, index32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, indices);
    else
        glDrawArrays(gl_prim(prim), (GLint)first_vertex, (GLsizei)num_vertices);
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
    glScissor((GLint)vp[0], (GLint)(d->height - vp[1] - vp[3]), (GLsizei)vp[2], (GLsizei)vp[3]);
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

void gl_present(d3d_device *d)
{
    pthread_mutex_lock(&pending_lock);
    if (pending_count) {
        glDeleteTextures((GLsizei)pending_count, pending_deletes);
        pending_count = 0;
    }
    pthread_mutex_unlock(&pending_lock);
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
    if ((int)d->frame == shot_frame || (shot_every && d->frame % (uint32_t)shot_every == 0)) {
        char name[64];
        snprintf(name, sizeof name, "frame_%04u.png", d->frame);
        plat_screenshot(name);
    }
    plat_swap();
    d->frame++;
    uint64_t now = plat_ticks_us();
    if (d->frame % 300 == 0) {
        rt_log("[d3d] frame %u, %.1f fps, guest heap %u MB\n", d->frame, last_present_us ? 1e6 / (double)(now - last_present_us) : 0.0, rt_heap_used() >> 20);
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
