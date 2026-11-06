/* pl_mpeg side of movconv: reads one movie out of the AFS archive and hands out its frames as NV12. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PL_MPEG_IMPLEMENTATION
#include "../../rt/third_party/pl_mpeg.h"

#include "plm_frames.h"

struct pm_movie {
    plm_t *plm;
    int w, h;
    double fps;
};

/* AFS header: "AFS\0", count, then (offset, size) per entry. */
static int afs_entry(const char *path, int entry, uint32_t *offset, uint32_t *size, int *count)
{
    FILE *f = fopen(path, "rb");
    uint32_t head[2];
    if (!f || fread(head, 4, 2, f) != 2 || memcmp(head, "AFS", 3) != 0 || head[1] > 4096) {
        if (f) fclose(f);
        return 0;
    }
    *count = (int)head[1];
    int ok = 1;
    if (entry >= 0) {
        uint32_t e[2];
        ok = entry < *count && fseek(f, 8 + 8L * entry, SEEK_SET) == 0 && fread(e, 4, 2, f) == 2;
        if (ok) { *offset = e[0]; *size = e[1]; }
    }
    fclose(f);
    return ok;
}

int pm_entry_count(const char *dat_path)
{
    uint32_t o, s;
    int count = 0;
    return afs_entry(dat_path, -1, &o, &s, &count) ? count : 0;
}

pm_movie *pm_open(const char *dat_path, int entry)
{
    uint32_t offset, size;
    int count;
    if (!afs_entry(dat_path, entry, &offset, &size, &count)) return NULL;
    FILE *f = fopen(dat_path, "rb");
    uint8_t *bytes = malloc(size);
    if (!f || !bytes || fseek(f, (long)offset, SEEK_SET) != 0 || fread(bytes, 1, size, f) != size) {
        if (f) fclose(f);
        free(bytes);
        return NULL;
    }
    fclose(f);
    plm_t *plm = plm_create_with_memory(bytes, size, 1);
    plm_set_audio_enabled(plm, 0);
    plm_set_loop(plm, 0);
    /* Same probe as rt/movie.c. */
    if (!plm_probe(plm, 256 * 1024) || plm_get_width(plm) <= 0) {
        plm_destroy(plm);
        return NULL;
    }
    pm_movie *m = calloc(1, sizeof *m);
    m->plm = plm;
    m->w = plm_get_width(plm);
    m->h = plm_get_height(plm);
    m->fps = plm_get_framerate(plm);
    return m;
}

void pm_size(pm_movie *m, int *width, int *height, double *fps)
{
    *width = m->w;
    *height = m->h;
    *fps = m->fps;
}

int pm_next_nv12(pm_movie *m, uint8_t *y, int y_stride, uint8_t *uv, int uv_stride)
{
    plm_frame_t *fr = plm_decode_video(m->plm);
    if (!fr) return 0;
    /* Plane rows are rounded up to whole macroblocks: copy only the picture. */
    for (int r = 0; r < m->h; r++) memcpy(y + (size_t)r * y_stride, fr->y.data + (size_t)r * fr->y.width, (size_t)m->w);
    for (int r = 0; r < m->h / 2; r++) {
        const uint8_t *cb = fr->cb.data + (size_t)r * fr->cb.width, *cr = fr->cr.data + (size_t)r * fr->cr.width;
        uint8_t *o = uv + (size_t)r * uv_stride;
        for (int i = 0; i < m->w / 2; i++) { o[2 * i] = cb[i]; o[2 * i + 1] = cr[i]; }
    }
    return 1;
}

void pm_close(pm_movie *m)
{
    if (!m) return;
    plm_destroy(m->plm);
    free(m);
}
