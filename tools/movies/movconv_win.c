/* movconv for Windows: the same TMNT_Vff_ee.avc files as movconv.swift (format described there), with the H.264 encoder that
 * comes with Windows (Media Foundation) instead of VideoToolbox. Every movie is decoded back with the Windows H.264 decoder and
 * compared with pl_mpeg's frames. Prints one line per movie, starting with its name (the builder counts them).
 *   movconv <game dir> <out dir> [V00_00 ...]
 *   movconv --self-test <out dir>      (synthetic frames: checks the encoder and decoder of this Windows without the game)
 * Build: zig cc -target x86_64-windows-gnu -O2 movconv_win.c plm_frames.c -lmfplat -lmfuuid -lole32 -lshell32 -o movconv.exe */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <shellapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <codecapi.h>
#include <wmcodecdsp.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plm_frames.h"

/* mingw's codecapi.h names its GUIDs with __uuidof, which C lacks: the same GUIDs from its STATIC_ values. */
#define GUID_FROM(name, ...) DEFINE_GUID(name, __VA_ARGS__)
GUID_FROM(B_PICTURE_COUNT, STATIC_CODECAPI_AVEncMPVDefaultBPictureCount);
GUID_FROM(GOP_SIZE, STATIC_CODECAPI_AVEncMPVGOPSize);
GUID_FROM(CABAC_ENABLE, STATIC_CODECAPI_AVEncH264CABACEnable);
GUID_FROM(RATE_CONTROL_MODE, STATIC_CODECAPI_AVEncCommonRateControlMode);
GUID_FROM(MEAN_BIT_RATE, STATIC_CODECAPI_AVEncCommonMeanBitRate);
GUID_FROM(MAX_BIT_RATE, STATIC_CODECAPI_AVEncCommonMaxBitRate);
GUID_FROM(QUALITY_VS_SPEED, STATIC_CODECAPI_AVEncCommonQualityVsSpeed);
GUID_FROM(LOW_LATENCY_MODE, STATIC_CODECAPI_AVLowLatencyMode);

#define BITRATE 3000000U       /* average, as movconv.swift */
#define PEAK_BITRATE 12000000U /* its limit of 1.5 MB in any second */
#define GOP 30                 /* a keyframe at least every second */
#define MAX_PATH_U8 2048

static char failure[512];
static int fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(failure, sizeof failure, fmt, ap);
    va_end(ap);
    return 0;
}
#define CHECK(hr, what) do { HRESULT h_ = (hr); if (FAILED(h_)) return fail("%s: 0x%08lx", what, (unsigned long)h_); } while (0)

/* ------------------------------------------------------------ bytes --- */

typedef struct { uint8_t *p; size_t n, cap; } bytes;

static void put(bytes *b, const void *src, size_t n)
{
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 4096;
        b->p = realloc(b->p, b->cap);
        if (!b->p) { fputs("out of memory\n", stdout); exit(1); }
    }
    memcpy(b->p + b->n, src, n);
    b->n += n;
}

static void put32(bytes *b, uint32_t v)
{
    uint8_t le[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    put(b, le, 4);
}

static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static const uint8_t start_code[4] = { 0, 0, 0, 1 };

/* NAL units of an Annex-B access unit: calls fn(payload, size) for each (without the start code). */
typedef void (*nal_fn)(const uint8_t *nal, size_t size, void *user);
static void each_nal(const uint8_t *b, size_t n, nal_fn fn, void *user)
{
    size_t i = 0, start = (size_t)-1;
    while (i + 3 <= n) {
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) {
            if (start != (size_t)-1) {
                size_t end = i;
                while (end > start && b[end - 1] == 0) end--;   /* trailing zeros belong to the next start code */
                fn(b + start, end - start, user);
            }
            start = i + 3;
            i += 3;
        } else {
            i++;
        }
    }
    if (start != (size_t)-1 && start < n) fn(b + start, n - start, user);
}

/* -------------------------------------------------------------- SPS --- */

/* The fields of the SPS that matter for the Vita decoder (see movconv.swift). */
typedef struct {
    int profile, level, max_ref_frames, poc_type, reorder_frames, dec_frame_buffering;
    uint8_t rbsp[256];
    int rbsp_n, vui_flag_bit, restriction_flag_bit;
} sps_info;

typedef struct { const uint8_t *b; int n, pos, bad; } bit_reader;

static int rd_bit(bit_reader *r)
{
    if (r->pos >= r->n * 8) { r->bad = 1; return 0; }
    int v = (r->b[r->pos >> 3] >> (7 - (r->pos & 7))) & 1;
    r->pos++;
    return v;
}
static int rd_bits(bit_reader *r, int n) { int v = 0; while (n--) v = v << 1 | rd_bit(r); return v; }
static int rd_ue(bit_reader *r)
{
    int zeros = 0;
    while (!rd_bit(r)) { if (++zeros > 31 || r->bad) { r->bad = 1; return 0; } }
    return (1 << zeros) - 1 + rd_bits(r, zeros);
}
static int rd_se(bit_reader *r) { int k = rd_ue(r); return k & 1 ? (k + 1) / 2 : -(k / 2); }

