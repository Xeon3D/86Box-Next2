/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             The physical USB ports: what is plugged into each, the plug and
 *             unplug requests from the UI (applied on the emulation thread at
 *             the next frame), the ports remembered in the config, and which
 *             controller the ports are wired to -- the standalone UHCI card,
 *             or the EHCI card, which hands each port to itself or to its
 *             companion UHCI as the guest's driver decides.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <86box/86box.h>
#include <86box/path.h>
#include <86box/plat.h>
#include <86box/thread.h>
#include <86box/usb_next.h>

char usbn_port_cfg[USBN_PORTS][16];

/* ------------------------------------------------------------ the trace --- */

int             usbn_trace_on;
uint32_t        usbn_frame_count;
static FILE    *trace_fp;
static uint32_t trace_t0;
static mutex_t *trace_lock;

uint32_t
usbn_ms(void)
{
    return plat_get_ticks();
}

void
usbn_trace_enable(int on)
{
    if (trace_lock == NULL)
        trace_lock = thread_create_mutex();
    thread_wait_mutex(trace_lock);
    if (on && !trace_fp) {
        char path[1024];
        path_append_filename(path, usr_path, "usb_trace.txt");
        trace_fp = plat_fopen(path, "a");
        trace_t0 = plat_get_ticks();
        if (trace_fp) {
            char      stamp[32];
            time_t    now = time(NULL);
            struct tm tm  = *localtime(&now);
            strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
            fprintf(trace_fp, "\n=== USB trace started %s ===   time: real seconds since start | f = emulated 1 ms frame\n", stamp);
        }
    } else if (!on && trace_fp) {
        fprintf(trace_fp, "=== USB trace stopped ===\n");
        fclose(trace_fp);
        trace_fp = NULL;
    }
    usbn_trace_on = (trace_fp != NULL);
    thread_release_mutex(trace_lock);
}

void
usbn_trace(const char *fmt, ...)
{
    va_list ap;

    if (!usbn_trace_on)
        return;
    thread_wait_mutex(trace_lock);
    if (trace_fp) {
        fprintf(trace_fp, "%9.3f f%-8u ", (plat_get_ticks() - trace_t0) / 1000.0, usbn_frame_count);
        va_start(ap, fmt);
        vfprintf(trace_fp, fmt, ap);
        va_end(ap);
        fputc('\n', trace_fp);
        fflush(trace_fp);
    }
    thread_release_mutex(trace_lock);
}

static mutex_t              *lock;
static usbn_device_t        *devs[USBN_PORTS];    /* plugged in, emulation side   */
static usbn_device_t        *plug[USBN_PORTS];    /* waiting to be plugged in     */
static int                   unplug[USBN_PORTS];  /* waiting to be pulled out     */
static const usbn_hub_ops_t  *hub;        /* a hub's downstream ports, slots USBN_HUB_BASE on */
static void                  *hub_priv;

/* The controllers, one per role, each with its own block of slots. */
typedef struct root_t {
    const usbn_root_ops_t *ops;
    void                  *priv;
    int                    nports;
    int                    high_speed;
} root_t;
static root_t roots[USBN_ROLES];

int usbn_card_context;   /* the USB card is being made: its controller is the card's */
int usbn_layout;         /* (C) 2: the ports are assigned by name; 0: an older config */
char usbn_legacy_cfg[6][16];   /* (C) an older config's [USB] port1-port6 */

static void
bus_lock(void)
{
    if (lock == NULL)
        lock = thread_create_mutex();
    thread_wait_mutex(lock);
}

static void
bus_unlock(void)
{
    thread_release_mutex(lock);
}

static int
role_of(const void *priv)
{
    for (int r = 0; r < USBN_ROLES; r++) {
        if (roots[r].ops && (roots[r].priv == priv))
            return r;
    }
    return -1;
}

int
usbn_port_role(int slot)
{
    if ((slot < 0) || (slot >= USBN_PORTS))
        return -2;
    return (slot < USBN_HUB_BASE) ? (slot / USBN_ROOT_PORTS) : -1;
}

/* Release what is plugged into a slot (the bus is going away). */
static void
slot_release(int p)
{
    if (devs[p] && devs[p]->destroy)
        devs[p]->destroy(devs[p]);
    if (plug[p] && plug[p]->destroy)
        plug[p]->destroy(plug[p]);
    devs[p] = plug[p] = NULL;
    unplug[p]         = 0;
}

