/* Direct3D 9 object layer: IDirect3D9, the device, textures, surfaces, buffers and vertex declarations as guest COM objects. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d9.h"

#define D3D_OK 0U
#define E_NOINTERFACE 0x80004002U
#define D3DERR_NOTAVAILABLE 0x8876086AU
#define D3DERR_INVALIDCALL 0x8876086CU

static d3d_device *device;
/* Live guest-memory bytes held by textures and buffers, for the memory budget. */
uint64_t d3d_tex_bytes, d3d_buf_bytes;
uint32_t d3d_tex_count;
static uint32_t d3d_object;
static int trace_d3d;

d3d_device *d3d_dev(void) { return device; }

typedef struct { int kind; uint32_t guest, refs; } d3d_any;

static uint32_t m_AddRef(CPU *c) { d3d_any *o = com_host(ARG(0)); return o ? ++o->refs : 1; }
static uint32_t m_Release(CPU *c)
{
    d3d_any *o = com_host(ARG(0));
    if (!o) return 0;
    if (o->refs) o->refs--;
    return o->refs;
}
static uint32_t m_QueryInterface(CPU *c) { if (ARG(2)) st32(ARG(2), 0); return E_NOINTERFACE; }
static uint32_t m_Zero(CPU *c) { (void)c; return 0; }
static uint32_t m_GetDevice(CPU *c) { if (device) { device->refs++; st32(ARG(1), device->guest); } return D3D_OK; }

/* ------------------------------------------------------------ formats --- */

uint32_t d3d_format_bpp(uint32_t f)
{
    switch (f) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8: case D3DFMT_D24S8: case D3DFMT_D24X8: case D3DFMT_D32: case D3DFMT_INDEX32:
        return 32;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4: case D3DFMT_A8L8: case D3DFMT_D16: case D3DFMT_INDEX16:
        return 16;
    case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_P8:
        return 8;
    case D3DFMT_DXT1:
        return 4;
    case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5:
        return 8;
    default:
        return 32;
    }
}

int d3d_format_is_dxt(uint32_t f)
{
    return f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 || f == D3DFMT_DXT4 || f == D3DFMT_DXT5;
}

static void level_layout(uint32_t fmt, uint32_t w, uint32_t h, uint32_t *pitch, uint32_t *bytes)
{
    if (d3d_format_is_dxt(fmt)) {
        uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, block = fmt == D3DFMT_DXT1 ? 8U : 16U;
        *pitch = bw * block;
        *bytes = bw * bh * block;
    } else {
        *pitch = (w * d3d_format_bpp(fmt) / 8 + 3) & ~3U;
        *bytes = *pitch * h;
    }
}

static uint32_t mipw(uint32_t v, int l) { v >>= l; return v ? v : 1; }

/* ------------------------------------------------------------ surfaces --- */

static uint32_t surface_vtbl(void);

static d3d_surf *surface_new(uint32_t fmt, uint32_t w, uint32_t h, uint32_t usage, uint32_t pool, int with_mem)
{
    d3d_surf *s = calloc(1, sizeof *s);
    s->kind = K_SURFACE;
    s->refs = 1;
    s->fmt = fmt; s->w = w; s->h = h; s->usage = usage; s->pool = pool;
    if (with_mem) {
        uint32_t bytes;
        level_layout(fmt, w, h, &s->pitch, &bytes);
        s->mem = rt_alloc(bytes);
        memset(G2H(s->mem), 0, bytes);
    }
    s->guest = com_new(surface_vtbl(), 16, s);
    return s;
}

static void surf_desc(uint32_t p, uint32_t fmt, uint32_t type, uint32_t usage, uint32_t pool, uint32_t w, uint32_t h)
{
    st32(p, fmt); st32(p + 4, type); st32(p + 8, usage); st32(p + 12, pool); st32(p + 16, 0); st32(p + 20, 0); st32(p + 24, w); st32(p + 28, h);
}

static uint32_t s_GetDesc(CPU *c)
{
    d3d_surf *s = THIS_HOST(d3d_surf);
    surf_desc(ARG(1), s->fmt, D3DRTYPE_SURFACE, s->usage, s->pool, s->w, s->h);
    return D3D_OK;
}
static uint32_t lock_rect(uint32_t locked, uint32_t mem, uint32_t pitch, uint32_t fmt, uint32_t rect)
{
    uint32_t off = 0;
    if (rect) {
        uint32_t x = ld32(rect), y = ld32(rect + 4);
        off = d3d_format_is_dxt(fmt) ? (y / 4) * pitch + (x / 4) * (fmt == D3DFMT_DXT1 ? 8U : 16U) : y * pitch + x * d3d_format_bpp(fmt) / 8;
    }
    st32(locked, pitch);
    st32(locked + 4, mem + off);
    return D3D_OK;
}
static uint32_t s_LockRect(CPU *c)
{
    d3d_surf *s = THIS_HOST(d3d_surf);
    if (s->parent)
        return lock_rect(ARG(1), s->parent->mem[s->level], s->parent->pitch[s->level], s->fmt, ARG(2));
    if (!s->mem) {
        uint32_t bytes;
        level_layout(s->fmt, s->w, s->h, &s->pitch, &bytes);
        s->mem = rt_alloc(bytes);
        memset(G2H(s->mem), 0, bytes);
    }
    return lock_rect(ARG(1), s->mem, s->pitch, s->fmt, ARG(2));
}
static uint32_t s_UnlockRect(CPU *c)
{
    d3d_surf *s = THIS_HOST(d3d_surf);
    if (s->parent) __sync_fetch_and_or(&s->parent->dirty_pending, 1U << s->level);
    return D3D_OK;
}
static uint32_t s_GetContainer(CPU *c)
{
    d3d_surf *s = THIS_HOST(d3d_surf);
    if (s->parent) { s->parent->refs++; st32(ARG(2), s->parent->guest); return D3D_OK; }
    st32(ARG(2), 0);
    return E_NOINTERFACE;
}
static uint32_t s_GetType(CPU *c) { (void)c; return D3DRTYPE_SURFACE; }

static uint32_t surface_vtbl(void)
{
    static const com_impl impls[] = {
        { "QueryInterface", m_QueryInterface }, { "AddRef", m_AddRef }, { "Release", m_Release }, { "GetDevice", m_GetDevice },
        { "GetType", s_GetType }, { "GetContainer", s_GetContainer }, { "GetDesc", s_GetDesc }, { "LockRect", s_LockRect },
        { "UnlockRect", s_UnlockRect }, { "SetPrivateData", m_Zero }, { "SetPriority", m_Zero }, { "PreLoad", m_Zero }, { NULL, NULL },
    };
    return COM_VTABLE(IDirect3DSurface9, impls);
}

