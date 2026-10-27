/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             Upstream 86Box's USB devices on our USB bus: a device built on
 *             upstream's device core (usb_dev.c, usb_dev_t -- for now the
 *             4-port hub, usb_hub.c, both taken as they are) is shown to our
 *             controllers as a usbn_device_t, each transaction handed to
 *             usb_dev_packet() with the data toggle it expects (our
 *             controllers do not track toggles).
 *
 *             The hub's downstream ports are bus slots USBN_HUB_BASE and
 *             up: what is plugged there (the emulated keyboard and mouse,
 *             passed-through host devices) reaches the hub as a stand-in
 *             usb_dev_t ("shim") whose bus reset resets our device, and the
 *             controllers find it through the hub's route().
 *
 *             This file also has what upstream's usb.c gave the device core:
 *             usb_dev_add() / usb_dev_remove(), usb_dev_suspended() and
 *             usb_resume_upstream().
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
#include <86box/plat_unused.h>
#include <86box/usb_dev.h>
#include <86box/usb_next.h>

/* What the hub needs of a device on one of its ports: a descriptor to
   satisfy usb_dev_init() and a configuration with no interfaces. */
static const uint8_t shim_dev_desc[18] = { 18, 1, 0x10, 0x01, 0, 0, 0, 8, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1 };
static const uint8_t shim_cfg_desc[9]  = { 9, 2, 9, 0, 0, 1, 0, 0x80, 50 };

typedef struct shim_t {
    usb_dev_t      udev;
    usbn_device_t *d;      /* our device, NULL while the port is empty */
} shim_t;

typedef struct bridge_t {
    usbn_device_t dev;     /* what our bus sees */
    usb_dev_t    *udev;    /* upstream's device, NULL once it closed */
    int           port;    /* its bus slot, -1 for none */
    shim_t        shim[USBN_HUB_PORTS];
} bridge_t;

#define BRIDGES_MAX 4
static bridge_t *bridges[BRIDGES_MAX];
static bridge_t *hub_bridge;

static bridge_t *
bridge_of(const usb_dev_t *udev)
{
    for (int i = 0; i < BRIDGES_MAX; i++) {
        if (bridges[i] && (bridges[i]->udev == udev))
            return bridges[i];
    }
    return NULL;
}

/* ---------------------------------------------- the device, to our bus --- */

static int
bridge_packet(usbn_device_t *d, uint8_t pid, uint8_t ep, uint8_t *buf, int len)
{
    bridge_t    *b = (bridge_t *) d->priv;
    usb_packet_t p;
    int          ret;

    if (b->udev == NULL)
        return USBN_NODEV;
    memset(&p, 0, sizeof(p));
    p.pid  = pid;
    p.ep   = ep & 0x0f;
    p.data = buf;
    p.len  = len;
    if (pid == USB_PID_IN)
        p.toggle = b->udev->ep_in[p.ep].toggle;
    else if (pid == USB_PID_OUT)
        p.toggle = b->udev->ep_out[p.ep].toggle;

    ret     = usb_dev_packet(b->udev, &p);
    d->addr = b->udev->addr;   /* SET_ADDRESS takes effect after its status stage */
    switch (ret) {
        case USB_RET_ACK:
        case USB_RET_TOGGLE_MISMATCH:
            return (pid == USB_PID_IN) ? p.actual : len;
        case USB_RET_NAK:
            return USBN_NAK;
        case USB_RET_STALL:
            return USBN_STALL;
        case USB_RET_BABBLE:
        case USB_RET_OVERRUN:
            return USBN_BABBLE;
        default:
            return USBN_NODEV;
    }
}

static void
bridge_reset(usbn_device_t *d)
{
    bridge_t *b = (bridge_t *) d->priv;

    if (b->udev)
        usb_dev_bus_reset(b->udev);
    d->addr = 0;
}

/* The hub's downstream device answering at addr: the shims' addresses
   follow our devices' (they take SET_ADDRESS themselves). */
