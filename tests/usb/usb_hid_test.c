/*
 * 86Box-Next  Tests for the emulated USB keyboard and mouse (src/usb/usb_hid.c).
 *
 *             Each device is driven packet by packet the way a controller
 *             would on a guest's behalf: enumeration, the descriptors, the
 *             HID class requests, the interrupt endpoint -- and when the
 *             host's input reaches them (only while the guest drives them),
 *             the keyboard layouts and the mouse's buttons and wheels.  The
 *             mouse framework and the keyboard LEDs are faked.
 *
 *             No test framework: build with CTest (BUILD_TESTING=ON).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/keyboard.h>
#include <86box/usb_next.h>
#include "usb_test_fakes.h"

/* ---- what usb_hid.c takes from the rest of the emulator ---- */
int usb_card_type = 1;
int keyboard_type;
int mouse_type;
int mouse_input_mode;
int debug_cmd_mouse;
int video_fullscreen;
int fullscreen_ui_visible;

static int fake_live = 1;
static int fake_dx, fake_dy, fake_dz, fake_dw, fake_b;
static int fake_caps, fake_num, fake_scroll, fake_kana;

int             mouse_live(void) { return fake_live; }
const device_t *mouse_get_device(int m) { (void) m; return NULL; }
int             mouse_get_buttons_ex(void) { return fake_b; }
void            mouse_set_buttons(int b) { (void) b; }
void            device_context(const device_t *d) { (void) d; }
void            device_context_restore(void) { }
int             device_get_config_int(const char *n) { (void) n; return 0; }

static int
take(int *v, int min, int max)
{
    int d = (*v < min) ? min : ((*v > max) ? max : *v);
    *v -= d;
    return d;
}
void
mouse_subtract_coords(int *dx, int *dy, int *ox, int *oy, int min, int max, int invert, int abs)
{
    (void) invert;
    (void) abs;
    *dx = take(&fake_dx, min, max);
    *dy = take(&fake_dy, min, max);
    *ox = *oy = 0;
}
void mouse_subtract_z(int *dz, int min, int max, int invert) { (void) invert; *dz = take(&fake_dz, min, max); }
void mouse_subtract_w(int *dw, int min, int max, int invert) { (void) invert; *dw = take(&fake_dw, min, max); }
void
keyboard_update_states(uint8_t cl, uint8_t nl, uint8_t sl, uint8_t kl)
{
    fake_caps   = cl;
    fake_num    = nl;
    fake_scroll = sl;
    fake_kana   = kl;
}

/* ---- the guest side ---- */
static usbn_device_t *d;

/* A control read: SETUP, IN packets of 8 until a short one, OUT status.
   Returns the bytes read, or -1 on a stall. */
static int
ctrl_in(uint8_t bm, uint8_t req, uint16_t wval, uint16_t widx, uint16_t wlen, uint8_t *out)
{
    uint8_t setup[8] = { bm, req, wval & 0xff, wval >> 8, widx & 0xff, widx >> 8, wlen & 0xff, wlen >> 8 };
    int     n        = 0;

    if (d->packet(d, USB_PID_SETUP, 0, setup, 8) != 8)
        return -1;
    for (;;) {
        int r = d->packet(d, USB_PID_IN, 0, out + n, 8);
        if (r < 0)
            return -1;
        n += r;
        if ((r < 8) || (n >= wlen))
            break;
    }
    return (d->packet(d, USB_PID_OUT, 0, NULL, 0) == 0) ? n : -1;
}

/* A control write (no data, or up to 8 bytes): returns 0, or -1 on a stall. */
static int
ctrl_out(uint8_t bm, uint8_t req, uint16_t wval, uint16_t widx, const uint8_t *data, int len)
{
    uint8_t setup[8] = { bm, req, wval & 0xff, wval >> 8, widx & 0xff, widx >> 8, len & 0xff, len >> 8 };
    uint8_t buf[8];

    if (d->packet(d, USB_PID_SETUP, 0, setup, 8) != 8)
        return -1;
    if (len) {
        memcpy(buf, data, len);
        if (d->packet(d, USB_PID_OUT, 0, buf, len) != len)
            return -1;
    }
    return (d->packet(d, USB_PID_IN, 0, buf, 0) == 0) ? 0 : -1;
}

