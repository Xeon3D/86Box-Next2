/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             An emulated USB keyboard and USB mouse: each a full-speed
 *             device of its own, on its own port of the USB controller (two
 *             devices, not one composite device: a composite keyboard and
 *             mouse leaves Windows 98 with neither).  Both speak their HID
 *             boot protocol too, so a BIOS with USB legacy support can drive
 *             them as well as an operating system.
 *
 *             They are the "USB Keyboard" and "USB Mouse" of Settings > Input
 *             devices, offered with a USB controller: the machine then has no
 *             other keyboard (none on the keyboard controller) or mouse, and
 *             the host's input reaches the guest only while it drives the USB
 *             ones -- the device configured and its interrupt endpoint polled
 *             in the last 500 ms.  Until then (the BIOS, DOS) it is dropped,
 *             as on a real machine without USB support.  Their options are the
 *             PS/2 ones: the keyboard's layout (the keys it has, and the HID
 *             country code) and the mouse's buttons and wheels.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <86box/86box.h>
#include <86box/version.h>
#include <86box/device.h>
#include <86box/plat.h>
#include <86box/thread.h>
#include <86box/keyboard.h>
#include <86box/mouse.h>
#include <86box/debug_cmd.h>
#include <86box/usb_next.h>

/* ---------------------------------------- the keyboard and mouse lists --- */

static const device_config_t keyboard_usb_config[] = {
  // clang-format off
    {
        .name           = "keys",
        .description    = "Keys",
        .type           = CONFIG_SELECTION,
        .default_string = NULL,
        .default_int    = KBD_101_KEY,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = {
            { .description = "101 (ANSI)",  .value = KBD_101_KEY },
            { .description = "102 (ISO)",   .value = KBD_102_KEY },
            { .description = "106 (JIS)",   .value = KBD_JIS     },
            { .description = "103 (KSC)",   .value = KBD_KSC     },
            { .description = "104 (ABNT2)", .value = KBD_ABNT2   }
        },
        .bios           = { { 0 } }
    },
    {
        .name = "", .description = "", .type = CONFIG_END
    }
  // clang-format on
};

static const device_config_t mouse_usb_config[] = {
  // clang-format off
    {
        .name           = "buttons",
        .description    = "Buttons",
        .type           = CONFIG_SELECTION,
        .default_string = NULL,
        .default_int    = 4,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = {
            { .description = "Two",             .value = 2 },
            { .description = "Three",           .value = 3 },
            { .description = "Wheel",           .value = 4 },
            { .description = "Five + Wheel",    .value = 5 },
            { .description = "Five + 2 Wheels", .value = 6 },
            { .description = ""                              }
        },
        .bios           = { { 0 } }
    },
    {
        .name = "", .description = "", .type = CONFIG_END
    }
  // clang-format on
};

/* No device of their own there: the USB controller's (below) do it. */
const device_t keyboard_usb_device = {
    .name          = "USB Keyboard",
    .internal_name = "usb",
    .flags         = 0,
    .local         = KEYBOARD_TYPE_USB,
    .init          = NULL,
    .close         = NULL,
    .reset         = NULL,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = keyboard_usb_config
};

const device_t mouse_usb_device = {
    .name          = "USB Mouse",
    .internal_name = "usb",
    .flags         = 0,
    .local         = 0,
    .init          = NULL,
    .close         = NULL,
    .reset         = NULL,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = mouse_usb_config
};

/* ------------------------------------------------------------- the HID --- */

#define HID_VID          0x86b0   /* 86B0h, as Guest Tools' ports; no vendor has it */
#define HID_KBD          0
#define HID_MOUSE        1
#define HID_POLL_TIMEOUT 500      /* ms without a poll: the guest no longer drives it */
#define KEY_QUEUE        256
#define KEY_MAX          32       /* non-modifier keys tracked down at once */

/* Standard and HID class requests, as bmRequestType << 8 | bRequest. */
#define REQ(bm, r) (((bm) << 8) | (r))

