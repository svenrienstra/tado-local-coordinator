#ifndef HOSTLINK_H
#define HOSTLINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Host link v0 (docs/HOSTLINK.md):
 *   7E A5 | len u16 | body | crc u16,  body = seq | type | payload
 * crc = CRC-16/CCITT-FALSE over len and body. */

#define HL_MAX_BODY    300
#define HL_MAX_PAYLOAD (HL_MAX_BODY - 2)
#define HL_MAX_FRAME   (HL_MAX_BODY + 6)
#define HL_REPLY_FLAG  0x80
/* Reply payload after the status byte; the largest is STATS, 15 u32. */
#define HL_MAX_REPLY   64

enum hl_status {
    HL_OK = 0x00,
    HL_BAD_ARG = 0x01,
    HL_NOT_CONFIGURED = 0x02,
    HL_BUSY = 0x03,
    HL_UNKNOWN = 0x04,
    HL_UNSUPPORTED = 0x05, /* silicon older than rev E (radio_supported) */
};

/* Executes one request. Writes the reply payload after the status byte into
 * reply (at most HL_MAX_REPLY bytes), sets *reply_len, returns the status.
 * ctx is the pointer given to hostlink_init. */
typedef enum hl_status (*hl_handler)(void *ctx, uint8_t type, const uint8_t *payload,
                                     size_t len, uint8_t *reply, size_t *reply_len);

/* Writes bytes to the host. Provided by the platform. */
void hostlink_write(const uint8_t *data, size_t len);

struct hostlink {
    hl_handler handler;
    void *ctx;
    uint8_t rx[HL_MAX_FRAME];
    size_t rx_len;
    /* Reply cache: a request identical to the previous one (same seq and CRC)
     * is a retransmission and gets the same reply without running again. A
     * restarted host may reuse a seq for a new request; that one runs. */
    bool have_last;
    uint8_t last_seq;
    uint16_t last_crc;
    uint8_t last_reply[6 + 1 + HL_MAX_REPLY + 2]; /* header, status, reply, crc */
    size_t last_reply_len;
    uint8_t event_seq;
    uint32_t crc_errors; /* frames dropped for a bad CRC */
    uint32_t resyncs;    /* bytes skipped while searching for a frame start */
};

void hostlink_init(struct hostlink *hl, hl_handler handler, void *ctx);

/* Feeds received bytes; complete requests are executed and answered. */
void hostlink_feed(struct hostlink *hl, const uint8_t *data, size_t len);

/* Sends an unsolicited event whose payload is head followed by data. Written
 * straight to the host, so a large data part needs no stack copy. */
void hostlink_event(struct hostlink *hl, uint8_t type, const uint8_t *head, size_t head_len,
                    const uint8_t *data, size_t data_len);

/* Encodes one frame into out (HL_MAX_FRAME bytes); returns its length, or 0
 * when the payload is too long. */
size_t hostlink_encode(uint8_t *out, uint8_t seq, uint8_t type, const uint8_t *payload,
                       size_t len);

#endif