static int
intr(uint8_t *buf)
{
    memset(buf, 0xee, 8);
    return d->packet(d, USB_PID_IN, 1, buf, 8);
}

/* Frames as the controller runs them: the device's 1 ms tick. */
static void
frames(int n)
{
    while (n--)
        d->frame(d);
}

static void
enumerate(void)
{
    uint8_t buf[256];

    d->reset(d);
    ctrl_in(0x80, 0x06, 0x0100, 0, 8, buf);
    ctrl_out(0x00, 0x05, 5, 0, NULL, 0);
    ctrl_in(0x80, 0x06, 0x0200, 0, 255, buf);
    ctrl_out(0x00, 0x09, 1, 0, NULL, 0);
}

static void
test_keyboard_descriptors(void)
{
    uint8_t buf[256];
    int     n;

    d = usbn_hid_create_keyboard(KBD_101_KEY);
    CHECK(!strcmp(d->name, "86Box USB Keyboard"), "name '%s'", d->name);
    CHECK(d->speed == USBN_SPEED_LOW, "low speed");
    d->reset(d);

    /* Windows asks for 8 bytes of the device descriptor first. */
    n = ctrl_in(0x80, 0x06, 0x0100, 0, 8, buf);
    CHECK((n == 8) && (buf[0] == 18) && (buf[1] == 1) && (buf[7] == 8), "device descriptor's first 8 bytes (n=%d)", n);
    CHECK(ctrl_out(0x00, 0x05, 7, 0, NULL, 0) == 0, "SET_ADDRESS");
    CHECK(d->addr == 7, "address 7 after the status stage (%d)", d->addr);

    n = ctrl_in(0x80, 0x06, 0x0100, 0, 18, buf);
    CHECK((n == 18) && (buf[8] == 0xb0) && (buf[9] == 0x86) && (buf[10] == 1) && (buf[4] == 0) && (buf[17] == 1),
          "device descriptor: 86B0:0001, class in the interface, one configuration");
    n = ctrl_in(0x80, 0x06, 0x0200, 0, 255, buf);
    CHECK((n == 34) && (buf[4] == 1), "configuration: 34 bytes, one interface (%d)", n);
    CHECK((buf[9 + 5] == 3) && (buf[9 + 6] == 1) && (buf[9 + 7] == 1), "a HID boot keyboard");
    CHECK(buf[25 + 4] == 33, "ANSI: country code 33 (US)");
    CHECK((buf[18 + 2] == 0x81) && (buf[18 + 3] == 3), "interrupt IN endpoint 81");
    CHECK((buf[18 + 1] == 0x05) && (buf[25 + 1] == 0x21), "the endpoint right after the interface, then the HID descriptor (AMI BIOSes need it so)");

    const int rlen = buf[25 + 7] | (buf[25 + 8] << 8);
    n = ctrl_in(0x81, 0x06, 0x2200, 0, 255, buf);
    CHECK((n == rlen) && (buf[3] == 0x06) && (buf[n - 1] == 0xc0), "report descriptor (%d of %d)", n, rlen);
    n = ctrl_in(0x81, 0x06, 0x2100, 0, 9, buf);
    CHECK((n == 9) && (buf[1] == 0x21), "the HID descriptor");
    n = ctrl_in(0x80, 0x06, 0x0302, 0x0409, 255, buf);
    CHECK((n == 2 + 2 * 18) && (buf[2] == '8') && (buf[4] == '6'), "product string (%d)", n);
    CHECK(ctrl_in(0x80, 0x06, 0x0f00, 0, 255, buf) < 0, "an unknown descriptor stalls");
    CHECK(ctrl_in(0x81, 0x06, 0x2200, 1, 255, buf) < 0, "no interface 1");

    CHECK(intr(buf) == USBN_STALL, "endpoint 1 before SET_CONFIGURATION");
    CHECK(ctrl_out(0x00, 0x09, 2, 0, NULL, 0) < 0, "no configuration 2");
    CHECK(ctrl_out(0x00, 0x09, 1, 0, NULL, 0) == 0, "SET_CONFIGURATION 1");
    n = ctrl_in(0x80, 0x08, 0, 0, 1, buf);
    CHECK((n == 1) && (buf[0] == 1), "GET_CONFIGURATION");
    CHECK(d->packet(d, USB_PID_IN, 2, buf, 8) == USBN_STALL, "no endpoint 2");
    n = ctrl_in(0x80, 0x00, 0, 0, 2, buf);
    CHECK((n == 2) && !(buf[0] & 0x02), "GET_STATUS: remote wakeup off");
    CHECK(ctrl_out(0x00, 0x03, 1, 0, NULL, 0) == 0, "SET_FEATURE(DEVICE_REMOTE_WAKEUP)");
    n = ctrl_in(0x80, 0x00, 0, 0, 2, buf);
    CHECK((n == 2) && (buf[0] & 0x02), "GET_STATUS: remote wakeup on");
    CHECK(ctrl_out(0x00, 0x01, 1, 0, NULL, 0) == 0, "CLEAR_FEATURE(DEVICE_REMOTE_WAKEUP)");
    n = ctrl_in(0x80, 0x00, 0, 0, 2, buf);
    CHECK((n == 2) && !(buf[0] & 0x02), "GET_STATUS: off again");
    CHECK(ctrl_out(0x00, 0x03, 2, 0, NULL, 0) < 0, "SET_FEATURE(TEST_MODE) stalls on a full-speed device");
    d->destroy(d);
}

