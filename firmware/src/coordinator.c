#include "coordinator.h"

#include <string.h>

#include "bytes.h"

/* ASCII "tado pairing key", shared by every valve. */
const uint8_t PAIRING_KEY[16] = {'t', 'a', 'd', 'o', ' ', 'p', 'a', 'i',
                                 'r', 'i', 'n', 'g', ' ', 'k', 'e', 'y'};

static const uint8_t EUI_SUFFIX[3] = {0xc5, 0x1b, 0x00};

/* TX flags (docs/HOSTLINK.md). */
#define TXF_PAIRING_KEY 0x01
#define TXF_BROADCAST   0x02
#define TXF_PENDING     0x04
#define TXF_TRAIN_SHIFT 4
#define TXF_TRAIN_MASK  0x30
#define TRAIN_NONE      0
#define TRAIN_FULL      1

/* RX event flags. */
#define RXF_PAIRING_KEY 0x01
#define RXF_BROADCAST   0x02
#define RXF_ACKED       0x04

static bool before(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }

static bool pair_window_open(struct coordinator *c)
{
    if (c->pair_open && !before(platform_ms(), c->pair_until_ms))
        c->pair_open = false;
    return c->pair_open;
}

/* At most per_second calls a second pass. */
static bool rate_ok(uint32_t *window_ms, unsigned *count, unsigned per_second)
{
    uint32_t now = platform_ms();
    if (!before(now, *window_ms + 1000u)) {
        *window_ms = now;
        *count = 0;
    }
    return (*count)++ < per_second;
}

/* LOG lines, built without printf so it stays out of the image. */
struct log_line {
    char text[LOG_MAX];
    size_t len;
};

static void log_str(struct log_line *l, const char *s)
{
    while (*s && l->len < LOG_MAX)
        l->text[l->len++] = *s++;
}

static void log_dec(struct log_line *l, uint32_t v)
{
    char digits[10];
    int n = 0;
    do
        digits[n++] = (char)('0' + v % 10u);
    while ((v /= 10u) != 0);
    while (n > 0 && l->len < LOG_MAX)
        l->text[l->len++] = digits[--n];
}

static void log_hex16(struct log_line *l, uint16_t v)
{
    log_str(l, "0x");
    for (int shift = 12; shift >= 0 && l->len < LOG_MAX; shift -= 4)
        l->text[l->len++] = "0123456789abcdef"[(v >> shift) & 0xf];
}

/* Rate-limited, so an error storm cannot flood the link. */
static void log_send(struct coordinator *c, const struct log_line *l)
{
    if (rate_ok(&c->log_window_ms, &c->log_count, LOG_PER_SECOND))
        hostlink_event(&c->link, HL_EV_LOG, (const uint8_t *)l->text, l->len, NULL, 0);
}

static void log_job(struct coordinator *c, uint8_t handle, const char *what)
{
    struct log_line l = {.len = 0};
    log_str(&l, "job ");
    log_dec(&l, handle);
    log_str(&l, " ");
    log_str(&l, what);
    log_send(c, &l);
}

static void tx_done(struct coordinator *c, enum tx_result result)
{
    uint8_t ev[2] = {c->queue[0].handle, (uint8_t)result};
    if (result == TX_ACKED || result == TX_SENT) {
        c->stats.jobs_ok++;
    } else {
        c->stats.jobs_failed++;
        log_job(c, ev[0], result == TX_NO_ACK ? "no-ack" : "radio-error");
    }
    memmove(&c->queue[0], &c->queue[1], (c->queue_len - 1) * sizeof c->queue[0]);
    c->queue_len--;
    c->job_on_air = c->awaiting_ack = false;
    hostlink_event(&c->link, HL_EV_TX_DONE, ev, sizeof ev, NULL, 0);
}

static enum hl_status do_config(struct coordinator *c, const uint8_t *p, size_t len)
{
    if (len != 8 + 16 + 4 + 4 || memcmp(&p[5], EUI_SUFFIX, 3) != 0)
        return HL_BAD_ARG;
    memcpy(c->eui, p, 8);
    memcpy(c->network_key, &p[8], 16);
    c->counter = get32(&p[24]);
    /* The retune aborts a job on air; the radio's late report of it then finds
     * no job. A job waiting for its ACK times out as usual. */
    if (c->job_on_air && !c->awaiting_ack)
        tx_done(c, TX_RADIO_ERROR);
    radio_set_channel(get32(&p[28]));
    c->configured = true;
    return HL_OK;
}

