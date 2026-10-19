/* COM objects the guest can call: a guest-memory object {vtbl, host index} whose vtable slots are host thunks. */
#ifndef TMNT_COM_H
#define TMNT_COM_H

#include <stdint.h>
#include "runtime.h"
#include "com_tables.h"

typedef uint32_t (*com_fn)(CPU *__restrict c);
typedef struct { const char *name; com_fn fn; } com_impl;

/* Builds (once per interface) a guest vtable from the method table; methods without an implementation log once and return 0. */
uint32_t com_vtable(const char *iface, const com_method_info *info, int count, const com_impl *impls);
#define COM_VTABLE(iface, impls) com_vtable(#iface, iface##_methods, iface##_COUNT, impls)

/* A guest object of `bytes` (>= 8) bytes: [0] vtable, [4] host index of `host`. */
uint32_t com_new(uint32_t vtbl, uint32_t bytes, void *host);
void *com_host(uint32_t guest_obj);
void com_forget(uint32_t guest_obj);

#define THIS_HOST(type) ((type *)com_host(ARG(0)))

#endif
