#include "hlc.h"

#include <string.h>

#include "bytes.h"

void hlc_init(struct hlc *h, const struct hlc_ops *ops, uint8_t first_seq)
{
    memset(h, 0, sizeof *h);
    h->ops = *ops;
    h->next_seq = first_seq;
}

bool hlc_busy(const struct hlc *h) { return h->pending; }

/* Sends the request whose len-byte payload is already at h->frame[6] as the
 * request in flight. */
static bool request(struct hlc *h, uint8_t type, size_t len, uint32_t now)
{
    uint8_t seq = h->next_seq++;
    h->frame_len = hl_frame_finish(h->frame, seq, type, len);
    if (h->frame_len == 0)
        return false;
    h->pending = true;
    h->attempts = 1;
    h->sent_ms = now;
    h->ops.write(h->ops.ctx, h->frame, h->frame_len);
    return true;
}

/* Payloads are written straight into the frame, after its 6-byte header. */
static uint8_t *payload(struct hlc *h) { return h->pending ? NULL : &h->frame[6]; }

bool hlc_ping(struct hlc *h, uint32_t now_ms) { return payload(h) && request(h, HL_PING, 0, now_ms); }

bool hlc_config(struct hlc *h, const uint8_t coordinator[8], const uint8_t network_key[16], uint32_t counter_floor,
                uint32_t channel_khz, uint32_t now_ms)
{
    uint8_t *p = payload(h);
    if (!p)
        return false;
    memcpy(p, coordinator, 8);
    memcpy(&p[8], network_key, 16);
    put32(&p[24], counter_floor);
    put32(&p[28], channel_khz);
    return request(h, HL_CONFIG, 8 + 16 + 4 + 4, now_ms);
}

bool hlc_pair_window(struct hlc *h, uint16_t seconds, uint32_t now_ms)
{
    uint8_t *p = payload(h);
    if (!p)
        return false;
    put16(p, seconds);
    return request(h, HL_PAIR_WINDOW, 2, now_ms);
}

bool hlc_tx(struct hlc *h, const struct link_tx *tx, uint32_t now_ms)
{
    uint8_t *p = payload(h);
    if (!p || 1 + 8 + tx->len > HL_MAX_PAYLOAD)
        return false;
    p[0] = (uint8_t)(tx->unicast ? tx->flags : tx->flags | HL_TXF_BROADCAST);
    if (tx->unicast)
        memcpy(&p[1], tx->dst, 8);
    else
        memset(&p[1], 0, 8);
    memcpy(&p[9], tx->packet, tx->len);
    return request(h, HL_TX, 9 + tx->len, now_ms);
}

bool hlc_stats(struct hlc *h, uint32_t now_ms) { return payload(h) && request(h, HL_STATS, 0, now_ms); }

void hlc_tick(struct hlc *h, uint32_t now_ms)
{
    if (!h->pending || now_ms - h->sent_ms < HLC_REPLY_TIMEOUT_MS)
        return;
    if (h->attempts >= HLC_ATTEMPTS) {
        h->pending = false;
        struct hlc_reply r = {.type = h->frame[5], .status = -1};
        h->ops.on_reply(h->ops.ctx, &r);
        return;
    }
    h->attempts++;
    h->sent_ms = now_ms;
    h->ops.write(h->ops.ctx, h->frame, h->frame_len); /* the same bytes: a retransmission */
}

/* --- receiving --------------------------------------------------------------- */

static void event(struct hlc *h, uint8_t type, const uint8_t *p, size_t len)
{
    struct link_event ev = {0};
    switch (type) {
    case HL_EV_LOG:
        ev.type = LINK_EV_LOG;
        ev.log.text = (const char *)p;
        ev.log.len = len;
        break;
    case HL_EV_RX:
        if (len < 1 + 1 + 1 + 8 + 4 + 4)
            return;
        ev.type = LINK_EV_RX;
        ev.rx.pairing_key = p[0] & HL_RXF_PAIRING_KEY;
        ev.rx.broadcast = p[0] & HL_RXF_BROADCAST;
        ev.rx.acked = p[0] & HL_RXF_ACKED;
        ev.rx.rssi = (int8_t)p[1];
        ev.rx.mac_seq = p[2];
        memcpy(ev.rx.src, &p[3], 8);
        ev.rx.counter = get32(&p[11]);
        ev.rx.rat = get32(&p[15]);
        ev.rx.packet = &p[19];
        ev.rx.len = len - 19;
        break;
    case HL_EV_BEACON_ANSWERED:
        if (len < 8 + 4 + 1 + 1)
            return;
        ev.type = LINK_EV_BEACON_ANSWERED;
        memcpy(ev.beacon.joiner, p, 8);
        ev.beacon.request_counter = get32(&p[8]);
        ev.beacon.rssi = (int8_t)p[12];
        ev.beacon.beacon_seq = p[13];
        break;
    case HL_EV_TX_DONE:
        if (len < 2)
            return;
        ev.type = LINK_EV_TX_DONE;
        ev.tx_done.handle = p[0];
        /* An unknown code from a newer firmware reads as an error, not as success. */
        ev.tx_done.result = p[1] <= LINK_RADIO_ERROR ? (enum link_tx_result)p[1] : LINK_RADIO_ERROR;
        break;
    default:
        return; /* RX_UNVERIFIED (debugging only) and types this gateway does not know */
    }
    h->ops.on_event(h->ops.ctx, &ev);
}

static void frame(void *ctx, uint16_t crc, uint8_t seq, uint8_t type, const uint8_t *p, size_t len)
{
    struct hlc *h = ctx;
    (void)crc;
    if (!(type & HL_REPLY_FLAG) || type >= HL_EV_LOG) {
        event(h, type, p, len);
        return;
    }
    if (!h->pending || seq != h->frame[4] || type != (h->frame[5] | HL_REPLY_FLAG) || len < 1) {
        h->stray++; /* a late reply to a request already given up, or noise */
        return;
    }
    h->pending = false;
    struct hlc_reply r = {.type = h->frame[5], .status = p[0], .data = &p[1], .len = len - 1};
    h->ops.on_reply(h->ops.ctx, &r);
}

void hlc_feed(struct hlc *h, const uint8_t *bytes, size_t len) { hl_rx_feed(&h->rx, bytes, len, frame, h); }

/* --- replies ----------------------------------------------------------------- */

bool hlc_ping_reply(const struct hlc_reply *r, uint16_t *fw_version, uint32_t *rat)
{
    if (r->status != HL_OK || r->len < 6)
        return false;
    *fw_version = get16(r->data);
    *rat = get32(&r->data[2]);
    return true;
}

bool hlc_tx_reply(const struct hlc_reply *r, uint8_t *handle, uint8_t *mac_seq, uint32_t *counter)
{
    if (r->status != HL_OK || r->len < 6)
        return false;
    *handle = r->data[0];
    *mac_seq = r->data[1];
    *counter = get32(&r->data[2]);
    return true;
}

size_t hlc_stats_reply(const struct hlc_reply *r, uint32_t *counters, size_t max)
{
    if (r->status != HL_OK)
        return 0;
    size_t n = r->len / 4 < max ? r->len / 4 : max;
    for (size_t i = 0; i < n; i++)
        counters[i] = get32(&r->data[4 * i]);
    return n;
}
