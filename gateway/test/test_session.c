/* The session, against the Java host's JoinSessionTest: the same scenarios,
 * one for one, on a stub CC1312 that records every frame and answers with
 * incrementing handles. */
#include <stdlib.h>

#include "coap.h"
#include "harness.h"
#include "lowpan.h"
#include "session.h"
#include "tado.h"

/* --- the stub CC1312 and clock --------------------------------------------- */

struct sent {
    struct link_tx tx;
    uint8_t packet[SESSION_MAX_PACKET];
};
static struct sent sent[64];
static size_t n_sent;
static uint32_t now;      /* ms */
static uint64_t rng_state;
static bool verbose;

static int refuse_next; /* how many of the next frames the CC1312 refuses */

static int stub_tx(void *ctx, const struct link_tx *tx)
{
    (void)ctx;
    if (refuse_next > 0) {
        refuse_next--;
        return -1;
    }
    if (n_sent == sizeof sent / sizeof sent[0])
        return -1;
    struct sent *s = &sent[n_sent++];
    s->tx = *tx;
    memcpy(s->packet, tx->packet, tx->len);
    s->tx.packet = s->packet;
    return (int)n_sent; /* handles 1, 2, ... as the Java stub's */
}

static void stub_log(void *ctx, const char *line)
{
    (void)ctx;
    if (verbose)
        printf("  [session] %s\n", line);
}

static void stub_rng(void *ctx, uint8_t *out, size_t n)
{
    (void)ctx;
    for (size_t i = 0; i < n; i++) {
        rng_state = rng_state * 6364136223846793005u + 1442695040888963407u;
        out[i] = (uint8_t)(rng_state >> 56);
    }
}

static bool clock_unset; /* before SNTP: the gateway's unix_time gives 0 */

/* 2026-11-06T12:00:00Z plus the session clock. */
static uint32_t stub_unix(void *ctx)
{
    (void)ctx;
    return clock_unset ? 0 : 1793966400u + now / 1000u;
}

/* Each zone's setpoint and away temperature (0: off in away), and the home
 * state, as the gateway keeps them. */
static uint16_t setpoints[256], away_temps[256];
static bool away;

static void stub_program(void *ctx, uint8_t zone, struct session_program *p)
{
    (void)ctx;
    p->away = away;
    p->on = !away || away_temps[zone] != 0;
    p->setpoint = away ? away_temps[zone] : setpoints[zone] ? setpoints[zone] : SESSION_DEFAULT_SETPOINT;
}

/* The last report handed to the gateway. */
static struct {
    int count;
    uint8_t device[8];
    char path[64];
    struct tado_report r;
} reported;

static void stub_on_report(void *ctx, const uint8_t device[8], const char *path, const struct tado_report *r)
{
    (void)ctx;
    reported.count++;
    memcpy(reported.device, device, 8);
    snprintf(reported.path, sizeof reported.path, "%s", path);
    reported.r = *r;
}

/* The last request the gateway was told of. */
static struct {
    int count;
    uint8_t code;
    char path[64];
    bool answered;
} requests;

static void stub_on_request(void *ctx, const uint8_t device[8], uint8_t code, const char *path, bool answered)
{
    (void)ctx, (void)device;
    requests.count++;
    requests.code = code;
    snprintf(requests.path, sizeof requests.path, "%s", path);
    requests.answered = answered;
}

/* What the gateway does with a new device: the zone its pair command named. */
static int joined_count;
static int join_into_zone = -1;

static void stub_on_joined(void *ctx, const uint8_t device[8])
{
    joined_count++;
    if (join_into_zone >= 0)
        CHECK(session_set_zone(ctx, (uint8_t)join_into_zone, device));
}

static const struct session_ops OPS = {.tx = stub_tx,
                                       .log = stub_log,
                                       .rng = stub_rng,
                                       .unix_time = stub_unix,
                                       .program = stub_program,
                                       .on_report = stub_on_report,
                                       .on_request = stub_on_request,
                                       .on_joined = stub_on_joined};

static const uint8_t *coordinator(void) { return V("in.coord_eui")->data; }
static const uint8_t *joiner(void) { return V("in.joiner_eui")->data; }

/* A session with its pair window open, as the Java test's JoinSession. */
static void fresh(struct session *s, bool pair_window)
{
    n_sent = 0;
    now = 0;
    rng_state = 1;
    refuse_next = 0;
    clock_unset = false;
    memset(&reported, 0, sizeof reported);
    memset(setpoints, 0, sizeof setpoints);
    memset(away_temps, 0, sizeof away_temps);
    away = false;
    memset(&requests, 0, sizeof requests);
    joined_count = 0;
    join_into_zone = -1;
    struct session_ops ops = OPS;
    ops.ctx = s;
    session_init(s, coordinator(), V("in.network_key")->data, &ops, now);
    if (pair_window)
        session_open_pair_window(s);
}

static void serving_zone11(struct session *s, uint32_t overlay_ms)
{
    fresh(s, false);
    session_set_home(s, 4242, overlay_ms);
    session_set_zone(s, 11, joiner());
}

/* --- events and frames ------------------------------------------------------ */

static uint8_t rx_packet[256];

static struct link_event rx_event(bool pairing_key, bool broadcast, const uint8_t src[8], const uint8_t *packet,
                                  size_t len)
{
    struct link_event ev = {.type = LINK_EV_RX};
    ev.rx.pairing_key = pairing_key;
    ev.rx.broadcast = broadcast;
    ev.rx.rssi = -50;
    memcpy(ev.rx.src, src, 8);
    memcpy(rx_packet, packet, len);
    ev.rx.packet = rx_packet;
    ev.rx.len = len;
    return ev;
}

/* IPHC: TF elided, NH inline (ICMPv6), hop limit 255, src from L2, dst ff02::2. */
static const uint8_t RS[] = {0x7b, 0x3b, 58, 0x02, 133, 0, 0, 0, 0, 0, 0, 0};

static void router_solicitation(struct session *s)
{
    struct link_event ev = rx_event(true, true, joiner(), RS, sizeof RS);
    session_on_event(s, &ev, now);
}

