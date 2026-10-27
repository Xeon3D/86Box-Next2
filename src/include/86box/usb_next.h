/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             A working USB stack: a PCI UHCI host controller (USB 1.1) with
 *             two root-hub ports, and devices that plug into them -- for now
 *             host devices passed through with libusb.  Upstream's usb.c is
 *             the southbridges' register stub and is left alone.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#ifndef EMU_USB_NEXT_H
#define EMU_USB_NEXT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The bus's slots: the root ports, then the downstream ports of a hub (the
   4-port hub, usb_hub.c, on a root port; they exist while it does). */
#define USBN_ROOT_PORTS 2
#define USBN_HUB_PORTS  4
#define USBN_PORTS      (USBN_ROOT_PORTS + USBN_HUB_PORTS)

#define USB_PID_SETUP   0x2d
#define USB_PID_IN      0x69
#define USB_PID_OUT     0xe1

/* packet() returns the bytes moved (>= 0), or one of these. */
#define USBN_NAK        (-1)  /* not now: the TD stays active and is retried  */
#define USBN_STALL      (-2)
#define USBN_NODEV      (-3)
#define USBN_BABBLE     (-4)
#define USBN_IOERROR    (-5)

#define USBN_SPEED_LOW  0
#define USBN_SPEED_FULL 1
#define USBN_SPEED_HIGH 2

typedef struct usbn_device_t {
    char    name[128];
    int     speed;
    uint8_t addr;           /* set by the guest's SET_ADDRESS */
    int     fs_view;        /* a high-speed device shown at full speed (on a USB 1.1 port) */
    void   *priv;

    /* One USB transaction.  Called from the emulation thread. */
    int  (*packet)(struct usbn_device_t *dev, uint8_t pid, uint8_t ep, uint8_t *buf, int len);
    /* One isochronous transaction (optional): once per frame, never NAKed
       or retried.  IN returns the bytes delivered (0 when there are none),
       OUT the bytes taken. */
    int  (*iso)(struct usbn_device_t *dev, uint8_t pid, uint8_t ep, uint8_t *buf, int len);
    void (*reset)(struct usbn_device_t *dev);    /* port reset */
    void (*frame)(struct usbn_device_t *dev);    /* once per 1 ms frame (optional) */
    void (*destroy)(struct usbn_device_t *dev);  /* unplugged */
    /* A hub (optional): the enabled device downstream answering at addr. */
    struct usbn_device_t *(*route)(struct usbn_device_t *dev, uint8_t addr, int low_speed);
} usbn_device_t;

/* ---- the ports (usb_bus.c) ---- */
/* The controller the ports are wired to.  connect() / disconnect() run on the
   emulation thread; the bus owns the device and destroys it after
   disconnect(). */
typedef struct usbn_root_ops_t {
    void (*connect)(void *priv, int port, usbn_device_t *dev);
    void (*disconnect)(void *priv, int port);
    void (*resume)(void *priv, int port);   /* the device signals remote wakeup (optional) */
    int  (*idle)(void *priv, int port);     /* nothing reaches the port: its device is suspended (optional) */
} usbn_root_ops_t;

/* The hub's downstream ports (bus slots USBN_ROOT_PORTS on), through the
   bridge to upstream's usb_hub.c (usb_devbridge.c). */
typedef struct usbn_hub_ops_t {
    void (*connect)(void *priv, int hub_port, usbn_device_t *dev);
    void (*disconnect)(void *priv, int hub_port);
    void (*resume)(void *priv, int hub_port);
} usbn_hub_ops_t;

/* A controller takes the ports (returns 0 when another one has them: the
   first one keeps them -- a motherboard's own, made with the chipset, before
   any card), or gives them back (ops NULL, only the one that has them). */
extern int            usbn_set_root(const usbn_root_ops_t *ops, void *priv, int high_speed);
extern void           usbn_apply(void *priv);       /* once per frame, from the root (others: no-op) */
extern int            usbn_bus_high_speed(void);    /* the root takes USB 2.0 devices */
extern usbn_device_t *usbn_port_device(int port);
/* A hub came (ops) or went (NULL): its downstream ports are bus slots. */
extern void           usbn_set_hub(const usbn_hub_ops_t *ops, void *priv);
extern int            usbn_port_exists(int port);
/* The device on a root port is suspended (its port, or the whole bus). */
extern int            usbn_root_idle(usbn_device_t *dev);
/* A root port's device, or one behind the hub on it, answering at addr. */
extern usbn_device_t *usbn_route(usbn_device_t *root_dev, uint8_t addr, int low_speed);
/* Hand the ports from one controller to another without unplugging anything
   (a southbridge turned the owner's function off).  Returns 0 unless from
   has them. */
