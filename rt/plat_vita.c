/* PS Vita platform layer: vitaGL on the 960x544 screen, SceCtrl as a DirectInput joystick, SceAudioOut fed by the DirectSound mixer. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/audioout.h>
#include <psp2/power.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>

#include "game_hooks.h"
#include "osdep.h"
#include "movie.h"
#include "plat.h"
#include "runtime.h"

#define SCREEN_W 960
#define SCREEN_H 544

static int inited;
static int game_w, game_h;   /* the size the game asked for; Win32 reports this one */
static uint8_t dik[256];
static SceCtrlData pad_now;

int plat_video_init(int width, int height)
{
    if (inited)
        return 1;
    game_w = width;
    game_h = height;
    SceKernelFreeMemorySizeInfo mem;
    memset(&mem, 0, sizeof mem);
    mem.size = sizeof mem;
    sceKernelGetFreeMemorySize(&mem);
    rt_log("[plat] free memory: user %d KB, cdram %d KB, phycont %d KB\n", mem.size_user / 1024, mem.size_cdram / 1024,
           mem.size_phycont / 1024);
    /* Explicit pools as in the Rogue Squadron port: vitaGL's default would take all free main memory. */
    int cdram = mem.size_cdram - 16 * 1024 * 1024;
    int phycont = mem.size_phycont < 8 * 1024 * 1024 ? mem.size_phycont : 8 * 1024 * 1024;
    vglInitWithCustomSizes(0, SCREEN_W, SCREEN_H, 32 * 1024 * 1024, cdram > 0 ? cdram : 0, phycont, 0, SCE_GXM_MULTISAMPLE_NONE);
    vglWaitVblankStart(getenv("TMNT_VSYNC") ? GL_TRUE : GL_FALSE);
    movie_hw_init();
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    rt_log("[plat] vitaGL %dx%d, game %dx%d\n", SCREEN_W, SCREEN_H, width, height);
    inited = 1;
    return 1;
}

void plat_screen_size(int *w, int *h)
{
    *w = SCREEN_W;
    *h = SCREEN_H;
}

void plat_game_size(int *w, int *h)
{
    *w = game_w;
    *h = game_h;
}

void plat_swap(void)
{
    vglSwapBuffers(GL_FALSE);
}

int plat_pump(void)
{
    sceCtrlPeekBufferPositive(0, &pad_now, 1);
    /* L + R + Select: ask the renderer for a screenshot and a draw dump of the next frame (it polls for dump.req). */
    static int combo_held;
    const uint32_t combo = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_SELECT;
    int held = (pad_now.buttons & combo) == combo;
    if (held && !combo_held) {
        FILE *f = fopen("dump.req", "w");
        if (f) fclose(f);
        rt_log("[plat] dump requested from the pad\n");
    }
    combo_held = held;
    /* Start: Enter on the title screen ("PRESS START" takes Enter, not Esc), elsewhere Esc, the game's pause key (the joystick has
     * no pause button). */
    int start = (pad_now.buttons & SCE_CTRL_START) != 0, title = start && tmnt_title_active();
    dik[0x1C] = title ? 0x80 : 0;
    dik[0x01] = start && !title ? 0x80 : 0;

    return 1;
}

const uint8_t *plat_dik_state(void)
{
    return dik;
}

/* Stick byte 0..255 -> -1000..1000 with a small dead zone. */
static int32_t stick(uint8_t v)
{
    int d = (int)v - 128;
    if (d > -16 && d < 16) return 0;
    return d * 1000 / (d < 0 ? 128 : 127);
}

/* Joystick buttons as the game numbers them (ReadMe, from 1): 1 weak attack/decide, 2 strong attack/cancel, 3 jump,
 * 4 shuriken, 5 dash, 6 shuriken change, 7 air juggling, 8 special attack. Fixed Vita buttons: 1 Cross, 2 Circle, 3 Square,
 * 4 Triangle, 5 L, 6 Select, 7 R, 8 L + R together; the game's own Controller settings > Game pad page reassigns actions.
 * The D-pad is the joystick's hat (movement and menus), like the left stick. Start is Enter/Esc on the keyboard (plat_pump). */