static void from_device(struct session *s, const uint8_t src[8], bool pairing_key, const struct coap_msg *m)
{
    uint8_t coap[256], packet[256];
    size_t n = coap_encode(m, coap, sizeof coap);
    size_t p = lowpan_udp(packet, sizeof packet, src, coordinator(), 5683, 5683, coap, n);
    struct link_event ev = rx_event(pairing_key, false, src, packet, p);
    session_on_event(s, &ev, now);
}

static void from_joiner(struct session *s, bool pairing_key, const struct coap_msg *m)
{
    from_device(s, joiner(), pairing_key, m);
}

static void tx_done(struct session *s, int handle, enum link_tx_result result)
{
    struct link_event ev = {.type = LINK_EV_TX_DONE};
    ev.tx_done.handle = (uint8_t)handle;
    ev.tx_done.result = result;
    session_on_event(s, &ev, now);
}

static void beacon_answered(struct session *s)
{
    struct link_event ev = {.type = LINK_EV_BEACON_ANSWERED};
    memcpy(ev.beacon.joiner, joiner(), 8);
    ev.beacon.request_counter = 16;
    ev.beacon.rssi = -40;
    ev.beacon.beacon_seq = 0x59;
    session_on_event(s, &ev, now);
}

static struct coap_msg msg(enum coap_type type, uint8_t code, uint16_t mid, const char *path)
{
    struct coap_msg m = {.type = type, .code = code, .mid = mid};
    if (path)
        coap_add_path(&m, path);
    return m;
}

static struct coap_msg ack(uint16_t mid, uint8_t code) { return msg(COAP_ACK, code, mid, NULL); }

/* The CoAP message of a sent frame; its options point into sent[i]. */
static struct coap_msg coap_of(size_t i)
{
    struct lowpan_datagram d;
    struct coap_msg m = {0};
    const struct sent *s = &sent[i];
    CHECK(lowpan_parse(s->packet, s->tx.len, coordinator(), s->tx.unicast ? s->tx.dst : NULL, &d) &&
          coap_decode(d.payload, d.payload_len, &m));
    return m;
}

static bool path_of(size_t i, const char *path)
{
    struct coap_msg m = coap_of(i);
    return coap_path_is(&m, path);
}

static bool same_packet(size_t a, size_t b)
{
    return sent[a].tx.len == sent[b].tx.len && memcmp(sent[a].packet, sent[b].packet, sent[a].tx.len) == 0;
}

/* What went to the device: not the periodic Router Advertisements a long wait
 * brings. Returns the index of the n-th unicast in sent[], or -1. */
static int unicast(size_t n)
{
    for (size_t i = 0; i < n_sent; i++)
        if (sent[i].tx.unicast && n-- == 0)
            return (int)i;
    return -1;
}

static size_t unicasts(void)
{
    size_t k = 0;
    for (size_t i = 0; i < n_sent; i++)
        k += sent[i].tx.unicast;
    return k;
}

/* --- the join ------------------------------------------------------------- */

static void walks_a_valve_from_beacon_to_joined(void)
{
    static struct session s;
    fresh(&s, true);
    beacon_answered(&s);
    router_solicitation(&s);

    /* The RA goes twice: network key with Frame Pending, then the pairing key;
     * both behind full trains. */
    CHECK(n_sent == 2);
    CHECK(sent[0].tx.flags == (LINK_TX_FRAME_PENDING | LINK_TX_TRAIN_FULL) && !sent[0].tx.unicast);
    CHECK(sent[1].tx.flags == (LINK_TX_PAIRING_KEY | LINK_TX_TRAIN_FULL) && !sent[1].tx.unicast);
    CHECK(same_packet(0, 1));

    tx_done(&s, 2, LINK_SENT);

    CHECK(n_sent == 3);
    CHECK(sent[2].tx.flags == (LINK_TX_PAIRING_KEY | LINK_TX_TRAIN_FULL) && sent[2].tx.unicast &&
          memcmp(sent[2].tx.dst, joiner(), 8) == 0);
    struct coap_msg m = coap_of(2);
    CHECK(coap_path_is(&m, "d/pair"));
    CHECK(m.payload_len == 18 && memcmp(&m.payload[2], V("in.network_key")->data, 16) == 0);
    CHECK(session_pair_state(&s) == SESSION_PAIR_PAIRING);

    tx_done(&s, 3, LINK_ACKED);
    struct coap_msg a = ack(m.mid, COAP_CODE(2, 4));
    from_joiner(&s, false, &a);
    CHECK(session_pair_state(&s) == SESSION_PAIR_JOINED);
}

static void a_solicitation_within_three_seconds_of_the_last_advertisement_gets_none(void)
{
    static struct session s;
    fresh(&s, true);
    router_solicitation(&s);
    router_solicitation(&s); /* while the first pair is still on air */
    CHECK(n_sent == 2);

    tx_done(&s, 2, LINK_SENT);
    size_t after_pair = n_sent; /* the RA ended, so d/pair followed */
    now += 2000;
    router_solicitation(&s);
    CHECK(n_sent == after_pair);

    now += 2000;
    router_solicitation(&s);
    CHECK(n_sent == after_pair + 2);
}

static void retries_d_pair_with_a_new_mid_and_gives_up_after_the_last_attempt(void)
{
    static struct session s;
    fresh(&s, true);
    beacon_answered(&s);
    router_solicitation(&s);
    tx_done(&s, 2, LINK_SENT);
    uint16_t mids[SESSION_PAIR_ATTEMPTS];
    for (int attempt = 0; attempt < SESSION_PAIR_ATTEMPTS; attempt++) {
        mids[attempt] = coap_of(n_sent - 1).mid;
        tx_done(&s, (int)n_sent, LINK_NO_ACK);
        now += SESSION_PAIR_ANSWER_TIMEOUT_MS;
        session_tick(&s, now);
    }
    CHECK(session_pair_state(&s) == SESSION_PAIR_FAILED);
    bool distinct = true;
    for (int i = 0; i < SESSION_PAIR_ATTEMPTS; i++)
        for (int j = i + 1; j < SESSION_PAIR_ATTEMPTS; j++)
            distinct &= mids[i] != mids[j];
    CHECK(distinct);
}

