/*
 * 86Box-Next  Tests for the PIIX3's USB function (src/usb/usb_uhci.c built
 *             into a southbridge): its configuration space, and the legacy
 *             keyboard/mouse support -- LEGSUP's traps on ports 60h and 64h,
 *             the A20GATE pass-through, the USB interrupt as an SMI or on
 *             PIRQD -- against the 82371SB datasheet, section 2.4.
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

/* The card list names the USB 2.0 cards too; this test does not link them. */
const device_t usb_ehci_via_device   = { .name = "unused", .internal_name = "usb_ehci_via" };
const device_t usb_ehci_intel_device = { .name = "unused", .internal_name = "usb_ehci_intel" };

static const usbn_pci_id_t piix3 = { 0x8086, 0x7020, 0x01, 0x0000, 0x0000 };

static uhci_t    *hc;
static int        smis;
static fake_dev_t fake;

static int  settled;
static void count_smi(void *priv, int event) { (void) priv; if (event) smis++; else settled++; }

static uint8_t rd(int a) { return uhci_pci_read(0, a, 1, hc); }
static void    wr(int a, uint8_t v) { uhci_pci_write(0, a, 1, v, hc); }
static uint16_t legsup(void) { return rd(0xc0) | (rd(0xc1) << 8); }
static void    set_legsup(uint16_t v) { wr(0xc0, v & 0xff); wr(0xc1, v >> 8); }

static void
test_config(void)
{
    CHECK((rd(0x00) == 0x86) && (rd(0x01) == 0x80) && (rd(0x02) == 0x20) && (rd(0x03) == 0x70), "8086:7020");
    CHECK(rd(0x08) == 0x01, "B-0 stepping: revision 01h");
    CHECK((rd(0x09) == 0x00) && (rd(0x0a) == 0x03) && (rd(0x0b) == 0x0c), "class 0C0300");
    CHECK((rd(0x06) == 0x80) && (rd(0x07) == 0x02), "device status 0280h");
    CHECK(rd(0x0e) == 0x00, "header type 00h (function 0 says multi-function)");
    CHECK(rd(0x3d) == 0x04, "interrupt pin: PIRQD");
    CHECK(rd(0x60) == 0x10, "USB release 1.0");
    CHECK(rd(0x6a) == 0x01, "MSTAT: 48 MHz");
    CHECK(legsup() == 0x2000, "LEGSUP 2000h (%04X)", legsup());
    CHECK(rd(0x20) == 0x01, "BASEADD: I/O space");

    wr(0x04, 0xff);
    CHECK(rd(0x04) == 0x05, "PCICMD: only bus master and I/O space (%02X)", rd(0x04));
    wr(0x0d, 0xff);
    CHECK(rd(0x0d) == 0xf0, "MLT: bits 7:4");
    wr(0x20, 0x00);
    wr(0x21, 0xe0);
    CHECK(fake_io_base == 0xe000, "the registers at E000 (%04X)", fake_io_base);
    wr(0x04, 0x00);
}

static void
test_legsup_bits(void)
{
    set_legsup(0xffff);
    CHECK(legsup() == 0x20bf, "LEGSUP: RW bits set, RO and RWC left alone (%04X)", legsup());
    set_legsup(0x2000);
    CHECK(legsup() == 0x2000, "and cleared");
}

static void
test_traps(void)
{
    smis = 0;
    CHECK(!fake_port_access(0x60, 0, 0), "no trap enabled: 60h not watched");

    set_legsup(0x2000 | 0x0001);   /* 60REN */
    CHECK(fake_port_access(0x60, 0, 0), "a trapped read of 60h never reaches the keyboard controller");
    CHECK((legsup() & 0x0100) && (smis == 1), "a read of 60h: TBY60R and an SMI");
    CHECK(!fake_port_access(0x60, 1, 0xed), "a write of 60h not enabled reaches it");
    CHECK(!(legsup() & 0x0200) && (smis == 1), "a write of 60h not enabled: nothing");
    fake_port_access(0x64, 0, 0);
    CHECK(smis == 1, "nor a read of 64h");
    settled = 0;
    wr(0xc1, 0x21);                /* write 1 to bit 8: clear it */
    CHECK(!(legsup() & 0x0100) && (legsup() & 0x2000), "TBY60R is write-1-to-clear");
    CHECK(settled > 0, "and with no event left the southbridge hears so (PIIX4 GLBSTS bit 1)");

    set_legsup(0x2000 | 0x000f);
    fake_port_access(0x60, 1, 0xf4);
    fake_port_access(0x64, 0, 0);
    fake_port_access(0x64, 1, 0xad);
    CHECK(((legsup() & 0x0f00) == 0x0e00) && (smis == 4), "60W, 64R, 64W each set their bit (%04X)", legsup());
    set_legsup(0x2f00);            /* clear them all, traps off */
    CHECK((legsup() & 0x0f00) == 0, "all cleared");
    CHECK(!fake_port_access(0x64, 1, 0), "traps off: not watched");
}

