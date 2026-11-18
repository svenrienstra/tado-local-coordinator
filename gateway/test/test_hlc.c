/* The host-link client against the real CC1312 firmware: firmware/src's
 * coordinator and host link, built for the Mac with stub radio hooks, are the
 * peer. Requests go through coord_on_host_bytes; whatever the firmware writes
 * comes back through hlc_feed. */
#include <stdlib.h>

#include "coordinator.h"
#include "fw_peer.h"
#include "harness.h"
#include "hlc.h"
#include "lowpan.h"
#include "mac.h"
#include "session.h"
#include "coap.h"

/* --- the gateway side ------------------------------------------------------- */

static struct coordinator fw;
static struct hlc h;
static uint32_t now;

/* What the gateway wrote, delivered to the firmware unless a test drops it. */
static uint8_t gw_out[1024];
static size_t gw_out_len;
static bool drop_requests, drop_replies;

static void gw_write(void *ctx, const uint8_t *b, size_t n)
{
    (void)ctx;
    memcpy(&gw_out[gw_out_len], b, n);
    gw_out_len += n;
    if (!drop_requests)
        coord_on_host_bytes(&fw, b, n);
}

static struct {
    bool got;
    uint8_t type;
    int status;
    uint8_t data[64];
    size_t len;
} reply;

static void gw_on_reply(void *ctx, const struct hlc_reply *r)
{
    (void)ctx;
    reply.got = true;
    reply.type = r->type;
    reply.status = r->status;
    reply.len = r->len < sizeof reply.data ? r->len : sizeof reply.data;
    if (r->len)
        memcpy(reply.data, r->data, reply.len);
}

/* Events are copied out and handled after hlc_feed returns: a callback must
 * not feed or request again while the parser is running. */
#define MAX_EVENTS 8
static struct link_event events[MAX_EVENTS];
static uint8_t event_data[MAX_EVENTS][320];
static size_t n_events;

static void gw_on_event(void *ctx, const struct link_event *ev)
{
    (void)ctx;
    if (n_events == MAX_EVENTS)
        return;
    struct link_event *e = &events[n_events];
    *e = *ev;
    if (ev->type == LINK_EV_RX) {
        memcpy(event_data[n_events], ev->rx.packet, ev->rx.len);
        e->rx.packet = event_data[n_events];
    } else if (ev->type == LINK_EV_LOG) {
        memcpy(event_data[n_events], ev->log.text, ev->log.len);
        e->log.text = (const char *)event_data[n_events];
    }
    n_events++;
}

static const struct hlc_ops OPS = {.write = gw_write, .on_reply = gw_on_reply, .on_event = gw_on_event};

/* Delivers what the firmware wrote to the gateway. */
static void pump(void)
{
    uint8_t b[sizeof fw_out];
    size_t n = fw_out_len;
    memcpy(b, fw_out, n);
    fw_out_len = 0;
    if (!drop_replies)
        hlc_feed(&h, b, n);
}

static void reset(void)
{
    coord_init(&fw);
    hlc_init(&h, &OPS, 0x40);
    fw_ms = fw_rat = 0;
    now = 0;
    fw_out_len = gw_out_len = 0;
    drop_requests = drop_replies = false;
    reply.got = false;
    n_events = 0;
    jobs = 0;
}

static const struct link_event *find_event(enum link_event_type type)
{
    for (size_t i = 0; i < n_events; i++)
        if (events[i].type == type)
            return &events[i];
    return NULL;
}

static bool configure(void)
{
    reply.got = false;
    CHECK(hlc_config(&h, V("in.coord_eui")->data, V("in.network_key")->data, 0x100, 868335, now));
    pump();
    return reply.got && reply.type == HL_CONFIG && reply.status == HL_OK && !hlc_busy(&h);
}

/* --- tests ---------------------------------------------------------------------- */

static void ping_config_and_stats(void)
{
    reset();
    coord_boot(&fw, "pin");
    pump();
    const struct link_event *boot = find_event(LINK_EV_LOG);
    CHECK(boot && boot->log.len > 5 && memcmp(boot->log.text, "boot ", 5) == 0);

    CHECK(hlc_ping(&h, now));
    CHECK(hlc_busy(&h) && !hlc_stats(&h, now)); /* one request at a time */
    pump();
    uint16_t version;
    uint32_t rat;
    struct hlc_reply r = {.type = reply.type, .status = reply.status, .data = reply.data, .len = reply.len};
    CHECK(reply.got && hlc_ping_reply(&r, &version, &rat) && version == FW_VERSION);

    CHECK(configure());
    CHECK(fw.configured && memcmp(fw.network_key, V("in.network_key")->data, 16) == 0);

    reply.got = false;
    CHECK(hlc_stats(&h, now));
    pump();
    uint32_t counters[32];
    r = (struct hlc_reply){.type = reply.type, .status = reply.status, .data = reply.data, .len = reply.len};
    size_t n = hlc_stats_reply(&r, counters, 32);
    CHECK(n == 15 && counters[0] == 7 && counters[1] == 1);
    CHECK(hlc_stats_reply(&r, counters, 2) == 2);
}