void plat_pad(uint32_t *buttons, int32_t axes[4], int *pov)
{
    static const uint32_t map[7] = { SCE_CTRL_CROSS, SCE_CTRL_CIRCLE, SCE_CTRL_SQUARE, SCE_CTRL_TRIANGLE,
                                     SCE_CTRL_LTRIGGER, SCE_CTRL_SELECT, SCE_CTRL_RTRIGGER };
    uint32_t b = pad_now.buttons;
    const uint32_t lr = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER;
    int chord = (b & lr) == lr;
    if (chord) b &= ~lr;
    *buttons = 0;
    for (int i = 0; i < 7; i++)
        if (b & map[i])
            *buttons |= 1U << i;
    if (chord) *buttons |= 1U << 7;   /* L + R is button 8, the game's default special attack */
    axes[0] = stick(pad_now.lx);
    axes[1] = stick(pad_now.ly);
    axes[2] = stick(pad_now.rx);
    axes[3] = stick(pad_now.ry);
    *pov = -1;
    int up = !!(b & SCE_CTRL_UP), dn = !!(b & SCE_CTRL_DOWN), lf = !!(b & SCE_CTRL_LEFT), rt = !!(b & SCE_CTRL_RIGHT);
    if (up || dn || lf || rt) {
        static const int dirs[3][3] = { { 31500, 0, 4500 }, { 27000, -1, 9000 }, { 22500, 18000, 13500 } };
        *pov = dirs[dn ? 2 : up ? 0 : 1][lf ? 0 : rt ? 2 : 1];
    }
}

uint64_t plat_ticks_us(void)
{
    return sceKernelGetProcessTimeWide();
}

/* PNG without zlib (none in the VitaSDK): stored deflate blocks, own CRC-32 and Adler-32. */
static uint32_t crc_table[256];

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320U ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    crc = ~crc;
    while (n--) crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n)
{
    uint8_t hdr[8], c[4];
    put32(hdr, n);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    put32(c, crc32_update(crc32_update(0, (const uint8_t *)type, 4), data, n));
    fwrite(c, 1, 4, f);
}

/* rows: h rows of 1 + w*3 bytes (filter byte 0 + RGB), top-down. */
static void write_png_rows(const char *path, const uint8_t *rows, int w, int h)
{
    size_t raw = (size_t)(w * 3 + 1) * h, blocks = (raw + 65534) / 65535;
    size_t zn = 2 + raw + blocks * 5 + 4;
    uint8_t *z = malloc(zn), *o = z;
    *o++ = 0x78; *o++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t off = 0; off < raw; off += 65535) {
        size_t n = raw - off < 65535 ? raw - off : 65535;
        *o++ = off + n == raw;
        *o++ = (uint8_t)n; *o++ = (uint8_t)(n >> 8);
        *o++ = (uint8_t)~n; *o++ = (uint8_t)(~n >> 8);
        memcpy(o, rows + off, n);
        for (size_t i = 0; i < n; i++) { a = (a + o[i]) % 65521U; b = (b + a) % 65521U; }
        o += n;
    }
    put32(o, (b << 16) | a);
    FILE *f = fopen(path, "wb");
    if (f) {
        static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
        uint8_t ihdr[13];
        fwrite(sig, 1, 8, f);
        put32(ihdr, (uint32_t)w);
        put32(ihdr + 4, (uint32_t)h);
        ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
        png_chunk(f, "IHDR", ihdr, 13);
        png_chunk(f, "IDAT", z, (uint32_t)zn);
        png_chunk(f, "IEND", NULL, 0);
        fclose(f);
        rt_log("[plat] png %s\n", path);
    }
    free(z);
}

void plat_write_rgba_png(const char *path, const uint8_t *rgba, int w, int h)
{
    uint8_t *rows = malloc((size_t)(w * 3 + 1) * h);
    for (int y = 0; y < h; y++) {
        uint8_t *r = rows + (size_t)y * (w * 3 + 1);
        r[0] = 0;
        for (int x = 0; x < w; x++) memcpy(r + 1 + x * 3, rgba + ((size_t)y * w + x) * 4, 3);
    }
    write_png_rows(path, rows, w, h);
    free(rows);
}

void plat_screenshot(const char *path)
{
    int w = SCREEN_W, h = SCREEN_H;
    uint8_t *px = malloc((size_t)w * h * 4), *rows = malloc((size_t)(w * 3 + 1) * h);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (int y = 0; y < h; y++) {
        uint8_t *r = rows + (size_t)y * (w * 3 + 1);
        const uint8_t *s = px + (size_t)(h - 1 - y) * w * 4;
        r[0] = 0;
        for (int x = 0; x < w; x++) memcpy(r + 1 + x * 3, s + x * 4, 3);
    }
    write_png_rows(path, rows, w, h);
    free(px);
    free(rows);
}

void plat_power_info(int *arm, int *gpu, int *battery)
{
    *arm = scePowerGetArmClockFrequency();
    *gpu = scePowerGetGpuClockFrequency();
    *battery = scePowerGetBatteryLifePercent();
}

/* Audio: a thread pulls 1024-frame blocks from the mixer into a blocking SceAudioOut port. */
#define AUDIO_RATE 48000
#define AUDIO_GRAIN 1024
static void (*audio_mix)(int16_t *out, int frames);