static void
test_a20_pass_through(void)
{
    smis = 0;
    set_legsup(0x2000 | 0x0080 | 0x0020 | 0x000f);   /* SMIEPTE, A20PTEN, all traps */
    fake_port_access(0x64, 1, 0xd1);
    CHECK((legsup() & 0x0040) && (smis == 0), "D1h to 64h: the sequence starts, PSS, no SMI");
    fake_port_access(0x64, 1, 0xd1);
    fake_port_access(0x60, 1, 0xdf);
    fake_port_access(0x64, 0, 0);
    CHECK(!fake_port_access(0x64, 0, 0), "the sequence's cycles reach the keyboard controller");
    CHECK(((legsup() & 0x0f00) == 0) && (smis == 0), "the sequence's cycles are not trapped");
    fake_port_access(0x64, 1, 0xff);
    CHECK(!(legsup() & 0x0040) && (legsup() & 0x8000) && (smis == 1), "FFh ends it: A20PTS, and the SMI at its end");
    wr(0xc1, 0xa0);
    CHECK(!(legsup() & 0x8000), "A20PTS is write-1-to-clear");

    fake_port_access(0x64, 1, 0xd1);
    fake_port_access(0x64, 1, 0xaa);   /* not the data: a deviation */
    CHECK(!(legsup() & 0x0040) && (legsup() & 0x0800) && (smis == 2), "a deviation ends it and is trapped (%04X)", legsup());

    fake_port_access(0x64, 1, 0xd1);
    CHECK(legsup() & 0x0040, "in progress again");
    set_legsup(0x2000 | 0x000f);       /* A20PTEN off */
    CHECK(!(legsup() & 0x0040), "turning A20PTEN off ends it at once");
    set_legsup(0x2f00);
}

static void
test_interrupt(void)
{
    smis = 0;
    wr(0x04, 0x01);                    /* the registers on, at E000 */
    fake_io_ww(0x04, 0x0002);          /* USBINTR: resume */
    fake_io_ww(0x00, 0x0008);          /* USBCMD: global suspend, so a connect is a resume */
    CHECK(usbn_attach(0, &fake.dev), "a device on port 1");
    fake_frames(1);
    fake_io_ww(0x04, 0x0002);          /* the interrupt is worked out again */
    CHECK(fake_irq[0] && (legsup() & 0x1000), "resume detect: the interrupt, on PIRQD, USBIRQS set");
    CHECK(smis == 0, "USBSMIEN off: no SMI");

    set_legsup(0x2000 | 0x0010);       /* USBSMIEN while the interrupt is up */
    CHECK(smis == 1, "USBSMIEN turned on with the interrupt up: an SMI");
    set_legsup(0x0010);                /* USBPIRQDEN off */
    CHECK(!fake_irq[0] && (legsup() & 0x1000), "USBPIRQDEN off: not on PIRQD, still in USBIRQS");

    fake_io_ww(0x02, 0x0004);          /* clear resume detect */
    CHECK(!(legsup() & 0x1000), "cleared in the controller: USBIRQS goes too");
    fake_io_ww(0x00, 0x0008);
    fake_io_ww(0x10, 0x0002);          /* the connect change, cleared */
    set_legsup(0x2010);
    usbn_detach(0);
    fake_frames(1);
    fake_dev_init(&fake, "fake", USBN_SPEED_FULL);
    usbn_attach(0, &fake.dev);
    fake_frames(1);
    fake_io_ww(0x04, 0x0002);
    CHECK(smis == 2, "the next interrupt (a reconnect's resume) is an SMI again (%d)", smis);
    set_legsup(0x2000);
}

/* An inactive TD with IOC still interrupts (the Award BIOS's timer). */
static void
test_inactive_ioc(void)
{
    smis = 0;
    for (int i = 0; i < 1024; i++)
        wr32m(0x20000 + i * 4, 1);           /* every frame empty ... */
    wr32m(0x20000, 0x21000 | 2);             /* ... but frame 0: a queue head */
    wr32m(0x21000, 1);
    wr32m(0x21004, 0x21100);                 /* whose element is a TD */
    wr32m(0x21100, 1);
    wr32m(0x21104, 0x01000000);              /* inactive, IOC */
    wr32m(0x21108, 0);
    wr32m(0x2110c, 0);

    fake_io_ww(0x02, 0x003f);                /* clear the status */
    fake_io_ww(0x04, 0x0004);                /* USBINTR: IOC */
    set_legsup(0x0010);                      /* USBSMIEN, not on PIRQD */
    fake_io_ww(0x08, 0x0000);
    fake_io_ww(0x0a, 0x0002);                /* frame list at 20000h */
    fake_io_ww(0x06, 0x0000);
    fake_io_ww(0x00, 0x0001);                /* run */
    fake_frames(1);
    CHECK((fake_io_rw(0x02) & 0x0001) && (smis == 1), "an inactive TD with IOC: USBINT and an SMI (%04X, %d)",
          fake_io_rw(0x02), smis);
    fake_io_ww(0x02, 0x0001);
    fake_frames(1);
    CHECK(!(fake_io_rw(0x02) & 0x0001), "not in a frame without it");
    fake_io_ww(0x00, 0x0000);
    set_legsup(0x2000);
}

