/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             Atmel AT88SC153-class secure memory model.  See at88sc153.h.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include <string.h>
#include <86box/at88sc153.h>

enum { ST_IDLE, ST_RX, ST_RX_ACK, ST_TX, ST_TX_ACK, ST_WAIT_STOP };

/* command decode: bits 1:0 = operation, bits 3:2 = zone / password set */
#define OP(c)      ((c) & 3)
#define OP_WRITE   0
#define OP_READ    1
#define OP_FUSE    2            /* 0xBA write fuses, 0xBE read fuses */
#define OP_VERIFY  3            /* 0xB3 set 0, 0xB7 set 1            */
#define SEL(c)     (((c) >> 2) & 3)

static const uint8_t pac_addr[4] = { AT88_PAC_W0, AT88_PAC_W1, AT88_PAC_R0, AT88_PAC_R1 };

void
at88_init_blank(at88sc153_t *c)
{
    static const uint8_t atr[8] = { 0x3B, 0xB2, 0x11, 0x00, 0x10, 0x80, 0x00, 0x01 };

    memset(c, 0, sizeof(*c));
    memset(c->mem, 0xFF, sizeof(c->mem));
    memcpy(&c->mem[AT88_ATR], atr, sizeof(atr));
    c->mem[AT88_FAB]     = 0xAE;
    c->mem[AT88_FAB + 1] = 0x60;
    c->mem[AT88_PW_W1]     = 0xD5;          /* transport secure code */
    c->mem[AT88_PW_W1 + 1] = 0xFE;
    c->mem[AT88_PW_W1 + 2] = 0x7E;
    c->fuses   = 0x07;
    c->present = 1;
    c->scl = c->sda_in = c->sda_out = 1;
    c->state = ST_IDLE;
}

static uint8_t
zone_addr(uint8_t base, int i)
{
    return (uint8_t) ((base & 0xC0) | ((base + i) & 0x3F));
}

static void
verify_password(at88sc153_t *c)
{
    int     set = SEL(c->cmd);
    uint8_t pac = pac_addr[set];

    if (c->mem[pac] && !memcmp(c->buf, &c->mem[pac + 1], 3)) {
        c->mem[pac] = 0xFF;
        if (set < 2)
            c->pw_ok[set] = 1;
    } else
        c->mem[pac] <<= 1;      /* one attempt consumed */
    c->dirty = 1;
}

/* A whole byte has been clocked in; return 1 to ACK it. */
static int
byte_in(at88sc153_t *c, uint8_t b)
{
    if (c->nbuf < 0) {                          /* command byte */
        if ((b & 0xF0) != 0xB0)
            return 0;
        c->cmd  = b;
        c->nbuf = 0;
        return 1;
    }
    switch (OP(c->cmd)) {
        case OP_READ:
        case OP_WRITE:
            if (c->nbuf++ == 0) {
                c->addr = (uint8_t) ((SEL(c->cmd) << 6) | (b & 0x3F));
                return 1;
            }
            if (OP(c->cmd) == OP_WRITE && c->nbuf - 2 < 8)
                c->buf[c->nbuf - 2] = b;
            return 1;
        case OP_FUSE:
        case OP_VERIFY:
            if (c->nbuf < 8)
                c->buf[c->nbuf] = b;
            c->nbuf++;
            if (OP(c->cmd) == OP_VERIFY && c->nbuf == 3)
                verify_password(c);
            return 1;
    }
    return 0;
}

/* after the ACK clock of a received byte: does the chip start sending? */
static int
wants_tx(const at88sc153_t *c)
{
    if (OP(c->cmd) == OP_READ && c->nbuf == 1)
        return 1;
    if (c->cmd == 0xBE && c->nbuf == 0)
        return 1;
    return 0;
}

static void
load_tx(at88sc153_t *c)
{
    c->txbyte  = (c->cmd == 0xBE) ? c->fuses : c->mem[c->addr];
    c->bitcnt  = 0;
    c->sda_out = (c->txbyte >> 7) & 1;
    c->state   = ST_TX;
}

static void
on_stop(at88sc153_t *c)
{
    int i, n;

    if (c->state != ST_IDLE && c->nbuf > 0) {
        if (OP(c->cmd) == OP_WRITE && c->nbuf > 1) {
            n = c->nbuf - 1;
            if (n > 8)
                n = 8;
            for (i = 0; i < n; i++)
                c->mem[zone_addr(c->addr, i)] = c->buf[i];
            c->dirty = 1;
        } else if (c->cmd == 0xBA) {
            c->fuses &= (uint8_t) ~c->buf[0];   /* blowing is one-way */
            c->dirty = 1;
        }
    }
    c->state   = ST_IDLE;
    c->sda_out = 1;
}

void
at88_pins(at88sc153_t *c, int scl, int sda, int rst)
{
    int line;

    scl = !!scl;
    sda = !!sda;
    if (!c->present) {
        c->scl = scl;
        c->sda_in = sda;
        return;
    }
    if (rst) {
        c->state   = ST_IDLE;
        c->sda_out = 1;
        c->pw_ok[0] = c->pw_ok[1] = 0;
        c->scl = scl;
        c->sda_in = sda;
        return;
    }

    if (c->scl && scl && sda != c->sda_in) {
        if (!sda) {                             /* START */
            c->state   = ST_RX;
            c->bitcnt  = 0;
            c->nbuf    = -1;
            c->sda_out = 1;
        } else                                  /* STOP */
            on_stop(c);
    } else if (!c->scl && scl) {                /* rising edge: sample */
        line = sda & c->sda_out;
        if (c->state == ST_RX) {
            c->shreg = (uint8_t) ((c->shreg << 1) | line);
            c->bitcnt++;
        } else if (c->state == ST_TX_ACK)
            c->bitcnt = line ? -1 : 0;          /* -1 = master NAK */
    } else if (c->scl && !scl) {                /* falling edge: shift */
        switch (c->state) {
            case ST_RX:
                if (c->bitcnt == 8) {
                    if (byte_in(c, c->shreg)) {
                        c->state   = ST_RX_ACK;
                        c->sda_out = 0;
                    } else
                        c->state = ST_WAIT_STOP;
                }
                break;
            case ST_RX_ACK:
                c->sda_out = 1;
                if (wants_tx(c))
                    load_tx(c);
                else {
                    c->state  = ST_RX;
                    c->bitcnt = 0;
                }
                break;
            case ST_TX:
                if (++c->bitcnt == 8) {
                    c->state   = ST_TX_ACK;
                    c->sda_out = 1;
                } else
                    c->sda_out = (c->txbyte >> (7 - c->bitcnt)) & 1;
                break;
            case ST_TX_ACK:
                if (c->bitcnt == 0 && c->cmd != 0xBE) {     /* sequential read */
                    c->addr = zone_addr(c->addr, 1);
                    load_tx(c);
                } else {
                    c->state   = ST_WAIT_STOP;
                    c->sda_out = 1;
                }
                break;
        }
    }
    c->scl    = scl;
    c->sda_in = sda;
}

int
at88_sda(const at88sc153_t *c)
{
    return c->present ? c->sda_out : 1;
}