/* A controller comes (ops) or goes (NULL).  Each has its own ports: the
   card's when it is made by usb_card_reset(), else the southbridge's (the
   first, or the VIA 686's second).  Returns 1 when it has ports. */
int
usbn_set_root(const usbn_root_ops_t *ops, void *priv, int high_speed)
{
    int r;

    bus_lock();
    if (ops == NULL) {
        r = role_of(priv);
        if (r < 0) {
            bus_unlock();
            return 0;
        }
        for (int i = 0; i < USBN_ROOT_PORTS; i++)
            slot_release(r * USBN_ROOT_PORTS + i);
        memset(&roots[r], 0, sizeof(root_t));
        /* The last controller gone: the hub's ports with it. */
        int any = 0;
        for (int i = 0; i < USBN_ROLES; i++)
            any |= (roots[i].ops != NULL);
        if (!any) {
            for (int p = USBN_HUB_BASE; p < USBN_PORTS; p++)
                slot_release(p);
            hub      = NULL;
            hub_priv = NULL;
        }
        bus_unlock();
        usbn_trace("USB controller removed");
        return 1;
    }
    if (usbn_card_context)
        r = USBN_ROLE_CARD;
    else
        r = roots[USBN_ROLE_ONBOARD].ops ? USBN_ROLE_ONBOARD2 : USBN_ROLE_ONBOARD;
    if (roots[r].ops) {
        bus_unlock();
        pclog("USB: no place for another USB controller\n");
        return 0;
    }
    roots[r].ops        = ops;
    roots[r].priv       = priv;
    roots[r].nports     = 2;
    roots[r].high_speed = high_speed;
    bus_unlock();

    if (getenv("BOX86NEXT_USB_TRACE") && !usbn_trace_on)
        usbn_trace_enable(1);
    usbn_trace("USB controller fitted (%s): %s", high_speed ? "USB 2.0" : "USB 1.1",
               (r == USBN_ROLE_CARD) ? "the card" : ((r == USBN_ROLE_ONBOARD) ? "the motherboard's" : "the motherboard's second"));
    return 1;
}

/* The ports a controller has, when it is not two (the ALi M5237's three). */
void
usbn_set_root_ports(void *priv, int nports)
{
    int r = role_of(priv);

    if (r >= 0)
        roots[r].nports = (nports > USBN_ROOT_PORTS) ? USBN_ROOT_PORTS : nports;
}

int
usbn_present(void)
{
    for (int r = 0; r < USBN_ROLES; r++) {
        if (roots[r].ops)
            return 1;
    }
    return 0;
}

int
usbn_bus_high_speed(void)
{
    for (int r = 0; r < USBN_ROLES; r++) {
        if (roots[r].ops && roots[r].high_speed)
            return 1;
    }
    return 0;
}

int
usbn_port_high_speed(int slot)
{
    const int r = usbn_port_role(slot);

    if (r == -1) {
        /* Behind the hub: as fast as the controller it is on. */
        for (int p = 0; p < USBN_HUB_BASE; p++) {
            if (devs[p] && devs[p]->route)
                return usbn_port_high_speed(p);
        }
        return 0;
    }
    return (r >= 0) && roots[r].ops && roots[r].high_speed;
}

usbn_device_t *
usbn_port_device(int port)
{
    return ((port >= 0) && (port < USBN_PORTS)) ? devs[port] : NULL;
}

void
usbn_remote_wakeup(usbn_device_t *dev)
{
    for (int p = 0; p < USBN_PORTS; p++) {
        if (devs[p] != dev)
            continue;
        const int r = usbn_port_role(p);
        if (r >= 0) {
            if (roots[r].ops && roots[r].ops->resume)
                roots[r].ops->resume(roots[r].priv, p % USBN_ROOT_PORTS);
        } else if (hub && hub->resume)
            hub->resume(hub_priv, p - USBN_HUB_BASE);
        return;
    }
}

/* Emulation thread only, and without the lock: a hub that goes away does so
   from inside usbn_apply() (unplugged) or usbn_set_root() (the bus). */
void
usbn_set_hub(const usbn_hub_ops_t *ops, void *priv)
{
    hub      = ops;
    hub_priv = ops ? priv : NULL;
    usbn_trace(ops ? "a hub is on the bus" : "the hub is gone");
}

int
usbn_port_exists(int port)
{
    const int r = usbn_port_role(port);

    if (r == -1)
        return hub != NULL;
    return (r >= 0) && roots[r].ops && ((port % USBN_ROOT_PORTS) < roots[r].nports);
}

