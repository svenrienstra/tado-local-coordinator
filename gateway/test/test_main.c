/* Host tests for the gateway's protocol core. Datagrams and documents are
 * checked byte for byte against golden/vectors.txt (freedo's builders) and
 * golden/host_vectors.txt (the Java host's), and freedo's captured frames
 * (reference/freedo/tado/testdata) are replayed through the parsers. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coap.h"
#include "harness.h"
#include "lowpan.h"
#include "mac.h"
#include "reasm.h"
#include "tado.h"

static const char *freedo_dir;

/* One captured frame per line; NULL when the fixture is missing. */
static FILE *fixture(const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", freedo_dir, name);
    FILE *f = fopen(path, "r");
    if (!f)
        fprintf(stderr, "skipping %s: %s not found (git submodule update --init)\n", name, path);
    return f;
}

static size_t next_frame(FILE *f, uint8_t *out, size_t cap)
{
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0])
            return unhex(line, out, cap);
    }
    return 0;
}

/* --- 6LoWPAN ------------------------------------------------------------ */

/* freedo's datagrams rebuild byte for byte from their parsed parts: link-local
 * addresses from the EUIs, ports, and the UDP checksum. */
static void test_lowpan_rebuilds_freedos_datagrams(void)
{
    const uint8_t *coord = V("in.coord_eui")->data, *joiner = V("in.joiner_eui")->data;
    static const struct {
        const char *name;
        int from_joiner;
    } cases[] = {{"pair.coap", 0}, {"pair_answer.coap", 1}, {"auth_token.coap", 0}, {"time.coap", 0}};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const struct vec *v = V(cases[i].name);
        const uint8_t *src = cases[i].from_joiner ? joiner : coord;
        const uint8_t *dst = cases[i].from_joiner ? coord : joiner;
        struct lowpan_datagram d;
        CHECK(lowpan_parse(v->data, v->len, src, dst, &d));
        CHECK(d.next_header == LOWPAN_NH_UDP && d.src_port == 5683 && d.dst_port == 5683);
        uint8_t ll[16];
        lowpan_link_local(src, ll);
        CHECK(d.has_src && memcmp(d.src, ll, 16) == 0);
        uint8_t out[256];
        size_t n = lowpan_udp(out, sizeof out, src, dst, d.src_port, d.dst_port, d.payload, d.payload_len);
        CHECK_BYTES(out, n, v->data, v->len);
        CHECK(lowpan_udp(out, n - 1, src, dst, 5683, 5683, d.payload, d.payload_len) == 0);
    }

    /* The Router Advertisement to ff02::1, checksum filled in. */
    const struct vec *ra = V("ra.packet");
    struct lowpan_datagram d;
    CHECK(lowpan_parse(ra->data, ra->len, coord, NULL, &d));
    CHECK(d.next_header == LOWPAN_NH_ICMPV6 && lowpan_icmp_type(&d) == 134);
    CHECK(d.has_dst && d.dst[0] == 0xff && d.dst[1] == 0x02 && d.dst[15] == 0x01);
    uint8_t msg[256], out[256];
    memcpy(msg, d.payload, d.payload_len);
    msg[2] = msg[3] = 0;
    size_t n = lowpan_icmpv6_to_link_multicast(out, sizeof out, coord, 1, msg, d.payload_len);
    CHECK_BYTES(out, n, ra->data, ra->len);
}

static void test_lowpan_parses_an_inline_udp_header(void)
{
    /* IPHC with next header 17 inline (no NHC): the 8-byte UDP header travels as is. */
    uint8_t p[] = {0x7b, 0x33, 0x11, 0x16, 0x33, 0x16, 0x34, 0x00, 0x0c, 0x00, 0x00, 1, 2, 3, 4};
    struct lowpan_datagram d;
    CHECK(lowpan_parse(p, sizeof p, V("in.joiner_eui")->data, V("in.coord_eui")->data, &d));
    CHECK(d.next_header == LOWPAN_NH_UDP && d.src_port == 5683 && d.dst_port == 5684);
    CHECK(d.payload_len == 4 && d.payload[0] == 1);

    CHECK(!lowpan_parse(p, 5, V("in.joiner_eui")->data, NULL, &d)); /* truncated */
    uint8_t not_iphc[] = {0x41, 0x60, 0x00};
    CHECK(!lowpan_parse(not_iphc, sizeof not_iphc, V("in.joiner_eui")->data, NULL, &d));
}

