/* Host platform: window + GL context, events, input state, screenshots (SDL2 on the Mac, vitaGL/SceCtrl on the Vita). */
#ifndef TMNT_PLAT_H
#define TMNT_PLAT_H

#include <stdint.h>

#ifdef __vita__
#include <vitaGL.h>
#else
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#endif

int plat_video_init(int width, int height);
void plat_swap(void);
/* Processes pending events; returns 0 once the user asked to quit. */
int plat_pump(void);
/* DirectInput key state, indexed by DIK_* code: nonzero while held. */
const uint8_t *plat_dik_state(void);
/* Pad: buttons as a DirectInput joystick (bit n = button n), axes -1000..1000. */
void plat_pad(uint32_t *buttons, int32_t axes[4], int *pov);
void plat_screen_size(int *w, int *h);
/* Writes the current back buffer as a PNG. */
void plat_screenshot(const char *path);
void plat_write_rgba_png(const char *path, const uint8_t *rgba, int w, int h);
uint64_t plat_ticks_us(void);
/* Starts stereo S16 output pulling from `mix`; returns the output rate. */
int plat_audio_start(void (*mix)(int16_t *out, int frames));

#endif
