/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             Key generator for the Midway Dolphin I/O board: programs a
 *             blank U9 chip and Abyss key as Midway TouchMaster II's own
 *             dBetty() ("diag init") and dChris() (ProgramAbyss) do, byte
 *             for byte.  A port of the TouchMaster preservation work's
 *             p1-mkchips.py, itself checked against Midway's dolphin.c;
 *             this port is checked against it in turn.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <86box/dolphin_keygen.h>

/* dwriteeprom()'s XOR below C0h (the start of an ELF header) */
static const uint8_t dloffset[64] = {
    0x7f, 0x45, 0x4c, 0x46, 0x54, 0xf2, 0x05, 0x00, 0xd4, 0x80, 0x04, 0x08, 0xf4, 0xe0, 0x19, 0xa0,
    0x65, 0x78, 0x65, 0x63, 0x46, 0x72, 0x65, 0xf2, 0x54, 0xf2, 0xc4, 0x60, 0x2f, 0xc6, 0x69, 0x92,
    0x80, 0x40, 0x84, 0x15, 0x19, 0x37, 0xae, 0xea, 0x08, 0x83, 0x3d, 0x3c, 0xed, 0xb0, 0x05, 0xff,
    0xbb, 0xec, 0xc7, 0xd0, 0xab, 0x50, 0xc8, 0x1d, 0x8d, 0x7d, 0x08, 0x8b, 0x38, 0x00, 0xef, 0x0a
};

/* the serial number's XOR in U9 */
static const uint8_t snof[16] = { 0x42, 0x31, 0x54, 0x2a, 0xfe, 0x91, 0xeb, 0x24, 0x99, 0x4d };

/* GetMasterPalEntry()'s address scramble for the key's first C0h bytes */
static const uint8_t masterpal[0xc0] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x0e, 0x18, 0x2f, 0x23, 0x17, 0x3c, 0x12, 0x30,
    0x1c, 0x15, 0x25, 0x20, 0x3b, 0x27, 0x29, 0x1d, 0x2a, 0x21, 0x14, 0x2e, 0x13, 0x09, 0x0f, 0x37,
    0x31, 0x1e, 0x0d, 0x0a, 0x2c, 0x3f, 0x0b, 0x1b, 0x1f, 0x2d, 0x3d, 0x39, 0x28, 0x36, 0x22, 0x3e,
    0x26, 0x1a, 0x10, 0x11, 0x16, 0x2b, 0x08, 0x33, 0x38, 0x3a, 0x0c, 0x32, 0x24, 0x34, 0x19, 0x35,
    0x7d, 0x45, 0x69, 0x73, 0x4c, 0x4b, 0x76, 0x6d, 0x56, 0x47, 0x44, 0x62, 0x59, 0x64, 0x42, 0x67,
    0x77, 0x61, 0x41, 0x6e, 0x4f, 0x5a, 0x58, 0x51, 0x53, 0x71, 0x57, 0x7f, 0x7a, 0x4d, 0x5c, 0x6c,
    0x55, 0x7c, 0x7b, 0x46, 0x54, 0x5f, 0x4e, 0x5d, 0x63, 0x6b, 0x48, 0x60, 0x68, 0x6a, 0x72, 0x5b,
    0x49, 0x43, 0x74, 0x79, 0x75, 0x7e, 0x65, 0x70, 0x52, 0x78, 0x40, 0x4a, 0x6f, 0x5e, 0x66, 0x50,
    0x9c, 0xad, 0x80, 0x91, 0x93, 0x97, 0x99, 0x9a, 0xb2, 0x85, 0x81, 0xb8, 0xb9, 0x86, 0x8d, 0x8a,
    0xb5, 0x9e, 0x98, 0x8f, 0xa0, 0x89, 0xb1, 0x8e, 0xa7, 0xaf, 0x88, 0x82, 0xa1, 0xaa, 0xa8, 0x96,
    0xb4, 0x94, 0x90, 0x9f, 0x95, 0xbb, 0xac, 0x83, 0x87, 0xab, 0x9d, 0xb0, 0xa5, 0x92, 0x9b, 0xa9,
    0x8b, 0xae, 0xa4, 0xa6, 0xa3, 0xb3, 0xb6, 0x84, 0xba, 0xb7, 0x8c, 0xa2, 0xbc, 0xbd, 0xbe, 0xbf
};