/* A TD with an illegal MaxLen (500h-7FEh): a host controller process error,
   which halts the controller and interrupts whatever USBINTR says (the Intel
   VS440FX's BIOS uses it as its SMI timer); FRNUM stays, so a restart meets
   the same TD in the next frame. */
static void
test_process_error(void)
{
    smis = 0;
    for (int i = 0; i < 1024; i++)
        wr32m(0x20000 + i * 4, 1);
    wr32m(0x20000, 0x21100);                 /* frame 0: a TD */
    wr32m(0x21100, 1);
    wr32m(0x21104, 0x10800000);              /* active, C_ERR 2, no IOC */
    wr32m(0x21108, 0xbfe27fe1);              /* OUT, address 127, endpoint 4, MaxLen 5FFh */
    wr32m(0x2110c, 0);

    fake_io_ww(0x02, 0x003f);
    fake_io_ww(0x04, 0x0000);                /* USBINTR: nothing */
    set_legsup(0x0010);                      /* USBSMIEN */
    fake_io_ww(0x08, 0x0000);
    fake_io_ww(0x0a, 0x0002);
    fake_io_ww(0x06, 0x0000);
    fake_io_ww(0x00, 0x0001);
    fake_frames(1);
    CHECK((fake_io_rw(0x02) & 0x0030) == 0x0030, "MaxLen 5FFh: process error, halted (%04X)", fake_io_rw(0x02));
    CHECK(!(fake_io_rw(0x00) & 0x0001), "Run/Stop cleared");
    CHECK(smis == 1, "an SMI, though USBINTR is 0 (%d)", smis);
    CHECK(rd32m(0x21104) & 0x00800000, "the TD is left active");
    CHECK((fake_io_rw(0x06) & 0x3ff) == 0, "halted in frame 0: FRNUM stays (%03X)", fake_io_rw(0x06));
    fake_io_ww(0x02, 0x0010);
    CHECK(!(fake_io_rw(0x02) & 0x0010), "HCPE is write-1-to-clear");
    fake_io_ww(0x00, 0x0001);                /* restarted: the same frame, the same error */
    fake_frames(1);
    CHECK(((fake_io_rw(0x02) & 0x0030) == 0x0030) && (smis == 2), "restarted, it meets the TD again (%04X, %d)",
          fake_io_rw(0x02), smis);
    fake_io_ww(0x02, 0x0030);
    set_legsup(0x2000);
}

static void
test_ports_owner(void)
{
    static const usbn_root_ops_t other = { NULL, NULL, NULL, NULL };
    int                          x;

    CHECK(usbn_present() && (usbn_port_role(0) == USBN_ROLE_ONBOARD) && usbn_port_exists(1), "the onboard controller has the motherboard's ports");
    usbn_card_context = 1;   /* as usb_card_reset() makes the card */
    CHECK(usbn_set_root(&other, &x, 0), "a card fitted later gets ports of its own");
    usbn_card_context = 0;
    CHECK(usbn_port_exists(USBN_ROLE_CARD * USBN_ROOT_PORTS) && usbn_port_exists(0), "the card's and the motherboard's side by side");
    CHECK(usbn_set_root(NULL, &x, 0) && !usbn_port_exists(USBN_ROLE_CARD * USBN_ROOT_PORTS), "the card goes with its ports");
    CHECK(usbn_port_exists(0), "the motherboard's stay");
}

int
main(void)
{
    fake_dev_init(&fake, "fake", USBN_SPEED_FULL);
    hc = uhci_onboard_create(&piix3, count_smi, NULL);

    test_config();
    test_legsup_bits();
    test_traps();
    test_a20_pass_through();
    test_interrupt();
    test_inactive_ioc();
    test_process_error();
    test_ports_owner();

    uhci_onboard_reset(hc);
    CHECK((legsup() == 0x2000) && (rd(0x04) == 0x00), "PCIRST#: LEGSUP and PCICMD back to their defaults");
    uhci_onboard_close(hc);
    CHECK(!usbn_present(), "closed: the ports are free");

    printf("%d checks, %d failures\n", fake_checks, fake_failures);
    return fake_failures != 0;
}