/* RBSP of a NAL: without the header byte and the emulation prevention bytes. */
static int unescape(const uint8_t *nal, size_t n, uint8_t *out, int cap)
{
    int k = 0, zeros = 0;
    for (size_t i = 1; i < n; i++) {
        if (zeros >= 2 && nal[i] == 3) { zeros = 0; continue; }
        if (k == cap) return -1;
        out[k++] = nal[i];
        zeros = nal[i] == 0 ? zeros + 1 : 0;
    }
    return k;
}

static void skip_hrd(bit_reader *r)
{
    int count = rd_ue(r) + 1;
    rd_bits(r, 8);
    for (int i = 0; i < count; i++) { rd_ue(r); rd_ue(r); rd_bit(r); }
    rd_bits(r, 20);
}

static int parse_sps(const uint8_t *nal, size_t n, sps_info *s)
{
    memset(s, 0, sizeof *s);
    s->poc_type = s->reorder_frames = s->dec_frame_buffering = s->vui_flag_bit = s->restriction_flag_bit = -1;
    s->rbsp_n = unescape(nal, n, s->rbsp, sizeof s->rbsp);
    if (s->rbsp_n < 4) return fail("SPS too short or too long");
    bit_reader r = { s->rbsp, s->rbsp_n, 0, 0 };
    s->profile = rd_bits(&r, 8);
    rd_bits(&r, 8);
    s->level = rd_bits(&r, 8);
    rd_ue(&r);
    static const int high[] = { 100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135 };
    for (int i = 0; i < (int)(sizeof high / sizeof high[0]); i++)
        if (s->profile == high[i]) return fail("profile %d is not Main", s->profile);
    rd_ue(&r);   /* log2_max_frame_num_minus4 */
    s->poc_type = rd_ue(&r);
    if (s->poc_type == 0) {
        rd_ue(&r);
    } else if (s->poc_type == 1) {
        rd_bit(&r);
        rd_se(&r);
        rd_se(&r);
        int k = rd_ue(&r);
        while (k-- > 0 && !r.bad) rd_se(&r);
    }
    s->max_ref_frames = rd_ue(&r);
    rd_bit(&r);
    rd_ue(&r);
    rd_ue(&r);
    if (!rd_bit(&r)) rd_bit(&r);   /* frame_mbs_only, mb_adaptive_frame_field */
    rd_bit(&r);
    if (rd_bit(&r)) for (int i = 0; i < 4; i++) rd_ue(&r);
    s->vui_flag_bit = r.pos;
    if (!rd_bit(&r)) return r.bad ? fail("SPS ends early") : 1;   /* no VUI */
    if (rd_bit(&r) && rd_bits(&r, 8) == 255) rd_bits(&r, 32);
    if (rd_bit(&r)) rd_bit(&r);
    if (rd_bit(&r)) { rd_bits(&r, 4); if (rd_bit(&r)) rd_bits(&r, 24); }
    if (rd_bit(&r)) { rd_ue(&r); rd_ue(&r); }
    if (rd_bit(&r)) { rd_bits(&r, 32); rd_bits(&r, 32); rd_bit(&r); }
    int nal_hrd = rd_bit(&r);
    if (nal_hrd) skip_hrd(&r);
    int vcl_hrd = rd_bit(&r);
    if (vcl_hrd) skip_hrd(&r);
    if (nal_hrd || vcl_hrd) rd_bit(&r);
    rd_bit(&r);
    s->restriction_flag_bit = r.pos;
    if (rd_bit(&r)) {
        rd_bit(&r);
        for (int i = 0; i < 4; i++) rd_ue(&r);
        s->reorder_frames = rd_ue(&r);
        s->dec_frame_buffering = rd_ue(&r);
    }
    return r.bad ? fail("SPS ends early") : 1;
}

typedef struct { uint8_t b[256]; int n; } bit_writer;

static void wr_bit(bit_writer *w, int v)
{
    if (w->n / 8 >= (int)sizeof w->b) return;
    if (w->n % 8 == 0) w->b[w->n / 8] = 0;
    if (v) w->b[w->n / 8] |= (uint8_t)(0x80 >> (w->n % 8));
    w->n++;
}
static void wr_ue(bit_writer *w, int v)
{
    unsigned x = (unsigned)v + 1;
    int len = 0;
    while ((x >> len) > 1) len++;
    for (int i = 0; i < len; i++) wr_bit(w, 0);
    for (int i = len; i >= 0; i--) wr_bit(w, (x >> i) & 1);
}