/* ------------------------------------------------------------ textures --- */

static uint32_t t_GetLevelCount(CPU *c) { return THIS_HOST(d3d_tex)->levels; }
static uint32_t t_GetLevelDesc(CPU *c)
{
    d3d_tex *t = THIS_HOST(d3d_tex);
    int l = (int)ARG(1);
    surf_desc(ARG(2), t->fmt, D3DRTYPE_SURFACE, t->usage, t->pool, mipw(t->w, l), mipw(t->h, l));
    return D3D_OK;
}
static uint32_t t_GetSurfaceLevel(CPU *c)
{
    d3d_tex *t = THIS_HOST(d3d_tex);
    uint32_t l = ARG(1);
    if (l >= t->levels) return D3DERR_INVALIDCALL;
    if (!t->surf[l]) {
        d3d_surf *s = surface_new(t->fmt, mipw(t->w, (int)l), mipw(t->h, (int)l), t->usage, t->pool, 0);
        s->parent = t;
        s->level = (int)l;
        t->surf[l] = s;
    }
    t->surf[l]->refs++;
    st32(ARG(2), t->surf[l]->guest);
    return D3D_OK;
}
static uint32_t t_LockRect(CPU *c)
{
    d3d_tex *t = THIS_HOST(d3d_tex);
    uint32_t l = ARG(1);
    if (l >= t->levels) return D3DERR_INVALIDCALL;
    return lock_rect(ARG(2), t->mem[l], t->pitch[l], t->fmt, ARG(3));
}
static uint32_t t_UnlockRect(CPU *c)
{
    d3d_tex *t = THIS_HOST(d3d_tex);
    __sync_fetch_and_or(&t->dirty_pending, 1U << ARG(1));
    return D3D_OK;
}
static uint32_t t_GetType(CPU *c) { (void)c; return D3DRTYPE_TEXTURE; }
static uint32_t t_Release(CPU *c)
{
    d3d_tex *t = THIS_HOST(d3d_tex);
    if (!t) return 0;
    if (t->refs) t->refs--;
    if (t->refs == 0 && !getenv("TMNT_KEEP_TEXTURES")) {
        d3d_tex_count--;
        for (uint32_t l = 0; l < t->levels; l++) {
            d3d_tex_bytes -= t->bytes[l];
            if (t->surf[l]) com_forget(t->surf[l]->guest);
        }
        for (int s = 0; s < MAX_STAGES; s++) if (device && device->tex[s] == t) { device->tex[s] = NULL; DEV_TOUCH(device, tex[s]); }
        com_forget(t->guest);
        /* The game may release on a loader thread: the GL texture and the memory go on the render side's terms. */
        gl_texture_release(t);
        return 0;
    }
    return t->refs;
}

static uint32_t texture_vtbl(void)
{
    static const com_impl impls[] = {
        { "QueryInterface", m_QueryInterface }, { "AddRef", m_AddRef }, { "Release", t_Release }, { "GetDevice", m_GetDevice },
        { "GetType", t_GetType }, { "GetLevelCount", t_GetLevelCount }, { "GetLevelDesc", t_GetLevelDesc },
        { "GetSurfaceLevel", t_GetSurfaceLevel }, { "LockRect", t_LockRect }, { "UnlockRect", t_UnlockRect },
        { "SetPrivateData", m_Zero }, { "SetPriority", m_Zero }, { "PreLoad", m_Zero }, { "SetLOD", m_Zero }, { "GetLOD", m_Zero },
        { "AddDirtyRect", m_Zero }, { "GenerateMipSubLevels", m_Zero }, { "SetAutoGenFilterType", m_Zero }, { NULL, NULL },
    };
    return COM_VTABLE(IDirect3DTexture9, impls);
}

static d3d_tex *texture_new(uint32_t w, uint32_t h, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool)
{
    d3d_tex *t = calloc(1, sizeof *t);
    t->kind = K_TEXTURE;
    t->refs = 1;
    if (levels == 0) {
        uint32_t m = w > h ? w : h;
        while (m) { levels++; m >>= 1; }
    }
    if (levels > MAX_LEVELS) levels = MAX_LEVELS;
    t->w = w; t->h = h; t->levels = levels; t->usage = usage; t->fmt = fmt; t->pool = pool;
    d3d_tex_count++;
    for (uint32_t l = 0; l < levels; l++) {
        level_layout(fmt, mipw(w, (int)l), mipw(h, (int)l), &t->pitch[l], &t->bytes[l]);
        d3d_tex_bytes += t->bytes[l];
        t->mem[l] = rt_alloc(t->bytes[l]);
        memset(G2H(t->mem[l]), 0, t->bytes[l]);
    }
    t->dirty = (1U << levels) - 1;
    t->guest = com_new(texture_vtbl(), 16, t);
    return t;
}

/* ------------------------------------------------------------- buffers --- */

static uint32_t b_Lock(CPU *c)
{
    d3d_buf *b = THIS_HOST(d3d_buf);
    if (!(ARG(4) & D3DLOCK_READONLY)) { b->version++; b->lock_frame = d3d_frame_now; }
    st32(ARG(3), b->mem + ARG(1));
    return D3D_OK;
}
static uint32_t b_GetDescVB(CPU *c)
{
    d3d_buf *b = THIS_HOST(d3d_buf);
    uint32_t p = ARG(1);
    st32(p, 100U); st32(p + 4, D3DRTYPE_VERTEXBUFFER); st32(p + 8, b->usage); st32(p + 12, b->pool); st32(p + 16, b->size); st32(p + 20, b->fvf);
    return D3D_OK;
}
static uint32_t b_GetDescIB(CPU *c)
{
    d3d_buf *b = THIS_HOST(d3d_buf);
    uint32_t p = ARG(1);
    st32(p, b->fmt); st32(p + 4, D3DRTYPE_INDEXBUFFER); st32(p + 8, b->usage); st32(p + 12, b->pool); st32(p + 16, b->size);
    return D3D_OK;
}
static uint32_t b_Release(CPU *c)
{
    d3d_buf *b = THIS_HOST(d3d_buf);
    if (!b) return 0;
    if (b->refs) b->refs--;
    if (b->refs == 0) {
        for (int s = 0; s < 4; s++) if (device && device->stream[s].vb == b) { device->stream[s].vb = NULL; DEV_TOUCH(device, stream[s]); }
        if (device && device->ib == b) { device->ib = NULL; DEV_TOUCH(device, ib); }
        d3d_buf_bytes -= b->size;
        com_forget(b->guest);
        gl_buffer_release(b);
        return 0;
    }
    return b->refs;
}
static uint32_t buf_vtbl(int index)
{
    static const com_impl vb[] = {
        { "QueryInterface", m_QueryInterface }, { "AddRef", m_AddRef }, { "Release", b_Release }, { "GetDevice", m_GetDevice },
        { "Lock", b_Lock }, { "Unlock", m_Zero }, { "GetDesc", b_GetDescVB }, { "SetPriority", m_Zero }, { "PreLoad", m_Zero }, { NULL, NULL },
    };
    static const com_impl ib[] = {
        { "QueryInterface", m_QueryInterface }, { "AddRef", m_AddRef }, { "Release", b_Release }, { "GetDevice", m_GetDevice },
        { "Lock", b_Lock }, { "Unlock", m_Zero }, { "GetDesc", b_GetDescIB }, { "SetPriority", m_Zero }, { "PreLoad", m_Zero }, { NULL, NULL },
    };
    return index ? COM_VTABLE(IDirect3DIndexBuffer9, ib) : COM_VTABLE(IDirect3DVertexBuffer9, vb);
}
static d3d_buf *buf_new(int index, uint32_t size, uint32_t usage, uint32_t fvf_or_fmt, uint32_t pool)
{
    d3d_buf *b = calloc(1, sizeof *b);
    b->kind = index ? K_IB : K_VB;
    b->refs = 1;
    b->size = size; b->usage = usage; b->pool = pool;
    if (index) b->fmt = fvf_or_fmt; else b->fvf = fvf_or_fmt;
    b->mem = rt_alloc(size ? size : 4);
    d3d_buf_bytes += size;
    memset(G2H(b->mem), 0, size);
    b->guest = com_new(buf_vtbl(index), 16, b);
    return b;
}

