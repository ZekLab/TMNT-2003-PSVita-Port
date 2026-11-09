/* TMNT_MOVIE_BENCH=1 (tmnt_env.txt, Vita): measures before the game what converting the movies on the console would cost, writes
 * the results to log.txt and closes the app. pl_mpeg on one core and on three at once, the hardware JPEG encoder (time and size per
 * frame at a few compression ratios) and the hardware MJPEG decoder (time per frame). Sample pictures go to bench_*.jpg/.rgba. */
#ifdef __vita__
#include <malloc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/jpeg.h>
#include <psp2/jpegenc.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "third_party/pl_mpeg.h"
#include "movie.h"
#include "plat.h"
#include "runtime.h"

#define BENCH_FRAMES 200
#define BENCH_READ (8U << 20)   /* bytes of each movie read into memory: well over BENCH_FRAMES frames */
#define BENCH_ENC_FRAMES 40

typedef struct { uint8_t *data; uint32_t size; char name[16]; } bench_movie;

static uint64_t now_us(void) { return sceKernelGetProcessTimeWide(); }

/* Physically contiguous memory (CDRAM, else main phycont), as the codec engine wants. */
static void *phys_block(const char *name, uint32_t size)
{
    uint32_t rounded = (size + 0xfffffU) & ~0xfffffU;
    SceUID b = sceKernelAllocMemBlock(name, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, rounded, NULL);
    if (b < 0) b = sceKernelAllocMemBlock(name, SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, rounded, NULL);
    void *p = NULL;
    if (b < 0 || sceKernelGetMemBlockBase(b, &p) < 0) { rt_log("[bench] %s: no memory (%08x)\n", name, (unsigned)b); return NULL; }
    return p;
}

/* The first movie of TMNT_V0<file>.DAT, read into memory. */
static int load_movie(int file, bench_movie *m)
{
    char path[32];
    snprintf(path, sizeof path, "TMNT_V%02d.DAT", file);
    snprintf(m->name, sizeof m->name, "V%02d_00", file);
    FILE *f = fopen(path, "rb");
    uint32_t head[4];
    if (!f || fread(head, 4, 4, f) != 4 || memcmp(head, "AFS", 3) != 0) { rt_log("[bench] %s: not an AFS archive\n", path); if (f) fclose(f); return 0; }
    m->size = head[3] < BENCH_READ ? head[3] : BENCH_READ;
    m->data = malloc(m->size);
    fseek(f, (long)head[2], SEEK_SET);
    int ok = m->data && fread(m->data, 1, m->size, f) == m->size;
    fclose(f);
    if (!ok) rt_log("[bench] %s: read failed\n", path);
    return ok;
}

static plm_t *open_movie(bench_movie *m)
{
    plm_t *plm = plm_create_with_memory(m->data, m->size, 0);
    plm_set_audio_enabled(plm, 0);
    plm_set_loop(plm, 0);
    if (!plm_probe(plm, 256 * 1024) || plm_get_width(plm) <= 0) { rt_log("[bench] %s: no video stream\n", m->name); plm_destroy(plm); return NULL; }
    return plm;
}

typedef struct { bench_movie *m; int core; int frames; double ms; } decode_job;

static void *decode_main(void *arg)
{
    decode_job *j = arg;
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), 0x10000 << j->core);
    plm_t *plm = open_movie(j->m);
    if (!plm) return NULL;
    uint64_t t0 = now_us();
    while (j->frames < BENCH_FRAMES && plm_decode_video(plm)) j->frames++;
    j->ms = (now_us() - t0) / 1000.0;
    plm_destroy(plm);
    return NULL;
}

static void run_decoders(bench_movie *movies, int count)
{
    decode_job jobs[3];
    pthread_t t[3];
    for (int i = 0; i < count; i++) {
        jobs[i] = (decode_job){ &movies[i], i, 0, 0 };
        pthread_create(&t[i], NULL, decode_main, &jobs[i]);
    }
    double total = 0;
    for (int i = 0; i < count; i++) {
        pthread_join(t[i], NULL);
        double fps = jobs[i].ms > 0 ? jobs[i].frames * 1000.0 / jobs[i].ms : 0;
        total += fps;
        rt_log("[bench] pl_mpeg %s on core %d: %d frames in %.0f ms = %.1f fps\n", movies[i].name, i, jobs[i].frames, jobs[i].ms, fps);
    }
    rt_log("[bench] pl_mpeg, %d at once: %.1f fps in total\n", count, total);
}

static void save(const char *path, const void *data, uint32_t size)
{
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(data, 1, size, f); fclose(f); }
}

