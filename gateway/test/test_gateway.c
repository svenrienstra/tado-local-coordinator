/* The gateway application against the real CC1312 firmware (fw_peer.c): the
 * blocking host link over a byte stream, configuring the CC1312 and doing it
 * again after it rebooted, frame counter blocks, a pair command that ends with
 * a zone owner, and device reports out as Home Assistant MQTT messages. */
#include <stdio.h>
#include <stdlib.h>

#include "coap.h"
#include "coordinator.h"
#include "fw_peer.h"
#include "gateway.h"
#include "harness.h"
#include "hlframe.h"
#include "lowpan.h"
#include "mac.h"

static struct coordinator fw;
static struct gateway gw;
static uint32_t clock_ms;
static bool verbose, clock_set;
static bool radio_stalled; /* the firmware's main loop does not run: its queue never drains */

/* --- the byte stream to the firmware ---------------------------------------- */

static void link_write(void *ctx, const uint8_t *b, size_t n)
{
    (void)ctx;
    coord_on_host_bytes(&fw, b, n);
}

/* What the firmware wrote; when nothing, the wait passes and the firmware
 * runs its timers, as on the bench. */
static size_t link_read(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    (void)ctx;
    if (fw_out_len == 0) {
        clock_ms += timeout_ms ? timeout_ms : 1;
        fw_ms = clock_ms;
        if (!radio_stalled)
            coord_poll(&fw);
    }
    size_t n = fw_out_len < cap ? fw_out_len : cap;
    memcpy(buf, fw_out, n);
    memmove(fw_out, &fw_out[n], fw_out_len - n);
    fw_out_len -= n;
    return n;
}

static uint32_t link_now(void *ctx)
{
    (void)ctx;
    return clock_ms;
}

static const struct gwlink_ops LINK = {.write = link_write, .read = link_read, .now_ms = link_now};

/* --- what the gateway persists and publishes -------------------------------- */

static struct {
    char topic[96];
    char payload[768];
    bool retain;
} published[64];
static size_t n_published;
static uint32_t saved_floor, saved_channel;
static bool refuse_save;
static struct gateway_zone saved_zones[SESSION_MAX_ZONES];
static size_t n_saved_zones;
static uint64_t rng_state;

static void gw_log(void *ctx, const char *line)
{
    (void)ctx;
    if (verbose)
        printf("  [gateway] %s\n", line);
}

static void gw_rng(void *ctx, uint8_t *out, size_t n)
{
    (void)ctx;
    for (size_t i = 0; i < n; i++) {
        rng_state = rng_state * 6364136223846793005u + 1442695040888963407u;
        out[i] = (uint8_t)(rng_state >> 56);
    }
}

static uint32_t gw_unix(void *ctx)
{
    (void)ctx;
    return clock_set ? 1793966400u + clock_ms / 1000u : 0;
}

static void gw_publish(void *ctx, const char *topic, const char *payload, bool retain)
{
    (void)ctx;
    if (n_published == sizeof published / sizeof published[0])
        return;
    snprintf(published[n_published].topic, sizeof published[n_published].topic, "%s", topic);
    snprintf(published[n_published].payload, sizeof published[n_published].payload, "%s", payload);
    published[n_published].retain = retain;
    n_published++;
}

static bool gw_save_floor(void *ctx, uint32_t floor)
{
    (void)ctx;
    if (refuse_save)
        return false;
    saved_floor = floor;
    return true;
}

static void gw_save_channel(void *ctx, uint32_t khz)
{
    (void)ctx;
    saved_channel = khz;
}

static void gw_save_zones(void *ctx, const struct gateway_zone *zones, size_t n)
{
    (void)ctx;
    memcpy(saved_zones, zones, n * sizeof *zones);
    n_saved_zones = n;
}

static const struct gateway_ops OPS = {.log = gw_log,
                                       .rng = gw_rng,
                                       .unix_time = gw_unix,
                                       .publish = gw_publish,
                                       .save_counter_floor = gw_save_floor,
                                       .save_channel = gw_save_channel,
                                       .save_zones = gw_save_zones};

/* The last message on topic, or NULL. */
static const char *last_on(const char *topic)
{
    for (size_t i = n_published; i-- > 0;)
        if (strcmp(published[i].topic, topic) == 0)
            return published[i].payload;
    return NULL;
}

