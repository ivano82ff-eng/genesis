/* UTF-8 helpers shared by the console, the editor and the desktop. */
#pragma once
#include "cyr8x8.h"

static inline int utf8_is_cont(unsigned char b) {
    return (b & 0xc0u) == 0x80u;
}

/* Decode one character from s[0..n). Returns the bytes it takes (at least 1,
 * 0 only when n == 0); a broken or cut sequence gives '?' for one byte. */
static inline int utf8_decode(const unsigned char *s, unsigned int n, unsigned int *cp) {
    if (n == 0) {
        return 0;
    }
    unsigned char b = s[0];
    int need = b < 0x80u ? 0 : (b & 0xe0u) == 0xc0u ? 1 : (b & 0xf0u) == 0xe0u ? 2 : (b & 0xf8u) == 0xf0u ? 3 : -1;
    if (need < 0 || (unsigned int)need + 1u > n) {
        *cp = '?';
        return 1;
    }
    if (need == 0) {
        *cp = b;
        return 1;
    }
    unsigned int v = b & (0x3fu >> need);
    for (int i = 1; i <= need; i++) {
        if (!utf8_is_cont(s[i])) {
            *cp = '?';
            return 1;
        }
        v = (v << 6) | (s[i] & 0x3fu);
    }
    *cp = v;
    return need + 1;
}

static inline int utf8_encode(unsigned int cp, char *out) {
    if (cp < 0x80u) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (char)(0xc0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3fu));
        return 2;
    }
    out[0] = (char)(0xe0u | (cp >> 12));
    out[1] = (char)(0x80u | ((cp >> 6) & 0x3fu));
    out[2] = (char)(0x80u | (cp & 0x3fu));
    return 3;
}

static inline const struct cyr_glyph *cyr_glyph(unsigned int cp) {
    for (int i = 0; i < CYR8X8_COUNT; i++) {
        if (CYR8X8[i].code == cp) {
            return &CYR8X8[i];
        }
    }
    return 0;
}

/* Byte to store in a VGA text cell for code point cp (see vga_load_cyrillic). */
static inline unsigned char vga_cell(unsigned int cp) {
    if (cp < 0x80u) {
        return (unsigned char)cp;
    }
    const struct cyr_glyph *g = cyr_glyph(cp);
    return g ? g->slot : (unsigned char)'?';
}
