/* DirectSound: guest-memory sound buffers mixed on the host (play cursors advance with the real output clock). */
#include <math.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "com.h"
#include "dx_consts.h"
#include "osdep.h"
#include "plat.h"

#define DS_OK 0U
#define E_NOINTERFACE 0x80004002U
#define MAX_BUFFERS 128

typedef struct ds_buf {
    int kind;
    uint32_t guest, refs;
    int primary;
    uint32_t mem, bytes;
    uint32_t channels, rate, bits, align;
    uint32_t freq;
    int32_t volume, pan;
    int playing, looping;
    double pos;
} ds_buf;

static ds_buf *buffers[MAX_BUFFERS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int out_rate = 44100;

static uint32_t m_AddRef(CPU *c) { ds_buf *b = com_host(ARG(0)); return b ? ++b->refs : 1; }
static uint32_t m_QueryInterface(CPU *c) { if (ARG(2)) st32(ARG(2), 0); return E_NOINTERFACE; }
static uint32_t m_Ok(CPU *c) { (void)c; return DS_OK; }

static uint32_t b_Release(CPU *c)
{
    ds_buf *b = com_host(ARG(0));
    if (!b) return 0;
    if (b->refs) b->refs--;
    if (b->refs) return b->refs;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_BUFFERS; i++) if (buffers[i] == b) buffers[i] = NULL;
    pthread_mutex_unlock(&lock);
    if (b->mem) rt_free(b->mem);
    com_forget(b->guest);
    free(b);
    return 0;
}
static uint32_t b_GetCaps(CPU *c)
{
    ds_buf *b = THIS_HOST(ds_buf);
    uint32_t p = ARG(1);
    st32(p + 4, b->primary ? 1U : 0U);
    st32(p + 8, b->bytes);
    st32(p + 12, 0); st32(p + 16, 0);
    return DS_OK;
}
static uint32_t b_GetCurrentPosition(CPU *c)
{
    ds_buf *b = THIS_HOST(ds_buf);
    pthread_mutex_lock(&lock);
    uint32_t play = b->bytes ? ((uint32_t)b->pos * b->align) % b->bytes : 0;
    uint32_t lead = b->align * (b->freq ? b->freq : b->rate) / 50;
    pthread_mutex_unlock(&lock);
    if (ARG(1)) st32(ARG(1), play);
    if (ARG(2)) st32(ARG(2), b->bytes ? (play + lead) % b->bytes : 0);
    return DS_OK;
}
static uint32_t b_GetFormat(CPU *c)
{
    ds_buf *b = THIS_HOST(ds_buf);
    uint32_t p = ARG(1);
    if (p && ARG(2) >= 16) {
        st16(p, 1); st16(p + 2, (uint16_t)b->channels); st32(p + 4, b->rate); st32(p + 8, b->rate * b->align);
        st16(p + 12, (uint16_t)b->align); st16(p + 14, (uint16_t)b->bits);
        if (ARG(2) >= 18) st16(p + 16, 0);
    }
    if (ARG(3)) st32(ARG(3), 18);
    return DS_OK;
}
static uint32_t b_GetVolume(CPU *c) { st32(ARG(1), (uint32_t)THIS_HOST(ds_buf)->volume); return DS_OK; }
static uint32_t b_GetPan(CPU *c) { st32(ARG(1), (uint32_t)THIS_HOST(ds_buf)->pan); return DS_OK; }
static uint32_t b_GetFrequency(CPU *c) { ds_buf *b = THIS_HOST(ds_buf); st32(ARG(1), b->freq ? b->freq : b->rate); return DS_OK; }
static uint32_t b_GetStatus(CPU *c)
{
    ds_buf *b = THIS_HOST(ds_buf);
    st32(ARG(1), (b->playing ? 1U : 0U) | (b->playing && b->looping ? 4U : 0U));
    return DS_OK;
}
/* Lock(offset, bytes, ppv1, pn1, ppv2, pn2, flags): the region may wrap around the end of the ring. */
static uint32_t b_Lock(CPU *c)
{
    ds_buf *b = THIS_HOST(ds_buf);
    uint32_t off = ARG(1), n = ARG(2), flags = ARG(7);
    if (flags & 2U) n = b->bytes;                       /* DSBLOCK_ENTIREBUFFER */
    if (!b->bytes) return 0x88780032U;
    if (flags & 1U) {
        pthread_mutex_lock(&lock);
        off = ((uint32_t)b->pos * b->align + b->align * (b->freq ? b->freq : b->rate) / 50) % b->bytes;
        pthread_mutex_unlock(&lock);
    }
    off %= b->bytes;
    if (n > b->bytes) n = b->bytes;
    uint32_t first = n < b->bytes - off ? n : b->bytes - off;
    st32(ARG(3), b->mem + off);
    st32(ARG(4), first);
    if (ARG(5)) st32(ARG(5), n > first ? b->mem : 0);
    if (ARG(6)) st32(ARG(6), n - first);
    return DS_OK;
}
static uint32_t b_Play(CPU *c)
{
    ds_buf *b = THIS_HOST(ds_buf);
    pthread_mutex_lock(&lock);
    b->playing = 1;
    b->looping = (ARG(3) & 1U) != 0;
    pthread_mutex_unlock(&lock);
    return DS_OK;
}
static uint32_t b_SetCurrentPosition(CPU *c)
{
    ds_buf *b = THIS_HOST(ds_buf);
    pthread_mutex_lock(&lock);
    b->pos = b->align ? (double)(ARG(1) / b->align) : 0;
    pthread_mutex_unlock(&lock);
    return DS_OK;
}
static uint32_t b_SetFormat(CPU *c)
{
    ds_buf *b = THIS_HOST(ds_buf);
    uint32_t p = ARG(1);
    if (b->primary && p) { b->channels = ld16(p + 2); b->rate = ld32(p + 4); b->bits = ld16(p + 14); b->align = ld16(p + 12); }
    return DS_OK;
}
static uint32_t b_SetVolume(CPU *c) { THIS_HOST(ds_buf)->volume = (int32_t)ARG(1); return DS_OK; }
static uint32_t b_SetPan(CPU *c) { THIS_HOST(ds_buf)->pan = (int32_t)ARG(1); return DS_OK; }
static uint32_t b_SetFrequency(CPU *c) { THIS_HOST(ds_buf)->freq = ARG(1); return DS_OK; }
static uint32_t b_Stop(CPU *c)
{
    ds_buf *b = THIS_HOST(ds_buf);
    pthread_mutex_lock(&lock);
    b->playing = 0;
    pthread_mutex_unlock(&lock);
    return DS_OK;
}