extern int            usbn_move_root(void *from, const usbn_root_ops_t *ops, void *priv);
extern void          *usbn_root_owner(void);
/* A device asks its port to resume (remote wakeup); emulation thread. */
extern void           usbn_remote_wakeup(usbn_device_t *dev);
/* One transaction's bus time in ns, data packet of len bytes with worst-case
   bit stuffing (USB 1.1 5.11.3; Host_Delay 0, Hub_LS_Setup 333 ns). */
extern double         usbn_bus_ns(int low_speed, int iso, int in, int len);

/* ---- the host controller cards (usb_uhci.c, usb_ehci.c) ---- */
extern int usb_card_type;                   /* (C) USB controller card, 0 = none */

extern void        usb_card_reset(void);
extern const char *usb_card_get_internal_name(int card);
extern int         usb_card_get_from_internal_name(const char *s);
extern const char *usb_card_get_name(int card);
extern int         usb_card_count(void);

/* Plug or unplug a device; safe from the UI thread.  The controller takes
   ownership of dev.  Returns 0 when there is no controller or the port is
   taken. */
extern int  usbn_present(void);
extern int  usbn_attach(int port, usbn_device_t *dev);
extern void usbn_detach(int port);
extern int  usbn_port_busy(int port);
extern const char *usbn_port_name(int port, char *buf, int len);

/* A UHCI controller as the companion of an EHCI one: it is function 0 of the
   EHCI card, raises INTA on the card's slot, and is told which of the shared
   ports it currently owns. */
typedef struct uhci_t uhci_t;

/* Who a controller says it is on the PCI bus. */
typedef struct usbn_pci_id_t {
    uint16_t vendor, device;
    uint8_t  revision;
    uint16_t sub_vendor, sub_device;
} usbn_pci_id_t;

extern uhci_t *uhci_companion_create(uint8_t *card_slot, const usbn_pci_id_t *id);

/* A UHCI built into a southbridge: the PIIX3's function 2.  Its PCI
   configuration goes through uhci_pci_read/write (as function 0), its
   interrupt is PIRQD (LEGSUP permitting), and its legacy keyboard/mouse
   support (LEGSUP, C0h: traps on ports 60h/64h, an SMI on the USB interrupt,
   the A20GATE pass-through) reports each SMI event through legacy_smi (event
   1) -- the southbridge decides whether SMI# is asserted -- and, with event 0,
   that LEGSUP has no enabled event left.  It takes the USB ports. */
extern uhci_t *uhci_onboard_create(const usbn_pci_id_t *id, void (*legacy_smi)(void *priv, int event), void *priv);
extern void    uhci_onboard_close(uhci_t *dev);
extern void    uhci_onboard_reset(uhci_t *dev);
extern void    uhci_companion_close(uhci_t *dev);
extern void    uhci_companion_reset(uhci_t *dev);
extern void    uhci_route_port(uhci_t *dev, int port, usbn_device_t *d);   /* NULL: not ours now */
extern uint8_t uhci_pci_read(int func, int addr, int len, void *priv);
extern void    uhci_pci_write(int func, int addr, int len, uint8_t val, void *priv);

/* A UHCI whose PCI configuration a southbridge owns (VIA's, through
   usb_chipset.c): the southbridge maps its I/O, routes its interrupt and does
   LEGSUP itself (usb_legacy.c). */
extern uhci_t *uhci_chipset_create(void);
extern void    uhci_chipset_close(uhci_t *dev);
extern void    uhci_chipset_reset(uhci_t *dev);
extern void    uhci_chipset_set_io(uhci_t *dev, uint16_t base, int enable);
extern void    uhci_chipset_set_irq(uhci_t *dev, void (*set_irq)(void *priv, int level), void *priv);
extern void    uhci_chipset_set_bus_master(uhci_t *dev, int enable);
extern void    uhci_chipset_set_babble_keep_port(uhci_t *dev, int keep);
extern void    uhci_chipset_set_present(uhci_t *dev, int present);
extern int     uhci_chipset_present(const uhci_t *dev);

/* ---- the OHCI (usb_ohci.c) ----
   Its 4 KB register window, bus mastering, INT and SMI outputs are set by
   whoever owns its PCI configuration (usb_chipset.c, the FireLink card). */
typedef struct ohci_t ohci_t;