/* --- CoAP ---------------------------------------------------------------- */

static void test_coap_round_trips_freedos_messages(void)
{
    const char *names[] = {"pair.coap", "pair_answer.coap", "auth_token.coap", "time.coap"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        const struct vec *v = V(names[i]);
        struct lowpan_datagram d;
        CHECK(lowpan_parse(v->data, v->len, V("in.coord_eui")->data, V("in.joiner_eui")->data, &d));
        struct coap_msg m;
        CHECK(coap_decode(d.payload, d.payload_len, &m));
        uint8_t out[256];
        size_t n = coap_encode(&m, out, sizeof out);
        CHECK_BYTES(out, n, d.payload, d.payload_len);
    }

    /* d/pair: CON POST, Content-Format 42, the session option (2048) last. */
    const struct vec *pair = V("pair.coap");
    struct lowpan_datagram d;
    lowpan_parse(pair->data, pair->len, V("in.coord_eui")->data, V("in.joiner_eui")->data, &d);
    struct coap_msg m;
    coap_decode(d.payload, d.payload_len, &m);
    char path[32];
    CHECK(m.type == COAP_CON && m.code == COAP_POST);
    CHECK(coap_path(&m, path, sizeof path) && strcmp(path, "d/pair") == 0);
    CHECK(coap_path_is(&m, "d/pair") && !coap_path_is(&m, "d") && !coap_path_is(&m, "d/pair/x"));
    CHECK(!coap_path(&m, path, 4) && strcmp(path, "d") == 0);
    const struct coap_option *s = coap_find(&m, COAP_OPT_SESSION);
    CHECK(s && s->len == 8 && memcmp(s->value, V("in.pair_session")->data, 8) == 0);
    CHECK(coap_uint(coap_find(&m, COAP_OPT_CONTENT_FORMAT)) == 42);
}