int
usbn_root_idle(usbn_device_t *dev)
{
    for (int p = 0; p < USBN_HUB_BASE; p++) {
        if (devs[p] == dev) {
            const root_t *rt = &roots[usbn_port_role(p)];
            return (rt->ops && rt->ops->idle) ? rt->ops->idle(rt->priv, p % USBN_ROOT_PORTS) : 0;
        }
    }
    return 0;
}

usbn_device_t *
usbn_route(usbn_device_t *d, uint8_t addr, int low_speed)
{
    if (d == NULL)
        return NULL;
    if (d->addr == addr)
        return d;
    return d->route ? d->route(d, addr, low_speed) : NULL;
}

double
usbn_bus_ns(int low_speed, int iso, int in, int len)
{
    /* Floor(3.167 + BitStuffTime(len)), BitStuffTime = 7 * 8 * len / 6. */
    double bits = (double) (int) (3.167 + ((7.0 * 8.0 * (double) len) / 6.0));

    if (low_speed)
        return (in ? 64060.0 : 64107.0) + (2.0 * 333.0) + ((in ? 676.67 : 667.0) * bits);
    if (iso)
        return (in ? 7268.0 : 6265.0) + (83.54 * bits);
    return 9107.0 + (83.54 * bits);
}

/* ------------------------------------------------------- port names --- */

static const char *const role_keys[USBN_ROLES] = { "onboard", "onboard2", "card" };

/* The config key of a slot: onboard_1, onboard2_2, card_1, hub_4. */
const char *
usbn_port_key(int slot, char *buf, int len)
{
    const int r = usbn_port_role(slot);

    if (r == -2)
        buf[0] = '\0';
    else if (r == -1)
        snprintf(buf, len, "hub_%d", slot - USBN_HUB_BASE + 1);
    else
        snprintf(buf, len, "%s_%d", role_keys[r], (slot % USBN_ROOT_PORTS) + 1);
    return buf;
}

int
usbn_port_from_key(const char *key)
{
    char buf[32];

    for (int p = 0; p < USBN_PORTS; p++) {
        if (!strcmp(usbn_port_key(p, buf, sizeof(buf)), key))
            return p;
    }
    return -1;
}

/* The slot assigned what ("keyboard", "mouse", "hub"), or -1. */
int
usbn_assigned(const char *what)
{
    for (int p = 0; p < USBN_PORTS; p++) {
        if (!strcmp(usbn_port_cfg[p], what))
            return p;
    }
    return -1;
}

/* An older config (no layout = 2): its port1-port6 were the one controller's
   two ports and the hub's four; the hub, keyboard and mouse went into the
   first free ports, in that order.  Written down once, as assignments, at
   the first hard reset with the controllers made. */
void
usbn_migrate(int hub_fitted, int keyboard, int mouse)
{
    int base = -1;

    if (usbn_layout >= 2)
        return;
    for (int r = 0; (r < USBN_ROLES) && (base < 0); r++) {
        if (roots[r].ops)
            base = r * USBN_ROOT_PORTS;
    }
    if (base < 0)
        return;
    for (int i = 0; i < 6; i++) {
        const int p = (i < 2) ? (base + i) : (USBN_HUB_BASE + i - 2);
        if (usbn_legacy_cfg[i][0] && !usbn_port_cfg[p][0])
            snprintf(usbn_port_cfg[p], sizeof(usbn_port_cfg[p]), "%s", usbn_legacy_cfg[i]);
        usbn_legacy_cfg[i][0] = '\0';
    }
    const char *what[3] = { hub_fitted ? "hub" : NULL, keyboard ? "keyboard" : NULL, mouse ? "mouse" : NULL };
    for (int w = 0; w < 3; w++) {
        if (!what[w] || (usbn_assigned(what[w]) >= 0))
            continue;
        for (int p = 0; p < USBN_PORTS; p++) {
            const int r = usbn_port_role(p);
            /* Root ports of the controller that had them, then (not for the
               hub itself) the hub's. */
            const int ok = (r == -1) ? (hub_fitted && (w > 0)) : ((p >= base) && (p < base + 2));
            if (ok && !usbn_port_cfg[p][0]) {
                snprintf(usbn_port_cfg[p], sizeof(usbn_port_cfg[p]), "%s", what[w]);
                break;
            }
        }
    }
    usbn_layout = 2;
    usbn_trace("an older config's USB ports written down as assignments");
}

