/* Loops split between the calling thread and one helper on another core (the idea of the NFS Most Wanted Vita patch). */
#ifndef PAR_H
#define PAR_H
#include <stdint.h>

/* worker: 0 the caller, 1 the helper. */
typedef void par_fn(void *ctx, uint32_t from, uint32_t to, int worker);

void par_init(void);
/* Runs fn over 0..n in chunks and returns when all are done; below min_n, or when off, the caller does it all. */
void par_for(par_fn *fn, void *ctx, uint32_t n, uint32_t min_n);

#endif