const dolphin_country_t dolphin_countries[] = {
    {  1, "USA (2 Coin)" },
    {  2, "Germany" },
    {  3, "Austria" },
    {  4, "France" },
    {  5, "Argentina" },
    {  6, "Australia" },
    {  7, "Belgium" },
    {  8, "Chile" },
    {  9, "Denmark" },
    { 10, "England" },
    { 11, "Finland" },
    { 12, "Greece" },
    { 13, "Holland" },
    { 14, "Italy" },
    { 15, "Japan" },
    { 16, "Neth Antilles" },
    { 17, "New Zealand" },
    { 18, "Norway" },
    { 19, "Netherlands" },
    { 20, "Spain" },
    { 21, "Sweden" },
    { 22, "Switzerland" },
    { 23, "Hungary" },
    { 24, "Canada" },
    { 25, "Euro" },
    { 28, "Germany NR1" },
    { 29, "USA (DBV Installed)" },
    { 30, "USA (DBV w/Stacker)" },
    { 31, "USA (DBV Stacker Ready)" },
    { 33, "USA (DBV Ready)" },
    { 34, "Portugal" },
    { 35, "USA (Universal)" },
    { 36, "USA (Cashflow)" },
    { 37, "Brazil" },
    { 38, "Saudi Arabia" },
    { 39, "Croatia" },
    { 71, "France (Cashflow)" },
    { 72, "Switzerland (Cashflow)" },
    { 73, "Louisiana (DBV Ready)" },
    { 74, "California (DBV Ready)" },
    { 75, "California (DBV Installed)" },
    { 76, "Minnesota (DBV Ready)" },
    { 77, "Minnesota (DBV Installed)" },
    { 78, "New Jersey (2SL w/Lock)" },
    { 86, "Indonesia" },
    { 87, "India" },
    { 89, "Cypress" },
    { 90, "Hong Kong" },
    { 91, "Oregon (DBV Ready)" },
    { 92, "Oregon (DBV Installed)" },
    { 93, "New Jersey (DBV Ready)" },
    { 94, "Quebec" },
    { 96, "Louisiana (DBV Installed)" },
    { 99, "Debug & Development" },
    { -1, NULL }
};

/* FreeBSD 3.2 libc random()/srandom() (TYPE_3) with a 32-bit long. */
typedef struct fbsd_rng_t {
    int32_t state[31];
    int     f;
    int     r;
} fbsd_rng_t;

static uint32_t
fbsd_random(fbsd_rng_t *g)
{
    uint32_t out;

    g->state[g->f] = (int32_t) ((uint32_t) g->state[g->f] + (uint32_t) g->state[g->r]);
    out            = ((uint32_t) g->state[g->f] >> 1) & 0x7fffffff;
    if (++g->f >= 31) {
        g->f = 0;
        ++g->r;
    } else if (++g->r >= 31)
        g->r = 0;
    return out;
}

static void
fbsd_srandom(fbsd_rng_t *g, uint32_t seed)
{
    g->state[0] = (int32_t) seed;
    for (int i = 1; i < 31; i++) {
        int32_t hi = g->state[i - 1] / 127773;
        int32_t lo = g->state[i - 1] % 127773;
        int32_t v  = 16807 * lo - 2836 * hi;

        if (v <= 0)
            v += 0x7fffffff;
        g->state[i] = v;
    }
    g->f = 3;
    g->r = 0;
    for (int i = 0; i < 310; i++)
        (void) fbsd_random(g);
}

/* A blank part as Atmel shipped it to Midway. */
static void
chip_blank(uint8_t *img)
{
    static const uint8_t atr[8] = { 0x3b, 0xb2, 0x11, 0x00, 0x10, 0x80, 0x00, 0x01 };

    memset(img, 0xff, 256);
    memcpy(&img[0xc0], atr, sizeof(atr));
    img[0xc8] = 0xae; /* fab code */
    img[0xc9] = 0x60;
    img[0xf9] = 0xd5; /* transport secure code */
    img[0xfa] = 0xfe;
    img[0xfb] = 0x7e;
    img[256]  = 0x07; /* fuses intact */
}

/* dwriteeprom() */
static void
wr(uint8_t *img, unsigned addr, unsigned data)
{
    addr &= 0xff;
    data &= 0xff;
    img[addr] = (addr < 0xc0) ? (uint8_t) (data ^ dloffset[addr & 0x3f]) : (uint8_t) data;
}

/* GetMasterPalEntry() */
static unsigned
lut(unsigned i)
{
    return (i > 0xbf) ? i : masterpal[i];
}