/* ------------------------------------------------- vertex declarations --- */

/* GetDeclaration(pElements, pNumElements): the element array including the D3DDECL_END marker. */
static uint32_t v_GetDeclaration(CPU *c)
{
    d3d_decl *dc = THIS_HOST(d3d_decl);
    uint32_t p = ARG(1), n = (uint32_t)dc->count + 1;
    if (p) {
        for (int i = 0; i < dc->count; i++) {
            const d3d_decl_elem *e = &dc->e[i];
            uint32_t q = p + 8U * (uint32_t)i;
            st16(q, e->stream); st16(q + 2, e->offset); st8(q + 4, e->type); st8(q + 5, e->method); st8(q + 6, e->usage); st8(q + 7, e->index);
        }
        uint32_t q = p + 8U * (uint32_t)dc->count;
        st16(q, 0xFF); st16(q + 2, 0); st8(q + 4, 17); st8(q + 5, 0); st8(q + 6, 0); st8(q + 7, 0);
    }
    if (ARG(2)) st32(ARG(2), n);
    return D3D_OK;
}

static uint32_t decl_vtbl(void)
{
    static const com_impl impls[] = {
        { "QueryInterface", m_QueryInterface }, { "AddRef", m_AddRef }, { "Release", m_Release }, { "GetDevice", m_GetDevice },
        { "GetDeclaration", v_GetDeclaration }, { NULL, NULL },
    };
    return COM_VTABLE(IDirect3DVertexDeclaration9, impls);
}

/* ---------------------------------------------------------------- caps --- */

static void fill_caps(uint32_t p)
{
    uint32_t v[76];
    memset(v, 0, sizeof v);
    v[0] = D3DDEVTYPE_HAL;
    v[2] = 0x00020000;           /* D3DCAPS_READ_SCANLINE */
    v[3] = 0x20000000 | 0x40000000; /* CANAUTOGENMIPMAP | FULLSCREENGAMMA */
    v[4] = 0x20;                 /* ALPHA_FULLSCREEN_FLIP_OR_DISCARD */
    v[5] = 0x80000001U;          /* INTERVAL_IMMEDIATE | ONE */
    v[7] = 0x00000010 | 0x00000020 | 0x00000040 | 0x00000080 | 0x00000200 | 0x00000400 | 0x00002000 | 0x00004000 | 0x00010000 | 0x00080000;
    v[8] = 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x40 | 0x80 | 0x100 | 0x800;
    v[9] = 0x1 | 0x10 | 0x80 | 0x100 | 0x2000 | 0x4000 | 0x8000 | 0x10000 | 0x200000 | 0x800000 | 0x02000000 | 0x04000000;
    v[10] = v[13] = 0xFF;
    v[11] = v[12] = 0x1FFF;
    v[14] = 0x200 | 0x4000 | 0x80000 | 0x100000;
    v[15] = 0x1 | 0x4 | 0x2 | 0x40 | 0x800 | 0x4000;  /* PERSPECTIVE | ALPHA | POW2 | MIPMAP | CUBEMAP... */
    v[16] = v[17] = 0x0303 | 0x30000 | 0x03000000;
    v[16] = 0x00000100 | 0x00000200 | 0x00010000 | 0x00020000 | 0x01000000 | 0x02000000;
    v[17] = v[16];
    v[19] = 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20;
    v[21] = 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20;
    v[22] = v[23] = 2048;
    v[24] = 256;
    v[25] = 8192;
    v[26] = 2048;
    v[27] = 1;
    { float f = 1e10f; memcpy(&v[28], &f, 4); }
    { float f = -1e8f; memcpy(&v[29], &f, 4); memcpy(&v[30], &f, 4); f = 1e8f; memcpy(&v[31], &f, 4); memcpy(&v[32], &f, 4); }
    v[34] = 0xFF;
    v[35] = 0x00080000 | 8;      /* PSIZE | 8 texcoords */
    v[36] = 0x03FFFFFF;
    v[37] = 8;
    v[38] = 4;
    v[39] = 0x1 | 0x2 | 0x8 | 0x10 | 0x20 | 0x80;
    v[40] = 8;
    v[41] = 0;
    v[42] = 0;
    v[43] = 0;
    { float f = 64.0f; memcpy(&v[44], &f, 4); }
    v[45] = 0xFFFFF;
    v[46] = 0xFFFFF;
    v[47] = 4;
    v[48] = 256;
    v[49] = 0;                   /* no vertex shaders: RenderWare stays on its fixed-function pipelines */
    v[50] = 0;
    v[51] = 0;
    v[58] = 1;
    v[59] = 0x1 | 0x2;
    v[60] = 1;
    v[61] = 0x03000300;
    memcpy(G2H(p), v, sizeof v);
}

/* --------------------------------------------------------------- device --- */

static uint32_t dev_vtbl(void);