static uint32_t buffer_vtbl(void)
{
    static const com_impl impls[] = {
        { "QueryInterface", m_QueryInterface }, { "AddRef", m_AddRef }, { "Release", b_Release }, { "GetCaps", b_GetCaps },
        { "GetCurrentPosition", b_GetCurrentPosition }, { "GetFormat", b_GetFormat }, { "GetVolume", b_GetVolume },
        { "GetPan", b_GetPan }, { "GetFrequency", b_GetFrequency }, { "GetStatus", b_GetStatus }, { "Initialize", m_Ok },
        { "Lock", b_Lock }, { "Play", b_Play }, { "SetCurrentPosition", b_SetCurrentPosition }, { "SetFormat", b_SetFormat },
        { "SetVolume", b_SetVolume }, { "SetPan", b_SetPan }, { "SetFrequency", b_SetFrequency }, { "Stop", b_Stop },
        { "Unlock", m_Ok }, { "Restore", m_Ok }, { NULL, NULL },
    };
    return COM_VTABLE(IDirectSoundBuffer, impls);
}

static ds_buf *buffer_new(void)
{
    ds_buf *b = calloc(1, sizeof *b);
    b->refs = 1;
    b->channels = 2; b->rate = 22050; b->bits = 16; b->align = 4;
    b->guest = com_new(buffer_vtbl(), 16, b);
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_BUFFERS; i++) if (!buffers[i]) { buffers[i] = b; break; }
    pthread_mutex_unlock(&lock);
    return b;
}

static uint32_t ds_CreateSoundBuffer(CPU *c)
{
    uint32_t d = ARG(1);
    uint32_t flags = ld32(d + 4), bytes = ld32(d + 8), wf = ld32(d + 16);
    ds_buf *b = buffer_new();
    b->primary = (flags & 1U) != 0;
    if (!b->primary && wf) {
        b->channels = ld16(wf + 2); b->rate = ld32(wf + 4); b->align = ld16(wf + 12); b->bits = ld16(wf + 14);
        if (!b->align) b->align = b->channels * b->bits / 8;
        b->bytes = bytes;
        b->mem = rt_alloc(bytes ? bytes : 4);
        memset(G2H(b->mem), b->bits == 8 ? 0x80 : 0, bytes);
    }
    rt_log("[dsound] CreateSoundBuffer(%s, %u bytes, %u ch, %u Hz, %u bit, flags %x)\n", b->primary ? "primary" : "secondary",
           b->bytes, b->channels, b->rate, b->bits, flags);
    st32(ARG(2), b->guest);
    return DS_OK;
}
static uint32_t ds_GetCaps(CPU *c)
{
    uint32_t p = ARG(1), size = ld32(p);
    memset(G2H(p + 4), 0, size > 4 ? size - 4 : 0);
    st32(p + 4, 0x00000F5FU);
    return DS_OK;
}
static uint32_t ds_GetSpeakerConfig(CPU *c) { st32(ARG(1), 4); return DS_OK; }

