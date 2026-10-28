/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             A PCI USB 1.1 host controller: Universal Host Controller
 *             Interface (UHCI, Intel's design, revision 1.1), presented as
 *             the VIA VT83C572 -- the UHCI function VIA used in its
 *             southbridges and on add-in cards -- or as the Intel PIIX4 USB
 *             function (82371AB, 8086:7112).  Windows 98 SE, ME, 2000 and XP
 *             and Linux drive both with their built-in UHCI drivers.  Also
 *             built into a southbridge: the PIIX3's function 2 (82371SB,
 *             8086:7020), with its legacy keyboard/mouse support (LEGSUP),
 *             which lets a BIOS drive a USB keyboard through SMIs.
 *
 *             Two root-hub ports.  The schedule (frame list, queue heads,
 *             transfer descriptors) is walked once per emulated millisecond.
 *             A device that cannot answer yet -- a passed-through host device
 *             whose transfer is still in flight -- NAKs, and the TD is simply
 *             retried on a later frame, which is exactly what UHCI does with
 *             a NAK; that keeps the host side asynchronous without the
 *             controller knowing.
 *
 *             Reference: Intel, "Universal Host Controller Interface (UHCI)
 *             Design Guide", revision 1.1, March 1996; Intel, "82371FB (PIIX)
 *             and 82371SB (PIIX3) PCI ISA IDE Xcelerator", section 2.4 (the
 *             USB function's registers, LEGSUP at 2.4.14) and its
 *             specification update 297658-004 (revision IDs).
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
#include <86box/io.h>
#include <86box/mem.h>
#include <86box/dma.h>
#include <86box/pci.h>
#include <86box/timer.h>
#include <86box/thread.h>
#include <86box/plat_unused.h>
#include <86box/usb_next.h>

#define UHCI_PORTS 2   /* root ports */

#ifdef ENABLE_USB_UHCI_LOG
int usb_uhci_do_log = ENABLE_USB_UHCI_LOG;

