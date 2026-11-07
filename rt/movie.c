/* Movies: the game's Sofdec has only SSE/MMX colour conversion and its plain decoding path leaves artifacts, so the picture comes
 * from pl_mpeg (MPEG-1) decoding the same movie on a worker thread; Sofdec keeps the sound and the pacing. The movie is the AFS
 * entry of TMNT_V0x.DAT the game is reading; the picture shown is the frame number Sofdec converts.
 * On the Vita pl_mpeg manages only ~10 frames a second, so movies converted beforehand to H.264 (tools/movies/movconv) go
 * through the hardware decoder instead, frame for frame the same numbering; pl_mpeg stays for movies that were not converted. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PL_MPEG_IMPLEMENTATION
#include "third_party/pl_mpeg.h"

#include "movie.h"
#include "osdep.h"
#include "plat.h"
#include "runtime.h"

#ifdef __vita__
#include <psp2/kernel/sysmem.h>
#include <psp2/sysmodule.h>
#include <psp2/videodec.h>
#endif

#define MOVIE_FILES 3
#define SLOTS 3

typedef struct { uint32_t offset, size; } afs_entry;

static char paths[MOVIE_FILES][1024];
static afs_entry *entries[MOVIE_FILES];
static uint32_t counts[MOVIE_FILES];
static volatile int last_file = -1;
static volatile uint64_t last_offset;

typedef struct {
    int file, entry, w, h;
    uint8_t *slot[SLOTS];
    volatile int slot_frame[SLOTS];   /* Sofdec frame number held by each slot, -1 when empty */
    volatile int decoded;             /* frames decoded so far */
    volatile int target;              /* the frame Sofdec is showing; the decoder converts only from here on */
    volatile int shown;               /* the frame pinned for the texture upload; its slot is not overwritten */
    volatile int stop;
    volatile int mode;                /* 0 starting, 1 hardware decoder (Vita), 2 pl_mpeg */
    pthread_t thread;
} session;

static session *cur;
/* Started as soon as the game reads a movie (before its first frame is shown); the main thread adopts it. Both pointers change
   only under mv_lock, since reads come from Sofdec's thread. */
static session *prefetch;
static pthread_mutex_t mv_lock = PTHREAD_MUTEX_INITIALIZER;
uint32_t movie_rgba_mem;
const uint8_t *movie_pin;
int movie_pin_w, movie_pin_h;
int movie_pin_slot = -1;
void *movie_hw_mem[MOVIE_HW_SLOTS];
uint32_t movie_hw_w, movie_hw_h;
volatile uint32_t movie_hw_bound_frame[MOVIE_HW_SLOTS];
static int last_target = -1;
static volatile int sofdec_frame;

void movie_set_frame(int frame) { sofdec_frame = frame; }

void movie_note_open(int file, const char *host_path)
{
    if (file < 0 || file >= MOVIE_FILES || entries[file]) return;
    snprintf(paths[file], sizeof paths[file], "%s", host_path);
    FILE *f = fopen(host_path, "rb");
    uint32_t head[2];
    if (!f || fread(head, 4, 2, f) != 2 || memcmp(head, "AFS", 3) != 0 || head[1] > 4096) { if (f) fclose(f); return; }
    afs_entry *e = malloc(head[1] * sizeof *e);
    if (fread(e, sizeof *e, head[1], f) == head[1]) { entries[file] = e; counts[file] = head[1]; }
    else free(e);
    fclose(f);
    rt_log("[movie] %s: %u movies\n", host_path, counts[file]);
}

static int entry_of(int file, uint64_t offset);
static session *session_start(int file, int entry, int target);
static void session_stop(session *s);

void movie_note_read(int file, uint64_t offset)
{
    last_file = file;
    last_offset = offset;
    int entry = entry_of(file, offset);
    if (entry < 0) return;
    pthread_mutex_lock(&mv_lock);
    int playing = cur && cur->file == file && cur->entry == entry;
    int ready = prefetch && prefetch->file == file && prefetch->entry == entry;
    if (!playing && !ready) {
        if (prefetch) session_stop(prefetch);
        prefetch = session_start(file, entry, 0);
        rt_log("[movie] TMNT_V%02d.DAT movie %d: decoding ahead\n", file, entry);
    }
    pthread_mutex_unlock(&mv_lock);
}

