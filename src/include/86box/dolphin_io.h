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

#ifdef __cplusplus
extern "C" {
#endif

/* Lines for dolphin_io_pulse(): switches on the board, active low. */
#define DOLPHIN_LINE_COIN1     0 /* 62h bit 0 */
#define DOLPHIN_LINE_COIN2     1 /* 62h bit 1 */
#define DOLPHIN_LINE_COIN3     2 /* 62h bit 2 */
#define DOLPHIN_LINE_CALIBRATE 3 /* 62h bit 6 */
#define DOLPHIN_LINE_TEST      4 /* 62h bit 7 */
#define DOLPHIN_LINES          5

extern void dolphin_io_pulse(int line);
extern int  dolphin_io_present(void);

#ifdef EMU_DEVICE_H
extern const device_t dolphin_io_device;
#endif

#ifdef __cplusplus
}
#endif

#endif /*EMU_DOLPHIN_IO_H*/
