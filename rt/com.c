/* Guest-visible COM objects backed by host structures. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "com.h"

#define MAX_OBJECTS 65536
static void *objects[MAX_OBJECTS];
static uint32_t free_hint = 1;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct { const char *iface; uint32_t vtbl; } vt_cache;
static vt_cache vtables[64];
static int vt_count;

static uint32_t com_unimplemented(CPU *__restrict c);

typedef struct { rt_thunk thunk; char name[96]; int warned; } com_thunk;

extern __thread const rt_thunk *rt_cur_thunk;

static uint32_t com_unimplemented(CPU *__restrict c)
{
    com_thunk *t = (com_thunk *)rt_cur_thunk;
    if (!t->warned) {
        t->warned = 1;
        rt_log("[com] unimplemented %s (this %08x, %08x, %08x, %08x)\n", t->name, ARG(0), ARG(1), ARG(2), ARG(3));
    }
    return 0;
}

uint32_t com_vtable(const char *iface, const com_method_info *info, int count, const com_impl *impls)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < vt_count; i++) {
        if (strcmp(vtables[i].iface, iface) == 0) {
            pthread_mutex_unlock(&lock);
            return vtables[i].vtbl;
        }
    }
    uint32_t vt = rt_alloc(4U * (uint32_t)count);
    for (int i = 0; i < count; i++) {
        com_thunk *t = calloc(1, sizeof *t);
        snprintf(t->name, sizeof t->name, "%s::%s", iface, info[i].name);
        t->thunk.name = t->name;
        t->thunk.nargs = info[i].nargs;
        t->thunk.fn = com_unimplemented;
        for (const com_impl *m = impls; m && m->name; m++)
            if (strcmp(m->name, info[i].name) == 0)
                t->thunk.fn = m->fn;
        st32(vt + 4U * (uint32_t)i, rt_add_thunk(&t->thunk));
    }
    vtables[vt_count].iface = iface;
    vtables[vt_count].vtbl = vt;
    vt_count++;
    pthread_mutex_unlock(&lock);
    return vt;
}

uint32_t com_new(uint32_t vtbl, uint32_t bytes, void *host)
{
    pthread_mutex_lock(&lock);
    uint32_t idx = 0;
    for (uint32_t i = 0; i < MAX_OBJECTS - 1; i++) {
        uint32_t k = (free_hint + i) % (MAX_OBJECTS - 1) + 1;
        if (objects[k] == NULL) {
            idx = k;
            break;
        }
    }
    if (idx == 0) {
        pthread_mutex_unlock(&lock);
        rt_log("[com] out of object slots\n");
        exit(2);
    }
    objects[idx] = host;
    free_hint = idx + 1;
    uint32_t obj = rt_alloc(bytes < 16U ? 16U : bytes);
    pthread_mutex_unlock(&lock);
    memset(G2H(obj), 0, bytes < 16U ? 16U : bytes);
    st32(obj, vtbl);
    st32(obj + 4, idx);
    return obj;
}

void *com_host(uint32_t guest_obj)
{
    if (guest_obj == 0)
        return NULL;
    uint32_t idx = ld32(guest_obj + 4);
    return idx < MAX_OBJECTS ? objects[idx] : NULL;
}

void com_forget(uint32_t guest_obj)
{
    pthread_mutex_lock(&lock);
    uint32_t idx = ld32(guest_obj + 4);
    if (idx < MAX_OBJECTS)
        objects[idx] = NULL;
    rt_free(guest_obj);
    pthread_mutex_unlock(&lock);
}