static void test_coap_encodes_options_in_order_with_extended_deltas(void)
{
    /* Added out of order, with repeats: sorted stably; delta 13+ and 269+ use
     * the extended forms, as does a 13-byte value. */
    struct coap_msg m = {.type = COAP_ACK, .code = COAP_CODE(2, 5), .mid = 0x1234, .tkl = 1, .token = {7}};
    static const uint8_t block = 0x22, sess[2] = {0xaa, 0xbb};
    static const char long_value[] = "thirteen-byte";
    coap_add_option(&m, COAP_OPT_SESSION, sess, 2);
    coap_add_option(&m, COAP_OPT_BLOCK2, &block, 1);
    coap_add_path(&m, "h/4242");
    coap_add_option(&m, COAP_OPT_URI_QUERY, long_value, 13);
    static const uint8_t body[] = {0x60, 0x20, 0x01, 0x01};
    m.payload = body;
    m.payload_len = sizeof body;
    uint8_t out[128];
    size_t n = coap_encode(&m, out, sizeof out);
    static const uint8_t want[] = {
        0x61, 0x45, 0x12, 0x34, 0x07,
        0xb1, 'h', 0x04, '4', '2', '4', '2',      /* 11, then delta 0 */
        0x4d, 0x00, 't', 'h', 'i', 'r', 't', 'e', 'e', 'n', '-', 'b', 'y', 't', 'e', /* 15 */
        0x81, 0x22,                               /* 23 */
        0xe2, 0x06, 0xdc, 0xaa, 0xbb,             /* 2048: delta 2025 = 269 + 0x06dc */
        0xff, 0x60, 0x20, 0x01, 0x01};
    CHECK_BYTES(out, n, want, sizeof want);
    CHECK(coap_encode(&m, out, n - 1) == 0);

    struct coap_msg back;
    CHECK(coap_decode(out, n, &back));
    CHECK(back.n_options == 5 && coap_path_is(&back, "h/4242") && coap_uint(coap_find(&back, COAP_OPT_BLOCK2)) == 0x22);
    CHECK(back.payload_len == 4 && back.tkl == 1 && back.token[0] == 7);

    /* Malformed: reserved nibble, truncated option, token longer than 8. */
    static const uint8_t reserved[] = {0x40, 0x01, 0, 1, 0xf0};
    static const uint8_t truncated[] = {0x40, 0x01, 0, 1, 0xb5, 'a'};
    static const uint8_t long_token[] = {0x49, 0x01, 0, 1, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    CHECK(!coap_decode(reserved, sizeof reserved, &back));
    CHECK(!coap_decode(truncated, sizeof truncated, &back));
    CHECK(!coap_decode(long_token, sizeof long_token, &back));
}

/* --- reassembly ------------------------------------------------------------ */

/* The wireless temperature sensor's PUT d/<serial>/fw/state right after joining
 * (capture of 2026-11-17): 139 uncompressed bytes, FRAG1 with 80 CoAP bytes,
 * FRAGN at 128. */
static const char FRAG1_HEX[] =
    "c08b00007e33f016330fa5a33c4003a302b1640c535532393232303830323536026677057374617465"
    "112ae806e7c4358ade308b0efdff01a00108003a0235c1003b010e003502124100390235c100"
    "36010a003c010e02100731353536";
static const char FRAGN_HEX[] = "e08b00001063356501800101014c0101";
static const uint8_t SENSOR[8] = {0x5b, 0x4b, 0x71, 0x31, 0x07, 0xc5, 0x1b, 0x00};
static const uint8_t COORD[8] = {0x1e, 0x34, 0x21, 0x75, 0x20, 0xc5, 0x1b, 0x00};

static void test_reasm_sensor_report(void)
{
    static struct reasm r;
    uint8_t f1[128], fn[32];
    size_t n1 = unhex(FRAG1_HEX, f1, sizeof f1), nn = unhex(FRAGN_HEX, fn, sizeof fn);
    const uint8_t *out;
    size_t len;
    CHECK(reasm_is_fragment(f1, n1) && reasm_is_fragment(fn, nn));

    CHECK(reasm_add(&r, SENSOR, f1, n1, 1000, &out, &len) == REASM_PENDING);
    CHECK(reasm_add(&r, SENSOR, fn, nn, 1024, &out, &len) == REASM_WHOLE);
    struct lowpan_datagram d;
    struct coap_msg m;
    CHECK(lowpan_parse(out, len, SENSOR, COORD, &d) && coap_decode(d.payload, d.payload_len, &m));
    CHECK(coap_path_is(&m, "d/SU2922080256/fw/state") && m.type == COAP_CON);
    CHECK(d.payload_len == 91); /* 139 - 48 bytes of uncompressed IPv6 + UDP headers */
    uint8_t whole[256];
    size_t whole_len = len;
    memcpy(whole, out, len);

    /* Out of order, with a MAC retransmission: the same datagram. */
    CHECK(reasm_add(&r, SENSOR, fn, nn, 2000, &out, &len) == REASM_PENDING);
    CHECK(reasm_add(&r, SENSOR, fn, nn, 2000, &out, &len) == REASM_PENDING);
    CHECK(reasm_add(&r, SENSOR, f1, n1, 2000, &out, &len) == REASM_WHOLE);
    CHECK_BYTES(out, len, whole, whole_len);

    /* The last fragment after the timeout starts a new datagram. */
    CHECK(reasm_add(&r, SENSOR, f1, n1, 3000, &out, &len) == REASM_PENDING);
    CHECK(reasm_add(&r, SENSOR, fn, nn, 3000 + REASM_TIMEOUT_MS, &out, &len) == REASM_PENDING);

    /* Another source's fragments stay apart. */
    static struct reasm r2;
    CHECK(reasm_add(&r2, SENSOR, f1, n1, 0, &out, &len) == REASM_PENDING);
    CHECK(reasm_add(&r2, COORD, fn, nn, 0, &out, &len) == REASM_PENDING);
    CHECK(reasm_add(&r2, SENSOR, fn, nn, 0, &out, &len) == REASM_WHOLE);

    /* A different fragment at an offset already held drops the datagram. */
    static struct reasm r3;
    uint8_t other[32];
    memcpy(other, fn, nn);
    other[nn - 1] ^= 0xff;
    CHECK(reasm_add(&r3, SENSOR, fn, nn, 0, &out, &len) == REASM_PENDING);
    CHECK(reasm_add(&r3, SENSOR, other, nn, 0, &out, &len) == REASM_DROPPED);
    CHECK(reasm_add(&r3, SENSOR, f1, n1, 0, &out, &len) == REASM_PENDING); /* started over */

    /* A FRAGN inside what FRAG1 stands for overlaps it. */
    static struct reasm r4;
    uint8_t early[32];
    memcpy(early, fn, nn);
    early[4] = 8; /* offset 64, inside FRAG1's 128 */
    CHECK(reasm_add(&r4, SENSOR, f1, n1, 0, &out, &len) == REASM_PENDING);
    CHECK(reasm_add(&r4, SENSOR, early, nn, 0, &out, &len) == REASM_DROPPED);

    /* An unfragmented datagram passes through. */
    static const uint8_t plain[] = {0x7e, 0x33, 0xf0, 0x16, 0x33, 0x0f, 0xa5, 0xa3, 0x3c, 0x40};
    CHECK(reasm_add(&r, SENSOR, plain, sizeof plain, 0, &out, &len) == REASM_WHOLE);
    CHECK(out == plain && len == sizeof plain);
}

/* A FRAG1 may hold the whole datagram, on any byte boundary; one that leaves
 * bytes to later fragments must end on an 8-byte unit. */
static void test_reasm_one_fragment_holds_it_all(void)
{
    static struct reasm r;
    uint8_t coap[13] = {0x40, 0x01, 0x12, 0x34, 0xb4, 't', 'i', 'm', 'e', 0xff, 1, 2, 3};
    uint8_t packet[64];
    size_t pn = lowpan_udp(packet, sizeof packet, SENSOR, COORD, 5683, 5683, coap, sizeof coap);
    size_t size = 40 + 8 + sizeof coap; /* 61: not a multiple of 8 */
    uint8_t f1[4 + 64] = {(uint8_t)(0xc0 | size >> 8), (uint8_t)size, 0, 9};
    memcpy(&f1[4], packet, pn);
    const uint8_t *out;
    size_t len;
    CHECK(reasm_add(&r, SENSOR, f1, 4 + pn, 0, &out, &len) == REASM_WHOLE);
    CHECK_BYTES(out, len, packet, pn);

    f1[1] = (uint8_t)(size + 8); /* now it claims 8 more bytes after an odd end */
    CHECK(reasm_add(&r, SENSOR, f1, 4 + pn, 0, &out, &len) == REASM_DROPPED);
}

static void test_reasm_inline_udp_header(void)
{
    /* IPHC with next header 17 inline: FRAG1 stands for 40 + 8 + 32 = 80 bytes. */
    static struct reasm r;
    uint8_t payload[80];
    memset(payload, 0x5a, sizeof payload);
    uint8_t f1[4 + 11 + 32] = {0xc0, 128, 0, 7, 0x7b, 0x33, 0x11, 0x16, 0x33, 0x16, 0x33, 0x00, 0x58, 0, 0};
    memcpy(&f1[15], payload, 32);
    uint8_t fn[5 + 48] = {0xe0, 128, 0, 7, 80 / 8};
    memcpy(&fn[5], &payload[32], 48);
    const uint8_t *out;
    size_t len;
    CHECK(reasm_add(&r, SENSOR, f1, sizeof f1, 0, &out, &len) == REASM_PENDING);
    CHECK(reasm_add(&r, SENSOR, fn, sizeof fn, 0, &out, &len) == REASM_WHOLE);
    struct lowpan_datagram d;
    CHECK(lowpan_parse(out, len, SENSOR, COORD, &d));
    CHECK_BYTES(d.payload, d.payload_len, payload, sizeof payload);
}

/* --- tado documents and resources ----------------------------------------- */

static const uint8_t *coord(void) { return V("in.coord_eui")->data; }
static const uint8_t *device(void) { return V("in.joiner_eui")->data; }

/* A request vector parsed as the coordinator receives it from the device. */
static void request(const char *name, struct lowpan_datagram *d, struct coap_msg *m)
{
    const struct vec *v = V(name);
    CHECK(lowpan_parse(v->data, v->len, device(), coord(), d) && coap_decode(d->payload, d->payload_len, m));
}

static void test_tado_documents(void)
{
    uint8_t doc[512];
    size_t n;
    static const struct tado_zone_role roles[] = {{TADO_ROLE_OWNER, 11}, {TADO_ROLE_MEMBER, 12}};
    n = tado_device_config(doc, sizeof doc, 4242, roles, 2);
    CHECK_VEC(doc, n, "doc.device_config");
    CHECK(tado_device_config(doc, sizeof doc, 0, roles, 2) == 0); /* a device applies nothing with 0 */
    CHECK(tado_device_config(doc, n - 1, 4242, roles, 2) == 0);

    n = tado_zone_with_owner(doc, sizeof doc, device());
    CHECK_VEC(doc, n, "doc.zone_with_owner");
    uint8_t tag[TADO_ETAG_LEN];
    tado_etag(doc, n, tag);
    CHECK_VEC(tag, sizeof tag, "doc.etag_zone");

    n = tado_zone_state(doc, sizeof doc, 1800, -1);
    CHECK_VEC(doc, n, "doc.zone_state");
    n = tado_zone_state(doc, sizeof doc, 1800, 2100);
    CHECK_VEC(doc, n, "doc.zone_state_overlay");

    char uri[64];
    CHECK(tado_zone_uri(uri, sizeof uri, device()));
    CHECK_VEC(uri, strlen(uri), "doc.zone_uri");
    CHECK(!tado_zone_uri(uri, 20, device()));

    /* Read back: the overlay is there; the last of a tag wins; a value running
     * past the body is malformed. */
    struct tado_report r;
    CHECK(tado_report_decode(doc, n, &r) && r.overlay_setpoint == 2100 && r.setpoint == 1800);
    static const uint8_t twice[] = {0x62, 0x40, 1, 2, 0x62, 0x40, 1, 0};
    CHECK(tado_report_decode(twice, sizeof twice, &r) && r.overlay == 0);
    static const uint8_t past[] = {0x62, 0x80, 2, 0x08};
    CHECK(!tado_report_decode(past, sizeof past, &r));
}

/* The z/s block of freedo's zone writes (home, away, boost), byte for byte. */
static void check_block(const char *name, const struct tado_zone_block *b)
{
    const struct vec *v = V(name);
    struct lowpan_datagram d;
    struct coap_msg m;
    CHECK(lowpan_parse(v->data, v->len, V("in.coord_eui")->data, V("in.joiner_eui")->data, &d) &&
          coap_decode(d.payload, d.payload_len, &m));
    uint8_t out[64];
    size_t n = tado_zone_block(out, sizeof out, b);
    CHECK_BYTES(out, n, m.payload, m.payload_len);
}

static void test_tado_zone_blocks_as_freedo_writes_them(void)
{
    /* Away at an away temperature, away and off, and home again: target changes. */
    check_block("zone.away_warm", &(struct tado_zone_block){.away = true, .on = true, .setpoint = 1500, .marker = true});
    check_block("zone.away_off", &(struct tado_zone_block){.away = true, .marker = true});
    check_block("zone.home_again", &(struct tado_zone_block){.on = true, .setpoint = 2000, .marker = true});
    /* A manual overlay restated at the switch: no marker. */
    check_block("zone.away_manual", &(struct tado_zone_block){.away = true, .on = true, .setpoint = 1500,
                                                              .overlay = TADO_OVERLAY_MANUAL, .overlay_on = true,
                                                              .overlay_setpoint = 2100});
    /* The boost: a timer overlay at 25 C, and its cancel. */
    check_block("zone.boost_on", &(struct tado_zone_block){.on = true, .setpoint = 2000, .overlay = TADO_OVERLAY_TIMER,
                                                           .overlay_on = true, .overlay_setpoint = 2500, .marker = true});
    check_block("zone.boost_cancel", &(struct tado_zone_block){.on = true, .setpoint = 2000, .marker = true});
    check_block("zone.boost_away", &(struct tado_zone_block){.away = true, .overlay = TADO_OVERLAY_TIMER,
                                                             .overlay_on = true, .overlay_setpoint = 2500,
                                                             .marker = true});
    uint8_t out[64];
    CHECK(tado_zone_block(out, 20, &(struct tado_zone_block){.on = true, .setpoint = 2000}) == 0);
}

static void test_tado_block_answers(void)
{
    uint8_t doc[512], out[256];
    size_t doc_len = tado_zone_with_owner(doc, sizeof doc, device());
    static const char *const names[][2] = {{"req.block0", "res.block0"}, {"req.block1", "res.block1"},
                                           {"req.block2", "res.block2"}, {"req.block3", "res.block3"},
                                           {"req.block_held", "res.block_held"}};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        struct lowpan_datagram d;
        struct coap_msg m;
        request(names[i][0], &d, &m);
        size_t n = tado_block_answer(out, sizeof out, &m, doc, doc_len, &d, coord(), device());
        CHECK_VEC(out, n, names[i][1]);
        CHECK(n <= MAC_UNICAST_MAX_PACKET); /* one frame each */
    }
}

