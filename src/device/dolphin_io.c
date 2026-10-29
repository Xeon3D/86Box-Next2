/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             Midway "Dolphin" I/O board, ISA variant, of Midway
 *             TouchMaster II (FreeBSD 3.2, 2000-2001): a 64K memory window
 *             at D0000h and IRQ 5.  Only what the TouchMaster software
 *             depends on is modelled (registers from Midway's dolphin.c,
 *             olib.c, coinmgr.c, io.c, diag.c, dl.cc and menu.cpp):
 *
 *               20h+2n  16550-style UART, stub (LSR reads 60h)
 *               50h     word, board ID: 01h board, FPGA revision 02h
 *               58h     bit-banged bus of the two AT88SC153-class chips:
 *                       bit 0 SDA, 1 SCL, 2 RST, 3 select U9, 4-5 select
 *                       the Abyss key, 6 host drives SDA
 *               60h     DIP switches, active low; bit 7 is Emergency
 *                       Restore (0 makes dl wipe /rw), so all off = FFh
 *               62h     switches, active low: coins 1-3, 6 Calibrate, 7 TEST
 *               64h     escrow inputs, active low
 *               66h     outputs (meters, lockouts)
 *               70h     interrupt cause (bit 1 UART)
 *               74h     word, control: bit 1 frame toggle (dl), bit 10
 *                       timer interrupt enable
 *               76h     word, timer setup (8010h: about 215 Hz)
 *
 *             The kernel's interrupt handler only wakes coinmgr, which
 *             acknowledges by clearing 74h bit 10, so the timer gives
 *             IRQ 5 an edge on every tick while bit 10 is set.
 *
 *             The chips (U9 on the board, the Abyss key in its socket) are
 *             256 bytes and a fuse byte each, kept in the machine's nvr
 *             folder as dolphin_u9.nvr / dolphin_abyss.nvr.  Without a file
 *             a chip starts blank, as Atmel shipped them to Midway, and the
 *             game serialises it itself (diag init, then ProgramAbyss).
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
#include <86box/device.h>
#include <86box/timer.h>
#include <86box/pic.h>
#include <86box/mem.h>
#include <86box/nvr.h>
#include <86box/plat.h>
#include <86box/plat_unused.h>
#include <86box/at88sc153.h>
#include <86box/dolphin_keygen.h>
#include <86box/dolphin_io.h>

#define DOL_BASE      0xd0000
#define DOL_SIZE      0x10000
#define DOL_IRQ       5
#define DOL_TICK_HZ   215.0
#define DOL_PULSE_MS  150
/* coinmgr counts board ticks while a coin line is low and credits it on
   release only for 6-30 ticks (coin 1), 6-90 (coin 2) or 2-90 (bill
   validator): 80 ms is about 17. */
#define DOL_COIN_MS   80

#define DOL_UART_BASE 0x20
#define DOL_ID        0x50
#define DOL_ATMEL     0x58
#define DOL_DIP       0x60
#define DOL_SW0       0x62
#define DOL_SW1       0x64
#define DOL_OUT       0x66
#define DOL_INTSTAT   0x70
#define DOL_CTRL      0x74
#define DOL_TIMER     0x76

#define DOL_CTRL_TIMER_IE 0x0400

#define DOL_A_SDA   0x01
#define DOL_A_SCL   0x02
#define DOL_A_RST   0x04
#define DOL_A_SEL0  0x08
#define DOL_A_SEL1  0x30
#define DOL_A_DRIVE 0x40

static const char *chip_files[2] = { "dolphin_u9.nvr", "dolphin_abyss.nvr" };
/* switch lines: register 62h or 64h, and the bit */
static const uint8_t line_regs[DOLPHIN_LINES] = { 0x62, 0x62, 0x62, 0x62, 0x62, 0x64, 0x64, 0x64, 0x64, 0x64, 0x64 };
static const uint8_t line_bits[DOLPHIN_LINES] = { 0x01, 0x02, 0x04, 0x40, 0x80, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40 };