/* Whether a tado/gateway/log line holds text. */
static bool logged(const char *text)
{
    for (size_t i = 0; i < n_published; i++)
        if (strcmp(published[i].topic, HA_LOG_TOPIC) == 0 && strstr(published[i].payload, text))
            return true;
    return false;
}

static const uint8_t *coordinator(void) { return V("in.coord_eui")->data; }
static const uint8_t *device(void) { return V("in.joiner_eui")->data; }
static const uint8_t *network_key(void) { return V("in.network_key")->data; }

static struct gateway_identity identity(uint32_t floor)
{
    struct gateway_identity id = {.counter_floor = floor, .channel_khz = GATEWAY_CHANNEL_KHZ, .home_id = 4242};
    memcpy(id.coordinator, coordinator(), 8);
    memcpy(id.network_key, network_key(), 16);
    return id;
}

/* A fresh CC1312 that just booted, and a gateway started next to it. */
static void start(const struct gateway_identity *id)
{
    coord_init(&fw);
    fw_ms = fw_rat = clock_ms = 0;
    fw_out_len = 0;
    jobs = 0;
    n_published = 0;
    saved_floor = saved_channel = 0;
    refuse_save = false;
    n_saved_zones = 0;
    rng_state = 7;
    fw_unsupported = false;
    clock_set = true;
    radio_stalled = false;
    coord_boot(&fw, "pin");
    gateway_init(&gw, id, &OPS, &LINK);
}

static void run(int times)
{
    for (int i = 0; i < times; i++)
        gateway_run(&gw, 10);
}

/* The radio sends the job the firmware gave it. */
static bool radio_sends(void)
{
    int before = jobs;
    coord_poll(&fw);
    if (jobs == before)
        return false;
    coord_on_job_sent(&fw, job_token, true);
    return true;
}

/* A broadcast, as the session sends one through the gateway. */
static const struct link_tx BROADCAST = {.flags = LINK_TX_TRAIN_FULL, .packet = (const uint8_t *)"\x7b", .len = 1};

static int session_sends(void) { return gw.session.ops.tx(gw.session.ops.ctx, &BROADCAST); }

/* A device's datagram as it seals it to us. */
static void device_sends(const uint8_t *key, bool broadcast, uint8_t seq, uint32_t counter, const uint8_t *packet,
                         size_t len)
{
    uint8_t frame[MAC_MAX_FRAME];
    size_t n = broadcast ? mac_seal_broadcast(frame, key, false, seq, device(), counter, packet, len)
                         : mac_seal_unicast(frame, key, false, seq, coordinator(), device(), counter, packet, len);
    CHECK(n > 0);
    coord_on_rx(&fw, frame, n, -61, fw_rat += 4000, 0);
}

static void device_sends_coap(const struct coap_msg *m, uint8_t seq, uint32_t counter)
{
    uint8_t coap[160], packet[200];
    size_t cn = coap_encode(m, coap, sizeof coap);
    size_t pn = lowpan_udp(packet, sizeof packet, device(), coordinator(), 5683, 5683, coap, cn);
    device_sends(network_key(), false, seq, counter, packet, pn);
}

/* --- tests ------------------------------------------------------------------ */

/* At boot it reserves a block of counters in flash first, then configures the
 * CC1312 from the start of that block, once: the boot line the CC1312 sent
 * before our CONFIG is not a reboot. */
static void configures_the_cc1312_from_a_reserved_block(void)
{
    struct gateway_identity id = identity(5000);
    start(&id);
    run(3);
    CHECK(gw.configured && fw.configured);
    CHECK(memcmp(fw.network_key, network_key(), 16) == 0 && memcmp(fw.eui, coordinator(), 8) == 0);
    CHECK(fw.counter == 5000 && saved_floor == 5000 + GATEWAY_COUNTER_BLOCK);
    CHECK(fw.channel_khz == GATEWAY_CHANNEL_KHZ);
    CHECK(logged("configured"));
}

/* The CC1312 keeps nothing across a reset: after a reboot it gets CONFIG
 * again, from a new block, so no counter is used twice. */