static void test_tado_resources(void)
{
    uint8_t out[256];
    size_t n;
    struct lowpan_datagram d;
    struct coap_msg m;

    n = tado_pair_request(out, sizeof out, V("in.network_key")->data, 0x1234, V("in.pair_session")->data, coord(),
                          device());
    CHECK_VEC(out, n, "pair.coap");

    /* auth/token and time as freedo answers them (requests as in its builders). */
    static const uint8_t token_ab[2] = {0xaa, 0xbb};
    struct coap_msg auth = {.type = COAP_CON, .code = COAP_POST, .mid = 0x4321, .tkl = 2};
    memcpy(auth.token, token_ab, 2);
    coap_add_path(&auth, "auth/token");
    struct lowpan_datagram from = {.next_header = LOWPAN_NH_UDP, .src_port = 5683, .dst_port = 5683};
    n = tado_auth_token_answer(out, sizeof out, &auth, V("in.session_id")->data, 8, &from, coord(), device());
    CHECK_VEC(out, n, "auth_token.coap");
    CHECK(tado_auth_token_answer(out, sizeof out, &auth, V("in.session_id")->data, 3, &from, coord(), device()) == 0);

    struct coap_msg time = {.type = COAP_CON, .code = COAP_GET, .mid = 0x5555, .tkl = 1, .token = {0xcc}};
    n = tado_time_answer(out, sizeof out, &time, 1786619383u, &from, coord(), device());
    CHECK_VEC(out, n, "time.coap");

    static const uint8_t none[4] = {0};
    n = tado_router_advert(out, sizeof out, coord(), none, none);
    CHECK_VEC(out, n, "ra.packet");

    request("req.report", &d, &m);
    n = tado_changed_answer(out, sizeof out, &m, &d, coord(), device());
    CHECK_VEC(out, n, "res.changed");

    request("req.separate", &d, &m);
    n = tado_empty_ack(out, sizeof out, &m, &d, coord(), device());
    CHECK_VEC(out, n, "res.empty_ack");

    uint8_t state[64];
    size_t sn = tado_zone_state(state, sizeof state, 1800, -1);
    n = tado_zone_state_write(out, sizeof out, 11, state, sn, 0x1234, coord(), device());
    CHECK_VEC(out, n, "res.zone_state_write");
    CHECK(tado_zone_state_write(out, n - 1, 11, state, sn, 0x1234, coord(), device()) == 0);
}