static usbn_device_t *
bridge_route(usbn_device_t *d, uint8_t addr, int low_speed)
{
    bridge_t  *b = (bridge_t *) d->priv;
    usb_dev_t *r;

    if ((b->udev == NULL) || (b->udev->ops->route == NULL))
        return NULL;
    for (int i = 0; i < USBN_HUB_PORTS; i++) {
        if (b->shim[i].d) {
            b->shim[i].udev.addr      = b->shim[i].d->addr;
            b->shim[i].udev.low_speed = (b->shim[i].d->speed == USBN_SPEED_LOW);
        }
    }
    r = b->udev->ops->route(b->udev, addr, low_speed);
    for (int i = 0; r && (i < USBN_HUB_PORTS); i++) {
        if (r == &b->shim[i].udev)
            return b->shim[i].d;
    }
    return NULL;
}

static void
bridge_unhook_shims(bridge_t *b)
{
    for (int i = 0; i < USBN_HUB_PORTS; i++) {
        if (b->udev && b->udev->ops->detach && b->shim[i].udev.parent)
            b->udev->ops->detach(b->udev, &b->shim[i].udev);
        b->shim[i].d = NULL;
    }
}

static void
bridge_forget(bridge_t *b)
{
    for (int i = 0; i < BRIDGES_MAX; i++) {
        if (bridges[i] == b)
            bridges[i] = NULL;
    }
    if (hub_bridge == b) {
        hub_bridge = NULL;
        usbn_set_hub(NULL, NULL);
    }
}

/* Unplugged, or the bus went away: upstream's device lives on (its own
   close frees it); it is only off our bus. */
static void
bridge_destroy(usbn_device_t *d)
{
    bridge_t *b = (bridge_t *) d->priv;

    bridge_unhook_shims(b);
    bridge_forget(b);
    if (b->udev)
        b->udev->port = NULL;
    free(b);
}

/* -------------------------------------------------------- speeds --- */

static const usb_dev_ops_t shim_ops;   /* the hub ports' stand-ins, below */

void
usbn_bridge_set_high_speed(usb_dev_t *udev)
{
    bridge_t *b = bridge_of(udev);

    if (b)
        b->dev.speed = USBN_SPEED_HIGH;
}

/* A high-speed device that a root port took at high speed (an EHCI's, or
   a USB 2.0 hub's upstream). */
int
usbn_bridge_running_high_speed(const usb_dev_t *udev)
{
    const bridge_t *b = bridge_of(udev);

    return b && (b->dev.speed == USBN_SPEED_HIGH) && !b->dev.fs_view;
}

/* A high-speed device behind a hub, seen at high speed when the hub runs at
   high speed (a USB 2.0 hub on an EHCI port), else at full speed: set again
   at each of the hub's resets, as its speed follows the port it is on. */
int
usbn_bridge_child_high_speed(const usb_dev_t *shim)
{
    const shim_t *s = (const shim_t *) shim->priv;

    return (shim->ops == &shim_ops) && s->d && (s->d->speed == USBN_SPEED_HIGH) && !s->d->fs_view;
}

void
usbn_bridge_hub_speed(usb_dev_t *hub_udev, int high_speed)
{
    const bridge_t *b = bridge_of(hub_udev);

    for (int i = 0; b && (i < USBN_HUB_PORTS); i++) {
        if (b->shim[i].d)
            b->shim[i].d->fs_view = (b->shim[i].d->speed == USBN_SPEED_HIGH) && !high_speed;
    }
}

/* --------------------------------------- the hub's downstream ports --- */

static void
shim_reset(usb_dev_t *udev)
{
    shim_t *s = (shim_t *) udev->priv;

    if (s->d) {
        s->d->addr = 0;
        if (s->d->reset)
            s->d->reset(s->d);
    }
}

static const usb_dev_ops_t shim_ops = { .reset = shim_reset };

static void
hub_connect(void *priv, int hp, usbn_device_t *d)
{
    bridge_t *b = (bridge_t *) priv;
    shim_t   *s = &b->shim[hp];

    if ((b->udev == NULL) || (b->udev->ops->attach == NULL))
        return;
    /* Behind a USB 1.1 hub, or a USB 2.0 one running at full speed, a
       high-speed device is shown at full speed. */
    d->fs_view = (d->speed == USBN_SPEED_HIGH) && !usbn_bridge_running_high_speed(b->udev);
    memset(s, 0, sizeof(*s));
    s->d              = d;
    s->udev.name      = d->name;
    s->udev.low_speed = (d->speed == USBN_SPEED_LOW);
    s->udev.dev_desc  = shim_dev_desc;
    s->udev.cfg_desc  = shim_cfg_desc;
    s->udev.cfg_len   = sizeof(shim_cfg_desc);
    s->udev.ops       = &shim_ops;
    s->udev.priv      = s;
    usb_dev_init(&s->udev);
    if (!b->udev->ops->attach(b->udev, &s->udev)) {
        pclog("USB: the hub has no free port for %s\n", d->name);
        s->d = NULL;
    }
}

