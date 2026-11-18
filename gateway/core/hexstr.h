#ifndef HEXSTR_H
#define HEXSTR_H

#include <stdbool.h>
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

static inline int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* eui_str's text back: exactly 16 hex digits; false otherwise. */
static inline bool eui_parse(const char *s, size_t len, uint8_t eui[8])
{
    if (len != 16)
        return false;
    for (size_t i = 0; i < 8; i++) {
        int hi = hex_digit(s[2 * i]), lo = hex_digit(s[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        eui[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

#endif