static int entry_of(int file, uint64_t offset)
{
    for (uint32_t i = 0; file >= 0 && file < MOVIE_FILES && i < counts[file]; i++)
        if (offset >= entries[file][i].offset && offset < (uint64_t)entries[file][i].offset + entries[file][i].size) return (int)i;
    return -1;
}

/* pl_mpeg reads only this movie's AFS entry (its own file buffer would rewind to the start of the archive). */
typedef struct { FILE *f; uint32_t base, size, pos; } movie_src;

static void src_load(plm_buffer_t *buf, void *user)
{
    movie_src *m = user;
    uint8_t chunk[64 * 1024];
    size_t want = m->size - m->pos < sizeof chunk ? m->size - m->pos : sizeof chunk;
    if (!want) { plm_buffer_signal_end(buf); return; }
    fseek(m->f, (long)(m->base + m->pos), SEEK_SET);
    size_t got = fread(chunk, 1, want, m->f);
    m->pos += (uint32_t)got;
    plm_buffer_write(buf, chunk, got);
    if (!got) plm_buffer_signal_end(buf);
}
static void src_seek(plm_buffer_t *buf, size_t offset, void *user) { (void)buf; ((movie_src *)user)->pos = (uint32_t)offset; }
static size_t src_tell(plm_buffer_t *buf, void *user) { (void)buf; return ((movie_src *)user)->pos; }

#ifdef __vita__
/* ------------------------------------------------------------------------------------------ hardware decoder (Vita) --- */

#define HW_DIR "ux0:data/TMNT2003/movies"
#define HW_AHEAD 2   /* pictures decoded past the one Sofdec shows */

/* tools/movies/movconv.swift: header, then per frame (offset, size, flags), then one Annex-B access unit per frame. */
typedef struct {
    char magic[8];
    uint32_t version, width, height, frames, fps_num, fps_den, refs, max_au, index_offset, data_offset, reserved[4];
} avc_header;

static int hw_ready;
static uint32_t hw_dec_w, hw_dec_h, hw_refs;       /* what the decoder library was set up for */
static void *hw_dec_mem, *hw_es;
static uint32_t hw_dec_size, hw_es_size;
static session *volatile hw_owner[MOVIE_HW_SLOTS];  /* the movie whose picture a slot holds */
static volatile int hw_frame[MOVIE_HW_SLOTS];       /* its frame number, -1 when none */
/* numOfStreams is 1: one decoder at a time, the next movie waits for the previous one to delete its decoder. */
static pthread_mutex_t hw_lock = PTHREAD_MUTEX_INITIALIZER;

/* vitaGL's aligned allocation in one of its pools (mem_utils.c); vglAlloc gives only 16-byte alignment. */
void *vgl_memalign(size_t alignment, size_t size, vglMemType type);

static void hw_path(char *out, size_t n, int file, int entry) { snprintf(out, n, HW_DIR "/TMNT_V%02d_%02d.avc", file, entry); }

static int hw_read_header(FILE *f, avc_header *h)
{
    return fread(h, sizeof *h, 1, f) == 1 && memcmp(h->magic, "TMNTAVC1", 8) == 0 && h->version == 1 && h->frames && h->width && h->height;
}

static uint32_t align16(uint32_t v) { return (v + 15U) & ~15U; }
static uint32_t pow2_at_least(uint32_t v) { uint32_t p = 1; while (p < v) p <<= 1; return p; }

