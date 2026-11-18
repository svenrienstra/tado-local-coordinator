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
#include "web.h"

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
    char payload[1536];
    bool retain;
} published[64];
static size_t n_published;
static uint32_t saved_floor, saved_channel;
static bool refuse_save;
static struct install saved; /* the installation as last saved, decoded */
static int install_saves;
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

static void gw_save_install(void *ctx, const uint8_t *blob, size_t len)
{
    (void)ctx;
    CHECK(install_decode(blob, len, &saved));
    install_saves++;
}

static const struct gateway_ops OPS = {.log = gw_log,
                                       .rng = gw_rng,
                                       .unix_time = gw_unix,
                                       .publish = gw_publish,
                                       .save_counter_floor = gw_save_floor,
                                       .save_channel = gw_save_channel,
                                       .save_install = gw_save_install};

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

/* The rooms and devices the next start() loads, as if from flash. */
static struct install inst;

/* Our identity, and no rooms or devices. */
static struct gateway_identity identity(uint32_t floor)
{
    install_empty(&inst);
    struct gateway_identity id = {.counter_floor = floor, .channel_khz = GATEWAY_CHANNEL_KHZ, .home_id = 4242};
    memcpy(id.coordinator, coordinator(), 8);
    memcpy(id.network_key, network_key(), 16);
    return id;
}

/* The sensor in room Living (zone 11), as a previous run saved it. */
static struct gateway_identity identity_with_room(uint32_t floor)
{
    struct gateway_identity id = identity(floor);
    inst.n_rooms = 1;
    inst.rooms[0] = (struct install_room){.zone = 11, .setpoint = 1800, .name = "Living"};
    inst.n_devices = 1;
    inst.devices[0] = (struct install_device){.zone = 11, .serial = "SU2922080256"};
    memcpy(inst.devices[0].eui, device(), 8);
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
    memset(&saved, 0, sizeof saved);
    install_saves = 0;
    rng_state = 7;
    fw_unsupported = false;
    clock_set = true;
    radio_stalled = false;
    coord_boot(&fw, "pin");
    gateway_init(&gw, id, &inst, &OPS, &LINK);
}

static bool pair_open_now(void) { return gw.pair == GATEWAY_PAIR_OPEN || gw.pair == GATEWAY_PAIR_JOINING; }

static void run(int times)
{
    for (int i = 0; i < times; i++)
        gateway_run(&gw, 10);
}

/* A restart: the gateway comes back from what it saved. */
static void restart_from_flash(void)
{
    struct gateway_identity id = identity(saved_floor);
    inst = saved;
    start(&id);
    run(3);
}

/* Runs until the gateway hands the radio a job after `before` jobs: an
 * anchored one waits for the device's channel sample, up to 2 s. */
static bool a_job_within(int before)
{
    for (int i = 0; i < 200 && jobs == before; i++)
        run(1);
    return jobs > before;
}

