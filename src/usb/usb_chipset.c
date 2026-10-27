/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             The southbridges' USB function behind upstream 86Box's usb.h
 *             interface (usb_device, uhci_update_io_mapping(),
 *             ohci_update_mem_mapping(), ...), so the chipset files take
 *             upstream's wiring as it is -- VIA's UHCIs, the ALi M5237, SiS
 *             7001, SMSC SLC90E66 and STPC OHCIs, and the OPTi FireLink card
 *             -- while the controllers underneath are ours: usb_uhci.c and
 *             usb_ohci.c on our USB ports (host passthrough, the emulated
 *             keyboard and mouse).  A usb_t becomes a UHCI or an OHCI the
 *             first time the southbridge uses it as one.  Upstream's usb.c
 *             (its own controllers and devices) is not built.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/mem.h>
#include <86box/plat_unused.h>
#include <86box/usb.h>
#include <86box/usb_next.h>

static uhci_t *
as_uhci(usb_t *dev)
{
    if ((dev->uhci == NULL) && (dev->ohci == NULL))
        dev->uhci = uhci_chipset_create();
    return dev->uhci;
}

static ohci_t *
as_ohci(usb_t *dev)
{
    if ((dev->uhci == NULL) && (dev->ohci == NULL))
        dev->ohci = ohci_core_create();
    return dev->ohci;
}

void
uhci_update_io_mapping(usb_t *dev, uint8_t base_l, uint8_t base_h, int enable)
{
    uhci_t *hc = as_uhci(dev);

    dev->uhci_io_base = (base_l | (base_h << 8)) & ~0x1f;
    dev->uhci_enable  = enable;
    if (hc)
        uhci_chipset_set_io(hc, dev->uhci_io_base, enable);
}

void
uhci_set_irq_handler(usb_t *dev, void (*set_irq)(void *priv, int level), void *priv)
{
    uhci_t *hc = as_uhci(dev);

    if (hc)
        uhci_chipset_set_irq(hc, set_irq, priv);
}

void
uhci_set_bus_master(usb_t *dev, int enable)
{
    uhci_t *hc = as_uhci(dev);

    if (hc)
        uhci_chipset_set_bus_master(hc, enable);
}

void
uhci_set_babble_keep_port(usb_t *dev, int keep)
{
    uhci_t *hc = as_uhci(dev);

    if (hc)
        uhci_chipset_set_babble_keep_port(hc, keep);
}

void
uhci_set_present(usb_t *dev, int present)
{
    uhci_t *hc = as_uhci(dev);

    if (hc)
        uhci_chipset_set_present(hc, present);
}

void
ohci_update_mem_mapping(usb_t *dev, uint8_t base1, uint8_t base2, uint8_t base3, int enable)
{
    ohci_t *hc = as_ohci(dev);

    dev->ohci_mem_base = ((base1 << 8) | (base2 << 16) | ((uint32_t) base3 << 24)) & 0xfffff000;
    dev->ohci_enable   = enable;
    if (hc)
        ohci_core_set_window(hc, dev->ohci_mem_base, enable);
}

void
ohci_set_irq_handler(usb_t *dev, void (*set_irq)(void *priv, int level), void *priv)
{
    ohci_t *hc = as_ohci(dev);

    if (hc)
        ohci_core_set_irq(hc, set_irq, priv);
}

void
ohci_set_smi_handler(usb_t *dev, void (*set_smi)(void *priv, int level), void *priv)
{
    ohci_t *hc = as_ohci(dev);

    if (hc)
        ohci_core_set_smi(hc, set_smi, priv);
}

void
ohci_set_bus_master(usb_t *dev, int enable)
{
    ohci_t *hc = as_ohci(dev);

    if (hc)
        ohci_core_set_bus_master(hc, enable, 0);
}

void
ohci_hardwire_bus_master(usb_t *dev)
{
    ohci_t *hc = as_ohci(dev);

    if (hc)
        ohci_core_set_bus_master(hc, 1, 1);
}

void
ohci_set_chip_flags(usb_t *dev, uint32_t flags)
{
    ohci_t *hc = as_ohci(dev);

    if (hc)
        ohci_core_set_chip_flags(hc, flags);
}

void
ohci_set_ndp(usb_t *dev, int ndp, int ports)
{
    ohci_t *hc = as_ohci(dev);

    if (hc)
        ohci_core_set_ndp(hc, ndp, ports);
}

/* PCIRST#: the controller resets, its window or I/O off.  Devices stay. */
static void
usb_reset(void *priv)
{
    usb_t *dev = (usb_t *) priv;

    if (dev->uhci)
        uhci_chipset_reset(dev->uhci);
    if (dev->ohci)
        ohci_core_reset(dev->ohci);
    dev->uhci_enable = 0;
    dev->ohci_enable = 0;
}

static void
usb_close(void *priv)
{
    usb_t *dev = (usb_t *) priv;

    if (dev->uhci)
        uhci_chipset_close(dev->uhci);
    if (dev->ohci)
        ohci_core_close(dev->ohci);
    free(dev);
}

static void *
usb_init(UNUSED(const device_t *info))
{
    return calloc(1, sizeof(usb_t));
}

const device_t usb_device = {
    .name          = "Universal Serial Bus",
    .internal_name = "usb",
    .flags         = DEVICE_PCI,
    .local         = 0,
    .init          = usb_init,
    .close         = usb_close,
    .reset         = usb_reset,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = NULL
};
