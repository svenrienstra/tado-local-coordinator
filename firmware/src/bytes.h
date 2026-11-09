#ifndef BYTES_H
#define BYTES_H

#include <stdint.h>

/* Little-endian integers, as the MAC frames and the host link carry them. */

static inline void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

static inline uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static inline uint32_t get32(const uint8_t *p) { return get16(p) | (uint32_t)get16(p + 2) << 16; }

#endif