static enum hl_status do_tx(struct coordinator *c, const uint8_t *p, size_t len, uint8_t *reply,
                            size_t *reply_len)
{
    if (len < 1 + 8 + 1)
        return HL_BAD_ARG;
    uint8_t flags = p[0];
    unsigned train = (flags & TXF_TRAIN_MASK) >> TXF_TRAIN_SHIFT;
    if (train != TRAIN_NONE && train != TRAIN_FULL)
        return HL_BAD_ARG; /* anchored short trains come with M0 */
    if (c->queue_len == TX_QUEUE_LEN)
        return HL_BUSY;

    const uint8_t *key = (flags & TXF_PAIRING_KEY) ? PAIRING_KEY : c->network_key;
    bool pending = (flags & TXF_PENDING) != 0;
    const uint8_t *dst = &p[1], *packet = &p[9];
    size_t packet_len = len - 9;
    struct tx_job *job = &c->queue[c->queue_len];
    job->unicast = !(flags & TXF_BROADCAST);
    job->seq = c->seq;
    job->len = job->unicast
                   ? mac_seal_unicast(job->frame, key, pending, c->seq, dst, c->eui, c->counter,
                                      packet, packet_len)
                   : mac_seal_broadcast(job->frame, key, pending, c->seq, c->eui, c->counter,
                                        packet, packet_len);
    if (job->len == 0)
        return HL_BAD_ARG;
    if (job->unicast)
        mac_short_addr(dst, job->dst_short);
    else
        job->dst_short[0] = job->dst_short[1] = 0xff;
    job->wake_frames = train == TRAIN_FULL ? MAC_TRAIN_FULL : 0;
    job->handle = c->next_handle++;
    c->queue_len++;

    reply[0] = job->handle;
    reply[1] = job->seq;
    put32(&reply[2], c->counter);
    *reply_len = 6;
    c->seq++;
    c->counter++;
    return HL_OK;
}

static void put_stats(struct coordinator *c, uint8_t *reply, size_t *reply_len)
{
    uint32_t v[12];
    radio_rx_counts(&v[0], &v[1]);
    v[2] = c->stats.rx_unverified;
    v[3] = c->stats.beacons_answered;
    v[4] = c->stats.acks_sent;
    v[5] = c->stats.answers_refused;
    v[6] = c->stats.jobs_ok;
    v[7] = c->stats.jobs_failed;
    v[8] = c->stats.jobs_timed_out;
    v[9] = c->stats.radio_errors;
    v[10] = c->link.crc_errors;
    v[11] = c->link.resyncs;
    for (size_t i = 0; i < 12; i++)
        put32(&reply[4 * i], v[i]);
    *reply_len = sizeof v;
}

static enum hl_status handle(void *ctx, uint8_t type, const uint8_t *p, size_t len,
                             uint8_t *reply, size_t *reply_len)
{
    struct coordinator *c = ctx;
    switch (type) {
    case HL_PING:
        put16(&reply[0], FW_VERSION);
        put32(&reply[2], radio_rat_now());
        *reply_len = 6;
        return HL_OK;
    case HL_CONFIG:
        return do_config(c, p, len);
    case HL_PAIR_WINDOW:
        if (len != 2)
            return HL_BAD_ARG;
        if (!c->configured)
            return HL_NOT_CONFIGURED;
        c->pair_open = get16(p) != 0;
        c->pair_until_ms = platform_ms() + 1000u * get16(p);
        return HL_OK;
    case HL_TX:
        if (!c->configured)
            return HL_NOT_CONFIGURED;
        return do_tx(c, p, len, reply, reply_len);
    case HL_STATS: /* also before CONFIG: receive-only debugging */
        if (len != 0)
            return HL_BAD_ARG;
        put_stats(c, reply, reply_len);
        return HL_OK;
    default:
        return HL_UNKNOWN;
    }
}

void coord_init(struct coordinator *c)
{
    memset(c, 0, sizeof *c);
    hostlink_init(&c->link, handle, c);
}

