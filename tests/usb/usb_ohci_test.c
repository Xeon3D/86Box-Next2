/*
 * 86Box-Next  Tests for the OHCI controller (src/usb/usb_ohci.c), on the OPTi
 *             FireLink card (src/usb/usb_opti861.c, through usb_chipset.c).
 *
 *             The controller runs against fake guest memory, PCI, the memory
 *             window, timers and mutexes (usb_test_fakes.c), with a fake
 *             device on its first port.  The tests build OpenHCI structures --
 *             HCCA, endpoint descriptors, general and isochronous TDs -- the
 *             way a guest driver would, step the frames by hand and check
 *             what the controller wrote back, against OpenHCI 1.0a.
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

extern const device_t usb_opti_82c861_device;
/* The card list (usb_uhci.c) names the USB 2.0 cards too; not linked here. */
const device_t usb_ehci_via_device   = { .name = "unused", .internal_name = "usb_ehci_via" };
const device_t usb_ehci_intel_device = { .name = "unused", .internal_name = "usb_ehci_intel" };

static fake_dev_t fake;

#define rd(r)    fake_mmio_rl(r)
#define wr(r, v) fake_mmio_wl(r, v)

#define HCCA  0x10000
#define ED0   0x11000
#define ED1   0x11010
#define ED2   0x11020
#define TD0   0x11100
#define ITD   0x11400
#define BUF   0x12000

#define CC(td) (rd32m(td) >> 28)

static void
ed(uint32_t at, int fa, int en, int dir, int mps, uint32_t flags, uint32_t head, uint32_t tail, uint32_t next)
{
    wr32m(at, fa | (en << 7) | (dir << 11) | flags | ((uint32_t) mps << 16));
    wr32m(at + 4, tail);
    wr32m(at + 8, head);
    wr32m(at + 12, next);
}

/* A general TD: dp 0 SETUP / 1 OUT / 2 IN, toggle -1 from the ED, else
   that DATAx; di the DelayInterrupt. */
static void
gtd(uint32_t at, int dp, int toggle, int rounding, int di, uint32_t buf, int len, uint32_t next)
{
    uint32_t w = 0xf0000000 | ((uint32_t) dp << 19) | ((uint32_t) di << 21) | (rounding ? 0x00040000 : 0);

    if (toggle >= 0)
        w |= 0x02000000 | ((uint32_t) toggle << 24);
    wr32m(at, w);
    wr32m(at + 4, len ? buf : 0);
    wr32m(at + 8, next);
    wr32m(at + 12, len ? (buf + len - 1) : 0);
}

static void
test_pci_and_registers(void)
{
    uint16_t ven = fake_pci_rd(0, 0, 1, fake_pci_priv) | (fake_pci_rd(0, 1, 1, fake_pci_priv) << 8);
    uint16_t dev = fake_pci_rd(0, 2, 1, fake_pci_priv) | (fake_pci_rd(0, 3, 1, fake_pci_priv) << 8);

    CHECK((ven == 0x1045) && (dev == 0xc861), "OPTi 82C861 identity %04X:%04X", ven, dev);
    CHECK((fake_pci_rd(0, 0x0b, 1, fake_pci_priv) == 0x0c) && (fake_pci_rd(0, 0x0a, 1, fake_pci_priv) == 0x03)
              && (fake_pci_rd(0, 0x09, 1, fake_pci_priv) == 0x10),
          "class is serial bus / USB / OHCI");
    fake_pci_wr(0, 0x11, 1, 0x00, fake_pci_priv);
    fake_pci_wr(0, 0x12, 1, 0xbf, fake_pci_priv);
    fake_pci_wr(0, 0x13, 1, 0xfe, fake_pci_priv);
    fake_pci_wr(0, 0x04, 1, 0x06, fake_pci_priv);   /* memory, bus master */
    CHECK(fake_mmio_base == 0xfebf0000, "BAR0 maps the registers at FEBF0000 (got %08X)", fake_mmio_base);
    CHECK(rd(0x00) == 0x110, "HcRevision 1.0 with legacy support (%08X)", rd(0x00));
    CHECK((rd(0x04) & 0xc0) == 0x00, "USBRESET after reset");
    CHECK((rd(0x48) & 0xff) == 2, "two downstream ports (%08X)", rd(0x48));
    CHECK(rd(0x34) == 0x2edf, "FrameInterval nominal (%08X)", rd(0x34));
    CHECK(!usbn_bus_high_speed(), "a USB 1.1 controller");
}