static const uint8_t kbd_report_desc[] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x06,       /* Usage (Keyboard) */
    0xa1, 0x01,       /* Collection (Application) */
    0x05, 0x07,       /*   Usage Page (Keyboard) */
    0x19, 0xe0,       /*   Usage Minimum (Left Control) */
    0x29, 0xe7,       /*   Usage Maximum (Right GUI) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x25, 0x01,       /*   Logical Maximum (1) */
    0x75, 0x01,       /*   Report Size (1) */
    0x95, 0x08,       /*   Report Count (8) */
    0x81, 0x02,       /*   Input (Data, Variable, Absolute): modifiers */
    0x95, 0x01,       /*   Report Count (1) */
    0x75, 0x08,       /*   Report Size (8) */
    0x81, 0x01,       /*   Input (Constant): reserved */
    0x95, 0x05,       /*   Report Count (5) */
    0x75, 0x01,       /*   Report Size (1) */
    0x05, 0x08,       /*   Usage Page (LEDs) */
    0x19, 0x01,       /*   Usage Minimum (Num Lock) */
    0x29, 0x05,       /*   Usage Maximum (Kana) */
    0x91, 0x02,       /*   Output (Data, Variable, Absolute): LEDs */
    0x95, 0x01,       /*   Report Count (1) */
    0x75, 0x03,       /*   Report Size (3) */
    0x91, 0x01,       /*   Output (Constant): padding */
    0x95, 0x06,       /*   Report Count (6) */
    0x75, 0x08,       /*   Report Size (8) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x26, 0xff, 0x00, /*   Logical Maximum (255) */
    0x05, 0x07,       /*   Usage Page (Keyboard) */
    0x19, 0x00,       /*   Usage Minimum (0) */
    0x2a, 0xff, 0x00, /*   Usage Maximum (255) */
    0x81, 0x00,       /*   Input (Data, Array): keys */
    0xc0              /* End Collection */
};

/* 86Box's key numbers (scan code set 1, 0x1xx for the E0 keys, 0x100 for
   Pause) to HID keyboard usages; 0 = no key on a USB keyboard. */
static const uint8_t key_usage[0x200] = {
    [0x001] = 0x29, [0x002] = 0x1e, [0x003] = 0x1f, [0x004] = 0x20, [0x005] = 0x21,
    [0x006] = 0x22, [0x007] = 0x23, [0x008] = 0x24, [0x009] = 0x25, [0x00a] = 0x26,
    [0x00b] = 0x27, [0x00c] = 0x2d, [0x00d] = 0x2e, [0x00e] = 0x2a, [0x00f] = 0x2b,
    [0x010] = 0x14, [0x011] = 0x1a, [0x012] = 0x08, [0x013] = 0x15, [0x014] = 0x17,
    [0x015] = 0x1c, [0x016] = 0x18, [0x017] = 0x0c, [0x018] = 0x12, [0x019] = 0x13,
    [0x01a] = 0x2f, [0x01b] = 0x30, [0x01c] = 0x28, [0x01d] = 0xe0, [0x01e] = 0x04,
    [0x01f] = 0x16, [0x020] = 0x07, [0x021] = 0x09, [0x022] = 0x0a, [0x023] = 0x0b,
    [0x024] = 0x0d, [0x025] = 0x0e, [0x026] = 0x0f, [0x027] = 0x33, [0x028] = 0x34,
    [0x029] = 0x35, [0x02a] = 0xe1, [0x02b] = 0x31, [0x02c] = 0x1d, [0x02d] = 0x1b,
    [0x02e] = 0x06, [0x02f] = 0x19, [0x030] = 0x05, [0x031] = 0x11, [0x032] = 0x10,
    [0x033] = 0x36, [0x034] = 0x37, [0x035] = 0x38, [0x036] = 0xe5, [0x037] = 0x55,
    [0x038] = 0xe2, [0x039] = 0x2c, [0x03a] = 0x39, [0x03b] = 0x3a, [0x03c] = 0x3b,
    [0x03d] = 0x3c, [0x03e] = 0x3d, [0x03f] = 0x3e, [0x040] = 0x3f, [0x041] = 0x40,
    [0x042] = 0x41, [0x043] = 0x42, [0x044] = 0x43, [0x045] = 0x53, [0x046] = 0x47,
    [0x047] = 0x5f, [0x048] = 0x60, [0x049] = 0x61, [0x04a] = 0x56, [0x04b] = 0x5c,
    [0x04c] = 0x5d, [0x04d] = 0x5e, [0x04e] = 0x57, [0x04f] = 0x59, [0x050] = 0x5a,
    [0x051] = 0x5b, [0x052] = 0x62, [0x053] = 0x63, [0x054] = 0x46, [0x056] = 0x64,
    [0x057] = 0x44, [0x058] = 0x45, [0x059] = 0x67, [0x064] = 0x68, [0x065] = 0x69,
    [0x066] = 0x6a, [0x067] = 0x6b, [0x068] = 0x6c, [0x069] = 0x6d, [0x06a] = 0x6e,
    [0x06b] = 0x6f, [0x06c] = 0x70, [0x06d] = 0x71, [0x06e] = 0x72, [0x070] = 0x88,
    [0x073] = 0x87, [0x076] = 0x73, [0x079] = 0x8a, [0x07b] = 0x8b, [0x07d] = 0x89,
    [0x07e] = 0x85,
    [0x0f1] = 0x91, /* Hanja (LANG2) */
    [0x0f2] = 0x90, /* Hangul (LANG1) */
    [0x100] = 0x48, /* Pause */
    [0x11c] = 0x58, [0x11d] = 0xe4, [0x135] = 0x54, [0x137] = 0x46, [0x138] = 0xe6,
    [0x145] = 0x48, [0x146] = 0x48, [0x147] = 0x4a, [0x148] = 0x52, [0x149] = 0x4b,
    [0x14b] = 0x50, [0x14d] = 0x4f, [0x14f] = 0x4d, [0x150] = 0x51, [0x151] = 0x4e,
    [0x152] = 0x49, [0x153] = 0x4c, [0x15b] = 0xe3, [0x15c] = 0xe7, [0x15d] = 0x65,
    [0x15e] = 0x66
};