/* Physically contiguous memory for the decoder (as moonlight-vita does): CDRAM, else main phycont, in 1 MB steps. */
static void *hw_block(const char *name, uint32_t size, uint32_t *rounded)
{
    *rounded = (size + 0xfffffU) & ~0xfffffU;
    SceUID b = sceKernelAllocMemBlock(name, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, *rounded, NULL);
    if (b < 0) b = sceKernelAllocMemBlock(name, SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, *rounded, NULL);
    void *p = NULL;
    if (b < 0 || sceKernelGetMemBlockBase(b, &p) < 0) {
        rt_log("[movie] %s: no memory for %u KB (%08x)\n", name, *rounded / 1024, (unsigned)b);
        return NULL;
    }
    return p;
}

void movie_hw_init(void)
{
    uint32_t w = 0, h = 0, refs = 0, au = 0;
    int found = 0;
    for (int file = 0; file < MOVIE_FILES; file++) {
        for (int entry = 0; entry < 64; entry++) {
            char p[128];
            hw_path(p, sizeof p, file, entry);
            FILE *f = fopen(p, "rb");
            if (!f) break;   /* the movies of an archive are numbered without gaps */
            avc_header hd;
            if (hw_read_header(f, &hd)) {
                found++;
                if (hd.width > w) w = hd.width;
                if (hd.height > h) h = hd.height;
                if (hd.refs > refs) refs = hd.refs;
                if (hd.max_au > au) au = hd.max_au;
            }
            fclose(f);
        }
    }
    if (!found) { rt_log("[movie] no converted movies in " HW_DIR ": pl_mpeg decodes them\n"); return; }
    int r = sceSysmoduleLoadModule(SCE_SYSMODULE_AVCDEC);
    if (r < 0) { rt_log("[movie] AVCDEC module: %08x\n", (unsigned)r); return; }
    SceVideodecQueryInitInfoHwAvcdec init;
    memset(&init, 0, sizeof init);
    init.size = sizeof init;
    init.horizontal = align16(w);
    init.vertical = align16(h);
    init.numOfRefFrames = refs < 5 ? 5 : refs;
    init.numOfStreams = 1;
    r = sceVideodecInitLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC, &init);
    if (r < 0) { rt_log("[movie] sceVideodecInitLibrary: %08x\n", (unsigned)r); return; }
    SceAvcdecQueryDecoderInfo q = { init.horizontal, init.vertical, init.numOfRefFrames };
    SceAvcdecDecoderInfo di;
    memset(&di, 0, sizeof di);
    r = sceAvcdecQueryDecoderMemSize(SCE_VIDEODEC_TYPE_HW_AVCDEC, &q, &di);
    if (r < 0) { rt_log("[movie] sceAvcdecQueryDecoderMemSize: %08x\n", (unsigned)r); return; }
    hw_dec_mem = hw_block("tmnt_avcdec", di.frameMemSize, &hw_dec_size);
    hw_es = hw_block("tmnt_avcdec_es", au + 4096, &hw_es_size);
    if (!hw_dec_mem || !hw_es) return;
    /* The pictures have the size of the game's movie texture (powers of two), so they can stand in for it. */
    movie_hw_w = pow2_at_least(w);
    movie_hw_h = pow2_at_least(h);
    size_t bytes = (size_t)movie_hw_w * movie_hw_h * 4;
    for (int i = 0; i < MOVIE_HW_SLOTS; i++) {
        movie_hw_mem[i] = vgl_memalign(4096, bytes, VGL_MEM_VRAM);
        if (!movie_hw_mem[i]) {
            rt_log("[movie] no video memory for picture %d (%u KB)\n", i, (unsigned)(bytes / 1024));
            for (int k = 0; k < i; k++) { vglFree(movie_hw_mem[k]); movie_hw_mem[k] = NULL; }
            return;
        }
        hw_frame[i] = -1;
    }
    hw_dec_w = init.horizontal;
    hw_dec_h = init.vertical;
    hw_refs = init.numOfRefFrames;
    hw_ready = 1;
    rt_log("[movie] hardware H.264 decoder: %d movies, up to %ux%u, %u reference frames; decoder %u KB, %d pictures of %ux%u\n",
           found, w, h, hw_refs, hw_dec_size / 1024, MOVIE_HW_SLOTS, movie_hw_w, movie_hw_h);
}