static void configures_the_cc1312_again_after_it_rebooted(void)
{
    struct gateway_identity id = identity(5000);
    start(&id);
    run(3);
    fw.counter = 5100; /* it sent a hundred frames */
    coord_init(&fw);
    coord_boot(&fw, "watchdog");
    run(3);
    CHECK(fw.configured && fw.counter == 5000 + GATEWAY_COUNTER_BLOCK);
    CHECK(saved_floor == 5000 + 2 * GATEWAY_COUNTER_BLOCK);
}

/* A CC1312 that reset without its boot line reaching us refuses the next TX
 * as not configured; that also leads to a new CONFIG. */
static void a_tx_refused_as_not_configured_leads_to_a_new_config(void)
{
    struct gateway_identity id = identity(5000);
    start(&id);
    run(3);
    coord_init(&fw); /* reset, its boot line lost */
    CHECK(session_sends() < 0);
    CHECK(!gw.configured);
    run(3);
    CHECK(fw.configured && gw.configured);
}

/* The floor in flash moves on before the CC1312 reaches it. */
static void reserves_the_next_block_before_the_counters_run_out(void)
{
    struct gateway_identity id = identity(5000);
    start(&id);
    run(3);
    fw.counter = 5000 + GATEWAY_COUNTER_BLOCK / 2 - 1;
    CHECK(session_sends() >= 0);
    CHECK(saved_floor == 5000 + GATEWAY_COUNTER_BLOCK); /* not yet */
    radio_sends();
    CHECK(session_sends() >= 0);
    CHECK(saved_floor == 5000 + 2 * GATEWAY_COUNTER_BLOCK);
}

/* No counters left, or none saved: the CC1312 is not configured with
 * counters the devices may already have seen. */
static void refuses_to_configure_without_a_saved_block(void)
{
    struct gateway_identity id = identity(UINT32_MAX - GATEWAY_COUNTER_BLOCK + 1);
    start(&id);
    run(3);
    CHECK(!fw.configured && logged("used up"));

    id = identity(5000);
    start(&id);
    refuse_save = true;
    run(3);
    CHECK(!fw.configured && !gw.configured);
    refuse_save = false;
    run(150); /* tried again after a second */
    CHECK(fw.configured && fw.counter == 5000);
}

/* A CONFIG the CC1312 refuses goes again from the same reserved floor: no
 * block, and no flash write, per attempt. */
static void a_refused_config_does_not_use_up_a_block_per_attempt(void)
{
    struct gateway_identity id = identity(5000);
    start(&id);
    fw_unsupported = true;
    run(1000); /* ten seconds of attempts */
    CHECK(!gw.configured && saved_floor == 5000 + GATEWAY_COUNTER_BLOCK);
    fw_unsupported = false;
    run(150);
    CHECK(fw.configured && fw.counter == 5000 && saved_floor == 5000 + GATEWAY_COUNTER_BLOCK);
}

/* With the floor not saved, nothing goes out that a restart could repeat. */
static void sends_nothing_past_a_floor_it_could_not_save(void)
{
    struct gateway_identity id = identity(5000);
    start(&id);
    run(3);
    refuse_save = true;
    fw.counter = 5000 + GATEWAY_COUNTER_BLOCK - GATEWAY_COUNTER_MARGIN - 1;
    CHECK(session_sends() >= 0); /* the counter the gateway learns */
    radio_sends();
    CHECK(session_sends() < 0);
    CHECK(logged("not sending"));
    refuse_save = false;
    CHECK(session_sends() >= 0);
    CHECK(saved_floor == 5000 + 2 * GATEWAY_COUNTER_BLOCK);
}

/* The carrier the CC1312 retunes to is kept, within its limits, for the next
 * CONFIG. */