/* The sensor's reports as captured on 2026-11-17 (docs/SENSOR.md). */
static void test_tado_reports(void)
{
    uint8_t b[64];
    struct tado_report r;
    size_t n;

    /* z/p: zone temperature 18.70 C, demand 100 %, humidity 54.7 %. */
    n = unhex("406002074e40a001640135020223", b, sizeof b);
    CHECK(tado_report_decode(b, n, &r));
    CHECK(r.fields == (TADO_ZONE_TEMPERATURE | TADO_DEMAND | TADO_HUMIDITY));
    CHECK(r.zone_temperature == 1870 && r.demand == 100 && r.humidity == 547);

    /* d/<serial>/sen: battery 4186 mV, 17.78 C calibrated and raw, 58.0 %, light 4. */
    n = unhex("016202105a012d0206f2012e0206f2013502024401360104", b, sizeof b);
    CHECK(tado_report_decode(b, n, &r));
    CHECK(r.battery_mv == 4186 && r.temperature == 1778 && r.temperature_raw == 1778 && r.humidity == 580);
    CHECK(r.light == 4 && (r.fields & TADO_LIGHT));

    /* h/<home>/z/11/act: demand 94 %. */
    n = unhex("40a0015e", b, sizeof b);
    CHECK(tado_report_decode(b, n, &r) && r.fields == TADO_DEMAND && r.demand == 94);

    /* The z/s echo while 20 C was set on the sensor over the zone's 18 C. */
    n = unhex("62800207d06240010262600101620002070861e0010161600101618001006020010162e00100", b, sizeof b);
    CHECK(tado_report_decode(b, n, &r));
    CHECK(r.overlay_setpoint == 2000 && r.overlay == TADO_OVERLAY_MANUAL && r.overlay_on && r.setpoint == 1800);

    /* Below zero, an unknown tag skipped, a malformed body refused. */
    n = unhex("012d02ff38beef03010203", b, sizeof b);
    CHECK(tado_report_decode(b, n, &r) && r.temperature == -200 && r.fields == TADO_TEMPERATURE);
    n = unhex("012d0206", b, sizeof b);
    CHECK(!tado_report_decode(b, n, &r));
}