/* A TX whose reply is lost goes again as the same bytes; the CC1312 answers
 * from its reply cache and queues the frame once. */
static void a_resent_request_is_the_same_frame_and_runs_once(void)
{
    reset();
    CHECK(configure());
    static const uint8_t packet[] = {0x7e, 0x33, 0xf0, 0x16, 0x33, 0x16, 0x33, 0x00, 0x00, 0x60, 0x44, 0x12, 0x34};
    struct link_tx tx = {.flags = LINK_TX_TRAIN_ANCHORED, .unicast = true, .packet = packet, .len = sizeof packet};
    memcpy(tx.dst, V("in.joiner_eui")->data, 8);

    gw_out_len = 0;
    reply.got = false;
    drop_replies = true;
    CHECK(hlc_tx(&h, &tx, now));
    pump();
    CHECK(fw.queue_len == 1 && !reply.got);
    uint8_t first[sizeof gw_out];
    size_t first_len = gw_out_len;
    memcpy(first, gw_out, first_len);

    now += HLC_REPLY_TIMEOUT_MS - 1;
    hlc_tick(&h, now);
    CHECK(gw_out_len == first_len); /* not yet */
    now += 1;
    drop_replies = false;
    hlc_tick(&h, now);
    CHECK(gw_out_len == 2 * first_len && memcmp(&gw_out[first_len], first, first_len) == 0);
    pump();
    CHECK(fw.queue_len == 1); /* replayed, not run again */
    uint8_t handle = 0, mac_seq = 0;
    uint32_t counter = 0;
    struct hlc_reply r = {.type = reply.type, .status = reply.status, .data = reply.data, .len = reply.len};
    CHECK(reply.got && hlc_tx_reply(&r, &handle, &mac_seq, &counter) && counter == 0x100);
    CHECK(handle == fw.queue[0].handle);

    /* The firmware sends it and reports: TX_DONE for that handle. */
    coord_poll(&fw);
    CHECK(jobs == 1);
    coord_on_job_sent(&fw, job_token, true);
    fw_ms += ACK_WAIT_MS;
    coord_poll(&fw); /* no ACK came */
    pump();
    const struct link_event *done = find_event(LINK_EV_TX_DONE);
    CHECK(done && done->tx_done.handle == handle && done->tx_done.result == LINK_NO_ACK);
}

static void gives_up_after_the_last_attempt(void)
{
    reset();
    drop_requests = true;
    CHECK(hlc_ping(&h, now));
    for (int i = 0; i < HLC_ATTEMPTS; i++) {
        now += HLC_REPLY_TIMEOUT_MS;
        hlc_tick(&h, now);
    }
    CHECK(gw_out_len == HLC_ATTEMPTS * 8u); /* a PING frame is 8 bytes, sent 4 times */
    CHECK(reply.got && reply.type == HL_PING && reply.status == -1 && !hlc_busy(&h));
    reply.got = false;
    now += HLC_REPLY_TIMEOUT_MS;
    hlc_tick(&h, now);
    CHECK(!reply.got);
}

static void finds_frames_again_after_noise_and_a_bad_crc(void)
{
    reset();
    coord_boot(&fw, "pin");
    uint8_t b[sizeof fw_out + 16];
    static const uint8_t noise[] = {0x00, 0x7e, 0x7e, 0x01, 0xa5};
    memcpy(b, noise, sizeof noise);
    memcpy(&b[sizeof noise], fw_out, fw_out_len);
    size_t n = sizeof noise + fw_out_len;
    /* The same LOG again with its CRC broken, then intact. */
    memcpy(&b[n], fw_out, fw_out_len);
    b[n + fw_out_len - 1] ^= 0xff;
    n += fw_out_len;
    memcpy(&b[n], fw_out, fw_out_len);
    n += fw_out_len;
    fw_out_len = 0;
    for (size_t i = 0; i < n; i++)
        hlc_feed(&h, &b[i], 1); /* a byte at a time, as a UART may deliver them */
    CHECK(n_events == 2 && events[0].type == LINK_EV_LOG && events[1].type == LINK_EV_LOG);
    CHECK(h.rx.stats.crc_errors == 1 && h.rx.stats.resyncs > 0);

    /* A torn start claiming a long body (the CC1312 reset mid-frame) does not
     * hold back the reply behind it. */
    CHECK(hlc_ping(&h, now));
    static const uint8_t torn[] = {0x7e, 0xa5, 0x2c, 0x01};
    hlc_feed(&h, torn, sizeof torn);
    reply.got = false;
    pump();
    CHECK(reply.got && reply.type == HL_PING && reply.status == HL_OK);
}

/* A device's report travels the whole chain: its sealed frame into the
 * firmware, the RX event to the gateway, the session's 2.04 back as a TX that
 * the firmware seals for the device. */
static struct session sess;

