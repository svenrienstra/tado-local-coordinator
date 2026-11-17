/* Host tests for the portable firmware core. Frames are checked byte for byte
 * against golden/vectors.txt, which tools/golden generates from freedo. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ccm.h"
#include "coordinator.h"
#include "crc.h"
#include "hostlink.h"
#include "mac.h"

static int failures, checks;

#define CHECK(cond)                                                                \
    do {                                                                           \
        checks++;                                                                  \
        if (!(cond)) {                                                             \
            failures++;                                                            \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                          \
    } while (0)

#define CHECK_BYTES(got, got_len, want, want_len)                                  \
    CHECK((got_len) == (want_len) && memcmp((got), (want), (want_len)) == 0)

/* Checks bytes against the whole golden vector of that name. */
#define CHECK_VEC(got, got_len, name)                                              \
    do {                                                                           \
        const struct vec *want_ = V(name);                                         \
        CHECK_BYTES(got, got_len, want_->data, want_->len);                        \
    } while (0)

/* --- golden vectors ----------------------------------------------------- */

struct vec {
    char name[64];
    uint8_t data[256];
    size_t len;
};
static struct vec vecs[64];
static size_t nvecs;

static void load_vectors(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        perror(path);
        exit(2);
    }
    char line[1024], hex[1024];
    while (fgets(line, sizeof line, f)) {
        struct vec *v = &vecs[nvecs];
        if (line[0] == '#' || sscanf(line, "%63s %1023s", v->name, hex) != 2)
            continue;
        v->len = strlen(hex) / 2;
        for (size_t i = 0; i < v->len; i++)
            sscanf(&hex[2 * i], "%2hhx", &v->data[i]);
        nvecs++;
    }
    fclose(f);
}

static const struct vec *V(const char *name)
{
    for (size_t i = 0; i < nvecs; i++)
        if (strcmp(vecs[i].name, name) == 0)
            return &vecs[i];
    fprintf(stderr, "missing vector %s\n", name);
    exit(2);
}

/* --- platform stubs ----------------------------------------------------- */

static uint32_t now_ms, now_rat;
static uint8_t host_out[8192];
static size_t host_out_len;
static uint8_t sent_frame[MAC_MAX_FRAME];
static size_t sent_len;
static uint32_t sent_at;
static int sends;
static int jobs;
static int job_aborts;
static uint16_t job_wake_frames;
static uint8_t job_short[2];
static uint8_t job_token; /* of the last job started, as the radio reports it */
static bool radio_refuses; /* radio_send_at refuses, as during a job */

uint32_t platform_ms(void) { return now_ms; }
uint32_t radio_rat_now(void) { return now_rat; }
static uint32_t channel_khz;
void radio_set_channel(uint32_t khz) { channel_khz = khz; }

bool radio_send_at(const uint8_t *frame, size_t len, uint32_t rat_start)
{
    if (radio_refuses)
        return false;
    memcpy(sent_frame, frame, len);
    sent_len = len;
    sent_at = rat_start;
    sends++;
    return true;
}

static uint32_t job_start; /* RAT time of the last job's first frame */
bool radio_send_job(const uint8_t dst_short[2], uint8_t seq, uint16_t wake_frames,
                    uint32_t rat_start, const uint8_t *frame, size_t len, uint8_t token)
{
    (void)seq;
    job_start = rat_start;
    job_token = token;
    memcpy(job_short, dst_short, 2);
    job_wake_frames = wake_frames;
    memcpy(sent_frame, frame, len);
    sent_len = len;
    jobs++;
    return true;
}

static bool radio_on_air; /* radio_busy(): a frame on air or posted */
bool radio_busy(void) { return radio_on_air; }
unsigned radio_abort_job(void)
{
    job_aborts++;
    return 0;
}

static uint32_t rf_rx_ok, rf_rx_crc_fail;
void radio_rx_counts(uint32_t *ok, uint32_t *crc_fail)
{
    *ok = rf_rx_ok;
    *crc_fail = rf_rx_crc_fail;
}

void hostlink_write(const uint8_t *data, size_t len)
{
    memcpy(&host_out[host_out_len], data, len);
    host_out_len += len;
}

static void reset_stubs(void)
{
    host_out_len = 0;
    sent_len = 0;
    sends = jobs = job_aborts = 0;
    radio_refuses = false;
    radio_on_air = false;
}

/* Finds the n-th frame of a given type in host_out; returns its payload. */
static const uint8_t *host_frame(uint8_t type, int n, size_t *len)
{
    for (size_t i = 0; i + 6 <= host_out_len;) {
        size_t body = host_out[i + 2] | (size_t)host_out[i + 3] << 8;
        if (host_out[i + 5] == type && n-- == 0) {
            *len = body - 2;
            return &host_out[i + 6];
        }
        i += body + 6;
    }
    return NULL;
}

/* --- tests -------------------------------------------------------------- */

