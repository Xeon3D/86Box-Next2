/*
 * 86Box-Next  Tests for the southbridges' USB function behind upstream's usb.h
 *             (src/usb/usb_chipset.c): a UHCI driven by its southbridge, as
 *             VIA's are -- I/O mapped, interrupt routed and bus mastering set
 *             from outside -- and the ports moving to the second function
 *             when the BIOS turns off the one that has them (the 686's
 *             Rx48 bit 2).
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

const device_t usb_ehci_via_device   = { .name = "unused", .internal_name = "usb_ehci_via" };
const device_t usb_ehci_intel_device = { .name = "unused", .internal_name = "usb_ehci_intel" };

static fake_dev_t fake;
static int        irq_level[2];

static void irq0(void *priv, int level) { (void) priv; irq_level[0] = level; }
static void irq1(void *priv, int level) { (void) priv; irq_level[1] = level; }

int
main(void)
{
    usb_t *a, *b;

    fake_dev_init(&fake, "fake", USBN_SPEED_FULL);
    a = usb_device.init(&usb_device);
    b = usb_device.init(&usb_device);
    uhci_set_irq_handler(a, irq0, NULL);
    uhci_set_irq_handler(b, irq1, NULL);
    CHECK(a->uhci && b->uhci && !a->ohci, "each usb_t became a UHCI when used as one");
    CHECK(usbn_root_owner() == a->uhci, "the first function has the ports");

    uhci_update_io_mapping(a, 0x00, 0xd0, 1);
    uhci_set_bus_master(a, 1);
    CHECK(fake_io_base == 0xd000, "I/O mapped where the southbridge says (%04X)", fake_io_base);
    CHECK(fake_io_rw(0x02) & 0x20, "halted after reset");

    CHECK(usbn_attach(0, &fake.dev), "attach accepted");
    fake_frames(1);
    CHECK(fake_io_rw(0x10) & 1, "the device is on the first function's port");

    /* The interrupt goes to the southbridge's handler. */
    fake_io_ww(0x04, 0x0002);
    fake_io_ww(0x00, 0x0008);   /* EGSM */
    fake_io_ww(0x10, 0x0004);
    usbn_remote_wakeup(&fake.dev);
    CHECK(irq_level[0] && !irq_level[1], "Resume Detect raised the first function's line");
    fake_io_ww(0x02, 0x0004);
    CHECK(!irq_level[0], "and it drops with the status");
    fake_io_ww(0x00, 0x0000);

    /* The BIOS turns the first function off: the ports move, device and all. */
    uhci_set_present(a, 0);
    CHECK(usbn_root_owner() == b->uhci, "the ports moved to the second function");
    uhci_update_io_mapping(b, 0x20, 0xd0, 1);
    CHECK(fake_io_base == 0xd020, "second function mapped (%04X)", fake_io_base);
    CHECK((fake_io_rw(0x10) & 0x0003) == 0x0003, "the device shows up there, connect change set (%04X)", fake_io_rw(0x10));
    CHECK(fake.destroyed == 0, "nothing was unplugged on the way");

    /* On again: the second keeps them while it is on. */
    uhci_set_present(a, 1);
    CHECK(usbn_root_owner() == b->uhci, "a function turned back on does not take them back");

    usb_device.close(a);
    usb_device.close(b);
    CHECK(usbn_root_owner() == NULL, "both gone: no controller has the ports");

    printf("USB chipset glue: %d checks, %d failed\n", fake_checks, fake_failures);
    return fake_failures ? 1 : 0;
}
