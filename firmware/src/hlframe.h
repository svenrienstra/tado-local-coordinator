#ifndef HLFRAME_H
#define HLFRAME_H

#include <stddef.h>
#include <stdint.h>

/* Host link v0 framing (docs/HOSTLINK.md), shared by the CC1312 and the
 * gateway so both ends find frames the same way:
 *   7E A5 | len u16 | body | crc u16,  body = seq | type | payload
 * crc = CRC-16/CCITT-FALSE over len and body. */

#define HL_MAX_BODY    300
#define HL_MAX_PAYLOAD (HL_MAX_BODY - 2)
#define HL_MAX_FRAME   (HL_MAX_BODY + 6)

/* Encodes one frame into out (HL_MAX_FRAME bytes); returns its length, or 0
 * when the payload is too long. */
size_t hl_frame_encode(uint8_t *out, uint8_t seq, uint8_t type, const uint8_t *payload, size_t len);

/* Completes a frame whose len-byte payload is already at out[6]; returns its
 * length, or 0 when the payload is too long. */
size_t hl_frame_finish(uint8_t *out, uint8_t seq, uint8_t type, size_t len);

/* Writes the 6 header bytes of a frame with a len-byte payload; returns the
 * CRC so far, over len, seq and type, to continue over the payload. */
uint16_t hl_frame_header(uint8_t out[6], uint8_t seq, uint8_t type, size_t len);

struct hl_scan_stats {
    uint32_t crc_errors; /* frames dropped for a bad CRC */
    uint32_t resyncs;    /* bytes skipped while searching for a frame start */
};

typedef void (*hl_frame_fn)(void *ctx, uint16_t crc, uint8_t seq, uint8_t type, const uint8_t *payload,
                            size_t len);

/* Received bytes not yet a whole frame. */
struct hl_rx {
    uint8_t buf[HL_MAX_FRAME];
    size_t len;
    struct hl_scan_stats stats;
};

/* Feeds received bytes; every whole, valid frame goes to fn. On a bad start,
 * length or CRC one byte is skipped and the next 7E A5 searched for. A start
 * that claims more bytes than have come is skipped when a whole valid frame
 * follows it: a torn or false start must not hold that frame back. */
void hl_rx_feed(struct hl_rx *rx, const uint8_t *data, size_t len, hl_frame_fn fn, void *ctx);

#endif