static void
test_power_connect_reset(void)
{
    CHECK(usbn_attach(0, &fake.dev), "attach accepted");
    fake_frames(1);
    CHECK(!(rd(0x54) & 1), "no connection seen in USBRESET");

    wr(0x18, HCCA);
    wr(0x04, 0x80);   /* USBOPERATIONAL */
    CHECK(!(rd(0x54) & 1), "a switched port without power sees nothing");
    wr(0x50, 0x00010000);   /* SetGlobalPower */
    CHECK((rd(0x54) & 0x00010101) == 0x00010101, "powered, connected, CSC (%08X)", rd(0x54));
    CHECK(rd(0x0c) & 0x40, "RootHubStatusChange");
    CHECK(!(rd(0x54) & 0x200), "full speed: LSDA clear");
    wr(0x54, 0x00010000);
    CHECK(!(rd(0x54) & 0x00010000), "CSC is write-1-to-clear");
    wr(0x0c, 0x40);

    wr(0x54, 0x10);   /* SetPortReset */
    CHECK(rd(0x54) & 0x10, "port in reset");
    CHECK(fake.resets == 1, "reset signalling reaches the device");
    fake_frames(1);
    CHECK((rd(0x54) & 0x00100012) == 0x00100002, "reset ended: enabled, PRSC (%08X)", rd(0x54));
    wr(0x54, 0x00100000);
    wr(0x0c, 0x7f);
}

static void
test_control_transfer(void)
{
    static const uint8_t setup[8] = { 0x80, 6, 0, 1, 0, 0, 18, 0 };
    uint32_t             fn;

    memcpy(ram + BUF, setup, 8);
    memset(ram + BUF + 0x100, 0, 32);
    gtd(TD0 + 0x00, 0, 0, 0, 7, BUF, 8, TD0 + 0x10);
    gtd(TD0 + 0x10, 2, 1, 1, 7, BUF + 0x100, 18, TD0 + 0x20);
    gtd(TD0 + 0x20, 1, 1, 0, 0, 0, 0, TD0 + 0x30);   /* status, DelayInterrupt 0 */
    ed(ED0, 0, 0, 0, 8, 0, TD0, TD0 + 0x30, 0);
    wr32m(HCCA + 0x84, 0);

    wr(0x20, ED0);
    wr(0x10, 0x80000002);   /* MIE | WDH */
    fake.naks = 1000000;   /* a device busy for good: NAKed over and over */
    wr(0x40, 0x2a27);       /* PeriodicStart at 90 %, as drivers set it */
    wr(0x04, 0x80 | 0x10);  /* operational, ControlListEnable */
    wr(0x08, 0x02);         /* ControlListFilled */

    fake_frames(1);
    CHECK(CC(TD0) == 0, "SETUP done (CC %X)", CC(TD0));
    CHECK(CC(TD0 + 0x10) == 0xf, "the data stage waits out the NAKs");
    CHECK((fake.naks < 1000000) && (fake.naks > 1000000 - 1000), "NAKs retried within the frame's bus time (%d)", 1000000 - fake.naks);
    fake.naks = 0;
    fake_frames(4);
    CHECK(!memcmp(ram + BUF + 0x100, fake_dev_desc, 18), "descriptor in guest memory");
    CHECK((CC(TD0 + 0x10) == 0) && (rd32m(TD0 + 0x14) == 0), "data stage done, CBP 0");
    CHECK(CC(TD0 + 0x20) == 0, "status stage done");
    CHECK((rd32m(ED0 + 8) & ~0xf) == (TD0 + 0x30), "ED head reached its tail");
    CHECK(!(rd32m(ED0 + 8) & 1), "ED not halted");
    CHECK((rd32m(HCCA + 0x84) & ~1) == TD0 + 0x20, "HccaDoneHead: the status TD first (%08X)", rd32m(HCCA + 0x84));
    CHECK(rd32m(TD0 + 0x28) == TD0 + 0x10, "done queue links back to the data TD");
    CHECK(rd(0x0c) & 0x02, "WritebackDoneHead");
    CHECK(fake_irq[1], "INTA asserted");
    fn = rd32m(HCCA + 0x80) & 0xffff;
    CHECK((fn != 0) && (fn == (rd(0x3c) & 0xffff)), "HccaFrameNumber follows HcFmNumber (%04X)", fn);
    wr(0x0c, 0x02);
    CHECK(!fake_irq[1], "INTA drops with WDH cleared");
}

