/*
 * 86Box-Next  Tests for the USB 2.0 hub (src/usb/usb_hub2.c) and the EHCI's
 *             split transactions (src/usb/usb_ehci.c), on the VIA USB 2.0
 *             card: the hub at full speed while the companion owns its port,
 *             at high speed once the guest's EHCI driver takes it; a full-speed
 *             device behind its TT reached by a split queue head and by an
 *             siTD, a high-speed one behind it at high speed.
 *
 *             No test framework: build with CTest (BUILD_TESTING=ON).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/usb_next.h>
#include "usb_test_fakes.h"

extern const device_t usb_ehci_via_device;
extern const device_t usb_hub2_4port_device;

static fake_dev_t     fs, hs;
static usbn_device_t *d;   /* the hub as our bus sees it */

#define OP          0x20
#define USBCMD      (OP + 0x00)
#define USBSTS      (OP + 0x04)
#define PERIODIC    (OP + 0x14)
#define ASYNC       (OP + 0x18)
#define CONFIGFLAG  (OP + 0x40)
#define PORTSC(p)   (OP + 0x44 + 4 * (p))

#define FL    0x30000
#define QH    0x31000
#define QTD   0x31100
#define SITD  0x31400
#define BUF   0x32000

static int
ctrl_in(uint8_t bm, uint8_t req, uint16_t wval, uint16_t widx, uint16_t wlen, uint8_t *out)
{
    uint8_t setup[8] = { bm, req, wval & 0xff, wval >> 8, widx & 0xff, widx >> 8, wlen & 0xff, wlen >> 8 };
    int     n        = 0;

    if (d->packet(d, USB_PID_SETUP, 0, setup, 8) != 8)
        return -1;
    for (;;) {
        int r = d->packet(d, USB_PID_IN, 0, out + n, 64);
        if (r < 0)
            return -1;
        n += r;
        if ((r < 64) || (n >= wlen))
            break;
    }
    return (d->packet(d, USB_PID_OUT, 0, NULL, 0) == 0) ? n : -1;
}

static int
ctrl_out(uint8_t bm, uint8_t req, uint16_t wval, uint16_t widx)
{
    uint8_t setup[8] = { bm, req, wval & 0xff, wval >> 8, widx & 0xff, widx >> 8, 0, 0 };
    uint8_t buf[8];

    if (d->packet(d, USB_PID_SETUP, 0, setup, 8) != 8)
        return -1;
    return (d->packet(d, USB_PID_IN, 0, buf, 0) == 0) ? 0 : -1;
}

static uint32_t
port_status(int port)
{
    uint8_t b[4] = { 0 };

    if (ctrl_in(0xa3, 0x00, 0, port, 4, b) != 4)
        return 0xffffffff;
    return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t) b[3] << 24);
}

static void
qtd(uint32_t at, uint32_t next, int pid, int bytes, int ioc, uint32_t buf)
{
    wr32m(at, next);
    wr32m(at + 4, 1);
    wr32m(at + 8, 0x80 | (pid << 8) | (3 << 10) | (ioc ? 0x8000 : 0) | ((uint32_t) bytes << 16));
    wr32m(at + 0x0c, buf);
    for (int i = 1; i < 5; i++)
        wr32m(at + 0x0c + i * 4, (buf & ~0xfff) + i * 0x1000);
}

/* A queue head alone in the async ring; eps 0 full, 2 high speed; a full-
   speed one names the hub (address hub_addr) and its port for the split. */
static void
qh(uint8_t addr, int eps, int maxp, int hub_addr, int hub_port, uint32_t first)
{
    wr32m(QH, QH | 2);
    wr32m(QH + 4, addr | ((uint32_t) eps << 12) | (1 << 14) | (1 << 15) | ((uint32_t) maxp << 16) |
                      ((eps != 2) ? (1u << 27) : 0));   /* C: a full-speed control endpoint */
    wr32m(QH + 8, 0x40000000 | ((uint32_t) hub_addr << 16) | ((uint32_t) hub_port << 23));
    wr32m(QH + 0x0c, 0);
    wr32m(QH + 0x10, first);
    wr32m(QH + 0x14, 1);
    wr32m(QH + 0x18, 0);
}

