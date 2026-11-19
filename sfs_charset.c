/*
 * Copyright 2026, liilliil <haiku@liilliil.net>
 * All rights reserved. Distributed under the terms of the MIT license.
*/
#include "sfs_charset.h"
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

static const struct { const char* name; int id; } kNames[] = {
    {"latin1", SFS_CS_LATIN1}, {"iso-8859-1", SFS_CS_LATIN1},
    {"cp1251", SFS_CS_CP1251}, {"windows-1251", SFS_CS_CP1251},
    {"iso5", SFS_CS_ISO5}, {"iso-8859-5", SFS_CS_ISO5},
    {"utf8", SFS_CS_UTF8}, {"utf-8", SFS_CS_UTF8},
};

/* "charset=cp1251" в строке параметров; без параметра - по умолчанию */
int sfs_charset_parse(const char* args) {
    const char* p = args != NULL ? strstr(args, "charset=") : NULL;
    if (p == NULL) return SFS_CS_DEFAULT;
    p += 8;
    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); i++) {
        size_t n = strlen(kNames[i].name);
        if (strncmp(p, kNames[i].name, n) == 0) return kNames[i].id;
    }
    return SFS_CS_DEFAULT;
}

/* cp1251, 0x80..0xBF; 0xC0..0xFF = U+0410.. */
static const uint16 kCp1251Low[64] = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021,
    0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
    0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0xFFFD, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
    0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7,
    0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
    0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7,
    0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,
};

static uint32 sfs_decode(uint8 b, int cs) {
    if (b < 0x80 || cs == SFS_CS_LATIN1) return b;
    if (cs == SFS_CS_CP1251) return b >= 0xC0 ? 0x0410 + (b - 0xC0) : kCp1251Low[b - 0x80];
    /* ISO-8859-5 */
    if (b <= 0xA0 || b == 0xAD) return b;
    if (b <= 0xAC) return 0x0401 + (b - 0xA1);
    if (b <= 0xAF) return 0x040E + (b - 0xAE);
    if (b <= 0xEF) return 0x0410 + (b - 0xB0);
    if (b == 0xF0) return 0x2116;
    if (b <= 0xFC) return 0x0451 + (b - 0xF1);
    if (b == 0xFD) return 0x00A7;
    return 0x045E + (b - 0xFE);
}

static bool sfs_is_utf8(const uint8* s, size_t n) {
    for (size_t i = 0; i < n;) {
        uint8 c = s[i];
        size_t k;
        if (c < 0x80) k = 0;
        else if (c >= 0xC2 && c < 0xE0) k = 1;
        else if (c >= 0xE0 && c < 0xF0) k = 2;
        else if (c >= 0xF0 && c < 0xF5) k = 3;
        else return false;
        if (i + k >= n) return false;
        for (size_t j = 1; j <= k; j++) if ((s[i + j] & 0xC0) != 0x80) return false;
        i += k + 1;
    }
    return true;
}

static char* sfs_put_utf8(char* o, uint32 cp) {
    if (cp < 0x80) *o++ = cp;
    else if (cp < 0x800) { *o++ = 0xC0 | (cp >> 6); *o++ = 0x80 | (cp & 0x3F); }
    else { *o++ = 0xE0 | (cp >> 12); *o++ = 0x80 | ((cp >> 6) & 0x3F); *o++ = 0x80 | (cp & 0x3F); }
    return o;
}

/* Результат - malloc, освобождает вызывающий. UTF-8 на входе отдаётся как есть. */
char* sfs_name_to_utf8(const uint8* s, size_t len, int cs) {
    char* out = (char*)malloc(len * 3 + 1);
    if (out == NULL) return NULL;
    if (sfs_is_utf8(s, len) && (cs == SFS_CS_UTF8 || cs == SFS_CS_LATIN1)) {
        memcpy(out, s, len);
        out[len] = '\0';
        return out;
    }
    char* o = out;
    for (size_t i = 0; i < len; i++) o = sfs_put_utf8(o, sfs_decode(s[i], cs));
    *o = '\0';
    return out;
}