static void a_late_204_to_an_earlier_attempt_still_counts_as_joined(void)
{
    static struct session s;
    fresh(&s, true);
    beacon_answered(&s);
    router_solicitation(&s);
    tx_done(&s, 2, LINK_SENT);
    uint16_t first = coap_of(2).mid;
    tx_done(&s, 3, LINK_NO_ACK);
    now += SESSION_PAIR_ANSWER_TIMEOUT_MS;
    session_tick(&s, now); /* attempt 2 goes out with a new MID */
    CHECK(coap_of(3).mid != first);

    struct coap_msg a = ack(first, COAP_CODE(2, 4));
    from_joiner(&s, false, &a);
    CHECK(session_pair_state(&s) == SESSION_PAIR_JOINED);
}

static void lost_tx_done_events_do_not_stall_the_join(void)
{
    static struct session s;
    fresh(&s, true);
    beacon_answered(&s);
    router_solicitation(&s);
    CHECK(n_sent == 2);

    now += SESSION_TX_DONE_TIMEOUT_MS; /* the RA's TX_DONE never comes */
    session_tick(&s, now);
    CHECK(n_sent == 3); /* d/pair follows the RA anyway */

    now += SESSION_TX_DONE_TIMEOUT_MS; /* nor does d/pair's */
    session_tick(&s, now);
    now += SESSION_PAIR_ANSWER_TIMEOUT_MS;
    session_tick(&s, now);
    CHECK(n_sent == 4); /* d/pair is retried */
}

/* The CC1312 refusing d/pair (its queue full) must not leave the window stuck:
 * the attempt times out like a lost answer, and the next one goes. */
static void a_refused_d_pair_is_tried_again(void)
{
    static struct session s;
    fresh(&s, true);
    beacon_answered(&s);
    router_solicitation(&s);
    refuse_next = 1;
    tx_done(&s, 2, LINK_SENT); /* the RA ended: d/pair, refused */
    CHECK(n_sent == 2 && session_pair_state(&s) == SESSION_PAIR_PAIRING);
    now += SESSION_PAIR_ANSWER_TIMEOUT_MS;
    session_tick(&s, now);
    CHECK(n_sent == 3 && path_of(2, "d/pair"));
}

/* A refused Router Advertisement is as good as sent: d/pair still follows,
 * and the next solicitation within 3 s gets none. */
static void a_refused_advertisement_still_leads_to_d_pair(void)
{
    static struct session s;
    fresh(&s, true);
    beacon_answered(&s);
    refuse_next = 2; /* both copies of the pair-window RA */
    router_solicitation(&s);
    CHECK(n_sent == 1 && path_of(0, "d/pair"));
    now += 1000;
    router_solicitation(&s);
    CHECK(n_sent == 1);
}

static void an_empty_ack_then_a_separate_204_joins(void)
{
    static struct session s;
    fresh(&s, true);
    beacon_answered(&s);
    router_solicitation(&s);
    tx_done(&s, 2, LINK_SENT);
    uint16_t mid = coap_of(2).mid;

    struct coap_msg empty = ack(mid, 0);
    from_joiner(&s, false, &empty);
    CHECK(session_pair_state(&s) == SESSION_PAIR_PAIRING);

    struct coap_msg separate = msg(COAP_CON, COAP_CODE(2, 4), 0x7777, NULL);
    from_joiner(&s, false, &separate);
    CHECK(session_pair_state(&s) == SESSION_PAIR_JOINED);
    struct coap_msg confirm = coap_of(3);
    CHECK(confirm.type == COAP_ACK && confirm.code == 0 && confirm.mid == 0x7777);
}

static void a_joiner_learned_within_the_advertisement_gap_still_gets_d_pair(void)
{
    static struct session s;
    fresh(&s, true);
    struct link_event other = rx_event(false, true, coordinator(), RS, sizeof RS); /* RS from another node */
    session_on_event(&s, &other, now);
    tx_done(&s, 2, LINK_SENT);
    now += 1000;

    router_solicitation(&s); /* the joiner, under the pairing key, 1 s after the RA */
    CHECK(n_sent == 3 && path_of(2, "d/pair"));
}

static void an_answer_under_the_pairing_key_means_the_valve_did_not_adopt_our_key(void)
{
    static struct session s;
    fresh(&s, true);
    beacon_answered(&s);
    router_solicitation(&s);
    tx_done(&s, 2, LINK_SENT);
    struct coap_msg a = ack(coap_of(2).mid, COAP_CODE(2, 4));
    from_joiner(&s, true, &a);
    CHECK(session_pair_state(&s) == SESSION_PAIR_FAILED);
}

static void ignores_requests_under_the_pairing_key(void)
{
    static struct session s;
    fresh(&s, true);
    struct coap_msg time = msg(COAP_CON, COAP_GET, 0x0102, "time");
    from_joiner(&s, true, &time);
    CHECK(n_sent == 0 && session_pair_state(&s) != SESSION_PAIR_FAILED);
}

/* --- serving ----------------------------------------------------------------- */

static void serves_a_session_and_the_time_after_the_join(void)
{
    static struct session s;
    fresh(&s, true);
    struct coap_msg auth = msg(COAP_CON, COAP_POST, 0x0101, "auth/token");
    auth.tkl = 1;
    auth.token[0] = 1;
    from_joiner(&s, false, &auth);

    /* Replies go on the device's next channel sample, not behind a 1 s train. */
    CHECK(sent[0].tx.flags == LINK_TX_TRAIN_ANCHORED);
    struct coap_msg a = coap_of(0);
    uint8_t id[8];
    session_id(joiner(), id);
    CHECK(a.type == COAP_ACK && a.mid == 0x0101 && a.payload_len >= 11);
    CHECK(a.payload[0] == 0x02 && a.payload[1] == 0x5e && memcmp(&a.payload[3], id, 8) == 0);
    CHECK_VEC(id, 8, "doc.session_id");

    now = 5000;
    struct coap_msg time = msg(COAP_CON, COAP_GET, 0x0102, "time");
    time.tkl = 1;
    time.token[0] = 2;
    from_joiner(&s, false, &time);
    struct coap_msg t = coap_of(1);
    uint32_t seconds = (uint32_t)t.payload[1] | (uint32_t)t.payload[2] << 8 | (uint32_t)t.payload[3] << 16 |
                       (uint32_t)t.payload[4] << 24;
    CHECK(t.payload_len == 5 && seconds == stub_unix(NULL));
}