static void
get_descriptor(uint8_t addr, int eps, int maxp, int hub_port)
{
    static const uint8_t setup[8] = { 0x80, 6, 0, 1, 0, 0, 18, 0 };

    memcpy(ram + BUF, setup, 8);
    memset(ram + BUF + 0x100, 0, 64);
    qtd(QTD + 0x00, QTD + 0x20, 2, 8, 0, BUF);
    qtd(QTD + 0x20, QTD + 0x40, 1, 18, 0, BUF + 0x100);
    qtd(QTD + 0x40, 1, 0, 0, 1, 0);
    qh(addr, eps, maxp, 1, hub_port, QTD);
    fake_mmio_wl(ASYNC, QH);
    fake_mmio_wl(USBCMD, 0x00080000 | 0x20 | 1);
    fake_frames(3);
}

int
main(void)
{
    uint8_t buf[64];
    int     n;

    fake_dev_init(&fs, "full-speed", USBN_SPEED_FULL);
    fake_dev_init(&hs, "high-speed", USBN_SPEED_HIGH);
    void *card = usb_ehci_via_device.init(&usb_ehci_via_device);
    fake_pci_wr(1, 0x13, 1, 0xfe, fake_pci_priv);
    fake_pci_wr(1, 0x04, 1, 0x06, fake_pci_priv);
    void *hub = usb_hub2_4port_device.init(&usb_hub2_4port_device);
    fake_frames(1);
    d = usbn_port_device(0);
    CHECK(d && (d->speed == USBN_SPEED_HIGH) && d->route, "a high-speed hub on port 1");
    if (!d)
        return 1;

    /* The companion owns the port: a full-speed hub. */
    CHECK(d->fs_view, "on the companion it is seen at full speed");
    d->reset(d);
    n = ctrl_in(0x80, 0x06, 0x0100, 0, 18, buf);
    CHECK((n == 18) && (buf[2] == 0x00) && (buf[3] == 0x02) && (buf[6] == 0), "full speed: bcdUSB 2.00, bDeviceProtocol 0 (%02X)", buf[6]);

    /* The EHCI driver takes the port: high speed. */
    fake_mmio_wl(CONFIGFLAG, 1);
    fake_mmio_wl(PORTSC(0), 0x1000 | 0x100 | 2);
    fake_mmio_wl(PORTSC(0), 0x1000);
    CHECK((fake_mmio_rl(PORTSC(0)) & 5) == 5, "EHCI port 1 enabled (%08X)", fake_mmio_rl(PORTSC(0)));
    CHECK(!d->fs_view, "at high speed");
    n = ctrl_in(0x80, 0x06, 0x0100, 0, 18, buf);
    CHECK((n == 18) && (buf[6] == 1), "high speed: bDeviceProtocol 1, one TT (%02X)", buf[6]);
    n = ctrl_in(0x80, 0x06, 0x0200, 0, 25, buf);
    CHECK((n == 25) && (buf[24] == 12), "status change endpoint polled at bInterval 12 (%d)", buf[24]);
    CHECK(ctrl_out(0x00, 0x05, 1, 0) == 0, "SET_ADDRESS 1");
    CHECK(ctrl_out(0x00, 0x09, 1, 0) == 0, "SET_CONFIGURATION 1");

    /* A full-speed device on hub port 1, a high-speed one on hub port 2. */
    CHECK(usbn_attach(2, &fs.dev) && usbn_attach(3, &hs.dev), "attach to hub ports 1 and 2");
    fake_frames(1);
    for (int p = 1; p <= 2; p++) {
        ctrl_out(0x23, 0x03, 8, p);    /* PORT_POWER */
        fake_frames(2);
        ctrl_out(0x23, 0x03, 4, p);    /* PORT_RESET */
        fake_frames(2);
    }
    uint32_t s1 = port_status(1), s2 = port_status(2);
    CHECK((s1 & 0x0003) == 0x0003 && !(s1 & 0x0600), "port 1: enabled, full speed (%08X)", s1);
    CHECK((s2 & 0x0403) == 0x0403, "port 2: enabled, PORT_HIGH_SPEED (%08X)", s2);
    CHECK(!hs.dev.fs_view && !fs.dev.fs_view, "the high-speed device runs at high speed behind it");
    CHECK(ctrl_out(0x23, 0x08, 0x0000, 1) == 0, "CLEAR_TT_BUFFER accepted");
    CHECK(ctrl_out(0x23, 0x09, 0, 1) == 0, "RESET_TT accepted");

    /* A split control transfer to the full-speed device at address 0. */
    hs.dev.addr = 7;
    get_descriptor(0, 0, 8, 1);
    CHECK(!(rd32m(QTD + 0x28) & 0x80) && !(rd32m(QTD + 0x28) & 0x40), "split IN done (%08X)", rd32m(QTD + 0x28));
    CHECK(!memcmp(ram + BUF + 0x100, fake_dev_desc, 18), "the full-speed device's descriptor, through the TT");
    CHECK(!(rd32m(QTD + 0x48) & 0x80), "split status stage done");

    /* High speed behind the hub: no split. */
    get_descriptor(7, 2, 64, 0);
    CHECK(!memcmp(ram + BUF + 0x100, fake_dev_desc, 18) && !(rd32m(QTD + 0x28) & 0xc0), "the high-speed device behind the hub");

    /* Nobody at 9: the split fails as a transaction error. */
    get_descriptor(9, 0, 8, 1);
    CHECK(rd32m(QH + 0x18) & 0x48, "no device behind the TT: halted, transaction error (%08X)", rd32m(QH + 0x18));
    fake_mmio_wl(USBCMD, 0x00080000);

    /* An siTD: 16 bytes of full-speed isochronous OUT, then 4 bytes IN. */
    fs.dev.addr = 3;
    for (int i = 0; i < 1024; i++)
        wr32m(FL + i * 4, SITD | 4);   /* type siTD */
    wr32m(SITD, 1);
    wr32m(SITD + 4, 3 | (5 << 8) | (1u << 16) | (1u << 24));   /* OUT, addr 3, ep 5, hub 1, port 1 */
    wr32m(SITD + 8, 0x01);
    wr32m(SITD + 0x0c, 0x80000000 | (16u << 16) | 0x80);       /* IOC, 16 bytes, active */
    wr32m(SITD + 0x10, BUF + 0x400);
    wr32m(SITD + 0x14, BUF + 0x1000);
    ram[BUF + 0x400] = 0x5a;
    fs.iso_out_len = 0;
    fake_mmio_wl(PERIODIC, FL);
    fake_mmio_wl(USBCMD, 0x00080000 | 0x10 | 1);
    fake_frames(1);
    uint32_t st = rd32m(SITD + 0x0c);
    CHECK((fs.iso_out_len == 16) && (fs.iso_out_first == 0x5a), "siTD OUT: the device got 16 bytes (%d)", fs.iso_out_len);
    CHECK(!(st & 0x80) && !((st >> 16) & 0x3ff), "siTD done, nothing left (%08X)", st);
    CHECK(fake_mmio_rl(USBSTS) & 1, "IOC: USBINT");
    fake_mmio_wl(USBSTS, 0x3f);

    wr32m(SITD + 4, 0x80000000 | 3 | (5 << 8) | (1u << 16) | (1u << 24));   /* IN */
    wr32m(SITD + 0x0c, (192u << 16) | 0x80);
    memset(ram + BUF + 0x400, 0, 8);
    fake_frames(1);
    st = rd32m(SITD + 0x0c);
    CHECK(!(st & 0x80) && (((st >> 16) & 0x3ff) == 188), "siTD IN: 4 of 192 bytes, 188 left (%08X)", st);
    CHECK((ram[BUF + 0x400] == 1) && (ram[BUF + 0x403] == 4), "the data in memory");

    usb_hub2_4port_device.close(hub);
    usb_ehci_via_device.close(card);
    printf("USB 2.0 hub: %d checks, %d failed\n", fake_checks, fake_failures);
    return fake_failures ? 1 : 0;
}