typedef struct hid_t {
    usbn_device_t dev;
    int           kind;       /* HID_KBD or HID_MOUSE */
    int           layout;     /* the keyboard's: KBD_101_KEY ... KBD_ABNT2 */
    int           buttons;    /* the mouse's: 2, 3, 4 (wheel), 5 (five + wheel), 6 (five + two wheels) */
    uint8_t       report_desc[96];
    int           report_len;

    int     config;
    int     protocol;    /* 0 = boot, 1 = report */
    int     idle;        /* 4 ms units, 0 = only on a change */
    int     idle_ms;     /* since the last report */
    int     since_poll;  /* ms since the guest last polled the endpoint */
    int     halted;
    int     remote_wakeup;   /* DEVICE_REMOTE_WAKEUP, set by the guest */
    uint8_t last[8];     /* the last keyboard report sent */

    uint8_t setup[8];
    int     ctl_active;
    int     ctl_in;
    int     ctl_stall;
    uint8_t ctl_buf[256];
    int     ctl_len;
    int     ctl_pos;

    /* The keyboard as the guest sees it (emulation thread). */
    uint8_t mods;
    uint8_t keys[KEY_MAX];
    int     nkeys;

    /* The mouse buttons last reported. */
    int mouse_b;
} hid_t;

/* The keyboard plugged in now, for the UI thread's keys. */
static hid_t        *kbd;
static mutex_t      *hid_lock;
static volatile int  kbd_active;
static volatile int  mouse_active;
static volatile int  kbd_wake;    /* a key came while the guest let the keyboard sleep */

/* Key events from the UI thread to the emulation thread: usage | down << 8. */
static uint16_t key_queue[KEY_QUEUE];
static int      key_head;
static int      key_tail;

/* ---------------------------------------------------- descriptors --- */

/* The keys a layout has beyond the 101 every layout shares. */
static int
key_on_layout(uint8_t usage, int layout)
{
    switch (usage) {
        case 0x64:   /* the ISO key beside Left Shift */
            return (layout == KBD_102_KEY) || (layout == KBD_ABNT2);
        case 0x87:   /* International1: JIS Ro, ABNT2's /? */
            return (layout == KBD_JIS) || (layout == KBD_ABNT2);
        case 0x85:   /* keypad comma: ABNT2's keypad . */
            return layout == KBD_ABNT2;
        case 0x88:   /* Katakana/Hiragana, Yen, Henkan, Muhenkan */
        case 0x89:
        case 0x8a:
        case 0x8b:
            return layout == KBD_JIS;
        case 0x90:   /* Hangul, Hanja */
        case 0x91:
            return layout == KBD_KSC;
        default:
            return 1;
    }
}

/* HID 1.11, 6.2.1: bCountryCode. */
static uint8_t
layout_country(int layout)
{
    switch (layout) {
        case KBD_101_KEY:
            return 33;   /* US */
        case KBD_102_KEY:
            return 13;   /* International (ISO) */
        case KBD_JIS:
            return 15;   /* Japan (Katakana) */
        case KBD_KSC:
            return 16;   /* Korean */
        default:
            return 0;    /* not supported: ABNT2 has no code */
    }
}

static int
make_mouse_report_desc(int buttons, uint8_t *d)
{
    const int nb     = (buttons >= 5) ? 5 : ((buttons == 2) ? 2 : 3);
    const int wheel  = (buttons >= 4);
    const int hwheel = (buttons >= 6);
    int       n      = 0;

#define PUT(...)                                     \
    do {                                             \
        const uint8_t b_[] = { __VA_ARGS__ };        \
        memcpy(d + n, b_, sizeof(b_));               \
        n += sizeof(b_);                             \
    } while (0)
    PUT(0x05, 0x01, 0x09, 0x02, 0xa1, 0x01,          /* Generic Desktop, Mouse, Application */
        0x09, 0x01, 0xa1, 0x00);                     /*   Pointer, Physical */
    PUT(0x05, 0x09, 0x19, 0x01, 0x29, nb,            /*     Buttons 1-nb */
        0x15, 0x00, 0x25, 0x01, 0x95, nb, 0x75, 0x01, 0x81, 0x02);
    PUT(0x95, 0x01, 0x75, 8 - nb, 0x81, 0x01);       /*     padding to a byte */
    PUT(0x05, 0x01, 0x09, 0x30, 0x09, 0x31);         /*     X, Y */
    if (wheel)
        PUT(0x09, 0x38);                             /*     Wheel */
    PUT(0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 2 + wheel, 0x81, 0x06);
    if (hwheel)                                      /*     Consumer: AC Pan */
        PUT(0x05, 0x0c, 0x0a, 0x38, 0x02, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06);
    PUT(0xc0, 0xc0);
#undef PUT
    return n;
}

