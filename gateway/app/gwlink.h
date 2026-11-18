#ifndef GWLINK_H
#define GWLINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hlc.h"
#include "link.h"

/* The host link as the gateway uses it: blocking requests over a byte stream
 * (the UART on the ESP32), with the events that arrive meanwhile queued for
 * later. This is what session_ops.tx needs (session.h): a TX returns the
 * CC1312's handle only once it answered, and an HL_BUSY answer is retried.
 *
 * Not thread-safe: one task makes the requests and drains the events. */

/* A TX answered HL_BUSY (the CC1312's queue is full) goes again every 100 ms
 * for up to 8 s, as the Java host does. */
#define GWLINK_BUSY_RETRY_MS 100u
#define GWLINK_BUSY_GIVE_UP_MS 8000u
#define GWLINK_MAX_EVENTS 16

struct gwlink_ops {
    void *ctx;
    void (*write)(void *ctx, const uint8_t *bytes, size_t len);
    /* Up to cap bytes, waiting at most timeout_ms for the first; 0 when none
     * came. */
    size_t (*read)(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms);
    uint32_t (*now_ms)(void *ctx);
};

/* A queued event with its own copy of the bytes it points to (log text is
 * NUL-terminated there). order counts events and replies as they arrived, so
 * an event can be told apart from a reply that came after it
 * (gwlink.last_reply_order). */
struct gwlink_event {
    struct link_event ev;
    uint32_t order;
    uint8_t data[HL_MAX_FRAME + 1];
};

struct gwlink {
    struct hlc hlc;
    struct gwlink_ops ops;
    uint32_t order;
    uint32_t last_reply_order;
    uint32_t events_dropped; /* the queue was full: v0 accepts losing an event */
    size_t head, count;
    struct gwlink_event events[GWLINK_MAX_EVENTS];
    struct {
        bool got;
        uint8_t type;
        int status;
        size_t len;
        uint8_t data[HL_MAX_FRAME];
    } reply;
};

/* first_seq must be random per boot (hlc.h). */
void gwlink_init(struct gwlink *l, const struct gwlink_ops *ops, uint8_t first_seq);

/* Requests: each returns the reply's status (enum hl_status), or -1 when the
 * CC1312 did not answer or the reply was too short. */
int gwlink_ping(struct gwlink *l, uint16_t *fw_version);
int gwlink_config(struct gwlink *l, const uint8_t coordinator[8], const uint8_t network_key[16],
                  uint32_t counter_floor, uint32_t channel_khz);
int gwlink_pair_window(struct gwlink *l, uint16_t seconds);

/* Queues a frame: the CC1312's handle (0-255) and the frame counter it gave
 * the frame, or -1 when it was refused; status (may be NULL) gets the last
 * reply's status, -1 when there was none. */
int gwlink_tx(struct gwlink *l, const struct link_tx *tx, uint32_t *counter, int *status);

/* Reads what arrived within timeout_ms and drives the resends. */
void gwlink_poll(struct gwlink *l, uint32_t timeout_ms);

/* Takes the oldest queued event; false when there is none. */
bool gwlink_next_event(struct gwlink *l, struct gwlink_event *out);

#endif
