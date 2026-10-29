/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             Atmel AT88SC153-class secure memory, as on the Midway Dolphin
 *             I/O board (TouchMaster II): the U9 chip and the Abyss key.
 *             Model from the TouchMaster preservation work (Phase 1),
 *             verified against Midway's own dolphin.c.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
/* at88sc153.h - behavioural model of the Atmel AT88SC153-class secure memory
 * used on the Midway Dolphin I/O board (U9 "Dolphin" chip and the "Abyss"
 * configuration/key card).
 *
 * Derived from how TouchMaster II t/coinmgr/dolphin.c drives the part:
 *   - 2-wire bus, MSB first, START/STOP, 9th-clock ACK.
 *   - 256 bytes: zones 0..2 (0x00-0xBF, user) + zone 3 (0xC0-0xFF, config).
 *   - Commands 0xB0|z<<2 write, 0xB1|z<<2 read, 0xB3 verify write-pw set 0,
 *     0xB7 verify write-pw set 1 ("secure code"), 0xBA write fuses,
 *     0xBE read fuses.
 * Pure C, no host dependencies, so it can be dropped into 86Box unchanged.
 */
#ifndef EMU_AT88SC153_H
#define EMU_AT88SC153_H

#include <stdint.h>

#define AT88_SIZE 256

/* Config-zone offsets (absolute addresses) */
#define AT88_ATR        0xC0    /* answer-to-reset, must not be 00/FF  */
#define AT88_FAB        0xC8    /* fab code, Midway = AE 60            */
#define AT88_AR0        0xCC    /* access registers, zones 0..2        */
#define AT88_PAC_W0     0xF0    /* password attempt counter, write 0   */
#define AT88_PW_W0      0xF1
#define AT88_PAC_R0     0xF4
#define AT88_PW_R0      0xF5
#define AT88_PAC_W1     0xF8    /* write 1 = secure code               */
#define AT88_PW_W1      0xF9
#define AT88_PAC_R1     0xFC
#define AT88_PW_R1      0xFD

typedef struct at88sc153 {
    uint8_t  mem[AT88_SIZE];
    uint8_t  fuses;         /* bit set = intact; WriteFuses clears bits   */
    int      present;       /* 0 = socket empty: never drives the bus     */

    /* bus state */
    int      scl, sda_in;   /* last levels seen on the chip pins          */
    int      sda_out;       /* 1 = released, 0 = pulling low              */

    /* protocol state */
    int      state;
    uint8_t  shreg;
    int      bitcnt;
    uint8_t  cmd;
    uint8_t  addr;
    uint8_t  buf[8];
    int      nbuf;
    uint8_t  txbyte;
    int      pw_ok[2];      /* write-password sets verified this session  */

    int      dirty;         /* contents changed, caller should persist    */
} at88sc153_t;

/* Fill with a blank, unserialised part as shipped by Atmel to Midway:
 * ATR set, fab code AE 60, transport secure code D5 FE 7E, PACs FF,
 * user zones FF, fuses intact. */
void at88_init_blank(at88sc153_t *c);

/* Pins in: called whenever the host changes SCL/SDA/RST.  sda is the
 * level the host drives (1 if the host released the line). */
void at88_pins(at88sc153_t *c, int scl, int sda, int rst);

/* Level the chip drives on SDA (1 = released). */
int  at88_sda(const at88sc153_t *c);

#endif