static void jpeg_bench(bench_movie *m)
{
    plm_t *plm = open_movie(m);
    if (!plm) return;
    int w = plm_get_width(plm), h = plm_get_height(plm);
    uint32_t rgba_size = (uint32_t)w * h * 4, yuv_size = (uint32_t)w * h * 3 / 2, out_size = 512 * 1024;
    uint8_t *rgba = phys_block("bench_rgba", rgba_size);
    uint8_t *yuv = phys_block("bench_yuv", yuv_size);
    uint8_t *out = phys_block("bench_jpeg", out_size);
    uint8_t *store = malloc(BENCH_ENC_FRAMES * out_size / 4);
    uint32_t *store_size = calloc(BENCH_ENC_FRAMES, sizeof *store_size);
    void *ctx = memalign(256, (size_t)sceJpegEncoderGetContextSize());
    if (!rgba || !yuv || !out || !store || !ctx) { plm_destroy(plm); return; }
    memset(rgba, 0xff, rgba_size);
    int r = sceJpegEncoderInit(ctx, w, h, SCE_JPEGENC_PIXELFORMAT_YCBCR420 | SCE_JPEGENC_PIXELFORMAT_CSC_ARGB_YCBCR, out, out_size);
    rt_log("[bench] JPEG encoder %dx%d: init %08x, context %d bytes\n", w, h, (unsigned)r, sceJpegEncoderGetContextSize());
    if (r < 0) { plm_destroy(plm); return; }

    /* Skip the black first frames of the movie. */
    for (int i = 0; i < 90 && plm_decode_video(plm); i++) {}
    static const int ratios[] = { 64, 128, 192 };
    for (int ri = 0; ri < 3; ri++) {
        sceJpegEncoderSetCompressionRatio(ctx, ratios[ri]);
        double csc_ms = 0, enc_ms = 0, bytes = 0;
        int n = 0;
        for (; n < BENCH_ENC_FRAMES; n++) {
            plm_frame_t *fr = plm_decode_video(plm);
            if (!fr) break;
            plm_frame_to_rgba(fr, rgba, w * 4);
            uint64_t t0 = now_us();
            r = sceJpegEncoderCsc(ctx, yuv, rgba, w, SCE_JPEGENC_PIXELFORMAT_ARGB8888);
            uint64_t t1 = now_us();
            int size = r < 0 ? r : sceJpegEncoderEncode(ctx, yuv);
            uint64_t t2 = now_us();
            if (r < 0 || size <= 0) { rt_log("[bench] JPEG ratio %d: csc %08x, encode %08x\n", ratios[ri], (unsigned)r, (unsigned)size); break; }
            csc_ms += (t1 - t0) / 1000.0;
            enc_ms += (t2 - t1) / 1000.0;
            bytes += size;
            if (ratios[ri] == 128 && (uint32_t)size <= out_size / 4) {
                memcpy(store + (size_t)n * (out_size / 4), out, (size_t)size);
                store_size[n] = (uint32_t)size;
            }
            if (n == 0) {
                char p[32];
                snprintf(p, sizeof p, "bench_r%d.jpg", ratios[ri]);
                save(p, out, (uint32_t)size);
                if (ri == 1) save("bench_src.rgba", rgba, rgba_size);
            }
        }
        if (n) rt_log("[bench] JPEG ratio %d: %d frames, csc %.2f ms + encode %.2f ms per frame, %.1f KB per frame\n", ratios[ri], n, csc_ms / n,
                      enc_ms / n, bytes / n / 1024);
    }
    sceJpegEncoderEnd(ctx);
    plm_destroy(plm);

    /* The hardware MJPEG decoder on the frames encoded at ratio 128. */
    r = sceJpegInitMJpeg(1);
    rt_log("[bench] MJPEG decoder init %08x\n", (unsigned)r);
    if (r < 0 || !store_size[0]) return;
    SceJpegOutputInfo info;
    memset(&info, 0, sizeof info);
    r = sceJpegGetOutputInfo(store, store_size[0], 0, 0, &info);
    rt_log("[bench] MJPEG output info %08x: color space %08x, %ux%u, output %u bytes, pitch %ux%u %ux%u\n", (unsigned)r, (unsigned)info.colorSpace,
           info.width, info.height, info.outputSize, info.pitch[0].x, info.pitch[0].y, info.pitch[1].x, info.pitch[1].y);
    if (r < 0) return;
    uint32_t dec_size = info.outputSize ? info.outputSize : yuv_size * 2;
    uint8_t *dec = phys_block("bench_dec", dec_size);
    if (!dec) return;
    double dec_ms = 0, csc_ms = 0;
    int n = 0;
    for (; n < BENCH_ENC_FRAMES && store_size[n]; n++) {
        uint64_t t0 = now_us();
        int got = sceJpegDecodeMJpegYCbCr(store + (size_t)n * (out_size / 4), store_size[n], 0, dec, dec_size, NULL, 0);
        uint64_t t1 = now_us();
        int c = got < 0 ? got : sceJpegMJpegCsc(rgba, dec, (SceSize)got, w, 0, info.colorSpace & 0xFFFF);
        uint64_t t2 = now_us();
        if (got < 0 || c < 0) { rt_log("[bench] MJPEG frame %d: decode %08x, csc %08x\n", n, (unsigned)got, (unsigned)c); break; }
        dec_ms += (t1 - t0) / 1000.0;
        csc_ms += (t2 - t1) / 1000.0;
        if (n == 0) save("bench_dec.rgba", rgba, rgba_size);
    }
    if (n) rt_log("[bench] MJPEG decode: %d frames, decode %.2f ms + csc %.2f ms per frame\n", n, dec_ms / n, csc_ms / n);
    sceJpegFinishMJpeg();
}

void movie_bench_run(void)
{
    rt_log("[bench] movie conversion benchmark, %d frames per decoder\n", BENCH_FRAMES);
    bench_movie movies[3];
    memset(movies, 0, sizeof movies);
    for (int i = 0; i < 3; i++)
        if (!load_movie(i, &movies[i])) return;
    run_decoders(&movies[1], 1);
    run_decoders(movies, 3);
    jpeg_bench(&movies[1]);
    rt_log("[bench] done\n");
}
#endif