/* A slot the decoder may write: not the picture on screen or one decoded ahead of it, and not bound in the last frames. */
static int hw_free_slot(session *s)
{
    uint32_t now = vglGetFrameNumber();
    for (int i = 0; i < MOVIE_HW_SLOTS; i++) {
        /* The render thread binds a picture up to a frame after the game thread picks it: two older ones stay too. A slot of
           this movie without a picture (-1: being decoded, or no picture came out yet) is free. */
        if (hw_owner[i] == s && hw_frame[i] >= 0 && hw_frame[i] >= s->shown - 2) continue;
        if (now - movie_hw_bound_frame[i] < 3) continue;
        return i;
    }
    return -1;
}

/* The movie through the hardware decoder; 0 when it cannot (not converted, or the decoder fails to start), before anything was
   shown, so pl_mpeg takes over. */
static int decode_hw(session *s, uint64_t t_start)
{
    char p[128];
    hw_path(p, sizeof p, s->file, s->entry);
    FILE *f = fopen(p, "rb");
    if (!f) { rt_log("[movie] %s missing: pl_mpeg\n", p); return 0; }
    avc_header hd;
    uint32_t *index = NULL;
    if (!hw_read_header(f, &hd) || align16(hd.width) > hw_dec_w || align16(hd.height) > hw_dec_h || hd.refs > hw_refs
        || hd.max_au > hw_es_size || pow2_at_least(hd.width) != movie_hw_w || pow2_at_least(hd.height) != movie_hw_h
        || !(index = malloc((size_t)hd.frames * 12)) || fseek(f, (long)hd.index_offset, SEEK_SET) != 0
        || fread(index, 12, hd.frames, f) != hd.frames) {
        rt_log("[movie] %s does not fit the decoder: pl_mpeg\n", p);
        free(index);
        fclose(f);
        return 0;
    }
    while (pthread_mutex_trylock(&hw_lock) != 0) {
        if (s->stop) { free(index); fclose(f); return 1; }
        usleep(2000);
    }
    SceAvcdecCtrl dec;
    memset(&dec, 0, sizeof dec);
    dec.frameBuf.pBuf = hw_dec_mem;
    dec.frameBuf.size = hw_dec_size;
    SceAvcdecQueryDecoderInfo q = { hw_dec_w, hw_dec_h, hw_refs };
    int r = sceAvcdecCreateDecoder(SCE_VIDEODEC_TYPE_HW_AVCDEC, &dec, &q);
    if (r < 0) {
        rt_log("[movie] sceAvcdecCreateDecoder: %08x: pl_mpeg\n", (unsigned)r);
        pthread_mutex_unlock(&hw_lock);
        free(index);
        fclose(f);
        return 0;
    }
    s->w = (int)hd.width;
    s->h = (int)hd.height;
    rt_log("[movie] hardware decoder: %ux%u, %u frames at %.2f fps, %.0f ms after start\n", hd.width, hd.height, hd.frames,
           (double)hd.fps_num / hd.fps_den, (plat_ticks_us() - t_start) / 1000.0);
    uint64_t busy_us = 0;
    int fed = 0, out = 0, errors = 0;
    /* The output size: the whole picture buffer (as moonlight-vita gives it); if the first frame is refused, the movie's size. */
    uint32_t out_w = movie_hw_w, out_h = movie_hw_h;
    while (!s->stop) {
        /* No picture from the first frames: the hardware path does not work here, pl_mpeg takes the movie (nothing shown yet). */
        if (out == 0 && fed >= 30) {
            rt_log("[movie] no picture from the hardware decoder after %d frames: pl_mpeg\n", fed);
            sceAvcdecDeleteDecoder(&dec);
            pthread_mutex_unlock(&hw_lock);
            free(index);
            fclose(f);
            return 0;
        }
        /* Enough decoded ahead, or the whole movie fed (with no reordering nothing stays inside the decoder). */
        if (out > s->target + HW_AHEAD || fed >= (int)hd.frames) { usleep(2000); continue; }
        int i = hw_free_slot(s);
        if (i < 0) { usleep(1000); continue; }
        uint32_t off = index[3 * fed], size = index[3 * fed + 1];
        if (size > hw_es_size || fseek(f, (long)off, SEEK_SET) != 0 || fread(hw_es, 1, size, f) != size) {
            if (errors++ < 5) rt_log("[movie] frame %d: cannot read %u bytes at %u\n", fed, size, off);
            fed++;
            continue;
        }
        SceAvcdecAu au;
        memset(&au, 0, sizeof au);
        au.pts.upper = au.pts.lower = au.dts.upper = au.dts.lower = 0xffffffffU;
        au.es.pBuf = hw_es;
        au.es.size = size;
        SceAvcdecPicture pic;
        memset(&pic, 0, sizeof pic);
        pic.size = sizeof pic;
        pic.frame.pixelType = SCE_AVCDEC_PIXELFORMAT_RGBA8888;
        pic.frame.framePitch = movie_hw_w;
        pic.frame.frameWidth = out_w;
        pic.frame.frameHeight = out_h;
        pic.frame.opt.rgba.alpha = 0xff;   /* opaque: the game draws movies with alpha test */
        pic.frame.pPicture[0] = movie_hw_mem[i];
        SceAvcdecPicture *pp = &pic;
        SceAvcdecArrayPicture arr;
        memset(&arr, 0, sizeof arr);
        arr.numOfElm = 1;
        arr.pPicture = &pp;
        hw_frame[i] = -1;
        hw_owner[i] = s;
        __sync_synchronize();
        uint64_t t0 = plat_ticks_us();
        r = sceAvcdecDecode(&dec, &au, &arr);
        if (r < 0 && fed == 0 && out_w == movie_hw_w) {
            rt_log("[movie] first frame with a %ux%u output: %08x; trying %ux%u\n", out_w, out_h, (unsigned)r, align16(hd.width),
                   align16(hd.height));
            out_w = pic.frame.frameWidth = align16(hd.width);
            out_h = pic.frame.frameHeight = align16(hd.height);
            arr.numOfOutput = 0;
            r = sceAvcdecDecode(&dec, &au, &arr);
        }
        busy_us += plat_ticks_us() - t0;
        fed++;
        if (r < 0) {
            if (errors++ < 5) rt_log("[movie] frame %d: sceAvcdecDecode %08x\n", fed - 1, (unsigned)r);
            continue;
        }
        if (arr.numOfOutput < 1) continue;
        if (out == 0)
            rt_log("[movie] first picture after %d access units, %.0f ms after start, %ux%u\n", fed,
                   (plat_ticks_us() - t_start) / 1000.0, pic.frame.horizontalSize, pic.frame.verticalSize);
        __sync_synchronize();
        hw_frame[i] = out++;
        s->decoded = out;
        s->mode = 1;   /* the main thread shows the hardware pictures from now on */
        if (out % 600 == 0) rt_log("[movie] hardware decoder: %d frames, %.2f ms each\n", out, busy_us / 1000.0 / fed);
    }
    sceAvcdecDeleteDecoder(&dec);
    pthread_mutex_unlock(&hw_lock);
    free(index);
    fclose(f);
    return 1;
}
#else
void movie_hw_init(void) {}
#endif

