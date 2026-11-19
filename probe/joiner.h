#ifndef JOINER_H
#define JOINER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mac.h"

/* The device side of the join, for the join probe (docs/JOIN.md, docs/PROBE.md).
 * NOT part of the coordinator firmware: this poses as a joining device to read
 * what a coordinator sends in d/pair. Sans-IO and buffer-based, like the
 * coordinator role — the platform (probe/main.c) or a test drives it and moves
 * the bytes on and off the radio.
 *
 * The 6LoWPAN/CoAP layers live on the host, not here: the probe seals a
 * datagram the host built (a Router Solicitation), and hands back the opened
 * datagram of a d/pair for the host to decode. This stays MAC-only, like the
 * rest of firmware/src. */

struct joiner {
    uint8_t eui[8];
    uint8_t pairing_key[16];
    uint32_t counter; /* bumped per frame we send under the pairing key */
};

enum joiner_rx_kind {
    JOINER_RX_OTHER,   /* not for us, or not opened under the pairing key */
    JOINER_RX_BEACON,  /* an Enhanced Beacon addressed to us: a coordinator answered */
    JOINER_RX_PAIR,    /* a Data frame addressed to us, opened under the pairing key */
};

struct joiner_rx {
    enum joiner_rx_kind kind;
    uint8_t seq;
    uint8_t src[8];                 /* beacon: the coordinator's EUI */
    uint32_t counter;               /* beacon */
    uint8_t packet[MAC_MAX_FRAME];  /* pair: the opened 6LoWPAN datagram */
    size_t packet_len;
};

/* eui is the identity we present; counter is the first frame counter to use. */
void joiner_init(struct joiner *j, const uint8_t eui[8], const uint8_t pairing_key[16],
                 uint32_t counter);

/* Builds the Enhanced Beacon Request to broadcast; returns its length. seq is
 * the MAC sequence number, independent of the security counter. */
size_t joiner_beacon_request(struct joiner *j, uint8_t seq, uint8_t out[MAC_BEACON_REQ_LEN]);

/* Seals a host-built datagram (e.g. a Router Solicitation) as a broadcast Data
 * frame under the pairing key, from our EUI. Returns the length, or 0 when it
 * does not fit. seq is the MAC sequence number to use. */
size_t joiner_seal(struct joiner *j, uint8_t seq, const uint8_t *datagram, size_t n,
                   uint8_t out[MAC_MAX_FRAME]);

/* Classifies a received CRC-valid frame (no PHY CRC). Fills rx and returns
 * true when it is for us: an Enhanced Beacon, or a Data frame we open under the
 * pairing key (the d/pair we are after). Returns false otherwise. */
bool joiner_on_rx(struct joiner *j, const uint8_t *frame, size_t len, struct joiner_rx *rx);

#endif
