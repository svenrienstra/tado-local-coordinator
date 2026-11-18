#include "hlframe.h"

#include <stdbool.h>
#include <string.h>

#include "bytes.h"
#include "crc.h"

#define SOF0 0x7E
#define SOF1 0xA5

uint16_t hl_frame_header(uint8_t out[6], uint8_t seq, uint8_t type, size_t len)
{
    out[0] = SOF0;
    out[1] = SOF1;
    put16(&out[2], (uint16_t)(len + 2));
    out[4] = seq;
    out[5] = type;
    return crc16_ccitt_false(CRC16_CCITT_FALSE_INIT, &out[2], 4);
}

size_t hl_frame_finish(uint8_t *out, uint8_t seq, uint8_t type, size_t len)
{
    if (len > HL_MAX_PAYLOAD)
        return 0;
    uint16_t crc = hl_frame_header(out, seq, type, len);
    put16(&out[6 + len], crc16_ccitt_false(crc, &out[6], len));
    return len + 8;
}

size_t hl_frame_encode(uint8_t *out, uint8_t seq, uint8_t type, const uint8_t *payload, size_t len)
{
    if (len > HL_MAX_PAYLOAD)
        return 0;
    if (len)
        memcpy(&out[6], payload, len);
    return hl_frame_finish(out, seq, type, len);
}

/* The body length of a plausible frame start at f, or 0. */
static size_t start_at(const uint8_t *f, size_t avail)
{
    if (avail < 4 || f[0] != SOF0 || f[1] != SOF1)
        return 0;
    size_t body = get16(&f[2]);
    return body >= 2 && body <= HL_MAX_BODY ? body : 0;
}

static bool crc_ok(const uint8_t *f, size_t body)
{
    return get16(&f[4 + body]) == crc16_ccitt_false(CRC16_CCITT_FALSE_INIT, &f[2], body + 2);
}

/* Where the first whole frame with a good CRC in buf[from..len) starts, or 0. */
static size_t valid_frame_after(const uint8_t *buf, size_t from, size_t len)
{
    for (size_t j = from; j + 4 <= len; j++) {
        const uint8_t *sof = memchr(&buf[j], SOF0, len - j);
        if (!sof)
            return 0;
        j = (size_t)(sof - buf);
        size_t body = start_at(&buf[j], len - j);
        if (body && len - j >= body + 6 && crc_ok(&buf[j], body))
            return j;
    }
    return 0;
}

/* Hands the whole, valid frames in buf[0..len) to fn; returns the bytes used. */
static size_t scan(const uint8_t *buf, size_t len, struct hl_scan_stats *st, hl_frame_fn fn, void *ctx)
{
    size_t pos = 0;
    while (pos < len) {
        const uint8_t *f = &buf[pos];
        size_t avail = len - pos;
        if (f[0] != SOF0) {
            const uint8_t *sof = memchr(f, SOF0, avail);
            size_t skip = sof ? (size_t)(sof - f) : avail;
            st->resyncs += (uint32_t)skip;
            pos += skip;
            continue;
        }
        if (avail >= 2 && f[1] != SOF1) {
            st->resyncs++;
            pos++;
            continue;
        }
        if (avail < 4)
            break;
        size_t body = start_at(f, avail);
        if (!body) {
            st->resyncs++;
            pos++;
            continue;
        }
        if (avail < body + 6) {
            size_t next = valid_frame_after(buf, pos + 1, len);
            if (!next)
                break; /* wait for the rest */
            st->resyncs += (uint32_t)(next - pos);
            pos = next;
            continue;
        }
        if (!crc_ok(f, body)) {
            st->crc_errors++;
            st->resyncs++;
            pos++;
            continue;
        }
        fn(ctx, get16(&f[4 + body]), f[4], f[5], &f[6], body - 2);
        pos += body + 6;
    }
    return pos;
}

void hl_rx_feed(struct hl_rx *rx, const uint8_t *data, size_t len, hl_frame_fn fn, void *ctx)
{
    while (len > 0) {
        size_t n = sizeof rx->buf - rx->len;
        if (n > len)
            n = len;
        memcpy(&rx->buf[rx->len], data, n);
        rx->len += n;
        data += n;
        len -= n;
        size_t used = scan(rx->buf, rx->len, &rx->stats, fn, ctx);
        if (used) {
            memmove(rx->buf, &rx->buf[used], rx->len - used);
            rx->len -= used;
        }
    }
}