/* The SPS with a VUI bitstream_restriction saying there is no reordering (as spsWithoutReordering in movconv.swift). */
static int sps_without_reordering(const uint8_t *nal, size_t n, bytes *out, sps_info *info)
{
    if (!parse_sps(nal, n, info)) return 0;
    if (info->reorder_frames == 0) { put(out, nal, n); return 1; }
    if (info->reorder_frames > 0) return fail("the stream reorders frames (%d)", info->reorder_frames);
    bit_reader src = { info->rbsp, info->rbsp_n, 0, 0 };
    bit_writer w = { { 0 }, 0 };
    int cut = info->restriction_flag_bit >= 0 ? info->restriction_flag_bit : info->vui_flag_bit;
    for (int i = 0; i < cut; i++) wr_bit(&w, rd_bit(&src));
    if (info->restriction_flag_bit < 0) {   /* no VUI: one with nothing but the restriction */
        wr_bit(&w, 1);
        for (int i = 0; i < 8; i++) wr_bit(&w, 0);
    }
    wr_bit(&w, 1);    /* bitstream_restriction_flag */
    wr_bit(&w, 1);    /* motion_vectors_over_pic_boundaries_flag */
    wr_ue(&w, 2);     /* max_bytes_per_pic_denom */
    wr_ue(&w, 1);     /* max_bits_per_mb_denom */
    wr_ue(&w, 16);    /* log2_max_mv_length_horizontal */
    wr_ue(&w, 16);    /* log2_max_mv_length_vertical */
    wr_ue(&w, 0);     /* max_num_reorder_frames */
    wr_ue(&w, info->max_ref_frames > 1 ? info->max_ref_frames : 1);   /* max_dec_frame_buffering */
    wr_bit(&w, 1);    /* rbsp_stop_one_bit */
    while (w.n % 8) wr_bit(&w, 0);
    put(out, nal, 1);
    int zeros = 0;
    for (int i = 0; i < w.n / 8; i++) {
        uint8_t byte = w.b[i];
        if (zeros >= 2 && byte <= 3) { uint8_t e = 3; put(out, &e, 1); zeros = 0; }
        put(out, &byte, 1);
        zeros = byte == 0 ? zeros + 1 : 0;
    }
    return parse_sps(out->p, out->n, info) && info->reorder_frames == 0 ? 1 : fail("the rewritten SPS does not parse");
}

/* ------------------------------------------------------- frame source --- */

/* The game's movie (pl_mpeg) or, for --self-test, moving synthetic pictures. */
typedef struct {
    pm_movie *movie;
    int w, h, synthetic_left, synthetic_k;
} source;

static int next_frame(source *s, uint8_t *nv12)
{
    if (s->movie) return pm_next_nv12(s->movie, nv12, s->w, nv12 + (size_t)s->w * s->h, s->w);
    if (s->synthetic_left-- <= 0) return 0;
    int k = s->synthetic_k++;
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++)
            nv12[(size_t)y * s->w + x] = (uint8_t)(16 + ((x + 3 * k) ^ (y + k)) % 200);
    for (int y = 0; y < s->h / 2; y++)
        for (int x = 0; x < s->w / 2; x++) {
            nv12[(size_t)s->w * s->h + (size_t)y * s->w + 2 * x] = (uint8_t)(128 + (x - k) % 60);
            nv12[(size_t)s->w * s->h + (size_t)y * s->w + 2 * x + 1] = (uint8_t)(128 - (y + k) % 60);
        }
    return 1;
}

/* ------------------------------------------------------- MF helpers --- */

static HRESULT set_size(IMFMediaType *t, REFGUID key, UINT32 hi, UINT32 lo)
{
    return IMFMediaType_SetUINT64(t, key, (UINT64)hi << 32 | lo);
}

static HRESULT codec_set(ICodecAPI *api, const GUID *key, VARTYPE vt, ULONG value)
{
    VARIANT v;
    VariantInit(&v);
    v.vt = vt;
    if (vt == VT_BOOL) v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    else v.ulVal = value;
    return ICodecAPI_SetValue(api, key, &v);
}

static IMFSample *make_sample(const uint8_t *data, DWORD size, LONGLONG time, LONGLONG duration)
{
    IMFSample *sample = NULL;
    IMFMediaBuffer *buf = NULL;
    BYTE *p;
    if (FAILED(MFCreateSample(&sample)) || FAILED(MFCreateMemoryBuffer(size, &buf))) goto bad;
    if (data) {
        if (FAILED(IMFMediaBuffer_Lock(buf, &p, NULL, NULL))) goto bad;
        memcpy(p, data, size);
        IMFMediaBuffer_Unlock(buf);
        IMFMediaBuffer_SetCurrentLength(buf, size);
    }
    IMFSample_AddBuffer(sample, buf);
    IMFMediaBuffer_Release(buf);
    if (time >= 0) { IMFSample_SetSampleTime(sample, time); IMFSample_SetSampleDuration(sample, duration); }
    return sample;
bad:
    if (buf) IMFMediaBuffer_Release(buf);
    if (sample) IMFSample_Release(sample);
    return NULL;
}