static int sess_tx(void *ctx, const struct link_tx *tx)
{
    (void)ctx;
    reply.got = false;
    if (!hlc_tx(&h, tx, now))
        return -1;
    pump(); /* the ESP waits for the reply here */
    uint8_t handle = 0, mac_seq = 0;
    uint32_t counter = 0;
    struct hlc_reply r = {.type = reply.type, .status = reply.status, .data = reply.data, .len = reply.len};
    return reply.got && hlc_tx_reply(&r, &handle, &mac_seq, &counter) ? handle : -1;
}

static void sess_log(void *ctx, const char *line)
{
    (void)ctx;
    if (getenv("VERBOSE"))
        printf("  [session] %s\n", line);
}

static void sess_rng(void *ctx, uint8_t *out, size_t n)
{
    (void)ctx;
    memset(out, 0x5a, n);
}

static uint32_t sess_unix(void *ctx)
{
    (void)ctx;
    return 1793966400u;
}

static uint16_t sess_setpoint(void *ctx, uint8_t zone)
{
    (void)ctx, (void)zone;
    return SESSION_DEFAULT_SETPOINT;
}

static void a_report_goes_end_to_end_through_the_firmware(void)
{
    reset();
    CHECK(configure());
    static const struct session_ops ops = {
        .tx = sess_tx, .log = sess_log, .rng = sess_rng, .unix_time = sess_unix, .setpoint = sess_setpoint};
    session_init(&sess, V("in.coord_eui")->data, V("in.network_key")->data, &ops, now);

    /* The device's PUT z/p, as it seals it. */
    const uint8_t *dev = V("in.joiner_eui")->data, *coord = V("in.coord_eui")->data;
    static const uint8_t body[] = {0x40, 0x60, 0x02, 0x07, 0x08};
    struct coap_msg put = {.type = COAP_CON, .code = COAP_PUT, .mid = 0xa339, .tkl = 1, .token = {7},
                           .payload = body, .payload_len = sizeof body};
    coap_add_path(&put, "z/p");
    uint8_t coap[64], packet[96], frame[MAC_MAX_FRAME];
    size_t cn = coap_encode(&put, coap, sizeof coap);
    size_t pn = lowpan_udp(packet, sizeof packet, dev, coord, 5683, 5683, coap, cn);
    size_t fn = mac_seal_unicast(frame, V("in.network_key")->data, false, 0x21, coord, dev, 0x500, packet, pn);
    CHECK(fn > 0);
    coord_on_rx(&fw, frame, fn, -48, 1000, 0);
    pump();

    const struct link_event *rx = find_event(LINK_EV_RX);
    CHECK(rx && !rx->rx.pairing_key && !rx->rx.broadcast && rx->rx.acked && rx->rx.rssi == -48);
    CHECK(rx && rx->rx.mac_seq == 0x21 && rx->rx.counter == 0x500 && memcmp(rx->rx.src, dev, 8) == 0);
    CHECK(rx && rx->rx.len == pn && memcmp(rx->rx.packet, packet, pn) == 0);

    /* Handled after the feed returned, as the ESP task will. */
    struct link_event ev = *rx;
    n_events = 0;
    session_on_event(&sess, &ev, now);
    CHECK(fw.queue_len == 1 && fw.queue[0].unicast);

    /* The firmware seals the 2.04 for the device: open it as the device would. */
    coord_poll(&fw);
    CHECK(jobs == 1);
    uint8_t scratch[MAC_MAX_FRAME];
    struct mac_data md;
    CHECK(mac_open_data(job_frame, job_len, V("in.network_key")->data, scratch, &md));
    struct lowpan_datagram d;
    struct coap_msg ans;
    CHECK(lowpan_parse(md.packet, md.packet_len, coord, dev, &d) && coap_decode(d.payload, d.payload_len, &ans));
    CHECK(ans.type == COAP_ACK && ans.code == COAP_CODE(2, 4) && ans.mid == 0xa339 && ans.tkl == 1 &&
          ans.token[0] == 7);
}

static void a_pair_window_and_its_beacon_answer(void)
{
    reset();
    CHECK(configure());
    reply.got = false;
    CHECK(hlc_pair_window(&h, 120, now));
    pump();
    CHECK(reply.got && reply.status == HL_OK);

    const struct vec *req = V("beacon_request.frame");
    coord_on_rx(&fw, req->data, req->len, -40, 2000, 0);
    pump();
    const struct link_event *b = find_event(LINK_EV_BEACON_ANSWERED);
    CHECK(b && memcmp(b->beacon.joiner, V("in.joiner_eui")->data, 8) == 0 && b->beacon.rssi == -40);
}

void run_hlc_tests(void)
{
    ping_config_and_stats();
    a_resent_request_is_the_same_frame_and_runs_once();
    gives_up_after_the_last_attempt();
    finds_frames_again_after_noise_and_a_bad_crc();
    a_report_goes_end_to_end_through_the_firmware();
    a_pair_window_and_its_beacon_answer();
}