/* Mixes every playing secondary buffer into interleaved stereo S16 at out_rate. */
void dsound_mix(int16_t *out, int frames)
{
    static float *acc;
    static int acc_frames;
    if (frames > acc_frames) { acc_frames = frames; acc = realloc(acc, (size_t)frames * 2 * sizeof(float)); }
    memset(acc, 0, (size_t)frames * 2 * sizeof(float));
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_BUFFERS; i++) {
        ds_buf *b = buffers[i];
        if (!b || b->primary || !b->playing || !b->bytes || !b->align) continue;
        uint32_t total = b->bytes / b->align, rate = b->freq ? b->freq : b->rate;
        double step = (double)rate / out_rate;
        float vol = b->volume <= -10000 ? 0.0f : powf(10.0f, (float)b->volume / 2000.0f);
        float pl = b->pan > 0 ? powf(10.0f, (float)-b->pan / 2000.0f) : 1.0f, pr = b->pan < 0 ? powf(10.0f, (float)b->pan / 2000.0f) : 1.0f;
        const uint8_t *mem = G2H(b->mem);
        for (int f = 0; f < frames; f++) {
            uint32_t idx = (uint32_t)b->pos;
            if (idx >= total) {
                if (!b->looping) { b->playing = 0; b->pos = 0; break; }
                b->pos -= total;
                idx = (uint32_t)b->pos;
            }
            const uint8_t *s = mem + (size_t)idx * b->align;
            float l, r;
            if (b->bits == 16) {
                l = (float)(int16_t)(s[0] | (s[1] << 8)) / 32768.0f;
                r = b->channels > 1 ? (float)(int16_t)(s[2] | (s[3] << 8)) / 32768.0f : l;
            } else {
                l = (float)(s[0] - 128) / 128.0f;
                r = b->channels > 1 ? (float)(s[1] - 128) / 128.0f : l;
            }
            acc[f * 2] += l * vol * pl;
            acc[f * 2 + 1] += r * vol * pr;
            b->pos += step;
        }
    }
    pthread_mutex_unlock(&lock);
    static long clipped, mixed;
    for (int i = 0; i < frames * 2; i++) {
        float v = acc[i] * 32767.0f;
        if (v > 32767.0f || v < -32768.0f) clipped++;
        out[i] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);
    }
    mixed += frames * 2;
    /* TMNT_AUDIO_DUMP=file.wav records the final mix (44.1 kHz stereo S16) for offline analysis. */
    static FILE *dump;
    static int dump_init;
    if (!dump_init) {
        dump_init = 1;
        const char *p = getenv("TMNT_AUDIO_DUMP");
        if (p && *p && (dump = fopen(p, "wb")) != NULL) {
            uint8_t hdr[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0 };
            uint32_t rate = (uint32_t)out_rate, br = rate * 4;
            memcpy(hdr + 24, &rate, 4); memcpy(hdr + 28, &br, 4);
            hdr[32] = 4; hdr[34] = 16;
            memcpy(hdr + 36, "data", 4);
            fwrite(hdr, 1, 44, dump);
        }
    }
    if (dump) {
        fwrite(out, 4, (size_t)frames, dump);
        long pos = ftell(dump);
        uint32_t riff = (uint32_t)(pos - 8), data = (uint32_t)(pos - 44);
        fseek(dump, 4, SEEK_SET); fwrite(&riff, 4, 1, dump);
        fseek(dump, 40, SEEK_SET); fwrite(&data, 4, 1, dump);
        fseek(dump, 0, SEEK_END);
        if (mixed % (out_rate * 2 * 10) < frames * 2)
            rt_log("[dsound] %ld samples mixed, %ld clipped\n", mixed, clipped);
    }
}

/* Without host audio the play cursors must still advance in real time (CRI ADX waits for its stream buffers to drain): mix into nothing. */
static void *silent_mixer(void *arg)
{
    (void)arg;
    rt_thread_place_self(0, 2);
    static int16_t scratch[2 * 4410];
    uint64_t last = plat_ticks_us();
    for (;;) {
        usleep(10000);
        uint64_t now = plat_ticks_us();
        int frames = (int)((now - last) * (uint64_t)out_rate / 1000000U);
        if (frames <= 0) continue;
        if (frames > 4410) frames = 4410;
        last += (uint64_t)frames * 1000000U / (uint64_t)out_rate;
        dsound_mix(scratch, frames);
    }
    return NULL;
}

uint32_t dsound_create(uint32_t pp)
{
    static const com_impl impls[] = {
        { "QueryInterface", m_QueryInterface }, { "AddRef", m_AddRef }, { "Release", m_Ok },
        { "CreateSoundBuffer", ds_CreateSoundBuffer }, { "GetCaps", ds_GetCaps }, { "SetCooperativeLevel", m_Ok },
        { "Compact", m_Ok }, { "GetSpeakerConfig", ds_GetSpeakerConfig }, { "SetSpeakerConfig", m_Ok }, { "Initialize", m_Ok },
        { NULL, NULL },
    };
    static ds_buf root;
    if (!root.guest) {
        root.refs = 1;
        root.guest = com_new(COM_VTABLE(IDirectSound, impls), 16, &root);
        /* Host audio output only with TMNT_AUDIO=1: the mixer is not finished and sounds bad. */
        const char *audio_env = getenv("TMNT_AUDIO");
        if (audio_env && *audio_env && *audio_env != '0') {
            out_rate = plat_audio_start(dsound_mix);
        } else {
            pthread_t t;
            pthread_create(&t, NULL, silent_mixer, NULL);
            pthread_detach(t);
        }
    }
    st32(pp, root.guest);
    return DS_OK;
}