static void keeps_the_carrier_the_cc1312_retunes_to(void)
{
    struct gateway_identity id = identity(5000);
    start(&id);
    run(3);
    static const char line[] = "retuned to 868351 kHz (16 from CONFIG)";
    fw_out_len += hl_frame_encode(&fw_out[fw_out_len], 0x77, HL_EV_LOG, (const uint8_t *)line, sizeof line - 1);
    run(1);
    CHECK(saved_channel == 868351 && gw.id.channel_khz == 868351);

    static const char far[] = "retuned to 869000 kHz (665 from CONFIG)";
    fw_out_len += hl_frame_encode(&fw_out[fw_out_len], 0x78, HL_EV_LOG, (const uint8_t *)far, sizeof far - 1);
    run(1);
    CHECK(saved_channel == GATEWAY_CHANNEL_KHZ + GATEWAY_TUNE_MAX_KHZ);

    coord_init(&fw);
    coord_boot(&fw, "pin");
    run(3);
    CHECK(fw.channel_khz == GATEWAY_CHANNEL_KHZ + GATEWAY_TUNE_MAX_KHZ);
}

/* The sensor's report (docs/SENSOR.md) from zone 11's owner: discovery for each
 * sensor that has a value, then the retained state of device and zone. */
static void publishes_a_report_for_home_assistant(void)
{
    struct gateway_identity id = identity(5000);
    id.n_zones = 1;
    id.zones[0] = (struct gateway_zone){.zone = 11, .setpoint = 1800};
    memcpy(id.zones[0].owner, device(), 8);
    start(&id);
    run(3);
    n_published = 0;

    static const uint8_t sen[] = {0x01, 0x62, 0x02, 0x10, 0x4f, 0x01, 0x2d, 0x02, 0x07, 0x4a, 0x01, 0x2e, 0x02,
                                  0x07, 0x4b, 0x01, 0x35, 0x02, 0x02, 0x35, 0x01, 0x36, 0x01, 0x04};
    struct coap_msg put = {.type = COAP_CON, .code = COAP_PUT, .mid = 0x2201, .payload = sen,
                           .payload_len = sizeof sen};
    coap_add_path(&put, "d/SU2922080256/sen");
    device_sends_coap(&put, 0x30, 0x900);
    run(1);

    CHECK(last_on("homeassistant/sensor/tado_SU2922080256/temperature/config") != NULL);
    CHECK(last_on("homeassistant/sensor/tado_SU2922080256/humidity/config") != NULL);
    CHECK(last_on("homeassistant/sensor/tado_SU2922080256/battery/config") != NULL);
    CHECK(last_on("homeassistant/sensor/tado_SU2922080256/rssi/config") != NULL);
    CHECK(last_on("homeassistant/sensor/tado_SU2922080256/last_seen/config") != NULL);
    const char *state = last_on("tado/device/SU2922080256/state");
    CHECK(state && strcmp(state, "{\"temperature\":18.66,\"humidity\":56.5,\"battery\":4.175,\"rssi\":-61,"
                                 "\"last_seen\":\"2026-11-06T12:00:00Z\"}") == 0);
    CHECK(last_on("homeassistant/sensor/tado_zone_11/temperature/config") != NULL);
    CHECK(last_on("homeassistant/sensor/tado_zone_11/demand/config") == NULL); /* no demand yet */
    const char *zone = last_on("tado/zone/11/state");
    CHECK(zone && strcmp(zone, "{\"temperature\":18.66}") == 0);
    CHECK(fw.queue_len == 1); /* and its 2.04 */

    /* The demand follows; discovery goes once per sensor, state every time. */
    size_t before = n_published;
    static const uint8_t act[] = {0x40, 0xa0, 0x01, 0x23};
    put = (struct coap_msg){.type = COAP_CON, .code = COAP_PUT, .mid = 0x2202, .payload = act,
                            .payload_len = sizeof act};
    coap_add_path(&put, "h/4242/z/11/act");
    device_sends_coap(&put, 0x31, 0x901);
    run(1);
    CHECK(strcmp(last_on("tado/zone/11/state"), "{\"temperature\":18.66,\"demand\":35}") == 0);
    CHECK(last_on("homeassistant/sensor/tado_zone_11/demand/config") != NULL);
    size_t discovery = 0;
    for (size_t i = before; i < n_published; i++)
        discovery += strncmp(published[i].topic, "homeassistant/", 14) == 0;
    CHECK(discovery == 1);

    /* After MQTT reconnects everything goes again. */
    n_published = 0;
    gateway_announce(&gw);
    CHECK(last_on(HA_STATUS_TOPIC) && strcmp(last_on(HA_STATUS_TOPIC), HA_ONLINE) == 0);
    CHECK(last_on("homeassistant/sensor/tado_SU2922080256/temperature/config") != NULL);
    CHECK(last_on("homeassistant/sensor/tado_zone_11/demand/config") != NULL);
    CHECK(last_on("tado/zone/11/state") != NULL);
}

