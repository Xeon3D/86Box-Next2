/*
 * 86Box-Next  Tests for the UHCI controller (src/usb/usb_uhci.c).
 *
 *             The controller runs against fake guest memory, PCI, I/O, timer
 *             and mutex (usb_test_fakes.c), with a fake device on its port.
 *             The tests build real UHCI schedules -- frame list, queue head,
 *             transfer descriptors -- the way a guest driver would, step the
 *             1 ms frame by hand and check what the controller wrote back,
 *             against the UHCI Design Guide 1.1.
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

/* Built once per card: UHCI_INTEL selects the Intel PIIX4 USB, else VIA. */
extern const device_t usb_uhci_via_device;
extern const device_t usb_uhci_intel_device;
#ifdef UHCI_INTEL
#    define CARD      usb_uhci_intel_device
#    define VEN       0x8086
#    define DEV       0x7112
#    define CARD_NAME "Intel"
#else
#    define CARD      usb_uhci_via_device
#    define VEN       0x1106
#    define DEV       0x3038
#    define CARD_NAME "VIA"
#endif
/* The card list names the USB 2.0 cards too; this test does not link them. */
const device_t usb_ehci_via_device   = { .name = "unused", .internal_name = "usb_ehci_via" };
const device_t usb_ehci_intel_device = { .name = "unused", .internal_name = "usb_ehci_intel" };

static fake_dev_t fake;

#define rdw(r)    fake_io_rw(r)
#define wrw(r, v) fake_io_ww(r, v)

#define FL  0x10000
#define QH  0x11000
#define TD0 0x11100
#define BUF 0x12000

static void
td(uint32_t at, uint32_t link, uint32_t ctrl, uint8_t pid, uint8_t addr, uint8_t ep, int maxlen, uint32_t buf)
{
    wr32m(at, link);
    wr32m(at + 4, ctrl | 0x00800000 | 0x18000000);   /* active, C_ERR = 3 */
    wr32m(at + 8, pid | (addr << 8) | (ep << 15) | ((uint32_t) ((maxlen - 1) & 0x7ff) << 21));
    wr32m(at + 12, buf);
}

static void
schedule(uint32_t first_td)
{
    for (int i = 0; i < 1024; i++)
        wr32m(FL + i * 4, QH | 2);
    wr32m(QH, 1);
    wr32m(QH + 4, first_td);
}

static void
test_pci_and_registers(void)
{
    uint16_t ven = fake_pci_rd(0, 0, 1, fake_pci_priv) | (fake_pci_rd(0, 1, 1, fake_pci_priv) << 8);
    uint16_t dev = fake_pci_rd(0, 2, 1, fake_pci_priv) | (fake_pci_rd(0, 3, 1, fake_pci_priv) << 8);
    CHECK(ven == VEN && dev == DEV, "%s identity %04X:%04X", CARD_NAME, ven, dev);
    CHECK(fake_pci_rd(0, 0x0b, 1, fake_pci_priv) == 0x0c && fake_pci_rd(0, 0x0a, 1, fake_pci_priv) == 0x03
              && fake_pci_rd(0, 0x09, 1, fake_pci_priv) == 0x00,
          "class is serial bus / USB / UHCI");
    fake_pci_wr(0, 0x20, 1, 0x00, fake_pci_priv);
    fake_pci_wr(0, 0x21, 1, 0xe0, fake_pci_priv);
    fake_pci_wr(0, 0x04, 1, 0x05, fake_pci_priv);
    CHECK(fake_io_base == 0xe000, "BAR4 maps the registers at E000 (got %04X)", fake_io_base);
    CHECK(rdw(0x02) & 0x20, "halted after reset");
    CHECK((rdw(0x10) & 0x80) && (rdw(0x12) & 0x80), "two ports, bit 7 reads 1");
    CHECK(!(rdw(0x14) & 0x80), "no third port");
    CHECK(!usbn_bus_high_speed(), "a USB 1.1 controller takes no high-speed devices");
}