/* dBetty(): the U9 chip. */
static int
betty(uint8_t *c, uint64_t serial, unsigned country, uint32_t now, fbsd_rng_t *g, uint32_t seed)
{
    static const uint8_t cfg[][2] = { { 0xf1, 0x4d }, { 0xf2, 0x3a }, { 0xf3, 0xec }, { 0xf5, 0xe0 }, { 0xf6, 0xff },
                                      { 0xf7, 0x3c }, { 0xf9, 0x17 }, { 0xfa, 0xab }, { 0xfb, 0xdc }, { 0xfd, 0xcd },
                                      { 0xfe, 0xa3 }, { 0xff, 0x45 }, { 0xcc, 0x7d }, { 0xcd, 0x7f }, { 0xce, 0x77 } };
    char                 buf[24];
    uint32_t             s = 0;
    uint32_t             gt;
    uint32_t             ck;
    uint32_t             r;

    for (unsigned i = 0x80; i < 0xc0; i++)
        wr(c, i, 0);
    wr(c, 0x3f, 0x01);
    for (unsigned i = 0x44; i < 0x7f; i++)
        wr(c, i, 0xff);
    wr(c, 0x40, country & 0xff);
    wr(c, 0x41, country >> 8);
    wr(c, 0x42, 0);
    wr(c, 0x43, (country & 0xff) ^ (serial & 0xff) ^ 0xa3);

    /* sprintf("%qd") into a 16-byte buffer, of which 14 bytes are stored */
    memset(buf, 0, sizeof(buf));
    snprintf(buf, sizeof(buf), "%lld", (long long) (int64_t) serial);
    if (strlen(buf) > 14)
        return -1;
    for (unsigned i = 0; i < 14; i++) {
        wr(c, i, (uint8_t) buf[i] ^ snof[i]);
        s += (uint32_t) (int32_t) (int8_t) buf[i];
    }
    gt = now;
    for (unsigned k = 0; k < 32; k += 8)
        s += (gt >> k) & 0xff;
    gt ^= 0x1b47a122;
    for (unsigned k = 0; k < 4; k++)
        wr(c, 0x0e + k, gt >> (8 * k));
    ck = (s + (s << 16)) ^ 0xb2f43749;
    for (unsigned k = 0; k < 4; k++)
        wr(c, 0x12 + k, ck >> (8 * k));
    wr(c, 0x16, country & 0xff);
    wr(c, 0x17, country >> 8);
    wr(c, 0x18, 0xae);
    wr(c, 0x19, 0xc0);
    fbsd_srandom(g, seed);
    r = fbsd_random(g);
    for (unsigned k = 0; k < 4; k++)
        wr(c, 0x1a + k, r >> (8 * k));
    for (unsigned i = 0x1e; i < 0x40; i++)
        wr(c, i, 0);
    wr(c, 0xbf, 1);
    for (unsigned i = 0; i < sizeof(cfg) / sizeof(cfg[0]); i++)
        wr(c, cfg[i][0], cfg[i][1]);
    c[256] &= ~0x06; /* fuses 1 and 2 blown */
    return 0;
}