static void test_aes_fips197(void)
{
    /* Guards the test harness itself: FIPS-197 appendix C.1. */
    static const uint8_t key[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    static const uint8_t pt[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                   0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    static const uint8_t ct[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
                                   0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
    uint8_t out[16];
    aes128_encrypt_block(key, pt, out);
    CHECK(memcmp(out, ct, 16) == 0);
}

static void test_crc(void)
{
    /* The catalogue check values: the host link's CRC is only tested against
     * itself elsewhere, so a wrong table would not show up there. */
    static const uint8_t check[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(crc16_ccitt_false(CRC16_CCITT_FALSE_INIT, check, 9) == 0x29b1);
    CHECK(crc16_mac_fcs(0, check, 9) == 0x2189); /* CRC-16/KERMIT */

    /* Continuing over split buffers gives the same result. */
    uint16_t a = crc16_ccitt_false(CRC16_CCITT_FALSE_INIT, check, 4);
    CHECK(crc16_ccitt_false(a, &check[4], 5) == 0x29b1);
    CHECK(crc16_mac_fcs(crc16_mac_fcs(0, check, 4), &check[4], 5) == 0x2189);
}

static void test_beacon_exchange(void)
{
    const struct vec *req = V("beacon_request.frame");
    uint8_t seq, src[8];
    uint32_t counter;
    CHECK(mac_parse_beacon_request(req->data, req->len, PAIRING_KEY, &seq, src, &counter));
    CHECK(seq == 0x7b && counter == 16);
    CHECK(memcmp(src, V("in.joiner_eui")->data, 8) == 0);
    CHECK(!mac_parse_beacon_request(req->data, req->len, V("in.network_key")->data, &seq, src,
                                    &counter));

    uint8_t eb[MAC_ENH_BEACON_LEN];
    mac_build_enh_beacon(eb, PAIRING_KEY, 0x59, V("in.joiner_eui")->data, V("in.coord_eui")->data,
                         13537);
    CHECK_VEC(eb, sizeof eb, "enh_beacon.frame");
}

static void test_wakeup_and_ack(void)
{
    uint8_t w[MAC_WAKEUP_LEN], sh[2];
    static const uint8_t bcast[2] = {0xff, 0xff};
    mac_build_wakeup(w, 0x10, bcast, 3125);
    CHECK_VEC(w, sizeof w, "wakeup.bcast_3125");
    mac_short_addr(V("in.joiner_eui")->data, sh);
    mac_build_wakeup(w, 0x11, sh, 248);
    CHECK_VEC(w, sizeof w, "wakeup.unicast_248");

    CHECK(V("wakeup.ra_burst_count")->data[0] == MAC_TRAIN_FULL);
    mac_build_wakeup(w, 0x12, bcast, mac_train_rendezvous(0, MAC_TRAIN_FULL));
    CHECK_BYTES(w, sizeof w, V("wakeup.ra_burst_first")->data, MAC_WAKEUP_LEN);
    mac_build_wakeup(w, 0x12, bcast, mac_train_rendezvous(MAC_TRAIN_FULL - 1, MAC_TRAIN_FULL));
    CHECK_BYTES(w, sizeof w, V("wakeup.ra_burst_last")->data, MAC_WAKEUP_LEN);

    /* A short train as the commercial bridge sent one to a valve (our capture of
     * 2026-11-17). */
    static const uint16_t bridge_21[21] = {248, 235, 223, 210, 198, 186, 173, 161, 148, 136, 124,
                                           111, 99,  86,  74,  62,  49,  37,  24,  12,  0};
    bool same = true;
    for (int i = 0; i < 21; i++)
        same = same && mac_train_rendezvous(i, 21) == bridge_21[i];
    CHECK(same);

    uint8_t ack[MAC_ENH_ACK_LEN], seq, dst[8];
    mac_build_enh_ack(ack, 0x22, V("in.joiner_eui")->data, V("in.coord_eui")->data, 0x0123);
    CHECK_VEC(ack, sizeof ack, "enh_ack.frame");
    CHECK(mac_parse_enh_ack(ack, sizeof ack, &seq, dst) && seq == 0x22);

    /* The ACK's target comes from the clear header of a unicast to us that asks
     * for one, the EUI's encrypted tail being the fixed tado suffix. */
    const uint8_t *coord = V("in.coord_eui")->data, *joiner = V("in.joiner_eui")->data;
    static const uint8_t pkt[2] = {0x7e, 0x33};
    uint8_t f[MAC_MAX_FRAME], src[8];
    size_t n = mac_seal_unicast(f, V("in.network_key")->data, false, 0x41, coord, joiner, 1, pkt,
                                sizeof pkt);
    CHECK(mac_ack_request(f, n, coord, &seq, src) && seq == 0x41 && memcmp(src, joiner, 8) == 0);
    CHECK(!mac_ack_request(f, n, joiner, &seq, src)); /* not to us */
    n = mac_seal_broadcast(f, V("in.network_key")->data, false, 0x42, coord, 1, pkt, sizeof pkt);
    CHECK(!mac_ack_request(f, n, coord, &seq, src));
    ack[5] ^= 1;
    CHECK(!mac_parse_enh_ack(ack, sizeof ack, &seq, dst));
}

static void test_pair_frames(void)
{
    const struct vec *coap = V("pair.coap");
    uint8_t f[MAC_MAX_FRAME], scratch[MAC_MAX_FRAME];
    size_t n = mac_seal_unicast(f, PAIRING_KEY, false, 0x30, V("in.joiner_eui")->data,
                                V("in.coord_eui")->data, 0x100, coap->data, coap->len);
    CHECK_VEC(f, n, "pair.frame");

    /* The valve's 2.04 opens under the new network key only. */
    const struct vec *ans = V("pair_answer.frame");
    struct mac_data d;
    CHECK(!mac_open_data(ans->data, ans->len, PAIRING_KEY, scratch, &d));
    CHECK(mac_open_data(ans->data, ans->len, V("in.network_key")->data, scratch, &d));
    CHECK(!d.broadcast && d.ack_request && d.seq == 0x05 && d.counter == 7);
    CHECK(memcmp(d.src, V("in.joiner_eui")->data, 8) == 0);
    CHECK(memcmp(d.dst, V("in.coord_eui")->data, 8) == 0);
    CHECK_VEC(d.packet, d.packet_len, "pair_answer.coap");

    /* A packet that does not fit one frame is refused. */
    uint8_t big[MAC_UNICAST_MAX_PACKET + 1] = {0};
    CHECK(mac_seal_unicast(f, PAIRING_KEY, false, 0, V("in.joiner_eui")->data,
                           V("in.coord_eui")->data, 0, big, sizeof big) == 0);
}

static void test_router_advert_frames(void)
{
    const struct vec *ra = V("ra.packet");
    const uint8_t *coord = V("in.coord_eui")->data;
    uint8_t f[MAC_MAX_FRAME], scratch[MAC_MAX_FRAME];
    size_t n = mac_seal_broadcast(f, V("in.network_key")->data, true, 0x40, coord, 0x0200,
                                  ra->data, ra->len);
    CHECK_VEC(f, n, "ra.frame_network_pending");
    n = mac_seal_broadcast(f, PAIRING_KEY, false, 0x41, coord, 0x0201, ra->data, ra->len);
    CHECK_VEC(f, n, "ra.frame_pairing");

    struct mac_data d;
    CHECK(mac_open_data(f, n, PAIRING_KEY, scratch, &d));
    CHECK(d.broadcast && !d.ack_request && d.counter == 0x0201);
    CHECK_BYTES(d.packet, d.packet_len, ra->data, ra->len);
}

/* --- host link ---------------------------------------------------------- */

static int handler_calls;
static enum hl_status echo_handler(void *ctx, uint8_t type, const uint8_t *p, size_t len,
                                   uint8_t *reply, size_t *reply_len)
{
    *(int *)ctx += 1;
    reply[0] = type;
    memcpy(&reply[1], p, len);
    *reply_len = len + 1;
    return HL_OK;
}

static void test_hostlink(void)
{
    struct hostlink hl;
    uint8_t req[HL_MAX_FRAME], first_reply[HL_MAX_FRAME];
    static const uint8_t payload[3] = {0xaa, 0xbb, 0xcc};
    size_t n = hostlink_encode(req, 7, 0x42, payload, sizeof payload);
    size_t len;
    const uint8_t *p;

    /* Round trip, after line noise that contains a false start. */
    hostlink_init(&hl, echo_handler, &handler_calls);
    reset_stubs();
    handler_calls = 0;
    static const uint8_t noise[] = {0x00, 0x7e, 0x13, 0x7e, 0xa5, 0xff, 0xff};
    hostlink_feed(&hl, noise, sizeof noise);
    hostlink_feed(&hl, req, n);
    p = host_frame(0x42 | HL_REPLY_FLAG, 0, &len);
    CHECK(handler_calls == 1 && p && len == 5 && p[0] == HL_OK && p[1] == 0x42 &&
          memcmp(&p[2], payload, 3) == 0);
    memcpy(first_reply, host_out, host_out_len);
    size_t first_len = host_out_len;

    /* A retransmission (same seq) gets the cached reply and does not run again. */
    reset_stubs();
    hostlink_feed(&hl, req, n);
    CHECK(handler_calls == 1);
    CHECK_BYTES(host_out, host_out_len, first_reply, first_len);

    /* The same seq with another payload, as from a restarted host, is a new
     * request: it runs and gets its own reply. */
    reset_stubs();
    static const uint8_t other[3] = {0x11, 0x22, 0x33};
    uint8_t req2[HL_MAX_FRAME];
    size_t n2 = hostlink_encode(req2, 7, 0x42, other, sizeof other);
    hostlink_feed(&hl, req2, n2);
    p = host_frame(0x42 | HL_REPLY_FLAG, 0, &len);
    CHECK(handler_calls == 2 && p && len == 5 && memcmp(&p[2], other, 3) == 0);

    /* A corrupted frame is dropped; the next good one, fed byte by byte, runs. */
    reset_stubs();
    uint8_t bad[HL_MAX_FRAME];
    size_t bn = hostlink_encode(bad, 8, 0x42, payload, sizeof payload);
    bad[7] ^= 0x01;
    hostlink_feed(&hl, bad, bn);
    CHECK(handler_calls == 2 && host_out_len == 0);
    n = hostlink_encode(req, 9, 0x43, payload, 1);
    for (size_t i = 0; i < n; i++)
        hostlink_feed(&hl, &req[i], 1);
    CHECK(handler_calls == 3 && host_frame(0x43 | HL_REPLY_FLAG, 0, &len) != NULL);
}

/* --- coordinator -------------------------------------------------------- */

static void host_request(struct coordinator *c, uint8_t seq, uint8_t type, const uint8_t *p,
                         size_t len)
{
    uint8_t f[HL_MAX_FRAME];
    size_t n = hostlink_encode(f, seq, type, p, len);
    coord_on_host_bytes(c, f, n);
}

static void config_request(struct coordinator *c, uint8_t seq, uint32_t counter_floor)
{
    uint8_t cfg[32];
    memcpy(cfg, V("in.coord_eui")->data, 8);
    memcpy(&cfg[8], V("in.network_key")->data, 16);
    memcpy(&cfg[24], &counter_floor, 4); /* host is little-endian */
    uint32_t khz = 868300;
    memcpy(&cfg[28], &khz, 4);
    host_request(c, seq, HL_CONFIG, cfg, sizeof cfg);
}

static void configure(struct coordinator *c, uint32_t counter_floor)
{
    config_request(c, 0x10, counter_floor); /* a seq no test request reuses */
}

static void test_coordinator_beacon_window(void)
{
    static struct coordinator c;
    const struct vec *req = V("beacon_request.frame");
    size_t len;
    coord_init(&c);
    now_ms = 1000;

    /* A CONFIG with an EUI outside the tado suffix is refused. */
    reset_stubs();
    uint8_t cfg[32] = {0};
    host_request(&c, 1, HL_CONFIG, cfg, sizeof cfg);
    const uint8_t *r = host_frame(HL_CONFIG | HL_REPLY_FLAG, 0, &len);
    CHECK(r && r[0] == HL_BAD_ARG);
    configure(&c, 13537);

    /* Window closed: no answer, the request is reported as unverified. */
    reset_stubs();
    coord_on_rx(&c, req->data, req->len, -40, 100000, 0);
    CHECK(sends == 0 && host_frame(HL_EV_RX_UNVERIFIED, 0, &len) != NULL);

    /* Window open: the beacon goes out 3 ms after the request, under the
     * pairing key, with the next seq and counter. */
    reset_stubs();
    uint8_t secs[2] = {120, 0};
    host_request(&c, 2, HL_PAIR_WINDOW, secs, 2);
    coord_on_rx(&c, req->data, req->len, -40, 100000, 0);
    CHECK(sends == 1 && sent_at == 100000 + BEACON_DELAY_RAT);
    uint8_t want[MAC_ENH_BEACON_LEN];
    mac_build_enh_beacon(want, PAIRING_KEY, 0, V("in.joiner_eui")->data, V("in.coord_eui")->data,
                         13537);
    CHECK_BYTES(sent_frame, sent_len, want, sizeof want);
    const uint8_t *ev = host_frame(HL_EV_BEACON_ANSWERED, 0, &len);
    CHECK(ev && len == 14 && memcmp(ev, V("in.joiner_eui")->data, 8) == 0 && ev[8] == 16);

    /* Radio busy with a job: no beacon, no event, seq and counter unspent. */
    reset_stubs();
    radio_refuses = true;
    uint8_t seq = c.seq;
    uint32_t counter = c.counter;
    coord_on_rx(&c, req->data, req->len, -40, 150000, 0);
    CHECK(host_frame(HL_EV_BEACON_ANSWERED, 0, &len) == NULL);
    CHECK(c.seq == seq && c.counter == counter);
    radio_refuses = false;
    coord_on_rx(&c, req->data, req->len, -40, 160000, 0);
    ev = host_frame(HL_EV_BEACON_ANSWERED, 0, &len);
    CHECK(sends == 1 && ev && ev[13] == seq);

    /* The window closes on its own after 120 s. */
    reset_stubs();
    now_ms += 120 * 1000;
    coord_on_rx(&c, req->data, req->len, -40, 200000, 0);
    CHECK(sends == 0);
}

static void test_coordinator_key_fallback(void)
{
    static struct coordinator c;
    const uint8_t *coord = V("in.coord_eui")->data, *joiner = V("in.joiner_eui")->data;
    static const uint8_t pkt[4] = {0x7e, 0x33, 0x3a, 0x85};
    uint8_t f[MAC_MAX_FRAME];
    size_t len;
    coord_init(&c);
    now_ms = 5000;
    configure(&c, 0);

    /* A joiner's frame under the pairing key: refused while the window is closed,
     * though ACKed, since the ACK goes out before any key is tried. */
    size_t n = mac_seal_unicast(f, PAIRING_KEY, false, 9, coord, joiner, 3, pkt, sizeof pkt);
    reset_stubs();
    coord_on_rx(&c, f, n, -50, 4000, 0);
    CHECK(host_frame(HL_EV_RX, 0, &len) == NULL && sends == 1);

    /* Window open: accepted, flagged as pairing key, and Enhanced-ACKed. */
    uint8_t secs[2] = {60, 0};
    host_request(&c, 2, HL_PAIR_WINDOW, secs, 2);
    reset_stubs();
    coord_on_rx(&c, f, n, -50, 4000, 0);
    const uint8_t *ev = host_frame(HL_EV_RX, 0, &len);
    CHECK(ev && (ev[0] & 0x01) && (ev[0] & 0x04) && ev[2] == 9 &&
          memcmp(&ev[3], joiner, 8) == 0 && ev[11] == 3 && len == 19 + sizeof pkt &&
          memcmp(&ev[19], pkt, sizeof pkt) == 0);
    uint8_t seq, dst[8];
    CHECK(sends == 1 && mac_parse_enh_ack(sent_frame, sent_len, &seq, dst) && seq == 9 &&
          memcmp(dst, joiner, 8) == 0 && sent_at == 4000 + ACK_DELAY_RAT);

    /* Under the network key it is accepted without the pairing flag. */
    n = mac_seal_unicast(f, V("in.network_key")->data, false, 10, coord, joiner, 4, pkt,
                         sizeof pkt);
    reset_stubs();
    coord_on_rx(&c, f, n, -50, 5000, 0);
    ev = host_frame(HL_EV_RX, 0, &len);
    CHECK(ev && !(ev[0] & 0x01));

    /* A repeat of it (same MAC seq, as a device retries a frame it saw no ACK
     * for) is ACKed again but not passed on; a new seq is. */
    reset_stubs();
    coord_on_rx(&c, f, n, -50, 5200, 0);
    CHECK(sends == 1 && host_frame(HL_EV_RX, 0, &len) == NULL && c.stats.rx_duplicates == 1);
    now_ms += DEDUP_WINDOW_MS;
    coord_on_rx(&c, f, n, -50, 5300, 0);
    CHECK(host_frame(HL_EV_RX, 0, &len) != NULL); /* outside the window: a new frame */

    /* An ACK the busy radio refuses is not reported as sent. */
    n = mac_seal_unicast(f, V("in.network_key")->data, false, 11, coord, joiner, 5, pkt,
                         sizeof pkt);
    reset_stubs();
    radio_refuses = true;
    coord_on_rx(&c, f, n, -50, 5500, 0);
    ev = host_frame(HL_EV_RX, 0, &len);
    CHECK(ev && !(ev[0] & 0x04));

    /* Frames to another coordinator are not ours to ACK. */
    uint8_t other[8];
    memcpy(other, coord, 8);
    other[0] ^= 0xff;
    n = mac_seal_unicast(f, V("in.network_key")->data, false, 11, other, joiner, 5, pkt,
                         sizeof pkt);
    reset_stubs();
    coord_on_rx(&c, f, n, -50, 6000, 0);
    CHECK(sends == 0 && host_frame(HL_EV_RX, 0, &len) == NULL);
}

static void test_coordinator_tx(void)
{
    static struct coordinator c;
    const uint8_t *joiner = V("in.joiner_eui")->data;
    const struct vec *coap = V("pair.coap");
    size_t len;
    coord_init(&c);
    now_ms = 0;

    /* TX before CONFIG is refused. */
    reset_stubs();
    uint8_t tx[1 + 8 + 128];
    tx[0] = 0x01 | (1 << 4); /* pairing key, full train */
    memcpy(&tx[1], joiner, 8);
    memcpy(&tx[9], coap->data, coap->len);
    host_request(&c, 1, HL_TX, tx, 9 + coap->len);
    const uint8_t *r = host_frame(HL_TX | HL_REPLY_FLAG, 0, &len);
    CHECK(r && r[0] == HL_NOT_CONFIGURED);

    /* d/pair as in the golden vector: seq 0x30, counter 0x100. */
    configure(&c, 0x100);
    c.seq = 0x30;
    reset_stubs();
    host_request(&c, 2, HL_TX, tx, 9 + coap->len);
    r = host_frame(HL_TX | HL_REPLY_FLAG, 0, &len);
    CHECK(r && r[0] == HL_OK && len == 7 && r[2] == 0x30 && r[3] == 0x00 && r[4] == 0x01);
    uint8_t handle = r[1];

    coord_poll(&c);
    CHECK(jobs == 1 && job_wake_frames == MAC_TRAIN_FULL && job_short[0] == joiner[1] &&
          job_short[1] == joiner[0]);
    CHECK_VEC(sent_frame, sent_len, "pair.frame");

    /* The valve's Enhanced ACK for seq 0x30 closes the job. */
    coord_on_job_sent(&c, job_token, true);
    uint8_t ack[MAC_ENH_ACK_LEN];
    mac_build_enh_ack(ack, 0x2f, V("in.coord_eui")->data, joiner, 0);
    coord_on_rx(&c, ack, sizeof ack, -50, 0, 0); /* wrong seq: ignored */
    CHECK(host_frame(HL_EV_TX_DONE, 0, &len) == NULL);
    mac_build_enh_ack(ack, 0x30, V("in.coord_eui")->data, joiner, 0);
    coord_on_rx(&c, ack, sizeof ack, -50, 0, 0);
    const uint8_t *ev = host_frame(HL_EV_TX_DONE, 0, &len);
    CHECK(ev && ev[0] == handle && ev[1] == TX_ACKED);

    /* No ACK within the wait: TX_DONE no-ack. */
    reset_stubs();
    host_request(&c, 3, HL_TX, tx, 9 + coap->len);
    coord_poll(&c);
    coord_on_job_sent(&c, job_token, true);
    now_ms += ACK_WAIT_MS;
    coord_poll(&c);
    ev = host_frame(HL_EV_TX_DONE, 0, &len);
    CHECK(ev && ev[1] == TX_NO_ACK);
}

/* An Enhanced ACK teaches the device's channel sample; an anchored job then goes
 * behind a short train centred on a later sample, handed to the radio only just
 * before it. The RAT wraps on the way. */
static void test_coordinator_anchor(void)
{
    static struct coordinator c;
    const uint8_t *coord = V("in.coord_eui")->data, *joiner = V("in.joiner_eui")->data;
    static const uint8_t pkt[2] = {0x7e, 0x33};
    const uint32_t step = WAKE_STEP_US * 4u;
    uint8_t tx[1 + 8 + sizeof pkt], ack[MAC_ENH_ACK_LEN];
    size_t len;
    coord_init(&c);
    now_ms = 1000;
    now_rat = 0xfff00000u; /* wraps 1048576 ticks (262 ms) from now */
    configure(&c, 0);
    tx[0] = 2 << 4; /* anchored train */
    memcpy(&tx[1], joiner, 8);
    memcpy(&tx[9], pkt, sizeof pkt);

    /* No anchor yet: a full train at once. */
    reset_stubs();
    host_request(&c, 1, HL_TX, tx, sizeof tx);
    coord_poll(&c);
    CHECK(jobs == 1 && job_wake_frames == MAC_TRAIN_FULL && job_start == now_rat + CHAIN_LEAD_RAT);

    /* Its ACK, after the wrap, says the device sampled 100 units (32 ms)
     * before the ACK ended. */
    uint32_t ack_end = now_rat + 4100000u;
    now_rat = ack_end;
    now_ms += 1030;
    mac_build_enh_ack(ack, c.queue[0].seq, coord, joiner, 100);
    coord_on_rx(&c, ack, sizeof ack, -50, ack_end, 0);
    const uint8_t *ev = host_frame(HL_EV_TX_DONE, 0, &len);
    CHECK(ev && ev[1] == TX_ACKED);
    uint32_t sample = ack_end - 100u * CSL_UNIT_RAT;

    /* The next job waits for the device's next sample: 21 frames centred on
     * it, handed over JOB_LEAD_RAT before the first. */
    reset_stubs();
    now_rat += 300000u;
    host_request(&c, 2, HL_TX, tx, sizeof tx);
    coord_poll(&c);
    uint32_t first = sample + CSL_PERIOD_RAT - 10u * step;
    now_rat = first - JOB_LEAD_RAT - 1u;
    coord_poll(&c);
    CHECK(jobs == 0);
    now_rat = first - JOB_LEAD_RAT;
    coord_poll(&c);
    CHECK(jobs == 1 && job_wake_frames == 21 && job_start == first);

    /* No ACK is a miss: the next job goes behind a full train, at once. */
    coord_on_job_sent(&c, job_token, true);
    now_ms += ACK_WAIT_MS;
    coord_poll(&c);
    ev = host_frame(HL_EV_TX_DONE, 0, &len);
    CHECK(ev && ev[1] == TX_NO_ACK);
    reset_stubs();
    host_request(&c, 3, HL_TX, tx, sizeof tx);
    coord_poll(&c);
    CHECK(jobs == 1 && job_wake_frames == MAC_TRAIN_FULL);

    /* Its ACK renews the anchor. A main loop that comes back after the train
     * should have started moves the job to the sample a period later. */
    ack_end = now_rat + 4100000u;
    now_rat = ack_end;
    mac_build_enh_ack(ack, c.queue[0].seq, coord, joiner, 100);
    coord_on_rx(&c, ack, sizeof ack, -50, ack_end, 0);
    reset_stubs();
    host_request(&c, 4, HL_TX, tx, sizeof tx);
    coord_poll(&c);
    first = ack_end - 100u * CSL_UNIT_RAT + CSL_PERIOD_RAT - 10u * step;
    now_rat = first + 2u * RAT_PER_MS;
    coord_poll(&c);
    CHECK(jobs == 0);
    now_rat = first + CSL_PERIOD_RAT - JOB_LEAD_RAT;
    coord_poll(&c);
    CHECK(jobs == 1 && job_wake_frames == 21 && job_start == first + CSL_PERIOD_RAT);
    CHECK(c.stats.jobs_anchored == 2);

    /* A full train is still a full train when the host asks for one. */
    coord_on_job_sent(&c, job_token, true);
    mac_build_enh_ack(ack, c.queue[0].seq, coord, joiner, 100);
    coord_on_rx(&c, ack, sizeof ack, -50, now_rat, 0);
    reset_stubs();
    tx[0] = 1 << 4;
    host_request(&c, 5, HL_TX, tx, sizeof tx);
    coord_poll(&c);
    CHECK(jobs == 1 && job_wake_frames == MAC_TRAIN_FULL);
}

static bool log_is(int n, const char *text);

/* Sends n verified frames from src carrying the RF core's offset v. */
static void frames_with_offset(struct coordinator *c, const uint8_t src[8], int n, int8_t v)
{
    static uint8_t seq;
    static uint32_t counter = 100;
    static const uint8_t pkt[2] = {0x7e, 0x33};
    uint8_t f[MAC_MAX_FRAME];
    for (int i = 0; i < n; i++) {
        size_t len = mac_seal_unicast(f, V("in.network_key")->data, false, seq++,
                                      V("in.coord_eui")->data, src, counter++, pkt, sizeof pkt);
        coord_on_rx(c, f, len, -50, 0, v);
    }
}

/* The radio follows our devices' carrier, read from the RF core's offset
 * estimate (390.625 Hz a unit). */
static void test_coordinator_tune(void)
{
    static struct coordinator c;
    uint8_t a[8], b[8], d[8];
    size_t len;
    memcpy(a, V("in.joiner_eui")->data, 8);
    memcpy(b, a, 8);
    b[0] ^= 1;
    memcpy(d, a, 8);
    d[0] ^= 2;
    coord_init(&c);
    now_ms = 1000;
    now_rat = 0; /* the frames end at 0: their ACKs are on time */
    configure(&c, 0);
    CHECK(channel_khz == 868300);

    /* 16 readings of +26 units (10156 Hz) from one device: logged, then the
     * radio moves 10 kHz up when it is free. */
    reset_stubs();
    frames_with_offset(&c, a, 15, 26);
    CHECK(host_frame(HL_EV_LOG, 0, &len) == NULL);
    frames_with_offset(&c, a, 1, 26);
    CHECK(log_is(0, "carrier offset 10156 Hz over 1 devices"));
    radio_on_air = true;
    coord_poll(&c);
    CHECK(channel_khz == 868300);
    radio_on_air = false;
    coord_poll(&c);
    CHECK(channel_khz == 868310 && log_is(1, "retuned to 868310 kHz (10 from CONFIG)"));

    /* Inside the deadband: logged, no retune. */
    reset_stubs();
    frames_with_offset(&c, a, 16, 8);
    coord_poll(&c);
    CHECK(log_is(0, "carrier offset 3125 Hz over 1 devices") && channel_khz == 868310);

    /* At most one retune per TUNE_INTERVAL_MS. */
    reset_stubs();
    frames_with_offset(&c, a, 16, -26);
    coord_poll(&c);
    CHECK(channel_khz == 868310);
    now_ms += TUNE_INTERVAL_MS;
    coord_poll(&c);
    CHECK(channel_khz == 868300);

    /* The median of the devices' means: 0, 10156 and 11718 Hz give 10156. A
     * frame no key opens, as from another network, does not count, nor does a
     * reading at the estimator's limit. */
    reset_stubs();
    static const uint8_t junk[30] = {0x69, 0xec};
    for (int i = 0; i < 16; i++)
        coord_on_rx(&c, junk, sizeof junk, -50, 0, 60);
    frames_with_offset(&c, a, 8, TUNE_CLIP);
    CHECK(host_frame(HL_EV_LOG, 0, &len) == NULL);
    frames_with_offset(&c, a, 4, 0);
    frames_with_offset(&c, b, 4, 26);
    frames_with_offset(&c, d, 8, 30);
    CHECK(log_is(0, "carrier offset 10156 Hz over 3 devices"));
    now_ms += TUNE_INTERVAL_MS;
    coord_poll(&c);
    CHECK(channel_khz == 868310);

    /* Never more than TUNE_MAX_HZ from the CONFIG frequency. */
    for (int i = 0; i < 4; i++) {
        frames_with_offset(&c, a, 16, 100);
        now_ms += TUNE_INTERVAL_MS;
        coord_poll(&c);
    }
    CHECK(channel_khz == 868300 + TUNE_MAX_HZ / 1000);

    /* CONFIG starts over from its own frequency. */
    config_request(&c, 0x11, 0);
    CHECK(channel_khz == 868300 && c.tune_khz == 0);
}

static void test_coordinator_job_recovery(void)
{
    static struct coordinator c;
    const uint8_t *joiner = V("in.joiner_eui")->data;
    const struct vec *coap = V("pair.coap");
    const uint8_t *ev;
    size_t len;
    coord_init(&c);
    now_ms = 0;
    configure(&c, 0x100);

    uint8_t tx[1 + 8 + 128];
    tx[0] = 1 << 4; /* network key, unicast, full train */
    memcpy(&tx[1], joiner, 8);
    memcpy(&tx[9], coap->data, coap->len);
    size_t tx_len = 9 + coap->len;

    /* A CONFIG while a job is on air aborts it: TX_DONE radio error at once,
     * the radio's late failure report adds nothing, and the next job goes. */
    reset_stubs();
    host_request(&c, 1, HL_TX, tx, tx_len);
    host_request(&c, 2, HL_TX, tx, tx_len);
    coord_poll(&c);
    CHECK(jobs == 1);
    uint8_t first = host_frame(HL_TX | HL_REPLY_FLAG, 0, &len)[1];
    config_request(&c, 3, 0x200);
    ev = host_frame(HL_EV_TX_DONE, 0, &len);
    CHECK(ev && ev[0] == first && ev[1] == TX_RADIO_ERROR);
    coord_on_job_sent(&c, job_token, false);
    coord_poll(&c);
    CHECK(host_frame(HL_EV_TX_DONE, 1, &len) == NULL && jobs == 2);

    /* The radio never reports the job: still on air through its full train,
     * failed and dropped from the radio once that is well over. */
    reset_stubs();
    uint8_t second = c.queue[0].handle;
    now_ms += MAC_TRAIN_FULL * WAKE_STEP_MS + 20;
    coord_poll(&c);
    CHECK(host_frame(HL_EV_TX_DONE, 0, &len) == NULL && job_aborts == 0);
    now_ms += 80;
    coord_poll(&c);
    ev = host_frame(HL_EV_TX_DONE, 0, &len);
    CHECK(ev && ev[0] == second && ev[1] == TX_RADIO_ERROR && job_aborts == 1);
    host_request(&c, 4, HL_TX, tx, tx_len);
    coord_poll(&c);
    CHECK(jobs == 1);

    /* The Enhanced ACK handled before the radio reports the Data frame done,
     * as both can come in one main-loop pass: acked, and only once. */
    reset_stubs();
    uint8_t ack[MAC_ENH_ACK_LEN];
    mac_build_enh_ack(ack, c.queue[0].seq, V("in.coord_eui")->data, joiner, 0);
    coord_on_rx(&c, ack, sizeof ack, -50, 0, 0);
    coord_on_job_sent(&c, job_token, true);
    now_ms += ACK_WAIT_MS;
    coord_poll(&c);
    ev = host_frame(HL_EV_TX_DONE, 0, &len);
    CHECK(ev && ev[1] == TX_ACKED && host_frame(HL_EV_TX_DONE, 1, &len) == NULL);

    /* The early ACK closes job A and B starts before the radio's late report
     * for A arrives: that report must not touch B, which goes on normally. */
    reset_stubs();
    host_request(&c, 5, HL_TX, tx, tx_len);
    host_request(&c, 6, HL_TX, tx, tx_len);
    coord_poll(&c);
    uint8_t token_a = job_token;
    mac_build_enh_ack(ack, c.queue[0].seq, V("in.coord_eui")->data, joiner, 0);
    coord_on_rx(&c, ack, sizeof ack, -50, 0, 0);
    coord_poll(&c);
    CHECK(jobs == 2 && job_token != token_a);
    uint8_t b = c.queue[0].handle;
    coord_on_job_sent(&c, token_a, true);
    now_ms += ACK_WAIT_MS;
    coord_poll(&c);
    CHECK(c.job_on_air && !c.awaiting_ack && host_frame(HL_EV_TX_DONE, 1, &len) == NULL);
    coord_on_job_sent(&c, job_token, true);
    mac_build_enh_ack(ack, c.queue[0].seq, V("in.coord_eui")->data, joiner, 0);
    coord_on_rx(&c, ack, sizeof ack, -50, 0, 0);
    ev = host_frame(HL_EV_TX_DONE, 1, &len);
    CHECK(ev && ev[0] == b && ev[1] == TX_ACKED);
}

/* Counts LOG lines in host_out that start with prefix. */
static int logs(const char *prefix)
{
    size_t len;
    const uint8_t *p;
    int found = 0;
    for (int i = 0; (p = host_frame(HL_EV_LOG, i, &len)) != NULL; i++)
        if (len >= strlen(prefix) && memcmp(p, prefix, strlen(prefix)) == 0)
            found++;
    return found;
}

static bool log_is(int n, const char *text)
{
    size_t len;
    const uint8_t *p = host_frame(HL_EV_LOG, n, &len);
    return p && len == strlen(text) && memcmp(p, text, len) == 0;
}

/* Requests STATS and returns counter i of the reply. */
static uint32_t stat(struct coordinator *c, int i)
{
    static uint8_t seq = 0x80;
    size_t len;
    host_out_len = 0;
    host_request(c, seq++, HL_STATS, NULL, 0);
    const uint8_t *r = host_frame(HL_STATS | HL_REPLY_FLAG, 0, &len);
    if (!r || len != 1 + 60 || r[0] != HL_OK)
        return 0xffffffffu;
    const uint8_t *v = &r[1 + 4 * i];
    return v[0] | (uint32_t)v[1] << 8 | (uint32_t)v[2] << 16 | (uint32_t)v[3] << 24;
}

enum {
    S_RX_OK, S_RX_CRC_FAIL, S_RX_UNVERIFIED, S_BEACONS, S_ACKS, S_REFUSED, S_JOBS_OK,
    S_JOBS_FAILED, S_JOBS_TIMED_OUT, S_RADIO_ERRORS, S_LINK_CRC, S_LINK_RESYNCS, S_ACKS_LATE,
    S_JOBS_ANCHORED, S_RX_DUPLICATES,
};

static void test_stats_and_log(void)
{
    static struct coordinator c;
    const struct vec *req = V("beacon_request.frame");
    const uint8_t *joiner = V("in.joiner_eui")->data;
    size_t len;
    coord_init(&c);
    now_ms = 50000;
    reset_stubs();

    /* The boot line, and PING reports version 2. */
    coord_boot(&c, "pin");
    CHECK(log_is(0, "boot fw=2 reset=pin"));
    host_request(&c, 1, HL_PING, NULL, 0);
    const uint8_t *r = host_frame(HL_PING | HL_REPLY_FLAG, 0, &len);
    CHECK(r && r[0] == HL_OK && r[1] == 2 && r[2] == 0);

    /* STATS before CONFIG: 12 counters, the RF core's two first. A frame
     * heard before CONFIG counts as unverified. */
    rf_rx_ok = 7;
    rf_rx_crc_fail = 0x12345678;
    coord_on_rx(&c, req->data, req->len, -40, 0, 0);
    CHECK(stat(&c, S_RX_OK) == 7 && stat(&c, S_RX_CRC_FAIL) == 0x12345678);
    CHECK(stat(&c, S_RX_UNVERIFIED) == 1 && stat(&c, S_LINK_RESYNCS) == 0);

    /* Link noise: every skipped byte is a resync; a bad CRC is counted. */
    static const uint8_t noise[] = {0x00, 0x7e, 0x13, 0x7e, 0xa5, 0xff, 0xff};
    coord_on_host_bytes(&c, noise, sizeof noise);
    uint8_t bad[HL_MAX_FRAME];
    size_t bn = hostlink_encode(bad, 2, HL_PING, NULL, 0);
    bad[bn - 1] ^= 1;
    coord_on_host_bytes(&c, bad, bn);
    CHECK(stat(&c, S_LINK_CRC) == 1 && stat(&c, S_LINK_RESYNCS) == sizeof noise + bn);

    /* A beacon answered, then one refused while a job is on air. */
    configure(&c, 100);
    uint8_t secs[2] = {60, 0};
    host_request(&c, 3, HL_PAIR_WINDOW, secs, 2);
    coord_on_rx(&c, req->data, req->len, -40, 1000, 0);
    CHECK(stat(&c, S_BEACONS) == 1 && stat(&c, S_REFUSED) == 0);
    reset_stubs();
    radio_refuses = true;
    coord_on_rx(&c, req->data, req->len, -40, 2000, 0);
    CHECK(logs("refused beacon seq=") == 1);
    CHECK(stat(&c, S_BEACONS) == 1 && stat(&c, S_REFUSED) == 1);

    /* An ACK sent and an ACK refused. */
    uint8_t f[MAC_MAX_FRAME];
    static const uint8_t pkt[2] = {0x7e, 0x33};
    size_t n = mac_seal_unicast(f, V("in.network_key")->data, false, 9, V("in.coord_eui")->data,
                                joiner, 1, pkt, sizeof pkt);
    reset_stubs();
    now_rat = 3000 + 400; /* the main loop sees the frame 100 us after it ended */
    coord_on_rx(&c, f, n, -50, 3000, 0);
    radio_refuses = true;
    coord_on_rx(&c, f, n, -50, 4000, 0);
    CHECK(log_is(0, "refused ack seq=9"));
    CHECK(stat(&c, S_ACKS) == 1 && stat(&c, S_REFUSED) == 2 && stat(&c, S_ACKS_LATE) == 0);
    radio_refuses = false;

    /* An ACK posted after its start time still goes out, late, and is counted. */
    reset_stubs();
    now_rat = 6000 + ACK_DELAY_RAT + 8;
    coord_on_rx(&c, f, n, -50, 6000, 0);
    char late[64];
    snprintf(late, sizeof late, "late ack seq 9: posted %u us after the frame end",
             (unsigned)(ACK_DELAY_RAT + 8) / 4u);
    CHECK(log_is(0, late));
    CHECK(stat(&c, S_ACKS) == 2 && stat(&c, S_ACKS_LATE) == 1);

    /* Jobs: one acked, one with no ACK, one the watchdog ends. */
    uint8_t tx[1 + 8 + sizeof pkt];
    tx[0] = 0;
    memcpy(&tx[1], joiner, 8);
    memcpy(&tx[9], pkt, sizeof pkt);
    reset_stubs();
    host_request(&c, 4, HL_TX, tx, sizeof tx);
    coord_poll(&c);
    coord_on_job_sent(&c, job_token, true);
    uint8_t ack[MAC_ENH_ACK_LEN];
    mac_build_enh_ack(ack, c.queue[0].seq, V("in.coord_eui")->data, joiner, 0);
    coord_on_rx(&c, ack, sizeof ack, -50, 0, 0);
    host_request(&c, 5, HL_TX, tx, sizeof tx);
    coord_poll(&c);
    uint8_t h = c.queue[0].handle;
    coord_on_job_sent(&c, job_token, true);
    now_ms += ACK_WAIT_MS;
    coord_poll(&c);
    char want[32] = "job ";
    want[4] = (char)('0' + h);
    strcpy(&want[5], " no-ack");
    CHECK(log_is(0, want));
    host_request(&c, 6, HL_TX, tx, sizeof tx);
    coord_poll(&c);
    h = c.queue[0].handle;
    unsigned n_tx = c.queue[0].wake_frames + 1u;
    now_ms += 1000;
    coord_poll(&c);
    want[4] = (char)('0' + h);
    /* The stub radio reports that none of the chain's TX commands ran. */
    snprintf(&want[5], sizeof want - 5, " timeout, 0/%u TX ran", n_tx);
    CHECK(log_is(1, want));
    strcpy(&want[5], " radio-error");
    CHECK(log_is(2, want));
    CHECK(stat(&c, S_JOBS_OK) == 1 && stat(&c, S_JOBS_FAILED) == 2 &&
          stat(&c, S_JOBS_TIMED_OUT) == 1);

    /* An RF error is counted and logged with its status. */
    reset_stubs();
    coord_on_radio_error(&c, 0x3803);
    CHECK(log_is(0, "rf error 0x3803") && stat(&c, S_RADIO_ERRORS) == 1);

    /* A burst is capped at LOG_PER_SECOND lines a second, but counted in
     * full; the next second logs again. */
    now_ms += 1000;
    reset_stubs();
    for (int i = 0; i < 3 * LOG_PER_SECOND; i++)
        coord_on_radio_error(&c, 0x0801);
    CHECK(logs("rf error") == LOG_PER_SECOND && stat(&c, S_RADIO_ERRORS) == 1 + 3 * LOG_PER_SECOND);
    now_ms += 1000;
    coord_on_radio_error(&c, 0x0801);
    CHECK(logs("rf error") == 1);
}

int main(int argc, char **argv)
{
    load_vectors(argc > 1 ? argv[1] : "../golden/vectors.txt");
    test_aes_fips197();
    test_crc();
    test_beacon_exchange();
    test_wakeup_and_ack();
    test_pair_frames();
    test_router_advert_frames();
    test_hostlink();
    test_coordinator_beacon_window();
    test_coordinator_key_fallback();
    test_coordinator_tx();
    test_coordinator_anchor();
    test_coordinator_tune();
    test_coordinator_job_recovery();
    test_stats_and_log();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