void coord_boot(struct coordinator *c, const char *reset_reason)
{
    struct log_line l = {.len = 0};
    log_str(&l, "boot fw=");
    log_dec(&l, FW_VERSION);
    log_str(&l, " reset=");
    log_str(&l, reset_reason);
    log_send(c, &l);
}

void coord_on_radio_error(struct coordinator *c, uint16_t status)
{
    c->stats.radio_errors++;
    struct log_line l = {.len = 0};
    log_str(&l, "rf error ");
    log_hex16(&l, status);
    log_send(c, &l);
}

/* The radio refused a beacon or ACK: a job was on air. */
static void answer_refused(struct coordinator *c, const char *what, uint8_t seq)
{
    c->stats.answers_refused++;
    struct log_line l = {.len = 0};
    log_str(&l, "refused ");
    log_str(&l, what);
    log_str(&l, " seq=");
    log_dec(&l, seq);
    log_send(c, &l);
}

void coord_on_host_bytes(struct coordinator *c, const uint8_t *data, size_t len)
{
    hostlink_feed(&c->link, data, len);
}

static void report_unverified(struct coordinator *c, const uint8_t *frame, size_t len,
                              int8_t rssi, uint32_t rat_end)
{
    c->stats.rx_unverified++;
    if (!rate_ok(&c->unverified_window_ms, &c->unverified_count, UNVERIFIED_PER_SECOND))
        return;
    uint8_t head[1 + 4];
    head[0] = (uint8_t)rssi;
    put32(&head[1], rat_end);
    hostlink_event(&c->link, HL_EV_RX_UNVERIFIED, head, sizeof head, frame, len);
}

static void answer_beacon_request(struct coordinator *c, const uint8_t *frame, size_t len,
                                  int8_t rssi, uint32_t rat_end)
{
    uint8_t req_seq, joiner[8];
    uint32_t req_counter;
    if (!pair_window_open(c) ||
        !mac_parse_beacon_request(frame, len, PAIRING_KEY, &req_seq, joiner, &req_counter)) {
        report_unverified(c, frame, len, rssi, rat_end);
        return;
    }
    /* seq and counter are spent only once the radio takes the beacon; when it
     * is busy the joiner simply asks again. */
    uint8_t beacon[MAC_ENH_BEACON_LEN];
    uint8_t beacon_seq = c->seq;
    mac_build_enh_beacon(beacon, PAIRING_KEY, beacon_seq, joiner, c->eui, c->counter);
    if (!radio_send_at(beacon, sizeof beacon, rat_end + BEACON_DELAY_RAT)) {
        answer_refused(c, "beacon", beacon_seq);
        return;
    }
    c->stats.beacons_answered++;
    c->seq++;
    c->counter++;

    uint8_t ev[8 + 4 + 1 + 1];
    memcpy(ev, joiner, 8);
    put32(&ev[8], req_counter);
    ev[12] = (uint8_t)rssi;
    ev[13] = beacon_seq;
    hostlink_event(&c->link, HL_EV_BEACON_ANSWERED, ev, sizeof ev, NULL, 0);
}

static void receive_data(struct coordinator *c, enum mac_kind kind, const uint8_t *frame,
                         size_t len, int8_t rssi, uint32_t rat_end)
{
    uint8_t scratch[MAC_MAX_FRAME];
    struct mac_data d;
    uint8_t flags = 0;
    bool to_us = mac_data_addressed_to(frame, len, kind, c->eui);

    /* A joiner talks the pairing key until d/pair; only an open window lets
     * that key in. */
    bool ok = to_us && mac_open_data(frame, len, c->network_key, scratch, &d);
    if (to_us && !ok && pair_window_open(c)) {
        ok = mac_open_data(frame, len, PAIRING_KEY, scratch, &d);
        flags |= RXF_PAIRING_KEY;
    }
    if (!ok) {
        report_unverified(c, frame, len, rssi, rat_end);
        return;
    }
    if (d.ack_request) {
        /* Phase: free-running timer in 320 us units, truncated to 16 bits. */
        uint8_t ack[MAC_ENH_ACK_LEN];
        mac_build_enh_ack(ack, d.seq, d.src, c->eui, (uint16_t)(rat_end / 1280u));
        if (radio_send_at(ack, sizeof ack, rat_end + ACK_DELAY_RAT)) {
            c->stats.acks_sent++;
            flags |= RXF_ACKED;
        } else {
            answer_refused(c, "ack", d.seq);
        }
    }
    if (d.broadcast)
        flags |= RXF_BROADCAST;

    uint8_t head[1 + 1 + 1 + 8 + 4 + 4];
    head[0] = flags;
    head[1] = (uint8_t)rssi;
    head[2] = d.seq;
    memcpy(&head[3], d.src, 8);
    put32(&head[11], d.counter);
    put32(&head[15], rat_end);
    hostlink_event(&c->link, HL_EV_RX, head, sizeof head, d.packet, d.packet_len);
}

