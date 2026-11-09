#include "hostlink.h"

#include <string.h>

#include "bytes.h"
#include "crc.h"

#define SOF0 0x7E
#define SOF1 0xA5

void hostlink_init(struct hostlink *hl, hl_handler handler, void *ctx)
{
    memset(hl, 0, sizeof *hl);
    hl->handler = handler;
    hl->ctx = ctx;
}

/* Writes the 6 header bytes of a frame with a len-byte payload; returns the
 * CRC so far, over len, seq and type. */
static uint16_t put_header(uint8_t out[6], uint8_t seq, uint8_t type, size_t len)
{
    out[0] = SOF0;
    out[1] = SOF1;
    put16(&out[2], (uint16_t)(len + 2));
    out[4] = seq;
    out[5] = type;
    return crc16_ccitt_false(CRC16_CCITT_FALSE_INIT, &out[2], 4);
}

/* Completes a frame whose payload is already at out[6]; returns its length. */
static size_t finish(uint8_t *out, uint8_t seq, uint8_t type, size_t len)
{
    uint16_t crc = put_header(out, seq, type, len);
    put16(&out[6 + len], crc16_ccitt_false(crc, &out[6], len));
    return len + 8;
}

size_t hostlink_encode(uint8_t *out, uint8_t seq, uint8_t type, const uint8_t *payload,
                       size_t len)
{
    if (len > HL_MAX_PAYLOAD)
        return 0;
    if (len)
        memcpy(&out[6], payload, len);
    return finish(out, seq, type, len);
}

static void handle_request(struct hostlink *hl, uint16_t crc, uint8_t seq, uint8_t type,
                           const uint8_t *payload, size_t len)
{
    if (hl->have_last && seq == hl->last_seq && crc == hl->last_crc) {
        hostlink_write(hl->last_reply, hl->last_reply_len);
        return;
    }
    /* The reply is built in place in the cache: status at [6], payload after. */
    uint8_t *reply = &hl->last_reply[6];
    size_t reply_len = 0;
    reply[0] = (uint8_t)hl->handler(hl->ctx, type, payload, len, &reply[1], &reply_len);
    hl->last_reply_len = finish(hl->last_reply, seq, type | HL_REPLY_FLAG, reply_len + 1);
    hl->last_seq = seq;
    hl->last_crc = crc;
    hl->have_last = true;
    hostlink_write(hl->last_reply, hl->last_reply_len);
}

/* Consumes complete frames from the buffer, then moves the unconsumed tail to
 * the front once. On a bad start, length or CRC it skips one byte, so the next
 * 7E A5 is found again; runs of noise are skipped up to the next 7E at once. */
static void parse(struct hostlink *hl)
{
    size_t pos = 0;
    while (pos < hl->rx_len) {
        const uint8_t *f = &hl->rx[pos];
        size_t avail = hl->rx_len - pos;
        if (f[0] != SOF0) {
            const uint8_t *sof = memchr(f, SOF0, avail);
            size_t skip = sof ? (size_t)(sof - f) : avail;
            hl->resyncs += (uint32_t)skip;
            pos += skip;
            continue;
        }
        if (avail >= 2 && f[1] != SOF1) {
            hl->resyncs++;
            pos++;
            continue;
        }
        if (avail < 4)
            break;
        size_t body = get16(&f[2]);
        if (body < 2 || body > HL_MAX_BODY) {
            hl->resyncs++;
            pos++;
            continue;
        }
        if (avail < body + 6)
            break;
        uint16_t crc = get16(&f[4 + body]);
        if (crc != crc16_ccitt_false(CRC16_CCITT_FALSE_INIT, &f[2], body + 2)) {
            hl->crc_errors++;
            hl->resyncs++;
            pos++;
            continue;
        }
        handle_request(hl, crc, f[4], f[5], &f[6], body - 2);
        pos += body + 6;
    }
    memmove(hl->rx, &hl->rx[pos], hl->rx_len - pos);
    hl->rx_len -= pos;
}

void hostlink_feed(struct hostlink *hl, const uint8_t *data, size_t len)
{
    while (len > 0) {
        size_t n = sizeof hl->rx - hl->rx_len;
        if (n > len)
            n = len;
        memcpy(&hl->rx[hl->rx_len], data, n);
        hl->rx_len += n;
        data += n;
        len -= n;
        parse(hl);
    }
}

void hostlink_event(struct hostlink *hl, uint8_t type, const uint8_t *head, size_t head_len,
                    const uint8_t *data, size_t data_len)
{
    size_t len = head_len + data_len;
    if (len > HL_MAX_PAYLOAD)
        return;
    uint8_t header[6], crc_bytes[2];
    uint16_t crc = put_header(header, hl->event_seq++, type, len);
    crc = crc16_ccitt_false(crc, head, head_len);
    crc = crc16_ccitt_false(crc, data, data_len);
    put16(crc_bytes, crc);
    hostlink_write(header, sizeof header);
    hostlink_write(head, head_len);
    if (data_len)
        hostlink_write(data, data_len);
    hostlink_write(crc_bytes, sizeof crc_bytes);
}