static uint32_t d_TestCooperativeLevel(CPU *c) { (void)c; return D3D_OK; }
static uint32_t d_GetAvailableTextureMem(CPU *c) { (void)c; return 256U << 20; }
static uint32_t d_GetDirect3D(CPU *c) { st32(ARG(1), d3d_object); return D3D_OK; }
static uint32_t d_GetDeviceCaps(CPU *c) { fill_caps(ARG(1)); return D3D_OK; }
static uint32_t d_GetDisplayMode(CPU *c)
{
    st32(ARG(2), device->width); st32(ARG(2) + 4, device->height); st32(ARG(2) + 8, 60); st32(ARG(2) + 12, D3DFMT_X8R8G8B8);
    return D3D_OK;
}
static uint32_t d_GetNumberOfSwapChains(CPU *c) { (void)c; return 1; }
static uint32_t d_Present(CPU *c) { gl_present(THIS_HOST(d3d_device)); return D3D_OK; }
static uint32_t d_GetBackBuffer(CPU *c)
{
    d3d_device *d = THIS_HOST(d3d_device);
    d->backbuffer->refs++;
    st32(ARG(4), d->backbuffer->guest);
    return D3D_OK;
}
static uint32_t d_GetRasterStatus(CPU *c) { st32(ARG(2), 0); st32(ARG(2) + 4, 0); return D3D_OK; }
static uint32_t d_CreateTexture(CPU *c)
{
    d3d_tex *t = texture_new(ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6));
    if (trace_d3d) rt_log("[d3d] CreateTexture %ux%u levels %u usage %x fmt %x pool %u -> %08x\n", ARG(1), ARG(2), t->levels, ARG(4), ARG(5), ARG(6), t->guest);
    st32(ARG(7), t->guest);
    return D3D_OK;
}
static uint32_t d_CreateVertexBuffer(CPU *c)
{
    d3d_buf *b = buf_new(0, ARG(1), ARG(2), ARG(3), ARG(4));
    st32(ARG(5), b->guest);
    return D3D_OK;
}
static uint32_t d_CreateIndexBuffer(CPU *c)
{
    d3d_buf *b = buf_new(1, ARG(1), ARG(2), ARG(3), ARG(4));
    st32(ARG(5), b->guest);
    return D3D_OK;
}
static uint32_t d_CreateRenderTarget(CPU *c)
{
    d3d_surf *s = surface_new(ARG(3), ARG(1), ARG(2), D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, 0);
    rt_log("[d3d] CreateRenderTarget %ux%u fmt %x\n", ARG(1), ARG(2), ARG(3));
    st32(ARG(7), s->guest);
    return D3D_OK;
}
static uint32_t d_CreateDepthStencilSurface(CPU *c)
{
    d3d_surf *s = surface_new(ARG(3), ARG(1), ARG(2), D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, 0);
    s->is_depth = 1;
    st32(ARG(7), s->guest);
    return D3D_OK;
}
static uint32_t d_CreateOffscreenPlainSurface(CPU *c)
{
    d3d_surf *s = surface_new(ARG(3), ARG(1), ARG(2), 0, ARG(4), 1);
    st32(ARG(5), s->guest);
    return D3D_OK;
}
static uint32_t d_SetRenderTarget(CPU *c)
{
    d3d_device *d = THIS_HOST(d3d_device);
    if (ARG(1) != 0) return D3D_OK;
    d3d_surf *s = com_host(ARG(2));
    gl_set_render_target(d, s ? s : d->backbuffer);
    return D3D_OK;
}
static uint32_t d_GetRenderTarget(CPU *c)
{
    d3d_device *d = THIS_HOST(d3d_device);
    d3d_surf *s = d->rt0 ? d->rt0 : d->backbuffer;
    s->refs++;
    st32(ARG(2), s->guest);
    return D3D_OK;
}
static uint32_t d_SetDepthStencilSurface(CPU *c)
{
    d3d_device *d = THIS_HOST(d3d_device);
    d->ds = com_host(ARG(1));
    DEV_TOUCH(d, ds);
    return D3D_OK;
}
static uint32_t d_GetDepthStencilSurface(CPU *c)
{
    d3d_device *d = THIS_HOST(d3d_device);
    d3d_surf *s = d->ds ? d->ds : d->depth;
    s->refs++;
    st32(ARG(1), s->guest);
    return D3D_OK;
}
static uint32_t d_BeginScene(CPU *c) { d3d_device *d = THIS_HOST(d3d_device); d->in_scene = 1; DEV_TOUCH(d, in_scene); return D3D_OK; }
static uint32_t d_EndScene(CPU *c) { d3d_device *d = THIS_HOST(d3d_device); d->in_scene = 0; DEV_TOUCH(d, in_scene); return D3D_OK; }
static uint32_t d_Clear(CPU *c)
{
    gl_clear(THIS_HOST(d3d_device), ARG(3), ARG(4), u2f(ARG(5)), ARG(6), ARG(1), ARG(2));
    return D3D_OK;
}
static float *transform_slot(d3d_device *d, uint32_t state)
{
    if (state == D3DTS_WORLD) return d->world;
    if (state == D3DTS_VIEW) return d->view;
    if (state == D3DTS_PROJECTION) return d->proj;
    if (state >= D3DTS_TEXTURE0 && state < D3DTS_TEXTURE0 + MAX_STAGES) return d->texm[state - D3DTS_TEXTURE0];
    return NULL;
}
static uint32_t d_SetTransform(CPU *c)
{
    float *m = transform_slot(THIS_HOST(d3d_device), ARG(1));
    if (m) {
        memcpy(m, G2H(ARG(2)), 64);
        gl_dev_touch(THIS_HOST(d3d_device), m, 64);
        static int nan_reports;
        for (int i = 0; i < 16; i++) {
            if (m[i] != m[i] || m[i] > 1e30f || m[i] < -1e30f) {
                if (nan_reports++ < 10)
                    rt_log("[d3d] SetTransform(%u) got a non-finite matrix (element %d = %g) from guest %08x\n", ARG(1), i, m[i], ld32(c->esp));
                break;
            }
        }
    }
    return D3D_OK;
}
static uint32_t d_GetTransform(CPU *c)
{
    float *m = transform_slot(THIS_HOST(d3d_device), ARG(1));
    if (m) memcpy(G2H(ARG(2)), m, 64); else memset(G2H(ARG(2)), 0, 64);
    return D3D_OK;
}
static uint32_t d_MultiplyTransform(CPU *c)
{
    float *m = transform_slot(THIS_HOST(d3d_device), ARG(1)), r[16], b[16];
    if (!m) return D3D_OK;
    memcpy(b, G2H(ARG(2)), 64);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = b[i * 4 + 0] * m[0 * 4 + j] + b[i * 4 + 1] * m[1 * 4 + j] + b[i * 4 + 2] * m[2 * 4 + j] + b[i * 4 + 3] * m[3 * 4 + j];
    memcpy(m, r, 64);
    gl_dev_touch(THIS_HOST(d3d_device), m, 64);
    return D3D_OK;
}
static uint32_t d_SetViewport(CPU *c) { d3d_device *d = THIS_HOST(d3d_device); memcpy(d->viewport, G2H(ARG(1)), 24); DEV_TOUCH(d, viewport); return D3D_OK; }
static uint32_t d_GetViewport(CPU *c) { memcpy(G2H(ARG(1)), THIS_HOST(d3d_device)->viewport, 24); return D3D_OK; }
static uint32_t d_SetMaterial(CPU *c) { d3d_device *d = THIS_HOST(d3d_device); memcpy(d->material, G2H(ARG(1)), 68); DEV_TOUCH(d, material); return D3D_OK; }
static uint32_t d_GetMaterial(CPU *c) { memcpy(G2H(ARG(1)), THIS_HOST(d3d_device)->material, 68); return D3D_OK; }
static uint32_t d_SetLight(CPU *c)
{
    d3d_device *d = THIS_HOST(d3d_device);
    if (ARG(1) < 8) { memcpy(d->lights[ARG(1)].raw, G2H(ARG(2)), 104); DEV_TOUCH(d, lights[ARG(1)]); }
    return D3D_OK;
}
static uint32_t d_GetLight(CPU *c)
{
    if (ARG(1) < 8) memcpy(G2H(ARG(2)), THIS_HOST(d3d_device)->lights[ARG(1)].raw, 104);
    return D3D_OK;
}
static uint32_t d_LightEnable(CPU *c)
{
    d3d_device *d = THIS_HOST(d3d_device);
    if (ARG(1) < 8) { d->lights[ARG(1)].enabled = ARG(2) != 0; DEV_TOUCH(d, lights[ARG(1)]); }
    return D3D_OK;
}
static uint32_t d_GetLightEnable(CPU *c) { st32(ARG(2), ARG(1) < 8 ? (uint32_t)THIS_HOST(d3d_device)->lights[ARG(1)].enabled : 0); return D3D_OK; }
static uint32_t d_SetRenderState(CPU *c)
{
    d3d_device *d = THIS_HOST(d3d_device);
    if (ARG(1) < 256) { d->rs[ARG(1)] = ARG(2); DEV_TOUCH(d, rs[ARG(1)]); }
    return D3D_OK;
}
static uint32_t d_GetRenderState(CPU *c) { st32(ARG(2), ARG(1) < 256 ? THIS_HOST(d3d_device)->rs[ARG(1)] : 0); return D3D_OK; }
static uint32_t d_GetTexture(CPU *c)
{
    d3d_tex *t = ARG(1) < MAX_STAGES ? THIS_HOST(d3d_device)->tex[ARG(1)] : NULL;
    if (t) t->refs++;
    st32(ARG(2), t ? t->guest : 0);
    return D3D_OK;
}
static uint32_t d_SetTexture(CPU *c)
{
    if (ARG(1) < MAX_STAGES) {
        d3d_tex *t = com_host(ARG(2));
        d3d_device *d = THIS_HOST(d3d_device);
        d->tex[ARG(1)] = (t && t->kind == K_TEXTURE) ? t : NULL;
        DEV_TOUCH(d, tex[ARG(1)]);
    }
    return D3D_OK;
}
static uint32_t d_GetTextureStageState(CPU *c) { st32(ARG(3), ARG(1) < MAX_STAGES && ARG(2) < 33 ? THIS_HOST(d3d_device)->tss[ARG(1)][ARG(2)] : 0); return D3D_OK; }
static uint32_t d_SetTextureStageState(CPU *c)
{
    d3d_device *d = THIS_HOST(d3d_device);
    if (ARG(1) < MAX_STAGES && ARG(2) < 33) { d->tss[ARG(1)][ARG(2)] = ARG(3); DEV_TOUCH(d, tss[ARG(1)][ARG(2)]); }
    return D3D_OK;
}
static uint32_t d_GetSamplerState(CPU *c) { st32(ARG(3), ARG(1) < MAX_STAGES && ARG(2) < 14 ? THIS_HOST(d3d_device)->ss[ARG(1)][ARG(2)] : 0); return D3D_OK; }
static uint32_t d_SetSamplerState(CPU *c)
{
    d3d_device *d = THIS_HOST(d3d_device);
    if (ARG(1) < MAX_STAGES && ARG(2) < 14) { d->ss[ARG(1)][ARG(2)] = ARG(3); DEV_TOUCH(d, ss[ARG(1)][ARG(2)]); }
    return D3D_OK;
}
static uint32_t d_ValidateDevice(CPU *c) { if (ARG(1)) st32(ARG(1), 1); return D3D_OK; }
static uint32_t d_SetScissorRect(CPU *c) { d3d_device *d = THIS_HOST(d3d_device); memcpy(d->scissor, G2H(ARG(1)), 16); DEV_TOUCH(d, scissor); return D3D_OK; }
static uint32_t d_GetScissorRect(CPU *c) { memcpy(G2H(ARG(1)), THIS_HOST(d3d_device)->scissor, 16); return D3D_OK; }

