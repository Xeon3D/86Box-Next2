/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             Key generator for the Midway Dolphin I/O board (TouchMaster
 *             II).  See dolphin_keygen.c.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#ifndef EMU_DOLPHIN_KEYGEN_H
#define EMU_DOLPHIN_KEYGEN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dolphin_country_t {
    int         code; /* TouchMaster country code */
    const char *name; /* as the game names it */
} dolphin_country_t;

/* The game's country list (t/video/config.h), ended by code -1. */
extern const dolphin_country_t dolphin_countries[];

extern const char *dolphin_country_name(int code);

/* Programs a blank U9 chip and Abyss key, 256 bytes and the fuse byte each,
   for serial, country, key version major.minor (2.03 writes 1.0, 3.0x 3.0)
   and manufacture date now (Unix time); seed stands in for the original's
   srandomdev() (it takes seed, seed + 1 and seed + 2). Returns -1 if the
   serial is longer than the chip's 14 characters. */
extern int dolphin_keygen(uint8_t u9[257], uint8_t abyss[257], uint64_t serial, unsigned country, unsigned major,
                          unsigned minor, uint32_t now, uint32_t seed);

/* The serial number and country of a programmed U9 image; -1 if blank. */
extern int dolphin_keygen_read_u9(const uint8_t u9[257], uint64_t *serial, unsigned *country);

#ifdef __cplusplus
}
#endif

#endif /*EMU_DOLPHIN_KEYGEN_H*/
