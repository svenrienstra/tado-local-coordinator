#ifndef HEXSTR_H
#define HEXSTR_H

#include <stddef.h>
#include <stdint.h>

/* Bytes as lowercase hex for log lines, cut to fit cap. */
static inline const char *hex_str(const uint8_t *b, size_t n, char *buf, size_t cap)
{
    static const char digits[] = "0123456789abcdef";
    size_t i = 0;
    for (; i < n && 2 * i + 2 < cap; i++) {
        buf[2 * i] = digits[b[i] >> 4];
        buf[2 * i + 1] = digits[b[i] & 0xf];
    }
    buf[2 * i] = '\0';
    return buf;
}

/* An EUI in wire order, as the host prints it. */
static inline const char *eui_str(const uint8_t eui[8], char buf[17]) { return hex_str(eui, 8, buf, 17); }

#endif