static uint32_t prim_vertices(uint32_t type, uint32_t count)
{
    switch (type) {
    case D3DPT_POINTLIST: return count;
    case D3DPT_LINELIST: return count * 2;
    case D3DPT_LINESTRIP: return count + 1;
    case D3DPT_TRIANGLELIST: return count * 3;
    default: return count + 2;
    }
}
static uint32_t d_DrawPrimitive(CPU *c)
{
    d3d_device *d = THIS_HOST(d3d_device);
    if (!d->stream[0].vb) return D3D_OK;
    draw_vb_obj = d->stream[0].vb;
    gl_draw(d, ARG(1), d->stream[0].vb->mem + d->stream[0].offset, d->stream[0].stride, ARG(2), prim_vertices(ARG(1), ARG(3)), NULL, 0, 0);
    draw_vb_obj = NULL;
    return D3D_OK;
}
static uint32_t d_DrawIndexedPrimitive(CPU *c)
{
    d3d_device *d = THIS_HOST(d3d_device);
    if (!d->stream[0].vb || !d->ib) return D3D_OK;
    int i32 = d->ib->fmt == D3DFMT_INDEX32;
    uint32_t n = prim_vertices(ARG(1), ARG(6));
    const void *idx = G2H(d->ib->mem + ARG(5) * (i32 ? 4U : 2U));
    extern uint32_t draw_min_index;
    draw_min_index = ARG(3);
    draw_vb_obj = d->stream[0].vb;
    draw_ib_obj = d->ib;
    gl_draw(d, ARG(1), d->stream[0].vb->mem + d->stream[0].offset + (uint32_t)((int32_t)ARG(2) * (int32_t)d->stream[0].stride),
            d->stream[0].stride, 0, ARG(3) + ARG(4), idx, i32, n);
    draw_min_index = 0;
    draw_vb_obj = draw_ib_obj = NULL;
    return D3D_OK;
}
static uint32_t d_DrawPrimitiveUP(CPU *c)
{
    d3d_device *d = THIS_HOST(d3d_device);
    gl_draw(d, ARG(1), ARG(3), ARG(4), 0, prim_vertices(ARG(1), ARG(2)), NULL, 0, 0);
    d->stream[0].vb = NULL;
    DEV_TOUCH(d, stream[0]);
    return D3D_OK;
}
static uint32_t d_DrawIndexedPrimitiveUP(CPU *c)
{
    d3d_device *d = THIS_HOST(d3d_device);
    int i32 = ARG(6) == D3DFMT_INDEX32;
    gl_draw(d, ARG(1), ARG(7), ARG(8), 0, ARG(2) + ARG(3), G2H(ARG(5)), i32, prim_vertices(ARG(1), ARG(4)));
    d->stream[0].vb = NULL;
    d->ib = NULL;
    DEV_TOUCH(d, stream[0]);
    DEV_TOUCH(d, ib);
    return D3D_OK;
}
static uint32_t d_CreateVertexDeclaration(CPU *c)
{
    d3d_decl *dc = calloc(1, sizeof *dc);
    dc->kind = K_DECL;
    dc->refs = 1;
    for (uint32_t p = ARG(1); dc->count < 32; p += 8) {
        d3d_decl_elem *e = &dc->e[dc->count];
        e->stream = ld16(p); e->offset = ld16(p + 2); e->type = ld8(p + 4); e->method = ld8(p + 5); e->usage = ld8(p + 6); e->index = ld8(p + 7);
        if (e->stream == 0xFF) break;
        dc->count++;
    }
    dc->guest = com_new(decl_vtbl(), 16, dc);
    st32(ARG(2), dc->guest);
    return D3D_OK;
}
static uint32_t d_SetVertexDeclaration(CPU *c)
{
    d3d_device *d = THIS_HOST(d3d_device);
    d->decl = com_host(ARG(1));
    if (d->decl) d->fvf = 0;
    DEV_TOUCH(d, decl);
    DEV_TOUCH(d, fvf);
    return D3D_OK;
}
static uint32_t d_GetVertexDeclaration(CPU *c) { d3d_device *d = THIS_HOST(d3d_device); st32(ARG(1), d->decl ? d->decl->guest : 0); return D3D_OK; }
static uint32_t d_SetFVF(CPU *c) { d3d_device *d = THIS_HOST(d3d_device); d->fvf = ARG(1); d->decl = NULL; DEV_TOUCH(d, fvf); DEV_TOUCH(d, decl); return D3D_OK; }
static uint32_t d_GetFVF(CPU *c) { st32(ARG(1), THIS_HOST(d3d_device)->fvf); return D3D_OK; }
static uint32_t d_SetVertexShader(CPU *c)
{
    if (ARG(1)) rt_log("[d3d] SetVertexShader(%08x): vertex shaders are not supported\n", ARG(1));
    return D3D_OK;
}
static uint32_t d_GetVertexShader(CPU *c) { st32(ARG(1), 0); return D3D_OK; }
static uint32_t d_SetPixelShader(CPU *c) { (void)c; return D3D_OK; }
static uint32_t d_GetPixelShader(CPU *c) { st32(ARG(1), 0); return D3D_OK; }
static uint32_t d_SetStreamSource(CPU *c)
{
    d3d_device *d = THIS_HOST(d3d_device);
    if (ARG(1) < 4) {
        d->stream[ARG(1)].vb = com_host(ARG(2));
        d->stream[ARG(1)].offset = ARG(3);
        d->stream[ARG(1)].stride = ARG(4);
        DEV_TOUCH(d, stream[ARG(1)]);
    }
    return D3D_OK;
}
static uint32_t d_GetStreamSource(CPU *c)
{
    d3d_device *d = THIS_HOST(d3d_device);
    uint32_t s = ARG(1) < 4 ? ARG(1) : 0;
    st32(ARG(2), d->stream[s].vb ? d->stream[s].vb->guest : 0);
    st32(ARG(3), d->stream[s].offset);
    st32(ARG(4), d->stream[s].stride);
    return D3D_OK;
}
static uint32_t d_SetIndices(CPU *c) { d3d_device *d = THIS_HOST(d3d_device); d->ib = com_host(ARG(1)); DEV_TOUCH(d, ib); return D3D_OK; }
static uint32_t d_GetIndices(CPU *c) { d3d_device *d = THIS_HOST(d3d_device); st32(ARG(1), d->ib ? d->ib->guest : 0); return D3D_OK; }
static uint32_t d_Reset(CPU *c) { rt_log("[d3d] Reset\n"); (void)c; return D3D_OK; }
static uint32_t d_GetCreationParameters(CPU *c)
{
    st32(ARG(1), 0); st32(ARG(1) + 4, D3DDEVTYPE_HAL); st32(ARG(1) + 8, 0x00020000); st32(ARG(1) + 12, 0x20);
    return D3D_OK;
}
static uint32_t d_UpdateTexture(CPU *c)
{
    d3d_tex *s = com_host(ARG(1)), *t = com_host(ARG(2));
    if (!s || !t || s->kind != K_TEXTURE || t->kind != K_TEXTURE) return D3D_OK;
    for (uint32_t l = 0; l < s->levels && l < t->levels; l++)
        memcpy(G2H(t->mem[l]), G2H(s->mem[l]), s->bytes[l] < t->bytes[l] ? s->bytes[l] : t->bytes[l]);
    __sync_fetch_and_or(&t->dirty_pending, (1U << t->levels) - 1);
    return D3D_OK;
}
static uint32_t d_StretchRect(CPU *c)
{
    static int warned;
    if (!warned++) rt_log("[d3d] StretchRect is not implemented\n");
    (void)c;
    return D3D_OK;
}
static uint32_t d_GetRenderTargetData(CPU *c) { (void)c; return D3D_OK; }