static int
make_device_desc(hid_t *h, uint8_t *d)
{
    const uint16_t pid = (h->kind == HID_KBD) ? 0x0001 : 0x0002;
    const uint8_t  desc[18] = {
        18, 0x01, 0x10, 0x01,       /* USB 1.1 */
        0x00, 0x00, 0x00,           /* class in the interface */
        8,                          /* endpoint 0 packet size */
        HID_VID & 0xff, HID_VID >> 8, pid & 0xff, pid >> 8,
        0x00, 0x01,                 /* release 1.00 */
        1, 2, 3,                    /* strings: manufacturer, product, serial number */
        1                           /* one configuration */
    };

    memcpy(d, desc, sizeof(desc));
    return sizeof(desc);
}

/* The HID descriptor comes after the endpoint, not between it and the
   interface as HID 1.10 has it: AMI's BIOS USB keyboard code of 1995-97
   (the Biostar MB-8500TUC's) takes the descriptor right after a boot
   keyboard's or mouse's interface for its endpoint and drops the device
   otherwise.  Windows and Linux find the HID descriptor either way. */
#define HID_DESC_OFFSET 25

static int
make_config_desc(hid_t *h, uint8_t *d)
{
    const uint8_t desc[34] = {
        9, 0x02, 34, 0, 1, 1, 0,             /* 34 bytes, one interface, configuration 1 */
        0xa0, 50,                            /* bus-powered, remote wakeup, 100 mA */
        9, 0x04, 0, 0, 1,                    /* interface 0, alternate 0, one endpoint */
        0x03, 0x01, (h->kind == HID_KBD) ? 1 : 2,   /* HID, boot interface, keyboard / mouse */
        0,
        7, 0x05, 0x81, 0x03, 8, 0, 10,       /* interrupt IN endpoint 1, 8 bytes, 10 ms */
        9, 0x21, 0x10, 0x01,                 /* HID 1.10 */
        (h->kind == HID_KBD) ? layout_country(h->layout) : 0,
        1, 0x22, h->report_len & 0xff, h->report_len >> 8   /* one descriptor: the report one */
    };

    memcpy(d, desc, sizeof(desc));
    return sizeof(desc);
}

static int
make_string_desc(hid_t *h, int index, uint8_t *d)
{
    const char *s;

    switch (index) {
        case 0:
            d[0] = 4;
            d[1] = 0x03;
            d[2] = 0x09;   /* English (US) */
            d[3] = 0x04;
            return 4;
        case 1:
            s = EMU_NAME;
            break;
        case 2:
            s = (h->kind == HID_KBD) ? EMU_NAME " USB Keyboard" : EMU_NAME " USB Mouse";
            break;
        case 3:
            s = "1";
            break;
        default:
            return -1;
    }
    int n = 2;
    for (; *s && (n < 126); s++, n += 2) {
        d[n]     = (uint8_t) *s;
        d[n + 1] = 0;
    }
    d[0] = n;
    d[1] = 0x03;
    return n;
}

/* ------------------------------------------------------- keyboard --- */

static void
key_queue_clear(void)
{
    key_head = key_tail = 0;
}

static void
kbd_release_all(hid_t *h)
{
    h->mods  = 0;
    h->nkeys = 0;
}

static void
kbd_apply(hid_t *h, uint16_t ev)
{
    const uint8_t usage = ev & 0xff;
    const int     down  = ev >> 8;

    if ((usage >= 0xe0) && (usage <= 0xe7)) {
        if (down)
            h->mods |= 1 << (usage - 0xe0);
        else
            h->mods &= ~(1 << (usage - 0xe0));
        return;
    }
    for (int i = 0; i < h->nkeys; i++) {
        if (h->keys[i] == usage) {
            if (!down) {
                memmove(&h->keys[i], &h->keys[i + 1], h->nkeys - i - 1);
                h->nkeys--;
            }
            return;
        }
    }
    if (down && (h->nkeys < KEY_MAX))
        h->keys[h->nkeys++] = usage;
}

static void
kbd_report(hid_t *h, uint8_t *r)
{
    memset(r, 0, 8);
    r[0] = h->mods;
    if (h->nkeys > 6)
        memset(r + 2, 0x01, 6);   /* ErrorRollOver */
    else
        memcpy(r + 2, h->keys, h->nkeys);
}