static void
test_connect_and_enable(void)
{
    CHECK(usbn_attach(0, &fake.dev), "attach accepted");
    fake_frames(1);   /* plugs are applied at the frame */
    uint16_t p = rdw(0x10);
    CHECK((p & 0x0003) == 0x0003, "connect status and its change bit set (%04X)", p);
    CHECK(!(p & 0x0100), "full speed: no low-speed bit");
    wrw(0x10, 0x0002);
    CHECK(!(rdw(0x10) & 0x0002), "CSC is write-1-to-clear");
    wrw(0x10, 0x0004);
    wrw(0x10, 0x0204);
    CHECK(!(rdw(0x10) & 0x0004), "a port being reset is not enabled");
    wrw(0x10, 0x0000);
    CHECK(fake.resets == 1, "port reset reaches the device");
    wrw(0x10, 0x0004);
    CHECK(rdw(0x10) & 0x0004, "port enabled");
}

static void
test_control_transfer(void)
{
    /* GET_DESCRIPTOR(device, 18): SETUP, IN 8, IN 8, IN 2, status OUT. */
    static const uint8_t setup[8] = { 0x80, 6, 0, 1, 0, 0, 18, 0 };
    memcpy(ram + BUF, setup, 8);
    memset(ram + BUF + 0x100, 0, 32);
    td(TD0 + 0x00, (TD0 + 0x20) | 4, 0, USB_PID_SETUP, 0, 0, 8, BUF);
    td(TD0 + 0x20, (TD0 + 0x40) | 4, 0, USB_PID_IN, 0, 0, 8, BUF + 0x100);
    td(TD0 + 0x40, (TD0 + 0x60) | 4, 0, USB_PID_IN, 0, 0, 8, BUF + 0x108);
    td(TD0 + 0x60, (TD0 + 0x80) | 4, 0, USB_PID_IN, 0, 0, 8, BUF + 0x110);
    td(TD0 + 0x80, 1, 0x01000000, USB_PID_OUT, 0, 0, 0x800, 0);   /* zero length, IOC */
    schedule(TD0);

    wrw(0x04, 0x000f);
    wrw(0x08, FL & 0xffff);
    wrw(0x0a, FL >> 16);
    fake.naks = 2;
    wrw(0x00, 0x0001);
    CHECK(!(rdw(0x02) & 0x20), "running");

    fake_frames(1);
    CHECK(!(rd32m(TD0 + 4) & 0x00800000), "SETUP done in the first frame");
    CHECK(rd32m(TD0 + 0x24) & 0x00800000, "first IN still active after a NAK");
    CHECK(rd32m(TD0 + 0x24) & 0x00080000, "NAK bit set on it");
    CHECK(rd32m(QH + 4) == ((TD0 + 0x20) | 4), "queue element points at the NAKed TD");
    CHECK(!fake_irq[1], "no interrupt yet");

    fake_frames(3);
    CHECK(rd32m(QH + 4) & 1, "queue ran to its end (%08X)", rd32m(QH + 4));
    CHECK((rd32m(TD0 + 0x24) & 0x7ff) == 7, "first IN moved 8 bytes (ActLen 7)");
    CHECK((rd32m(TD0 + 0x64) & 0x7ff) == 1, "last IN moved 2 bytes (ActLen 1)");
    CHECK((rd32m(TD0 + 0x84) & 0x7ff) == 0x7ff, "status stage zero length (ActLen 7FF)");
    CHECK(!memcmp(ram + BUF + 0x100, fake_dev_desc, 18), "descriptor in guest memory");
    CHECK(rdw(0x02) & 0x0001, "USBINT from the IOC TD");
    CHECK(fake_irq[1], "INTA asserted");
    wrw(0x02, 0x0001);
    CHECK(!fake_irq[1], "INTA drops when USBINT is cleared");
    CHECK(rdw(0x06) == 4, "frame number advanced to 4 (%d)", rdw(0x06));
}

static void
test_stall(void)
{
    fake.dev.addr = 3;
    td(TD0, 1, 0x01000000, USB_PID_OUT, 3, 1, 4, BUF);
    schedule(TD0);
    fake_frames(1);
    uint32_t c = rd32m(TD0 + 4);
    CHECK(!(c & 0x00800000) && (c & 0x00400000), "STALL: inactive with the stalled bit (%08X)", c);
    CHECK(rdw(0x02) & 0x0002, "error interrupt status");
    CHECK(rd32m(QH + 4) == TD0, "queue stops at the stalled TD");
    fake_frames(2);
    CHECK(rd32m(QH + 4) == TD0, "and stays stopped on later frames until the driver deals with it");
    wrw(0x02, 0x0003);
}