static void
hub_disconnect(void *priv, int hp)
{
    bridge_t *b = (bridge_t *) priv;
    shim_t   *s = &b->shim[hp];

    if (b->udev && b->udev->ops->detach && s->udev.parent)
        b->udev->ops->detach(b->udev, &s->udev);
    s->d = NULL;
}

static void
hub_resume(void *priv, int hp)
{
    bridge_t *b = (bridge_t *) priv;
    shim_t   *s = &b->shim[hp];

    if (b->udev && b->udev->ops->resume && s->udev.parent)
        b->udev->ops->resume(b->udev, &s->udev);
}

static const usbn_hub_ops_t bridge_hub_ops = { hub_connect, hub_disconnect, hub_resume };

/* ------------------------------------- what upstream's usb.c gave usb_dev --- */

/* A device of upstream's at its init: onto the first free root port. */
void
usb_dev_add(usb_dev_t *udev, UNUSED(const device_t *info))
{
    bridge_t *b = calloc(1, sizeof(bridge_t));
    int       slot;

    for (slot = 0; (slot < BRIDGES_MAX) && bridges[slot]; slot++)
        ;
    if (slot == BRIDGES_MAX) {
        free(b);
        return;
    }
    bridges[slot] = b;
    b->udev       = udev;
    b->port       = -1;
    snprintf(b->dev.name, sizeof(b->dev.name), "%s", udev->name);
    b->dev.speed   = udev->low_speed ? USBN_SPEED_LOW : USBN_SPEED_FULL;
    b->dev.priv    = b;
    b->dev.packet  = bridge_packet;
    b->dev.reset   = bridge_reset;
    b->dev.destroy = bridge_destroy;
    if (udev->ops->route)
        b->dev.route = bridge_route;

    /* The hub goes on the controller port it is assigned (Settings > Ports >
       USB): "hub" in usbn_port_cfg.  Not assigned, it is not plugged in. */
    {
        const int p = usbn_assigned("hub");

        if ((p >= 0) && (usbn_port_role(p) >= 0) && usbn_attach(p, &b->dev))
            b->port = p;
    }
    if (b->port < 0) {
        pclog("USB: the %s is not connected to a USB port (Settings > Ports > USB)\n", udev->name);
        bridges[slot] = NULL;
        free(b);
        return;
    }
    udev->port = b;
    if (b->dev.route && (hub_bridge == NULL)) {
        hub_bridge = b;
        usbn_set_hub(&bridge_hub_ops, b);
        usbn_restore_ports();   /* host devices remembered on the hub's ports */
    }
}

/* At its close: off the bus.  At a hard reset the controller may already
   have released everything (bridge_destroy). */
void
usb_dev_remove(usb_dev_t *udev)
{
    bridge_t *b = bridge_of(udev);

    if (b == NULL)
        return;
    bridge_unhook_shims(b);
    bridge_forget(b);
    b->udev = NULL;
    usbn_detach(b->port);   /* destroyed at the next frame, or with the bus */
}

int
usb_dev_suspended(const usb_dev_t *udev)
{
    bridge_t *b;

    if (udev->parent)
        return udev->parent->ops->port_idle && udev->parent->ops->port_idle(udev->parent, udev);
    b = bridge_of(udev);
    return b ? usbn_root_idle(&b->dev) : 0;
}

void
usb_resume_upstream(usb_dev_t *udev)
{
    bridge_t *b;

    if (udev->parent) {
        if (udev->parent->ops->resume)
            udev->parent->ops->resume(udev->parent, udev);
        return;
    }
    if ((b = bridge_of(udev)) != NULL)
        usbn_remote_wakeup(&b->dev);
}