static void serving_sends_an_unsolicited_router_advertisement_every_200_to_600_seconds(void)
{
    static struct session s;
    fresh(&s, false);
    now += SESSION_RA_INTERVAL_MIN_MS - 1000;
    session_tick(&s, now);
    CHECK(n_sent == 0);

    now += SESSION_RA_INTERVAL_MAX_MS - SESSION_RA_INTERVAL_MIN_MS + 1000;
    session_tick(&s, now);
    /* One copy under the network key: with no pair window there is no pairing-key copy. */
    CHECK(n_sent == 1 && sent[0].tx.flags == LINK_TX_TRAIN_FULL && !sent[0].tx.unicast);
    struct lowpan_datagram d;
    CHECK(lowpan_parse(sent[0].packet, sent[0].tx.len, coordinator(), NULL, &d) && lowpan_icmp_type(&d) == 134);

    tx_done(&s, 1, LINK_SENT);
    now += SESSION_RA_INTERVAL_MIN_MS - 1000;
    session_tick(&s, now);
    CHECK(n_sent == 1);
    now += SESSION_RA_INTERVAL_MAX_MS - SESSION_RA_INTERVAL_MIN_MS + 1000;
    session_tick(&s, now);
    CHECK(n_sent == 2);
}

static void serving_answers_a_solicitation_under_the_network_key_only(void)
{
    static struct session s;
    fresh(&s, false);
    struct link_event ev = rx_event(false, true, joiner(), RS, sizeof RS);
    session_on_event(&s, &ev, now);
    CHECK(n_sent == 1 && sent[0].tx.flags == LINK_TX_TRAIN_FULL);
}

static void answers_a_repeat_again_when_the_tx_done_of_the_first_answer_was_lost(void)
{
    static struct session s;
    fresh(&s, true);
    static const uint8_t body[] = {0x40, 0x60};
    struct coap_msg put = msg(COAP_CON, COAP_PUT, 0xa340, "z/p");
    put.payload = body;
    put.payload_len = sizeof body;
    from_joiner(&s, false, &put); /* its TX_DONE never comes */
    now += SESSION_TX_DONE_TIMEOUT_MS;
    from_joiner(&s, false, &put);
    CHECK(n_sent == 2 && same_packet(0, 1));
}

static void answers_a_repeated_request_once_while_its_answer_is_on_its_way(void)
{
    static struct session s;
    fresh(&s, true);
    static const uint8_t body[] = {0x40, 0x60};
    struct coap_msg put = msg(COAP_CON, COAP_PUT, 0xa339, "z/p");
    put.payload = body;
    put.payload_len = sizeof body;
    from_joiner(&s, false, &put);
    from_joiner(&s, false, &put); /* the device repeats before our answer lands */
    CHECK(n_sent == 1);

    tx_done(&s, 1, LINK_NO_ACK);
    from_joiner(&s, false, &put); /* it repeats again: the same answer goes again */
    CHECK(n_sent == 2 && same_packet(0, 1));

    struct coap_msg next = put;
    next.mid = 0xa33a;
    from_joiner(&s, false, &next); /* a new MID is a new request */
    CHECK(n_sent == 3 && coap_of(2).mid == 0xa33a);
}

static struct coap_msg get(uint16_t mid, const char *path)
{
    static const uint8_t block = 0x03; /* Block2 0/0/128 */
    struct coap_msg m = msg(COAP_CON, COAP_GET, mid, path);
    coap_add_option(&m, COAP_OPT_BLOCK2, &block, 1);
    return m;
}

static void serves_the_installations_devices_their_config_and_zone_document(void)
{
    static struct session s;
    serving_zone11(&s, SESSION_DEFAULT_OVERLAY_MS);
    struct coap_msg m = get(0x0201, "d/VA0000000001/config");
    from_joiner(&s, false, &m);
    struct coap_msg c = coap_of(0);
    uint8_t want[64];
    static const struct tado_zone_role owner11 = {TADO_ROLE_OWNER, 11};
    size_t wn = tado_device_config(want, sizeof want, 4242, &owner11, 1);
    CHECK(c.code == COAP_CODE(2, 5));
    CHECK_BYTES(c.payload, c.payload_len, want, wn);

    m = get(0x0202, "h/4242/z/11/config");
    from_joiner(&s, false, &m);
    c = coap_of(1);
    CHECK(c.code == COAP_CODE(2, 5) && c.payload_len == 64); /* block 0 of a longer document */

    m = get(0x0205, "h/4242/z/11/s");
    from_joiner(&s, false, &m);
    c = coap_of(2);
    wn = tado_zone_state(want, sizeof want, SESSION_DEFAULT_SETPOINT, -1);
    CHECK_BYTES(c.payload, c.payload_len, want, wn);

    m = get(0x0203, "h/4242/z/12/config");
    from_joiner(&s, false, &m);
    m = get(0x0204, "h/1/z/11/config");
    from_joiner(&s, false, &m);
    CHECK(n_sent == 3); /* no zone 12, no home 1: not ours to answer */
}

static void answers_a_fragmented_report_with_204(void)
{
    static struct session s;
    fresh(&s, true);
    static const uint8_t body[40] = {0};
    struct coap_msg put = msg(COAP_CON, COAP_PUT, 0xa302, "d/SU0000000001/fw/state");
    put.tkl = 1;
    put.token[0] = 7;
    put.payload = body;
    put.payload_len = sizeof body;
    uint8_t coap[128], packet[160];
    size_t cn = coap_encode(&put, coap, sizeof coap);
    size_t pn = lowpan_udp(packet, sizeof packet, joiner(), coordinator(), 5683, 5683, coap, cn);
    /* RFC 4944: FRAG1 holds the 9 compressed header bytes (48 uncompressed) and
     * 16 CoAP bytes, so the FRAGN starts at offset 64 (8 x 8). */
    size_t size = 48 + pn - 9;
    uint8_t f1[4 + 25], fn[5 + 160];
    f1[0] = (uint8_t)(0xc0 | size >> 8);
    f1[1] = (uint8_t)size;
    f1[2] = 0;
    f1[3] = 5;
    memcpy(&f1[4], packet, 25);
    fn[0] = (uint8_t)(0xe0 | size >> 8);
    fn[1] = (uint8_t)size;
    fn[2] = 0;
    fn[3] = 5;
    fn[4] = 8;
    memcpy(&fn[5], &packet[25], pn - 25);

    struct link_event e1 = rx_event(false, false, joiner(), f1, sizeof f1);
    session_on_event(&s, &e1, now);
    CHECK(n_sent == 0);
    struct link_event e2 = rx_event(false, false, joiner(), fn, 5 + pn - 25);
    session_on_event(&s, &e2, now);

    struct coap_msg a = coap_of(0);
    CHECK(a.type == COAP_ACK && a.code == COAP_CODE(2, 4) && a.mid == 0xa302 && a.tkl == 1 && a.token[0] == 7);
}