static void
uhci_log(const char *fmt, ...)
{
    va_list ap;

    if (usb_uhci_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define uhci_log(fmt, ...)
#endif

/* Registers (I/O, 32 bytes at BAR4). */
#define REG_USBCMD    0x00
#define REG_USBSTS    0x02
#define REG_USBINTR   0x04
#define REG_FRNUM     0x06
#define REG_FLBASEADD 0x08
#define REG_SOFMOD    0x0c
#define REG_PORTSC1   0x10

#define CMD_RS        0x0001
#define CMD_HCRESET   0x0002
#define CMD_GRESET    0x0004
#define CMD_EGSM      0x0008
#define CMD_FGR       0x0010
#define CMD_SWDBG     0x0020
#define CMD_CF        0x0040
#define CMD_MAXP      0x0080

#define STS_USBINT    0x0001
#define STS_ERRINT    0x0002
#define STS_RD        0x0004
#define STS_HSE       0x0008
#define STS_HCPE      0x0010
#define STS_HCHALTED  0x0020

#define INTR_CRC      0x0001
#define INTR_RESUME   0x0002
#define INTR_IOC      0x0004
#define INTR_SPD      0x0008

#define PORT_CCS      0x0001
#define PORT_CSC      0x0002
#define PORT_PED      0x0004
#define PORT_PEDC     0x0008
#define PORT_LS_J     0x0010  /* line status D+ (full speed idle) */
#define PORT_LS_K     0x0020  /* line status D- (low speed idle)  */
#define PORT_RD       0x0040
#define PORT_ALWAYS1  0x0080
#define PORT_LSDA     0x0100
#define PORT_PR       0x0200
#define PORT_SUSP     0x1000
#define PORT_RWC      (PORT_CSC | PORT_PEDC)
#define PORT_RW       (PORT_PED | PORT_RD | PORT_PR | PORT_SUSP)

/* LEGSUP (PCI C0h-C1h): legacy keyboard/mouse support (PIIX3 2.4.14). */
#define LEG_60REN      0x0001   /* trap/SMI on a read of 60h      */
#define LEG_60WEN      0x0002   /*                 write of 60h   */
#define LEG_64REN      0x0004   /*                 read of 64h    */
#define LEG_64WEN      0x0008   /*                 write of 64h   */
#define LEG_USBSMIEN   0x0010   /* SMI on the USB interrupt       */
#define LEG_A20PTEN    0x0020   /* A20GATE pass-through           */
#define LEG_PSS        0x0040   /* pass-through in progress (RO)  */
#define LEG_SMIEPTE    0x0080   /* SMI at its end                 */
#define LEG_TBY60R     0x0100   /* trap statuses (RWC): 60h read, ... */
#define LEG_USBIRQS    0x1000   /* the USB interrupt is active (RO) */
#define LEG_USBPIRQDEN 0x2000   /* the USB interrupt goes to PIRQD */
#define LEG_A20PTS     0x8000   /* pass-through ended (RWC)       */
#define LEG_RW         0x20bf
#define LEG_RWC        0x8f00
#define LEG_TRAPS      (LEG_60REN | LEG_60WEN | LEG_64REN | LEG_64WEN)

/* Transfer descriptor bits. */
#define LINK_T        0x00000001
#define LINK_QH       0x00000002
#define LINK_VF       0x00000004

#define TD_ACTLEN     0x000007ff
#define TD_BITSTUFF   0x00020000
#define TD_CRCTO      0x00040000
#define TD_NAK        0x00080000
#define TD_BABBLE     0x00100000
#define TD_DBUF       0x00200000
#define TD_STALLED    0x00400000
#define TD_ACTIVE     0x00800000
#define TD_IOC        0x01000000
#define TD_ISO        0x02000000
#define TD_LS         0x04000000
#define TD_CERR       0x18000000
#define TD_SPD        0x20000000

/* What processing a TD came to. */
enum {
    TD_DONE,      /* completed; a queue moves on to the next TD             */
    TD_RETRY,     /* still active (NAK, or already inactive and skipped)    */
    TD_STOP,      /* error or short packet: the queue stops here this frame */
    TD_EOF,       /* the controller halted: the frame ends here            */
    TD_NO_TIME,   /* past the preSOF point: the frame ends, the TD waits    */
};

/* End of frame (UHCI 1.1 1.3.1): EOF2 is 2 bit times before the SOF; a
   device still sending at EOF2 babbles. */
#define UHCI_EOF2_NS  (2.0 * 1000.0 / 12.0)
#define UHCI_SOF_NS   (35.0 * 1000.0 / 12.0)

typedef struct uhci_t {
    uint8_t  pci_conf[256];
    uint8_t  pci_slot;
    uint8_t *slotp;          /* the card's slot: our own, or the EHCI card's */
    uint8_t  irq_pin;        /* INTA on a card; 0 onboard (slotp: PIRQD)     */
    uint8_t  iirq;           /* onboard: the chipset's PIRQD                 */
    int      companion;      /* function 0 of an EHCI card                   */
    int      onboard;        /* a southbridge's function: LEGSUP works       */
    uint8_t  irq_state;
    int      irq_level;      /* the USB interrupt, before LEGSUP routes it   */

    /* Legacy keyboard/mouse support (onboard). */
    void   (*legacy_smi)(void *priv, int event);
    void    *legacy_priv;
    int      claiming;       /* the traps take port 60h/64h cycles          */
    int      a20_state;      /* the A20GATE pass-through: 0 none, 1 after D1, 2 after the data */
    uint16_t io_base;
    int      io_mapped;

    uint16_t cmd, sts, intr, frnum;
    uint32_t flbase;
    uint8_t  sofmod;
    uint16_t portsc[UHCI_PORTS];

    usbn_device_t *dev[UHCI_PORTS];   /* what this controller sees on each port */

    pc_timer_t frame_timer;
    uint8_t    pending_sts;   /* status bits raised during this frame */
    double     ns_left;       /* bus time left in the frame being run */
    int        executed;      /* a TD of this frame went on the wire  */
    int        cur_port;      /* the port of the device being talked to */

    /* Software debug mode: the walk stops after each TD and goes on from
       there when Run/Stop is set again (UHCI 1.1 2.1.1). */
    int        dbg_resume;
    uint32_t   dbg_link;

    /* Southbridge-driven (usb_chipset.c): the southbridge owns the PCI
       configuration and routes the interrupt. */
    int        chipset;
    void     (*set_irq)(void *priv, int level);
    void      *irq_priv;
    int        irq_out;
    int        bus_master;
    int        babble_keep;   /* VIA Rx40 bit 6: a babbling port stays enabled */
    int        present;
} uhci_t;

int        usb_card_type = 0;
static int usb_card_more[USBN_CARDS - 1];   /* cards 2 to 4 */

int
usbn_card_get(int i)
{
    if ((i < 0) || (i >= USBN_CARDS))
        return 0;
    return i ? usb_card_more[i - 1] : usb_card_type;
}

void
usbn_card_set(int i, int type)
{
    if (i == 0)
        usb_card_type = type;
    else if ((i > 0) && (i < USBN_CARDS))
        usb_card_more[i - 1] = type;
}

/* ------------------------------------------------------------- the IRQ --- */

static uint16_t
legsup(const uhci_t *dev)
{
    return dev->pci_conf[0xc0] | (dev->pci_conf[0xc1] << 8);
}

static void
set_legsup(uhci_t *dev, uint16_t v)
{
    dev->pci_conf[0xc0] = v & 0xff;
    dev->pci_conf[0xc1] = v >> 8;
}

/* A legacy event the southbridge may turn into an SMI. */
static void
uhci_legacy_event(uhci_t *dev)
{
    usbn_trace("UHCI: legacy SMI event, LEGSUP %04X", legsup(dev));
    if (dev->legacy_smi)
        dev->legacy_smi(dev->legacy_priv, 1);
}

/* Whether LEGSUP still has an enabled event: a trap status with its enable,
   the USB interrupt with USBSMIEN, the end of a pass-through with SMIEPTE. */
static int
uhci_legacy_pending(const uhci_t *dev)
{
    const uint16_t leg = legsup(dev);

    return ((leg >> 8) & leg & LEG_TRAPS) || ((leg & LEG_USBIRQS) && (leg & LEG_USBSMIEN)) ||
           ((leg & LEG_A20PTS) && (leg & LEG_SMIEPTE));
}

/* None left: the southbridge's status may follow (the PIIX4's GLBSTS bit 1). */
static void
uhci_legacy_settle(uhci_t *dev)
{
    if (dev->legacy_smi && !uhci_legacy_pending(dev))
        dev->legacy_smi(dev->legacy_priv, 0);
}

static void
uhci_update_irq(uhci_t *dev)
{
    int level = 0;

    if ((dev->sts & STS_USBINT) && (dev->intr & (INTR_IOC | INTR_SPD)))
        level = 1;
    if ((dev->sts & STS_ERRINT) && (dev->intr & INTR_CRC))
        level = 1;
    if ((dev->sts & STS_RD) && (dev->intr & INTR_RESUME))
        level = 1;
    if (dev->sts & (STS_HSE | STS_HCPE))
        level = 1;

    if (dev->onboard) {
        uint16_t leg = legsup(dev);

        /* An SMI when the interrupt comes up with USBSMIEN on. */
        if (level && !dev->irq_level && (leg & LEG_USBSMIEN))
            uhci_legacy_event(dev);
        dev->irq_level = level;
        set_legsup(dev, level ? (leg | LEG_USBIRQS) : (leg & ~LEG_USBIRQS));
        if (!level)
            uhci_legacy_settle(dev);
        if (!(leg & LEG_USBPIRQDEN))
            level = 0;
    }

    if (dev->chipset) {
        if (level != dev->irq_out) {
            dev->irq_out = level;
            if (dev->set_irq)
                dev->set_irq(dev->irq_priv, level);
        }
    } else if (level)
        pci_set_irq(*dev->slotp, dev->irq_pin, &dev->irq_state);
    else
        pci_clear_irq(*dev->slotp, dev->irq_pin, &dev->irq_state);
}

/* ------------------------------------------------ legacy support (LEGSUP) --- */

/* The A20GATE pass-through sequence (A20PTEN): write 64h D1h (one or more),
   write 60h data, read 64h (none or more), write 64h FFh.  Its cycles are not
   trapped; anything else ends it and is handled as usual.  Returns 1 for a
   cycle of the sequence. */
static int
uhci_a20_pass_through(uhci_t *dev, uint16_t port, uint8_t write, uint8_t val)
{
    uint16_t leg = legsup(dev);

    switch (dev->a20_state) {
        case 0:
            if (write && (port == 0x64) && (val == 0xd1)) {
                dev->a20_state = 1;
                set_legsup(dev, leg | LEG_PSS);
                return 1;
            }
            return 0;
        case 1:
            if (write && (port == 0x64) && (val == 0xd1))
                return 1;
            if (write && (port == 0x60)) {
                dev->a20_state = 2;
                return 1;
            }
            break;
        case 2:
            if (!write && (port == 0x64))
                return 1;
            if (write && (port == 0x64) && (val == 0xff)) {
                dev->a20_state = 0;
                set_legsup(dev, (leg & ~LEG_PSS) | LEG_A20PTS);
                if (leg & LEG_SMIEPTE)
                    uhci_legacy_event(dev);
                return 1;
            }
            break;
        default:
            break;
    }
    dev->a20_state = 0;   /* a deviation: the sequence ends */
    set_legsup(dev, leg & ~LEG_PSS);
    return 0;
}

/* A cycle to port 60h or 64h.  A trapped one never reaches the keyboard
   controller: the PIIX3 keeps it from the bus (a read floats) and asks for the
   SMI, which the CPU takes once the instruction is done; the handler then does
   the access itself, or answers in the saved EAX.  Intel's handler (VS440FX)
   needs that: it gives up on a command when it finds the controller's input
   buffer already full, so a guest's command that got through was lost, and
   the controller's answer then raised IRQ1 and was taken for a key.  Cycles of
   the A20GATE pass-through and untrapped ones reach the controller as usual. */
static int
uhci_legacy_claim(uint16_t port, uint8_t write, uint8_t val, void *priv)
{
    uhci_t   *dev = (uhci_t *) priv;
    uint16_t  leg;
    int       bit;

    if ((port != 0x60) && (port != 0x64))
        return 0;
    leg = legsup(dev);
    bit = ((port == 0x64) ? 2 : 0) + (write ? 1 : 0);
    if ((leg & LEG_A20PTEN) && uhci_a20_pass_through(dev, port, write, val))
        return 0;
    if (!(leg & (1 << bit)))
        return 0;
    set_legsup(dev, legsup(dev) | (LEG_TBY60R << bit));
    uhci_legacy_event(dev);
    return 1;
}

/* The traps are only in place while something needs them. */
static void
uhci_legacy_remap(uhci_t *dev)
{
    const int on = !!(legsup(dev) & (LEG_TRAPS | LEG_A20PTEN));

    if (on != dev->claiming) {
        io_set_claim(0x60, on ? uhci_legacy_claim : NULL, dev);
        io_set_claim(0x64, on ? uhci_legacy_claim : NULL, dev);
    }
    dev->claiming = on;
}

static void
uhci_legsup_write(uhci_t *dev, int addr, uint8_t val)
{
    const uint16_t old   = legsup(dev);
    const int      shift = (addr & 1) ? 8 : 0;
    const uint16_t mask  = 0xff << shift;
    const uint16_t v     = val << shift;
    uint16_t       leg;

    leg = (old & ~(LEG_RW & mask)) | (v & LEG_RW & mask);
    leg &= ~(v & LEG_RWC);
    if (!(leg & LEG_A20PTEN)) {   /* turning it off ends a sequence at once */
        dev->a20_state = 0;
        leg &= ~LEG_PSS;
    }
    set_legsup(dev, leg);
    if (leg != old)
        usbn_trace("UHCI: LEGSUP %04X -> %04X", old, leg);
    uhci_legacy_remap(dev);
    /* USBSMIEN set while the interrupt is up: that is an SMI too. */
    if (dev->irq_level && (leg & LEG_USBSMIEN) && !(old & LEG_USBSMIEN))
        uhci_legacy_event(dev);
    uhci_update_irq(dev);   /* USBPIRQDEN may have changed */
    uhci_legacy_settle(dev);
}

/* --------------------------------------------------------------- ports --- */

static void
uhci_port_connect(uhci_t *dev, int p, usbn_device_t *d)
{
    /* A USB 1.1 port: a high-speed device is shown at full speed. */
    d->fs_view     = (d->speed == USBN_SPEED_HIGH);
    dev->dev[p]    = d;
    dev->portsc[p] = (dev->portsc[p] & ~(PORT_LSDA | PORT_LS_J | PORT_LS_K | PORT_PED)) | PORT_CCS | PORT_CSC;
    if (d->speed == USBN_SPEED_LOW)
        dev->portsc[p] |= PORT_LSDA | PORT_LS_K;
    else
        dev->portsc[p] |= PORT_LS_J;
    if (dev->cmd & CMD_EGSM)
        dev->sts |= STS_RD;
}

static void
uhci_port_disconnect(uhci_t *dev, int p)
{
    usbn_device_t *d = dev->dev[p];

    if (d == NULL)
        return;
    dev->dev[p] = NULL;
    if (dev->portsc[p] & PORT_PED)
        dev->portsc[p] |= PORT_PEDC;
    dev->portsc[p] = (dev->portsc[p] & ~(PORT_CCS | PORT_PED | PORT_LSDA | PORT_LS_J | PORT_LS_K)) | PORT_CSC;
}

static usbn_device_t *
uhci_find_device(uhci_t *dev, uint8_t addr, int ls)
{
    for (int p = 0; p < UHCI_PORTS; p++) {
        usbn_device_t *d = dev->dev[p];
        if (d && (dev->portsc[p] & PORT_PED) && !(dev->portsc[p] & (PORT_PR | PORT_SUSP)) &&
            ((d = usbn_route(d, addr, ls)) != NULL)) {   /* the device, or one behind its hub */
            dev->cur_port = p;
            return d;
        }
    }
    return NULL;
}

/* -------------------------------------------------------- the schedule --- */

/* A transaction's bus time comes off the frame.  Returns 1 when the device
   was still sending at EOF2: an EOF babble, which disables its port (unless
   the southbridge keeps babbling ports, VIA Rx40 bit 6) (UHCI 1.1 1.3.1,
   2.1.7). */
static int
uhci_charge(uhci_t *dev, int ls, int iso, int in, int len)
{
    dev->ns_left -= usbn_bus_ns(ls, iso, in, len);
    dev->executed = 1;
    if (dev->ns_left >= UHCI_EOF2_NS)
        return 0;
    usbn_trace("UHCI: port %d: babble at EOF", dev->cur_port + 1);
    if (!dev->babble_keep && (dev->portsc[dev->cur_port] & PORT_PED))
        dev->portsc[dev->cur_port] = (dev->portsc[dev->cur_port] & ~PORT_PED) | PORT_PEDC;
    return 1;
}

static uint32_t
rd32(uint32_t a)
{
    uint32_t v;
    dma_bm_read(a, (uint8_t *) &v, 4, 4);
    return v;
}

static void
wr32(uint32_t a, uint32_t v)
{
    dma_bm_write(a, (uint8_t *) &v, 4, 4);
}

static int
uhci_process_td(uhci_t *dev, uint32_t td)
{
    uint32_t       ctrl  = rd32(td + 4);
    uint32_t       token = rd32(td + 8);
    uint32_t       bufp  = rd32(td + 12);
    uint8_t        pid   = token & 0xff;
    uint8_t        addr  = (token >> 8) & 0x7f;
    uint8_t        ep    = (token >> 15) & 0x0f;
    int            maxlen = ((token >> 21) + 1) & 0x7ff;   /* 0x7ff encodes 0 */
    uint8_t        buf[1280];
    usbn_device_t *d;
    int            ret;

    /* An inactive TD at the head of a queue means the queue is stopped -- a
       short packet or an error the driver has not dealt with yet -- and the
       controller goes on to the next queue without touching it (UHCI Design
       Guide 1.1).  Only a TD completed now moves a queue's element pointer on;
       moving past one the driver left there would send the rest of an
       abandoned transfer to the device.  Its IOC still interrupts at the end
       of the frame, as on real controllers: BIOSes keep an inactive TD with
       IOC in the schedule as a timer for their USB legacy SMI handler (the
       Award one of the ECS P5VX-B), as Linux's uhci-hcd does with its term
       TD to get an interrupt at the next frame. */
    if (!(ctrl & TD_ACTIVE)) {
        if (ctrl & TD_IOC)
            dev->pending_sts |= STS_USBINT;
        return TD_RETRY;
    }

    /* MaxLen 500h-7FEh is illegal: a consistency check failure, so a host
       controller process error -- the controller halts and interrupts,
       whatever USBINTR says (UHCI 1.1, 3.2.2 and 2.1.2).  The Intel
       VS440FX's BIOS keeps two such TDs (OUT, address 127, 5FFh) in its
       frame list as the timer of its USB legacy SMI handler, which restarts
       the controller each time. */
    if ((((token >> 21) & 0x7ff) >= 0x500) && (((token >> 21) & 0x7ff) != 0x7ff)) {
        usbn_trace("UHCI: TD %08X: MaxLen %03X is illegal: process error, halted", td, (token >> 21) & 0x7ff);
        dev->sts |= STS_HCPE | STS_HCHALTED;
        dev->cmd &= ~CMD_RS;
        return TD_EOF;
    }

    if (maxlen > (int) sizeof(buf))
        maxlen = sizeof(buf);

    /* Nothing starts past the preSOF point: the frame keeps room for a
       maximum packet (MAXP: 64 or 32 bytes) before EOF2 (UHCI 1.1 1.3.1). */
    if (dev->ns_left < (UHCI_EOF2_NS + usbn_bus_ns(0, 0, 1, (dev->cmd & CMD_MAXP) ? 64 : 32)))
        return TD_NO_TIME;

    /* Isochronous: one transaction in this TD's frame, whatever happens --
       no handshake, no NAK, no retry.  A device that does not stream
       (no iso entry) completes it empty, so a driver polling it does not
       hang. */
    if (ctrl & TD_ISO) {
        usbn_device_t *idev = uhci_find_device(dev, addr, 0);
        int            n    = 0;

        if (idev && idev->iso) {
            if (pid == USB_PID_OUT) {
                if (maxlen)
                    dma_bm_read(bufp, buf, maxlen, 1);
                n = idev->iso(idev, pid, ep, buf, maxlen);
            } else if (pid == USB_PID_IN) {
                n = idev->iso(idev, pid, ep, buf, maxlen);
                if (n > maxlen)
                    n = maxlen;
                if (n > 0)
                    dma_bm_write(bufp, buf, n, 1);
            }
            if (n < 0)
                n = 0;
        }
        ctrl &= ~(TD_ACTIVE | TD_ACTLEN);
        if (uhci_charge(dev, 0, 1, pid == USB_PID_IN, (pid == USB_PID_IN) ? n : maxlen))
            ctrl |= TD_BABBLE;
        ctrl |= (n - 1) & 0x7ff;    /* 0x7ff: nothing moved */
        wr32(td + 4, ctrl);
        if (ctrl & TD_IOC)
            dev->pending_sts |= STS_USBINT;
        return TD_DONE;
    }

    d = uhci_find_device(dev, addr, !!(ctrl & TD_LS));
    if (d == NULL) {
        dev->ns_left -= usbn_bus_ns(!!(ctrl & TD_LS), 0, pid == USB_PID_IN, 0);
        dev->executed = 1;
        usbn_trace("UHCI: no device at address %d (pid %02X ep %d): timeout", addr, pid, ep);
        /* Nobody answered: a timeout.  Real hardware retries up to C_ERR
           times; the device is not going to appear, so give up at once. */
        ctrl = (ctrl & ~(TD_ACTIVE | TD_CERR)) | TD_CRCTO;
        wr32(td + 4, ctrl);
        dev->pending_sts |= STS_ERRINT;
        if (ctrl & TD_IOC)
            dev->pending_sts |= STS_USBINT;
        return TD_STOP;
    }

    if ((pid == USB_PID_OUT) || (pid == USB_PID_SETUP)) {
        if (maxlen)
            dma_bm_read(bufp, buf, maxlen, 1);
        ret = d->packet(d, pid, ep, buf, maxlen);
        if (ret >= 0)
            ret = maxlen;
    } else if (pid == USB_PID_IN) {
        ret = d->packet(d, pid, ep, buf, maxlen);
        if (ret > maxlen)
            ret = USBN_BABBLE;
        else if (ret > 0)
            dma_bm_write(bufp, buf, ret, 1);
    } else {
        /* Not a PID the controller sends: a host controller process error. */
        dev->sts |= STS_HCPE | STS_HCHALTED;
        dev->cmd &= ~CMD_RS;
        return TD_STOP;
    }

    /* Its bus time: the data the host sent, or the device's packet. */
    if (uhci_charge(dev, !!(ctrl & TD_LS), 0, pid == USB_PID_IN,
                    (ret < 0) ? 0 : ((pid == USB_PID_IN) ? ret : maxlen)) && (ret != USBN_STALL))
        ret = USBN_BABBLE;

    if (ret == USBN_NAK) {
        if (!(ctrl & TD_NAK)) {
            ctrl |= TD_NAK;
            wr32(td + 4, ctrl);
        }
        return TD_RETRY;
    }

    ctrl &= ~(TD_ACTIVE | TD_NAK | TD_ACTLEN);
    if (ret < 0) {
        if (ret == USBN_STALL)
            ctrl |= TD_STALLED;
        else if (ret == USBN_BABBLE)
            ctrl |= TD_STALLED | TD_BABBLE;
        else
            ctrl |= TD_STALLED | TD_CRCTO;
        ctrl |= 0x7ff;
        ctrl &= ~TD_CERR;
        wr32(td + 4, ctrl);
        dev->pending_sts |= STS_ERRINT;
        if (ctrl & TD_IOC)
            dev->pending_sts |= STS_USBINT;
        return TD_STOP;
    }

    ctrl |= (ret - 1) & 0x7ff;      /* ActLen is n-1, 0x7ff for none */
    wr32(td + 4, ctrl);
    if (ctrl & TD_IOC)
        dev->pending_sts |= STS_USBINT;

    if ((pid == USB_PID_IN) && (ret < maxlen) && (ctrl & TD_SPD)) {
        dev->pending_sts |= STS_USBINT;
        return TD_STOP;
    }
    return TD_DONE;
}

/* A descriptor fetch: about one PCI burst (3 bit times) of the frame. */
#define UHCI_FETCH_NS (3.0 * 1000.0 / 12.0)

/* A queue: run its TDs in order until one cannot complete this frame. */
static int
uhci_process_qh(uhci_t *dev, uint32_t qh, int depth)
{
    uint32_t elem = rd32(qh + 4);

    dev->ns_left -= UHCI_FETCH_NS;
    for (int n = 0; (n < 64) && !(elem & LINK_T); n++) {
        if (elem & LINK_QH)
            return (depth < 4) ? uhci_process_qh(dev, elem & ~0xf, depth + 1) : TD_RETRY;

        uint32_t td  = elem & ~0xf;
        int      res;

        dev->executed = 0;
        dev->ns_left -= UHCI_FETCH_NS;
        res = uhci_process_td(dev, td);
        if (res == TD_DONE) {
            elem = rd32(td);
            wr32(qh + 4, elem);     /* the queue element pointer moves on */
        }
        if ((dev->cmd & CMD_SWDBG) && dev->executed)
            return TD_EOF;          /* software debug: one TD, then stop */
        if (res != TD_DONE)
            return res;
    }
    return TD_RETRY;
}

/* Software debug mode: the controller stops after each TD it ran, Run/Stop
   cleared and HCHalted set, and goes on from there once Run/Stop is set
   again -- the same frame, FRNUM unchanged (UHCI 1.1 2.1.1). */
static int
uhci_debug_stop(uhci_t *dev, uint32_t next)
{
    if (!(dev->cmd & CMD_SWDBG) || !dev->executed || !(dev->cmd & CMD_RS))
        return 0;
    dev->dbg_resume = 1;
    dev->dbg_link   = next;
    dev->cmd &= ~CMD_RS;
    dev->sts |= STS_HCHALTED;
    return 1;
}

static void
uhci_frame(void *priv)
{
    uhci_t  *dev = (uhci_t *) priv;
    uint32_t link;
    int      res      = TD_RETRY;
    int      stopped  = 0;

    timer_on_auto(&dev->frame_timer, 1000.0);
    if (!dev->companion)
        usbn_apply(dev);   /* an EHCI card does this for its companion */

    /* Stopped, globally suspended, or kept off the bus by the southbridge. */
    if (!(dev->cmd & CMD_RS) || (dev->cmd & CMD_EGSM))
        return;
    if (dev->chipset && (!dev->bus_master || !dev->present))
        return;

    dev->pending_sts = 0;
    /* The frame is 11936 + SOFMOD bit times (UHCI 1.1 2.1.6), less its SOF. */
    dev->ns_left = (11936.0 + (double) dev->sofmod) * (1000.0 / 12.0) - UHCI_SOF_NS;
    if (dev->dbg_resume) {
        link            = dev->dbg_link;
        dev->dbg_resume = 0;
    } else
        link = rd32(dev->flbase + ((dev->frnum & 0x3ff) << 2));

    /* The horizontal walk, until the frame's time runs out.  Bounded too: a
       schedule may loop back on itself (the bandwidth-reclamation loop). */
    for (int n = 0; (n < 1024) && !(link & LINK_T) && (dev->cmd & CMD_RS); n++) {
        if (link & LINK_QH) {
            uint32_t qh = link & ~0xf;

            res = uhci_process_qh(dev, qh, 0);
            if ((res == TD_EOF) && uhci_debug_stop(dev, link)) {
                stopped = 1;
                break;
            }
            if ((res == TD_EOF) || (res == TD_NO_TIME))
                break;
            link = rd32(qh);
        } else {
            uint32_t td = link & ~0xf;

            dev->executed = 0;
            dev->ns_left -= UHCI_FETCH_NS;
            res = uhci_process_td(dev, td);
            if ((res != TD_NO_TIME) && uhci_debug_stop(dev, rd32(td))) {
                stopped = 1;
                break;
            }
            if ((res == TD_EOF) || (res == TD_NO_TIME))
                break;
            link = rd32(td);
        }
    }

    /* Halted in this frame (a process error, a debug stop): FRNUM stays, so
       a restart meets the same TD again.  The VS440FX's BIOS ticks on that;
       with FRNUM moving on, its tick came twice a second and its POST crawled
       for minutes. */
    if (dev->cmd & CMD_RS)
        dev->frnum = (dev->frnum + 1) & 0x7ff;
    /* A halt (process error, debug stop) interrupts as well. */
    if (dev->pending_sts || !(dev->cmd & CMD_RS) || stopped) {
        dev->sts |= dev->pending_sts;
        uhci_update_irq(dev);
    }
}

/* ----------------------------------------------------------- registers --- */

static void
uhci_reset_regs(uhci_t *dev)
{
    dev->cmd    = 0;
    dev->sts    = STS_HCHALTED;
    dev->intr   = 0;
    dev->frnum  = 0;
    dev->flbase = 0;
    dev->sofmod = 0x40;
    dev->dbg_resume = 0;
    for (int p = 0; p < UHCI_PORTS; p++) {
        dev->portsc[p] &= PORT_CCS | PORT_LSDA | PORT_LS_J | PORT_LS_K;
        if (dev->portsc[p] & PORT_CCS)
            dev->portsc[p] |= PORT_CSC;
        if (dev->dev[p]) {
            dev->dev[p]->addr = 0;
            if (dev->dev[p]->reset)
                dev->dev[p]->reset(dev->dev[p]);
        }
    }
    uhci_update_irq(dev);
}

static uint16_t
uhci_readw(uint16_t addr, void *priv)
{
    uhci_t  *dev = (uhci_t *) priv;
    uint16_t ret = 0xffff;

    switch (addr & 0x1e) {
        case REG_USBCMD:
            ret = dev->cmd;
            break;
        case REG_USBSTS:
            ret = dev->sts;
            break;
        case REG_USBINTR:
            ret = dev->intr;
            break;
        case REG_FRNUM:
            ret = dev->frnum;
            break;
        case REG_FLBASEADD:
            ret = dev->flbase & 0xffff;
            break;
        case REG_FLBASEADD + 2:
            ret = dev->flbase >> 16;
            break;
        case REG_SOFMOD:
            ret = dev->sofmod;
            break;
        case REG_PORTSC1:
        case REG_PORTSC1 + 2:
            ret = dev->portsc[((addr & 0x1e) - REG_PORTSC1) >> 1] | PORT_ALWAYS1;
            break;
        default:
            /* Past the last port: reads as an invalid port (bit 7 clear) so
               drivers that probe for more ports stop here. */
            ret = 0xff7f;
            break;
    }
    return ret;
}

static uint8_t
uhci_readb(uint16_t addr, void *priv)
{
    uint16_t w = uhci_readw(addr & ~1, priv);
    return (addr & 1) ? (w >> 8) : (w & 0xff);
}

static uint32_t
uhci_readl(uint16_t addr, void *priv)
{
    return uhci_readw(addr, priv) | ((uint32_t) uhci_readw(addr + 2, priv) << 16);
}

static void
uhci_writew(uint16_t addr, uint16_t val, void *priv)
{
    uhci_t *dev = (uhci_t *) priv;
    int     p;

    switch (addr & 0x1e) {
        case REG_USBCMD:
            usbn_trace("UHCI: USBCMD <- %04X", val);
            if (val & CMD_GRESET) {
                uhci_reset_regs(dev);
                dev->cmd = val & CMD_GRESET;
                return;
            }
            if (val & CMD_HCRESET) {
                uhci_reset_regs(dev);
                return;
            }
            dev->cmd = val & 0x00ff;
            if (dev->cmd & CMD_RS)
                dev->sts &= ~STS_HCHALTED;
            else
                dev->sts |= STS_HCHALTED;
            break;
        case REG_USBSTS:
            dev->sts &= ~(val & 0x003f & ~STS_HCHALTED);
            uhci_update_irq(dev);
            break;
        case REG_USBINTR:
            dev->intr = val & 0x000f;
            uhci_update_irq(dev);
            break;
        case REG_FRNUM:
            if (dev->sts & STS_HCHALTED)
                dev->frnum = val & 0x07ff;
            break;
        case REG_FLBASEADD:
            dev->flbase = (dev->flbase & 0xffff0000) | (val & 0xf000);
            break;
        case REG_FLBASEADD + 2:
            dev->flbase = (dev->flbase & 0x0000ffff) | ((uint32_t) val << 16);
            usbn_trace("UHCI: FLBASEADD %08X", dev->flbase);
            break;
        case REG_SOFMOD:
            dev->sofmod = val & 0x7f;
            break;
        case REG_PORTSC1:
        case REG_PORTSC1 + 2:
            p = ((addr & 0x1e) - REG_PORTSC1) >> 1;
            /* Reset ends on the 1 -> 0 transition of PR: the device is reset
               and back at address 0. */
            if (!(dev->portsc[p] & PORT_PR) && (val & PORT_PR))
                usbn_trace("UHCI: port %d reset begins", p + 1);
            if ((dev->portsc[p] & PORT_PED) != (val & PORT_PED))
                usbn_trace("UHCI: port %d %s", p + 1, (val & PORT_PED) ? "enabled" : "disabled");
            if ((dev->portsc[p] & PORT_PR) && !(val & PORT_PR) && dev->dev[p]) {
                usbn_trace("UHCI: port %d reset ends", p + 1);
                dev->dev[p]->addr = 0;
                if (dev->dev[p]->reset)
                    dev->dev[p]->reset(dev->dev[p]);
            }
            dev->portsc[p] &= ~(val & PORT_RWC);
            dev->portsc[p] = (dev->portsc[p] & ~PORT_RW) | (val & PORT_RW);
            if (!(dev->portsc[p] & PORT_CCS) || (dev->portsc[p] & PORT_PR))
                dev->portsc[p] &= ~PORT_PED;   /* nothing to enable, or being reset */
            break;
        default:
            break;
    }
}

static void
uhci_writeb(uint16_t addr, uint8_t val, void *priv)
{
    /* Byte writes only make sense for SOFMOD and the halves of the 16-bit
       registers; merge them into a word write. */
    uhci_t  *dev = (uhci_t *) priv;
    uint16_t w;

    if ((addr & 0x1f) == REG_SOFMOD) {
        dev->sofmod = val & 0x7f;
        return;
    }
    w = uhci_readw(addr & ~1, priv);
    /* Write-1-to-clear bits must not be echoed back as ones. */
    if ((addr & 0x1e) == REG_USBSTS)
        w = 0;
    if ((addr & 0x1e) >= REG_PORTSC1)
        w &= ~PORT_RWC;
    if (addr & 1)
        w = (w & 0x00ff) | (val << 8);
    else
        w = (w & 0xff00) | val;
    uhci_writew(addr & ~1, w, priv);
}

static void
uhci_writel(uint16_t addr, uint32_t val, void *priv)
{
    uhci_writew(addr, val & 0xffff, priv);
    uhci_writew(addr + 2, val >> 16, priv);
}

static void
uhci_remap(uhci_t *dev)
{
    if (dev->io_mapped) {
        io_removehandler(dev->io_base, 0x20, uhci_readb, uhci_readw, uhci_readl,
                         uhci_writeb, uhci_writew, uhci_writel, dev);
        dev->io_mapped = 0;
    }
    dev->io_base = (dev->pci_conf[0x20] & 0xe0) | (dev->pci_conf[0x21] << 8);
    if ((dev->pci_conf[0x04] & 0x01) && dev->io_base) {
        io_sethandler(dev->io_base, 0x20, uhci_readb, uhci_readw, uhci_readl,
                      uhci_writeb, uhci_writew, uhci_writel, dev);
        dev->io_mapped = 1;
    }
}

/* ------------------------------------------------------------------ PCI --- */

uint8_t
uhci_pci_read(int func, int addr, UNUSED(int len), void *priv)
{
    const uhci_t *dev = (uhci_t *) priv;

    if (func > 0)
        return 0xff;
    return dev->pci_conf[addr & 0xff];
}

/* A PIIX3 (8086:7020) has MSTAT at 6Ah; a PIIX4 (8086:7112) does not. */
#define UHCI_IS_PIIX3(dev) (((dev)->pci_conf[0x02] | ((dev)->pci_conf[0x03] << 8)) == 0x7020)

void
uhci_pci_write(int func, int addr, UNUSED(int len), uint8_t val, void *priv)
{
    uhci_t *dev = (uhci_t *) priv;

    if (func > 0)
        return;

    if (dev->onboard)
        usbn_trace("UHCI: PCI %02X <- %02X", addr, val);
    switch (addr) {
        case 0x04:
            dev->pci_conf[addr] = val & (dev->onboard ? 0x05 : 0x17);   /* PIIX3: BME and IOSE only */
            uhci_remap(dev);
            break;
        case 0x05:
            dev->pci_conf[addr] = dev->onboard ? 0x00 : (val & 0x01);
            break;
        case 0x07:
            dev->pci_conf[addr] &= ~(val & 0xf9);
            break;
        case 0x0d:
            dev->pci_conf[addr] = dev->onboard ? (val & 0xf0) : val;
            break;
        case 0x20:
            dev->pci_conf[addr] = (val & 0xe0) | 0x01;
            uhci_remap(dev);
            break;
        case 0x21:
            dev->pci_conf[addr] = val;
            uhci_remap(dev);
            break;
        case 0x3c:
            dev->pci_conf[addr] = val;
            break;
        case 0x40 ... 0x4f:
            /* VIA's configuration registers: kept. */
            if (!dev->onboard)
                dev->pci_conf[addr] = val;
            break;
        case 0x6a:
            /* PIIX3 MSTAT: the USB clock, 48 or 24 MHz. */
            if (dev->onboard && UHCI_IS_PIIX3(dev))
                dev->pci_conf[addr] = val & 0x01;
            break;
        case 0xff:
            /* PIIX4: bit 4 the RTC index read-back, done by the southbridge. */
            if (dev->onboard && !UHCI_IS_PIIX3(dev))
                dev->pci_conf[addr] = val & 0x10;
            break;
        case 0xc0 ... 0xc1:
            /* The legacy-support register: does its work onboard, kept on a card. */
            if (dev->onboard)
                uhci_legsup_write(dev, addr, val);
            else
                dev->pci_conf[addr] = val;
            break;
        default:
            break;
    }
}

/* The standalone cards' identities, by device_t .local. */
static const usbn_pci_id_t uhci_ids[] = {
    { 0x1106, 0x3038, 0x1a, 0x1106, 0x3038 },   /* VIA VT83C572                  */
    { 0x8086, 0x7112, 0x01, 0x0000, 0x0000 },   /* Intel 82371AB/EB (PIIX4) USB  */
};

static void
uhci_pci_init_conf(uhci_t *dev, const usbn_pci_id_t *id)
{
    uint8_t *c = dev->pci_conf;

    memset(c, 0, sizeof(dev->pci_conf));
    c[0x00] = id->vendor & 0xff;
    c[0x01] = id->vendor >> 8;
    c[0x02] = id->device & 0xff;
    c[0x03] = id->device >> 8;
    c[0x06] = 0x00; c[0x07] = 0x02;   /* medium DEVSEL */
    c[0x08] = id->revision;
    c[0x09] = 0x00;                   /* prog-if: UHCI */
    c[0x0a] = 0x03;                   /* USB */
    c[0x0b] = 0x0c;                   /* serial bus controller */
    c[0x0d] = 0x16;
    c[0x20] = 0x01;                   /* BAR4: I/O, 32 bytes */
    c[0x2c] = id->sub_vendor & 0xff;
    c[0x2d] = id->sub_vendor >> 8;
    c[0x2e] = id->sub_device & 0xff;
    c[0x2f] = id->sub_device >> 8;
    c[0x3d] = 0x01;                   /* INTA# */
    if (id->vendor == 0x1106) {       /* VIA's own configuration registers */
        c[0x40] = 0x40;
        c[0x41] = 0x10;
    }
    c[0x60] = 0x10;                   /* SBRN: USB 1.0 / 1.1 */
    c[0xc1] = 0x20;                   /* LEGSUP: PIRQ enable */
}

/* --------------------------------------------------------- the device --- */

static void
uhci_root_connect(void *priv, int port, usbn_device_t *d)
{
    uhci_port_connect((uhci_t *) priv, port, d);
}

static void
uhci_root_disconnect(void *priv, int port)
{
    uhci_port_disconnect((uhci_t *) priv, port);
}

/* Resume signalling from a device: in global suspend the controller sets
   Force Global Resume and Resume Detect (an interrupt with its enable); on a
   selectively suspended port the port's Resume Detect (UHCI 1.1 2.1.1-2,
   2.1.7). */
static void
uhci_root_resume(void *priv, int port)
{
    uhci_t *dev = (uhci_t *) priv;

    if ((port >= UHCI_PORTS) || !dev->dev[port] || !(dev->portsc[port] & PORT_PED))
        return;
    if (dev->cmd & CMD_EGSM) {
        usbn_trace("UHCI: port %d: remote wakeup from global suspend", port + 1);
        dev->cmd |= CMD_FGR;
        dev->sts |= STS_RD;
        uhci_update_irq(dev);
    } else if (dev->portsc[port] & PORT_SUSP) {
        usbn_trace("UHCI: port %d: remote wakeup", port + 1);
        dev->portsc[port] |= PORT_RD;
    }
}

/* Nothing reaches the port: selectively suspended, or the bus in global
   suspend (or stopped). */
static int
uhci_root_idle(void *priv, int port)
{
    const uhci_t *dev = (uhci_t *) priv;

    if ((port >= UHCI_PORTS) || !(dev->portsc[port] & PORT_PED))
        return 0;
    return !!(dev->portsc[port] & PORT_SUSP) || !!(dev->cmd & CMD_EGSM);
}

static const usbn_root_ops_t uhci_root_ops = { uhci_root_connect, uhci_root_disconnect, uhci_root_resume, uhci_root_idle };

static uhci_t *
uhci_core_create(const usbn_pci_id_t *id)
{
    uhci_t *dev = calloc(1, sizeof(uhci_t));

    uhci_pci_init_conf(dev, id);
    dev->slotp   = &dev->pci_slot;
    dev->irq_pin = PCI_INTA;
    uhci_reset_regs(dev);
    timer_add(&dev->frame_timer, uhci_frame, dev, 0);
    timer_on_auto(&dev->frame_timer, 1000.0);
    return dev;
}

static void *
uhci_init(const device_t *info)
{
    uhci_t *dev = uhci_core_create(&uhci_ids[info->local]);

    pci_add_card(PCI_ADD_NORMAL, uhci_pci_read, uhci_pci_write, dev, &dev->pci_slot);
    if (usbn_set_root(&uhci_root_ops, dev, 0))
        usbn_restore_ports();
    return dev;
}

static void
uhci_close(void *priv)
{
    usbn_set_root(NULL, priv, 0);
    free(priv);
}

static void
uhci_reset(void *priv)
{
    uhci_reset_regs((uhci_t *) priv);
}

const device_t usb_uhci_via_device = {
    .name          = "VIA VT83C572 USB 1.1 Host Controller (UHCI, PCI)",
    .internal_name = "usb_uhci_via",
    .flags         = DEVICE_PCI,
    .local         = 0,
    .init          = uhci_init,
    .close         = uhci_close,
    .reset         = uhci_reset,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = NULL
};

const device_t usb_uhci_intel_device = {
    .name          = "Intel 82371AB/EB (PIIX4) USB 1.1 Host Controller (UHCI, PCI)",
    .internal_name = "usb_uhci_intel",
    .flags         = DEVICE_PCI,
    .local         = 1,
    .init          = uhci_init,
    .close         = uhci_close,
    .reset         = uhci_reset,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = NULL
};

/* ------------------------------------------------- as an EHCI companion --- */

uhci_t *
uhci_companion_create(uint8_t *card_slot, const usbn_pci_id_t *id)
{
    uhci_t *dev = uhci_core_create(id);

    dev->companion      = 1;
    dev->slotp          = card_slot;
    dev->pci_conf[0x0e] = 0x80;   /* function 0 of a multi-function card */
    return dev;
}

void
uhci_companion_close(uhci_t *dev)
{
    free(dev);
}

void
uhci_companion_reset(uhci_t *dev)
{
    uhci_reset_regs(dev);
}

/* The EHCI card gives a port to us (d) or takes it back (NULL). */
void
uhci_route_port(uhci_t *dev, int port, usbn_device_t *d)
{
    if (dev->dev[port] == d)
        return;
    if (dev->dev[port])
        uhci_port_disconnect(dev, port);
    if (d)
        uhci_port_connect(dev, port, d);
}

/* -------------------------------------------- built into a southbridge --- */

/* The PIIX3's function 2 as it comes out of reset (2.4.1-2.4.14), and the
   PIIX4's, which is the same but for MSTAT (and FFh as upstream has it). */
static void
uhci_onboard_init_conf(uhci_t *dev, const usbn_pci_id_t *id)
{
    uint8_t *c = dev->pci_conf;

    uhci_pci_init_conf(dev, id);
    c[0x06] = 0x80;   /* fast back-to-back capable */
    c[0x07] = 0x02;   /* medium DEVSEL */
    c[0x0d] = 0x00;   /* MLT */
    c[0x0e] = 0x00;   /* function 0's HEDT says multi-function */
    c[0x3d] = 0x04;   /* INTD: PIRQD */
    if (UHCI_IS_PIIX3(dev))
        c[0x6a] = 0x01;   /* MSTAT: 48 MHz USB clock */
    else
        c[0xff] = 0x10;
    c[0xc0] = 0x00;   /* LEGSUP: 2000h, the interrupt to PIRQD */
    c[0xc1] = 0x20;
}

uhci_t *
uhci_onboard_create(const usbn_pci_id_t *id, void (*legacy_smi)(void *priv, int event), void *priv)
{
    uhci_t *dev = uhci_core_create(id);

    dev->onboard     = 1;
    dev->iirq        = PCI_IIRQ_BASE | PCI_INTD;
    dev->slotp       = &dev->iirq;
    dev->irq_pin     = 0;
    dev->legacy_smi  = legacy_smi;
    dev->legacy_priv = priv;
    uhci_onboard_init_conf(dev, id);
    uhci_legacy_remap(dev);

    if (usbn_set_root(&uhci_root_ops, dev, 0))
        usbn_restore_ports();
    return dev;
}

void
uhci_onboard_reset(uhci_t *dev)
{
    usbn_pci_id_t id = {
        dev->pci_conf[0x00] | (dev->pci_conf[0x01] << 8), dev->pci_conf[0x02] | (dev->pci_conf[0x03] << 8),
        dev->pci_conf[0x08], 0, 0
    };

    uhci_onboard_init_conf(dev, &id);
    uhci_remap(dev);          /* I/O space off, and no base */
    dev->a20_state = 0;
    dev->irq_level = 0;
    uhci_legacy_remap(dev);
    uhci_reset_regs(dev);
}

void
uhci_onboard_close(uhci_t *dev)
{
    if (dev->claiming) {
        io_set_claim(0x60, NULL, NULL);
        io_set_claim(0x64, NULL, NULL);
    }
    dev->pci_conf[0x04] = 0;
    uhci_remap(dev);
    usbn_set_root(NULL, dev, 0);
    free(dev);
}

/* ------------------------------------------- driven by a southbridge --- */

/* Each has its own ports (a block of bus slots): one turned off by the
   southbridge (VIA Rx48 bit 2 / Rx85 bit 4) runs no frames, and what is
   assigned to its ports is out of the guest's reach, as on the board. */
uhci_t *
uhci_chipset_create(void)
{
    static const usbn_pci_id_t none = { 0, 0, 0, 0, 0 };
    uhci_t                    *dev  = uhci_core_create(&none);

    dev->chipset    = 1;
    dev->present    = 1;
    dev->bus_master = 0;
    if (usbn_set_root(&uhci_root_ops, dev, 0))
        usbn_restore_ports();
    return dev;
}

void
uhci_chipset_close(uhci_t *dev)
{
    if (dev->io_mapped) {
        dev->pci_conf[0x04] = 0;
        uhci_remap(dev);
    }
    usbn_set_root(NULL, dev, 0);
    timer_disable(&dev->frame_timer);
    free(dev);
}

/* PCIRST#: the registers, I/O off, the interrupt low, no bus mastering. */
void
uhci_chipset_reset(uhci_t *dev)
{
    dev->pci_conf[0x04] = 0;
    uhci_remap(dev);
    dev->bus_master = 0;
    uhci_reset_regs(dev);
}

void
uhci_chipset_set_io(uhci_t *dev, uint16_t base, int enable)
{
    dev->pci_conf[0x20] = (base & 0xe0) | 0x01;
    dev->pci_conf[0x21] = base >> 8;
    dev->pci_conf[0x04] = enable ? 0x01 : 0x00;
    uhci_remap(dev);
}

void
uhci_chipset_set_irq(uhci_t *dev, void (*set_irq)(void *priv, int level), void *priv)
{
    dev->set_irq  = set_irq;
    dev->irq_priv = priv;
    dev->irq_out  = 0;
}

void
uhci_chipset_set_bus_master(uhci_t *dev, int enable)
{
    dev->bus_master = !!enable;
}

void
uhci_chipset_set_babble_keep_port(uhci_t *dev, int keep)
{
    dev->babble_keep = !!keep;
}

void
uhci_chipset_set_present(uhci_t *dev, int present)
{
    if (dev->present != !!present)
        usbn_trace("UHCI: the southbridge turned a controller %s", present ? "on" : "off");
    dev->present = !!present;
}

int
uhci_chipset_present(const uhci_t *dev)
{
    return dev->present;
}

/* -------------------------------------------------------- the card list --- */

extern const device_t usb_ehci_via_device;
extern const device_t usb_ehci_intel_device;
extern const device_t usb_opti_82c861_device;

static const device_t *usb_cards[] = {
    &device_none,
    &usb_uhci_via_device,
    &usb_uhci_intel_device,
    &usb_opti_82c861_device,
    &usb_ehci_via_device,
    &usb_ehci_intel_device,
    NULL
};

void
usb_card_reset(void)
{
    /* Its controller takes the card's ports (usb_bus.c). */
    for (int i = 0; i < USBN_CARDS; i++) {
        const int t = usbn_card_get(i);
        if ((t > 0) && (t < usb_card_count())) {
            usbn_card_context = i + 1;
            device_add_inst(usb_cards[t], i + 1);
            usbn_card_context = 0;
        }
    }
}

int
usb_card_count(void)
{
    int n = 0;
    while (usb_cards[n])
        n++;
    return n;
}

const char *
usb_card_get_internal_name(int card)
{
    return device_get_internal_name(usb_cards[card]);
}

const char *
usb_card_get_name(int card)
{
    return usb_cards[card]->name;
}

int
usb_card_get_from_internal_name(const char *s)
{
    for (int c = 0; usb_cards[c]; c++)
        if (!strcmp(usb_cards[c]->internal_name, s))
            return c;
    return 0;
}