static void *audio_thread(void *arg)
{
    (void)arg;
    rt_thread_place_self(0, 4);
    plat_thread_label("audio out");
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, AUDIO_GRAIN, AUDIO_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) {
        rt_log("[plat] sceAudioOutOpenPort: %08x\n", port);
        return NULL;
    }
    static int16_t buf[2][AUDIO_GRAIN * 2];
    /* TMNT_PERF: mix time and the longest gap between two blocks handed to SceAudioOut (a block lasts 21.3 ms). */
    int stats = getenv("TMNT_PERF") && atoi(getenv("TMNT_PERF"));
    uint64_t last = sceKernelGetProcessTimeWide(), report = last + 2000000, mix_sum = 0, mix_max = 0, gap_max = 0;
    int blocks = 0;
    for (int i = 0;; i ^= 1) {
        uint64_t t0 = sceKernelGetProcessTimeWide();
        audio_mix(buf[i], AUDIO_GRAIN);
        uint64_t t1 = sceKernelGetProcessTimeWide();
        sceAudioOutOutput(port, buf[i]);
        if (!stats) continue;
        mix_sum += t1 - t0; if (t1 - t0 > mix_max) mix_max = t1 - t0;
        if (t0 - last > gap_max) gap_max = t0 - last;
        last = t0; blocks++;
        if (t0 >= report) {
            rt_log("[audio] %d blocks, mix avg %.2f ms max %.2f ms, longest gap %.1f ms\n", blocks,
                   (double)mix_sum / blocks / 1000.0, (double)mix_max / 1000.0, (double)gap_max / 1000.0);
            report = t0 + 2000000; mix_sum = mix_max = gap_max = 0; blocks = 0;
        }
    }
    return NULL;
}

int plat_audio_start(void (*mix)(int16_t *out, int frames))
{
    audio_mix = mix;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 256 * 1024);
    pthread_t t;
    pthread_create(&t, &a, audio_thread, NULL);
    pthread_attr_destroy(&a);
    rt_log("[plat] audio %d Hz, %d frames per block\n", AUDIO_RATE, AUDIO_GRAIN);
    return AUDIO_RATE;
}

/* [cpu]: each registered thread's share of one core over the last report window (SceKernelThreadInfo.runClocks, microseconds).
   Threads register when rt_thread_place_self places them; plat_thread_label names them. */
#define CPU_THREADS 48
static struct { SceUID id; char name[24]; uint64_t last; } cpu_threads[CPU_THREADS];
static volatile int cpu_count;
static uint64_t cpu_last_us;

void plat_thread_label(const char *name)
{
    SceUID id = sceKernelGetThreadId();
    int n = cpu_count < CPU_THREADS ? cpu_count : CPU_THREADS;
    for (int i = 0; i < n; i++) {
        if (cpu_threads[i].id != id) continue;
        if (name) snprintf(cpu_threads[i].name, sizeof cpu_threads[i].name, "%s", name);
        return;
    }
    int i = __sync_fetch_and_add(&cpu_count, 1);
    if (i >= CPU_THREADS) return;
    snprintf(cpu_threads[i].name, sizeof cpu_threads[i].name, "%s", name ? name : "thread");
    __sync_synchronize();
    cpu_threads[i].id = id;
}

void plat_cpu_report(void)
{
    uint64_t now = sceKernelGetSystemTimeWide(), span = now - cpu_last_us;
    int first = cpu_last_us == 0;
    cpu_last_us = now;
    char line[768];
    int len = snprintf(line, sizeof line, "[cpu]");
    double side = 0;
    int n = cpu_count < CPU_THREADS ? cpu_count : CPU_THREADS;
    for (int i = 0; i < n; i++) {
        if (!cpu_threads[i].id) continue;
        SceKernelThreadInfo info;
        memset(&info, 0, sizeof info);
        info.size = sizeof info;
        if (sceKernelGetThreadInfo(cpu_threads[i].id, &info) < 0) continue;
        uint64_t run = info.runClocks, d = run - cpu_threads[i].last;
        cpu_threads[i].last = run;
        if (first || !span) continue;
        double pct = 100.0 * (double)d / (double)span;
        if (!(info.currentCpuAffinityMask & 0x10000)) side += pct;   /* placed on cores 1-2 */
        if (pct >= 1.0 && len < (int)sizeof line - 40)
            len += snprintf(line + len, sizeof line - len, " %s %.0f%%,", cpu_threads[i].name, pct);
    }
    if (!first) rt_log("%s cores 1-2 busy %.0f%% of 200%%\n", line, side);
}

/* vitaGL has no per-thread context: the render thread just makes the calls. */
void plat_gl_thread(int take) { (void)take; }