/* The device's side of a join up to d/pair: its beacon request (answered on
 * the CC1312), its RS under the pairing key, the RA twice, then d/pair. */
static struct coap_msg joiner_gets_d_pair(void)
{
    /* Beacon request, answered on the CC1312; the RS under the pairing key. */
    const struct vec *req = V("beacon_request.frame");
    coord_on_rx(&fw, req->data, req->len, -40, fw_rat += 4000, 0);
    run(1);
    static const uint8_t rs[] = {0x7b, 0x3b, 58, 0x02, 133, 0, 0, 0, 0, 0, 0, 0};
    device_sends(PAIRING_KEY, true, 0x40, 17, rs, sizeof rs);
    run(1);
    CHECK(fw.queue_len == 2); /* the RA twice */
    CHECK(radio_sends());
    run(1);
    CHECK(radio_sends());
    run(1);

    /* d/pair under the pairing key, with the network key in it. */
    CHECK(fw.queue_len == 1 && radio_sends());
    static uint8_t scratch[MAC_MAX_FRAME];
    struct mac_data md;
    CHECK(mac_open_data(job_frame, job_len, PAIRING_KEY, scratch, &md));
    struct lowpan_datagram d;
    struct coap_msg pair = {0}; /* its options point into the static scratch */
    CHECK(lowpan_parse(md.packet, md.packet_len, coordinator(), device(), &d) &&
          coap_decode(d.payload, d.payload_len, &pair) && coap_path_is(&pair, "d/pair"));

    return pair;
}

static void device_answers_d_pair(const struct coap_msg *pair)
{
    struct coap_msg ack = {.type = COAP_ACK, .code = COAP_CODE(2, 4), .mid = pair->mid};
    device_sends_coap(&ack, 0x41, 18);
    run(1);
}

/* The pair command names the zone; the device that joins becomes its owner,
 * kept in flash, and the CC1312 stops answering under the pairing key. */
static void a_pair_command_ends_with_the_joiner_owning_its_zone(void)
{
    struct gateway_identity id = identity(5000);
    start(&id);
    run(3);

    gateway_command(&gw, HA_PAIR_TOPIC, "zone", 4);
    gateway_command(&gw, HA_PAIR_TOPIC, "0", 1);
    CHECK(!fw.pair_open && !gw.pairing);
    gateway_command(&gw, HA_PAIR_TOPIC, " 3\n", 3);
    CHECK(fw.pair_open && gw.pairing && gw.pair_zone == 3);

    struct coap_msg pair = joiner_gets_d_pair();
    /* The 2.04 under our key. */
    device_answers_d_pair(&pair);
    CHECK(!gw.pairing && !fw.pair_open);
    CHECK(n_saved_zones == 1 && saved_zones[0].zone == 3 && memcmp(saved_zones[0].owner, device(), 8) == 0);
    CHECK(gw.session.zones[0].used && gw.session.zones[0].zone == 3);
    CHECK(logged("owner of zone 3"));
}

/* A window that ends while d/pair is out does not cut the join short, and a
 * second pair command does not take it over. */
static void a_join_under_way_outlasts_its_window(void)
{
    struct gateway_identity id = identity(5000);
    start(&id);
    run(3);
    CHECK(gateway_pair(&gw, 3, 1));
    struct coap_msg pair = joiner_gets_d_pair();
    CHECK(session_pair_state(&gw.session) == SESSION_PAIR_PAIRING);
    run(120); /* past the window */
    CHECK(gw.pairing && !gateway_pair(&gw, 7, 120) && gw.pair_zone == 3);
    device_answers_d_pair(&pair);
    CHECK(n_saved_zones == 1 && saved_zones[0].zone == 3);
}