static void
test_short_packet(void)
{
    td(TD0, (TD0 + 0x20) | 4, 0x20000000, USB_PID_IN, 3, 2, 8, BUF + 0x200);   /* SPD */
    td(TD0 + 0x20, 1, 0, USB_PID_IN, 3, 2, 8, BUF + 0x208);
    schedule(TD0);
    fake.in2_calls = 0;
    fake_frames(1);
    CHECK((rd32m(TD0 + 4) & 0x7ff) == 1 && !(rd32m(TD0 + 4) & 0x00800000), "short packet: 2 bytes, done");
    CHECK(ram[BUF + 0x200] == 0xaa && ram[BUF + 0x201] == 0x55, "short packet data");
    CHECK(rd32m(QH + 4) == TD0, "short packet with SPD stops the queue");
    CHECK(rd32m(TD0 + 0x24) & 0x00800000, "the next TD is untouched");
    CHECK(rdw(0x02) & 0x0001, "short packet raises USBINT");
    /* What a USB stick's short reply looked like: the next TD of the
       abandoned data stage must not reach the device on any later frame
       either, or it reads the status reply as data and the real status read
       then waits forever. */
    fake_frames(3);
    CHECK(rd32m(QH + 4) == TD0, "the stopped queue is not moved on by later frames");
    CHECK(rd32m(TD0 + 0x24) & 0x00800000, "the abandoned TD is still untouched");
    CHECK(fake.in2_calls == 1, "only one IN reached the device (%d)", fake.in2_calls);
    wrw(0x02, 0x0003);
}

/* Isochronous TDs in the frame list: one transaction each, in their frame,
   never NAKed -- and a device that does not stream completes them empty. */
static void
test_iso(void)
{
    /* Every frame list entry: an IN and an OUT isochronous TD, then the QH. */
    for (int i = 0; i < 1024; i++)
        wr32m(FL + i * 4, TD0 + 0x40);
    wr32m(QH, 1);
    wr32m(QH + 4, 1);
    td(TD0 + 0x40, TD0 + 0x60, 0x02000000 | 0x01000000, USB_PID_IN, 3, 4, 192, BUF + 0x300);    /* ISO, IOC */
    td(TD0 + 0x60, QH | 2, 0x02000000, USB_PID_OUT, 3, 5, 16, BUF + 0x400);
    memset(ram + BUF + 0x300, 0, 8);
    ram[BUF + 0x400] = 0x5a;
    fake_frames(1);

    uint32_t c = rd32m(TD0 + 0x44);
    CHECK(!(c & 0x00800000) && ((c & 0x7ff) == 3), "iso IN done in its frame, 4 bytes (%08X)", c);
    CHECK(ram[BUF + 0x300] == 1 && ram[BUF + 0x303] == 4, "iso IN data in memory");
    c = rd32m(TD0 + 0x64);
    CHECK(!(c & 0x00800000) && ((c & 0x7ff) == 15), "iso OUT done, 16 bytes (%08X)", c);
    CHECK(fake.iso_out_len == 16 && fake.iso_out_first == 0x5a, "the device got the 16 bytes");
    CHECK(rdw(0x02) & 0x0001, "IOC on an iso TD raises USBINT");
    wrw(0x02, 0x0003);

    /* A device without isochronous support: completed empty, not hung. */
    fake.dev.iso = NULL;
    td(TD0 + 0x40, TD0 + 0x60, 0x02000000, USB_PID_IN, 3, 4, 192, BUF + 0x300);
    td(TD0 + 0x60, QH | 2, 0x02000000, USB_PID_OUT, 3, 5, 16, BUF + 0x400);
    fake_frames(1);
    c = rd32m(TD0 + 0x44);
    CHECK(!(c & 0x00800000) && ((c & 0x7ff) == 0x7ff), "no iso support: completed with nothing (%08X)", c);
    schedule(TD0);
}

static void
test_no_device(void)
{
    td(TD0, 1, 0, USB_PID_IN, 9, 1, 8, BUF);
    schedule(TD0);
    fake_frames(1);
    uint32_t c = rd32m(TD0 + 4);
    CHECK(!(c & 0x00800000) && (c & 0x00040000), "nobody at address 9: CRC/timeout (%08X)", c);
    wrw(0x02, 0x0003);
}

/* A bandwidth-reclamation loop (the QH linked to itself) with a device that
   NAKs: the controller keeps asking until the frame's bus time runs out at
   the preSOF point -- about a hundred full-speed NAKs -- then goes on to the
   next frame. */