/* One output of an MFT: 1 with *out (to release), 0 when it needs more input, -1 on error; stream changes renegotiate. */
typedef int (*renegotiate_fn)(IMFTransform *t);
static int pull_output(IMFTransform *t, IMFSample **out, renegotiate_fn renegotiate)
{
    for (;;) {
        MFT_OUTPUT_STREAM_INFO info;
        if (FAILED(IMFTransform_GetOutputStreamInfo(t, 0, &info))) return fail("GetOutputStreamInfo"), -1;
        IMFSample *mine = NULL;
        if (!(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
            mine = make_sample(NULL, info.cbSize ? info.cbSize : 4 << 20, -1, 0);
            if (!mine) return fail("output sample"), -1;
        }
        MFT_OUTPUT_DATA_BUFFER db = { 0 };
        db.pSample = mine;
        DWORD status = 0;
        HRESULT hr = IMFTransform_ProcessOutput(t, 0, 1, &db, &status);
        if (db.pEvents) IMFCollection_Release(db.pEvents);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) { if (mine) IMFSample_Release(mine); return 0; }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (db.pSample && db.pSample != mine) IMFSample_Release(db.pSample);
            if (mine) IMFSample_Release(mine);
            if (!renegotiate || !renegotiate(t)) return fail("stream change"), -1;
            continue;
        }
        if (FAILED(hr)) { if (mine) IMFSample_Release(mine); return fail("ProcessOutput: 0x%08lx", (unsigned long)hr), -1; }
        *out = db.pSample;
        return 1;
    }
}

static int sample_bytes(IMFSample *s, bytes *out)
{
    IMFMediaBuffer *buf;
    BYTE *p;
    DWORD n;
    if (FAILED(IMFSample_ConvertToContiguousBuffer(s, &buf))) return fail("ConvertToContiguousBuffer");
    if (FAILED(IMFMediaBuffer_Lock(buf, &p, NULL, &n))) { IMFMediaBuffer_Release(buf); return fail("Lock"); }
    out->n = 0;
    put(out, p, n);
    IMFMediaBuffer_Unlock(buf);
    IMFMediaBuffer_Release(buf);
    return 1;
}

/* ---------------------------------------------------------- encoding --- */

typedef struct { bytes au; int key; } frame_au;

typedef struct {
    frame_au *aus;
    int n, cap;
    bytes sps, pps;
    int slices_max;
} encoded;

typedef struct { encoded *e; bytes *body; int key, slices, bframe, bad; } split_state;

static void split_nal(const uint8_t *nal, size_t size, void *user)
{
    split_state *s = user;
    int type = nal[0] & 0x1f;
    if (type == 7 || type == 8) {   /* parameter sets: kept once, written before each keyframe */
        bytes *keep = type == 7 ? &s->e->sps : &s->e->pps;
        if (keep->n == 0) put(keep, nal, size);
        else if (keep->n != size || memcmp(keep->p, nal, size) != 0) s->bad = 1;
        return;
    }
    if (type == 9) return;   /* access unit delimiter */
    if (type == 1 || type == 5) {
        s->slices++;
        if (type == 5) s->key = 1;
        uint8_t rbsp[16];
        int n = unescape(nal, size < 17 ? size : 17, rbsp, sizeof rbsp);
        bit_reader r = { rbsp, n > 0 ? n : 0, 0, 0 };
        rd_ue(&r);   /* first_mb_in_slice */
        if (rd_ue(&r) % 5 == 1) s->bframe = 1;
    }
    put(s->body, start_code, 4);
    put(s->body, nal, size);
}

static int keep_au(encoded *e, IMFSample *sample)
{
    bytes raw = { 0 };
    if (!sample_bytes(sample, &raw)) return 0;
    if (e->n == e->cap) {
        e->cap = e->cap * 2 + 256;
        e->aus = realloc(e->aus, sizeof *e->aus * e->cap);
        if (!e->aus) { free(raw.p); return fail("out of memory"); }
    }
    frame_au *f = &e->aus[e->n];
    memset(f, 0, sizeof *f);
    split_state s = { e, &f->au, 0, 0, 0, 0 };
    each_nal(raw.p, raw.n, split_nal, &s);
    free(raw.p);
    if (s.bad) return fail("the encoder changed its parameter sets");
    if (s.bframe) return fail("the encoder made B-frames");
    if (s.slices == 0) return fail("frame %d has no picture", e->n);
    f->key = s.key;
    if (s.slices > e->slices_max) e->slices_max = s.slices;
    e->n++;
    return 1;
}