static void
test_keyboard(void)
{
    uint8_t buf[8];
    int     n;

    d = usbn_hid_create_keyboard(KBD_101_KEY);
    enumerate();
    CHECK(!usbn_hid_keyboard_active(), "configured but not polled: not driven yet");
    usbn_hid_key(1, 0x1e);   /* dropped: the guest does not drive the keyboard yet */

    CHECK(ctrl_out(0x21, 0x0a, 0, 0, NULL, 0) == 0, "SET_IDLE 0, as Windows does");
    CHECK(intr(buf) == USBN_NAK, "nothing to report");
    frames(1);
    CHECK(usbn_hid_keyboard_active(), "polled: the keys reach the USB keyboard");

    usbn_hid_key(1, 0x1e);   /* A */
    n = intr(buf);
    CHECK((n == 8) && (buf[0] == 0) && (buf[2] == 0x04) && (buf[3] == 0), "A down (n=%d, %02X %02X)", n, buf[0], buf[2]);
    CHECK(intr(buf) == USBN_NAK, "no change, no report");
    usbn_hid_key(0, 0x1e);
    n = intr(buf);
    CHECK((n == 8) && (buf[2] == 0), "A up");

    /* Down and up between two polls: still seen down once. */
    usbn_hid_key(1, 0x30);   /* B */
    usbn_hid_key(0, 0x30);
    n = intr(buf);
    CHECK((n == 8) && (buf[2] == 0x05), "B tapped: down");
    n = intr(buf);
    CHECK((n == 8) && (buf[2] == 0), "B tapped: up");

    usbn_hid_key(1, 0x02a);  /* Left Shift */
    usbn_hid_key(1, 0x148);  /* Up (E0 48) */
    intr(buf);
    n = intr(buf);
    CHECK((n == 8) && (buf[0] == 0x02) && (buf[2] == 0x52), "Shift+Up: modifier 02, usage 52 (%02X %02X)", buf[0], buf[2]);
    usbn_hid_key(0, 0x148);
    usbn_hid_key(0, 0x02a);
    intr(buf);
    intr(buf);

    for (int k = 0x10; k < 0x17; k++)   /* Q W E R T Y U: seven keys */
        usbn_hid_key(1, k);
    for (int i = 0; i < 7; i++)
        n = intr(buf);
    CHECK((n == 8) && (buf[2] == 0x01) && (buf[7] == 0x01), "seven keys down: ErrorRollOver");
    for (int k = 0x10; k < 0x17; k++)
        usbn_hid_key(0, k);
    for (int i = 0; i < 7; i++)
        intr(buf);
    CHECK(intr(buf) == USBN_NAK, "all up again");

    usbn_hid_key(1, 0x056);  /* the ISO key: not on an ANSI keyboard */
    usbn_hid_key(1, 0x07d);  /* Yen: JIS only */
    CHECK(intr(buf) == USBN_NAK, "ANSI: no ISO key, no JIS key");

    uint8_t leds = 0x02 | 0x01;   /* Caps Lock and Num Lock */
    CHECK(ctrl_out(0x21, 0x09, 0x0200, 0, &leds, 1) == 0, "SET_REPORT: LEDs");
    CHECK(fake_caps && fake_num && !fake_scroll, "the LEDs reach the keyboard's lock states");

    CHECK(ctrl_out(0x21, 0x0a, 1 << 8, 0, NULL, 0) == 0, "SET_IDLE 4 ms");
    intr(buf);
    frames(4);
    CHECK(intr(buf) == 8, "the idle rate repeats an unchanged report");

    /* The guest stops polling: the keys are dropped, nothing stays held. */
    usbn_hid_key(1, 0x1e);
    frames(500);
    CHECK(!usbn_hid_keyboard_active(), "no poll for 500 ms: not driven any more");
    CHECK(ctrl_out(0x21, 0x0a, 0, 0, NULL, 0) == 0, "SET_IDLE 0");
    n = intr(buf);
    CHECK(n == USBN_NAK, "polled again: the A held before is gone (%d)", n);

    d->reset(d);
    CHECK(!usbn_hid_keyboard_active(), "a port reset: not driven");
    CHECK(intr(buf) == USBN_STALL, "and unconfigures");
    d->destroy(d);
}