/* Called by each controller once per frame, on the emulation thread: its
   own ports, and (from the first controller) the hub's. */
void
usbn_apply(void *priv)
{
    const int r = role_of(priv);
    int       first = 1;

    if (r < 0)
        return;
    for (int i = 0; i < r; i++)
        first &= (roots[i].ops == NULL);
    if (first)
        usbn_frame_count++;
    bus_lock();
    for (int p = 0; p < USBN_PORTS; p++) {
        const int pr = usbn_port_role(p);
        if (!((pr == r) || ((pr == -1) && first)))
            continue;
        if (unplug[p]) {
            unplug[p] = 0;
            if (devs[p]) {
                usbn_device_t *d = devs[p];
                char           key[32];
                if (pr >= 0)
                    roots[r].ops->disconnect(roots[r].priv, p % USBN_ROOT_PORTS);
                else if (hub)
                    hub->disconnect(hub_priv, p - USBN_HUB_BASE);
                devs[p] = NULL;
                pclog("USB: %s unplugged from %s\n", d->name, usbn_port_key(p, key, sizeof(key)));
                usbn_trace("%s: %s unplugged", key, d->name);
                if (d->destroy)
                    d->destroy(d);
            }
        }
        if (plug[p] && (devs[p] == NULL) && ((pr >= 0) || hub)) {
            char key[32];
            devs[p] = plug[p];
            plug[p] = NULL;
            pclog("USB: %s plugged into %s\n", devs[p]->name, usbn_port_key(p, key, sizeof(key)));
            usbn_trace("%s: %s plugged in (%s speed%s)", key, devs[p]->name,
                       (devs[p]->speed == USBN_SPEED_HIGH) ? "high" : ((devs[p]->speed == USBN_SPEED_LOW) ? "low" : "full"),
                       usbn_port_high_speed(p) ? "" : ", on a USB 1.1 port");
            if (pr >= 0)
                roots[r].ops->connect(roots[r].priv, p % USBN_ROOT_PORTS, devs[p]);
            else
                hub->connect(hub_priv, p - USBN_HUB_BASE, devs[p]);
        }
        if (devs[p] && devs[p]->frame)
            devs[p]->frame(devs[p]);
    }
    bus_unlock();
}

int
usbn_port_busy(int port)
{
    int busy;

    if (!usbn_port_exists(port))
        return 1;
    bus_lock();
    busy = (devs[port] != NULL && !unplug[port]) || (plug[port] != NULL);
    bus_unlock();
    return busy;
}

const char *
usbn_port_name(int port, char *buf, int len)
{
    buf[0] = '\0';
    if ((port < 0) || (port >= USBN_PORTS))
        return buf;
    bus_lock();
    if (plug[port])
        snprintf(buf, len, "%s", plug[port]->name);
    else if (devs[port] && !unplug[port])
        snprintf(buf, len, "%s", devs[port]->name);
    bus_unlock();
    return buf;
}

int
usbn_attach(int port, usbn_device_t *d)
{
    if (!d || !usbn_port_exists(port) || usbn_port_busy(port))
        return 0;
    bus_lock();
    plug[port] = d;
    bus_unlock();
    return 1;
}

/* Pull a device out.  What the port is assigned (usbn_port_cfg) is kept:
   the UI forgets it itself when the user disconnects a device. */
void
usbn_detach(int port)
{
    if ((port < 0) || (port >= USBN_PORTS))
        return;
    bus_lock();
    if (plug[port]) {
        if (plug[port]->destroy)
            plug[port]->destroy(plug[port]);
        plug[port] = NULL;
    } else if (devs[port])
        unplug[port] = 1;
    bus_unlock();
}

/* Plug back in the host devices the config has on the ports. */
void
usbn_restore_ports(void)
{
    for (int p = 0; p < USBN_PORTS; p++) {
        unsigned vid, pid;
        char     err[512], key[32];

        if (sscanf(usbn_port_cfg[p], "%x:%x", &vid, &pid) != 2)
            continue;
        /* Not there yet (a hub port before the hub), or already planned. */
        if (!usbn_port_exists(p) || usbn_port_busy(p))
            continue;
        usbn_device_t *d = usbn_host_open((uint16_t) vid, (uint16_t) pid, err, sizeof(err));
        if (d)
            usbn_attach(p, d);
        else
            pclog("USB: %s: %04X:%04X not reattached: %s\n", usbn_port_key(p, key, sizeof(key)), vid, pid, err);
    }
}