static void
kbd_set_leds(uint8_t leds)
{
    keyboard_update_states(!!(leds & 0x02), !!(leds & 0x01), !!(leds & 0x04), !!(leds & 0x10));
}

static int
kbd_poll(hid_t *h, uint8_t *buf, int len)
{
    uint8_t r[8];

    /* Take the waiting events until the report changes, so a key pressed
       and let go between two polls is still seen down once. */
    thread_wait_mutex(hid_lock);
    kbd_report(h, r);
    while (!memcmp(r, h->last, 8) && (key_tail != key_head)) {
        kbd_apply(h, key_queue[key_tail]);
        key_tail = (key_tail + 1) % KEY_QUEUE;
        kbd_report(h, r);
    }
    thread_release_mutex(hid_lock);

    if (!memcmp(r, h->last, 8) && !(h->idle && (h->idle_ms >= h->idle * 4)))
        return USBN_NAK;
    if (memcmp(r, h->last, 8))
        usbn_trace("%s: report %02X %02X %02X %02X %02X %02X %02X %02X", h->dev.name, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
    memcpy(h->last, r, 8);
    h->idle_ms = 0;
    if (len > 8)
        len = 8;
    memcpy(buf, r, len);
    return len;
}

/* ---------------------------------------------------------- mouse --- */

static int
mouse_report_len(hid_t *h)
{
    if (!h->protocol)
        return 3;   /* boot: buttons, X, Y */
    return 3 + (h->buttons >= 4) + (h->buttons >= 6);
}

static int
mouse_poll(hid_t *h, uint8_t *buf, int len)
{
    const int nb = (h->buttons >= 5) ? 5 : ((h->buttons == 2) ? 2 : 3);
    int       dx = 0;
    int       dy = 0;
    int       dz = 0;
    int       dw = 0;
    int       ox;
    int       oy;
    int       b  = h->mouse_b;
    uint8_t   r[5];
    int       n;

    /* As the PS/2 mouse: only while the host's mouse is the machine's. */
    if ((mouse_live() || debug_cmd_mouse || (video_fullscreen && !fullscreen_ui_visible)) && (mouse_input_mode == 0)) {
        mouse_subtract_coords(&dx, &dy, &ox, &oy, -127, 127, 0, 0);
        if (h->buttons >= 4)
            mouse_subtract_z(&dz, -127, 127, 0);
        if (h->buttons >= 6)
            mouse_subtract_w(&dw, -127, 127, 0);
        b = mouse_get_buttons_ex() & ((1 << nb) - 1);
    }
    if (!dx && !dy && !dz && !dw && (b == h->mouse_b) && !(h->idle && (h->idle_ms >= h->idle * 4)))
        return USBN_NAK;

    r[0] = h->protocol ? b : (b & 0x07);
    r[1] = (uint8_t) dx;
    r[2] = (uint8_t) dy;
    n    = 3;
    if (h->protocol && (h->buttons >= 4))
        r[n++] = (uint8_t) dz;
    if (h->protocol && (h->buttons >= 6))
        r[n++] = (uint8_t) dw;

    h->mouse_b = b;
    h->idle_ms = 0;
    if (n > len)
        n = len;
    memcpy(buf, r, n);
    return n;
}

/* ------------------------------------------------------- requests --- */

static void
hid_defaults(hid_t *h)
{
    h->protocol = 1;
    h->idle     = (h->kind == HID_KBD) ? 125 : 0;   /* 500 ms for a keyboard, as the HID spec has it */
    h->idle_ms  = 0;
    h->halted   = 0;
    h->mouse_b  = 0;
    memset(h->last, 0, sizeof(h->last));
}

/* An IN request's answer into ctl_buf: its length, or -1 to stall. */
static int
request_in(hid_t *h)
{
    const uint8_t  bm   = h->setup[0];
    const uint8_t  req  = h->setup[1];
    const uint16_t wval = h->setup[2] | (h->setup[3] << 8);
    const uint16_t widx = h->setup[4] | (h->setup[5] << 8);
    uint8_t       *d    = h->ctl_buf;

    switch (REQ(bm, req)) {
        case REQ(0x80, 0x00):   /* GET_STATUS (device): bit 1 remote wakeup */
            d[0] = h->remote_wakeup ? 0x02 : 0x00;
            d[1] = 0;
            return 2;
        case REQ(0x81, 0x00):   /* GET_STATUS (interface) */
            d[0] = d[1] = 0;
            return 2;
        case REQ(0x82, 0x00):   /* GET_STATUS (endpoint) */
            d[0] = ((widx & 0xff) == 0x81) && h->halted;
            d[1] = 0;
            return (((widx & 0xff) == 0x81) || !(widx & 0x7f)) ? 2 : -1;
        case REQ(0x80, 0x06):   /* GET_DESCRIPTOR */
            switch (wval >> 8) {
                case 0x01:
                    return make_device_desc(h, d);
                case 0x02:
                    return make_config_desc(h, d);
                case 0x03:
                    return make_string_desc(h, wval & 0xff, d);
                default:
                    return -1;
            }
        case REQ(0x81, 0x06):   /* GET_DESCRIPTOR (interface): HID, report */
            if ((widx & 0xff) != 0)
                return -1;
            if ((wval >> 8) == 0x21) {
                uint8_t cfg[34];
                make_config_desc(h, cfg);
                memcpy(d, cfg + HID_DESC_OFFSET, 9);
                return 9;
            }
            if ((wval >> 8) == 0x22) {
                memcpy(d, h->report_desc, h->report_len);
                return h->report_len;
            }
            return -1;
        case REQ(0x80, 0x08):   /* GET_CONFIGURATION */
            d[0] = h->config;
            return 1;
        case REQ(0x81, 0x0a):   /* GET_INTERFACE */
            d[0] = 0;
            return ((widx & 0xff) == 0) ? 1 : -1;
        case REQ(0xa1, 0x01):   /* GET_REPORT */
            if ((widx & 0xff) != 0)
                return -1;
            if (h->kind == HID_KBD) {
                thread_wait_mutex(hid_lock);
                kbd_report(h, d);
                thread_release_mutex(hid_lock);
                return 8;
            }
            memset(d, 0, 5);
            d[0] = h->mouse_b;
            return mouse_report_len(h);
        case REQ(0xa1, 0x02):   /* GET_IDLE */
            if ((widx & 0xff) != 0)
                return -1;
            d[0] = h->idle;
            return 1;
        case REQ(0xa1, 0x03):   /* GET_PROTOCOL */
            if ((widx & 0xff) != 0)
                return -1;
            d[0] = h->protocol;
            return 1;
        default:
            return -1;
    }
}

/* An OUT or no-data request, at its status stage.  Returns 0, or -1 to stall. */
static int
request_out(hid_t *h, const uint8_t *data, int len)
{
    const uint8_t  bm   = h->setup[0];
    const uint8_t  req  = h->setup[1];
    const uint16_t wval = h->setup[2] | (h->setup[3] << 8);
    const uint16_t widx = h->setup[4] | (h->setup[5] << 8);

    switch (REQ(bm, req)) {
        case REQ(0x00, 0x05):   /* SET_ADDRESS */
            h->dev.addr = wval & 0x7f;
            return 0;
        case REQ(0x00, 0x09):   /* SET_CONFIGURATION */
            if (wval > 1)
                return -1;
            h->config = wval;
            hid_defaults(h);
            return 0;
        case REQ(0x00, 0x01):   /* CLEAR_FEATURE / SET_FEATURE (device): remote wakeup */
        case REQ(0x00, 0x03):
            if (wval != 1)
                return -1;
            h->remote_wakeup = (req == 0x03);
            return 0;
        case REQ(0x01, 0x01):   /* (interface): nothing to set */
        case REQ(0x01, 0x03):
            return 0;
        case REQ(0x02, 0x01):   /* CLEAR_FEATURE (endpoint): ENDPOINT_HALT */
        case REQ(0x02, 0x03):   /* SET_FEATURE (endpoint) */
            if ((widx & 0xff) == 0x81)
                h->halted = (req == 0x03);
            return (((widx & 0xff) == 0x81) || !(widx & 0x7f)) ? 0 : -1;
        case REQ(0x01, 0x0b):   /* SET_INTERFACE */
            return (((widx & 0xff) == 0) && (wval == 0)) ? 0 : -1;
        case REQ(0x21, 0x09):   /* SET_REPORT: the keyboard's LEDs */
            if ((widx & 0xff) != 0)
                return -1;
            if ((h->kind == HID_KBD) && (len >= 1))
                kbd_set_leds(data[0]);
            return 0;
        case REQ(0x21, 0x0a):   /* SET_IDLE */
            if ((widx & 0xff) != 0)
                return -1;
            h->idle    = wval >> 8;
            h->idle_ms = 0;
            return 0;
        case REQ(0x21, 0x0b):   /* SET_PROTOCOL */
            if ((widx & 0xff) != 0)
                return -1;
            h->protocol = wval & 1;
            return 0;
        default:
            return -1;
    }
}

static int
hid_control(hid_t *h, uint8_t pid, uint8_t *buf, int len)
{
    if (pid == USB_PID_SETUP) {
        if (len != 8)
            return USBN_STALL;
        memcpy(h->setup, buf, 8);
        /* A HID class request to an endpoint is one to the interface: AMI's
           BIOS USB keyboard code of 1995-97 sends SET_PROTOCOL to the
           interrupt endpoint (as early HID drafts had it), and Windows 98 SE,
           finding the HID descriptor after the endpoint's (which AMI's code
           needs), sends SET_IDLE to endpoint 0 and asks the endpoint for the
           report descriptor.  Such devices answer whatever the recipient. */
        if (((buf[0] & 0x7f) == 0x22) && ((buf[4] & 0x7f) <= 1) && !buf[5]) {
            h->setup[0] = (buf[0] & 0x80) | 0x21;
            h->setup[4] = 0;
        } else if ((buf[0] == 0x82) && (buf[1] == 0x06) && ((buf[3] == 0x21) || (buf[3] == 0x22)) &&
                   ((buf[4] & 0x7f) <= 1) && !buf[5]) {
            h->setup[0] = 0x81;
            h->setup[4] = 0;
        }
        usbn_trace("%s: SETUP %02X %02X wValue %04X wIndex %04X wLength %d", h->dev.name, buf[0], buf[1],
                   buf[2] | (buf[3] << 8), buf[4] | (buf[5] << 8), buf[6] | (buf[7] << 8));
        h->ctl_active = 1;
        h->ctl_in     = buf[0] & 0x80;
        h->ctl_pos    = 0;
        h->ctl_stall  = 0;
        h->ctl_len    = 0;
        if (h->ctl_in) {
            const int want = buf[6] | (buf[7] << 8);
            const int n    = request_in(h);
            if (n < 0)
                h->ctl_stall = 1;
            else
                h->ctl_len = (n < want) ? n : want;
        }
        return 8;
    }
    if (!h->ctl_active || h->ctl_stall)
        return USBN_STALL;

    if (h->ctl_in) {
        if (pid == USB_PID_OUT) {   /* status stage */
            h->ctl_active = 0;
            return 0;
        }
        int n = h->ctl_len - h->ctl_pos;
        if (n > len)
            n = len;
        memcpy(buf, h->ctl_buf + h->ctl_pos, n);
        h->ctl_pos += n;
        return n;
    }

    if (pid == USB_PID_OUT) {       /* data stage */
        if (h->ctl_len + len > (int) sizeof(h->ctl_buf))
            return USBN_STALL;
        memcpy(h->ctl_buf + h->ctl_len, buf, len);
        h->ctl_len += len;
        return len;
    }
    h->ctl_active = 0;              /* status stage: now the request is done */
    if (request_out(h, h->ctl_buf, h->ctl_len) < 0) {
        usbn_trace("  request %02X %02X stalled", h->setup[0], h->setup[1]);
        return USBN_STALL;
    }
    return 0;
}

/* ---------------------------------------------------- the devices --- */

static int
hid_packet(usbn_device_t *d, uint8_t pid, uint8_t ep, uint8_t *buf, int len)
{
    hid_t *h = (hid_t *) d->priv;

    if (ep == 0)
        return hid_control(h, pid, buf, len);
    if ((pid != USB_PID_IN) || !h->config || (ep != 1) || h->halted)
        return USBN_STALL;
    h->since_poll = 0;
    return (h->kind == HID_KBD) ? kbd_poll(h, buf, len) : mouse_poll(h, buf, len);
}

/* Whether the guest drives it: worked out once a frame. */
static void
hid_update_active(hid_t *h)
{
    const int on = h->config && !h->halted && (h->since_poll < HID_POLL_TIMEOUT);

    if (h->kind == HID_KBD) {
        if (on == kbd_active)
            return;
        /* Whatever was down is forgotten: the next time the guest drives
           the keyboard, it starts with nothing held. */
        thread_wait_mutex(hid_lock);
        key_queue_clear();
        kbd_release_all(h);
        memset(h->last, 0, 8);
        kbd_active = on;
        thread_release_mutex(hid_lock);
    } else {
        if (on == mouse_active)
            return;
        mouse_active = on;
    }
    pclog("USB: the guest %s the USB %s\n", on ? "drives" : "no longer drives", (h->kind == HID_KBD) ? "keyboard" : "mouse");
    usbn_trace("%s: %s by the guest's USB driver", h->dev.name, on ? "driven" : "no longer driven");
}

static void
hid_frame(usbn_device_t *d)
{
    hid_t *h = (hid_t *) d->priv;

    if (h->since_poll < HID_POLL_TIMEOUT)
        h->since_poll++;
    if (h->idle_ms < 0x10000)
        h->idle_ms++;
    hid_update_active(h);
    /* A key pressed while the guest suspended the keyboard with remote
       wakeup enabled: resume signalling, which the controller turns into
       a resume of the port or the bus (USB 1.1 9.1.1.6, 11.4.4). */
    if ((h->kind == HID_KBD) && kbd_wake) {
        kbd_wake = 0;
        if (h->remote_wakeup && h->config) {
            usbn_trace("%s: remote wakeup", h->dev.name);
            usbn_remote_wakeup(d);
        }
    }
}

static void
hid_reset(usbn_device_t *d)
{
    hid_t *h = (hid_t *) d->priv;

    h->config        = 0;
    h->remote_wakeup = 0;
    h->ctl_active    = 0;
    h->since_poll = HID_POLL_TIMEOUT;
    hid_defaults(h);
    hid_update_active(h);
}

static void
hid_destroy(usbn_device_t *d)
{
    hid_t *h = (hid_t *) d->priv;

    thread_wait_mutex(hid_lock);
    if (h->kind == HID_KBD) {
        if (kbd == h)
            kbd = NULL;
        kbd_active = 0;
        key_queue_clear();
    } else
        mouse_active = 0;
    thread_release_mutex(hid_lock);
    free(h);
}

static hid_t *
hid_create(int kind)
{
    hid_t *h;

    if (hid_lock == NULL)
        hid_lock = thread_create_mutex();

    h             = calloc(1, sizeof(hid_t));
    h->kind       = kind;
    h->since_poll = HID_POLL_TIMEOUT;
    snprintf(h->dev.name, sizeof(h->dev.name), "%s", (kind == HID_KBD) ? EMU_NAME " USB Keyboard" : EMU_NAME " USB Mouse");
    /* Low speed, as the keyboards and mice of the time: AMI's USB keyboard
       code of 1995-97 (the Biostar MB-8500TUC's) drops a full-speed one. */
    h->dev.speed   = USBN_SPEED_LOW;
    h->dev.priv    = h;
    h->dev.packet  = hid_packet;
    h->dev.reset   = hid_reset;
    h->dev.frame   = hid_frame;
    h->dev.destroy = hid_destroy;
    hid_defaults(h);
    return h;
}

usbn_device_t *
usbn_hid_create_keyboard(int layout)
{
    hid_t *h = hid_create(HID_KBD);

    h->layout     = layout;
    h->report_len = sizeof(kbd_report_desc);
    memcpy(h->report_desc, kbd_report_desc, sizeof(kbd_report_desc));
    thread_wait_mutex(hid_lock);
    kbd = h;
    thread_release_mutex(hid_lock);
    return &h->dev;
}

usbn_device_t *
usbn_hid_create_mouse(int buttons)
{
    hid_t *h = hid_create(HID_MOUSE);

    h->buttons    = ((buttons >= 2) && (buttons <= 6)) ? buttons : 4;
    h->report_len = make_mouse_report_desc(h->buttons, h->report_desc);
    return &h->dev;
}

/* A device into the first free port. */
static void
hid_plug(usbn_device_t *d)
{
    for (int p = 0; p < USBN_PORTS; p++) {
        if (!usbn_port_busy(p)) {
            usbn_attach(p, d);
            return;
        }
    }
    pclog("USB: no free port for the %s\n", d->name);
    d->destroy(d);
}

static int
config_int(const device_t *dev, const char *name)
{
    int v;

    device_context(dev);
    v = device_get_config_int(name);
    device_context_restore();
    return v;
}

/* At the hard reset, once the controller has plugged back in the host
   devices it remembers: the keyboard, then the mouse, each into the first
   free port. */
void
usbn_hid_attach(void)
{
    if (!usbn_present())
        return;
    if (keyboard_type == KEYBOARD_TYPE_USB)
        hid_plug(usbn_hid_create_keyboard(config_int(&keyboard_usb_device, "keys")));
    if (mouse_get_device(mouse_type) == &mouse_usb_device) {
        const int buttons = config_int(&mouse_usb_device, "buttons");
        hid_plug(usbn_hid_create_mouse(buttons));
        mouse_set_buttons(buttons);   /* the host's mouse is captured for it, as for a PS/2 one */
    }
}

int
usbn_hid_keyboard_active(void)
{
    return kbd_active;
}

int
usbn_hid_mouse_active(void)
{
    return mouse_active;
}

/* From any thread: a key for the USB keyboard (86Box's key number),
   dropped while the guest does not drive it or its layout has no such key. */
void
usbn_hid_key(int down, uint16_t scan)
{
    const uint8_t usage = key_usage[scan & 0x1ff];

    if (!usage || (hid_lock == NULL))
        return;
    thread_wait_mutex(hid_lock);
    if (kbd && kbd_active && key_on_layout(usage, kbd->layout)) {
        const int next = (key_head + 1) % KEY_QUEUE;
        if (next != key_tail) {
            key_queue[key_head] = usage | (down ? 0x100 : 0);
            key_head            = next;
        }
    } else if (kbd && down && kbd->remote_wakeup)
        kbd_wake = 1;   /* the key wakes the guest; it is not delivered */
    thread_release_mutex(hid_lock);
}