extern ohci_t  *ohci_core_create(void);   /* takes the USB ports if free */
extern void     ohci_core_close(ohci_t *dev);
extern void     ohci_core_reset(ohci_t *dev);
extern void     ohci_core_set_window(ohci_t *dev, uint32_t base, int enable);
extern void     ohci_core_set_irq(ohci_t *dev, void (*set_irq)(void *priv, int level), void *priv);
extern void     ohci_core_set_smi(ohci_t *dev, void (*set_smi)(void *priv, int level), void *priv);
extern void     ohci_core_set_bus_master(ohci_t *dev, int enable, int hardwired);
extern void     ohci_core_set_chip_flags(ohci_t *dev, uint32_t flags);   /* usb.h's OHCI_CHIP_* */
extern void     ohci_core_set_ndp(ohci_t *dev, int ndp, int ports);
extern uint32_t ohci_core_read(ohci_t *dev, uint32_t off);
extern void     ohci_core_write(ohci_t *dev, uint32_t off, uint32_t val);

/* ---- the USB activity trace (usb_bus.c) ----
   Off unless switched on (the USB menu, or BOX86NEXT_USB_TRACE=1): port events,
   control requests and transfers, each with real time and emulated frame,
   written to usb_trace.txt beside the machine's 86box.cfg. */
extern int      usbn_trace_on;
extern uint32_t usbn_frame_count;
extern void     usbn_trace_enable(int on);
extern void     usbn_trace(const char *fmt, ...);
extern uint32_t usbn_ms(void);     /* real time, milliseconds */

/* ---- a high-speed device at full speed (usb_speed.c) ---- */
extern int  usbn_fs_maxp(int type, int hs_maxp);
extern void usbn_config_to_full_speed(uint8_t *desc, int len);

/* ---- host passthrough (usb_host.c) ---- */
typedef struct usbn_host_info_t {
    uint16_t vid, pid;
    uint8_t  bus, port_path[8], port_depth;
    int      speed;
    char     desc[128];
} usbn_host_info_t;

extern int            usbn_host_available(void);   /* built with libusb */
extern int            usbn_host_uses_usbdk(void);  /* Windows: capture any device through UsbDk */
extern int            usbn_host_list(usbn_host_info_t *out, int max);
extern usbn_device_t *usbn_host_open(uint16_t vid, uint16_t pid, char *err, int errlen);

/* ---- the emulated keyboard and mouse (usb_hid.c) ----
   The keyboard type and mouse type "usb": a HID boot keyboard and a HID boot
   mouse, separate devices, each on the first free port.  The host's input
   reaches the guest only while it drives them (configured, and the interrupt
   endpoint polled within the last 500 ms). */
extern usbn_device_t *usbn_hid_create_keyboard(int layout);   /* KBD_101_KEY ... KBD_ABNT2 */
extern usbn_device_t *usbn_hid_create_mouse(int buttons);     /* 2, 3, 4 wheel, 5 five + wheel, 6 two wheels */
extern void           usbn_hid_attach(void);     /* at the hard reset, after the controller */
extern int            usbn_hid_keyboard_active(void);
extern int            usbn_hid_mouse_active(void);
extern void           usbn_hid_key(int down, uint16_t scan);   /* any thread; 86Box's key number */

/* ---- upstream's USB devices on our bus (usb_devbridge.c) and the hubs (usb_hub2.c) ---- */
struct usb_dev_t;
extern void usbn_bridge_set_high_speed(struct usb_dev_t *udev);               /* a high-speed device */
extern int  usbn_bridge_running_high_speed(const struct usb_dev_t *udev);     /* ... and on a high-speed port now */
extern int  usbn_bridge_child_high_speed(const struct usb_dev_t *shim);       /* behind a hub: a high-speed device */
extern void usbn_bridge_hub_speed(struct usb_dev_t *hub, int high_speed);    /* what the hub's children see */

extern int         usbn_hub_type;   /* (C) Settings > Other peripherals: 0 none */
extern int         usbn_hub_count(void);
extern const char *usbn_hub_get_name(int hub);
extern const char *usbn_hub_get_internal_name(int hub);
extern int         usbn_hub_get_from_internal_name(const char *s);
extern void        usbn_hub_reset(void);

/* Remembered attachments ("vvvv:pppp" per port), for the config file. */
extern char usbn_port_cfg[USBN_PORTS][16];
extern void usbn_restore_ports(void);

#ifdef __cplusplus
}
#endif

#endif /*EMU_USB_NEXT_H*/