/* Decodes ahead into the ring, never more than SLOTS - 1 frames past the one on screen. */
static void *decoder(void *arg)
{
    session *s = arg;
    uint64_t t_start = plat_ticks_us();
    rt_thread_place_self(0, 2);
    plat_thread_label("movie");
#ifdef __vita__
    if (hw_ready && decode_hw(s, t_start)) {
        while (!s->stop) usleep(10000);
        free(s);
        return NULL;
    }
#endif
    s->mode = 2;
    movie_src src = { fopen(paths[s->file], "rb"), entries[s->file][s->entry].offset, entries[s->file][s->entry].size, 0 };
    if (!src.f) { while (!s->stop) usleep(10000); free(s); return NULL; }
    plm_buffer_t *buf = plm_buffer_create_with_callbacks(src_load, src_seek, src_tell, src.size, &src);
    plm_t *plm = plm_create_with_buffer(buf, 1);
    plm_set_audio_enabled(plm, 0);
    plm_set_loop(plm, 0);
    /* The video stream starts at the head of the movie: a short probe (pl_mpeg scans every byte of it). */
    if (!plm_probe(plm, 256 * 1024) || plm_get_width(plm) <= 0) {
        rt_log("[movie] pl_mpeg found no video stream\n");
        plm_destroy(plm); fclose(src.f);
        while (!s->stop) usleep(10000);
        free(s);
        return NULL;
    }
    rt_log("[movie] %dx%d at %.2f fps, probe %.0f ms\n", plm_get_width(plm), plm_get_height(plm), plm_get_framerate(plm),
           (plat_ticks_us() - t_start) / 1000.0);
    s->w = plm_get_width(plm); s->h = plm_get_height(plm);
    /* plm_frame_to_rgba leaves the fourth byte alone: opaque alpha once, since the game draws movies with alpha test. */
    for (int i = 0; i < SLOTS; i++) {
        s->slot[i] = malloc((size_t)s->w * s->h * 4);
        memset(s->slot[i], 0xff, (size_t)s->w * s->h * 4);
        s->slot_frame[i] = -1;
    }
    /* TMNT_MOVIE_SLOW_US: extra delay per decoded frame, to test on the Mac a decoder that falls behind (as on the Vita). */
    const char *slow_env = getenv("TMNT_MOVIE_SLOW_US");
    int slow_us = slow_env ? atoi(slow_env) : 0;
    while (!s->stop) {
        if (slow_us) usleep(slow_us);
        int k = s->decoded;
        /* Never more than SLOTS - 1 frames ahead of what Sofdec shows. */
        if (k > s->target + SLOTS - 1) { usleep(2000); continue; }
        plm_frame_t *fr = plm_decode_video(plm);
        if (!fr) break;
        if (k == 0) rt_log("[movie] first frame %.0f ms after start, Sofdec at frame %d\n", (plat_ticks_us() - t_start) / 1000.0, s->target);
        /* Far behind Sofdec: decode (later frames depend on it) and convert only every fourth frame, so the picture still moves. */
        /* The slot pinned for the upload is not overwritten; that frame is simply not converted. */
        if ((k >= s->target - 1 || (k & 3) == 0) && s->slot_frame[k % SLOTS] != s->shown) {
            int i = k % SLOTS;
            s->slot_frame[i] = -1;
            __sync_synchronize();
            plm_frame_to_rgba(fr, s->slot[i], s->w * 4);
            __sync_synchronize();
            s->slot_frame[i] = k;
        }
        s->decoded = k + 1;
    }
    plm_destroy(plm);
    fclose(src.f);
    /* Wait until the main thread lets go of this movie, then free it here: the main thread never waits for the decoder. */
    while (!s->stop) usleep(10000);
    for (int i = 0; i < SLOTS; i++) free(s->slot[i]);
    free(s);
    return NULL;
}