static void
test_stall_and_halt(void)
{
    fake.dev.addr = 3;
    gtd(TD0, 1, -1, 0, 0, BUF, 4, TD0 + 0x10);
    ed(ED0, 3, 1, 0, 8, 0, TD0, TD0 + 0x10, 0);
    wr(0x08, 0x02);
    fake_frames(2);
    CHECK(CC(TD0) == 4, "STALL completion code (%X)", CC(TD0));
    CHECK(rd32m(ED0 + 8) & 1, "the ED is halted");
    wr(0x0c, 0x7f);
}

static void
test_short_packet(void)
{
    /* Endpoint 2 IN answers 2 bytes. Without BufferRounding: underrun. */
    gtd(TD0, 2, -1, 0, 0, BUF + 0x200, 8, TD0 + 0x10);
    ed(ED1, 3, 2, 0, 8, 0, TD0, TD0 + 0x10, 0);
    wr(0x20, ED1);
    wr(0x08, 0x02);
    fake_frames(2);
    CHECK(CC(TD0) == 9, "short packet without rounding: DataUnderrun (%X)", CC(TD0));
    CHECK(rd32m(ED1 + 8) & 1, "and the ED halts");
    CHECK(rd32m(TD0 + 4) == BUF + 0x202, "CBP after the 2 bytes (%08X)", rd32m(TD0 + 4));

    gtd(TD0, 2, -1, 1, 0, BUF + 0x200, 8, TD0 + 0x10);
    ed(ED1, 3, 2, 0, 8, 0, TD0, TD0 + 0x10, 0);
    wr(0x08, 0x02);
    fake_frames(2);
    CHECK(CC(TD0) == 0, "with BufferRounding: no error (%X)", CC(TD0));
    CHECK(!(rd32m(ED1 + 8) & 1), "ED goes on");
    CHECK((ram[BUF + 0x200] == 0xaa) && (ram[BUF + 0x201] == 0x55), "short packet data");
    wr(0x0c, 0x7f);
}

static void
test_interrupt_ed(void)
{
    /* Every interrupt table entry points at one ED: endpoint 2 IN. */
    for (int i = 0; i < 32; i++)
        wr32m(HCCA + i * 4, ED2);
    gtd(TD0, 2, -1, 1, 0, BUF + 0x300, 8, TD0 + 0x10);
    ed(ED2, 3, 2, 2, 8, 0, TD0, TD0 + 0x10, 0);
    fake.in2_calls = 0;
    wr(0x04, 0x80 | 0x04);   /* operational, PeriodicListEnable only */
    fake_frames(2);
    CHECK((CC(TD0) == 0) && (fake.in2_calls == 1), "periodic IN served once (CC %X, %d calls)", CC(TD0), fake.in2_calls);
    for (int i = 0; i < 32; i++)
        wr32m(HCCA + i * 4, 0);
    wr(0x0c, 0x7f);
}

