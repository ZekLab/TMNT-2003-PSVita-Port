/* DirectInput 8: keyboard from the platform DIK state, one gamepad as a DirectInput joystick, an idle mouse. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "com.h"
#include "dx_consts.h"
#include "plat.h"

#define DI_OK 0U
#define DIERR_NOTFOUND 0x80070002U
#define E_NOINTERFACE 0x80004002U

enum { DEV_KEYBOARD, DEV_MOUSE, DEV_JOYSTICK };

typedef struct di_dev {
    int kind;
    uint32_t guest, refs;
    int type;
    uint32_t state_size;
    int32_t range_min[8], range_max[8];
} di_dev;

/* GUID_SysKeyboard / GUID_SysMouse: {6F1D2B61/60-D5A0-11CF-BFC7-444553540000}; the pad uses a private instance GUID. */
static const uint8_t guid_keyboard[16] = { 0x61, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
static const uint8_t guid_mouse[16] = { 0x60, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
static const uint8_t guid_pad[16] = { 0x54, 0x4D, 0x4E, 0x54, 0x50, 0x41, 0x44, 0x30, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 };

static uint32_t m_AddRef(CPU *c) { di_dev *d = com_host(ARG(0)); return d ? ++d->refs : 1; }
static uint32_t m_Release(CPU *c) { di_dev *d = com_host(ARG(0)); if (d && d->refs) d->refs--; return d ? d->refs : 0; }
static uint32_t m_QueryInterface(CPU *c) { if (ARG(2)) st32(ARG(2), 0); return E_NOINTERFACE; }
static uint32_t m_Ok(CPU *c) { (void)c; return DI_OK; }

static int32_t scale_axis(di_dev *d, int i, int32_t v)
{
    int64_t lo = d->range_min[i], hi = d->range_max[i];
    return (int32_t)(lo + (int64_t)(v + 1000) * (hi - lo) / 2000);
}

static uint32_t dev_GetDeviceState(CPU *c)
{
    di_dev *d = THIS_HOST(di_dev);
    uint32_t n = ARG(1), p = ARG(2);
    memset(G2H(p), 0, n);
    if (d->type == DEV_KEYBOARD) {
        plat_pump();
        memcpy(G2H(p), plat_dik_state(), n < 256 ? n : 256);
    } else if (d->type == DEV_JOYSTICK) {
        uint32_t buttons;
        int32_t axes[4];
        int pov;
        plat_pad(&buttons, axes, &pov);
        if (n >= 80) {
            st32(p + 0, (uint32_t)scale_axis(d, 0, axes[0]));
            st32(p + 4, (uint32_t)scale_axis(d, 1, axes[1]));
            st32(p + 8, (uint32_t)scale_axis(d, 2, 0));
            st32(p + 12, (uint32_t)scale_axis(d, 3, axes[2]));
            st32(p + 16, (uint32_t)scale_axis(d, 4, axes[3]));
            st32(p + 20, (uint32_t)scale_axis(d, 5, 0));
            st32(p + 32, (uint32_t)pov);
            st32(p + 36, 0xFFFFFFFFU); st32(p + 40, 0xFFFFFFFFU); st32(p + 44, 0xFFFFFFFFU);
            for (int b = 0; b < 32; b++) st8(p + 48 + (uint32_t)b, (buttons >> b) & 1 ? 0x80 : 0);
        }
    }
    return DI_OK;
}
static uint32_t dev_GetDeviceData(CPU *c) { if (ARG(3)) st32(ARG(3), 0); return DI_OK; }
static uint32_t dev_SetDataFormat(CPU *c)
{
    di_dev *d = THIS_HOST(di_dev);
    d->state_size = ld32(ARG(1) + 12);
    rt_log("[dinput] SetDataFormat(device %d, %u bytes, %u objects)\n", d->type, d->state_size, ld32(ARG(1) + 16));
    return DI_OK;
}
static uint32_t dev_SetProperty(CPU *c)
{
    di_dev *d = THIS_HOST(di_dev);
    uint32_t prop = ARG(1), hdr = ARG(2);
    if (prop == 4) {
        /* DIPROP_RANGE: {DIPROPHEADER(16 bytes), lMin, lMax}; dwHow 0 = whole device, else by offset */
        int32_t lo = (int32_t)ld32(hdr + 16), hi = (int32_t)ld32(hdr + 20);
        uint32_t how = ld32(hdr + 12), obj = ld32(hdr + 8);
        for (int i = 0; i < 8; i++)
            if (how == 0 || (how == 1 && obj == (uint32_t)i * 4) || (how == 2 && ((obj >> 8) & 0xFFFFU) == (uint32_t)i && (obj & 3U))) { d->range_min[i] = lo; d->range_max[i] = hi; }
    }
    return DI_OK;
}
static uint32_t dev_GetCapabilities(CPU *c)
{
    di_dev *d = THIS_HOST(di_dev);
    uint32_t p = ARG(1);
    uint32_t size = ld32(p);
    memset(G2H(p + 4), 0, size > 4 ? size - 4 : 0);
    st32(p + 4, 0x00000001U);
    st32(p + 8, d->type == DEV_JOYSTICK ? 0x00000214U : d->type == DEV_KEYBOARD ? 0x00000403U : 0x00000212U);
    st32(p + 12, d->type == DEV_JOYSTICK ? 6 : 0);
    st32(p + 16, d->type == DEV_JOYSTICK ? 12 : d->type == DEV_KEYBOARD ? 128 : 3);
    st32(p + 20, d->type == DEV_JOYSTICK ? 1 : 0);
    return DI_OK;
}
static void fill_instance(uint32_t p, const uint8_t *guid, uint32_t devtype, const char *name)
{
    uint32_t size = ld32(p);
    memset(G2H(p + 4), 0, size > 4 ? size - 4 : 0);
    memcpy(G2H(p + 4), guid, 16);
    memcpy(G2H(p + 20), guid, 16);
    st32(p + 36, devtype);
    strncpy((char *)G2H(p + 40), name, 259);
    strncpy((char *)G2H(p + 300), name, 259);
}
static uint32_t dev_GetDeviceInfo(CPU *c)
{
    di_dev *d = THIS_HOST(di_dev);
    fill_instance(ARG(1), d->type == DEV_JOYSTICK ? guid_pad : d->type == DEV_KEYBOARD ? guid_keyboard : guid_mouse,
                  d->type == DEV_JOYSTICK ? 0x00010114U : d->type == DEV_KEYBOARD ? 0x00000413U : 0x00000312U,
                  d->type == DEV_JOYSTICK ? "Gamepad" : d->type == DEV_KEYBOARD ? "Keyboard" : "Mouse");
    return DI_OK;
}
/* Joystick objects: 6 absolute axes (X, Y, Z, Rx, Ry, Rz at DIJOYSTATE offsets 0..20), 1 POV, 12 buttons. */
static uint32_t dev_EnumObjects(CPU *c)
{
    di_dev *d = THIS_HOST(di_dev);
    uint32_t cb = ARG(1), ref = ARG(2), flags = ARG(3);
    if (d->type != DEV_JOYSTICK || (flags & 0x01000000U)) return DI_OK;
    static const uint8_t axis_guid_last[6] = { 0xE0, 0xE1, 0xE2, 0xF4, 0xF5, 0xE3 };
    uint32_t o = rt_alloc(316);
    int n_axes = (flags == 0 || (flags & 0x03U)) ? 6 : 0, n_pov = (flags == 0 || (flags & 0x10U)) ? 1 : 0, n_btn = (flags == 0 || (flags & 0x0CU)) ? 12 : 0;
    for (int k = 0; k < n_axes + n_pov + n_btn; k++) {
        memset(G2H(o), 0, 316);
        st32(o, 316);
        static const uint8_t base[16] = { 0xE0, 0x02, 0x6D, 0xA3, 0xF3, 0xC9, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
        memcpy(G2H(o + 4), base, 16);
        const char *name;
        char nb[16];
        if (k < n_axes) {
            st8(o + 4, axis_guid_last[k]);
            st32(o + 20, (uint32_t)k * 4);
            st32(o + 24, 0x02U | ((uint32_t)k << 8));
            static const char *names[6] = { "X Axis", "Y Axis", "Z Axis", "X Rotation", "Y Rotation", "Z Rotation" };
            name = names[k];
        } else if (k < n_axes + n_pov) {
            st8(o + 4, 0xF2);
            st32(o + 20, 32);
            st32(o + 24, 0x10U);
            name = "POV";
        } else {
            int b = k - n_axes - n_pov;
            st8(o + 4, 0xF0);
            st32(o + 20, 48U + (uint32_t)b);
            st32(o + 24, 0x04U | ((uint32_t)b << 8));
            snprintf(nb, sizeof nb, "Button %d", b);
            name = nb;
        }
        strcpy((char *)G2H(o + 32), name);
        uint32_t a[2] = { o, ref };
        if (rt_call_guest(c, cb, 2, a) == 0) break;
    }
    rt_free(o);
    return DI_OK;
}

static uint32_t device_vtbl(void)
{
    static const com_impl impls[] = {
        { "QueryInterface", m_QueryInterface }, { "AddRef", m_AddRef }, { "Release", m_Release },
        { "GetCapabilities", dev_GetCapabilities }, { "EnumObjects", dev_EnumObjects }, { "SetProperty", dev_SetProperty },
        { "GetProperty", m_Ok }, { "Acquire", m_Ok }, { "Unacquire", m_Ok }, { "GetDeviceState", dev_GetDeviceState },
        { "GetDeviceData", dev_GetDeviceData }, { "SetDataFormat", dev_SetDataFormat }, { "SetEventNotification", m_Ok },
        { "SetCooperativeLevel", m_Ok }, { "GetDeviceInfo", dev_GetDeviceInfo }, { "Poll", m_Ok }, { NULL, NULL },
    };
    return COM_VTABLE(IDirectInputDevice8A, impls);
}

static uint32_t di_CreateDevice(CPU *c)
{
    const uint8_t *g = G2H(ARG(1));
    di_dev *d = calloc(1, sizeof *d);
    d->refs = 1;
    d->type = memcmp(g, guid_keyboard, 16) == 0 ? DEV_KEYBOARD : memcmp(g, guid_mouse, 16) == 0 ? DEV_MOUSE : DEV_JOYSTICK;
    for (int i = 0; i < 8; i++) { d->range_min[i] = 0; d->range_max[i] = 65535; }
    d->guest = com_new(device_vtbl(), 16, d);
    rt_log("[dinput] CreateDevice(%s)\n", d->type == DEV_KEYBOARD ? "keyboard" : d->type == DEV_MOUSE ? "mouse" : "joystick");
    st32(ARG(2), d->guest);
    return DI_OK;
}
static uint32_t di_EnumDevices(CPU *c)
{
    uint32_t type = ARG(1), cb = ARG(2), ref = ARG(3);
    rt_log("[dinput] EnumDevices(type %u)\n", type);
    if (type == 4 || type == 0) {
        uint32_t inst = rt_alloc(580);
        st32(inst, 580);
        fill_instance(inst, guid_pad, 0x00010114U, "Gamepad");
        uint32_t args[2] = { inst, ref };
        rt_call_guest(c, cb, 2, args);
        rt_free(inst);
    }
    return DI_OK;
}
static uint32_t di_GetDeviceStatus(CPU *c) { (void)c; return DI_OK; }

uint32_t dinput8_create(CPU *c, uint32_t pp)
{
    static const com_impl impls[] = {
        { "QueryInterface", m_QueryInterface }, { "AddRef", m_AddRef }, { "Release", m_Release },
        { "CreateDevice", di_CreateDevice }, { "EnumDevices", di_EnumDevices }, { "GetDeviceStatus", di_GetDeviceStatus },
        { "Initialize", m_Ok }, { NULL, NULL },
    };
    static di_dev root = { 0, 0, 1, -1, 0, { 0 }, { 0 } };
    (void)c;
    if (!root.guest)
        root.guest = com_new(COM_VTABLE(IDirectInput8A, impls), 16, &root);
    st32(pp, root.guest);
    return DI_OK;
}
