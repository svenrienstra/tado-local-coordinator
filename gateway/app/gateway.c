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

/* --- devices and rooms ------------------------------------------------------ */

static int device_slot(struct gateway *g, const uint8_t eui[8], bool create)
{
    int free_slot = -1;
    for (int i = 0; i < INSTALL_MAX_DEVICES; i++) {
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

struct gateway_device *gateway_device(struct gateway *g, const uint8_t eui[8])
{
    int i = device_slot(g, eui, false);
    return i < 0 ? NULL : &g->devices[i];
}

struct gateway_room *gateway_room(struct gateway *g, uint8_t zone)
{
    for (size_t i = 0; i < INSTALL_MAX_ROOMS; i++)
        if (g->rooms[i].used && g->rooms[i].zone == zone && zone != 0)
            return &g->rooms[i];
    return NULL;
}

/* The device in the room, or NULL. */
static struct gateway_device *owner_of(struct gateway *g, uint8_t zone)
{
    for (size_t i = 0; i < INSTALL_MAX_DEVICES; i++)
        if (g->devices[i].used && g->devices[i].zone == zone)
            return &g->devices[i];
    return NULL;
}

static void save_install(struct gateway *g)
{
    static struct install in; /* one task; keeps it off the stack */
    uint8_t blob[INSTALL_MAX_BLOB];
    memset(&in, 0, sizeof in);
    for (size_t i = 0; i < INSTALL_MAX_ROOMS; i++) {
        const struct gateway_room *r = &g->rooms[i];
        if (!r->used)
            continue;
        struct install_room *o = &in.rooms[in.n_rooms++];
        o->zone = r->zone;
        o->setpoint = r->setpoint;
        memcpy(o->name, r->name, sizeof o->name);
    }
    for (size_t i = 0; i < INSTALL_MAX_DEVICES; i++) {
        const struct gateway_device *d = &g->devices[i];
        if (!d->used)
            continue;
        struct install_device *o = &in.devices[in.n_devices++];
        memcpy(o->eui, d->eui, 8);
        o->zone = d->zone;
        memcpy(o->serial, d->d.serial, sizeof o->serial);
    }
    size_t n = install_encode(&in, blob, sizeof blob);
    if (n)
        g->ops.save_install(g->ops.ctx, blob, n);
}

/* Serves each room that has a device as a zone with that device as owner, and
 * no other zone. A room whose device changed starts its measure afresh. */
static void sync_session(struct gateway *g)
{
    for (size_t i = 0; i < SESSION_MAX_ZONES; i++) {
        const struct session_zone *z = &g->session.zones[i];
        if (z->used && !gateway_room(g, z->zone))
            session_drop_zone(&g->session, z->zone);
    }
    for (size_t i = 0; i < INSTALL_MAX_ROOMS; i++) {
        struct gateway_room *r = &g->rooms[i];
        if (!r->used)
            continue;
        const struct gateway_device *owner = owner_of(g, r->zone);
        const struct session_zone *z = NULL;
        for (size_t k = 0; k < SESSION_MAX_ZONES; k++)
            if (g->session.zones[k].used && g->session.zones[k].zone == r->zone)
                z = &g->session.zones[k];
        if (!owner) {
            if (z)
                session_drop_zone(&g->session, r->zone);
        } else if (!z || memcmp(z->owner, owner->eui, 8) != 0) {
            session_set_zone(&g->session, r->zone, owner->eui, r->setpoint);
            memset(&r->r, 0, sizeof r->r);
        }
    }
}

static void publish(struct gateway *g) { g->ops.publish(g->ops.ctx, g->topic, g->payload, true); }

/* Discovery for the entities that got a value since the last time, then the
 * retained state. Nothing before the device's serial is known. */
static void publish_device(struct gateway *g, struct gateway_device *dev)
{
    const struct ha_device *d = &dev->d;
    if (!d->serial[0])
        return;
    uint32_t fresh = ha_device_entities(d) & ~dev->discovered;
    for (int e = 0; e <= HA_LAST_SEEN; e++)
        if ((fresh & 1u << e) &&
            ha_device_discovery(g->topic, sizeof g->topic, g->payload, sizeof g->payload, d->serial, e))
            publish(g);
    dev->discovered |= fresh;
    if (ha_device_state_topic(g->topic, sizeof g->topic, d->serial) &&
        ha_device_state(g->payload, sizeof g->payload, d))
        publish(g);
}

static void publish_room(struct gateway *g, struct gateway_room *room)
{
    uint32_t fresh = ha_zone_entities(&room->r) & ~room->discovered;
    for (int e = HA_ZONE_TEMPERATURE; e <= HA_ZONE_DEMAND; e++)
        if ((fresh & 1u << e) && ha_zone_discovery(g->topic, sizeof g->topic, g->payload, sizeof g->payload,
                                                   room->zone, room->name, e))
            publish(g);
    room->discovered |= fresh;
    if (room->r.fields && ha_zone_state_topic(g->topic, sizeof g->topic, room->zone) &&
        ha_zone_state(g->payload, sizeof g->payload, &room->r))
        publish(g);
}

/* Takes the room's entities out of Home Assistant: an empty retained config. */
static void unpublish_room(struct gateway *g, struct gateway_room *room)
{
    for (int e = HA_ZONE_TEMPERATURE; e <= HA_ZONE_DEMAND; e++)
        if ((room->discovered & 1u << e) && ha_zone_discovery(g->topic, sizeof g->topic, g->payload,
                                                              sizeof g->payload, room->zone, room->name, e)) {
            g->payload[0] = '\0';
            publish(g);
        }
    room->discovered = 0;
}

static void publish_dirty(struct gateway *g)
{
    for (size_t i = 0; i < INSTALL_MAX_DEVICES; i++)
        if (g->devices[i].used && g->devices[i].dirty) {
            g->devices[i].dirty = false;
            publish_device(g, &g->devices[i]);
        }
    for (size_t i = 0; i < INSTALL_MAX_ROOMS; i++)
        if (g->rooms[i].used && g->rooms[i].dirty) {
            g->rooms[i].dirty = false;
            publish_room(g, &g->rooms[i]);
        }
}

static void seen(struct gateway *g, struct gateway_device *d)
{
    /* Called while the session handles g->ev, the frame that carried it. */
    d->d.has_rssi = true;
    d->d.rssi = g->ev.ev.rx.rssi;
    d->d.last_seen = g->ops.unix_time(g->ops.ctx);
}

/* Signal and time of a frame under our key, for the devices we know. */
static void on_rx(struct gateway *g, const struct link_event *ev)
{
    struct gateway_device *d = ev->rx.pairing_key ? NULL : gateway_device(g, ev->rx.src);
    if (d)
        seen(g, d);
}

/* A device that talks to us under our key holds it: it is ours, whether it
 * joined in this run or before. NULL when the table is full. */
static struct gateway_device *known(struct gateway *g, const uint8_t eui[8])
{
    int i = device_slot(g, eui, false);
    if (i >= 0)
        return &g->devices[i];
    char e[17];
    i = device_slot(g, eui, true);
    if (i < 0) {
        notice(g, "device table full: %s not kept", eui_str(eui, e));
        return NULL;
    }
    seen(g, &g->devices[i]);
    save_install(g);
    return &g->devices[i];
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

/* Learns the device's serial from its requests, and whether it waits for a
 * room: its config request goes unanswered until it has one. */
static void session_request(void *ctx, const uint8_t device[8], uint8_t code, const char *path, bool answered)
{
    struct gateway *g = ctx;
    struct gateway_device *d = known(g, device);
    if (!d)
        return;
    if (!d->d.serial[0] && serial_of(path, d->d.serial, sizeof d->d.serial)) {
        d->dirty = true;
        save_install(g);
    }
    const char *leaf = strrchr(path, '/');
    if (code == COAP_GET && strncmp(path, "d/", 2) == 0 && leaf && strcmp(leaf, "/config") == 0) {
        if (!answered && !d->waiting)
            notice(g, "%s %s waits for a room: put it in one on the setup page", d->d.serial,
                   eui_str(device, (char[17]){0}));
        d->waiting = !answered;
    }
}

static void session_report(void *ctx, const uint8_t device[8], const char *path, const struct tado_report *r)
{
    (void)path;
    struct gateway *g = ctx;
    struct gateway_device *d = known(g, device);
    if (!d)
        return;
    seen(g, d);
    ha_merge(&d->d.r, r);
    d->dirty = true;
    /* The owner's reports are its room's: measure, demand, setpoint echo. */
    struct gateway_room *room = gateway_room(g, d->zone);
    if (room) {
        struct tado_report before = room->r;
        ha_merge(&room->r, r);
        room->dirty |= memcmp(&before, &room->r, sizeof before) != 0;
    }
}

static void session_joined(void *ctx, const uint8_t device[8])
{
    struct gateway *g = ctx;
    char e[17];
    memcpy(g->pair_joined, device, 8);
    if (!known(g, device))
        return;
    notice(g, "%s joined: put it in a room on the setup page", eui_str(device, e));
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
                            .on_request = session_request,
                            .on_joined = session_joined};
    session_init(&g->session, id->coordinator, id->network_key, &s, now(g));
    session_set_home(&g->session, id->home_id, SESSION_DEFAULT_OVERLAY_MS);
    const struct install *in = &id->install;
    for (size_t i = 0; i < in->n_rooms && i < INSTALL_MAX_ROOMS; i++) {
        struct gateway_room *r = &g->rooms[i];
        r->used = true;
        r->zone = in->rooms[i].zone;
        r->setpoint = in->rooms[i].setpoint;
        memcpy(r->name, in->rooms[i].name, sizeof r->name);
    }
    for (size_t i = 0; i < in->n_devices && i < INSTALL_MAX_DEVICES; i++) {
        struct gateway_device *d = &g->devices[i];
        d->used = true;
        memcpy(d->eui, in->devices[i].eui, 8);
        d->zone = gateway_room(g, in->devices[i].zone) ? in->devices[i].zone : 0;
        memcpy(d->d.serial, in->devices[i].serial, sizeof d->d.serial);
    }
    sync_session(g);
}

/* --- rooms ------------------------------------------------------------------ */

uint8_t gateway_add_room(struct gateway *g, const char *name)
{
    if (!install_name_ok(name, strlen(name)))
        return 0;
    struct gateway_room *slot = NULL;
    for (size_t i = 0; i < INSTALL_MAX_ROOMS && !slot; i++)
        if (!g->rooms[i].used)
            slot = &g->rooms[i];
    if (!slot)
        return 0;
    unsigned zone = 1;
    while (gateway_room(g, (uint8_t)zone))
        zone++;
    memset(slot, 0, sizeof *slot);
    slot->used = true;
    slot->zone = (uint8_t)zone;
    slot->setpoint = SESSION_DEFAULT_SETPOINT;
    strcpy(slot->name, name);
    save_install(g);
    notice(g, "room %s added as zone %u", name, zone);
    return (uint8_t)zone;
}

bool gateway_rename_room(struct gateway *g, uint8_t zone, const char *name)
{
    struct gateway_room *r = gateway_room(g, zone);
    if (!r || !install_name_ok(name, strlen(name)))
        return false;
    strcpy(r->name, name);
    r->discovered = 0; /* discovery again, under the new name */
    r->dirty = true;
    save_install(g);
    return true;
}

bool gateway_delete_room(struct gateway *g, uint8_t zone)
{
    struct gateway_room *r = gateway_room(g, zone);
    if (!r)
        return false;
    for (size_t i = 0; i < INSTALL_MAX_DEVICES; i++)
        if (g->devices[i].used && g->devices[i].zone == zone)
            g->devices[i].zone = 0;
    unpublish_room(g, r);
    notice(g, "room %s (zone %u) deleted", r->name, zone);
    r->used = false;
    sync_session(g);
    save_install(g);
    return true;
}

bool gateway_assign(struct gateway *g, const uint8_t eui[8], uint8_t zone)
{
    struct gateway_device *d = gateway_device(g, eui);
    if (!d || (zone != 0 && !gateway_room(g, zone)))
        return false;
    struct gateway_device *other = zone ? owner_of(g, zone) : NULL;
    if (other && other != d)
        return false;
    if (d->zone == zone)
        return true;
    d->zone = zone;
    sync_session(g);
    save_install(g);
    char e[17];
    if (zone)
        notice(g, "%s %s put in room %s: it takes it at its next config read", d->d.serial, eui_str(eui, e),
               gateway_room(g, zone)->name);
    else
        notice(g, "%s %s taken out of its room", d->d.serial, eui_str(eui, e));
    return true;
}

/* --- pairing ---------------------------------------------------------------- */

static void close_pair_window(struct gateway *g, enum gateway_pair_state end)
{
    g->pairing = false;
    g->pair = end;
    if (g->configured && gwlink_pair_window(&g->link, 0) != HL_OK)
        notice(g, "could not close the pair window; the CC1312 closes it at its deadline");
    if (end != GATEWAY_PAIR_JOINED)
        session_close_pair_window(&g->session);
}

bool gateway_pair(struct gateway *g, uint16_t seconds)
{
    if (!g->configured || seconds == 0) {
        notice(g, "pair window not opened: the cc1312 is not configured");
        return false;
    }
    if (session_pair_state(&g->session) == SESSION_PAIR_PAIRING) {
        notice(g, "pair window not opened: a device is joining");
        return false;
    }
    int status = gwlink_pair_window(&g->link, seconds);
    if (status != HL_OK) {
        notice(g, "cc1312 refused the pair window (status %d)", status);
        return false;
    }
    session_open_pair_window(&g->session);
    g->pairing = true;
    g->pair = GATEWAY_PAIR_OPEN;
    g->pair_until_ms = now(g) + seconds * 1000u;
    notice(g, "pair window open for %u s: long-press the device's button", seconds);
    return true;
}

void gateway_close_pair(struct gateway *g)
{
    if (g->pairing && session_pair_state(&g->session) != SESSION_PAIR_PAIRING)
        close_pair_window(g, GATEWAY_PAIR_CLOSED);
}

uint32_t gateway_pair_left(struct gateway *g)
{
    int32_t left = (int32_t)(g->pair_until_ms - now(g));
    return g->pairing && left > 0 ? ((uint32_t)left + 999) / 1000 : 0;
}

static void watch_pairing(struct gateway *g)
{
    if (!g->pairing)
        return;
    switch (session_pair_state(&g->session)) {
    case SESSION_PAIR_JOINED:
        /* The joiner talks our key now: stop answering under the shared one. */
        close_pair_window(g, GATEWAY_PAIR_JOINED);
        break;
    case SESSION_PAIR_FAILED:
        notice(g, "pairing failed");
        close_pair_window(g, GATEWAY_PAIR_FAILED);
        break;
    case SESSION_PAIR_PAIRING:
        g->pair = GATEWAY_PAIR_JOINING; /* it ends in JOINED or FAILED, past the window too */
        break;
    default:
        if ((int32_t)(now(g) - g->pair_until_ms) >= 0) {
            notice(g, "pair window closed: no device joined");
            close_pair_window(g, GATEWAY_PAIR_CLOSED);
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
    (void)payload, (void)len;
    if (strcmp(topic, HA_PAIR_TOPIC) == 0)
        gateway_pair(g, GATEWAY_PAIR_WINDOW_S);
}

void gateway_announce(struct gateway *g)
{
    g->ops.publish(g->ops.ctx, HA_STATUS_TOPIC, HA_ONLINE, true);
    for (size_t i = 0; i < INSTALL_MAX_DEVICES; i++) {
        g->devices[i].discovered = 0;
        g->devices[i].dirty = true;
    }
    for (size_t i = 0; i < INSTALL_MAX_ROOMS; i++) {
        g->rooms[i].discovered = 0;
        g->rooms[i].dirty = true;
    }
    publish_dirty(g);
}