/* Runs until a setpoint change is saved (GATEWAY_SAVE_DELAY_MS). */
static void run_past_save(void)
{
    for (uint32_t i = 0; i <= GATEWAY_SAVE_DELAY_MS / 10 + 1; i++)
        run(1);
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
static void sends_from(const uint8_t *src, const uint8_t *key, bool broadcast, uint8_t seq, uint32_t counter,
                       const uint8_t *packet, size_t len)
{
    uint8_t frame[MAC_MAX_FRAME];
    size_t n = broadcast ? mac_seal_broadcast(frame, key, false, seq, src, counter, packet, len)
                         : mac_seal_unicast(frame, key, false, seq, coordinator(), src, counter, packet, len);
    CHECK(n > 0);
    coord_on_rx(&fw, frame, n, -61, fw_rat += 4000, 0);
}

static void device_sends(const uint8_t *key, bool broadcast, uint8_t seq, uint32_t counter, const uint8_t *packet,
                         size_t len)
{
    sends_from(device(), key, broadcast, seq, counter, packet, len);
}

static void coap_from(const uint8_t *src, const struct coap_msg *m, uint8_t seq, uint32_t counter)
{
    uint8_t coap[160], packet[200];
    size_t cn = coap_encode(m, coap, sizeof coap);
    size_t pn = lowpan_udp(packet, sizeof packet, src, coordinator(), 5683, 5683, coap, cn);
    sends_from(src, network_key(), false, seq, counter, packet, pn);
}

static void device_sends_coap(const struct coap_msg *m, uint8_t seq, uint32_t counter)
{
    coap_from(device(), m, seq, counter);
}

/* The CoAP message in the radio's last job, opened under key as the device
 * would; its options point into a static scratch. */
static bool radio_job_coap(const uint8_t *key, struct coap_msg *m)
{
    static uint8_t scratch[MAC_MAX_FRAME];
    struct mac_data md;
    struct lowpan_datagram d;
    *m = (struct coap_msg){0};
    return mac_open_data(job_frame, job_len, key, scratch, &md) &&
           lowpan_parse(md.packet, md.packet_len, coordinator(), device(), &d) &&
           coap_decode(d.payload, d.payload_len, m);
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
    struct gateway_identity id = identity_with_room(5000);
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
    const char *zone_config = last_on("homeassistant/sensor/tado_zone_11/temperature/config");
    CHECK(zone_config && strstr(zone_config, "\"name\":\"Living\"")); /* the room's name */
    CHECK(last_on("homeassistant/sensor/tado_zone_11/demand/config") == NULL); /* no demand yet */
    const char *zone = last_on("tado/zone/11/state");
    CHECK(zone && strcmp(zone, "{\"temperature\":18.66,\"target\":18.00,\"confirmed\":false}") == 0);
    CHECK(fw.queue_len == 1); /* and its 2.04 */

    /* The demand follows; discovery goes once per sensor, state every time. */
    size_t before = n_published;
    static const uint8_t act[] = {0x40, 0xa0, 0x01, 0x23};
    put = (struct coap_msg){.type = COAP_CON, .code = COAP_PUT, .mid = 0x2202, .payload = act,
                            .payload_len = sizeof act};
    coap_add_path(&put, "h/4242/z/11/act");
    device_sends_coap(&put, 0x31, 0x901);
    run(1);
    CHECK(strcmp(last_on("tado/zone/11/state"),
                 "{\"temperature\":18.66,\"demand\":35,\"target\":18.00,\"confirmed\":false}") == 0);
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
    struct coap_msg pair;
    CHECK(radio_job_coap(PAIRING_KEY, &pair) && coap_path_is(&pair, "d/pair"));

    return pair;
}

static void device_answers_d_pair(const struct coap_msg *pair)
{
    struct coap_msg ack = {.type = COAP_ACK, .code = COAP_CODE(2, 4), .mid = pair->mid};
    device_sends_coap(&ack, 0x41, 18);
    run(1);
}

/* The device asks for its config, as the sensor does about once a minute. */
static void device_asks_config(uint16_t mid, uint8_t seq)
{
    struct coap_msg get = {.type = COAP_CON, .code = COAP_GET, .mid = mid};
    coap_add_path(&get, "d/SU2922080256/config");
    device_sends_coap(&get, seq, 0x1000u + mid);
    run(1);
}

static int web(const char *method, const char *path, const char *body, char *out, size_t cap)
{
    return web_handle(&gw, method, path, body, strlen(body), out, cap);
}

/* As in the tado app: pair first, then put the device in a room. Until then
 * its config request goes unanswered and it is listed as waiting; once in a
 * room it gets the room's zone at its next request, and it stays there after
 * a restart. */
static void a_paired_device_waits_for_a_room_then_takes_it(void)
{
    struct gateway_identity id = identity(5000);
    start(&id);
    run(3);
    gateway_command(&gw, HA_PAIR_TOPIC, "", 0, false); /* clears a retained message */
    gateway_command(&gw, HA_PAIR_TOPIC, "1", 1, true);  /* an old one, again at reconnect */
    CHECK(!fw.pair_open && gw.pair == GATEWAY_PAIR_IDLE);
    gateway_command(&gw, HA_PAIR_TOPIC, "1", 1, false);
    CHECK(fw.pair_open && gw.pair == GATEWAY_PAIR_OPEN);
    struct coap_msg pair = joiner_gets_d_pair();
    device_answers_d_pair(&pair);
    CHECK(!pair_open_now() && !fw.pair_open && gw.pair == GATEWAY_PAIR_JOINED);
    CHECK(saved.n_devices == 1 && memcmp(saved.devices[0].eui, device(), 8) == 0 && saved.devices[0].zone == 0);
    CHECK(logged("joined: put it in a room"));

    size_t queued = fw.queue_len;
    device_asks_config(0x3101, 0x60);
    CHECK(fw.queue_len == queued); /* unanswered */
    struct gateway_device *d = gateway_device(&gw, device());
    CHECK(d && d->waiting && strcmp(d->d.serial, "SU2922080256") == 0);
    CHECK(strcmp(saved.devices[0].serial, "SU2922080256") == 0);
    CHECK(logged("waits for a room"));

    static char out[4096];
    CHECK(web("POST", "/api/rooms", "name=Living+room", out, sizeof out) == 200 && strcmp(out, "{\"zone\":1}") == 0);
    CHECK(web("POST", "/api/devices/0bb1cefa07c51b00", "zone=1", out, sizeof out) == 200);
    CHECK(saved.n_rooms == 1 && strcmp(saved.rooms[0].name, "Living room") == 0 && saved.devices[0].zone == 1);

    int jobs_before = jobs;
    device_asks_config(0x3102, 0x61);
    CHECK(!d->waiting);
    CHECK(a_job_within(jobs_before) && jobs == jobs_before + 1);
    struct coap_msg answer;
    CHECK(radio_job_coap(network_key(), &answer));
    static const struct tado_zone_role owner1 = {TADO_ROLE_OWNER, 1};
    uint8_t want[64];
    size_t wn = tado_device_config(want, sizeof want, 4242, &owner1, 1);
    CHECK(answer.code == COAP_CODE(2, 5) && answer.mid == 0x3102);
    CHECK_BYTES(answer.payload, answer.payload_len, want, wn);

    CHECK(web("GET", "/api/state", "", out, sizeof out) == 200);
    CHECK(strstr(out, "\"rooms\":[{\"zone\":1,\"name\":\"Living room\",\"setpoint\":1800,\"away_setpoint\":0,"
                     "\"boost_left\":0,\"state\":{\"target\":18.00,\"confirmed\":false}}]"));
    CHECK(strstr(out, "{\"eui\":\"0bb1cefa07c51b00\",\"serial\":\"SU2922080256\",\"zone\":1,\"waiting\":false,"));
    CHECK(strstr(out, "\"pair\":{\"state\":\"joined\",\"left\":0,\"joined\":\"0bb1cefa07c51b00\"}"));

    /* After a restart the room is served from flash. */
    restart_from_flash();
    CHECK(gw.session.zones[0].used && gw.session.zones[0].zone == 1 &&
          memcmp(gw.session.zones[0].owner, device(), 8) == 0);
}

/* One device per room until zones get members; a device leaves a room for
 * none, and a deleted room leaves its devices in none and Home Assistant. */
static void rooms_and_devices_on_the_setup_page(void)
{
    struct gateway_identity id = identity_with_room(5000);
    uint8_t other[8];
    memcpy(other, device(), 8);
    other[0] ^= 0x10;
    inst.n_devices = 2;
    inst.devices[1] = (struct install_device){.zone = 0};
    memcpy(inst.devices[1].eui, other, 8);
    start(&id);
    run(3);
    static char out[4096];

    CHECK(!gateway_assign(&gw, other, 11)); /* the sensor has it */
    CHECK(web("POST", "/api/devices/1bb1cefa07c51b00", "zone=11", out, sizeof out) == 409);
    CHECK(web("POST", "/api/devices/1bb1cefa07c51b00", "zone=7", out, sizeof out) == 409); /* no room 7 */
    CHECK(web("POST", "/api/devices/ffb1cefa07c51b00", "zone=0", out, sizeof out) == 404);
    CHECK(web("POST", "/api/devices/1bb1cefa07c51b00", "zone=x", out, sizeof out) == 400);
    CHECK(web("POST", "/api/rooms", "name=Bad%22name", out, sizeof out) == 400);
    CHECK(web("POST", "/api/rooms/12", "name=Kitchen", out, sizeof out) == 404);
    CHECK(web("GET", "/api/rooms", "", out, sizeof out) == 404);
    CHECK(web("PUT", "/api/state", "", out, sizeof out) == 405);

    /* The sensor out of its room: zone 11 is no longer served, and Home
     * Assistant no longer shows its last measure for the room. */
    static const uint8_t sen[] = {0x01, 0x2d, 0x02, 0x07, 0x4a};
    struct coap_msg report = {.type = COAP_CON, .code = COAP_PUT, .mid = 0x3300, .payload = sen,
                              .payload_len = sizeof sen};
    coap_add_path(&report, "d/SU2922080256/sen");
    device_sends_coap(&report, 0x6f, 0x1fff);
    run(1);
    CHECK(strcmp(last_on("tado/zone/11/state"), "{\"temperature\":18.66,\"target\":18.00,\"confirmed\":false}") == 0);
    CHECK(gateway_assign(&gw, device(), 0));
    CHECK(!gw.session.zones[0].used);
    run(1);
    CHECK(strcmp(last_on("tado/zone/11/state"), "{\"target\":18.00,\"confirmed\":false}") == 0);
    CHECK(gateway_assign(&gw, other, 11) && gw.session.zones[0].used &&
          memcmp(gw.session.zones[0].owner, other, 8) == 0);

    /* Rename: discovery again under the new name. A report gives the room a
     * state first, from its owner. */
    static const uint8_t act[] = {0x40, 0xa0, 0x01, 0x23};
    struct coap_msg put = {.type = COAP_CON, .code = COAP_PUT, .mid = 0x3301, .payload = act,
                           .payload_len = sizeof act};
    coap_add_path(&put, "h/4242/z/11/act");
    coap_from(other, &put, 0x70, 0x2000);
    run(1);
    CHECK(last_on("homeassistant/sensor/tado_zone_11/demand/config") != NULL);
    n_published = 0;
    CHECK(web("POST", "/api/rooms/11", "name=Study", out, sizeof out) == 200);
    run(1);
    CHECK(strstr(last_on("homeassistant/sensor/tado_zone_11/demand/config"), "\"name\":\"Study\""));

    /* Delete, after a restart (nothing says which entities went out): its
     * device goes to no room, all its entities and its state leave Home
     * Assistant. */
    restart_from_flash();
    CHECK(web("POST", "/api/rooms/11/delete", "", out, sizeof out) == 200);
    CHECK(gateway_device(&gw, other)->zone == 0 && !gw.session.zones[0].used && !gateway_room(&gw, 11));
    CHECK(strcmp(last_on("homeassistant/sensor/tado_zone_11/temperature/config"), "") == 0);
    CHECK(strcmp(last_on("homeassistant/sensor/tado_zone_11/setpoint/config"), "") == 0);
    CHECK(strcmp(last_on("homeassistant/sensor/tado_zone_11/demand/config"), "") == 0);
    CHECK(strcmp(last_on("tado/zone/11/state"), "") == 0);
    CHECK(saved.n_rooms == 0 && saved.devices[1].zone == 0);

    /* A new room does not take the deleted one's number, which its device may
     * still hold, not even after a restart. */
    restart_from_flash();
    uint8_t zone = gateway_add_room(&gw, "Kitchen");
    CHECK(zone != 0 && zone != 11 && saved.next_zone == zone + 1);
    CHECK(gateway_add_room(&gw, "Hall") == zone + 1);
}

/* Room numbers go round after 255, past the ones in use. */
static void room_numbers_come_round_after_255(void)
{
    struct gateway_identity id = identity_with_room(5000); /* zone 11 */
    inst.next_zone = 255;
    start(&id);
    run(3);
    CHECK(gateway_add_room(&gw, "Last") == 255);
    for (uint8_t z = 1; z <= 10; z++)
        gateway_delete_room(&gw, z); /* nothing there */
    CHECK(gateway_add_room(&gw, "First") == 1);
    gw.next_zone = 11;
    CHECK(gateway_add_room(&gw, "Skips") == 12); /* 11 is the sensor's */
}

/* With the device table full no window opens: a device that joined would
 * hold our key without the gateway keeping it. */
static void no_pair_window_with_the_device_table_full(void)
{
    struct gateway_identity id = identity(5000);
    inst.n_devices = INSTALL_MAX_DEVICES;
    for (size_t i = 0; i < INSTALL_MAX_DEVICES; i++)
        memset(inst.devices[i].eui, (int)(0x80 + i), 8);
    start(&id);
    run(3);
    static char out[512];
    CHECK(!gateway_pair(&gw, 120) && !fw.pair_open && logged("device table is full"));
    CHECK(web("POST", "/api/pair", "", out, sizeof out) == 409 && strstr(out, "table is full"));
}

/* M3: a setpoint from Home Assistant, as its climate entity sends it, goes to
 * the room's device in its window and is kept in flash; the device's echo is
 * what Home Assistant then shows. */
static void a_setpoint_from_home_assistant_reaches_the_device(void)
{
    struct gateway_identity id = identity_with_room(5000);
    start(&id);
    run(3);
    CHECK(last_on("homeassistant/climate/tado_zone_11/config") != NULL);

    /* Refused: out of range, not a number, a room that does not exist, and an
     * old retained command again at reconnect. */
    int jobs_before = jobs;
    gateway_command(&gw, "tado/zone/11/setpoint/set", "30", 2, false);
    gateway_command(&gw, "tado/zone/11/setpoint/set", "warm", 4, false);
    gateway_command(&gw, "tado/zone/12/setpoint/set", "21", 2, false);
    gateway_command(&gw, "tado/zone/11/setpoint/set", "21", 2, true);
    gateway_command(&gw, "tado/zone/11/setpoint/set/x", "21", 2, false);
    run(50);
    CHECK(jobs == jobs_before && gateway_room(&gw, 11)->setpoint == 1800 && logged("refused"));

    int saves = install_saves;
    gateway_command(&gw, "tado/zone/11/setpoint/set", "21.5", 4, false);
    CHECK(gateway_room(&gw, 11)->setpoint == 2150);
    CHECK(a_job_within(jobs_before)); /* on the device's channel sample */
    /* Home Assistant shows the new target at once, not yet confirmed. */
    CHECK(strstr(last_on("tado/zone/11/state"), "\"target\":21.50,\"confirmed\":false"));
    struct coap_msg write;
    CHECK(radio_job_coap(network_key(), &write) && write.code == COAP_PUT && coap_path_is(&write, "z/s"));
    uint8_t want[64];
    size_t wn = home_state(want, sizeof want, 2150, -1);
    CHECK_BYTES(write.payload, write.payload_len, want, wn);

    /* The device ACKs, then echoes the zone state, as the sensor does. */
    struct coap_msg ack = {.type = COAP_ACK, .code = COAP_CODE(2, 4), .mid = write.mid};
    device_sends_coap(&ack, 0x71, 0x2fff);
    struct coap_msg put = {.type = COAP_CON, .code = COAP_PUT, .mid = 0x3401, .payload = want, .payload_len = wn};
    coap_add_path(&put, "z/s");
    device_sends_coap(&put, 0x72, 0x3000);
    run(1);
    CHECK(strstr(last_on("tado/zone/11/state"), "\"setpoint\":21.50"));
    CHECK(strstr(last_on("tado/zone/11/state"), "\"target\":21.50,\"confirmed\":true")); /* ACKed and echoed */

    /* Saved once the changes stop, not per change. */
    CHECK(install_saves == saves);
    run_past_save();
    CHECK(install_saves == saves + 1 && saved.rooms[0].setpoint == 2150);

    /* The page sets it the same way. */
    static char out[256];
    CHECK(web("POST", "/api/rooms/11", "setpoint=19", out, sizeof out) == 200);
    CHECK(gateway_room(&gw, 11)->setpoint == 1900);
    CHECK(web("POST", "/api/rooms/11", "setpoint=26", out, sizeof out) == 400 && strstr(out, "5-25"));
    /* Nothing changes when one of two fails, not the name either. */
    CHECK(web("POST", "/api/rooms/11", "name=Kitchen&setpoint=30", out, sizeof out) == 400);
    CHECK(web("POST", "/api/rooms/11", "name=Kitchen&setpoint=21.000000000000000000000000000001", out, sizeof out) == 400);
    CHECK(web("POST", "/api/rooms/11", "name=Bad%22&setpoint=20", out, sizeof out) == 400);
    CHECK(strcmp(gateway_room(&gw, 11)->name, "Living") == 0 && gateway_room(&gw, 11)->setpoint == 1900);
    run_past_save();
    restart_from_flash();
    CHECK(gateway_room(&gw, 11)->setpoint == 1900);
}

/* The same setpoint again wakes the device only when there is something to
 * end: an overlay set on it. */
static void the_same_setpoint_again_only_ends_an_overlay(void)
{
    struct gateway_identity id = identity_with_room(5000); /* 18 C */
    start(&id);
    run(3);
    int jobs_before = jobs, saves = install_saves;
    gateway_command(&gw, "tado/zone/11/setpoint/set", "18", 2, false);
    CHECK(!a_job_within(jobs_before));

    static const uint8_t overlay[] = {0x62, 0x80, 0x02, 0x08, 0x34, 0x62, 0x40, 0x01, 0x02, 0x62, 0x60, 0x01, 0x01};
    struct coap_msg put = {.type = COAP_CON, .code = COAP_PUT, .mid = 0x3501, .payload = overlay,
                           .payload_len = sizeof overlay};
    coap_add_path(&put, "h/4242/z/11/overlay");
    device_sends_coap(&put, 0x73, 0x3100);
    CHECK(a_job_within(jobs)); /* its 2.04 */
    jobs_before = jobs;
    gateway_command(&gw, "tado/zone/11/setpoint/set", "18", 2, false);
    CHECK(a_job_within(jobs_before));
    struct coap_msg write;
    uint8_t want[64]; /* the overlay's end, a cancel: with 6440 */
    size_t wn = tado_zone_block(want, sizeof want, &(struct tado_zone_block){.on = true, .setpoint = 1800, .marker = true});
    CHECK(radio_job_coap(network_key(), &write) && coap_path_is(&write, "z/s"));
    CHECK_BYTES(write.payload, write.payload_len, want, wn);
    run_past_save();
    CHECK(install_saves == saves); /* unchanged: nothing to save */
}

/* Away from Home Assistant: each room is written with its away temperature
 * (or off), the state goes to Home Assistant, and it is kept in flash. */
static void the_home_goes_away_and_comes_back(void)
{
    struct gateway_identity id = identity_with_room(5000);
    start(&id);
    run(3);
    n_published = 0;
    gateway_announce(&gw);
    CHECK(last_on("homeassistant/switch/tado_gateway/away/config") != NULL);
    CHECK(strcmp(last_on(HA_HOME_STATE_TOPIC), "home") == 0);

    static char out[4096];
    CHECK(web("POST", "/api/rooms/11", "away_setpoint=15", out, sizeof out) == 200);
    CHECK(web("POST", "/api/rooms/11", "away_setpoint=30", out, sizeof out) == 400);
    CHECK(gateway_room(&gw, 11)->away_setpoint == 1500);
    int jobs_before = jobs;
    gateway_command(&gw, HA_HOME_SET_TOPIC, "elsewhere", 9, false);
    gateway_command(&gw, HA_HOME_SET_TOPIC, "away", 4, true); /* retained: old */
    CHECK(!gw.away && logged("home state refused"));
    gateway_command(&gw, HA_HOME_SET_TOPIC, "away", 4, false);
    CHECK(gw.away && strcmp(last_on(HA_HOME_STATE_TOPIC), "away") == 0);
    CHECK(a_job_within(jobs_before));
    struct coap_msg write;
    uint8_t want[64];
    size_t wn = tado_zone_block(want, sizeof want, &(struct tado_zone_block){.away = true, .on = true,
                                                                             .setpoint = 1500, .marker = true});
    CHECK(radio_job_coap(network_key(), &write) && coap_path_is(&write, "z/s"));
    CHECK_BYTES(write.payload, write.payload_len, want, wn);
    run_past_save();
    CHECK(saved.away && saved.rooms[0].away_setpoint == 1500);

    CHECK(web("POST", "/api/home", "away=0", out, sizeof out) == 200 && !gw.away);
    CHECK(web("POST", "/api/home", "away=maybe", out, sizeof out) == 400);
    CHECK(web("GET", "/api/state", "", out, sizeof out) == 200 && strstr(out, "\"away\":false"));
}

/* Boost from Home Assistant's preset, and its end: no device, no boost. */
static void a_boost_from_home_assistant(void)
{
    struct gateway_identity id = identity_with_room(5000);
    uint8_t zone_without = 0;
    start(&id);
    run(3);
    zone_without = gateway_add_room(&gw, "Empty");
    CHECK(!gateway_boost(&gw, zone_without, true));

    int jobs_before = jobs;
    gateway_command(&gw, "tado/zone/11/preset/set", "boost", 5, false);
    CHECK(a_job_within(jobs_before));
    struct coap_msg write;
    uint8_t want[64];
    size_t wn = tado_zone_block(want, sizeof want, &(struct tado_zone_block){
        .on = true, .setpoint = 1800, .overlay = TADO_OVERLAY_TIMER, .overlay_on = true, .overlay_setpoint = 2500,
        .marker = true});
    CHECK(radio_job_coap(network_key(), &write) && coap_path_is(&write, "z/s"));
    CHECK_BYTES(write.payload, write.payload_len, want, wn);
    uint32_t left = gateway_boost_left(&gw, 11);
    CHECK(left > 1790 && left <= 1800);
    static char out[4096];
    char want_left[32];
    snprintf(want_left, sizeof want_left, "\"boost_left\":%lu", (unsigned long)left);
    CHECK(web("GET", "/api/state", "", out, sizeof out) == 200 && strstr(out, want_left));

    gateway_command(&gw, "tado/zone/11/preset/set", "eco", 3, false);
    CHECK(logged("preset for zone 11 refused"));
    CHECK(web("POST", "/api/rooms/11", "boost=0", out, sizeof out) == 200);
    CHECK(gateway_boost_left(&gw, 11) == 0);
    CHECK(web("POST", "/api/rooms/11", "boost=0", out, sizeof out) == 409); /* none runs */
}

/* A device that does not confirm its target: unavailable in Home Assistant
 * after the stale time, available again once it confirms. Configurable, off
 * with 0, and never for a room without a device. */
static void a_room_whose_device_does_not_confirm_goes_unavailable(void)
{
    struct gateway_identity id = identity_with_room(5000);
    start(&id);
    run(3);
    CHECK(gw.stale_minutes == INSTALL_STALE_MINUTES_DEFAULT);
    static char out[4096];
    CHECK(web("POST", "/api/settings", "stale_minutes=2", out, sizeof out) == 200);
    CHECK(web("POST", "/api/settings", "stale_minutes=99999", out, sizeof out) == 400);
    CHECK(web("POST", "/api/settings", "stale_minutes=", out, sizeof out) == 400);
    uint8_t empty = gateway_add_room(&gw, "Empty"); /* no device: never stale */

    gateway_command(&gw, "tado/zone/11/setpoint/set", "21", 2, false);
    run(100); /* the write goes, unanswered */
    CHECK(!strstr(last_on("tado/zone/11/state"), "stale"));
    for (int i = 0; i < 2 * 6000 + 10; i++)
        run(1); /* two minutes */
    CHECK(strstr(last_on("tado/zone/11/state"), "\"confirmed\":false,\"stale\":true"));
    char topic[32];
    snprintf(topic, sizeof topic, "tado/zone/%u/state", empty);
    CHECK(!strstr(last_on(topic), "stale"));

    /* The device takes it after all: its echo, as at its periodic read. */
    uint8_t echo[64];
    size_t en = home_state(echo, sizeof echo, 2100, -1);
    struct coap_msg put = {.type = COAP_CON, .code = COAP_PUT, .mid = 0x3601, .payload = echo, .payload_len = en};
    coap_add_path(&put, "z/s");
    gw.session.zones[0].writing = false; /* its ACKs were lost: the gateway gave up */
    device_sends_coap(&put, 0x74, 0x3200);
    run(1);
    CHECK(strstr(last_on("tado/zone/11/state"), "\"target\":21.00,\"confirmed\":true") &&
          !strstr(last_on("tado/zone/11/state"), "stale"));

    /* 0: never. */
    CHECK(web("POST", "/api/settings", "stale_minutes=0", out, sizeof out) == 200);
    gateway_command(&gw, "tado/zone/11/setpoint/set", "22", 2, false);
    for (int i = 0; i < 3 * 6000; i++)
        run(1);
    CHECK(!strstr(last_on("tado/zone/11/state"), "stale"));
    run_past_save();
    CHECK(saved.stale_minutes == 0);
    CHECK(web("GET", "/api/state", "", out, sizeof out) == 200 && strstr(out, "\"stale_minutes\":0"));
}

/* Away with a room off: confirmed only once the device reports off. A
 * setpoint set while away is the home one: nothing goes to the device. */
static void away_off_is_confirmed_by_the_device_and_setpoints_wait_for_home(void)
{
    struct gateway_identity id = identity_with_room(5000); /* away temperature: off */
    start(&id);
    run(3);
    int jobs_before = jobs;
    gateway_set_away(&gw, true);
    CHECK(a_job_within(jobs_before));
    struct coap_msg write;
    CHECK(radio_job_coap(network_key(), &write));
    struct coap_msg ack = {.type = COAP_ACK, .code = COAP_CODE(2, 4), .mid = write.mid};
    device_sends_coap(&ack, 0x75, 0x3300);
    run(1);
    CHECK(strstr(last_on("tado/zone/11/state"), "\"confirmed\":false")); /* ACKed, not yet reported */
    uint8_t off[64];
    size_t on = tado_zone_block(off, sizeof off, &(struct tado_zone_block){.away = true});
    struct coap_msg put = {.type = COAP_CON, .code = COAP_PUT, .mid = 0x3701, .payload = off, .payload_len = on};
    coap_add_path(&put, "z/s");
    device_sends_coap(&put, 0x76, 0x3301);
    run(1);
    CHECK(strstr(last_on("tado/zone/11/state"), "\"confirmed\":true"));
    a_job_within(jobs); /* the 2.04 to that report */

    jobs_before = jobs;
    gateway_command(&gw, "tado/zone/11/setpoint/set", "21", 2, false);
    CHECK(!a_job_within(jobs_before) && gateway_room(&gw, 11)->setpoint == 2100);
    CHECK(logged("for when the home is back"));
}

/* An unconfirmed device that left its room does not make the next one stale. */
static void a_new_device_in_a_room_starts_its_own_stale_time(void)
{
    struct gateway_identity id = identity_with_room(5000);
    uint8_t other[8];
    memcpy(other, device(), 8);
    other[0] ^= 0x20;
    inst.n_devices = 2;
    memcpy(inst.devices[1].eui, other, 8);
    start(&id);
    run(3);
    gw.stale_minutes = 1;
    gateway_command(&gw, "tado/zone/11/setpoint/set", "21", 2, false);
    for (int i = 0; i < 6000 + 10; i++)
        run(1);
    CHECK(strstr(last_on("tado/zone/11/state"), "\"stale\":true"));
    CHECK(gateway_assign(&gw, device(), 0));
    run(1);
    CHECK(gateway_assign(&gw, other, 11));
    run(1);
    CHECK(!strstr(last_on("tado/zone/11/state"), "stale"));
}

/* The page's room changes: a boost with a setpoint is refused, and a refused
 * request changes nothing; settings take a whole number. */
static void the_page_refuses_contradicting_room_changes(void)
{
    struct gateway_identity id = identity_with_room(5000);
    start(&id);
    run(3);
    static char out[4096];
    CHECK(web("POST", "/api/rooms/11", "setpoint=21&boost=1", out, sizeof out) == 400);
    CHECK(web("POST", "/api/rooms/11", "name=Kitchen&boost=0", out, sizeof out) == 409); /* none runs */
    CHECK(strcmp(gateway_room(&gw, 11)->name, "Living") == 0 && gateway_room(&gw, 11)->setpoint == 1800);
    CHECK(web("POST", "/api/settings", "stale_minutes=0abc", out, sizeof out) == 400);
    CHECK(web("POST", "/api/settings", "stale_minutes=3e1", out, sizeof out) == 400);
    CHECK(gw.stale_minutes == INSTALL_STALE_MINUTES_DEFAULT);
    CHECK(web("GET", "/api/state", "", out, sizeof out) == 200 && strstr(out, "\"boost_setpoint\":2500,\"boost_minutes\":30"));
}

/* A window that ends while d/pair is out does not cut the join short, and a
 * second window does not take it over. */
static void a_join_under_way_outlasts_its_window(void)
{
    struct gateway_identity id = identity(5000);
    start(&id);
    run(3);
    CHECK(gateway_pair(&gw, 1));
    struct coap_msg pair = joiner_gets_d_pair();
    CHECK(session_pair_state(&gw.session) == SESSION_PAIR_PAIRING);
    run(120); /* past the window */
    CHECK(pair_open_now() && gw.pair == GATEWAY_PAIR_JOINING && !gateway_pair(&gw, 120));
    device_answers_d_pair(&pair);
    CHECK(gw.pair == GATEWAY_PAIR_JOINED && saved.n_devices == 1);
}

static void a_pair_window_without_a_joiner_closes_at_its_end(void)
{
    struct gateway_identity id = identity(5000);
    start(&id);
    run(3);
    CHECK(gateway_pair(&gw, 2));
    CHECK(gateway_pair_left(&gw) == 2);
    run(150);
    CHECK(pair_open_now());
    run(60);
    CHECK(!pair_open_now() && gw.pair == GATEWAY_PAIR_CLOSED && session_pair_state(&gw.session) == SESSION_PAIR_IDLE);
    CHECK(logged("no device joined") && !fw.pair_open);
    CHECK(saved.n_devices == 0);
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
    struct gateway_identity id = identity_with_room(5000);
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
    a_paired_device_waits_for_a_room_then_takes_it();
    rooms_and_devices_on_the_setup_page();
    room_numbers_come_round_after_255();
    no_pair_window_with_the_device_table_full();
    a_setpoint_from_home_assistant_reaches_the_device();
    the_same_setpoint_again_only_ends_an_overlay();
    the_home_goes_away_and_comes_back();
    a_boost_from_home_assistant();
    a_room_whose_device_does_not_confirm_goes_unavailable();
    away_off_is_confirmed_by_the_device_and_setpoints_wait_for_home();
    a_new_device_in_a_room_starts_its_own_stale_time();
    the_page_refuses_contradicting_room_changes();
    a_join_under_way_outlasts_its_window();
    a_pair_window_without_a_joiner_closes_at_its_end();
    a_busy_cc1312_is_retried_then_given_up();
    without_a_clock_there_is_no_last_seen();
}