/* --- freedo's captures ------------------------------------------------------ */

/* Opens one captured Data frame with the fixture key and files its packet. */
static enum reasm_result replay(struct reasm *r, const uint8_t *frame, size_t n, struct mac_data *md,
                                uint8_t scratch[MAC_MAX_FRAME], const uint8_t **out, size_t *len)
{
    if (!mac_open_data(frame, n, V("in.network_key")->data, scratch, md))
        return REASM_DROPPED;
    return reasm_add(r, md->src, md->packet, md->packet_len, 0, out, len);
}

/* Every captured frame of freedo's analyzer fixture opens, and every datagram
 * parses: CoAP over UDP, or ICMPv6. */
static void test_replays_freedos_captures(void)
{
    FILE *f = fixture("frames_69ec.txt");
    if (!f)
        return;
    static struct reasm r;
    uint8_t frame[256], scratch[MAC_MAX_FRAME];
    size_t n, frames = 0, opened = 0, coap = 0, icmp = 0, pending = 0;
    while ((n = next_frame(f, frame, sizeof frame)) > 0) {
        frames++;
        struct mac_data md;
        const uint8_t *out;
        size_t len;
        enum reasm_result res = replay(&r, frame, n, &md, scratch, &out, &len);
        if (res == REASM_DROPPED && !mac_open_data(frame, n, V("in.network_key")->data, scratch, &md))
            continue;
        opened++;
        if (res == REASM_PENDING) {
            pending++;
            continue;
        }
        struct lowpan_datagram d;
        struct coap_msg m;
        if (res == REASM_WHOLE && lowpan_parse(out, len, md.src, md.broadcast ? NULL : md.dst, &d)) {
            if (d.next_header == LOWPAN_NH_UDP && coap_decode(d.payload, d.payload_len, &m))
                coap++;
            else if (lowpan_icmp_type(&d) >= 0)
                icmp++;
        }
    }
    fclose(f);
    printf("frames_69ec: %zu frames, %zu opened, %zu CoAP, %zu ICMPv6, %zu fragments pending\n", frames,
           opened, coap, icmp, pending);
    CHECK(frames == 370 && opened == frames);
    CHECK(coap + icmp + pending == opened);

    /* Fragmented downlink and uplink: two frames each, one CoAP message with the
     * zone document's coap:// resource URIs. */
    const char *frag[] = {"frames_frag.txt", "frames_frag_uplink.txt"};
    for (size_t i = 0; i < 2; i++) {
        f = fixture(frag[i]);
        if (!f)
            continue;
        static struct reasm rf;
        memset(&rf, 0, sizeof rf);
        size_t whole = 0;
        struct coap_msg m = {0};
        while ((n = next_frame(f, frame, sizeof frame)) > 0) {
            struct mac_data md;
            const uint8_t *out;
            size_t len;
            enum reasm_result res = replay(&rf, frame, n, &md, scratch, &out, &len);
            CHECK(res != REASM_DROPPED);
            struct lowpan_datagram d;
            if (res == REASM_WHOLE && lowpan_parse(out, len, md.src, md.dst, &d) &&
                coap_decode(d.payload, d.payload_len, &m)) {
                whole++;
                CHECK(m.payload_len > 0);
            }
        }
        fclose(f);
        CHECK(whole == 1);
        CHECK(m.payload && memmem(m.payload, m.payload_len, "coap://", 7) != NULL);
    }

    /* A corrupted frame fails its MIC. */
    f = fixture("frames_corrupt.txt");
    if (f) {
        size_t rejected = 0;
        while ((n = next_frame(f, frame, sizeof frame)) > 0) {
            struct mac_data md;
            rejected += !mac_open_data(frame, n, V("in.network_key")->data, scratch, &md);
        }
        fclose(f);
        CHECK(rejected == 2);
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <golden dir> <freedo tado/testdata>\n", argv[0]);
        return 2;
    }
    char path[512];
    snprintf(path, sizeof path, "%s/vectors.txt", argv[1]);
    load_vectors(path);
    snprintf(path, sizeof path, "%s/host_vectors.txt", argv[1]);
    load_vectors(path);
    freedo_dir = argv[2];

    test_lowpan_rebuilds_freedos_datagrams();
    test_lowpan_parses_an_inline_udp_header();
    test_coap_round_trips_freedos_messages();
    test_coap_encodes_options_in_order_with_extended_deltas();
    test_reasm_sensor_report();
    test_reasm_inline_udp_header();
    test_reasm_one_fragment_holds_it_all();
    test_tado_documents();
    test_tado_block_answers();
    test_tado_zone_blocks_as_freedo_writes_them();
    test_tado_resources();
    test_tado_reports();
    run_session_tests();
    run_hlc_tests();
    run_gateway_tests();
    run_ha_tests();
    run_web_tests();
    test_replays_freedos_captures();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