static session *session_start(int file, int entry, int target)
{
    session *s = calloc(1, sizeof *s);
    s->file = file; s->entry = entry; s->target = target; s->shown = target;
    for (int i = 0; i < SLOTS; i++) s->slot_frame[i] = -1;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 256 * 1024);
    pthread_create(&s->thread, &a, decoder, s);
    pthread_attr_destroy(&a);
    return s;
}

/* The decoder frees the session itself once it sees stop. */
static void session_stop(session *s)
{
    s->stop = 1;
    pthread_detach(s->thread);
}

static void session_end(void)
{
    if (!cur) return;
    movie_pin = NULL;
    movie_pin_slot = -1;
    pthread_mutex_lock(&mv_lock);
    session_stop(cur);
    cur = NULL;
    pthread_mutex_unlock(&mv_lock);
}

/* Every 60 conversions (~2 s): how the picture follows Sofdec. new = a frame not shown before, same = the previous frame again,
   late = sum of (Sofdec frame - shown frame), lead = decoded frames ahead of Sofdec at the end of the window. */
static void movie_stats(session *s, int shown, int target)
{
    static int calls, fresh, same, late, last_shown = -1;
    calls++;
    if (shown != last_shown) fresh++; else same++;
    if (target > shown) late += target - shown;
    last_shown = shown;
    if (calls < 60) return;
    rt_log("[movie] %d conversions: %d new, %d repeated, avg %.1f frames behind Sofdec, decoder lead %d\n", calls, fresh, same,
           (double)late / calls, s->decoded - 1 - target);
    calls = fresh = same = late = 0;
}