static int encode(source *src, int num, int den, encoded *e)
{
    IMFTransform *enc = NULL;
    ICodecAPI *api = NULL;
    IMFMediaType *t = NULL;
    CHECK(CoCreateInstance(&CLSID_CMSH264EncoderMFT, NULL, CLSCTX_INPROC_SERVER, &IID_IMFTransform, (void **)&enc),
          "the Windows H.264 encoder is missing (an N edition of Windows needs the Media Feature Pack)");
    if (SUCCEEDED(IMFTransform_QueryInterface(enc, &IID_ICodecAPI, (void **)&api))) {
        /* No B-frames: the decoder hands out pictures in the order they go in, so frame k is the k-th picture. */
        codec_set(api, &B_PICTURE_COUNT, VT_UI4, 0);
        codec_set(api, &GOP_SIZE, VT_UI4, GOP);
        codec_set(api, &CABAC_ENABLE, VT_BOOL, 1);
        codec_set(api, &RATE_CONTROL_MODE, VT_UI4, eAVEncCommonRateControlMode_PeakConstrainedVBR);
        codec_set(api, &MEAN_BIT_RATE, VT_UI4, BITRATE);
        codec_set(api, &MAX_BIT_RATE, VT_UI4, PEAK_BITRATE);
        codec_set(api, &QUALITY_VS_SPEED, VT_UI4, 100);
        codec_set(api, &LOW_LATENCY_MODE, VT_BOOL, 0);
    }
    /* Output before input (the encoder requires it): what the Vita decodes in hardware, Main 3.1. */
    CHECK(MFCreateMediaType(&t), "MFCreateMediaType");
    IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &MFVideoFormat_H264);
    IMFMediaType_SetUINT32(t, &MF_MT_AVG_BITRATE, BITRATE);
    set_size(t, &MF_MT_FRAME_SIZE, src->w, src->h);
    set_size(t, &MF_MT_FRAME_RATE, num, den);
    set_size(t, &MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    IMFMediaType_SetUINT32(t, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    IMFMediaType_SetUINT32(t, &MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
    IMFMediaType_SetUINT32(t, &MF_MT_MPEG2_LEVEL, eAVEncH264VLevel3_1);
    CHECK(IMFTransform_SetOutputType(enc, 0, t, 0), "encoder output type");
    IMFMediaType_Release(t);
    CHECK(MFCreateMediaType(&t), "MFCreateMediaType");
    IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &MFVideoFormat_NV12);
    set_size(t, &MF_MT_FRAME_SIZE, src->w, src->h);
    set_size(t, &MF_MT_FRAME_RATE, num, den);
    set_size(t, &MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    IMFMediaType_SetUINT32(t, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    IMFMediaType_SetUINT32(t, &MF_MT_DEFAULT_STRIDE, src->w);
    /* pl_mpeg's pictures are BT.601 video range. */
    IMFMediaType_SetUINT32(t, &MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT601);
    IMFMediaType_SetUINT32(t, &MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    CHECK(IMFTransform_SetInputType(enc, 0, t, 0), "encoder input type");
    IMFMediaType_Release(t);
    /* The sequence header of the output type, in case the samples do not repeat the parameter sets. */
    IMFMediaType *cur = NULL;
    if (SUCCEEDED(IMFTransform_GetOutputCurrentType(enc, 0, &cur))) {
        UINT32 n = 0;
        if (SUCCEEDED(IMFMediaType_GetBlobSize(cur, &MF_MT_MPEG_SEQUENCE_HEADER, &n)) && n) {
            uint8_t *blob = malloc(n);
            if (blob && SUCCEEDED(IMFMediaType_GetBlob(cur, &MF_MT_MPEG_SEQUENCE_HEADER, blob, n, NULL))) {
                encoded probe = { 0 };
                bytes body = { 0 };
                split_state s = { &probe, &body, 0, 0, 0, 0 };
                each_nal(blob, n, split_nal, &s);
                e->sps = probe.sps;
                e->pps = probe.pps;
                free(body.p);
            }
            free(blob);
        }
        IMFMediaType_Release(cur);
    }
    IMFTransform_ProcessMessage(enc, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    IMFTransform_ProcessMessage(enc, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    size_t frame_size = (size_t)src->w * src->h * 3 / 2;
    uint8_t *nv12 = malloc(frame_size);
    LONGLONG duration = 10000000LL * den / num;
    int k = 0, ok = 1;
    while (ok && next_frame(src, nv12)) {
        IMFSample *in = make_sample(nv12, (DWORD)frame_size, k * duration, duration);
        if (!in) { ok = fail("input sample"); break; }
        HRESULT hr;
        while ((hr = IMFTransform_ProcessInput(enc, 0, in, 0)) == MF_E_NOTACCEPTING) {
            IMFSample *out;
            int r = pull_output(enc, &out, NULL);
            if (r < 0) { ok = 0; break; }
            if (r == 0) { ok = fail("the encoder takes no input and gives no output"); break; }
            ok = keep_au(e, out);
            IMFSample_Release(out);
            if (!ok) break;
        }
        IMFSample_Release(in);
        if (ok && FAILED(hr)) ok = fail("ProcessInput: 0x%08lx", (unsigned long)hr);
        for (IMFSample *out; ok;) {
            int r = pull_output(enc, &out, NULL);
            if (r <= 0) { ok = r == 0; break; }
            ok = keep_au(e, out);
            IMFSample_Release(out);
        }
        k++;
    }
    free(nv12);
    if (ok) {
        IMFTransform_ProcessMessage(enc, MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        IMFTransform_ProcessMessage(enc, MFT_MESSAGE_COMMAND_DRAIN, 0);
        for (IMFSample *out; ok;) {
            int r = pull_output(enc, &out, NULL);
            if (r <= 0) { ok = r == 0; break; }
            ok = keep_au(e, out);
            IMFSample_Release(out);
        }
    }
    if (api) ICodecAPI_Release(api);
    IMFTransform_Release(enc);
    if (!ok) return 0;
    if (k == 0 || e->n != k) return fail("%d access units for %d frames", e->n, k);
    if (!e->sps.n || !e->pps.n) return fail("no SPS/PPS from the encoder");
    if (!e->aus[0].key) return fail("the first frame is not a keyframe");
    return 1;
}

/* ------------------------------------------------------------ writing --- */

typedef struct { int frames, keyframes, max_au, w, h; size_t bytes; sps_info sps; } converted;

static FILE *open_u8(const char *path, const wchar_t *mode)
{
    wchar_t w[MAX_PATH_U8];
    return MultiByteToWideChar(CP_UTF8, 0, path, -1, w, MAX_PATH_U8) ? _wfopen(w, mode) : NULL;
}

static int write_avc(const char *path, encoded *e, int w, int h, int num, int den, converted *c)
{
    bytes head = { 0 }, sps = { 0 }, file = { 0 };
    if (!sps_without_reordering(e->sps.p, e->sps.n, &sps, &c->sps)) return 0;
    put(&head, start_code, 4); put(&head, sps.p, sps.n);
    put(&head, start_code, 4); put(&head, e->pps.p, e->pps.n);
    uint32_t index_offset = 64, data_offset = 64 + 12 * (uint32_t)e->n, at = data_offset;
    for (int k = 0; k < e->n; k++) {
        size_t size = e->aus[k].au.n + (e->aus[k].key ? head.n : 0);
        if (size > (size_t)c->max_au) c->max_au = (int)size;
        c->keyframes += e->aus[k].key;
    }
    put(&file, "TMNTAVC1", 8);
    put32(&file, 1); put32(&file, w); put32(&file, h); put32(&file, e->n); put32(&file, num); put32(&file, den);
    put32(&file, c->sps.max_ref_frames); put32(&file, c->max_au); put32(&file, index_offset); put32(&file, data_offset);
    while (file.n < 64) { uint8_t z = 0; put(&file, &z, 1); }
    for (int k = 0; k < e->n; k++) {
        uint32_t size = (uint32_t)(e->aus[k].au.n + (e->aus[k].key ? head.n : 0));
        put32(&file, at); put32(&file, size); put32(&file, e->aus[k].key);
        at += size;
    }
    for (int k = 0; k < e->n; k++) {
        if (e->aus[k].key) put(&file, head.p, head.n);
        put(&file, e->aus[k].au.p, e->aus[k].au.n);
    }
    FILE *f = open_u8(path, L"wb");
    int ok = f && fwrite(file.p, 1, file.n, f) == file.n;
    if (f && fclose(f) != 0) ok = 0;
    c->frames = e->n; c->w = w; c->h = h; c->bytes = file.n;
    free(head.p); free(sps.p); free(file.p);
    return ok ? 1 : fail("cannot write %s", path);
}

/* --------------------------------------------------------- verifying --- */

static int pick_nv12_output(IMFTransform *t)
{
    IMFMediaType *type;
    for (DWORD i = 0; SUCCEEDED(IMFTransform_GetOutputAvailableType(t, 0, i, &type)); i++) {
        GUID sub;
        int nv12 = SUCCEEDED(IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &sub)) && IsEqualGUID(&sub, &MFVideoFormat_NV12);
        HRESULT hr = nv12 ? IMFTransform_SetOutputType(t, 0, type, 0) : E_FAIL;
        IMFMediaType_Release(type);
        if (nv12) return SUCCEEDED(hr);
    }
    return 0;
}

typedef struct { source *src; uint8_t *ref; int w, h, k, n; double min_psnr, sum; } checker;

/* Luma PSNR of decoded frame k against pl_mpeg's frame k. */
static int compare(IMFTransform *dec, IMFSample *s, checker *c)
{
    if (c->k >= c->n) return fail("the decoder gives more frames than the file has (%d)", c->n);
    if (!next_frame(c->src, c->ref)) return fail("pl_mpeg has fewer frames (%d) than the file (%d)", c->k, c->n);
    IMFMediaType *type;
    UINT64 size = 0;
    UINT32 stride = 0;
    if (FAILED(IMFTransform_GetOutputCurrentType(dec, 0, &type))) return fail("decoder output type");
    IMFMediaType_GetUINT64(type, &MF_MT_FRAME_SIZE, &size);
    if (FAILED(IMFMediaType_GetUINT32(type, &MF_MT_DEFAULT_STRIDE, &stride)) || (INT32)stride <= 0) stride = (UINT32)(size >> 32);
    IMFMediaType_Release(type);
    IMFMediaBuffer *buf;
    if (FAILED(IMFSample_ConvertToContiguousBuffer(s, &buf))) return fail("decoded buffer");
    IMF2DBuffer *b2 = NULL;
    BYTE *p;
    LONG pitch = (LONG)stride;
    DWORD len = 0;
    HRESULT hr;
    if (SUCCEEDED(IMFMediaBuffer_QueryInterface(buf, &IID_IMF2DBuffer, (void **)&b2))) hr = IMF2DBuffer_Lock2D(b2, &p, &pitch);
    else hr = IMFMediaBuffer_Lock(buf, &p, NULL, &len);
    if (FAILED(hr)) { if (b2) IMF2DBuffer_Release(b2); IMFMediaBuffer_Release(buf); return fail("decoded lock"); }
    int ok = pitch >= c->w && (b2 || len >= (DWORD)pitch * c->h);
    double se = 0;
    for (int y = 0; ok && y < c->h; y++)
        for (int x = 0; x < c->w; x++) {
            double d = (double)p[(size_t)y * pitch + x] - c->ref[(size_t)y * c->w + x];
            se += d * d;
        }
    if (b2) { IMF2DBuffer_Unlock2D(b2); IMF2DBuffer_Release(b2); } else IMFMediaBuffer_Unlock(buf);
    IMFMediaBuffer_Release(buf);
    if (!ok) return fail("decoded picture smaller than %dx%d", c->w, c->h);
    double mse = se / ((double)c->w * c->h), psnr = mse == 0 ? 99.0 : 10 * log10(255.0 * 255.0 / mse);
    if (psnr < c->min_psnr) c->min_psnr = psnr;
    c->sum += psnr;
    c->k++;
    return 1;
}

static int drain_decoded(IMFTransform *dec, checker *c)
{
    for (IMFSample *out;;) {
        int r = pull_output(dec, &out, pick_nv12_output);
        if (r <= 0) return r == 0;
        int ok = compare(dec, out, c);
        IMFSample_Release(out);
        if (!ok) return 0;
    }
}

/* Decodes the file back in the order the Vita feeds it and compares the luma with the source's frames. */
static int verify(const char *path, source *src, double *min_psnr, double *avg_psnr)
{
    FILE *f = open_u8(path, L"rb");
    if (!f) return fail("cannot read %s", path);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(size);
    int got = d && fread(d, 1, size, f) == (size_t)size;
    fclose(f);
    if (!got || size < 64 || memcmp(d, "TMNTAVC1", 8) != 0) { free(d); return fail("bad file"); }
    int w = get32(d + 12), h = get32(d + 16), n = get32(d + 20), num = get32(d + 24), den = get32(d + 28);
    uint32_t index = get32(d + 40);
    IMFTransform *dec = NULL;
    IMFMediaType *t = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_CMSH264DecoderMFT, NULL, CLSCTX_INPROC_SERVER, &IID_IMFTransform, (void **)&dec);
    if (FAILED(hr)) { free(d); return fail("the Windows H.264 decoder is missing: 0x%08lx", (unsigned long)hr); }
    MFCreateMediaType(&t);
    IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &MFVideoFormat_H264);
    set_size(t, &MF_MT_FRAME_SIZE, w, h);
    set_size(t, &MF_MT_FRAME_RATE, num, den);
    IMFMediaType_SetUINT32(t, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = IMFTransform_SetInputType(dec, 0, t, 0);
    IMFMediaType_Release(t);
    checker c = { src, malloc((size_t)w * h * 3 / 2), w, h, 0, n, 1000.0, 0 };
    int ok = SUCCEEDED(hr) ? pick_nv12_output(dec) || fail("decoder: no NV12 output") : fail("decoder input type: 0x%08lx",
                                                                                              (unsigned long)hr);
    if (ok) {
        IMFTransform_ProcessMessage(dec, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        IMFTransform_ProcessMessage(dec, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }
    LONGLONG duration = 10000000LL * den / num;
    for (int k = 0; ok && k < n; k++) {
        uint32_t off = get32(d + index + 12 * k), len = get32(d + index + 12 * k + 4);
        if ((size_t)off + len > (size_t)size) { ok = fail("frame %d is outside the file", k); break; }
        IMFSample *in = make_sample(d + off, len, k * duration, duration);
        if (!in) { ok = fail("decoder input sample"); break; }
        while (ok && (hr = IMFTransform_ProcessInput(dec, 0, in, 0)) == MF_E_NOTACCEPTING) ok = drain_decoded(dec, &c);
        IMFSample_Release(in);
        if (ok && FAILED(hr)) ok = fail("frame %d did not decode: 0x%08lx", k, (unsigned long)hr);
        if (ok) ok = drain_decoded(dec, &c);
    }
    if (ok) {
        IMFTransform_ProcessMessage(dec, MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        IMFTransform_ProcessMessage(dec, MFT_MESSAGE_COMMAND_DRAIN, 0);
        ok = drain_decoded(dec, &c);
    }
    if (ok && c.k != n) ok = fail("the decoder gave %d frames of %d", c.k, n);
    if (ok && next_frame(src, c.ref)) ok = fail("pl_mpeg has more frames than the file (%d)", n);
    IMFTransform_Release(dec);
    free(c.ref);
    free(d);
    *min_psnr = c.min_psnr;
    *avg_psnr = n ? c.sum / n : 0;
    return ok;
}

/* -------------------------------------------------------------- main --- */

static void fps_fraction(double fps, int *num, int *den)
{
    if (fabs(fps - 30000.0 / 1001.0) < 0.01) { *num = 30000; *den = 1001; }
    else if (fabs(fps - 24000.0 / 1001.0) < 0.01) { *num = 24000; *den = 1001; }
    else { *num = (int)(fps * 1000 + 0.5); *den = 1000; }
}

static void free_encoded(encoded *e)
{
    for (int k = 0; k < e->n; k++) free(e->aus[k].au.p);
    free(e->aus); free(e->sps.p); free(e->pps.p);
    memset(e, 0, sizeof *e);
}

/* One movie (dat, entry) or the synthetic one (dat NULL): convert, write, verify, print its line. */
static int one_movie(const char *name, const char *dat, int entry, const char *out)
{
    DWORD t0 = GetTickCount();
    source src = { 0 };
    double fps = 30000.0 / 1001.0;
    if (dat) {
        src.movie = pm_open(dat, entry);
        if (!src.movie) return fail("pl_mpeg cannot open the movie");
        pm_size(src.movie, &src.w, &src.h, &fps);
    } else {
        src.w = 640; src.h = 448; src.synthetic_left = 150;
    }
    int num, den;
    fps_fraction(fps, &num, &den);
    encoded e = { 0 };
    converted c = { 0 };
    int ok = encode(&src, num, den, &e) && write_avc(out, &e, src.w, src.h, num, den, &c);
    int slices = e.slices_max;
    free_encoded(&e);
    if (src.movie) pm_close(src.movie);
    if (!ok) return 0;
    source again = { dat ? pm_open(dat, entry) : NULL, src.w, src.h, dat ? 0 : 150, 0 };
    if (dat && !again.movie) return fail("pl_mpeg cannot open the movie");
    double min_p = 0, avg_p = 0;
    ok = verify(out, &again, &min_p, &avg_p);
    if (again.movie) pm_close(again.movie);
    if (!ok) return 0;
    printf("%s: %dx%d %.2f fps, %d frames (%.1f s), %d keyframes, %.1f MB, largest frame %d KB, %d slices; "
           "SPS profile %d level %d refs %d poc %d reorder %d buffering %d; check %d frames, luma PSNR min %.1f avg %.1f dB (%.0f s)\n",
           name, c.w, c.h, fps, c.frames, c.frames / fps, c.keyframes, c.bytes / 1e6, c.max_au / 1024, slices,
           c.sps.profile, c.sps.level, c.sps.max_ref_frames, c.sps.poc_type, c.sps.reorder_frames, c.sps.dec_frame_buffering,
           c.frames, min_p, avg_p, (GetTickCount() - t0) / 1000.0);
    fflush(stdout);
    return 1;
}

static void to_u8(const wchar_t *w, char *out)
{
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, MAX_PATH_U8, NULL, NULL)) out[0] = 0;
}

int main(void)
{
    int argc;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wargv || argc < 3) {
        puts("usage: movconv <game dir> <out dir> [V00_00 ...]\n       movconv --self-test <out dir>");
        return 2;
    }
    static char args[16][MAX_PATH_U8];
    int nargs = argc < 16 ? argc : 16;
    for (int i = 0; i < nargs; i++) to_u8(wargv[i], args[i]);
    CreateDirectoryW(wargv[2], NULL);
    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED)) || FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        puts("Media Foundation is not available on this Windows (an N edition needs the Media Feature Pack)");
        return 1;
    }
    int failed = 0;
    if (strcmp(args[1], "--self-test") == 0) {
        char out[MAX_PATH_U8 + 32];
        snprintf(out, sizeof out, "%s/self_test.avc", args[2]);
        if (!one_movie("self-test", NULL, 0, out)) { printf("self-test: FAILED: %s\n", failure); failed = 1; }
    } else {
        for (int file = 0; file < 3; file++) {
            char dat[MAX_PATH_U8 + 32];
            snprintf(dat, sizeof dat, "%s/TMNT_V0%d.DAT", args[1], file);
            int count = pm_entry_count(dat);
            for (int entry = 0; entry < count; entry++) {
                char name[16], out[MAX_PATH_U8 + 32];
                snprintf(name, sizeof name, "V%02d_%02d", file, entry);
                int wanted = nargs <= 3;
                for (int i = 3; i < nargs; i++) wanted |= strcmp(args[i], name) == 0;
                if (!wanted) continue;
                snprintf(out, sizeof out, "%s/TMNT_%s.avc", args[2], name);
                failure[0] = 0;
                if (!one_movie(name, dat, entry, out)) {
                    printf("%s: FAILED: %s\n", name, failure);
                    fflush(stdout);
                    failed++;
                }
            }
        }
    }
    printf("Done: %d failed\n", failed);
    MFShutdown();
    CoUninitialize();
    LocalFree(wargv);
    return failed ? 1 : 0;
}
