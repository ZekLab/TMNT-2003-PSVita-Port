/* Movie pictures from pl_mpeg in place of Sofdec's (rt/movie.c). */
#pragma once
#include <stdint.h>

void movie_note_open(int file, const char *host_path);
void movie_note_read(int file, uint64_t offset);
/* The number of the frame Sofdec is about to show (frame info + 0x20). */
void movie_set_frame(int frame);
/* Points movie_pin at the current movie frame (RGBA, not the texture's BGRA); 0 when no movie is known (the caller converts the
   frame of Sofdec). */
int movie_frame_rgba(uint8_t *dst, uint32_t pitch, int w, int h);
/* Guest address of the movie texture memory: d3d9_gl uploads it as RGBA, without the BGRA conversion. */
extern uint32_t movie_rgba_mem;
/* The decoded frame (RGBA, rows of movie_pin_w pixels) d3d9_gl uploads into that texture instead of the guest memory; NULL when
   the guest memory holds the picture (black before the first frame). */
extern const uint8_t *movie_pin;
extern int movie_pin_w, movie_pin_h;

/* Vita: movies converted by tools/movies/movconv (H.264 in ux0:data/TMNT2003/movies) are decoded by the hardware decoder into
   MOVIE_HW_SLOTS video-memory pictures (RGBA, movie_hw_w x movie_hw_h, rows of movie_hw_w pixels). d3d9_gl makes each one a GL
   texture and binds the one in movie_pin_slot (-1: none) instead of uploading anything, and notes in movie_hw_bound_frame when
   it did, so the decoder leaves a picture alone while the GPU may still read it. */
#define MOVIE_HW_SLOTS 8
extern int movie_pin_slot;
extern void *movie_hw_mem[MOVIE_HW_SLOTS];
extern uint32_t movie_hw_w, movie_hw_h;
extern volatile uint32_t movie_hw_bound_frame[MOVIE_HW_SLOTS];
/* Once, on the main thread, after vitaGL starts; without converted movies (or on any error) the movies stay on pl_mpeg. */
void movie_hw_init(void);
