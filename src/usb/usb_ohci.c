/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             A USB 1.1 host controller: Open Host Controller Interface
 *             (OpenHCI, release 1.0a), the controller of the ALi, SiS, SMSC
 *             and ST southbridges and of OPTi's FireLink card.  Its registers
 *             are a 4 KB memory window; the southbridge (or the card) owns the
 *             PCI configuration space and tells the controller where the
 *             window is, whether it may master the bus and where its INT and
 *             SMI outputs go (usb_chipset.c, behind upstream's usb.h API).
 *
 *             The root hub has two connectors on our ports (a third on the
 *             ALi M5237, never with a device), with global or per-port power
 *             switching, port reset (10 ms), selective suspend and resume
 *             (20 ms + 3 ms), and the USBRESET / USBRESUME / USBOPERATIONAL /
 *             USBSUSPEND states.  Each 1 ms frame -- FrameInterval + 1 bit
 *             times -- writes HccaFrameNumber, then runs the control and bulk
 *             lists (ControlBulkServiceRatio) until FrameRemaining reaches
 *             PeriodicStart, the periodic list (interrupt EDs from the HCCA's
 *             table, isochronous EDs with IsochronousEnable), and the
 *             nonperiodic lists again, every transaction charged its USB 1.1
 *             bus time (usbn_bus_ns()).  A transaction that does not fit what
 *             is left of the frame waits for the next; a periodic list cut
 *             short is a SchedulingOverrun.  A NAK leaves the TD where it is,
 *             so a passed-through host device whose transfer is in flight is
 *             simply asked again.  Completed TDs go on the done queue, written
 *             to HccaDoneHead as the TDs' DelayInterrupt allows.
 *
 *             References: Compaq, Microsoft, National Semiconductor, "OpenHCI
 *             Open Host Controller Interface Specification for USB", release
 *             1.0a, 1999; "Universal Serial Bus Specification", revision 1.1,
 *             1998.  The chip differences (usb.h's OHCI_CHIP_* flags) and the
 *             root hub's details follow upstream 86Box's usb.c (skiretic,
 *             2026), which has them from the ALi M1543C, SiS 5595, SMSC
 *             SLC90E66, STPC Atlas and OPTi 82C861 data books.
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
#include <86box/dma.h>
#include <86box/timer.h>
#include <86box/plat_unused.h>
#include <86box/usb.h>
#include <86box/usb_next.h>

/* Operational registers (OpenHCI 1.0a s7). */
#define REG_REVISION       0x000
#define REG_CONTROL        0x004
#define REG_CMDSTATUS      0x008
#define REG_INTSTATUS      0x00c
#define REG_INTENABLE      0x010
#define REG_INTDISABLE     0x014
#define REG_HCCA           0x018
#define REG_PERIODCURED    0x01c
#define REG_CTRLHEADED     0x020
#define REG_CTRLCURED      0x024
#define REG_BULKHEADED     0x028
#define REG_BULKCURED      0x02c
#define REG_DONEHEAD       0x030
#define REG_FMINTERVAL     0x034
#define REG_FMREMAINING    0x038
#define REG_FMNUMBER       0x03c
#define REG_PERIODICSTART  0x040
#define REG_LSTHRESHOLD    0x044
#define REG_RHDESCA        0x048
#define REG_RHDESCB        0x04c
#define REG_RHSTATUS       0x050
#define REG_RHPORT1        0x054
#define REG_HCECONTROL     0x100   /* legacy support (Appendix B) */
#define REG_HCEINPUT       0x104
#define REG_HCEOUTPUT      0x108
#define REG_HCESTATUS      0x10c

#define CTL_CBSR           0x00000003
#define CTL_PLE            0x00000004
#define CTL_IE             0x00000008
#define CTL_CLE            0x00000010
#define CTL_BLE            0x00000020
#define CTL_HCFS           0x000000c0
#define CTL_IR             0x00000100
#define CTL_RWC            0x00000200
#define CTL_RWE            0x00000400

#define HCFS_RESET         0x00000000
#define HCFS_RESUME        0x00000040
#define HCFS_OPERATIONAL   0x00000080
#define HCFS_SUSPEND       0x000000c0

#define CS_HCR             0x00000001
#define CS_CLF             0x00000002
#define CS_BLF             0x00000004
#define CS_OCR             0x00000008
#define CS_SOC             0x00030000

#define INT_SO             0x00000001
#define INT_WDH            0x00000002
#define INT_SF             0x00000004
#define INT_RD             0x00000008
#define INT_UE             0x00000010
#define INT_FNO            0x00000020
#define INT_RHSC           0x00000040
#define INT_OC             0x40000000
#define INT_MIE            0x80000000
#define INT_ALL            0x4000007f

/* HcRhStatus; a write of 1 to some bits is a command (named on the right). */
#define RH_LPS             0x00000001   /* ClearGlobalPower         */
#define RH_OCI             0x00000002
#define RH_DRWE            0x00008000   /* SetRemoteWakeupEnable    */
#define RH_LPSC            0x00010000   /* SetGlobalPower           */
#define RH_OCIC            0x00020000
#define RH_CRWE            0x80000000

/* HcRhPortStatus, likewise. */
#define PS_CCS             0x00000001   /* ClearPortEnable          */
#define PS_PES             0x00000002   /* SetPortEnable            */
#define PS_PSS             0x00000004   /* SetPortSuspend           */
#define PS_POCI            0x00000008   /* ClearSuspendStatus       */
#define PS_PRS             0x00000010   /* SetPortReset             */
#define PS_PPS             0x00000100   /* SetPortPower             */
#define PS_LSDA            0x00000200   /* ClearPortPower           */
#define PS_CSC             0x00010000
#define PS_PESC            0x00020000
#define PS_PSSC            0x00040000
#define PS_OCIC            0x00080000
#define PS_PRSC            0x00100000
#define PS_CHANGE          0x001f0000

#define DA_PSM             0x00000100
#define DA_NPS             0x00000200

#define HCE_EE             0x00000001
#define HCE_EI             0x00000002
#define HCE_CP             0x00000004
#define HCE_EXTIRQEN       0x00000010
#define HCE_IRQ1ACT        0x00000040
#define HCE_IRQ12ACT       0x00000080
#define HCES_OUTFULL       0x01
#define HCES_INFULL        0x02

/* Endpoint descriptor (s4.2) and transfer descriptors (s4.3). */
#define ED_D               0x00001800
#define ED_S               0x00002000
#define ED_K               0x00004000
#define ED_F               0x00008000
#define ED_H               0x00000001
#define ED_C               0x00000002
#define TD_R               0x00040000
#define TD_DP              0x00180000
#define TD_DI              0x00e00000
#define TD_T               0x03000000
#define TD_EC              0x0c000000
#define TD_CC              0xf0000000

#define CC_NOERROR         0x0
#define CC_STALL           0x4
#define CC_NOTRESPONDING   0x5
#define CC_DATAOVERRUN     0x8
#define CC_DATAUNDERRUN    0x9

#define FI_NOMINAL         0x00002edf   /* 11999: a frame of 12000 bit times */
#define SOF_BITS           35.0         /* SYNC, PID, frame number, CRC5, EOP */
#define FETCH_BITS         3.0          /* a descriptor fetch, about one PCI burst */
#define FS_BIT_NS          (1000.0 / 12.0)

#define PORT_RESET_US      10000.0
#define PORT_RESUME_US     23000.0      /* 20 ms resume, EOP, 3 ms resync */

#define OHCI_MAX_PORTS     3

enum { PORT_TIMER_NONE, PORT_TIMER_RESET, PORT_TIMER_RESUME };

/* What serving an ED came to. */
enum {
    ED_NONE,      /* nothing to do: skipped, halted or empty                */
    ED_SENT,      /* a transaction went out (or was NAKed)                  */
    ED_NO_TIME,   /* a TD waits, but its packet does not fit the frame      */
};

typedef struct ohci_port_t {
    uint32_t       sts;        /* status 15:0, change 31:16 */
    int            timer_op;
    int            num;        /* 0-based */
    usbn_device_t *dev;        /* on the wire */
    struct ohci_t *hc;
    pc_timer_t     timer;
} ohci_port_t;

struct ohci_t {
    uint32_t control, cmdsts, intsts, inten, hcca;
    uint32_t period_cur, ctrl_head, ctrl_cur, bulk_head, bulk_cur, done_head;
    uint32_t fm_interval, fm_remaining, fm_number, periodic_start, ls_threshold;
    uint32_t rh_desc_a, rh_desc_b, rh_status;
    int      ndp, ports;
    ohci_port_t port[OHCI_MAX_PORTS];

    uint32_t chip_flags;
    uint8_t  bus_master, bm_hardwired, mem_access;

    /* The frame in progress. */
    uint32_t   fr_load;      /* what FrameRemaining counted down from */
    uint32_t   fn_msb;       /* FrameNumber bit 15 at the last HCCA update */
    int        frame_running;
    double     bits_left;
    int        ctrl_served;  /* control EDs since the last bulk ED */
    uint8_t    so_pending, di_count;
    pc_timer_t frame_timer;  /* 1 ms always: the bus's plug requests too */

    /* The root hub waits 5 ms after entering USBSUSPEND before a wakeup. */
    pc_timer_t suspend_timer;
    uint8_t    suspend_settling, resume_held;

    void   (*set_irq)(void *priv, int level);
    void    *irq_priv;
    void   (*set_smi)(void *priv, int level);
    void    *smi_priv;
    uint8_t  int_level, smi_level;

    uint32_t hce_control;
    uint8_t  hce_input, hce_output, hce_status;

    mem_mapping_t mmio;
    uint32_t      mem_base;
    int           mapped;
    int           root;      /* the USB ports are ours */
};

/* ------------------------------------------------------- INT and SMI --- */

/* An enabled status with MasterInterruptEnable requests an interrupt: on INT
   with InterruptRouting clear, on SMI with it set; OwnershipChange always
   goes to SMI (s6.5).  With PCI Memory Access off both stay inactive. */
static void
ohci_update_irq(ohci_t *dev)
{
    uint32_t pending = (dev->mem_access && (dev->inten & INT_MIE)) ? (dev->intsts & dev->inten & INT_ALL) : 0;
    uint8_t  req     = !!(pending & ~INT_OC);
    uint8_t  ir      = !!(dev->control & CTL_IR);
    uint8_t  int_lvl = req && !ir;
    uint8_t  smi_lvl = (req && ir) || !!(pending & INT_OC);

    if (int_lvl != dev->int_level) {
        dev->int_level = int_lvl;
        if (dev->set_irq)
            dev->set_irq(dev->irq_priv, int_lvl);
    }
    if (smi_lvl != dev->smi_level) {
        dev->smi_level = smi_lvl;
        if (dev->set_smi)
            dev->set_smi(dev->smi_priv, smi_lvl);
    }
}

/* ---------------------------------------------------------- root hub --- */

static void
port_timer_stop(ohci_port_t *p)
{
    timer_stop(&p->timer);
    p->timer_op = PORT_TIMER_NONE;
}

/* A change the root hub makes raises RootHubStatusChange, in USBOPERATIONAL
   only (s5.1.2, s6.5.7); the change bit latches either way. */
static void
port_change(ohci_port_t *p, uint32_t bits)
{
    p->sts |= bits;
    if ((p->hc->control & CTL_HCFS) == HCFS_OPERATIONAL)
        p->hc->intsts |= INT_RHSC;
}

static int
port_fixed(const ohci_port_t *p)
{
    return !!(p->hc->rh_desc_b & (1 << (p->num + 1)));
}

static int
port_powered(const ohci_port_t *p)
{
    return (p->hc->rh_desc_a & DA_NPS) || (p->sts & PS_PPS);
}

static int
rh_in_reset(const ohci_t *dev)
{
    return (dev->control & CTL_HCFS) == HCFS_RESET;
}

static uint32_t
port_read(const ohci_port_t *p)
{
    uint32_t ret = p->sts;

    if (p->hc->rh_desc_a & DA_NPS)
        ret |= PS_PPS;
    if (port_fixed(p))
        ret |= PS_CCS;
    return ret;
}

static void
dev_bus_reset(usbn_device_t *d)
{
    d->addr = 0;
    if (d->reset)
        d->reset(d);
}

/* Entering USBRESUME clears every port's PortSuspendStatus. */
static void
rh_global_resume(ohci_t *dev)
{
    for (int i = 0; i < dev->ndp; i++) {
        if (dev->port[i].timer_op == PORT_TIMER_RESUME)
            port_timer_stop(&dev->port[i]);
        dev->port[i].sts &= ~PS_PSS;
    }
}

/* A resume event in USBSUSPEND moves the HC to USBRESUME with
   ResumeDetected; in the first 5 ms of USBSUSPEND it waits (s6.2.3). */
static void
resume_detected(ohci_t *dev)
{
    if ((dev->control & CTL_HCFS) != HCFS_SUSPEND)
        return;
    if (dev->suspend_settling) {
        dev->resume_held = 1;
        return;
    }
    usbn_trace("OHCI: resume detected");
    dev->control = (dev->control & ~CTL_HCFS) | HCFS_RESUME;
    dev->intsts |= INT_RD;
    rh_global_resume(dev);
}

static void
suspend_settled(void *priv)
{
    ohci_t *dev = (ohci_t *) priv;

    dev->suspend_settling = 0;
    if (dev->resume_held) {
        dev->resume_held = 0;
        resume_detected(dev);
        ohci_update_irq(dev);
    }
}

static void
suspend_settle(ohci_t *dev, int entered)
{
    dev->resume_held      = 0;
    dev->suspend_settling = entered;
    if (entered)
        timer_on_auto(&dev->suspend_timer, 5000.0);
    else
        timer_stop(&dev->suspend_timer);
}

/* With DeviceRemovable set, CSC comes only after a root hub reset; with
   DeviceRemoteWakeupEnable a connect change is a resume event (s7.4.3-4). */
static void
port_connect_change(ohci_port_t *p)
{
    if (!port_fixed(p))
        port_change(p, PS_CSC);
    if (p->hc->rh_status & RH_DRWE)
        resume_detected(p->hc);
}

/* A device is seen on a powered port within NDP, outside USBRESET. */
static void
port_detect(ohci_port_t *p)
{
    if (!p->dev || (p->num >= p->hc->ndp) || (p->sts & PS_CCS) || !port_powered(p) || rh_in_reset(p->hc))
        return;
    p->sts |= PS_CCS | ((p->dev->speed == USBN_SPEED_LOW) ? PS_LSDA : 0);
    port_connect_change(p);
}

/* Power off clears CCS, PES, PSS and PRS; losing PES sets PESC. */
static void
port_power_off(ohci_port_t *p)
{
    port_timer_stop(p);
    if (p->sts & PS_PES)
        port_change(p, PS_PESC);
    p->sts &= ~(PS_CCS | PS_PES | PS_PSS | PS_PRS | PS_LSDA);
    if (p->dev)
        dev_bus_reset(p->dev);
}

static void
port_set_power(ohci_port_t *p, int on)
{
    int was = port_powered(p);

    if (on)
        p->sts |= PS_PPS;
    else
        p->sts &= ~PS_PPS;
    if (was && !port_powered(p))
        port_power_off(p);
    else
        port_detect(p);
}

/* Set/ClearGlobalPower: every port in global mode, else the ports whose
   PortPowerControlMask bit is clear. */
static void
rh_global_power(ohci_t *dev, int on)
{
    for (int i = 0; i < dev->ndp; i++) {
        if (!(dev->rh_desc_a & DA_PSM) || !(dev->rh_desc_b & (0x10000 << (i + 1))))
            port_set_power(&dev->port[i], on);
    }
}

static void
port_timer(void *priv)
{
    ohci_port_t *p  = (ohci_port_t *) priv;
    int          op = p->timer_op;

    p->timer_op = PORT_TIMER_NONE;
    if (op == PORT_TIMER_RESET) {
        /* The end of reset: PES set, PSS and PSSC clear. */
        p->sts = (p->sts & ~(PS_PRS | PS_PSS | PS_PSSC)) | PS_PES;
        port_change(p, PS_PRSC);
        usbn_trace("OHCI: port %d reset ends", p->num + 1);
    } else if (op == PORT_TIMER_RESUME) {
        p->sts = (p->sts & ~PS_PSS) | PS_PES;
        port_change(p, PS_PSSC);
        usbn_trace("OHCI: port %d resumed", p->num + 1);
    }
    ohci_update_irq(p->hc);
}

static void
port_resume_start(ohci_port_t *p)
{
    p->timer_op = PORT_TIMER_RESUME;
    timer_on_auto(&p->timer, PORT_RESUME_US);
}

static void
port_write(ohci_port_t *p, uint32_t val)
{
    const ohci_t *dev       = p->hc;
    int           connected = !!(port_read(p) & PS_CCS);

    if (rh_in_reset(dev))
        return;

    /* Change bits first, so a CSC a command below sets survives. */
    p->sts &= ~(val & PS_CHANGE);
    if (val & PS_CCS)
        p->sts &= ~PS_PES;

    /* SetPortEnable/Suspend/Reset on a disconnected port set CSC instead. */
    if (val & (PS_PES | PS_PSS | PS_PRS)) {
        if (!connected)
            port_change(p, PS_CSC);
        else {
            if (val & PS_PES)
                p->sts |= PS_PES;
            if (val & PS_PSS)
                p->sts |= PS_PSS;
            if ((val & PS_PRS) && !(p->sts & PS_PRS)) {
                usbn_trace("OHCI: port %d reset begins", p->num + 1);
                p->sts |= PS_PRS;
                p->timer_op = PORT_TIMER_RESET;
                timer_on_auto(&p->timer, PORT_RESET_US);
                if (p->dev)
                    dev_bus_reset(p->dev);
            }
        }
    }

    /* ClearSuspendStatus resumes a suspended port. */
    if ((val & PS_POCI) && (p->sts & PS_PSS) && (p->timer_op == PORT_TIMER_NONE))
        port_resume_start(p);

    /* Per-port power: per-port mode with the port's PortPowerControlMask. */
    if (!(dev->rh_desc_a & DA_NPS) && (dev->rh_desc_a & DA_PSM) && (dev->rh_desc_b & (0x10000 << (p->num + 1)))) {
        if (val & PS_PPS)
            port_set_power(p, 1);
        if (val & PS_LSDA)
            port_set_power(p, 0);
    }
}

static void
rh_status_write(ohci_t *dev, uint32_t val)
{
    if (rh_in_reset(dev))
        return;
    dev->rh_status &= ~(val & RH_OCIC);
    if (val & RH_LPS)
        rh_global_power(dev, 0);
    if (val & RH_LPSC)
        rh_global_power(dev, 1);
    if (val & RH_DRWE)
        dev->rh_status |= RH_DRWE;
    if (val & RH_CRWE)
        dev->rh_status &= ~RH_DRWE;
}

static void
rh_detect_all(ohci_t *dev)
{
    for (int i = 0; i < dev->ndp; i++)
        port_detect(&dev->port[i]);
}

/* The root hub's reset, entering USBRESET and at a hardware reset: reset
   signalling reaches every device; descriptors A and B are kept.  CSC
   resets to 1 on a non-removable port. */
static void
root_hub_reset(ohci_t *dev)
{
    dev->rh_status = 0;
    for (int i = 0; i < OHCI_MAX_PORTS; i++) {
        ohci_port_t *p = &dev->port[i];

        port_timer_stop(p);
        p->sts = 0;
        if (p->dev)
            dev_bus_reset(p->dev);
        if ((i < dev->ndp) && port_fixed(p))
            port_change(p, PS_CSC);
    }
}

/* Entering USBSUSPEND while a port resumes, or has with PSSC set, ends that
   resume and is a resume event. */
static void
rh_suspend(ohci_t *dev)
{
    int event = 0;

    for (int i = 0; i < dev->ndp; i++) {
        ohci_port_t *p = &dev->port[i];

        if ((p->timer_op == PORT_TIMER_RESUME) || (p->sts & PS_PSSC)) {
            if (p->timer_op == PORT_TIMER_RESUME)
                port_timer_stop(p);
            p->sts &= ~PS_PSS;
            event = 1;
        }
    }
    if (event)
        resume_detected(dev);
}

static void
port_attach(ohci_port_t *p, usbn_device_t *d)
{
    d->fs_view = (d->speed == USBN_SPEED_HIGH);   /* a USB 1.1 port */
    p->dev     = d;
    port_detect(p);
    ohci_update_irq(p->hc);
}

static void
port_detach(ohci_port_t *p)
{
    port_timer_stop(p);
    if (p->sts & PS_CCS) {
        if (p->sts & PS_PES)
            port_change(p, PS_PESC);
        p->sts &= ~(PS_CCS | PS_PES | PS_PSS | PS_PRS | PS_LSDA);
        port_connect_change(p);
    }
    p->dev = NULL;
    ohci_update_irq(p->hc);
}

/* Resume signalling from a device (remote wakeup): in USBSUSPEND a resume
   event, on a suspended port in USBOPERATIONAL that port's resume. */
static void
port_remote_wakeup(ohci_port_t *p)
{
    uint32_t hcfs = p->hc->control & CTL_HCFS;

    if (!(p->sts & PS_CCS) || !(p->sts & PS_PES))
        return;
    usbn_trace("OHCI: port %d: remote wakeup", p->num + 1);
    if (hcfs == HCFS_SUSPEND)
        resume_detected(p->hc);
    else if ((p->sts & PS_PSS) && (p->timer_op == PORT_TIMER_NONE))
        port_resume_start(p);
    ohci_update_irq(p->hc);
}

/* ----------------------------------------------------------- the lists --- */

static void
rd_dwords(uint32_t a, uint32_t *d, int n)
{
    dma_bm_read(a, (uint8_t *) d, n * 4, 4);
}

static void
wr_dword(uint32_t a, uint32_t v)
{
    dma_bm_write(a, (uint8_t *) &v, 4, 4);
}

static usbn_device_t *
find_device(ohci_t *dev, int addr)
{
    for (int i = 0; i < dev->ndp; i++) {
        const ohci_port_t *p = &dev->port[i];

        if (p->dev && (p->sts & PS_CCS) && (p->sts & PS_PES) && !(p->sts & (PS_PSS | PS_PRS)) &&
            (p->dev->addr == addr))
            return p->dev;
    }
    return NULL;
}

static double
bus_bits(int low_speed, int iso, int in, int len)
{
    return usbn_bus_ns(low_speed, iso, in, len) / FS_BIT_NS;
}

/* The bytes a general TD still has, CBP to BE across at most one page
   crossing (s4.3.1.3.4); CBP 0 is a zero-length packet. */
static int
td_remaining(uint32_t cbp, uint32_t be)
{
    if (cbp == 0)
        return 0;
    if ((cbp & ~0xfff) == (be & ~0xfff))
        return (int) (be - cbp) + 1;
    return (int) (0x1000 - (cbp & 0xfff)) + (int) (be & 0xfff) + 1;
}

/* CBP n bytes on, into BE's page past the end of CBP's. */
static uint32_t
td_advance(uint32_t cbp, uint32_t be, int n)
{
    if (n <= 0)
        return cbp;
    if (((cbp & ~0xfff) != (be & ~0xfff)) && (n >= (int) (0x1000 - (cbp & 0xfff))))
        return (be & ~0xfff) + (n - (0x1000 - (cbp & 0xfff)));
    return cbp + n;
}

/* Move n bytes at CBP; returns the CBP after them. */
static uint32_t
td_data(uint32_t cbp, uint32_t be, uint8_t *buf, int n, int to_memory)
{
    int first = n;

    if (n <= 0)
        return cbp;
    if (((cbp & ~0xfff) != (be & ~0xfff)) && (first > (int) (0x1000 - (cbp & 0xfff))))
        first = 0x1000 - (cbp & 0xfff);
    if (to_memory)
        dma_bm_write(cbp, buf, first, 1);
    else
        dma_bm_read(cbp, buf, first, 1);
    if (first == n)
        return td_advance(cbp, be, n);
    if (to_memory)
        dma_bm_write(be & ~0xfff, buf + first, n - first, 1);
    else
        dma_bm_read(be & ~0xfff, buf + first, n - first, 1);
    return (be & ~0xfff) + (n - first);
}

/* A TD done: onto the done queue, the ED's head moved past it (halted on an
   error), and the done interrupt brought forward as its DelayInterrupt
   asks (s6.4.4). */
static void
td_retire(ohci_t *dev, uint32_t ed_addr, uint32_t *ed, uint32_t td_addr, uint32_t *td, int halt)
{
    uint32_t next = td[2] & ~0xf;
    int      di   = (td[0] & TD_DI) >> 21;

    td[2] = dev->done_head;
    dev->done_head = td_addr;
    ed[2] = next | (ed[2] & ED_C) | (halt ? ED_H : 0);
    wr_dword(ed_addr + 8, ed[2]);
    if ((di != 7) && (di < dev->di_count))
        dev->di_count = di;
}

/* One transaction of the general TD at an ED's head. */
static int
service_gtd(ohci_t *dev, uint32_t ed_addr, uint32_t *ed)
{
    uint32_t       td_addr = ed[2] & ~0xf;
    uint32_t       td[4];
    int            fa  = ed[0] & 0x7f;
    int            en  = (ed[0] >> 7) & 0xf;
    int            ls  = !!(ed[0] & ED_S);
    int            mps = (ed[0] >> 16) & 0x7ff;
    int            dir = (ed[0] & ED_D) >> 11;
    int            remaining, len, in, toggle, ret, cc;
    uint8_t        pid;
    uint8_t        buf[1024];
    usbn_device_t *d;

    rd_dwords(td_addr, td, 4);
    if ((dir == 0) || (dir == 3))
        dir = (td[0] & TD_DP) >> 19;
    switch (dir) {
        case 0:
            pid = USB_PID_SETUP;
            break;
        case 1:
            pid = USB_PID_OUT;
            break;
        case 2:
            pid = USB_PID_IN;
            break;
        default:
            /* A reserved direction: the TD fails at once. */
            td[0] = (td[0] & ~TD_CC) | (0x7u << 28);   /* UnexpectedPID */
            wr_dword(td_addr, td[0]);
            td_retire(dev, ed_addr, ed, td_addr, td, 1);
            wr_dword(td_addr + 8, td[2]);
            return ED_SENT;
    }
    in        = (pid == USB_PID_IN);
    remaining = td_remaining(td[1], td[3]);
    len       = (remaining < mps) ? remaining : mps;
    if (len > (int) sizeof(buf))
        len = sizeof(buf);

    /* Nothing starts that cannot end in this frame, and a low-speed one not
       below LSThreshold (s7.3.5). */
    if ((dev->bits_left < bus_bits(ls, 0, in, len)) || (ls && (dev->bits_left < (double) dev->ls_threshold)))
        return ED_NO_TIME;
    dev->bits_left -= bus_bits(ls, 0, in, len);

    toggle = (td[0] & 0x02000000) ? ((td[0] >> 24) & 1) : !!(ed[2] & ED_C);
    d      = find_device(dev, fa);
    if (d == NULL) {
        usbn_trace("OHCI: no device at address %d (pid %02X ep %d): not responding", fa, pid, en);
        ret = USBN_NODEV;
    } else if (in) {
        ret = d->packet(d, pid, en, buf, len);
        if (ret > len)
            ret = USBN_BABBLE;
    } else {
        if (len)
            td_data(td[1], td[3], buf, len, 0);
        ret = d->packet(d, pid, en, buf, len);
        if (ret >= 0)
            ret = len;
    }

    if (ret == USBN_NAK)
        return ED_SENT;   /* the TD stays; the ED is asked again later */

    if (ret < 0) {
        cc = (ret == USBN_STALL) ? CC_STALL : ((ret == USBN_BABBLE) ? CC_DATAOVERRUN : CC_NOTRESPONDING);
        td[0] = (td[0] & ~(TD_CC | TD_EC)) | ((uint32_t) cc << 28) | (((ret == USBN_STALL) ? 0 : 3u) << 26);
        wr_dword(td_addr, td[0]);
        td_retire(dev, ed_addr, ed, td_addr, td, 1);
        wr_dword(td_addr + 8, td[2]);
        return ED_SENT;
    }

    /* Data moved: CBP past it, the toggle flips, the error count clears. */
    if (in)
        td[1] = td_data(td[1], td[3], buf, ret, 1);
    else
        td[1] = td_advance(td[1], td[3], ret);
    toggle ^= 1;
    td[0] = (td[0] & ~(TD_T | TD_EC)) | 0x02000000 | ((uint32_t) toggle << 24);
    ed[2] = (ed[2] & ~ED_C) | (toggle ? ED_C : 0);

    remaining -= ret;
    if ((remaining == 0) || (in && (ret < len))) {
        /* Done, or a short packet: an underrun unless BufferRounding. */
        cc = CC_NOERROR;
        if (remaining == 0)
            td[1] = 0;
        else if (!(td[0] & TD_R))
            cc = CC_DATAUNDERRUN;
        td[0] = (td[0] & ~TD_CC) | ((uint32_t) cc << 28);
        wr_dword(td_addr, td[0]);
        wr_dword(td_addr + 4, td[1]);
        td_retire(dev, ed_addr, ed, td_addr, td, cc != CC_NOERROR);
        wr_dword(td_addr + 8, td[2]);
        return ED_SENT;
    }

    wr_dword(td_addr, td[0]);
    wr_dword(td_addr + 4, td[1]);
    wr_dword(ed_addr + 8, ed[2]);
    return ED_SENT;
}

/* One packet of the isochronous TD at an ED's head, in the frame it is for
   (s4.3.2): too early waits, too late retires with DataOverrun. */
static int
service_itd(ohci_t *dev, uint32_t ed_addr, uint32_t *ed)
{
    uint32_t       td_addr = ed[2] & ~0x1f;
    uint32_t       td[8];
    int            fa  = ed[0] & 0x7f;
    int            en  = (ed[0] >> 7) & 0xf;
    int            mps = (ed[0] >> 16) & 0x7ff;
    int            dir = (ed[0] & ED_D) >> 11;
    int            fc, r, start, end, size, n = 0, cc = CC_NOERROR;
    uint16_t       psw;
    uint8_t        buf[1024];
    uint32_t       bp0, be, addr;
    usbn_device_t *d;

    rd_dwords(td_addr, td, 8);
    fc = (td[0] >> 24) & 7;
    r  = (int16_t) (dev->fm_number - (td[0] & 0xffff));
    if (r < 0)
        return ED_NONE;
    if (r > fc) {
        td[0] = (td[0] & ~TD_CC) | ((uint32_t) CC_DATAOVERRUN << 28);
        wr_dword(td_addr, td[0]);
        td_retire(dev, ed_addr, ed, td_addr, td, 0);
        wr_dword(td_addr + 8, td[2]);
        return ED_SENT;
    }

    bp0   = td[1] & ~0xfff;
    be    = td[3];
    psw   = (td[4 + (r >> 1)] >> ((r & 1) * 16)) & 0xffff;
    start = psw & 0x1fff;
    if (r < fc)
        end = ((td[4 + ((r + 1) >> 1)] >> (((r + 1) & 1) * 16)) & 0x1fff) - 1;
    else
        end = ((be & ~0xfff) != bp0) ? (0x1000 | (be & 0xfff)) : (be & 0xfff);
    size = end - start + 1;
    if ((size < 0) || (size > 1023) || (size > mps))
        size = (size < 0) ? 0 : ((size > mps) ? mps : 1023);

    if (dev->bits_left < bus_bits(0, 1, dir == 2, size))
        return ED_NO_TIME;
    dev->bits_left -= bus_bits(0, 1, dir == 2, size);

    addr = ((start & 0x1000) ? (be & ~0xfff) : bp0) | (start & 0xfff);
    d    = find_device(dev, fa);
    if (d && d->iso) {
        if (dir == 2) {
            n = d->iso(d, USB_PID_IN, en, buf, size);
            if (n > size)
                n = size;
            if (n > 0)
                td_data(addr, be, buf, n, 1);   /* may cross into BE's page */
            if (n < 0)
                n = 0;
        } else {
            if (size)
                td_data(addr, be, buf, size, 0);
            d->iso(d, USB_PID_OUT, en, buf, size);
            n = 0;
        }
    } else if (d == NULL)
        cc = CC_NOTRESPONDING;
    psw = (uint16_t) ((cc << 12) | (n & 0x7ff));
    td[4 + (r >> 1)] = (td[4 + (r >> 1)] & ~(0xffffu << ((r & 1) * 16))) | ((uint32_t) psw << ((r & 1) * 16));
    wr_dword(td_addr + 16 + ((r >> 1) << 2), td[4 + (r >> 1)]);

    if (r == fc) {
        td[0] = (td[0] & ~TD_CC) | ((uint32_t) CC_NOERROR << 28);
        wr_dword(td_addr, td[0]);
        td_retire(dev, ed_addr, ed, td_addr, td, 0);
        wr_dword(td_addr + 8, td[2]);
    }
    return ED_SENT;
}

static int
service_ed(ohci_t *dev, uint32_t ed_addr, uint32_t *ed, int periodic)
{
    dev->bits_left -= FETCH_BITS;
    if ((ed[0] & ED_K) || (ed[2] & ED_H) || ((ed[2] & ~0xf) == (ed[1] & ~0xf)))
        return ED_NONE;
    if (ed[0] & ED_F) {
        if (!periodic || !(dev->control & CTL_IE))
            return ED_NONE;
        return service_itd(dev, ed_addr, ed);
    }
    return service_gtd(dev, ed_addr, ed);
}

/* One ED of the control or bulk list at its current pointer: the list's
   Filled bit restarts it from its head at the end (s6.4.2.2, s7.1.3). */
static int
serve_list(ohci_t *dev, int bulk)
{
    uint32_t *cur  = bulk ? &dev->bulk_cur : &dev->ctrl_cur;
    uint32_t  head = bulk ? dev->bulk_head : dev->ctrl_head;
    uint32_t  lf   = bulk ? CS_BLF : CS_CLF;
    uint32_t  ed[4];
    int       res;

    if (!(dev->control & (bulk ? CTL_BLE : CTL_CLE)))
        return -1;
    if (*cur == 0) {
        if (!(dev->cmdsts & lf) || (head == 0))
            return -1;
        dev->cmdsts &= ~lf;
        *cur = head;
    }
    rd_dwords(*cur, ed, 4);
    res = service_ed(dev, *cur, ed, 0);
    if (res == ED_NO_TIME)
        return ED_NO_TIME;
    if (res == ED_SENT)
        dev->cmdsts |= lf;   /* a TD was found: the list goes on */
    *cur = ed[3] & ~0xf;
    return res;
}

/* The control and bulk lists, ControlBulkServiceRatio + 1 control EDs to a
   bulk ED, until the frame's bit time reaches stop. */
static void
run_nonperiodic(ohci_t *dev, double stop)
{
    for (int guard = 0; (guard < 1024) && (dev->bits_left > stop); guard++) {
        int c = -1, b = -1;

        if (dev->ctrl_served <= (int) (dev->control & CTL_CBSR)) {
            c = serve_list(dev, 0);
            if (c >= 0)
                dev->ctrl_served++;
        }
        if ((c < 0) || (dev->ctrl_served > (int) (dev->control & CTL_CBSR))) {
            b = serve_list(dev, 1);
            dev->ctrl_served = 0;
            if ((c < 0) && (b < 0))
                break;     /* both lists idle */
        }
        if ((c == ED_NO_TIME) || (b == ED_NO_TIME))
            break;
    }
}

/* The periodic list of this frame: the interrupt table entry, its EDs to the
   end (s6.4.1).  Running out of time is a SchedulingOverrun. */
static void
run_periodic(ohci_t *dev)
{
    uint32_t ed_addr;
    uint32_t ed[4];

    if (!(dev->control & CTL_PLE))
        return;
    rd_dwords(dev->hcca + ((dev->fm_number & 0x1f) << 2), &ed_addr, 1);
    ed_addr &= ~0xf;
    for (int guard = 0; (guard < 1024) && ed_addr; guard++) {
        dev->period_cur = ed_addr;
        rd_dwords(ed_addr, ed, 4);
        if (service_ed(dev, ed_addr, ed, 1) == ED_NO_TIME) {
            dev->so_pending = 1;
            break;
        }
        ed_addr = ed[3] & ~0xf;
    }
    dev->period_cur = 0;
}

static void
run_frame(ohci_t *dev)
{
    if (!dev->bus_master)
        return;
    dev->bits_left = (double) dev->fr_load + 1.0 - SOF_BITS;
    run_nonperiodic(dev, (double) dev->periodic_start + 1.0);
    if (dev->bits_left > ((double) dev->periodic_start + 1.0))
        dev->bits_left = (double) dev->periodic_start + 1.0;
    run_periodic(dev);
    run_nonperiodic(dev, 0.0);
}

/* ------------------------------------------------------------ frames --- */

static double
frame_us(uint32_t fi)
{
    return ((double) (fi & 0x3fff) + 1.0) / 12.0;
}

/* FrameRemaining from the frame timer, held outside USBOPERATIONAL. */
static uint32_t
fm_remaining(ohci_t *dev)
{
    double   bits;
    uint32_t fr;

    if (((dev->control & CTL_HCFS) != HCFS_OPERATIONAL) || !dev->frame_running)
        return dev->fm_remaining;
    /* The timer's 32.32 time stamp to microseconds, 12 bit times each. */
    bits = (((double) timer_get_remaining_u64(&dev->frame_timer) / 4294967296.0) / (double) TIMER_USEC) * 12.0;
    fr   = (bits >= 1.0) ? (uint32_t) bits : 0;
    if (fr > dev->fr_load)
        fr = dev->fr_load;
    return (dev->fm_remaining & 0x80000000) | fr;
}

/* HccaDoneHead at a frame boundary, once the counter reached 0 and the HCD
   has taken the last one (s6.4.5). */
static void
done_writeback(ohci_t *dev)
{
    uint32_t head;

    if (dev->di_count != 0) {
        if (dev->di_count != 7)
            dev->di_count--;
        return;
    }
    if ((dev->intsts & INT_WDH) || !dev->bus_master)
        return;
    head = dev->done_head;
    if (dev->intsts & dev->inten & INT_ALL & ~INT_WDH)
        head |= 1;
    wr_dword(dev->hcca + 0x84, head);
    dev->done_head = 0;
    dev->intsts |= INT_WDH;
    dev->di_count = 7;
}

/* Entering USBOPERATIONAL loads FrameRemaining and counts FrameNumber, with
   no SOF; the first one is at the next boundary (s6.2.1). */
static void
frame_start(ohci_t *dev)
{
    dev->fr_load       = dev->fm_interval & 0x3fff;
    dev->fm_remaining  = (dev->fm_remaining & 0x80000000) | dev->fr_load;
    dev->fm_number     = (dev->fm_number + 1) & 0xffff;
    dev->ctrl_served   = 0;
    dev->di_count      = 7;
    dev->frame_running = 1;
    timer_on_auto(&dev->frame_timer, frame_us(dev->fr_load));
}

static void
frame_stop(ohci_t *dev)
{
    dev->fm_remaining  = fm_remaining(dev);
    dev->frame_running = 0;
}

static void
frame_boundary(void *priv)
{
    ohci_t *dev = (ohci_t *) priv;

    if (dev->root)
        usbn_apply(dev);

    if (!dev->frame_running) {
        timer_on_auto(&dev->frame_timer, 1000.0);
        return;
    }

    dev->fr_load      = dev->fm_interval & 0x3fff;
    dev->fm_remaining = (dev->fm_interval & 0x80000000) | dev->fr_load;
    dev->fm_number    = (dev->fm_number + 1) & 0xffff;
    timer_on_auto(&dev->frame_timer, frame_us(dev->fr_load));

    /* SOF, HccaFrameNumber (Pad1 0), StartOfFrame (s4.4, s6.3.1). */
    if (dev->bus_master)
        wr_dword(dev->hcca + 0x80, dev->fm_number & 0xffff);
    if ((dev->fm_number & 0x8000) != dev->fn_msb) {
        dev->fn_msb = dev->fm_number & 0x8000;
        dev->intsts |= INT_FNO;
    }
    if (dev->so_pending) {
        dev->so_pending = 0;
        dev->intsts |= INT_SO;
        dev->cmdsts = (dev->cmdsts & ~CS_SOC) | ((dev->cmdsts + 0x00010000) & CS_SOC);
    }
    dev->intsts |= INT_SF;
    done_writeback(dev);

    run_frame(dev);
    ohci_update_irq(dev);
}

/* ---------------------------------------------------------- registers --- */

/* HostControllerReset: USBSUSPEND, the operational registers to their reset
   values, FrameInterval nominal; InterruptRouting, RemoteWakeupConnected,
   the root hub and the legacy registers are kept (s7.1.2-3). */
static void
soft_reset(ohci_t *dev)
{
    frame_stop(dev);
    dev->so_pending     = 0;
    dev->di_count       = 7;
    dev->fn_msb         = 0;
    dev->fr_load        = 0;
    dev->control        = (dev->control & (CTL_IR | CTL_RWC)) | HCFS_SUSPEND;
    suspend_settle(dev, 1);
    dev->cmdsts         = 0;
    dev->intsts         = 0;
    dev->inten          = 0;
    dev->hcca           = 0;
    dev->period_cur     = 0;
    dev->ctrl_head      = 0;
    dev->ctrl_cur       = 0;
    dev->bulk_head      = 0;
    dev->bulk_cur       = 0;
    dev->done_head      = 0;
    dev->fm_interval    = FI_NOMINAL;
    dev->fm_remaining   = 0;
    dev->fm_number      = 0;
    dev->periodic_start = 0;
    dev->ls_threshold   = (dev->chip_flags & OHCI_CHIP_LST_0628H) ? 0x628 : 0;
}

static void
hard_reset(ohci_t *dev)
{
    soft_reset(dev);
    dev->control = HCFS_RESET;
    suspend_settle(dev, 0);
    dev->rh_desc_a = 0x01000000 | dev->ndp;
    dev->rh_desc_b = 0;
    root_hub_reset(dev);
    dev->hce_control = 0;
    dev->hce_input   = 0;
    dev->hce_output  = 0;
    dev->hce_status  = 0;
}

static void
control_write(ohci_t *dev, uint32_t val)
{
    uint32_t old  = dev->control;
    uint32_t mask = CTL_CBSR | CTL_PLE | CTL_IE | CTL_CLE | CTL_BLE | CTL_HCFS | CTL_IR | CTL_RWE;

    if (dev->chip_flags & OHCI_CHIP_RWC)
        mask |= CTL_RWC;
    if (dev->chip_flags & OHCI_CHIP_RWE_RO)
        mask &= ~CTL_RWE;
    dev->control = val & mask;

    /* The InterruptRouting change that hands the HC over ends
       OwnershipChangeRequest (s5.1.1.3.3). */
    if (((dev->control ^ old) & CTL_IR) && (dev->cmdsts & CS_OCR))
        dev->cmdsts &= ~CS_OCR;

    if ((dev->control ^ old) & CTL_HCFS) {
        usbn_trace("OHCI: HcControl HCFS %X -> %X", (old & CTL_HCFS) >> 6, (dev->control & CTL_HCFS) >> 6);
        if ((old & CTL_HCFS) == HCFS_OPERATIONAL)
            frame_stop(dev);
        else if ((dev->control & CTL_HCFS) == HCFS_OPERATIONAL)
            frame_start(dev);
        suspend_settle(dev, (dev->control & CTL_HCFS) == HCFS_SUSPEND);
        switch (dev->control & CTL_HCFS) {
            case HCFS_RESET:
                root_hub_reset(dev);
                break;
            case HCFS_RESUME:
                rh_global_resume(dev);
                break;
            case HCFS_SUSPEND:
                rh_suspend(dev);
                break;
            default:
                break;
        }
        if ((old & CTL_HCFS) == HCFS_RESET)
            rh_detect_all(dev);
    }
}

static int
emulation_interrupt(const ohci_t *dev)
{
    uint32_t ctl = dev->hce_control;

    if ((ctl & HCE_EE) && (ctl & HCE_CP) && !(dev->hce_status & HCES_OUTFULL))
        return 1;
    if ((ctl & HCE_EE) && (dev->hce_status & HCES_INFULL))
        return 1;
    return (ctl & HCE_EXTIRQEN) && (ctl & (HCE_IRQ1ACT | HCE_IRQ12ACT));
}

static uint32_t
reg_read(ohci_t *dev, uint32_t off)
{
    uint32_t n;

    switch (off) {
        case REG_REVISION:
            return 0x00000110;   /* 1.0, legacy support present */
        case REG_CONTROL:
            return dev->control;
        case REG_CMDSTATUS:
            return dev->cmdsts;
        case REG_INTSTATUS:
            return dev->intsts;
        case REG_INTENABLE:
        case REG_INTDISABLE:
            return dev->inten;
        case REG_HCCA:
            return dev->hcca;
        case REG_PERIODCURED:
            return dev->period_cur;
        case REG_CTRLHEADED:
            return dev->ctrl_head;
        case REG_CTRLCURED:
            return dev->ctrl_cur;
        case REG_BULKHEADED:
            return dev->bulk_head;
        case REG_BULKCURED:
            return dev->bulk_cur;
        case REG_DONEHEAD:
            return dev->done_head;
        case REG_FMINTERVAL:
            return dev->fm_interval;
        case REG_FMREMAINING:
            return fm_remaining(dev);
        case REG_FMNUMBER:
            return dev->fm_number;
        case REG_PERIODICSTART:
            return dev->periodic_start;
        case REG_LSTHRESHOLD:
            return dev->ls_threshold;
        case REG_RHDESCA:
            return dev->rh_desc_a;
        case REG_RHDESCB:
            return dev->rh_desc_b;
        case REG_RHSTATUS:
            return dev->rh_status;
        case REG_HCECONTROL:
            return dev->hce_control | (emulation_interrupt(dev) ? HCE_EI : 0);
        case REG_HCEINPUT:
            return dev->hce_input;
        case REG_HCEOUTPUT:
            return dev->hce_output;
        case REG_HCESTATUS:
            return dev->hce_status;
        default:
            n = (off - REG_RHPORT1) >> 2;
            if ((off >= REG_RHPORT1) && (n < (uint32_t) dev->ndp))
                return port_read(&dev->port[n]);
            return 0;
    }
}

static uint32_t
merge(uint32_t cur, uint32_t val, uint32_t lanes)
{
    return (cur & ~lanes) | (val & lanes);
}

static uint32_t
inten_mask(const ohci_t *dev)
{
    uint32_t mask = INT_ALL | INT_MIE;

    return (dev->chip_flags & OHCI_CHIP_UE_RW) ? mask : (mask & ~INT_UE);
}

/* lanes: the bytes written (the byte enables); unwritten bytes write 0 to
   the command bits. */
static void
reg_write(ohci_t *dev, uint32_t off, uint32_t val, uint32_t lanes)
{
    uint32_t cmd = val & lanes;
    uint32_t ports;
    uint32_t n;

    switch (off) {
        case REG_CONTROL:
            control_write(dev, merge(dev->control, val, lanes));
            break;
        case REG_CMDSTATUS:
            if (cmd & CS_HCR) {
                int was_reset = rh_in_reset(dev);

                usbn_trace("OHCI: HostControllerReset");
                soft_reset(dev);
                if (was_reset)
                    rh_detect_all(dev);
                break;
            }
            dev->cmdsts |= cmd & (CS_CLF | CS_BLF | CS_OCR);
            if ((cmd & CS_OCR) && !(dev->chip_flags & OHCI_CHIP_NO_SMI))
                dev->intsts |= INT_OC;
            break;
        case REG_INTSTATUS:
            dev->intsts &= ~(cmd & INT_ALL);
            break;
        case REG_INTENABLE:
            dev->inten |= cmd & inten_mask(dev);
            break;
        case REG_INTDISABLE:
            dev->inten &= ~(cmd & inten_mask(dev));
            break;
        case REG_HCCA:
            dev->hcca = merge(dev->hcca, val, lanes) & 0xffffff00;
            break;
        case REG_PERIODCURED:
            if (!(dev->chip_flags & OHCI_CHIP_PTRS_RO))
                dev->period_cur = merge(dev->period_cur, val, lanes) & 0xfffffff0;
            break;
        case REG_CTRLHEADED:
            dev->ctrl_head = merge(dev->ctrl_head, val, lanes) & 0xfffffff0;
            break;
        case REG_CTRLCURED:
            dev->ctrl_cur = merge(dev->ctrl_cur, val, lanes) & 0xfffffff0;
            break;
        case REG_BULKHEADED:
            dev->bulk_head = merge(dev->bulk_head, val, lanes) & 0xfffffff0;
            break;
        case REG_BULKCURED:
            dev->bulk_cur = merge(dev->bulk_cur, val, lanes) & 0xfffffff0;
            break;
        case REG_DONEHEAD:
            if (!(dev->chip_flags & OHCI_CHIP_PTRS_RO))
                dev->done_head = merge(dev->done_head, val, lanes) & 0xfffffff0;
            break;
        case REG_FMINTERVAL:
            dev->fm_interval = merge(dev->fm_interval, val, lanes) & 0xffff3fff;
            break;
        case REG_PERIODICSTART:
            dev->periodic_start = merge(dev->periodic_start, val, lanes) & 0x3fff;
            break;
        case REG_LSTHRESHOLD:
            dev->ls_threshold = merge(dev->ls_threshold, val, lanes) & 0xfff;
            break;
        case REG_RHDESCA:
            /* PSM, NPS, OCPM, NOCP and POTPGT (all of it, or 25:24). */
            ports = 0;
            for (int i = 0; i < dev->ndp; i++)
                ports |= port_powered(&dev->port[i]) << i;
            dev->rh_desc_a = (merge(dev->rh_desc_a, val, lanes) &
                              ((dev->chip_flags & OHCI_CHIP_POTPGT_RW) ? 0xff001b00 : 0x03001b00)) | dev->ndp;
            for (int i = 0; i < dev->ndp; i++) {
                if ((ports & (1 << i)) && !port_powered(&dev->port[i]))
                    port_power_off(&dev->port[i]);
                else
                    port_detect(&dev->port[i]);
            }
            break;
        case REG_RHDESCB:
            ports          = ((1u << dev->ndp) - 1) << 1;
            dev->rh_desc_b = merge(dev->rh_desc_b, val, lanes) & (ports | (ports << 16));
            break;
        case REG_RHSTATUS:
            rh_status_write(dev, cmd);
            break;
        case REG_HCECONTROL:
            dev->hce_control = (merge(dev->hce_control, val, lanes) & 0x0000013d) |
                               (dev->hce_control & ~cmd & (HCE_IRQ1ACT | HCE_IRQ12ACT));
            break;
        case REG_HCEINPUT:
            if (lanes & 0xff)
                dev->hce_input = val;
            break;
        case REG_HCEOUTPUT:
            if (lanes & 0xff)
                dev->hce_output = val;
            break;
        case REG_HCESTATUS:
            if (lanes & 0xff)
                dev->hce_status = val;
            break;
        default:
            n = (off - REG_RHPORT1) >> 2;
            if ((off >= REG_RHPORT1) && (n < (uint32_t) dev->ndp))
                port_write(&dev->port[n], cmd);
            break;
    }
    ohci_update_irq(dev);
}

static uint8_t
mmio_readb(uint32_t addr, void *priv)
{
    uint32_t off = addr & 0xfff;

    return reg_read((ohci_t *) priv, off & 0xffc) >> ((off & 3) << 3);
}

static uint16_t
mmio_readw(uint32_t addr, void *priv)
{
    ohci_t  *dev = (ohci_t *) priv;
    uint32_t off = addr & 0xfff;

    if ((off & 3) == 3)
        return (reg_read(dev, off & 0xffc) >> 24) | (reg_read(dev, (off + 1) & 0xffc) << 8);
    return reg_read(dev, off & 0xffc) >> ((off & 3) << 3);
}

static uint32_t
mmio_readl(uint32_t addr, void *priv)
{
    return reg_read((ohci_t *) priv, addr & 0xffc);
}

static void
mmio_writeb(uint32_t addr, uint8_t val, void *priv)
{
    uint32_t off = addr & 0xfff;
    int      sh  = (off & 3) << 3;

    reg_write((ohci_t *) priv, off & 0xffc, (uint32_t) val << sh, 0xffu << sh);
}

static void
mmio_writew(uint32_t addr, uint16_t val, void *priv)
{
    ohci_t  *dev = (ohci_t *) priv;
    uint32_t off = addr & 0xfff;
    int      sh  = (off & 3) << 3;

    if ((off & 3) == 3) {
        reg_write(dev, off & 0xffc, (uint32_t) val << 24, 0xff000000);
        reg_write(dev, (off + 1) & 0xffc, val >> 8, 0x000000ff);
    } else
        reg_write(dev, off & 0xffc, (uint32_t) val << sh, 0xffffu << sh);
}

static void
mmio_writel(uint32_t addr, uint32_t val, void *priv)
{
    reg_write((ohci_t *) priv, addr & 0xffc, val, 0xffffffff);
}

/* ------------------------------------------------------------ the bus --- */

static void
root_connect(void *priv, int port, usbn_device_t *d)
{
    ohci_t *dev = (ohci_t *) priv;

    if (port < OHCI_MAX_PORTS)
        port_attach(&dev->port[port], d);
}

static void
root_disconnect(void *priv, int port)
{
    ohci_t *dev = (ohci_t *) priv;

    if ((port < OHCI_MAX_PORTS) && dev->port[port].dev)
        port_detach(&dev->port[port]);
}

static void
root_resume(void *priv, int port)
{
    ohci_t *dev = (ohci_t *) priv;

    if ((port < OHCI_MAX_PORTS) && dev->port[port].dev)
        port_remote_wakeup(&dev->port[port]);
}

static const usbn_root_ops_t ohci_root_ops = { root_connect, root_disconnect, root_resume };

/* ------------------------------------------------------- the interface --- */

ohci_t *
ohci_core_create(void)
{
    ohci_t *dev = calloc(1, sizeof(ohci_t));

    dev->ndp   = 2;
    dev->ports = 2;
    for (int i = 0; i < OHCI_MAX_PORTS; i++) {
        dev->port[i].num = i;
        dev->port[i].hc  = dev;
        timer_add(&dev->port[i].timer, port_timer, &dev->port[i], 0);
    }
    timer_add(&dev->frame_timer, frame_boundary, dev, 0);
    timer_add(&dev->suspend_timer, suspend_settled, dev, 0);
    mem_mapping_add(&dev->mmio, 0, 0, mmio_readb, mmio_readw, mmio_readl,
                    mmio_writeb, mmio_writew, mmio_writel, NULL, MEM_MAPPING_EXTERNAL, dev);
    mem_mapping_disable(&dev->mmio);
    hard_reset(dev);
    timer_on_auto(&dev->frame_timer, 1000.0);

    dev->root = usbn_set_root(&ohci_root_ops, dev, 0);
    if (dev->root)
        usbn_restore_ports();
    return dev;
}

void
ohci_core_close(ohci_t *dev)
{
    for (int i = 0; i < OHCI_MAX_PORTS; i++)
        timer_disable(&dev->port[i].timer);
    timer_disable(&dev->frame_timer);
    timer_disable(&dev->suspend_timer);
    mem_mapping_disable(&dev->mmio);
    if (dev->root)
        usbn_set_root(NULL, dev, 0);
    free(dev);
}

/* A PCI reset: the hardware reset, Bus Master and Memory Access off (or
   Bus Master on where the chip ties it so).  Devices stay plugged in. */
void
ohci_core_reset(ohci_t *dev)
{
    hard_reset(dev);
    dev->bus_master = dev->bm_hardwired;
    dev->mem_access = 0;
    if (dev->mapped)
        mem_mapping_disable(&dev->mmio);
    dev->mapped = 0;
    ohci_update_irq(dev);
}

void
ohci_core_set_window(ohci_t *dev, uint32_t base, int enable)
{
    if (dev->mapped)
        mem_mapping_disable(&dev->mmio);
    dev->mapped   = 0;
    dev->mem_base = base & 0xfffff000;
    if (enable && dev->mem_base) {
        mem_mapping_set_addr(&dev->mmio, dev->mem_base, 0x1000);
        dev->mapped = 1;
    }
    dev->mem_access = !!enable;
    ohci_update_irq(dev);
}

void
ohci_core_set_irq(ohci_t *dev, void (*set_irq)(void *priv, int level), void *priv)
{
    dev->set_irq  = set_irq;
    dev->irq_priv = priv;
}

void
ohci_core_set_smi(ohci_t *dev, void (*set_smi)(void *priv, int level), void *priv)
{
    dev->set_smi  = set_smi;
    dev->smi_priv = priv;
}

void
ohci_core_set_bus_master(ohci_t *dev, int enable, int hardwired)
{
    if (hardwired)
        dev->bm_hardwired = 1;
    dev->bus_master = dev->bm_hardwired || !!enable;
}

void
ohci_core_set_chip_flags(ohci_t *dev, uint32_t flags)
{
    dev->chip_flags   = flags;
    dev->ls_threshold = (flags & OHCI_CHIP_LST_0628H) ? 0x628 : 0;
}

/* NDP of the connectors are implemented; a port dropped goes to its reset
   value with no change reported, one brought in starts from reset. */
void
ohci_core_set_ndp(ohci_t *dev, int ndp, int ports)
{
    int      old = dev->ndp;
    uint32_t pps = 0;
    uint32_t mask;

    if (ndp > OHCI_MAX_PORTS)
        ndp = OHCI_MAX_PORTS;
    dev->ports = ports;
    if (ndp == old)
        return;
    for (int i = 0; i < old; i++) {
        if (!(dev->rh_desc_a & DA_PSM) || !(dev->rh_desc_b & (0x10000 << (i + 1)))) {
            pps = dev->port[i].sts & PS_PPS;
            break;
        }
    }
    for (int i = ndp; i < old; i++) {
        port_timer_stop(&dev->port[i]);
        dev->port[i].sts = 0;
        if (dev->port[i].dev)
            dev_bus_reset(dev->port[i].dev);
    }
    dev->ndp       = ndp;
    mask           = ((1u << ndp) - 1) << 1;
    dev->rh_desc_a = (dev->rh_desc_a & ~0xffu) | ndp;
    dev->rh_desc_b &= mask | (mask << 16);
    for (int i = old; i < ndp; i++) {
        dev->port[i].sts = pps;
        port_detect(&dev->port[i]);
    }
    ohci_update_irq(dev);
}

/* For the tests: the register window without a memory mapping. */
uint32_t
ohci_core_read(ohci_t *dev, uint32_t off)
{
    return reg_read(dev, off & 0xffc);
}

void
ohci_core_write(ohci_t *dev, uint32_t off, uint32_t val)
{
    reg_write(dev, off & 0xffc, val, 0xffffffff);
}
