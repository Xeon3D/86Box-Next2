/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             Midway "Dolphin" I/O board (ISA), Midway TouchMaster II.
 *             See dolphin_io.c.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#ifndef EMU_DOLPHIN_IO_H
#define EMU_DOLPHIN_IO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Lines for dolphin_io_pulse(): switches on the board, active low. */
#define DOLPHIN_LINE_COIN1     0 /* 62h bit 0 */
#define DOLPHIN_LINE_COIN2     1 /* 62h bit 1 */
#define DOLPHIN_LINE_COIN3     2 /* 62h bit 2 */
#define DOLPHIN_LINE_CALIBRATE 3 /* 62h bit 6 */
#define DOLPHIN_LINE_TEST      4 /* 62h bit 7 */
#define DOLPHIN_LINE_ELEC_A    5 /* 64h bit 1: electronic coin inputs A-F */
#define DOLPHIN_LINE_ELEC_B    6 /* 64h bit 2 */
#define DOLPHIN_LINE_ELEC_C    7 /* 64h bit 3 */
#define DOLPHIN_LINE_ELEC_D    8 /* 64h bit 4 */
#define DOLPHIN_LINE_ELEC_E    9 /* 64h bit 5 */
#define DOLPHIN_LINE_ELEC_F    10 /* 64h bit 6 */
#define DOLPHIN_LINES          11

extern void dolphin_io_pulse(int line);
extern int  dolphin_io_present(void);

/* Program U9 and the Abyss key as the game's factory flow would (see
   dolphin_keygen.h), on the board's next tick, and save them; the game reads
   them when it starts. 0 if queued, -1 bad serial, -2 no board, -3 busy. */
extern int dolphin_io_write_keys(uint64_t serial, unsigned country, unsigned major, unsigned minor);
/* Serial number and country of the fitted U9; -1 if blank or no board. */
extern int dolphin_io_read_keys(uint64_t *serial, unsigned *country);

#ifdef EMU_DEVICE_H
extern const device_t dolphin_io_device;
#endif

#ifdef __cplusplus
}
#endif

#endif /*EMU_DOLPHIN_IO_H*/