/* Each layout: its country code and the keys it has beyond the 101. */
static void
test_layouts(void)
{
    static const struct {
        int         layout;
        uint8_t     country;
        uint16_t    key;      /* a key of this layout */
        uint8_t     usage;
        uint16_t    foreign;  /* a key it has not */
        const char *name;
    } t[] = {
        { KBD_102_KEY, 13, 0x056, 0x64, 0x07d, "ISO" },
        { KBD_JIS,     15, 0x07d, 0x89, 0x056, "JIS" },
        { KBD_KSC,     16, 0x0f2, 0x90, 0x073, "KSC" },
        { KBD_ABNT2,    0, 0x073, 0x87, 0x0f1, "ABNT2" },
    };
    uint8_t buf[64];

    for (int i = 0; i < 4; i++) {
        d = usbn_hid_create_keyboard(t[i].layout);
        enumerate();
        ctrl_in(0x80, 0x06, 0x0200, 0, 255, buf);
        CHECK(buf[25 + 4] == t[i].country, "%s: country code %d (%d)", t[i].name, t[i].country, buf[25 + 4]);
        ctrl_out(0x21, 0x0a, 0, 0, NULL, 0);
        intr(buf);
        frames(1);
        usbn_hid_key(1, t[i].foreign);
        usbn_hid_key(1, t[i].key);
        int n = intr(buf);
        CHECK((n == 8) && (buf[2] == t[i].usage) && (buf[3] == 0), "%s: its own key, not the foreign one (%02X %02X)",
              t[i].name, buf[2], buf[3]);
        d->destroy(d);
    }
}