static void receive_ack(struct coordinator *c, const uint8_t *frame, size_t len, int8_t rssi,
                        uint32_t rat_end)
{
    uint8_t seq, dst[8];
    if (!mac_parse_enh_ack(frame, len, &seq, dst)) {
        report_unverified(c, frame, len, rssi, rat_end);
        return;
    }
    /* An overheard ACK carries another node's seq; only ours closes the job.
     * It counts before the radio reports the Data frame done: both can arrive
     * in the same main-loop pass, in either order. */
    if (c->job_on_air && c->queue[0].unicast && seq == c->queue[0].seq &&
        memcmp(dst, c->eui, 8) == 0)
        tx_done(c, TX_ACKED);
}

void coord_on_rx(struct coordinator *c, const uint8_t *frame, size_t len, int8_t rssi,
                 uint32_t rat_end)
{
    /* Nothing opens before CONFIG; counted, not reported. */
    if (!c->configured) {
        c->stats.rx_unverified++;
        return;
    }
    enum mac_kind kind = mac_classify(frame, len);
    switch (kind) {
    case MAC_KIND_BEACON_REQUEST:
        answer_beacon_request(c, frame, len, rssi, rat_end);
        break;
    case MAC_KIND_UNICAST:
    case MAC_KIND_BROADCAST:
        receive_data(c, kind, frame, len, rssi, rat_end);
        break;
    case MAC_KIND_ENH_ACK:
        receive_ack(c, frame, len, rssi, rat_end);
        break;
    default:
        report_unverified(c, frame, len, rssi, rat_end);
        break;
    }
}

void coord_on_job_sent(struct coordinator *c, uint8_t token, bool ok)
{
    /* A late report for a job already closed (by its ACK, a CONFIG or the
     * watchdog) must not land on the next one. */
    if (!c->job_on_air || token != c->queue[0].handle)
        return;
    if (!ok)
        tx_done(c, TX_RADIO_ERROR);
    else if (!c->queue[0].unicast)
        tx_done(c, TX_SENT);
    else {
        c->awaiting_ack = true;
        c->ack_deadline_ms = platform_ms() + ACK_WAIT_MS;
    }
}

/* How long a job may stay on air: the wake-up train, the Data frame at 50 kbps
 * (0.16 ms a byte) and a margin. */
static uint32_t job_airtime_ms(const struct tx_job *j)
{
    return j->wake_frames * WAKE_STEP_MS + (uint32_t)j->len * 4u / 25u + JOB_MARGIN_MS;
}

void coord_poll(struct coordinator *c)
{
    if (c->awaiting_ack && !before(platform_ms(), c->ack_deadline_ms))
        tx_done(c, TX_NO_ACK);
    /* A lost completion would hold the radio forever: give up on the job. */
    if (c->job_on_air && !c->awaiting_ack && !before(platform_ms(), c->job_deadline_ms)) {
        c->stats.jobs_timed_out++;
        log_job(c, c->queue[0].handle, "timeout");
        radio_abort_job();
        tx_done(c, TX_RADIO_ERROR);
    }
    if (c->queue_len > 0 && !c->job_on_air) {
        struct tx_job *j = &c->queue[0];
        if (radio_send_job(j->dst_short, j->seq, j->wake_frames, j->frame, j->len, j->handle)) {
            c->job_on_air = true;
            c->job_deadline_ms = platform_ms() + job_airtime_ms(j);
        }
    }
}