static uint32_t dev_vtbl(void)
{
    static const com_impl impls[] = {
        { "QueryInterface", m_QueryInterface }, { "AddRef", m_AddRef }, { "Release", m_Release },
        { "TestCooperativeLevel", d_TestCooperativeLevel }, { "GetAvailableTextureMem", d_GetAvailableTextureMem },
        { "EvictManagedResources", m_Zero }, { "GetDirect3D", d_GetDirect3D }, { "GetDeviceCaps", d_GetDeviceCaps },
        { "GetDisplayMode", d_GetDisplayMode }, { "GetCreationParameters", d_GetCreationParameters },
        { "SetCursorProperties", m_Zero }, { "SetCursorPosition", m_Zero }, { "ShowCursor", m_Zero },
        { "GetNumberOfSwapChains", d_GetNumberOfSwapChains }, { "Reset", d_Reset }, { "Present", d_Present },
        { "GetBackBuffer", d_GetBackBuffer }, { "GetRasterStatus", d_GetRasterStatus }, { "SetDialogBoxMode", m_Zero },
        { "SetGammaRamp", m_Zero }, { "GetGammaRamp", m_Zero }, { "CreateTexture", d_CreateTexture },
        { "CreateVertexBuffer", d_CreateVertexBuffer }, { "CreateIndexBuffer", d_CreateIndexBuffer },
        { "CreateRenderTarget", d_CreateRenderTarget }, { "CreateDepthStencilSurface", d_CreateDepthStencilSurface },
        { "UpdateTexture", d_UpdateTexture }, { "GetRenderTargetData", d_GetRenderTargetData }, { "StretchRect", d_StretchRect },
        { "CreateOffscreenPlainSurface", d_CreateOffscreenPlainSurface }, { "SetRenderTarget", d_SetRenderTarget },
        { "GetRenderTarget", d_GetRenderTarget }, { "SetDepthStencilSurface", d_SetDepthStencilSurface },
        { "GetDepthStencilSurface", d_GetDepthStencilSurface }, { "BeginScene", d_BeginScene }, { "EndScene", d_EndScene },
        { "Clear", d_Clear }, { "SetTransform", d_SetTransform }, { "GetTransform", d_GetTransform },
        { "MultiplyTransform", d_MultiplyTransform }, { "SetViewport", d_SetViewport }, { "GetViewport", d_GetViewport },
        { "SetMaterial", d_SetMaterial }, { "GetMaterial", d_GetMaterial }, { "SetLight", d_SetLight }, { "GetLight", d_GetLight },
        { "LightEnable", d_LightEnable }, { "GetLightEnable", d_GetLightEnable }, { "SetClipPlane", m_Zero },
        { "SetRenderState", d_SetRenderState }, { "GetRenderState", d_GetRenderState }, { "GetTexture", d_GetTexture },
        { "SetTexture", d_SetTexture }, { "GetTextureStageState", d_GetTextureStageState },
        { "SetTextureStageState", d_SetTextureStageState }, { "GetSamplerState", d_GetSamplerState },
        { "SetSamplerState", d_SetSamplerState }, { "ValidateDevice", d_ValidateDevice }, { "SetScissorRect", d_SetScissorRect },
        { "GetScissorRect", d_GetScissorRect }, { "SetSoftwareVertexProcessing", m_Zero }, { "DrawPrimitive", d_DrawPrimitive },
        { "DrawIndexedPrimitive", d_DrawIndexedPrimitive }, { "DrawPrimitiveUP", d_DrawPrimitiveUP },
        { "DrawIndexedPrimitiveUP", d_DrawIndexedPrimitiveUP }, { "CreateVertexDeclaration", d_CreateVertexDeclaration },
        { "SetVertexDeclaration", d_SetVertexDeclaration }, { "GetVertexDeclaration", d_GetVertexDeclaration },
        { "SetFVF", d_SetFVF }, { "GetFVF", d_GetFVF }, { "SetVertexShader", d_SetVertexShader },
        { "GetVertexShader", d_GetVertexShader }, { "SetStreamSource", d_SetStreamSource }, { "GetStreamSource", d_GetStreamSource },
        { "SetStreamSourceFreq", m_Zero }, { "SetIndices", d_SetIndices }, { "GetIndices", d_GetIndices },
        { "SetPixelShader", d_SetPixelShader }, { "GetPixelShader", d_GetPixelShader }, { "SetNPatchMode", m_Zero },
        { NULL, NULL },
    };
    return COM_VTABLE(IDirect3DDevice9, impls);
}

