#include "hostlink.h"

#include <string.h>

#include "bytes.h"
#include "crc.h"

void hostlink_init(struct hostlink *hl, hl_handler handler, void *ctx)
{
    memset(hl, 0, sizeof *hl);
    hl->handler = handler;
    hl->ctx = ctx;
}

static void handle_request(void *ctx, uint16_t crc, uint8_t seq, uint8_t type, const uint8_t *payload,
                           size_t len)
{
    struct hostlink *hl = ctx;
    if (hl->have_last && seq == hl->last_seq && crc == hl->last_crc) {
        hostlink_write(hl->last_reply, hl->last_reply_len);
        return;
    }
    /* The reply is built in place in the cache: status at [6], payload after. */
    uint8_t *reply = &hl->last_reply[6];
    size_t reply_len = 0;
    reply[0] = (uint8_t)hl->handler(hl->ctx, type, payload, len, &reply[1], &reply_len);
    hl->last_reply_len = hl_frame_finish(hl->last_reply, seq, type | HL_REPLY_FLAG, reply_len + 1);
    hl->last_seq = seq;
    hl->last_crc = crc;
    hl->have_last = true;
    hostlink_write(hl->last_reply, hl->last_reply_len);
}

void hostlink_feed(struct hostlink *hl, const uint8_t *data, size_t len)
{
    hl_rx_feed(&hl->rx, data, len, handle_request, hl);
}

void hostlink_event(struct hostlink *hl, uint8_t type, const uint8_t *head, size_t head_len,
                    const uint8_t *data, size_t data_len)
{
    size_t len = head_len + data_len;
    if (len > HL_MAX_PAYLOAD)
        return;
    uint8_t header[6], crc_bytes[2];
    uint16_t crc = hl_frame_header(header, hl->event_seq++, type, len);
    crc = crc16_ccitt_false(crc, head, head_len);
    crc = crc16_ccitt_false(crc, data, data_len);
    put16(crc_bytes, crc);
    hostlink_write(header, sizeof header);
    hostlink_write(head, head_len);
    if (data_len)
        hostlink_write(data, data_len);
    hostlink_write(crc_bytes, sizeof crc_bytes);
}