/* One session serves and pairs at once: a pair window on top of a served
 * installation leaves the owner served, and the joiner's own reports answered. */
static void a_pair_window_does_not_interrupt_serving(void)
{
    static struct session s;
    serving_zone11(&s, SESSION_DEFAULT_OVERLAY_MS);
    uint8_t newcomer[8];
    memcpy(newcomer, joiner(), 8);
    newcomer[0] ^= 2;
    session_open_pair_window(&s);
    struct link_event b = {.type = LINK_EV_BEACON_ANSWERED};
    memcpy(b.beacon.joiner, newcomer, 8);
    session_on_event(&s, &b, now);
    struct link_event rs = rx_event(true, true, newcomer, RS, sizeof RS);
    session_on_event(&s, &rs, now);
    tx_done(&s, 2, LINK_SENT); /* the RA ended: d/pair to the newcomer */
    CHECK(session_pair_state(&s) == SESSION_PAIR_PAIRING && path_of(2, "d/pair"));

    /* The owner of zone 11 reads its state as usual, on its own sample. */
    struct coap_msg m = get(0x0401, "h/4242/z/11/s");
    from_joiner(&s, false, &m);
    CHECK(n_sent == 4 && sent[3].tx.flags == LINK_TX_TRAIN_ANCHORED && coap_of(3).code == COAP_CODE(2, 5));

    /* A report from the newcomer under the network key while d/pair waits: a
     * request with a token, not a pair answer. It gets its 2.04. */
    static const uint8_t body[] = {0x40, 0x60, 0x02, 0x07, 0x08};
    struct coap_msg report = msg(COAP_CON, COAP_PUT, 0x0402, "z/p");
    report.tkl = 1;
    report.token[0] = 9;
    report.payload = body;
    report.payload_len = sizeof body;
    from_device(&s, newcomer, false, &report);
    CHECK(n_sent == 5 && coap_of(4).code == COAP_CODE(2, 4) && coap_of(4).mid == 0x0402);
    CHECK(session_pair_state(&s) == SESSION_PAIR_PAIRING);
}

/* --- device overlays -------------------------------------------------------- */

/* The device's report of a setpoint set on it, as the sensor sent it. */
static struct coap_msg overlay_put(uint16_t mid, const char *zone_path, const uint8_t *body, size_t len)
{
    struct coap_msg m = msg(COAP_CON, COAP_PUT, mid, zone_path);
    m.payload = body;
    m.payload_len = len;
    return m;
}

static const uint8_t OVERLAY_21C[] = {0x62, 0x80, 0x02, 0x08, 0x34, 0x62, 0x40, 0x01, 0x02, 0x62, 0x60, 0x01, 0x01};
static const uint8_t OVERLAY_ENDED[] = {0x62, 0x40, 0x01, 0x00, 0x62, 0x60, 0x01, 0x01};

static void keeps_a_setpoint_set_on_the_device_for_the_overlay_time_then_writes_the_zone_back(void)
{
    static struct session s;
    serving_zone11(&s, 3600000u);
    struct coap_msg m = overlay_put(0x0301, "h/4242/z/11/overlay", OVERLAY_21C, sizeof OVERLAY_21C);
    from_joiner(&s, false, &m);
    CHECK(coap_of((size_t)unicast(0)).code == COAP_CODE(2, 4));

    now += 59u * 60000u;
    m = get(0x0302, "h/4242/z/11/s");
    from_joiner(&s, false, &m);
    uint8_t want[64];
    size_t wn = tado_zone_state(want, sizeof want, 1800, 2100);
    struct coap_msg st = coap_of((size_t)unicast(1));
    CHECK_BYTES(st.payload, st.payload_len, want, wn);

    now += 60000u;
    session_tick(&s, now);
    CHECK(unicasts() == 3);
    int w = unicast(2);
    CHECK(sent[w].tx.flags == LINK_TX_TRAIN_ANCHORED);
    struct coap_msg write = coap_of((size_t)w);
    CHECK(write.type == COAP_CON && write.code == COAP_PUT && coap_path_is(&write, "z/s"));
    const struct coap_option *q = coap_find(&write, COAP_OPT_URI_QUERY);
    CHECK(q && q->len == 5 && memcmp(q->value, "id=11", 5) == 0);
    wn = tado_zone_state(want, sizeof want, 1800, -1); /* 6240=0, no 6440 */
    CHECK_BYTES(write.payload, write.payload_len, want, wn);

    m = get(0x0303, "h/4242/z/11/s");
    from_joiner(&s, false, &m);
    st = coap_of((size_t)unicast(3));
    CHECK_BYTES(st.payload, st.payload_len, want, wn);

    struct coap_msg a = ack(write.mid, COAP_CODE(2, 4));
    from_joiner(&s, false, &a);
    now += 60000u;
    session_tick(&s, now);
    CHECK(unicasts() == 4); /* acknowledged: not again */
}

static void repeats_an_unacknowledged_zone_write_under_its_mid_then_gives_up(void)
{
    static struct session s;
    serving_zone11(&s, 60000u);
    struct coap_msg m = overlay_put(0x0311, "h/4242/z/11/overlay", OVERLAY_21C, sizeof OVERLAY_21C);
    from_joiner(&s, false, &m);
    now += 60000u;
    session_tick(&s, now);
    uint16_t mid = coap_of((size_t)unicast(1)).mid;
    for (int i = 0; i < 10; i++) {
        now += SESSION_ZONE_WRITE_TIMEOUT_MS;
        session_tick(&s, now);
    }
    CHECK(unicasts() == 1 + SESSION_ZONE_WRITE_ATTEMPTS);
    for (size_t i = 1; i < unicasts(); i++)
        CHECK(coap_of((size_t)unicast(i)).mid == mid);
}

