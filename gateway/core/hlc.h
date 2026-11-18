#ifndef HLC_H
#define HLC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hlframe.h"
#include "hlproto.h"
#include "link.h"

/* The gateway's side of the host link (docs/HOSTLINK.md): it frames requests
 * to the CC1312 and parses its replies and events, without doing any I/O.
 *
 * One request is in flight at a time. It is kept as its encoded frame, so a
 * resend after HLC_REPLY_TIMEOUT_MS is the identical bytes: the CC1312 takes
 * same seq and same CRC as a retransmission and replays its reply instead of
 * running the request again (a TX would otherwise go on air twice). Bytes go
 * out through ops.write; replies and events come back through ops.on_reply and
 * ops.on_event as hlc_feed parses them. Those callbacks must not call hlc_feed
 * or send a request: copy what they need and act after hlc_feed returns.
 *
 * Policy stays with the caller: a TX answered HL_BUSY (the CC1312's queue is
 * full) is retried by the caller as a new request, as the Java host does
 * (every 100 ms, for up to 8 s). */

#define HLC_REPLY_TIMEOUT_MS 200u
#define HLC_ATTEMPTS         4 /* the first send and up to three resends */

struct hlc_reply {
    uint8_t type;        /* the request's type */
    int status;          /* enum hl_status, or -1 when the CC1312 never answered */
    const uint8_t *data; /* after the status byte */
    size_t len;
};

struct hlc_ops {
    void *ctx;
    void (*write)(void *ctx, const uint8_t *bytes, size_t len);
    void (*on_reply)(void *ctx, const struct hlc_reply *reply);
    void (*on_event)(void *ctx, const struct link_event *ev);
};

struct hlc {
    struct hlc_ops ops;
    struct hl_rx rx;
    uint32_t stray;      /* replies that answer no request in flight */
    /* The request in flight, as its encoded frame (seq at [4], type at [5]). */
    uint8_t next_seq;
    bool pending;
    int attempts;
    uint32_t sent_ms;
    size_t frame_len;
    uint8_t frame[HL_MAX_FRAME];
};

/* first_seq must be random per boot: the CC1312 answers a request with the
 * seq and CRC of the last one from its cache, so a restarted gateway that
 * repeats both would get the old reply and its request would not run. */
void hlc_init(struct hlc *h, const struct hlc_ops *ops, uint8_t first_seq);

/* Whether a request is waiting for its reply; a new one must wait until not. */
bool hlc_busy(const struct hlc *h);

/* Requests. False (nothing sent) while another is in flight or when the
 * payload does not fit a frame. */
bool hlc_ping(struct hlc *h, uint32_t now_ms);
bool hlc_config(struct hlc *h, const uint8_t coordinator[8], const uint8_t network_key[16], uint32_t counter_floor,
                uint32_t channel_khz, uint32_t now_ms);
bool hlc_pair_window(struct hlc *h, uint16_t seconds, uint32_t now_ms);
bool hlc_tx(struct hlc *h, const struct link_tx *tx, uint32_t now_ms);
bool hlc_stats(struct hlc *h, uint32_t now_ms);

/* Parses received bytes; whole frames go to on_reply / on_event. */
void hlc_feed(struct hlc *h, const uint8_t *bytes, size_t len);

/* Resends the request in flight after HLC_REPLY_TIMEOUT_MS, and gives up after
 * HLC_ATTEMPTS sends with an on_reply of status -1. */
void hlc_tick(struct hlc *h, uint32_t now_ms);

/* Reply payloads (after the status byte); false when too short. */
bool hlc_ping_reply(const struct hlc_reply *r, uint16_t *fw_version, uint32_t *rat);
bool hlc_tx_reply(const struct hlc_reply *r, uint8_t *handle, uint8_t *mac_seq, uint32_t *counter);
/* The STATS counters in wire order; returns how many, at most max (a newer
 * firmware may append counters this gateway does not know). */
size_t hlc_stats_reply(const struct hlc_reply *r, uint32_t *counters, size_t max);

#endif
