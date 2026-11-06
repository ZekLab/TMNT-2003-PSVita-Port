/* Frames of one TMNT movie (an AFS entry of TMNT_V0x.DAT) decoded by pl_mpeg, for movconv.swift.
   Same decoder and frame numbering as rt/movie.c, so frame k here is the frame Sofdec numbers k. */
#pragma once
#include <stdint.h>

typedef struct pm_movie pm_movie;

/* Number of movies in an AFS archive, 0 when it is not one. */
int pm_entry_count(const char *dat_path);
pm_movie *pm_open(const char *dat_path, int entry);
void pm_size(pm_movie *m, int *width, int *height, double *fps);
/* Next frame in display order as NV12 (Y plane, then interleaved Cb Cr); 0 at the end of the movie. */
int pm_next_nv12(pm_movie *m, uint8_t *y, int y_stride, uint8_t *uv, int uv_stride);
void pm_close(pm_movie *m);