static void an_overlay_the_device_ends_or_that_is_not_its_zone_leaves_nothing_to_write(void)
{
    static struct session s;
    serving_zone11(&s, 60000u);
    struct coap_msg m = overlay_put(0x0321, "h/4242/z/11/overlay", OVERLAY_21C, sizeof OVERLAY_21C);
    from_joiner(&s, false, &m);
    m = overlay_put(0x0322, "h/4242/z/11/overlay", OVERLAY_ENDED, sizeof OVERLAY_ENDED); /* ended on the device */
    from_joiner(&s, false, &m);
    m = overlay_put(0x0323, "h/4242/z/12/overlay", OVERLAY_21C, sizeof OVERLAY_21C); /* not the joiner's zone */
    from_joiner(&s, false, &m);
    uint8_t other[8];
    memcpy(other, joiner(), 8);
    other[0] ^= 1;
    m = overlay_put(0x0324, "h/4242/z/11/overlay", OVERLAY_21C, sizeof OVERLAY_21C); /* zone 11, not its owner */
    from_device(&s, other, false, &m);
    now += 120000u;
    session_tick(&s, now);
    CHECK(unicasts() == 4); /* four 2.04s, no write */

    m = get(0x0325, "h/4242/z/11/s");
    from_joiner(&s, false, &m);
    uint8_t want[64];
    size_t wn = tado_zone_state(want, sizeof want, 1800, -1);
    struct coap_msg st = coap_of((size_t)unicast(4));
    CHECK_BYTES(st.payload, st.payload_len, want, wn);
}

static void an_overlay_time_of_zero_keeps_the_devices_setpoint_until_changed(void)
{
    static struct session s;
    serving_zone11(&s, 0);
    struct coap_msg m = overlay_put(0x0331, "h/4242/z/11/overlay", OVERLAY_21C, sizeof OVERLAY_21C);
    from_joiner(&s, false, &m);
    for (int day = 0; day < 30; day++) { /* well past 2^31 ms */
        now += 86400000u;
        session_tick(&s, now);
    }
    m = get(0x0332, "h/4242/z/11/s");
    from_joiner(&s, false, &m);
    CHECK(unicasts() == 2);
    uint8_t want[64];
    size_t wn = tado_zone_state(want, sizeof want, 1800, 2100);
    struct coap_msg st = coap_of((size_t)unicast(1));
    CHECK_BYTES(st.payload, st.payload_len, want, wn);
}

/* An overlay time of 2^31 ms or more (25 days) is a time like any other. */
static void a_thirty_day_overlay_lasts_thirty_days(void)
{
    static struct session s;
    serving_zone11(&s, 30u * 86400000u);
    struct coap_msg m = overlay_put(0x0341, "h/4242/z/11/overlay", OVERLAY_21C, sizeof OVERLAY_21C);
    from_joiner(&s, false, &m);
    for (int day = 0; day < 29; day++) {
        now += 86400000u;
        session_tick(&s, now);
    }
    CHECK(unicasts() == 1); /* still running: no write */
    now += 86400000u;
    session_tick(&s, now);
    CHECK(unicasts() == 2 && path_of((size_t)unicast(1), "z/s"));
}

/* The gateway's view of the join: told once, it gives the device the zone the
 * pair command named, and the device's config request is then answered. */
static void a_joined_device_gets_the_zone_the_gateway_gives_it_and_its_config(void)
{
    static struct session s;
    fresh(&s, true);
    session_set_home(&s, 4242, SESSION_DEFAULT_OVERLAY_MS);
    join_into_zone = 3;
    beacon_answered(&s);
    router_solicitation(&s);
    tx_done(&s, 2, LINK_SENT);
    tx_done(&s, 3, LINK_ACKED);
    struct coap_msg a = ack(coap_of(2).mid, COAP_CODE(2, 4));
    from_joiner(&s, false, &a);
    from_joiner(&s, false, &a); /* a repeated 2.04 is not a second join */
    CHECK(session_pair_state(&s) == SESSION_PAIR_JOINED && joined_count == 1);

    struct coap_msg m = get(0x0301, "d/VA0000000001/config");
    from_joiner(&s, false, &m);
    struct coap_msg c = coap_of(n_sent - 1);
    static const struct tado_zone_role owner3 = {TADO_ROLE_OWNER, 3};
    uint8_t want[64];
    size_t wn = tado_device_config(want, sizeof want, 4242, &owner3, 1);
    CHECK(c.code == COAP_CODE(2, 5) && c.mid == 0x0301);
    CHECK_BYTES(c.payload, c.payload_len, want, wn);
}

/* Without a zone a joined device's config request goes unanswered: the
 * gateway must give it one. */
static void a_device_without_a_zone_gets_no_config(void)
{
    static struct session s;
    fresh(&s, false);
    session_set_home(&s, 4242, SESSION_DEFAULT_OVERLAY_MS);
    struct coap_msg m = get(0x0302, "d/VA0000000001/config");
    from_joiner(&s, false, &m);
    CHECK(n_sent == 0);
    /* The gateway hears of it: the device waits for a room. */
    CHECK(requests.count == 1 && requests.code == COAP_GET && !requests.answered);
    CHECK(strcmp(requests.path, "d/VA0000000001/config") == 0);

    session_set_zone(&s, 2, joiner());
    m = get(0x0303, "d/VA0000000001/config");
    from_joiner(&s, false, &m);
    CHECK(n_sent == 1 && requests.count == 2 && requests.answered);

    /* Repeats and requests under the pairing key are not new requests. */
    from_joiner(&s, false, &m);
    m = get(0x0304, "d/VA0000000001/config");
    from_joiner(&s, true, &m);
    CHECK(requests.count == 2);
}

/* The sensor's report as it sent it (docs/SENSOR.md): handed to the gateway
 * decoded, with its device and path, and answered 2.04. */