static void
test_bus_time(void)
{
    td(TD0, 1, 0, USB_PID_IN, 3, 0, 8, BUF);
    schedule(TD0);
    wr32m(QH, QH | 2);
    fake.naks = 100000;
    uint16_t fr = rdw(0x06);
    fake_frames(1);
    int asked = 100000 - fake.naks;
    CHECK((asked > 50) && (asked < 200), "NAKs in one frame bounded by its bus time (%d)", asked);
    CHECK(rdw(0x06) == ((fr + 1) & 0x7ff), "and the frame ended");
    fake.naks = 0;
    wr32m(QH, 1);
    schedule(TD0);
    wrw(0x02, 0x0003);
}

/* Software debug mode: one TD, then Run/Stop clears and HCHalted sets; Run/
   Stop again goes on from the next TD in the same frame. */
static void
test_debug_mode(void)
{
    td(TD0, (TD0 + 0x20) | 4, 0, USB_PID_IN, 3, 2, 8, BUF + 0x200);
    td(TD0 + 0x20, 1, 0, USB_PID_IN, 3, 2, 8, BUF + 0x208);
    schedule(TD0);
    uint16_t fr = rdw(0x06);
    wrw(0x00, 0x0021);   /* SWDBG | RS */
    fake_frames(1);
    CHECK(!(rd32m(TD0 + 4) & 0x00800000), "debug: the first TD ran");
    CHECK(rd32m(TD0 + 0x24) & 0x00800000, "debug: the second waits");
    CHECK(!(rdw(0x00) & 1) && (rdw(0x02) & 0x20), "debug: stopped and halted after one TD");
    CHECK(rdw(0x06) == fr, "debug: the frame number stays");
    wrw(0x00, 0x0021);
    fake_frames(1);
    CHECK(!(rd32m(TD0 + 0x24) & 0x00800000), "debug: Run/Stop again runs the next TD");
    wrw(0x00, 0x0001);
    wrw(0x02, 0x0003);
}

/* Remote wakeup: from global suspend the controller sets Force Global
   Resume and Resume Detect; on a suspended port, the port's Resume Detect. */
static void
test_remote_wakeup(void)
{
    wrw(0x00, 0x0000);
    wrw(0x04, 0x0002);   /* the resume interrupt */
    wrw(0x00, 0x0008);   /* EGSM */
    usbn_remote_wakeup(&fake.dev);
    CHECK(rdw(0x00) & 0x0010, "remote wakeup in global suspend: FGR set");
    CHECK(rdw(0x02) & 0x0004, "Resume Detect status");
    CHECK(fake_irq[1], "with its interrupt enabled, an interrupt");
    wrw(0x00, 0x0000);
    wrw(0x02, 0x0004);
    wrw(0x04, 0x0000);

    wrw(0x10, 0x1004);   /* enabled, suspended */
    usbn_remote_wakeup(&fake.dev);
    CHECK(rdw(0x10) & 0x0040, "remote wakeup on a suspended port: its Resume Detect");
    wrw(0x10, 0x0004);
    wrw(0x00, 0x0001);
}

static void
test_disconnect(void)
{
    usbn_detach(0);
    fake_frames(1);
    uint16_t p = rdw(0x10);
    CHECK(!(p & 0x0001) && (p & 0x0002), "unplugged: no connection, change bit set (%04X)", p);
    CHECK(!(p & 0x0004) && (p & 0x0008), "port disabled with its change bit");
    CHECK(fake.destroyed == 1, "the device is released");
}

static void
test_global_reset(void)
{
    wrw(0x00, 0x0002);
    CHECK(rdw(0x02) & 0x20, "halted after HCRESET");
    CHECK(rdw(0x00) == 0, "command register cleared");
}

int
main(void)
{
    fake_dev_init(&fake, "fake", USBN_SPEED_FULL);
    void *dev = CARD.init(&CARD);

    test_pci_and_registers();
    test_connect_and_enable();
    test_control_transfer();
    test_stall();
    test_short_packet();
    test_iso();
    test_no_device();
    test_bus_time();
    test_debug_mode();
    test_remote_wakeup();
    test_disconnect();
    test_global_reset();
    CARD.close(dev);

    printf("UHCI (%s): %d checks, %d failed\n", CARD_NAME, fake_checks, fake_failures);
    return fake_failures ? 1 : 0;
}