static void
test_mouse(void)
{
    uint8_t buf[64];
    int     n;

    d = usbn_hid_create_mouse(4);
    CHECK(!strcmp(d->name, "86Box USB Mouse"), "name '%s'", d->name);
    enumerate();
    n = ctrl_in(0x80, 0x06, 0x0100, 0, 18, buf);
    CHECK((n == 18) && (buf[10] == 2), "a device of its own: 86B0:0002");
    n = ctrl_in(0x80, 0x06, 0x0200, 0, 255, buf);
    CHECK((n == 34) && (buf[9 + 2] == 0) && (buf[9 + 7] == 2) && (buf[18 + 2] == 0x81), "a HID boot mouse, interface 0, endpoint 81");

    CHECK(intr(buf) == USBN_NAK, "mouse: nothing to report");
    frames(1);
    CHECK(usbn_hid_mouse_active(), "polled: the mouse is driven");

    fake_dx = 5, fake_dy = -3, fake_dz = 1, fake_b = 1;
    n = intr(buf);
    CHECK((n == 4) && (buf[0] == 1) && (buf[1] == 5) && (buf[2] == 0xfd) && (buf[3] == 1), "wheel mouse: 4 bytes");
    CHECK(intr(buf) == USBN_NAK, "no change");
    fake_dx = 300;
    n = intr(buf);
    CHECK((n == 4) && (buf[1] == 127), "a big move is split");
    n = intr(buf);
    CHECK((n == 4) && (buf[1] == 127), "and goes on");
    intr(buf);

    fake_b = 0x1f;
    n = intr(buf);
    CHECK((n == 4) && (buf[0] == 0x07), "three buttons reported, not five");
    CHECK(ctrl_out(0x21, 0x0b, 0, 0, NULL, 0) == 0, "SET_PROTOCOL boot");
    CHECK(ctrl_out(0x22, 0x0b, 1, 0x81, NULL, 0) == 0, "SET_PROTOCOL to the endpoint, as AMI's BIOSes send it");
    CHECK(ctrl_out(0x22, 0x0b, 0, 0x01, NULL, 0) == 0, "and with the endpoint's number alone");
    CHECK(ctrl_out(0x22, 0x0b, 0, 0x02, NULL, 0) < 0, "another endpoint stalls");
    CHECK(ctrl_out(0x22, 0x0a, 0, 0x00, NULL, 0) == 0, "SET_IDLE to endpoint 0, as Windows 98 SE sends it");
    CHECK(ctrl_in(0x82, 0x06, 0x2200, 1, 255, buf) > 0, "and the report descriptor asked of the endpoint");
    fake_dx = 1;
    n = intr(buf);
    CHECK((n == 3) && (buf[0] == 0x07), "boot protocol: 3 bytes");
    n = ctrl_in(0xa1, 0x03, 0, 0, 1, buf);
    CHECK((n == 1) && (buf[0] == 0), "GET_PROTOCOL");

    fake_live = 0;
    fake_dx   = 10;
    CHECK(intr(buf) == USBN_NAK, "the host's mouse not captured: nothing taken");
    CHECK(fake_dx == 10, "and the motion left alone");
    fake_live = 1;
    fake_dx   = 0;

    CHECK(ctrl_out(0x02, 0x03, 0, 0x81, NULL, 0) == 0, "SET_FEATURE(HALT) on 81");
    CHECK(intr(buf) == USBN_STALL, "halted");
    n = ctrl_in(0x82, 0x00, 0, 0x81, 2, buf);
    CHECK((n == 2) && (buf[0] == 1), "GET_STATUS says halted");
    CHECK(ctrl_out(0x02, 0x01, 0, 0x81, NULL, 0) == 0, "CLEAR_FEATURE(HALT)");
    CHECK(intr(buf) != USBN_STALL, "running again");
    fake_b = 0;
    d->destroy(d);
    CHECK(!usbn_hid_mouse_active(), "unplugged: not driven");
}

/* Each button setting: its report descriptor and its report. */
static void
test_mouse_buttons(void)
{
    static const struct { int buttons, nb, len; } t[] = { { 2, 2, 3 }, { 3, 3, 3 }, { 4, 3, 4 }, { 5, 5, 4 }, { 6, 5, 5 } };
    uint8_t buf[128];

    for (int i = 0; i < 5; i++) {
        d = usbn_hid_create_mouse(t[i].buttons);
        enumerate();
        int n = ctrl_in(0x81, 0x06, 0x2200, 0, 255, buf);
        int max = 0, wheel = 0, pan = 0;
        for (int k = 0; k + 1 < n; k++) {
            if ((buf[k] == 0x29) && (k >= 4) && (buf[k - 4] == 0x05) && (buf[k - 3] == 0x09))   /* Buttons' Usage Maximum */
                max = buf[k + 1];
            if ((buf[k] == 0x09) && (buf[k + 1] == 0x38))
                wheel = 1;
            if ((buf[k] == 0x0a) && (buf[k + 1] == 0x38) && (buf[k + 2] == 0x02))
                pan = 1;
        }
        CHECK((max == t[i].nb) && (wheel == (t[i].buttons >= 4)) && (pan == (t[i].buttons >= 6)),
              "buttons %d: %d buttons, wheel %d, pan %d", t[i].buttons, max, wheel, pan);
        intr(buf);
        frames(1);
        fake_dx = 1, fake_dz = 1, fake_dw = 1, fake_b = 0x1f;
        n = intr(buf);
        CHECK((n == t[i].len) && (buf[0] == ((1 << t[i].nb) - 1)), "buttons %d: a %d-byte report, buttons %02X",
              t[i].buttons, n, buf[0]);
        fake_dx = fake_dz = fake_dw = fake_b = 0;
        d->destroy(d);
    }
}

int
main(void)
{
    test_keyboard_descriptors();
    test_keyboard();
    test_layouts();
    test_mouse();
    test_mouse_buttons();

    printf("%d checks, %d failures\n", fake_checks, fake_failures);
    return fake_failures != 0;
}