static void hands_a_decoded_report_to_the_gateway(void)
{
    static struct session s;
    serving_zone11(&s, SESSION_DEFAULT_OVERLAY_MS);
    static const uint8_t sen[] = {0x01, 0x62, 0x02, 0x10, 0x4f, 0x01, 0x2d, 0x02, 0x07, 0x4a, 0x01, 0x2e, 0x02,
                                  0x07, 0x4b, 0x01, 0x35, 0x02, 0x02, 0x35, 0x01, 0x36, 0x01, 0x04};
    struct coap_msg put = msg(COAP_CON, COAP_PUT, 0x0501, "d/SU2922080256/sen");
    put.payload = sen;
    put.payload_len = sizeof sen;
    from_joiner(&s, false, &put);
    CHECK(reported.count == 1 && memcmp(reported.device, joiner(), 8) == 0);
    CHECK(strcmp(reported.path, "d/SU2922080256/sen") == 0);
    CHECK(reported.r.fields == (TADO_BATTERY | TADO_TEMPERATURE | TADO_TEMPERATURE_RAW | TADO_HUMIDITY | TADO_LIGHT));
    CHECK(reported.r.battery_mv == 4175 && reported.r.temperature == 1866 && reported.r.temperature_raw == 1867);
    CHECK(reported.r.humidity == 565 && reported.r.light == 4);
    CHECK(coap_of(0).code == COAP_CODE(2, 4));

    /* A malformed body is still answered (or the device retries), not reported. */
    static const uint8_t torn[] = {0x01, 0x2d, 0x02, 0x07};
    put = msg(COAP_CON, COAP_PUT, 0x0502, "d/SU2922080256/sen");
    put.payload = torn;
    put.payload_len = sizeof torn;
    from_joiner(&s, false, &put);
    CHECK(reported.count == 1 && n_sent == 2 && coap_of(1).code == COAP_CODE(2, 4));

    /* A repeat of an answered report is answered again, not reported twice. */
    put = msg(COAP_CON, COAP_PUT, 0x0501, "d/SU2922080256/sen");
    put.payload = sen;
    put.payload_len = sizeof sen;
    from_joiner(&s, false, &put);
    CHECK(reported.count == 1);
}

/* Before SNTP the clock says 1970: the device would take it, so the request
 * goes unanswered and the device asks again. */
static void leaves_time_unanswered_until_the_clock_is_set(void)
{
    static struct session s;
    fresh(&s, false);
    clock_unset = true;
    struct coap_msg time = msg(COAP_CON, COAP_GET, 0x0601, "time");
    from_joiner(&s, false, &time);
    CHECK(n_sent == 0);

    clock_unset = false;
    time.mid = 0x0602;
    from_joiner(&s, false, &time);
    CHECK(n_sent == 1 && coap_of(0).code == COAP_CODE(2, 5) && coap_of(0).payload_len == 5);
}

/* A setpoint from Home Assistant: the zone state goes to the owner on its
 * sample, ends an overlay set on the device, and is what the zone serves
 * from then on; a 2.05 ACK (a VA01 answers so, freedo) ends it as a 2.04. */
static void writes_a_new_setpoint_to_the_owner(void)
{
    static struct session s;
    serving_zone11(&s, 3600000u);
    struct coap_msg m = overlay_put(0x0601, "h/4242/z/11/overlay", OVERLAY_21C, sizeof OVERLAY_21C);
    from_joiner(&s, false, &m);
    CHECK(!session_write_zone(&s, 12, now)); /* not served */

    setpoints[11] = 2150;
    CHECK(session_write_zone(&s, 11, now));
    int w = unicast(1);
    CHECK(w >= 0 && sent[w].tx.flags == LINK_TX_TRAIN_ANCHORED);
    struct coap_msg write = coap_of((size_t)w);
    CHECK(write.type == COAP_CON && write.code == COAP_PUT && coap_path_is(&write, "z/s"));
    /* It ends the device's overlay: a cancel, with 6440. */
    uint8_t want[64];
    size_t wn = tado_zone_block(want, sizeof want, &(struct tado_zone_block){.on = true, .setpoint = 2150, .marker = true});
    CHECK_BYTES(write.payload, write.payload_len, want, wn);

    /* The zone serves it, without the device's overlay. */
    m = get(0x0602, "h/4242/z/11/s");
    from_joiner(&s, false, &m);
    struct coap_msg st = coap_of((size_t)unicast(2));
    wn = tado_zone_state(want, sizeof want, 2150, -1);
    CHECK_BYTES(st.payload, st.payload_len, want, wn);

    struct coap_msg a = ack(write.mid, COAP_CODE(2, 5));
    from_joiner(&s, false, &a);
    now += SESSION_ZONE_WRITE_TIMEOUT_MS;
    session_tick(&s, now);
    CHECK(unicasts() == 3); /* acknowledged: not again */

    /* A burst of changes while the first frame waits for the device's sample:
     * nothing more is queued; when that frame left, the newest goes, once. */
    setpoints[11] = 1900;
    session_write_zone(&s, 11, now);
    int first = unicast(3);
    setpoints[11] = 1950;
    session_write_zone(&s, 11, now);
    setpoints[11] = 2000;
    session_write_zone(&s, 11, now);
    CHECK(unicasts() == 4);
    tx_done(&s, first + 1, LINK_ACKED); /* handles are index + 1 */
    CHECK(unicasts() == 5);
    struct coap_msg w1 = coap_of((size_t)first), w2 = coap_of((size_t)unicast(4));
    wn = tado_zone_state(want, sizeof want, 2000, -1);
    CHECK(w1.mid != w2.mid);
    CHECK_BYTES(w2.payload, w2.payload_len, want, wn);
    struct coap_msg a2 = ack(w2.mid, COAP_CODE(2, 4));
    from_joiner(&s, false, &a2);
    now += SESSION_ZONE_WRITE_TIMEOUT_MS;
    session_tick(&s, now);
    CHECK(unicasts() == 5); /* the newest was ACKed; the older MID is not repeated */

    /* A lost TX_DONE does not hold the newest back for good. */
    setpoints[11] = 2100;
    session_write_zone(&s, 11, now);
    setpoints[11] = 2200;
    session_write_zone(&s, 11, now);
    CHECK(unicasts() == 6);
    now += SESSION_TX_DONE_TIMEOUT_MS;
    session_tick(&s, now);
    CHECK(unicasts() == 7);
    wn = tado_zone_state(want, sizeof want, 2200, -1);
    struct coap_msg w3 = coap_of((size_t)unicast(6));
    CHECK_BYTES(w3.payload, w3.payload_len, want, wn);
}