/* Paired again into another zone, a device leaves the zone it owned. */
static void a_device_paired_into_another_zone_leaves_its_old_one(void)
{
    struct gateway_identity id = identity(5000);
    id.n_zones = 2;
    id.zones[0] = (struct gateway_zone){.zone = 3, .setpoint = 1800};
    memcpy(id.zones[0].owner, device(), 8);
    id.zones[1] = (struct gateway_zone){.zone = 4, .setpoint = 2000};
    memset(id.zones[1].owner, 0x44, 8);
    start(&id);
    run(3);
    CHECK(gateway_pair(&gw, 5, 120));
    struct coap_msg pair = joiner_gets_d_pair();
    device_answers_d_pair(&pair);
    CHECK(n_saved_zones == 2 && saved_zones[0].zone + saved_zones[1].zone == 4 + 5 &&
          saved_zones[0].zone != 3 && saved_zones[1].zone != 3);
    size_t owned = 0;
    for (size_t i = 0; i < SESSION_MAX_ZONES; i++)
        owned += gw.session.zones[i].used && memcmp(gw.session.zones[i].owner, device(), 8) == 0;
    CHECK(owned == 1);
}

static void a_pair_window_without_a_joiner_closes_at_its_end(void)
{
    struct gateway_identity id = identity(5000);
    start(&id);
    run(3);
    CHECK(gateway_pair(&gw, 4, 2));
    run(150);
    CHECK(gw.pairing);
    run(60);
    CHECK(!gw.pairing && session_pair_state(&gw.session) == SESSION_PAIR_IDLE);
    CHECK(logged("no device joined"));
    CHECK(n_saved_zones == 0);
}

/* A TX the CC1312 keeps answering busy is given up after 8 s, while reading
 * on, as its queue does not drain. */
static void a_busy_cc1312_is_retried_then_given_up(void)
{
    struct gateway_identity id = identity(5000);
    start(&id);
    run(3);
    radio_stalled = true;
    for (int i = 0; i < TX_QUEUE_LEN; i++)
        CHECK(gwlink_tx(&gw.link, &BROADCAST, NULL, NULL) >= 0);
    uint32_t from = clock_ms;
    CHECK(gwlink_tx(&gw.link, &BROADCAST, NULL, NULL) < 0);
    CHECK(clock_ms - from >= GWLINK_BUSY_GIVE_UP_MS && clock_ms - from < GWLINK_BUSY_GIVE_UP_MS + 500);
}

/* Before SNTP no time goes out, and no last_seen either. */
static void without_a_clock_there_is_no_last_seen(void)
{
    struct gateway_identity id = identity(5000);
    id.n_zones = 1;
    id.zones[0] = (struct gateway_zone){.zone = 11, .setpoint = 1800};
    memcpy(id.zones[0].owner, device(), 8);
    start(&id);
    clock_set = false;
    run(3);
    static const uint8_t sen[] = {0x01, 0x2d, 0x02, 0x07, 0x4a};
    struct coap_msg put = {.type = COAP_CON, .code = COAP_PUT, .mid = 0x2301, .payload = sen,
                           .payload_len = sizeof sen};
    coap_add_path(&put, "d/SU2922080256/sen");
    device_sends_coap(&put, 0x50, 0x910);
    run(1);
    CHECK(strcmp(last_on("tado/device/SU2922080256/state"), "{\"temperature\":18.66,\"rssi\":-61}") == 0);
    CHECK(last_on("homeassistant/sensor/tado_SU2922080256/last_seen/config") == NULL);
}

void run_gateway_tests(void)
{
    verbose = getenv("VERBOSE") != NULL;
    configures_the_cc1312_from_a_reserved_block();
    configures_the_cc1312_again_after_it_rebooted();
    a_tx_refused_as_not_configured_leads_to_a_new_config();
    reserves_the_next_block_before_the_counters_run_out();
    refuses_to_configure_without_a_saved_block();
    a_refused_config_does_not_use_up_a_block_per_attempt();
    sends_nothing_past_a_floor_it_could_not_save();
    keeps_the_carrier_the_cc1312_retunes_to();
    publishes_a_report_for_home_assistant();
    a_pair_command_ends_with_the_joiner_owning_its_zone();
    a_join_under_way_outlasts_its_window();
    a_device_paired_into_another_zone_leaves_its_old_one();
    a_pair_window_without_a_joiner_closes_at_its_end();
    a_busy_cc1312_is_retried_then_given_up();
    without_a_clock_there_is_no_last_seen();
}
