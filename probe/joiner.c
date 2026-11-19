#include "joiner.h"

#include <string.h>

void joiner_init(struct joiner *j, const uint8_t eui[8], const uint8_t pairing_key[16],
                 uint32_t counter)
{
    memset(j, 0, sizeof *j);
    memcpy(j->eui, eui, 8);
    memcpy(j->pairing_key, pairing_key, 16);
    j->counter = counter;
}

size_t joiner_beacon_request(struct joiner *j, uint8_t seq, uint8_t out[MAC_BEACON_REQ_LEN])
{
    mac_build_beacon_request(out, j->pairing_key, seq, j->eui, j->counter);
    j->counter++;
    return MAC_BEACON_REQ_LEN;
}

size_t joiner_seal(struct joiner *j, uint8_t seq, const uint8_t *datagram, size_t n,
                   uint8_t out[MAC_MAX_FRAME])
{
    size_t len = mac_seal_broadcast(out, j->pairing_key, false, seq, j->eui, j->counter, datagram, n);
    if (len)
        j->counter++;
    return len;
}

bool joiner_on_rx(struct joiner *j, const uint8_t *frame, size_t len, struct joiner_rx *rx)
{
    memset(rx, 0, sizeof *rx);
    enum mac_kind kind = mac_classify(frame, len);

    uint8_t dst[8];
    if (mac_parse_enh_beacon(frame, len, j->pairing_key, dst, rx->src, &rx->seq, &rx->counter) &&
        memcmp(dst, j->eui, 8) == 0) {
        rx->kind = JOINER_RX_BEACON;
        return true;
    }

    /* A d/pair is a unicast Data frame to us, sealed under the pairing key. */
    if ((kind == MAC_KIND_UNICAST || kind == MAC_KIND_BROADCAST) &&
        mac_data_addressed_to(frame, len, kind, j->eui)) {
        uint8_t scratch[MAC_MAX_FRAME];
        struct mac_data d;
        if (mac_open_data(frame, len, j->pairing_key, scratch, &d) && !d.broadcast) {
            rx->kind = JOINER_RX_PAIR;
            rx->seq = d.seq;
            memcpy(rx->src, d.src, 8);
            rx->counter = d.counter;
            rx->packet_len = d.packet_len;
            if (d.packet_len <= sizeof rx->packet)
                memcpy(rx->packet, d.packet, d.packet_len);
            return true;
        }
    }

    rx->kind = JOINER_RX_OTHER;
    return false;
}
