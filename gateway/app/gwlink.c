#include "gwlink.h"

#include <string.h>

/* hlc's callbacks: copy out, act after hlc_feed returns (hlc.h). */

static void on_reply(void *ctx, const struct hlc_reply *r)
{
    struct gwlink *l = ctx;
    l->reply.got = true;
    l->reply.type = r->type;
    l->reply.status = r->status;
    l->reply.len = r->len < sizeof l->reply.data ? r->len : sizeof l->reply.data;
    if (l->reply.len)
        memcpy(l->reply.data, r->data, l->reply.len);
    l->last_reply_order = ++l->order;
}

/* Points the event at its own copy of the bytes. */
static void rebase(struct gwlink_event *e)
{
    if (e->ev.type == LINK_EV_RX)
        e->ev.rx.packet = e->data;
    else if (e->ev.type == LINK_EV_LOG)
        e->ev.log.text = (const char *)e->data;
}

static void on_event(void *ctx, const struct link_event *ev)
{
    struct gwlink *l = ctx;
    if (l->count == GWLINK_MAX_EVENTS) {
        l->events_dropped++;
        return;
    }
    struct gwlink_event *e = &l->events[(l->head + l->count++) % GWLINK_MAX_EVENTS];
    e->ev = *ev;
    e->order = ++l->order;
    /* The pointers go into hlc's receive buffer, which the next feed reuses. */
    if (ev->type == LINK_EV_RX) {
        size_t n = ev->rx.len < HL_MAX_FRAME ? ev->rx.len : HL_MAX_FRAME;
        memcpy(e->data, ev->rx.packet, n);
        e->ev.rx.len = n;
    } else if (ev->type == LINK_EV_LOG) {
        size_t n = ev->log.len < HL_MAX_FRAME ? ev->log.len : HL_MAX_FRAME;
        memcpy(e->data, ev->log.text, n);
        e->data[n] = '\0';
        e->ev.log.len = n;
    }
    rebase(e);
}

static void write_bytes(void *ctx, const uint8_t *bytes, size_t len)
{
    struct gwlink *l = ctx;
    l->ops.write(l->ops.ctx, bytes, len);
}

void gwlink_init(struct gwlink *l, const struct gwlink_ops *ops, uint8_t first_seq)
{
    memset(l, 0, sizeof *l);
    l->ops = *ops;
    struct hlc_ops h = {.ctx = l, .write = write_bytes, .on_reply = on_reply, .on_event = on_event};
    hlc_init(&l->hlc, &h, first_seq);
}

static uint32_t now(struct gwlink *l) { return l->ops.now_ms(l->ops.ctx); }

void gwlink_poll(struct gwlink *l, uint32_t timeout_ms)
{
    uint8_t buf[256];
    size_t n = l->ops.read(l->ops.ctx, buf, sizeof buf, timeout_ms);
    if (n)
        hlc_feed(&l->hlc, buf, n);
    hlc_tick(&l->hlc, now(l));
}

/* Waits for the reply to the request just sent; hlc resends it and reports
 * status -1 once it gives up. */
static int wait_reply(struct gwlink *l, bool sent)
{
    if (!sent)
        return -1;
    while (!l->reply.got)
        gwlink_poll(l, 20);
    return l->reply.status;
}

static struct hlc_reply last_reply(const struct gwlink *l)
{
    return (struct hlc_reply){
        .type = l->reply.type, .status = l->reply.status, .data = l->reply.data, .len = l->reply.len};
}

int gwlink_ping(struct gwlink *l, uint16_t *fw_version)
{
    l->reply.got = false;
    int status = wait_reply(l, hlc_ping(&l->hlc, now(l)));
    struct hlc_reply r = last_reply(l);
    uint32_t rat;
    if (status == HL_OK && !hlc_ping_reply(&r, fw_version, &rat))
        return -1;
    return status;
}

int gwlink_config(struct gwlink *l, const uint8_t coordinator[8], const uint8_t network_key[16],
                  uint32_t counter_floor, uint32_t channel_khz)
{
    l->reply.got = false;
    return wait_reply(l, hlc_config(&l->hlc, coordinator, network_key, counter_floor, channel_khz, now(l)));
}

int gwlink_pair_window(struct gwlink *l, uint16_t seconds)
{
    l->reply.got = false;
    return wait_reply(l, hlc_pair_window(&l->hlc, seconds, now(l)));
}

int gwlink_tx(struct gwlink *l, const struct link_tx *tx, uint32_t *counter, int *status_out)
{
    uint32_t first = now(l);
    for (;;) {
        l->reply.got = false;
        int status = wait_reply(l, hlc_tx(&l->hlc, tx, now(l)));
        if (status_out)
            *status_out = status;
        if (status == HL_OK) {
            struct hlc_reply r = last_reply(l);
            uint8_t handle, mac_seq;
            uint32_t c;
            if (!hlc_tx_reply(&r, &handle, &mac_seq, &c))
                return -1;
            if (counter)
                *counter = c;
            return handle;
        }
        if (status != HL_BUSY || now(l) - first >= GWLINK_BUSY_GIVE_UP_MS)
            return -1;
        /* Keep reading while waiting: TX_DONEs free the queue. */
        uint32_t wait_from = now(l);
        while (now(l) - wait_from < GWLINK_BUSY_RETRY_MS)
            gwlink_poll(l, GWLINK_BUSY_RETRY_MS - (now(l) - wait_from));
    }
}

bool gwlink_next_event(struct gwlink *l, struct gwlink_event *out)
{
    if (l->count == 0)
        return false;
    struct gwlink_event *e = &l->events[l->head];
    *out = *e;
    rebase(out); /* its own data, not the slot the next event reuses */
    l->head = (l->head + 1) % GWLINK_MAX_EVENTS;
    l->count--;
    return true;
}
