#ifndef LINK_H
#define LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What the gateway exchanges with the CC1312 (docs/HOSTLINK.md), independent of
 * the wire: the host-link codec turns frames into these and back. */

/* TX flags. The CC1312 allocates MAC seq and counter, wraps and seals. */
#define LINK_TX_PAIRING_KEY    0x01
#define LINK_TX_FRAME_PENDING  0x04
#define LINK_TX_TRAIN_FULL     0x10
/* A short train on the device's channel sample, which the CC1312 learns from
 * its Enhanced ACKs; a full train while it knows none, or after a miss. */
#define LINK_TX_TRAIN_ANCHORED 0x20

struct link_tx {
    uint8_t flags;
    bool unicast;
    uint8_t dst[8];         /* unicast only */
    const uint8_t *packet;  /* the 6LoWPAN datagram */
    size_t len;
};

enum link_tx_result { LINK_ACKED = 0, LINK_SENT = 1, LINK_NO_ACK = 2, LINK_RADIO_ERROR = 3 };

enum link_event_type { LINK_EV_LOG, LINK_EV_RX, LINK_EV_BEACON_ANSWERED, LINK_EV_TX_DONE };

struct link_event {
    enum link_event_type type;
    union {
        struct {
            const char *text;
            size_t len;
        } log;
        /* A verified Data frame; packet is the 6LoWPAN datagram. */
        struct {
            bool pairing_key, broadcast, acked;
            int8_t rssi;
            uint8_t mac_seq;
            uint8_t src[8];
            uint32_t counter, rat;
            const uint8_t *packet;
            size_t len;
        } rx;
        struct {
            uint8_t joiner[8];
            uint32_t request_counter;
            int8_t rssi;
            uint8_t beacon_seq;
        } beacon;
        struct {
            uint8_t handle;
            enum link_tx_result result;
        } tx_done;
    };
};

#endif