int movie_frame_rgba(uint8_t *dst, uint32_t pitch, int w, int h)
{
    int target = sofdec_frame;
    /* A new movie: the frame number of Sofdec starts again. */
    if (cur && target < last_target) session_end();
    last_target = target;
    if (!cur) {
        int file = last_file, entry = entry_of(file, last_offset);
        if (entry < 0) return 0;
        pthread_mutex_lock(&mv_lock);
        if (prefetch && prefetch->file == file && prefetch->entry == entry) {
            cur = prefetch;
            rt_log("[movie] TMNT_V%02d.DAT movie %d (decoded ahead: %d frames)\n", file, entry, cur->decoded);
        } else {
            /* A movie decoded ahead that is not this one is not needed any more. */
            if (prefetch) session_stop(prefetch);
            cur = session_start(file, entry, target);
            rt_log("[movie] TMNT_V%02d.DAT movie %d\n", file, entry);
        }
        prefetch = NULL;
        pthread_mutex_unlock(&mv_lock);
    }
    session *s = cur;
    s->target = target;
    movie_pin_slot = -1;
#ifdef __vita__
    if (s->mode == 1) {
        /* The newest picture not past the frame Sofdec shows, or else the newest one. */
        int best = -1, best_f = -1, newest = -1, newest_f = -1;
        for (int i = 0; i < MOVIE_HW_SLOTS; i++) {
            if (hw_owner[i] != s) continue;
            int f = hw_frame[i];
            if (f < 0) continue;
            if (f <= target && f > best_f) { best = i; best_f = f; }
            if (f > newest_f) { newest = i; newest_f = f; }
        }
        if (best < 0) { best = newest; best_f = newest_f; }
        movie_pin = NULL;
        if (best < 0) {
            for (int y = 0; y < h; y++) memset(dst + (size_t)y * pitch, 0, (size_t)w * 4);
            return 1;
        }
        movie_stats(s, best_f, target);
        s->shown = best_f;
        movie_pin_slot = best;
        return 1;
    }
#endif
    /* The slot with the frame Sofdec shows, or else the newest frame not past it, or else the newest one. */
    int best = -1, newest = -1;
    for (int i = 0; i < SLOTS; i++) {
        int f = s->slot_frame[i];
        if (f < 0) continue;
        if (f <= target && (best < 0 || f > s->slot_frame[best])) best = i;
        if (newest < 0 || f > s->slot_frame[newest]) newest = i;
    }
    if (best < 0) best = newest;
    if (best < 0) {
        /* Nothing yet: black instead of the picture of Sofdec. */
        movie_pin = NULL;
        for (int y = 0; y < h; y++) memset(dst + (size_t)y * pitch, 0, (size_t)w * 4);
        return 1;
    }
    __sync_synchronize();
    const uint8_t *src = s->slot[best];
    /* A texture narrower than the movie: copy the rows that fit (rare). */
    if (w < s->w) {
        movie_pin = NULL;
        int ch = h < s->h ? h : s->h;
        for (int y = 0; y < ch; y++) memcpy(dst + (size_t)y * pitch, src + (size_t)y * s->w * 4, (size_t)w * 4);
        return 1;
    }
    /* No copy into guest memory (memcpy is slow on the Vita): d3d9_gl uploads the pinned slot straight into the texture. */
    movie_stats(s, s->slot_frame[best], target);
    s->shown = s->slot_frame[best];
    movie_pin = src;
    movie_pin_w = s->w;
    movie_pin_h = h < s->h ? h : s->h;
    return 1;
}
