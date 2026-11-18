#ifndef HOSTLINK_H
#define HOSTLINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hlframe.h"
#include "hlproto.h"

/* The CC1312 side of the host link v0 (docs/HOSTLINK.md); framing in
 * hlframe.h, message types and flags in hlproto.h. */

/* Reply payload after the status byte; the largest is STATS, 15 u32. */
#define HL_MAX_REPLY   64

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
    struct hl_rx rx;
    /* Reply cache: a request identical to the previous one (same seq and CRC)
     * is a retransmission and gets the same reply without running again. A
     * restarted host may reuse a seq for a new request; that one runs. */
    bool have_last;
    uint8_t last_seq;
    uint16_t last_crc;
    uint8_t last_reply[6 + 1 + HL_MAX_REPLY + 2]; /* header, status, reply, crc */
    size_t last_reply_len;
    uint8_t event_seq;
};

void hostlink_init(struct hostlink *hl, hl_handler handler, void *ctx);

/* Feeds received bytes; complete requests are executed and answered. */
void hostlink_feed(struct hostlink *hl, const uint8_t *data, size_t len);

/* Sends an unsolicited event whose payload is head followed by data. Written
 * straight to the host, so a large data part needs no stack copy. */
void hostlink_event(struct hostlink *hl, uint8_t type, const uint8_t *head, size_t head_len,
                    const uint8_t *data, size_t data_len);

#endif