typedef struct dolphin_io_t {
    mem_mapping_t win;
    pc_timer_t    tick;

    at88sc153_t chip[2]; /* [0] U9 "Dolphin", [1] the Abyss key */
    uint16_t    id;
    uint8_t     dip;
    uint8_t     sw1;
    uint8_t     out;
    uint8_t     intstat;
    uint16_t    ctrl;
    uint16_t    timer;
    uint8_t     atmel;
    uint8_t     uart[8];

    int line_ticks[DOLPHIN_LINES]; /* ticks left on a pressed switch */
} dolphin_io_t;

static dolphin_io_t *dol_inst;

/* Chip images from the key generator, handed to the emulation thread. */
static uint8_t      keys_img[2][AT88_SIZE + 1];
static volatile int keys_pending;

#ifdef ENABLE_DOLPHIN_IO_LOG
int dolphin_io_do_log = ENABLE_DOLPHIN_IO_LOG;

static void
dolphin_io_log(const char *fmt, ...)
{
    va_list ap;

    if (dolphin_io_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define dolphin_io_log(fmt, ...)
#endif

static void
chip_load(dolphin_io_t *dev, int n)
{
    at88sc153_t *c = &dev->chip[n];
    uint8_t      img[AT88_SIZE + 1];
    FILE        *f;

    at88_init_blank(c);
    f = nvr_fopen(chip_files[n], "rb");
    if (!f) {
        pclog("Dolphin I/O: %s not found, blank chip\n", chip_files[n]);
        return;
    }
    if (fread(img, 1, sizeof(img), f) >= AT88_SIZE) {
        memcpy(c->mem, img, AT88_SIZE);
        c->fuses = img[AT88_SIZE];
        pclog("Dolphin I/O: %s loaded\n", chip_files[n]);
    } else
        pclog("Dolphin I/O: %s is short, blank chip\n", chip_files[n]);
    fclose(f);
}

static void
chip_save(dolphin_io_t *dev, int n)
{
    at88sc153_t *c = &dev->chip[n];
    FILE        *f;

    if (!c->dirty || !c->present)
        return;
    f = nvr_fopen(chip_files[n], "wb");
    if (f) {
        fwrite(c->mem, 1, AT88_SIZE, f);
        fputc(c->fuses, f);
        fclose(f);
    }
    c->dirty = 0;
}

static void
atmel_write(dolphin_io_t *dev, uint8_t val)
{
    int sda = (val & DOL_A_DRIVE) ? (val & DOL_A_SDA) : 1;
    int scl = !!(val & DOL_A_SCL);
    int rst = !!(val & DOL_A_RST);

    dev->atmel = val;
    if (val & DOL_A_SEL0)
        at88_pins(&dev->chip[0], scl, sda, rst);
    if (val & DOL_A_SEL1)
        at88_pins(&dev->chip[1], scl, sda, rst);
}

static uint8_t
atmel_read(dolphin_io_t *dev)
{
    int sda = (dev->atmel & DOL_A_DRIVE) ? (dev->atmel & DOL_A_SDA) : 1;

    if (dev->atmel & DOL_A_SEL0)
        sda &= at88_sda(&dev->chip[0]);
    if (dev->atmel & DOL_A_SEL1)
        sda &= at88_sda(&dev->chip[1]);
    return (dev->atmel & ~DOL_A_SDA) | (uint8_t) sda;
}

static uint8_t
sw_read(dolphin_io_t *dev, uint8_t reg, uint8_t v)
{
    for (int l = 0; l < DOLPHIN_LINES; l++)
        if ((line_regs[l] == reg) && (dev->line_ticks[l] > 0))
            v &= ~line_bits[l];
    return v;
}

static void
ctrl_write(dolphin_io_t *dev, uint16_t val)
{
    dev->ctrl = val;
    /* coinmgr acknowledges by clearing the enable */
    if (!(val & DOL_CTRL_TIMER_IE))
        picintc(1 << DOL_IRQ);
}

static uint8_t
dol_readb(uint32_t addr, void *priv)
{
    dolphin_io_t *dev = (dolphin_io_t *) priv;
    uint32_t      off = addr & 0xffff;

    if ((off >= DOL_UART_BASE) && (off < (DOL_UART_BASE + 16)))
        return (off & 1) ? 0xff : dev->uart[(off - DOL_UART_BASE) >> 1];

    switch (off) {
        case DOL_ID:
            return dev->id & 0xff;
        case DOL_ID + 1:
            return dev->id >> 8;
        case DOL_ATMEL:
            return atmel_read(dev);
        case DOL_DIP:
            return dev->dip;
        case DOL_SW0:
            return sw_read(dev, DOL_SW0, 0xff);
        case DOL_SW1:
            return sw_read(dev, DOL_SW1, dev->sw1);
        case DOL_OUT:
            return dev->out;
        case DOL_INTSTAT:
            return dev->intstat;
        case DOL_CTRL:
            return dev->ctrl & 0xff;
        case DOL_CTRL + 1:
            return dev->ctrl >> 8;
        case DOL_TIMER:
            return dev->timer & 0xff;
        case DOL_TIMER + 1:
            return dev->timer >> 8;
        default:
            break;
    }
    return 0xff;
}

static void
dol_writeb(uint32_t addr, uint8_t val, void *priv)
{
    dolphin_io_t *dev = (dolphin_io_t *) priv;
    uint32_t      off = addr & 0xffff;

    if ((off >= DOL_UART_BASE) && (off < (DOL_UART_BASE + 16))) {
        if (!(off & 1) && (((off - DOL_UART_BASE) >> 1) != 5))
            dev->uart[(off - DOL_UART_BASE) >> 1] = val;
        return;
    }

    switch (off) {
        case DOL_ATMEL:
            atmel_write(dev, val);
            break;
        case DOL_OUT:
            dev->out = val;
            break;
        case DOL_CTRL:
            ctrl_write(dev, (dev->ctrl & 0xff00) | val);
            break;
        case DOL_CTRL + 1:
            ctrl_write(dev, (dev->ctrl & 0x00ff) | (val << 8));
            break;
        case DOL_TIMER:
            dev->timer = (dev->timer & 0xff00) | val;
            break;
        case DOL_TIMER + 1:
            dev->timer = (dev->timer & 0x00ff) | (val << 8);
            break;
        default:
            break;
    }
}

static uint16_t
dol_readw(uint32_t addr, void *priv)
{
    return dol_readb(addr, priv) | (dol_readb(addr + 1, priv) << 8);
}

static void
dol_writew(uint32_t addr, uint16_t val, void *priv)
{
    dolphin_io_t *dev = (dolphin_io_t *) priv;
    uint32_t      off = addr & 0xffff;

    /* One 16-bit cycle: the control word must not be seen half-written. */
    if (off == DOL_CTRL) {
        ctrl_write(dev, val);
        return;
    }
    dol_writeb(addr, val & 0xff, priv);
    dol_writeb(addr + 1, val >> 8, priv);
}

/* New chips from the key generator: as if freshly programmed, and saved. */
static void
keys_apply(dolphin_io_t *dev)
{
    if (!keys_pending)
        return;
    for (int n = 0; n < 2; n++) {
        int present = dev->chip[n].present;

        at88_init_blank(&dev->chip[n]);
        memcpy(dev->chip[n].mem, keys_img[n], AT88_SIZE);
        dev->chip[n].fuses   = keys_img[n][AT88_SIZE];
        dev->chip[n].present = 1; /* saved even with the key out */
        dev->chip[n].dirty   = 1;
        chip_save(dev, n);
        dev->chip[n].present = present;
    }
    keys_pending = 0;
    pclog("Dolphin I/O: new chips from the key generator\n");
}

static void
dol_tick(void *priv)
{
    dolphin_io_t *dev = (dolphin_io_t *) priv;

    for (int l = 0; l < DOLPHIN_LINES; l++)
        if (dev->line_ticks[l] > 0)
            dev->line_ticks[l]--;

    if (dev->ctrl & DOL_CTRL_TIMER_IE) {
        /* an edge per tick */
        picintc(1 << DOL_IRQ);
        picint(1 << DOL_IRQ);
    }

    keys_apply(dev);

    /* persist programmed chips promptly; the game writes them rarely */
    chip_save(dev, 0);
    chip_save(dev, 1);

    timer_on_auto(&dev->tick, 1000000.0 / DOL_TICK_HZ);
}

void
dolphin_io_pulse(int line)
{
    if (!dol_inst || (line < 0) || (line >= DOLPHIN_LINES))
        return;
    dol_inst->line_ticks[line] = (int) ((((line <= DOLPHIN_LINE_COIN3) || (line >= DOLPHIN_LINE_ELEC_A)) ? DOL_COIN_MS : DOL_PULSE_MS) * DOL_TICK_HZ / 1000.0);
}

int
dolphin_io_present(void)
{
    return dol_inst != NULL;
}

int
dolphin_io_write_keys(uint64_t serial, unsigned country, unsigned major, unsigned minor)
{
    uint32_t now  = (uint32_t) time(NULL);
    uint32_t seed = now ^ (plat_get_ticks() * 2654435761U); /* srandomdev() */

    if (!dol_inst)
        return -2;
    if (keys_pending)
        return -3;
    if (dolphin_keygen(keys_img[0], keys_img[1], serial, country, major, minor, now, seed))
        return -1;
    keys_pending = 1;
    return 0;
}

int
dolphin_io_read_keys(uint64_t *serial, unsigned *country)
{
    uint8_t img[AT88_SIZE + 1];

    if (!dol_inst)
        return -1;
    memcpy(img, dol_inst->chip[0].mem, AT88_SIZE);
    img[AT88_SIZE] = dol_inst->chip[0].fuses;
    return dolphin_keygen_read_u9(img, serial, country);
}

static void *
dolphin_io_init(UNUSED(const device_t *info))
{
    dolphin_io_t *dev = calloc(1, sizeof(dolphin_io_t));

    dev->id      = 0x0102;
    dev->dip     = device_get_config_hex16("dip") & 0xff;
    dev->sw1     = 0xff;
    dev->uart[5] = 0x60; /* LSR: transmitter empty */

    chip_load(dev, 0);
    chip_load(dev, 1);
    dev->chip[1].present = device_get_config_int("abyss");

    mem_mapping_add(&dev->win, DOL_BASE, DOL_SIZE,
                    dol_readb, dol_readw, NULL, dol_writeb, dol_writew, NULL,
                    NULL, MEM_MAPPING_EXTERNAL, dev);

    timer_add(&dev->tick, dol_tick, dev, 0);
    timer_on_auto(&dev->tick, 1000000.0 / DOL_TICK_HZ);

    dol_inst = dev;
    pclog("Dolphin I/O: D0000h, IRQ %d, DIP %02X, Abyss key %s\n", DOL_IRQ, dev->dip,
          dev->chip[1].present ? "fitted" : "not fitted");
    dolphin_io_log("Dolphin I/O: init\n");

    return dev;
}

static void
dolphin_io_close(void *priv)
{
    dolphin_io_t *dev = (dolphin_io_t *) priv;

    keys_apply(dev); /* a reset right after the key generator */
    chip_save(dev, 0);
    chip_save(dev, 1);
    dol_inst = NULL;
    free(dev);
}

static const device_config_t dolphin_io_config[] = {
  // clang-format off
    {
        .name           = "abyss",
        .description    = "Abyss key fitted",
        .type           = CONFIG_BINARY,
        .default_string = NULL,
        .default_int    = 1,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    {
        .name           = "dip",
        .description    = "DIP switches (active low)",
        .type           = CONFIG_HEX16,
        .default_string = NULL,
        .default_int    = 0xff,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = {
            { .description = "FF: all off",                        .value = 0xff },
            { .description = "7F: DIP 8 on, Emergency Restore (wipes /rw)", .value = 0x7f },
            { .description = "FE: DIP 1 on, touch screen Y inverted", .value = 0xfe },
            { .description = ""                                                  }
        },
        .bios           = { { 0 } }
    },
    { .name = "", .description = "", .type = CONFIG_END }
  // clang-format on
};

const device_t dolphin_io_device = {
    .name          = "Midway Dolphin I/O Board (TouchMaster II)",
    .internal_name = "dolphin_io",
    .flags         = DEVICE_ISA,
    .local         = 0,
    .init          = dolphin_io_init,
    .close         = dolphin_io_close,
    .reset         = NULL,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = dolphin_io_config
};
