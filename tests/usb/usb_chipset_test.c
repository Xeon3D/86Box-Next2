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

static fake_dev_t fake, fake2;
static int        irq_level[2];

static void irq0(void *priv, int level) { (void) priv; irq_level[0] = level; }
static void irq1(void *priv, int level) { (void) priv; irq_level[1] = level; }

int
main(void)
{
    usb_t *a, *b;

    fake_dev_init(&fake, "fake", USBN_SPEED_FULL);
    fake_dev_init(&fake2, "fake2", USBN_SPEED_FULL);
    a = usb_device.init(&usb_device);
    b = usb_device.init(&usb_device);
    uhci_set_irq_handler(a, irq0, NULL);
    uhci_set_irq_handler(b, irq1, NULL);
    CHECK(a->uhci && b->uhci && !a->ohci, "each usb_t became a UHCI when used as one");
    CHECK(usbn_port_exists(0) && usbn_port_exists(1) && !usbn_port_exists(2), "the first function: the motherboard's two ports");
    CHECK(usbn_port_exists(USBN_ROOT_PORTS) && usbn_port_exists(USBN_ROOT_PORTS + 1), "the second function: two of its own");
    CHECK(!usbn_port_exists(2 * USBN_ROOT_PORTS), "no card ports without a card");

    /* The ports' names in the config, and an older config written down. */
    char key[32];
    CHECK(!strcmp(usbn_port_key(0, key, sizeof(key)), "onboard_1"), "slot 0 is onboard_1 (%s)", key);
    CHECK(!strcmp(usbn_port_key(USBN_ROOT_PORTS + 1, key, sizeof(key)), "onboard2_2"), "the second function's port 2 (%s)", key);
    CHECK(!strcmp(usbn_port_key(USBN_HUB_BASE + 2, key, sizeof(key)), "hub_3"), "hub port 3 (%s)", key);
    CHECK(usbn_port_from_key("card_2") == (USBN_ROLE_CARD * USBN_ROOT_PORTS + 1), "card_2 found by name");
    CHECK(usbn_port_from_key("card2_1") == ((USBN_ROLE_CARD + 1) * USBN_ROOT_PORTS), "the second card's port 1 (card2_1)");
    CHECK(usbn_port_role(usbn_port_from_key("card4_2")) == (USBN_ROLE_CARD + 3), "the fourth card's ports");
    usbn_layout = 0;
    snprintf(usbn_legacy_cfg[0], sizeof(usbn_legacy_cfg[0]), "1234:5678");
    usbn_migrate(1, 1, 1);
    CHECK(!strcmp(usbn_port_cfg[0], "1234:5678") && !strcmp(usbn_port_cfg[1], "hub"), "old port1 kept, the hub next (%s, %s)", usbn_port_cfg[0], usbn_port_cfg[1]);
    CHECK((usbn_assigned("keyboard") == USBN_HUB_BASE) && (usbn_assigned("mouse") == USBN_HUB_BASE + 1), "keyboard and mouse on the hub's first ports");
    CHECK((usbn_layout == 2) && !usbn_legacy_cfg[0][0], "written down once");
    usbn_migrate(0, 0, 0);
    CHECK(!strcmp(usbn_port_cfg[1], "hub"), "a second migration changes nothing");
    for (int p = 0; p < USBN_PORTS; p++)
        usbn_port_cfg[p][0] = '\0';

    uhci_update_io_mapping(a, 0x00, 0xd0, 1);
    uhci_set_bus_master(a, 1);
    CHECK(fake_io_base == 0xd000, "I/O mapped where the southbridge says (%04X)", fake_io_base);
    CHECK(fake_io_rw(0x02) & 0x20, "halted after reset");

    CHECK(usbn_attach(0, &fake.dev), "attach to the first function's port 1");
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

    /* The BIOS turns the first function off: its port keeps the device,
       out of the guest's reach, as on the board. */
    uhci_set_present(a, 0);
    CHECK(usbn_port_device(0) == &fake.dev, "the device stays on its port");
    CHECK(fake.destroyed == 0, "nothing was unplugged");
    CHECK(usbn_attach(USBN_ROOT_PORTS, &fake2.dev), "the second function's port takes another");
    fake_frames(1);
    uhci_update_io_mapping(b, 0x20, 0xd0, 1);
    CHECK((fake_io_rw(0x10) & 0x0003) == 0x0003, "and shows it there (%04X)", fake_io_rw(0x10));

    usb_device.close(a);
    CHECK(fake.destroyed == 1, "closing the first function releases its port's device");
    CHECK(fake2.destroyed == 0, "but not the second's");
    usb_device.close(b);
    CHECK(!usbn_present(), "both gone: no controller");

    printf("USB chipset glue: %d checks, %d failed\n", fake_checks, fake_failures);
    return fake_failures ? 1 : 0;
}
