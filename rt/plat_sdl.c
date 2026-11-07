/* SDL2 + OpenGL 2.1 platform layer for the Mac build. */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "plat.h"
#include "runtime.h"

static SDL_Window *window;
static SDL_GLContext glctx;
static int win_w, win_h;
static int game_w, game_h;   /* the size the game asked for; Win32 reports this one */
static uint8_t dik[256];
static SDL_GameController *pad;

/* SDL scancode -> DirectInput DIK code (US layout, the keys a game uses). */
static uint8_t dik_of(SDL_Scancode s)
{
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) {
        static const uint8_t letters[26] = { 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                                             0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C };
        return letters[s - SDL_SCANCODE_A];
    }
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9) return (uint8_t)(0x02 + (s - SDL_SCANCODE_1));
    switch (s) {
    case SDL_SCANCODE_0: return 0x0B;
    case SDL_SCANCODE_RETURN: return 0x1C;
    case SDL_SCANCODE_ESCAPE: return 0x01;
    case SDL_SCANCODE_BACKSPACE: return 0x0E;
    case SDL_SCANCODE_TAB: return 0x0F;
    case SDL_SCANCODE_SPACE: return 0x39;
    case SDL_SCANCODE_LCTRL: return 0x1D;
    case SDL_SCANCODE_RCTRL: return 0x9D;
    case SDL_SCANCODE_LSHIFT: return 0x2A;
    case SDL_SCANCODE_RSHIFT: return 0x36;
    case SDL_SCANCODE_LALT: return 0x38;
    case SDL_SCANCODE_RALT: return 0xB8;
    case SDL_SCANCODE_UP: return 0xC8;
    case SDL_SCANCODE_DOWN: return 0xD0;
    case SDL_SCANCODE_LEFT: return 0xCB;
    case SDL_SCANCODE_RIGHT: return 0xCD;
    case SDL_SCANCODE_F1: return 0x3B;
    case SDL_SCANCODE_F2: return 0x3C;
    case SDL_SCANCODE_F3: return 0x3D;
    case SDL_SCANCODE_F4: return 0x3E;
    case SDL_SCANCODE_F5: return 0x3F;
    case SDL_SCANCODE_COMMA: return 0x33;
    case SDL_SCANCODE_PERIOD: return 0x34;
    case SDL_SCANCODE_SLASH: return 0x35;
    case SDL_SCANCODE_SEMICOLON: return 0x27;
    case SDL_SCANCODE_MINUS: return 0x0C;
    case SDL_SCANCODE_EQUALS: return 0x0D;
    default: return 0;
    }
}

int plat_video_init(int width, int height)
{
    if (window)
        return 1;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        rt_log("[plat] SDL_Init: %s\n", SDL_GetError());
        return 0;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    game_w = win_w = width;
    game_h = win_h = height;
    /* TMNT_WINDOW=WxH: a window of another size, e.g. 960x544 to preview the Vita screen. */
    const char *ws = getenv("TMNT_WINDOW");
    if (ws && sscanf(ws, "%dx%d", &win_w, &win_h) != 2) { win_w = width; win_h = height; }
    /* TMNT_HIDDEN=1: no visible window (automated test runs); rendering and screenshots still work. */
    Uint32 wflags = SDL_WINDOW_OPENGL | (getenv("TMNT_HIDDEN") && atoi(getenv("TMNT_HIDDEN")) ? SDL_WINDOW_HIDDEN : 0);
    window = SDL_CreateWindow("TMNT (translated)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h, wflags);
    if (!window) {
        rt_log("[plat] SDL_CreateWindow: %s\n", SDL_GetError());
        return 0;
    }
    glctx = SDL_GL_CreateContext(window);
    if (!glctx) {
        rt_log("[plat] SDL_GL_CreateContext: %s\n", SDL_GetError());
        return 0;
    }
    SDL_GL_SetSwapInterval(getenv("TMNT_VSYNC") ? 1 : 0);
    rt_log("[plat] GL %s / %s, window %dx%d\n", glGetString(GL_VERSION), glGetString(GL_RENDERER), win_w, win_h);
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i) && (pad = SDL_GameControllerOpen(i)) != NULL)
            break;
    return 1;
}

void plat_screen_size(int *w, int *h)
{
    *w = win_w;
    *h = win_h;
}

void plat_game_size(int *w, int *h)
{
    *w = game_w;
    *h = game_h;
}

void plat_swap(void)
{
    SDL_GL_SwapWindow(window);
}

/* TMNT_KEYS="ms:dik:hold_ms,...": scripted key presses for headless runs (DIK codes in hex, times from start). */
static void scripted_keys(void)
{
    static struct { uint32_t at, dik, hold; } keys[64];
    static int count = -1;
    static uint64_t t0;
    if (count < 0) {
        count = 0;
        t0 = plat_ticks_us();
        const char *s = getenv("TMNT_KEYS");
        while (s && *s && count < 64) {
            char *e;
            keys[count].at = (uint32_t)strtoul(s, &e, 10);
            if (*e != ':') break;
            keys[count].dik = (uint32_t)strtoul(e + 1, &e, 16);
            keys[count].hold = 150;
            if (*e == ':') keys[count].hold = (uint32_t)strtoul(e + 1, &e, 10);
            count++;
            s = *e == ',' ? e + 1 : e;
        }
    }
    uint32_t now = (uint32_t)((plat_ticks_us() - t0) / 1000U);
    for (int i = 0; i < count; i++) {
        if (now >= keys[i].at && now < keys[i].at + keys[i].hold) dik[keys[i].dik & 0xFF] = 0x80;
        else if (now >= keys[i].at + keys[i].hold && now < keys[i].at + keys[i].hold + 100) dik[keys[i].dik & 0xFF] = 0;
    }
}

