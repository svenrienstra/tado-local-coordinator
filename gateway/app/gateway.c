#include "gateway.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bytes.h"
#include "hexstr.h"

/* How long to wait before configuring a CC1312 again that did not take it. */
#define CONFIG_RETRY_MS 1000u

static const uint8_t EUI_SUFFIX[3] = {0xc5, 0x1b, 0x00};

/* --- small helpers ---------------------------------------------------------- */

static uint32_t now(struct gateway *g) { return g->link.ops.now_ms(g->link.ops.ctx); }

static void vsay(struct gateway *g, bool publish, const char *fmt, va_list ap)
{
    char line[192];
    vsnprintf(line, sizeof line, fmt, ap);
    g->ops.log(g->ops.ctx, line);
    if (publish)
        g->ops.publish(g->ops.ctx, HA_LOG_TOPIC, line, false);
}

/* A line for the console. */
static void say(struct gateway *g, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(struct gateway *g, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsay(g, false, fmt, ap);
    va_end(ap);
}

/* A line for the console and tado/gateway/log. */
static void notice(struct gateway *g, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void notice(struct gateway *g, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsay(g, true, fmt, ap);
    va_end(ap);
}

static uint32_t kept_channel(uint32_t khz)
{
    if (khz < GATEWAY_CHANNEL_KHZ - GATEWAY_TUNE_MAX_KHZ)
        return GATEWAY_CHANNEL_KHZ - GATEWAY_TUNE_MAX_KHZ;
    if (khz > GATEWAY_CHANNEL_KHZ + GATEWAY_TUNE_MAX_KHZ)
        return GATEWAY_CHANNEL_KHZ + GATEWAY_TUNE_MAX_KHZ;
    return khz;
}

/* --- frame counters --------------------------------------------------------- */

/* Reserves the next block: the floor in flash moves past it before any of it
 * is used. Returns its first counter, or 0 when none is left or the floor
 * could not be saved. */
static uint32_t reserve_counters(struct gateway *g)
{
    uint32_t start = g->id.counter_floor;
    if (start == 0) {
        notice(g, "no frame counter floor saved: not configuring the cc1312");
        return 0;
    }
    if (start > UINT32_MAX - GATEWAY_COUNTER_BLOCK) {
        notice(g, "frame counters used up (floor %lu): pair the devices to a new network key",
               (unsigned long)start);
        return 0;
    }
    if (!g->ops.save_counter_floor(g->ops.ctx, start + GATEWAY_COUNTER_BLOCK)) {
        notice(g, "could not save the frame counter floor");
        return 0;
    }
    g->id.counter_floor = start + GATEWAY_COUNTER_BLOCK;
    return start;
}

/* --- the CC1312 ------------------------------------------------------------- */

static void configure(struct gateway *g)
{
    uint16_t fw;
    int status = gwlink_ping(&g->link, &fw);
    if (status != HL_OK) {
        say(g, "cc1312 does not answer (status %d)", status);
        g->next_config_ms = now(g) + CONFIG_RETRY_MS;
        return;
    }
    /* A block per CONFIG the CC1312 may have taken, not per attempt: a refused
     * one goes again from the same floor. */
    if (g->config_floor == 0)
        g->config_floor = reserve_counters(g);
    uint32_t floor = g->config_floor;
    if (floor == 0) {
        g->next_config_ms = now(g) + CONFIG_RETRY_MS;
        return;
    }
    status = gwlink_config(&g->link, g->id.coordinator, g->id.network_key, floor, g->id.channel_khz);
    if (status != HL_OK) {
        notice(g, "cc1312 refused CONFIG (status %d)", status);
        if (status < 0)
            g->config_floor = 0; /* unanswered: it may run on that floor already */
        g->next_config_ms = now(g) + CONFIG_RETRY_MS;
        return;
    }
    g->configured = true;
    g->config_order = g->link.last_reply_order;
    g->config_floor = 0;
    g->counter_next = floor;
    notice(g, "cc1312 firmware v%u configured: carrier %lu kHz, counter floor %lu", fw,
           (unsigned long)g->id.channel_khz, (unsigned long)floor);
    if (g->pairing) {
        /* It rebooted with the window open: open it again for the time left. */
        uint32_t left = g->pair_until_ms - now(g);
        if ((int32_t)left > 0 && gwlink_pair_window(&g->link, (uint16_t)((left + 999) / 1000)) != HL_OK)
            notice(g, "could not open the pair window again");
    }
}

/* The CC1312's LOG lines: a boot after our CONFIG means it lost it; a retune
 * is kept for the next CONFIG (it stores nothing). */
static void on_cc1312_log(struct gateway *g, const struct gwlink_event *e)
{
    const char *text = e->ev.log.text; /* NUL-terminated by gwlink */
    g->ops.publish(g->ops.ctx, HA_LOG_TOPIC, text, false);
    if (strncmp(text, "boot ", 5) == 0 && e->order > g->config_order && g->configured) {
        g->configured = false;
        notice(g, "cc1312 rebooted (%s): configuring it again", text);
    }
    const char *r = strstr(text, "retuned to ");
    if (r) {
        unsigned long khz = strtoul(r + 11, NULL, 10);
        if (khz && kept_channel((uint32_t)khz) != g->id.channel_khz) {
            g->id.channel_khz = kept_channel((uint32_t)khz);
            g->ops.save_channel(g->ops.ctx, g->id.channel_khz);
        }
    }
}

/* --- devices and zones ------------------------------------------------------ */

static int device_slot(struct gateway *g, const uint8_t eui[8], bool create)
{
    int free_slot = -1;
    for (int i = 0; i < GATEWAY_MAX_DEVICES; i++) {
        if (g->devices[i].used && memcmp(g->devices[i].eui, eui, 8) == 0)
            return i;
        if (!g->devices[i].used && free_slot < 0)
            free_slot = i;
    }
    if (!create || free_slot < 0)
        return -1;
    memset(&g->devices[free_slot], 0, sizeof g->devices[free_slot]);
    g->devices[free_slot].used = true;
    memcpy(g->devices[free_slot].eui, eui, 8);
    return free_slot;
}

static void publish(struct gateway *g) { g->ops.publish(g->ops.ctx, g->topic, g->payload, true); }

/* Discovery for the entities that got a value since the last time, then the
 * retained state. Nothing before the device's serial is known. */
static void publish_device(struct gateway *g, int i)
{
    const struct ha_device *d = &g->devices[i].d;
    if (!d->serial[0])
        return;
    uint32_t fresh = ha_device_entities(d) & ~g->devices[i].discovered;
    for (int e = 0; e <= HA_LAST_SEEN; e++)
        if ((fresh & 1u << e) &&
            ha_device_discovery(g->topic, sizeof g->topic, g->payload, sizeof g->payload, d->serial, e))
            publish(g);
    g->devices[i].discovered |= fresh;
    if (ha_device_state_topic(g->topic, sizeof g->topic, d->serial) &&
        ha_device_state(g->payload, sizeof g->payload, d))
        publish(g);
}

static void publish_zone(struct gateway *g, size_t i)
{
    const struct tado_report *z = &g->zones[i].r;
    uint8_t zone = g->zones[i].zone;
    uint32_t fresh = ha_zone_entities(z) & ~g->zones[i].discovered;
    for (int e = HA_ZONE_TEMPERATURE; e <= HA_ZONE_DEMAND; e++)
        if ((fresh & 1u << e) && ha_zone_discovery(g->topic, sizeof g->topic, g->payload, sizeof g->payload, zone, e))
            publish(g);
    g->zones[i].discovered |= fresh;
    if (z->fields && ha_zone_state_topic(g->topic, sizeof g->topic, zone) &&
        ha_zone_state(g->payload, sizeof g->payload, z))
        publish(g);
}

static void publish_dirty(struct gateway *g)
{
    for (int i = 0; i < GATEWAY_MAX_DEVICES; i++)
        if (g->devices[i].used && g->devices[i].dirty) {
            g->devices[i].dirty = false;
            publish_device(g, i);
        }
    for (size_t i = 0; i < SESSION_MAX_ZONES; i++)
        if (g->zones[i].used && g->zones[i].dirty) {
            g->zones[i].dirty = false;
            publish_zone(g, i);
        }
}

/* The Home Assistant side of the session's zone i, started afresh when the
 * slot now holds another zone. */
static void reset_zone(struct gateway *g, size_t i)
{
    memset(&g->zones[i], 0, sizeof g->zones[i]);
    g->zones[i].used = true;
    g->zones[i].zone = g->session.zones[i].zone;
}

static void seen(struct gateway *g, int i, int8_t rssi, uint32_t unix)
{
    g->devices[i].d.has_rssi = true;
    g->devices[i].d.rssi = rssi;
    g->devices[i].d.last_seen = unix;
}

/* Signal and time of a frame under our key. A device gets a slot with its
 * first report, so strangers and joiners that never made it take none. */
static void on_rx(struct gateway *g, const struct link_event *ev)
{
    int i = ev->rx.pairing_key ? -1 : device_slot(g, ev->rx.src, false);
    if (i >= 0)
        seen(g, i, ev->rx.rssi, g->ops.unix_time(g->ops.ctx));
}

/* --- the session's callbacks ------------------------------------------------ */

static int session_tx(void *ctx, const struct link_tx *tx)
{
    struct gateway *g = ctx;
    if (!g->configured)
        return -1;
    /* Keep the floor in flash at least half a block ahead of the CC1312, and
     * send nothing that could pass it: after a restart those counters would
     * come again. */
    while (g->counter_next + GATEWAY_COUNTER_BLOCK / 2 >= g->id.counter_floor)
        if (reserve_counters(g) == 0)
            break;
    if (g->counter_next + GATEWAY_COUNTER_MARGIN >= g->id.counter_floor) {
        notice(g, "frame counter at the floor saved in flash: not sending");
        return -1;
    }
    uint32_t counter;
    int status;
    int handle = gwlink_tx(&g->link, tx, &counter, &status);
    if (handle < 0) {
        if (status == HL_NOT_CONFIGURED) {
            g->configured = false;
            notice(g, "cc1312 lost its CONFIG: configuring it again");
        }
        return -1;
    }
    g->counter_next = counter + 1;
    return handle;
}

static void session_log(void *ctx, const char *line)
{
    struct gateway *g = ctx;
    g->ops.log(g->ops.ctx, line);
}

static void session_rng(void *ctx, uint8_t *out, size_t n)
{
    struct gateway *g = ctx;
    g->ops.rng(g->ops.ctx, out, n);
}

static uint32_t session_unix(void *ctx)
{
    struct gateway *g = ctx;
    return g->ops.unix_time(g->ops.ctx);
}

/* d/<serial>/<leaf>: the serial; false for other paths. */
static bool serial_of(const char *path, char *out, size_t cap)
{
    if (strncmp(path, "d/", 2) != 0)
        return false;
    const char *end = strchr(path + 2, '/');
    size_t n = end ? (size_t)(end - (path + 2)) : 0;
    if (n >= cap || !ha_serial_ok(path + 2, n))
        return false;
    memcpy(out, path + 2, n);
    out[n] = '\0';
    return true;
}

static void session_report(void *ctx, const uint8_t device[8], const char *path, const struct tado_report *r)
{
    struct gateway *g = ctx;
    int i = device_slot(g, device, true);
    if (i < 0) {
        char e[17];
        notice(g, "device table full: %s not published", eui_str(device, e));
        return;
    }
    /* Called while the session handles g->ev, the frame that carried it. */
    seen(g, i, g->ev.ev.rx.rssi, g->ops.unix_time(g->ops.ctx));
    struct ha_device *d = &g->devices[i].d;
    if (!d->serial[0])
        serial_of(path, d->serial, sizeof d->serial);
    ha_merge(&d->r, r);
    g->devices[i].dirty = true;
    /* An owner's reports are its zone's: measure, demand, setpoint echo. */
    for (size_t z = 0; z < SESSION_MAX_ZONES; z++) {
        const struct session_zone *sz = &g->session.zones[z];
        if (!sz->used || memcmp(sz->owner, device, 8) != 0)
            continue;
        if (!g->zones[z].used || g->zones[z].zone != sz->zone)
            reset_zone(g, z);
        struct tado_report before = g->zones[z].r;
        ha_merge(&g->zones[z].r, r);
        g->zones[z].dirty |= memcmp(&before, &g->zones[z].r, sizeof before) != 0;
    }
}

/* The session's zones, to flash. */
static void save_zones(struct gateway *g)
{
    struct gateway_zone zones[SESSION_MAX_ZONES];
    size_t n = 0;
    for (size_t i = 0; i < SESSION_MAX_ZONES; i++) {
        const struct session_zone *z = &g->session.zones[i];
        if (!z->used)
            continue;
        zones[n] = (struct gateway_zone){.zone = z->zone, .setpoint = z->setpoint};
        memcpy(zones[n++].owner, z->owner, 8);
    }
    g->ops.save_zones(g->ops.ctx, zones, n);
}

/* The device that joined our window owns the zone the pair command named. */
static void session_joined(void *ctx, const uint8_t device[8])
{
    struct gateway *g = ctx;
    char e[17];
    if (!g->pairing)
        return;
    uint16_t setpoint = SESSION_DEFAULT_SETPOINT;
    for (size_t i = 0; i < SESSION_MAX_ZONES; i++) {
        const struct session_zone *z = &g->session.zones[i];
        if (!z->used)
            continue;
        if (z->zone == g->pair_zone) {
            setpoint = z->setpoint;
        } else if (memcmp(z->owner, device, 8) == 0) {
            /* A device owns one zone: paired again, it leaves the one it had. */
            notice(g, "%s leaves zone %u", eui_str(device, e), z->zone);
            session_drop_zone(&g->session, z->zone);
            g->zones[i].used = false;
        }
    }
    if (!session_set_zone(&g->session, g->pair_zone, device, setpoint)) {
        notice(g, "%s joined, but the zone table is full", eui_str(device, e));
        save_zones(g);
        return;
    }
    for (size_t i = 0; i < SESSION_MAX_ZONES; i++)
        if (g->session.zones[i].used && g->session.zones[i].zone == g->pair_zone)
            reset_zone(g, i); /* a new owner's measure */
    save_zones(g);
    notice(g, "%s joined: owner of zone %u", eui_str(device, e), g->pair_zone);
}

/* --- setup ------------------------------------------------------------------ */

void gateway_new_identity(struct gateway_identity *id, void (*rng)(void *ctx, uint8_t *out, size_t n), void *ctx)
{
    memset(id, 0, sizeof *id);
    rng(ctx, id->coordinator, 5);
    memcpy(&id->coordinator[5], EUI_SUFFIX, 3);
    rng(ctx, id->network_key, sizeof id->network_key);
    uint8_t b[4];
    rng(ctx, b, sizeof b);
    /* The home in every h/<home>/... path; never 0. As the Java host draws it. */
    id->home_id = 1 + get32(b) % (0x7fffffffu - 1);
    id->counter_floor = 1;
    id->channel_khz = GATEWAY_CHANNEL_KHZ;
}

void gateway_init(struct gateway *g, const struct gateway_identity *id, const struct gateway_ops *ops,
                  const struct gwlink_ops *link_ops)
{
    memset(g, 0, sizeof *g);
    g->ops = *ops;
    g->id = *id;
    g->id.channel_khz = kept_channel(id->channel_khz);
    uint8_t seq;
    ops->rng(ops->ctx, &seq, 1);
    gwlink_init(&g->link, link_ops, seq);
    struct session_ops s = {.ctx = g,
                            .tx = session_tx,
                            .log = session_log,
                            .rng = session_rng,
                            .unix_time = session_unix,
                            .on_report = session_report,
                            .on_joined = session_joined};
    session_init(&g->session, id->coordinator, id->network_key, &s, now(g));
    session_set_home(&g->session, id->home_id, SESSION_DEFAULT_OVERLAY_MS);
    for (size_t i = 0; i < id->n_zones; i++)
        session_set_zone(&g->session, id->zones[i].zone, id->zones[i].owner, id->zones[i].setpoint);
}

/* --- pairing ---------------------------------------------------------------- */

static void close_pair_window(struct gateway *g, bool keep_session_state)
{
    g->pairing = false;
    if (g->configured && gwlink_pair_window(&g->link, 0) != HL_OK)
        notice(g, "could not close the pair window; the CC1312 closes it at its deadline");
    if (!keep_session_state)
        session_close_pair_window(&g->session);
}

bool gateway_pair(struct gateway *g, uint8_t zone, uint16_t seconds)
{
    if (!g->configured || seconds == 0) {
        notice(g, "pair window for zone %u not opened: the cc1312 is not configured", zone);
        return false;
    }
    if (session_pair_state(&g->session) == SESSION_PAIR_PAIRING) {
        notice(g, "pair window for zone %u not opened: a device is joining zone %u", zone, g->pair_zone);
        return false;
    }
    int status = gwlink_pair_window(&g->link, seconds);
    if (status != HL_OK) {
        notice(g, "cc1312 refused the pair window (status %d)", status);
        return false;
    }
    session_open_pair_window(&g->session);
    g->pairing = true;
    g->pair_zone = zone;
    g->pair_until_ms = now(g) + seconds * 1000u;
    notice(g, "pair window open for %u s for zone %u: long-press the device's button", seconds, zone);
    return true;
}

static void watch_pairing(struct gateway *g)
{
    if (!g->pairing)
        return;
    switch (session_pair_state(&g->session)) {
    case SESSION_PAIR_JOINED:
        /* The joiner talks our key now: stop answering under the shared one. */
        close_pair_window(g, true);
        break;
    case SESSION_PAIR_FAILED:
        notice(g, "pairing for zone %u failed", g->pair_zone);
        close_pair_window(g, false);
        break;
    case SESSION_PAIR_PAIRING:
        break; /* d/pair is out: it ends in JOINED or FAILED, past the window too */
    default:
        if ((int32_t)(now(g) - g->pair_until_ms) >= 0) {
            notice(g, "pair window for zone %u closed: no device joined", g->pair_zone);
            close_pair_window(g, false);
        }
    }
}

/* --- the loop --------------------------------------------------------------- */

void gateway_run(struct gateway *g, uint32_t timeout_ms)
{
    if (!g->configured && (int32_t)(now(g) - g->next_config_ms) >= 0)
        configure(g);
    gwlink_poll(&g->link, timeout_ms);
    /* Handling an event can send, and sending reads: drain what that queues too. */
    while (gwlink_next_event(&g->link, &g->ev)) {
        if (g->ev.ev.type == LINK_EV_LOG)
            on_cc1312_log(g, &g->ev);
        else if (g->ev.ev.type == LINK_EV_RX)
            on_rx(g, &g->ev.ev);
        session_on_event(&g->session, &g->ev.ev, now(g));
    }
    session_tick(&g->session, now(g));
    watch_pairing(g);
    publish_dirty(g);
}

void gateway_command(struct gateway *g, const char *topic, const char *payload, size_t len)
{
    if (strcmp(topic, HA_PAIR_TOPIC) != 0)
        return;
    char buf[8];
    while (len && (*payload == ' ' || *payload == '\n'))
        payload++, len--;
    while (len && (payload[len - 1] == ' ' || payload[len - 1] == '\n' || payload[len - 1] == '\r'))
        len--;
    unsigned long zone = 0;
    char *end = NULL;
    if (len > 0 && len < sizeof buf) {
        memcpy(buf, payload, len);
        buf[len] = '\0';
        zone = strtoul(buf, &end, 10);
    }
    if (!end || *end != '\0' || zone < 1 || zone > 255) {
        notice(g, "pair command: expected a zone number 1-255");
        return;
    }
    gateway_pair(g, (uint8_t)zone, GATEWAY_PAIR_WINDOW_S);
}

void gateway_announce(struct gateway *g)
{
    g->ops.publish(g->ops.ctx, HA_STATUS_TOPIC, HA_ONLINE, true);
    for (int i = 0; i < GATEWAY_MAX_DEVICES; i++) {
        g->devices[i].discovered = 0;
        g->devices[i].dirty = true;
    }
    for (size_t i = 0; i < SESSION_MAX_ZONES; i++) {
        g->zones[i].discovered = 0;
        g->zones[i].dirty = true;
    }
    publish_dirty(g);
}
