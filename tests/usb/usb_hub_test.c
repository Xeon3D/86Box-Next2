/*
 * 86Box-Next  Tests for upstream's 4-port hub (src/usb_hub.c, on its device
 *             core src/usb_dev.c) on our USB bus through the bridge
 *             (src/usb/usb_devbridge.c): the hub on a UHCI card's first port,
 *             enumerated packet by packet as a guest's hub driver does it,
 *             a fake device on the hub's first downstream port powered,
 *             reset and found through the hub's route.
 *
 *             No test framework: build with CTest (BUILD_TESTING=ON).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/mem.h>
#include <86box/usb.h>
#include <86box/usb_next.h>
#include "usb_test_fakes.h"

extern const device_t usb_uhci_via_device;
const device_t usb_ehci_via_device   = { .name = "unused", .internal_name = "usb_ehci_via" };
const device_t usb_ehci_intel_device = { .name = "unused", .internal_name = "usb_ehci_intel" };

static fake_dev_t     fake;
static usbn_device_t *d;   /* the hub as our bus sees it */

static int
ctrl_in(uint8_t bm, uint8_t req, uint16_t wval, uint16_t widx, uint16_t wlen, uint8_t *out)
{
    uint8_t setup[8] = { bm, req, wval & 0xff, wval >> 8, widx & 0xff, widx >> 8, wlen & 0xff, wlen >> 8 };
    int     n        = 0;

    if (d->packet(d, USB_PID_SETUP, 0, setup, 8) != 8)
        return -1;
    for (;;) {
        int r = d->packet(d, USB_PID_IN, 0, out + n, 64);   /* the hub's bMaxPacketSize0 */
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

int
main(void)
{
    uint8_t buf[64];
    int     n;

    fake_dev_init(&fake, "fake", USBN_SPEED_FULL);
    void *card = usb_uhci_via_device.init(&usb_uhci_via_device);
    snprintf(usbn_port_cfg[0], sizeof(usbn_port_cfg[0]), "hub");   /* Settings > Ports > USB */
    void *hub  = usb_hub_4port_device.init(&usb_hub_4port_device);

    CHECK(usbn_port_exists(USBN_HUB_BASE) && usbn_port_exists(USBN_PORTS - 1), "the hub's four ports are bus slots");
    CHECK(!usbn_port_exists(2), "a UHCI has two ports");
    CHECK(usbn_port_busy(0) && !usbn_port_busy(1), "the hub is on the port it is assigned");
    fake_frames(1);
    d = usbn_port_device(0);
    CHECK(d && (d->route != NULL), "on port 1 as a hub (%s)", d ? d->name : "nothing");
    if (!d)
        return 1;

    /* Enumeration. */
    d->reset(d);
    n = ctrl_in(0x80, 0x06, 0x0100, 0, 18, buf);
    CHECK((n == 18) && (buf[4] == 0x09), "device descriptor: hub class (%d bytes, class %02X)", n, buf[4]);
    CHECK(ctrl_out(0x00, 0x05, 1, 0) == 0, "SET_ADDRESS 1");
    CHECK(d->addr == 1, "the bus sees address 1 (%d)", d->addr);
    CHECK(ctrl_out(0x00, 0x09, 1, 0) == 0, "SET_CONFIGURATION 1");
    n = ctrl_in(0xa0, 0x06, 0x2900, 0, 9, buf);
    CHECK((n >= 7) && (buf[1] == 0x29) && (buf[2] == 4), "hub descriptor: 4 ports (%d bytes)", n);

    /* A device on the hub's first port: power, connect, reset. */
    CHECK(usbn_attach(USBN_HUB_BASE, &fake.dev), "attach to hub port 1");
    fake_frames(1);
    CHECK(usbn_port_device(USBN_HUB_BASE) == &fake.dev, "plugged into the hub");
    CHECK(!(port_status(1) & 0x0001), "no connection before port power");
    CHECK(ctrl_out(0x23, 0x03, 8, 1) == 0, "SetPortFeature(PORT_POWER)");
    fake_frames(2);
    uint32_t st = port_status(1);
    CHECK((st & 0x00010101) == 0x00010101, "powered, connected, C_PORT_CONNECTION (%08X)", st);
    CHECK(ctrl_out(0x23, 0x01, 16, 1) == 0, "ClearPortFeature(C_PORT_CONNECTION)");
    memset(buf, 0, 8);
    fake.resets = 0;
    CHECK(ctrl_out(0x23, 0x03, 4, 1) == 0, "SetPortFeature(PORT_RESET)");
    fake_frames(2);
    st = port_status(1);
    CHECK((st & 0x00100003) == 0x00100003, "reset done: enabled, C_PORT_RESET (%08X)", st);
    CHECK(fake.resets >= 1, "the reset reached the device");

    /* The controller finds it through the hub. */
    fake.dev.addr = 0;
    CHECK(usbn_route(d, 0, 0) == &fake.dev, "address 0 routes through the hub to the device");
    CHECK(usbn_route(d, 1, 0) == d, "address 1 is the hub itself");
    fake.dev.addr = 5;
    CHECK(usbn_route(d, 5, 0) == &fake.dev, "the device at its new address");
    CHECK(usbn_route(d, 5, 1) == NULL, "not as a low-speed device");
    n = d->packet(d, USB_PID_IN, 1, buf, 1);
    CHECK((n == 1) && (buf[0] & 0x02), "status change endpoint: port 1 (%d, %02X)", n, buf[0]);

    /* Unplugged: the hub sees the disconnect. */
    usbn_detach(USBN_HUB_BASE);
    fake_frames(1);
    st = port_status(1);
    CHECK(!(st & 0x0001) && (st & 0x00010000), "unplugged: no connection, C_PORT_CONNECTION (%08X)", st);
    CHECK(fake.destroyed == 1, "the device is released");

    usb_hub_4port_device.close(hub);
    usb_uhci_via_device.close(card);
    printf("USB hub: %d checks, %d failed\n", fake_checks, fake_failures);
    return fake_failures ? 1 : 0;
}