/* dChris(): the Abyss key. */
static void
chris(uint8_t *c, uint64_t serial, unsigned country, unsigned major, unsigned minor, fbsd_rng_t *g, uint32_t seed_h,
      uint32_t seed_r)
{
    static const uint8_t cfg[][2] = { { 0xf1, 0x5e }, { 0xf2, 0xd4 }, { 0xf3, 0xc6 }, { 0xf5, 0x73 }, { 0xf6, 0xe9 }, { 0xf7, 0x3c },
                                      { 0xf9, 0x4b }, { 0xfa, 0x00 }, { 0xfb, 0xd2 }, { 0xfd, 0x32 }, { 0xfe, 0x41 }, { 0xff, 0x72 } };
    uint8_t              x[0xc0];
    uint32_t             s = 0;
    uint32_t             v = (major << 16) | minor;
    uint32_t             h;
    uint32_t             ck;
    uint32_t             t;
    uint32_t             r;

    fbsd_srandom(g, seed_h);
    h = fbsd_random(g) & 0xffff;
    fbsd_srandom(g, (uint32_t) serial + h);
    for (unsigned i = 0; i < 0xbf; i++)
        x[i] = (uint8_t) ((fbsd_random(g) & 0xff) ^ 0xb5);
    x[0xbf] = 0;

    for (unsigned k = 0; k < 8; k++) {
        unsigned b = (serial >> (8 * k)) & 0xff;

        wr(c, k, b);
        s += b;
    }
    for (unsigned k = 0; k < 4; k++) {
        wr(c, lut(0x0c + k), v >> (8 * k));
        s += (v >> (8 * k)) & 0xff;
    }
    wr(c, lut(0x10), 0x0b);
    s += 0x0b;
    for (unsigned i = 0x08; i < 0x0c; i++)
        wr(c, lut(i), 0);
    for (unsigned i = 0x11; i < 0x38; i++)
        wr(c, lut(i), 0);
    wr(c, 0x38, 0x02);
    s += 2;
    wr(c, 0xbc, 0x50);
    wr(c, 0xbd, 0xaa);
    wr(c, 0xbe, h & 0xff);
    wr(c, 0xbf, h >> 8);
    ck = s + (s << 16);
    for (unsigned k = 0; k < 4; k++)
        wr(c, lut(0x3c + k), x[0x3c + k] ^ ((ck >> (8 * k)) & 0xff));
    for (unsigned i = 0; i < sizeof(cfg) / sizeof(cfg[0]); i++)
        wr(c, cfg[i][0], cfg[i][1]);

    s = 0;
#define PUT(idx, val)                                            \
    do {                                                         \
        wr(c, lut(idx), x[(idx)] ^ ((unsigned) (val) & 0xff));   \
        s += (unsigned) (val) & 0xff;                            \
    } while (0)
    for (unsigned k = 0; k < 4; k++)
        PUT(0x40 + k, v >> (8 * k));
    for (unsigned k = 0; k < 2; k++)
        PUT(0x48 + k, country >> (8 * k));
    t = country ^ 0xda4b;
    for (unsigned k = 0; k < 2; k++)
        PUT(0x4a + k, t >> (8 * k));
    t = v ^ 0xa41301b4;
    for (unsigned k = 0; k < 4; k++)
        PUT(0x4c + k, t >> (8 * k));
    t = (country ^ 0xda4b) ^ (v ^ 0xa41301b4) ^ 0xc582;
    for (unsigned k = 0; k < 4; k++)
        PUT(0x44 + k, t >> (8 * k));
#undef PUT
    wr(c, lut(0x78), 0x01);
    s += 1;
    for (unsigned i = 0x50; i < 0x78; i++)
        wr(c, lut(i), x[i]);
    wr(c, lut(0x79), x[0x79]);
    fbsd_srandom(g, seed_r);
    r = fbsd_random(g);
    wr(c, 0x7a, r & 0xff);
    wr(c, 0x7b, (r >> 8) & 0xff);
    s += (r & 0xff) + ((r >> 8) & 0xff);
    ck = s + (s << 16);
    for (unsigned k = 0; k < 4; k++)
        wr(c, lut(0x7c + k), x[0x7c + k] ^ ((ck >> (8 * k)) & 0xff));
    for (unsigned i = 0x80; i < 0xbc; i++)
        wr(c, lut(i), x[i] ^ lut(i));
    wr(c, lut(0xbe), h & 0xff);
    wr(c, lut(0xbf), h >> 8);
    wr(c, 0xcc, 0x7d);
    wr(c, 0xcd, 0x7f);
    wr(c, 0xce, 0x77);
    c[256] &= ~0x06; /* fuses 1 and 2 blown */
}

int
dolphin_keygen(uint8_t u9[257], uint8_t abyss[257], uint64_t serial, unsigned country, unsigned major, unsigned minor,
               uint32_t now, uint32_t seed)
{
    fbsd_rng_t g;

    chip_blank(u9);
    chip_blank(abyss);
    /* The original takes srandomdev() each time it seeds: three times. */
    if (betty(u9, serial, country, now, &g, seed))
        return -1;
    chris(abyss, serial, country, major, minor, &g, seed + 1, seed + 2);
    return 0;
}

int
dolphin_keygen_read_u9(const uint8_t u9[257], uint64_t *serial, unsigned *country)
{
    char buf[15];

    /* dDana()'s "programmed" magic, AE C0 */
    if (((u9[0x18] ^ dloffset[0x18]) != 0xae) || ((u9[0x19] ^ dloffset[0x19]) != 0xc0))
        return -1;
    for (unsigned i = 0; i < 14; i++)
        buf[i] = (char) (u9[i] ^ dloffset[i] ^ snof[i]);
    buf[14] = 0;
    *serial  = strtoull(buf, NULL, 10);
    *country = (u9[0x16] ^ dloffset[0x16]) | ((u9[0x17] ^ dloffset[0x17]) << 8);
    return 0;
}

const char *
dolphin_country_name(int code)
{
    for (const dolphin_country_t *c = dolphin_countries; c->name; c++)
        if (c->code == code)
            return c->name;
    return NULL;
}