int plat_pump(void)
{
    SDL_Event e;
    int alive = 1;
    scripted_keys();
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT)
            alive = 0;
        else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
            uint8_t k = dik_of(e.key.keysym.scancode);
            if (k)
                dik[k] = e.type == SDL_KEYDOWN ? 0x80 : 0;
            if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F12)
                plat_screenshot("shot.png");
        } else if (e.type == SDL_CONTROLLERDEVICEADDED && !pad)
            pad = SDL_GameControllerOpen(e.cdevice.which);
    }
    return alive;
}

const uint8_t *plat_dik_state(void)
{
    return dik;
}

void plat_pad(uint32_t *buttons, int32_t axes[4], int *pov)
{
    *buttons = 0;
    axes[0] = axes[1] = axes[2] = axes[3] = 0;
    *pov = -1;
    if (!pad)
        return;
    static const SDL_GameControllerButton map[] = { SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X,
        SDL_CONTROLLER_BUTTON_Y, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
        SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_START };
    for (int i = 0; i < 8; i++)
        if (SDL_GameControllerGetButton(pad, map[i]))
            *buttons |= 1U << i;
    axes[0] = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX) * 1000 / 32767;
    axes[1] = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY) * 1000 / 32767;
    axes[2] = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX) * 1000 / 32767;
    axes[3] = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY) * 1000 / 32767;
    int up = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP), dn = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    int lf = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT), rt = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    if (up || dn || lf || rt) {
        static const int dirs[3][3] = { { 31500, 0, 4500 }, { 27000, -1, 9000 }, { 22500, 18000, 13500 } };
        *pov = dirs[dn ? 2 : up ? 0 : 1][lf ? 0 : rt ? 2 : 1];
    }
}

uint64_t plat_ticks_us(void)
{
    return SDL_GetPerformanceCounter() * 1000000ULL / SDL_GetPerformanceFrequency();
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n)
{
    uint8_t hdr[8];
    put32(hdr, n);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    uLong crc = crc32(0, (const Bytef *)type, 4);
    if (n) crc = crc32(crc, data, n);
    uint8_t c[4];
    put32(c, (uint32_t)crc);
    fwrite(c, 1, 4, f);
}

static void write_png(const char *path, const uint8_t *rgb_top_down, int w, int h)
{
    uint8_t *raw = malloc((size_t)(w * 3 + 1) * h);
    for (int y = 0; y < h; y++) {
        raw[(size_t)y * (w * 3 + 1)] = 0;
        memcpy(raw + (size_t)y * (w * 3 + 1) + 1, rgb_top_down + (size_t)y * w * 3, (size_t)w * 3);
    }
    uLongf zn = compressBound((uLong)(w * 3 + 1) * h);
    uint8_t *z = malloc(zn);
    compress(z, &zn, raw, (uLong)(w * 3 + 1) * h);
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
    }
    free(raw);
    free(z);
}

void plat_write_rgba_png(const char *path, const uint8_t *rgba, int w, int h)
{
    uint8_t *rgb = malloc((size_t)w * h * 3);
    for (int i = 0; i < w * h; i++) memcpy(rgb + i * 3, rgba + i * 4, 3);
    write_png(path, rgb, w, h);
    free(rgb);
}

void plat_screenshot(const char *path)
{
    int w = win_w, h = win_h;
    uint8_t *px = malloc((size_t)w * h * 3), *raw = malloc((size_t)(w * 3 + 1) * h);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
    for (int y = 0; y < h; y++) {
        raw[(size_t)y * (w * 3 + 1)] = 0;
        memcpy(raw + (size_t)y * (w * 3 + 1) + 1, px + (size_t)(h - 1 - y) * w * 3, (size_t)w * 3);
    }
    uLongf zn = compressBound((uLong)(w * 3 + 1) * h);
    uint8_t *z = malloc(zn);
    compress(z, &zn, raw, (uLong)(w * 3 + 1) * h);
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
        rt_log("[plat] screenshot %s\n", path);
    }
    free(px);
    free(raw);
    free(z);
}

static void (*audio_mix)(int16_t *out, int frames);

static void audio_cb(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    audio_mix((int16_t *)stream, len / 4);
}

int plat_audio_start(void (*mix)(int16_t *out, int frames))
{
    audio_mix = mix;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        rt_log("[plat] SDL audio: %s\n", SDL_GetError());
        return 44100;
    }
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof want);
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audio_cb;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) {
        rt_log("[plat] SDL_OpenAudioDevice: %s\n", SDL_GetError());
        return 44100;
    }
    SDL_PauseAudioDevice(dev, 0);
    rt_log("[plat] audio %d Hz, %d ch, %d samples\n", have.freq, have.channels, have.samples);
    return have.freq;
}

void plat_power_info(int *arm, int *gpu, int *battery) { *arm = *gpu = *battery = 0; }

/* The [cpu] line is Vita only. */
void plat_thread_label(const char *name) { (void)name; }
void plat_cpu_report(void) {}

void plat_gl_thread(int take)
{
    if (SDL_GL_MakeCurrent(window, take ? glctx : NULL) != 0) rt_log("[plat] SDL_GL_MakeCurrent(%d): %s\n", take, SDL_GetError());
}