/* The block of the last write to the device, against what it should be. */
static void last_write_is(const struct tado_zone_block *b)
{
    int w = unicast(unicasts() - 1);
    CHECK(w >= 0);
    struct coap_msg m = coap_of((size_t)w);
    CHECK(m.code == COAP_PUT && coap_path_is(&m, "z/s"));
    uint8_t want[64];
    size_t wn = tado_zone_block(want, sizeof want, b);
    CHECK_BYTES(m.payload, m.payload_len, want, wn);
}

/* Boost: a timer overlay at 25 C, served while it runs, and the return to the
 * program at its end (no marker) or at a cancel (marker). */
static void a_boost_runs_its_time_or_until_cancelled(void)
{
    static struct session s;
    serving_zone11(&s, 3600000u);
    CHECK(!session_end_overlay(&s, 11, now)); /* none runs */
    CHECK(session_boost(&s, 11, 2500, 1800000u, now));
    last_write_is(&(struct tado_zone_block){.on = true, .setpoint = 1800, .overlay = TADO_OVERLAY_TIMER,
                                            .overlay_on = true, .overlay_setpoint = 2500, .marker = true});
    struct coap_msg m = get(0x0701, "h/4242/z/11/s");
    from_joiner(&s, false, &m);
    struct coap_msg st = coap_of((size_t)unicast(unicasts() - 1));
    uint8_t want[64];
    size_t wn = tado_zone_block(want, sizeof want, &(struct tado_zone_block){
        .on = true, .setpoint = 1800, .overlay = TADO_OVERLAY_TIMER, .overlay_on = true, .overlay_setpoint = 2500});
    CHECK_BYTES(st.payload, st.payload_len, want, wn);

    size_t before = unicasts();
    tx_done(&s, (int)n_sent, LINK_ACKED);
    struct coap_msg a = ack(coap_of((size_t)unicast(0)).mid, COAP_CODE(2, 4));
    from_joiner(&s, false, &a);
    now += 1800000u - 1;
    session_tick(&s, now);
    CHECK(unicasts() == before); /* not yet */
    now += 1;
    session_tick(&s, now);
    last_write_is(&(struct tado_zone_block){.on = true, .setpoint = 1800}); /* expiry: no 6440 */

    /* Cancelled before its end. */
    serving_zone11(&s, 3600000u);
    CHECK(session_boost(&s, 11, 2500, 1800000u, now));
    tx_done(&s, (int)n_sent, LINK_ACKED);
    CHECK(session_end_overlay(&s, 11, now));
    last_write_is(&(struct tado_zone_block){.on = true, .setpoint = 1800, .marker = true});
}

/* Home and away: each zone is restated with the program in force. A target
 * that moved is marked; a running overlay is kept, unmarked. */
static void the_switch_to_away_restates_each_zone(void)
{
    static struct session s;
    serving_zone11(&s, 3600000u);
    away = true;
    away_temps[11] = 1500;
    CHECK(session_restate_zone(&s, 11, true, now));
    last_write_is(&(struct tado_zone_block){.away = true, .on = true, .setpoint = 1500, .marker = true});
    tx_done(&s, (int)n_sent, LINK_ACKED);

    away_temps[11] = 0; /* off in away */
    CHECK(session_restate_zone(&s, 11, true, now));
    last_write_is(&(struct tado_zone_block){.away = true, .marker = true});
    tx_done(&s, (int)n_sent, LINK_ACKED);

    /* A setpoint set on the device survives the switch home. */
    struct coap_msg m = overlay_put(0x0801, "h/4242/z/11/overlay", OVERLAY_21C, sizeof OVERLAY_21C);
    from_joiner(&s, false, &m);
    tx_done(&s, (int)n_sent, LINK_ACKED);
    away = false;
    CHECK(session_restate_zone(&s, 11, true, now));
    last_write_is(&(struct tado_zone_block){.on = true, .setpoint = 1800, .overlay = TADO_OVERLAY_MANUAL,
                                            .overlay_on = true, .overlay_setpoint = 2100});
    CHECK(!session_restate_zone(&s, 12, true, now));
}

void run_session_tests(void)
{
    verbose = getenv("VERBOSE") != NULL;
    walks_a_valve_from_beacon_to_joined();
    a_solicitation_within_three_seconds_of_the_last_advertisement_gets_none();
    retries_d_pair_with_a_new_mid_and_gives_up_after_the_last_attempt();
    a_late_204_to_an_earlier_attempt_still_counts_as_joined();
    lost_tx_done_events_do_not_stall_the_join();
    a_refused_d_pair_is_tried_again();
    a_refused_advertisement_still_leads_to_d_pair();
    an_empty_ack_then_a_separate_204_joins();
    a_joiner_learned_within_the_advertisement_gap_still_gets_d_pair();
    an_answer_under_the_pairing_key_means_the_valve_did_not_adopt_our_key();
    ignores_requests_under_the_pairing_key();
    serves_a_session_and_the_time_after_the_join();
    serving_sends_an_unsolicited_router_advertisement_every_200_to_600_seconds();
    serving_answers_a_solicitation_under_the_network_key_only();
    answers_a_repeat_again_when_the_tx_done_of_the_first_answer_was_lost();
    answers_a_repeated_request_once_while_its_answer_is_on_its_way();
    serves_the_installations_devices_their_config_and_zone_document();
    answers_a_fragmented_report_with_204();
    a_pair_window_does_not_interrupt_serving();
    keeps_a_setpoint_set_on_the_device_for_the_overlay_time_then_writes_the_zone_back();
    repeats_an_unacknowledged_zone_write_under_its_mid_then_gives_up();
    an_overlay_the_device_ends_or_that_is_not_its_zone_leaves_nothing_to_write();
    an_overlay_time_of_zero_keeps_the_devices_setpoint_until_changed();
    a_thirty_day_overlay_lasts_thirty_days();
    a_joined_device_gets_the_zone_the_gateway_gives_it_and_its_config();
    a_device_without_a_zone_gets_no_config();
    hands_a_decoded_report_to_the_gateway();
    leaves_time_unanswered_until_the_clock_is_set();
    writes_a_new_setpoint_to_the_owner();
    a_boost_runs_its_time_or_until_cancelled();
    the_switch_to_away_restates_each_zone();
}
