/* Host tests for the join probe's device role. No hardware: the joiner's
 * frames are checked against the same MAC primitives the coordinator and the
 * real bridge use, and against the real captured frames in golden/vectors.txt. */
#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "joiner.h"
#include "mac.h"

/* The fixed pairing key, as a device and every coordinator hold it. */
static const uint8_t PK[16] = {'t', 'a', 'd', 'o', ' ', 'p', 'a', 'i',
                               'r', 'i', 'n', 'g', ' ', 'k', 'e', 'y'};

/* The joiner rebuilds the real captured beacon request byte for byte. */
static void test_beacon_request(void)
{
    const struct vec *want = V("beacon_request.frame");
    const uint8_t *joiner_eui = V("in.joiner_eui")->data;

    struct joiner j;
    joiner_init(&j, joiner_eui, PK, 16); /* the captured frame's counter */
    uint8_t out[MAC_BEACON_REQ_LEN];
    size_t n = joiner_beacon_request(&j, 0x7b, out); /* the captured frame's seq */
    CHECK(n == (size_t)want->len && memcmp(out, want->data, n) == 0);
    CHECK(j.counter == 17); /* bumped once */
}

/* The joiner recognises the Enhanced Beacon the coordinator answers with. */
static void test_reads_beacon(void)
{
    const struct vec *beacon = V("enh_beacon.frame");
    const uint8_t *joiner_eui = V("in.joiner_eui")->data;
    const uint8_t *coord_eui = V("in.coord_eui")->data;

    struct joiner j;
    joiner_init(&j, joiner_eui, PK, 0);
    struct joiner_rx rx;
    CHECK(joiner_on_rx(&j, beacon->data, beacon->len, &rx));
    CHECK(rx.kind == JOINER_RX_BEACON);
    CHECK(memcmp(rx.src, coord_eui, 8) == 0);
    CHECK(rx.seq == 0x59 && rx.counter == 13537);

    /* A beacon to a different EUI is not for us. */
    struct joiner other;
    uint8_t stranger[8] = {0x02, 0, 0, 0, 0, 0xc5, 0x1b, 0x00};
    joiner_init(&other, stranger, PK, 0);
    CHECK(!joiner_on_rx(&other, beacon->data, beacon->len, &rx));
    CHECK(rx.kind == JOINER_RX_OTHER);
}

/* The joiner opens a d/pair addressed to it and hands back the datagram; it
 * rejects one sealed under the wrong key or aimed elsewhere. */
static void test_opens_pair(void)
{
    const uint8_t *joiner_eui = V("in.joiner_eui")->data;
    const uint8_t *coord_eui = V("in.coord_eui")->data;
    const uint8_t *netkey = V("in.network_key")->data;

    /* A stand-in d/pair payload: the joiner is MAC-only and does not decode
     * CoAP, so any datagram proves the open-and-surface path. */
    static const uint8_t datagram[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99};
    uint8_t frame[MAC_MAX_FRAME];
    size_t n = mac_seal_unicast(frame, PK, false, 0x30, joiner_eui, coord_eui, 0x100,
                                datagram, sizeof datagram);
    CHECK(n > 0);

    struct joiner j;
    joiner_init(&j, joiner_eui, PK, 0);
    struct joiner_rx rx;
    CHECK(joiner_on_rx(&j, frame, n, &rx));
    CHECK(rx.kind == JOINER_RX_PAIR);
    CHECK(memcmp(rx.src, coord_eui, 8) == 0 && rx.seq == 0x30 && rx.counter == 0x100);
    CHECK(rx.packet_len == sizeof datagram && memcmp(rx.packet, datagram, sizeof datagram) == 0);

    /* Same frame sealed under the network key: the joiner cannot open it. */
    n = mac_seal_unicast(frame, netkey, false, 0x31, joiner_eui, coord_eui, 0x101,
                         datagram, sizeof datagram);
    CHECK(!joiner_on_rx(&j, frame, n, &rx));

    /* A d/pair to a different EUI: not for us. */
    uint8_t stranger[8] = {0x02, 0, 0, 0, 0, 0xc5, 0x1b, 0x00};
    n = mac_seal_unicast(frame, PK, false, 0x32, stranger, coord_eui, 0x102,
                         datagram, sizeof datagram);
    CHECK(!joiner_on_rx(&j, frame, n, &rx));
}

/* A Router Solicitation we seal under the pairing key opens again as the same
 * broadcast datagram (what the coordinator / bridge will see). */
static void test_seal_roundtrip(void)
{
    const uint8_t *joiner_eui = V("in.joiner_eui")->data;
    static const uint8_t rs[] = {0x60, 0x00, 0x00, 0x00, 0x00, 0x08, 0x3a, 0xff};

    struct joiner j;
    joiner_init(&j, joiner_eui, PK, 7);
    uint8_t frame[MAC_MAX_FRAME];
    size_t n = joiner_seal(&j, 0x40, rs, sizeof rs, frame);
    CHECK(n > 0 && j.counter == 8);

    uint8_t scratch[MAC_MAX_FRAME];
    struct mac_data d;
    CHECK(mac_open_data(frame, n, PK, scratch, &d));
    CHECK(d.broadcast && memcmp(d.src, joiner_eui, 8) == 0 && d.counter == 7);
    CHECK(d.packet_len == sizeof rs && memcmp(d.packet, rs, sizeof rs) == 0);
}

int main(int argc, char **argv)
{
    load_vectors(argc > 1 ? argv[1] : "../golden/vectors.txt");
    test_beacon_request();
    test_reads_beacon();
    test_opens_pair();
    test_seal_roundtrip();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