static void default_states(d3d_device *d)
{
    uint32_t *rs = d->rs;
    rs[D3DRS_ZENABLE] = 1; rs[D3DRS_FILLMODE] = D3DFILL_SOLID; rs[D3DRS_SHADEMODE] = D3DSHADE_GOURAUD; rs[D3DRS_ZWRITEENABLE] = 1;
    rs[D3DRS_SRCBLEND] = D3DBLEND_ONE; rs[D3DRS_DESTBLEND] = D3DBLEND_ZERO; rs[D3DRS_CULLMODE] = D3DCULL_CCW; rs[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
    rs[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS; rs[D3DRS_FOGEND] = 0x3F800000; rs[D3DRS_FOGDENSITY] = 0x3F800000; rs[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
    rs[D3DRS_STENCILMASK] = 0xFFFFFFFF; rs[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFF; rs[D3DRS_TEXTUREFACTOR] = 0xFFFFFFFF;
    rs[D3DRS_LIGHTING] = 1; rs[D3DRS_COLORVERTEX] = 1; rs[D3DRS_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1;
    rs[D3DRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2; rs[D3DRS_COLORWRITEENABLE] = 0xF; rs[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
    for (int s = 0; s < MAX_STAGES; s++) {
        d->tss[s][D3DTSS_COLOROP] = s == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE;
        d->tss[s][D3DTSS_COLORARG1] = D3DTA_TEXTURE; d->tss[s][D3DTSS_COLORARG2] = D3DTA_CURRENT;
        d->tss[s][D3DTSS_ALPHAOP] = s == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
        d->tss[s][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE; d->tss[s][D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
        d->tss[s][D3DTSS_TEXCOORDINDEX] = (uint32_t)s;
        d->ss[s][D3DSAMP_ADDRESSU] = d->ss[s][D3DSAMP_ADDRESSV] = D3DTADDRESS_WRAP;
        d->ss[s][D3DSAMP_MAGFILTER] = d->ss[s][D3DSAMP_MINFILTER] = D3DTEXF_POINT;
        static const float id[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        memcpy(d->texm[s], id, 64);
    }
    static const float id[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    memcpy(d->world, id, 64); memcpy(d->view, id, 64); memcpy(d->proj, id, 64);
    d->viewport[0] = 0; d->viewport[1] = 0; d->viewport[2] = d->width; d->viewport[3] = d->height;
    float one = 1.0f;
    memcpy(&d->viewport[5], &one, 4);
}

/* -------------------------------------------------------------- IDirect3D9 --- */

static uint32_t m_GetAdapterCount(CPU *c) { (void)c; return 1; }
static uint32_t m_GetAdapterIdentifier(CPU *c)
{
    uint32_t p = ARG(3);
    memset(G2H(p), 0, 1100);
    strcpy((char *)G2H(p), "tmnt-gl");
    strcpy((char *)G2H(p + 512), "TMNT translated OpenGL device");
    strcpy((char *)G2H(p + 1024), "\\\\.\\DISPLAY1");
    st32(p + 1064, 0x10DE); st32(p + 1068, 0x0322);
    return D3D_OK;
}
static const uint32_t modes[][2] = { { 640, 480 }, { 800, 600 }, { 960, 544 }, { 1024, 768 } };
static uint32_t m_GetAdapterModeCount(CPU *c) { return (ARG(2) == D3DFMT_X8R8G8B8 || ARG(2) == D3DFMT_R5G6B5) ? 4 : 0; }
static uint32_t m_EnumAdapterModes(CPU *c)
{
    uint32_t i = ARG(3), p = ARG(4);
    if (i >= 4) return D3DERR_INVALIDCALL;
    st32(p, modes[i][0]); st32(p + 4, modes[i][1]); st32(p + 8, 60); st32(p + 12, ARG(2));
    return D3D_OK;
}
static uint32_t m_GetAdapterDisplayMode(CPU *c)
{
    uint32_t p = ARG(2);
    st32(p, 1024); st32(p + 4, 768); st32(p + 8, 60); st32(p + 12, D3DFMT_X8R8G8B8);
    return D3D_OK;
}
static uint32_t m_CheckDeviceType(CPU *c) { (void)c; return D3D_OK; }
static uint32_t m_CheckDeviceFormat(CPU *c)
{
    switch (ARG(6)) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4: case D3DFMT_DXT1: case D3DFMT_DXT3: case D3DFMT_DXT5: case D3DFMT_D16: case D3DFMT_D24S8: case D3DFMT_D24X8:
    case D3DFMT_L8: case D3DFMT_A8L8: case D3DFMT_A8:
        return D3D_OK;
    default:
        if (trace_d3d) rt_log("[d3d] CheckDeviceFormat(%x, usage %x) -> not available\n", ARG(6), ARG(4));
        return D3DERR_NOTAVAILABLE;
    }
}
static uint32_t m_CheckDeviceMultiSampleType(CPU *c) { if (ARG(6)) st32(ARG(6), 1); return ARG(5) == 0 ? D3D_OK : D3DERR_NOTAVAILABLE; }
static uint32_t m_GetDeviceCaps(CPU *c) { fill_caps(ARG(3)); return D3D_OK; }
static uint32_t m_GetAdapterMonitor(CPU *c) { (void)c; return 0x00050000U; }
static uint32_t m_CreateDevice(CPU *c)
{
    uint32_t pp = ARG(5);
    uint32_t w = ld32(pp), h = ld32(pp + 4);
    if (!w || !h) { w = 640; h = 480; }
    rt_log("[d3d] CreateDevice %ux%u fmt %x windowed %u depth %u fmt %x flags %x interval %x\n", w, h, ld32(pp + 8), ld32(pp + 32),
           ld32(pp + 36), ld32(pp + 40), ARG(4), ld32(pp + 52));
    if (!plat_video_init((int)w, (int)h)) return D3DERR_NOTAVAILABLE;
    if (ld32(pp + 8) == D3DFMT_UNKNOWN) st32(pp + 8, D3DFMT_X8R8G8B8);
    st32(pp, w);
    st32(pp + 4, h);
    d3d_device *d = calloc(1, sizeof *d);
    d->kind = K_DEVICE;
    d->refs = 1;
    d->width = w;
    d->height = h;
    d->d3d_guest = ARG(0);
    default_states(d);
    d->backbuffer = surface_new(D3DFMT_X8R8G8B8, w, h, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, 0);
    d->backbuffer->is_backbuffer = 1;
    d->depth = surface_new(D3DFMT_D24S8, w, h, D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, 0);
    d->depth->is_depth = 1;
    d->guest = com_new(dev_vtbl(), 16, d);
    device = d;
    gl_init_device(d);
    st32(ARG(6), d->guest);
    return D3D_OK;
}

/* Native callers of the device (game_tmnt.c): the same effects as the COM methods, without the guest call and thunk. */
int d3d9_is_device(uint32_t guest)
{
    /* Our device, still with our method table (the replacement calls the methods the table would). */
    return device && guest == device->guest && ld32(guest) == ld32(device->guest);
}

void d3d9_set_render_state(uint32_t state, uint32_t value)
{
    if (state < 256) { device->rs[state] = value; DEV_TOUCH(device, rs[state]); }
}

void d3d9_set_texture_stage_state(uint32_t stage, uint32_t type, uint32_t value)
{
    if (stage < MAX_STAGES && type < 33) { device->tss[stage][type] = value; DEV_TOUCH(device, tss[stage][type]); }
}

/* TMNT_HLE_CHECK: the render and texture stage states, saved and put back around the native and translated runs. */
size_t d3d9_states_size(void) { return sizeof device->rs + sizeof device->tss; }
void d3d9_states_save(uint8_t *buf)
{
    memcpy(buf, device->rs, sizeof device->rs);
    memcpy(buf + sizeof device->rs, device->tss, sizeof device->tss);
}
void d3d9_states_load(const uint8_t *buf)
{
    memcpy(device->rs, buf, sizeof device->rs);
    memcpy(device->tss, buf + sizeof device->rs, sizeof device->tss);
    DEV_TOUCH(device, rs);
    DEV_TOUCH(device, tss);
}

uint32_t d3d9_create(void)
{
    static const com_impl impls[] = {
        { "QueryInterface", m_QueryInterface }, { "AddRef", m_AddRef }, { "Release", m_Release },
        { "RegisterSoftwareDevice", m_Zero }, { "GetAdapterCount", m_GetAdapterCount },
        { "GetAdapterIdentifier", m_GetAdapterIdentifier }, { "GetAdapterModeCount", m_GetAdapterModeCount },
        { "EnumAdapterModes", m_EnumAdapterModes }, { "GetAdapterDisplayMode", m_GetAdapterDisplayMode },
        { "CheckDeviceType", m_CheckDeviceType }, { "CheckDeviceFormat", m_CheckDeviceFormat },
        { "CheckDeviceMultiSampleType", m_CheckDeviceMultiSampleType }, { "CheckDepthStencilMatch", m_CheckDeviceType },
        { "CheckDeviceFormatConversion", m_CheckDeviceType }, { "GetDeviceCaps", m_GetDeviceCaps },
        { "GetAdapterMonitor", m_GetAdapterMonitor }, { "CreateDevice", m_CreateDevice }, { NULL, NULL },
    };
    trace_d3d = getenv("TMNT_TRACE_D3D") != NULL;
    static d3d_any obj = { K_D3D, 0, 1 };
    if (!d3d_object)
        d3d_object = obj.guest = com_new(COM_VTABLE(IDirect3D9, impls), 16, &obj);
    return d3d_object;
}
