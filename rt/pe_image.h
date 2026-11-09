/* The game's memory image built from TMNT_EN.EXE (instead of a prepared image.bin). */
#ifndef TMNT_PE_IMAGE_H
#define TMNT_PE_IMAGE_H

#include <stddef.h>
#include <stdint.h>

/* Lays out the executable at dst (image_size bytes); 0 and a reason in err when it is not the expected one. */
int pe_image_load(const char *path, uint8_t *dst, uint32_t image_size, uint32_t entry, char *err, size_t errlen);

#endif