static void
test_iso(void)
{
    uint32_t fn = rd(0x3c) & 0xffff;

    for (int i = 0; i < 32; i++)
        wr32m(HCCA + i * 4, ED2);
    ram[BUF + 0x400] = 0x5a;
    /* One packet of 16 bytes in frame fn + 1. */
    wr32m(ITD, ((fn + 1) & 0xffff) | (7u << 21) | (0u << 24) | 0xf0000000);
    wr32m(ITD + 4, BUF + 0x400);
    wr32m(ITD + 8, ITD + 0x20);
    wr32m(ITD + 12, BUF + 0x40f);
    wr32m(ITD + 16, 0xe000 | ((BUF + 0x400) & 0xfff));
    ed(ED2, 3, 5, 1, 64, 0x8000, ITD, ITD + 0x20, 0);
    wr(0x04, 0x80 | 0x04 | 0x08);   /* periodic and isochronous */
    fake.iso_out_len = 0;
    fake_frames(3);
    CHECK((fake.iso_out_len == 16) && (fake.iso_out_first == 0x5a), "iso OUT: the device got 16 bytes (%d)", fake.iso_out_len);
    CHECK((rd32m(ITD + 16) & 0xf000) == 0, "PSW completion code 0 (%04X)", rd32m(ITD + 16) & 0xffff);
    CHECK((rd32m(ED2 + 8) & ~0xf) == ITD + 0x20, "the iso TD retired");
    for (int i = 0; i < 32; i++)
        wr32m(HCCA + i * 4, 0);
    wr(0x04, 0x80 | 0x10);
    wr(0x0c, 0x7f);
}

static void
test_no_device(void)
{
    gtd(TD0, 2, -1, 0, 0, BUF, 8, TD0 + 0x10);
    ed(ED0, 9, 1, 0, 8, 0, TD0, TD0 + 0x10, 0);
    wr(0x20, ED0);
    wr(0x08, 0x02);
    fake_frames(2);
    CHECK(CC(TD0) == 5, "nobody at address 9: DeviceNotResponding (%X)", CC(TD0));
    wr(0x0c, 0x7f);
}

static void
test_ownership_and_remote_wakeup(void)
{
    wr(0x08, 0x08);   /* OwnershipChangeRequest */
    CHECK(rd(0x0c) & 0x40000000, "OwnershipChange status");
    wr(0x04, rd(0x04) | 0x100);   /* InterruptRouting: the changeover */
    CHECK(!(rd(0x08) & 0x08), "OCR clears at the changeover");
    wr(0x04, 0x80);
    wr(0x0c, 0x4000007f);

    /* Global suspend, then the device signals resume. */
    wr(0x04, 0xc0);
    fake_frames(1);   /* past the 5 ms settling */
    usbn_remote_wakeup(&fake.dev);
    CHECK((rd(0x04) & 0xc0) == 0x40, "remote wakeup in USBSUSPEND: USBRESUME (%08X)", rd(0x04));
    CHECK(rd(0x0c) & 0x08, "ResumeDetected");
    wr(0x04, 0x80);
    wr(0x0c, 0x7f);
}

static void
test_disconnect_and_reset(void)
{
    usbn_detach(0);
    fake_frames(1);
    CHECK(!(rd(0x54) & 1) && (rd(0x54) & 0x00030000), "unplugged: CCS clear, CSC and PESC (%08X)", rd(0x54));
    CHECK(fake.destroyed == 1, "the device is released");
    wr(0x08, 0x01);   /* HostControllerReset */
    CHECK((rd(0x04) & 0xc0) == 0xc0, "USBSUSPEND after HostControllerReset");
    CHECK(rd(0x18) == 0, "HCCA cleared");
}

int
main(void)
{
    fake_dev_init(&fake, "fake", USBN_SPEED_FULL);
    void *dev = usb_opti_82c861_device.init(&usb_opti_82c861_device);

    test_pci_and_registers();
    test_power_connect_reset();
    test_control_transfer();
    test_stall_and_halt();
    test_short_packet();
    test_interrupt_ed();
    test_iso();
    test_no_device();
    test_ownership_and_remote_wakeup();
    test_disconnect_and_reset();
    usb_opti_82c861_device.close(dev);

    printf("OHCI (OPTi): %d checks, %d failed\n", fake_checks, fake_failures);
    return fake_failures ? 1 : 0;
}
