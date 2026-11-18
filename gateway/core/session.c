#include "session.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "bytes.h"
#include "coap.h"
#include "hexstr.h"
#include "lowpan.h"
#include "tado.h"

#define ICMP_ROUTER_SOLICITATION 133

static const uint8_t NO_IPV4[4];

/* --- small helpers ---------------------------------------------------------- */

static void say(struct session *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void say(struct session *s, const char *fmt, ...)
{
    char line[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    s->ops.log(s->ops.ctx, line);
}

static bool elapsed(uint32_t now, uint32_t stamp, uint32_t interval) { return now - stamp >= interval; }

static uint32_t rand32(struct session *s)
{
    uint8_t b[4];
    s->ops.rng(s->ops.ctx, b, sizeof b);
    return get32(b);
}

static int send(struct session *s, bool unicast, const uint8_t dst[8], uint8_t flags, const uint8_t *packet,
                size_t len)
{
    struct link_tx tx = {.flags = flags, .unicast = unicast, .packet = packet, .len = len};
    if (unicast)
        memcpy(tx.dst, dst, 8);
    int handle = s->ops.tx(s->ops.ctx, &tx);
    if (handle < 0)
        say(s, "the CC1312 refused a frame");
    return handle;
}

static void schedule_next_ra(struct session *s, uint32_t now)
{
    uint32_t spread = SESSION_RA_INTERVAL_MAX_MS - SESSION_RA_INTERVAL_MIN_MS;
    s->next_ra_ms = now + SESSION_RA_INTERVAL_MIN_MS + rand32(s) % (spread + 1);
}

static bool ra_allowed(const struct session *s, uint32_t now)
{
    return !s->ra_ended_once || elapsed(now, s->ra_ended_ms, SESSION_RA_MIN_GAP_MS);
}

static bool pair_window_open(const struct session *s)
{
    return s->pair == SESSION_PAIR_WAITING || s->pair == SESSION_PAIR_PAIRING;
}

static struct session_zone *zone_find(struct session *s, unsigned zone)
{
    for (size_t i = 0; i < SESSION_MAX_ZONES; i++)
        if (s->zones[i].used && s->zones[i].zone == zone)
            return &s->zones[i];
    return NULL;
}

/* --- setup ------------------------------------------------------------------ */

void session_init(struct session *s, const uint8_t coordinator[8], const uint8_t network_key[16],
                  const struct session_ops *ops, uint32_t now_ms)
{
    memset(s, 0, sizeof *s);
    memcpy(s->coordinator, coordinator, 8);
    memcpy(s->network_key, network_key, 16);
    s->ops = *ops;
    s->overlay_ms = SESSION_DEFAULT_OVERLAY_MS;
    s->ra_handle = s->pair_handle = -1;
    s->next_mid = (uint16_t)rand32(s);
    schedule_next_ra(s, now_ms);
}

void session_set_home(struct session *s, uint32_t home_id, uint32_t overlay_ms)
{
    s->home_id = home_id;
    s->overlay_ms = overlay_ms;
}

bool session_set_zone(struct session *s, uint8_t zone, const uint8_t owner[8], uint16_t setpoint)
{
    struct session_zone *z = zone_find(s, zone);
    for (size_t i = 0; !z && i < SESSION_MAX_ZONES; i++)
        if (!s->zones[i].used)
            z = &s->zones[i];
    if (!z)
        return false;
    memset(z, 0, sizeof *z);
    z->used = true;
    z->zone = zone;
    memcpy(z->owner, owner, 8);
    z->setpoint = setpoint;
    return true;
}

void session_drop_zone(struct session *s, uint8_t zone)
{
    struct session_zone *z = zone_find(s, zone);
    if (z)
        z->used = false;
}

void session_open_pair_window(struct session *s)
{
    s->pair = SESSION_PAIR_WAITING;
    s->has_joiner = false;
    s->pair_attempts = 0;
    s->pair_deadline_set = false;
}

void session_close_pair_window(struct session *s)
{
    s->pair = SESSION_PAIR_IDLE;
    s->has_joiner = false;
    s->pair_deadline_set = false;
}

enum session_pair_state session_pair_state(const struct session *s) { return s->pair; }

void session_id(const uint8_t device[8], uint8_t out[8]) { tado_etag(device, 8, out); }

/* --- pairing ---------------------------------------------------------------- */

static void pair_left_radio(struct session *s, uint32_t now)
{
    s->pair_handle = -1;
    s->pair_deadline_set = true;
    s->pair_deadline_ms = now + SESSION_PAIR_ANSWER_TIMEOUT_MS;
}

static void send_pair(struct session *s, uint32_t now)
{
    uint8_t session[8], packet[SESSION_MAX_PACKET];
    s->ops.rng(s->ops.ctx, session, sizeof session);
    uint16_t mid = s->next_mid++;
    s->pair_mids[s->pair_attempts % SESSION_PAIR_ATTEMPTS] = mid;
    s->pair_attempts++;
    s->pair = SESSION_PAIR_PAIRING;
    size_t n = tado_pair_request(packet, sizeof packet, s->network_key, mid, session, s->coordinator, s->joiner);
    s->pair_handle = send(s, true, s->joiner, LINK_TX_PAIRING_KEY | LINK_TX_TRAIN_FULL, packet, n);
    s->pair_sent_ms = now;
    char e[17];
    say(s, "d/pair to %s (attempt %d)", eui_str(s->joiner, e), s->pair_attempts);
    if (s->pair_handle < 0)
        pair_left_radio(s, now); /* refused: no TX_DONE will come, the attempt times out instead */
}

/* A piggybacked answer (an ACK with one of our d/pair MIDs), or a separate
 * response: a CON/NON response code with d/pair's empty token. */
static bool is_pair_answer(const struct session *s, const struct coap_msg *m)
{
    if (m->type == COAP_ACK) {
        for (int i = 0; i < s->pair_attempts && i < SESSION_PAIR_ATTEMPTS; i++)
            if (s->pair_mids[i] == m->mid)
                return true;
        return false;
    }
    return m->code >> 5 >= 2 && m->tkl == 0 && (m->type == COAP_CON || m->type == COAP_NON);
}

static void on_pair_answer(struct session *s, bool pairing_key, const struct coap_msg *m, const uint8_t src[8])
{
    s->pair_deadline_set = false;
    char e[17];
    if (pairing_key) {
        say(s, "d/pair answered under the pairing key: device did not adopt our key");
        s->pair = SESSION_PAIR_FAILED;
    } else if (m->code == COAP_CODE(2, 4)) {
        say(s, "2.04 under our network key: %s JOINED", eui_str(src, e));
        s->pair = SESSION_PAIR_JOINED;
        if (s->ops.on_joined)
            s->ops.on_joined(s->ops.ctx, src);
    } else {
        say(s, "d/pair answered %u.%02u", m->code >> 5, m->code & 0x1f);
        s->pair = SESSION_PAIR_FAILED;
    }
}

/* --- Router Advertisements ----------------------------------------------- */

static void ra_ended(struct session *s, uint32_t now);

/* During a pair window every RA goes twice: network key with Frame Pending,
 * then the pairing key. */
static void send_router_advert(struct session *s, uint32_t now)
{
    uint8_t ra[MAC_BROADCAST_MAX_PACKET];
    size_t n = tado_router_advert(ra, sizeof ra, s->coordinator, NO_IPV4, NO_IPV4);
    schedule_next_ra(s, now);
    if (!pair_window_open(s)) {
        s->ra_handle = send(s, false, NULL, LINK_TX_TRAIN_FULL, ra, n);
        say(s, "router advertisement sent");
    } else {
        send(s, false, NULL, LINK_TX_FRAME_PENDING | LINK_TX_TRAIN_FULL, ra, n);
        s->ra_handle = send(s, false, NULL, LINK_TX_PAIRING_KEY | LINK_TX_TRAIN_FULL, ra, n);
        say(s, "router advertisement sent (network + pairing key)");
    }
    s->ra_sent_ms = now;
    if (s->ra_handle < 0)
        ra_ended(s, now); /* refused: no TX_DONE will come; d/pair and the 3 s gap go on from now */
}

static void ra_ended(struct session *s, uint32_t now)
{
    s->ra_handle = -1;
    s->ra_ended_once = true;
    s->ra_ended_ms = now;
    if (s->pair == SESSION_PAIR_WAITING && s->has_joiner)
        send_pair(s, now); /* as the bridge does: d/pair follows the advertisement for the joiner */
}

static void on_router_solicitation(struct session *s, const struct link_event *ev, uint32_t now)
{
    char e[17];
    say(s, "router solicitation from %s%s", eui_str(ev->rx.src, e), ev->rx.pairing_key ? " (pairing key)" : "");
    if (ev->rx.pairing_key && s->pair == SESSION_PAIR_WAITING && !s->has_joiner) {
        memcpy(s->joiner, ev->rx.src, 8);
        s->has_joiner = true;
    }
    /* One in flight answers this solicitation too; within 3 s of the last, RFC 4861 sends none. */
    if (s->ra_handle < 0 && ra_allowed(s, now))
        send_router_advert(s, now);
    else if (s->ra_handle < 0 && s->pair == SESSION_PAIR_WAITING && s->has_joiner)
        send_pair(s, now); /* the advertisement that just ended reached the joiner too */
}

/* --- answers and documents ------------------------------------------------- */

static struct session_answer *answer_find(struct session *s, const uint8_t device[8], uint16_t mid)
{
    for (size_t i = 0; i < SESSION_MAX_ANSWERS; i++) {
        struct session_answer *a = &s->answers[i];
        if (a->used && a->mid == mid && memcmp(a->device, device, 8) == 0)
            return a;
    }
    return NULL;
}

/* The slot the next answer is built in: a free one, else the oldest. */
static struct session_answer *answer_slot(struct session *s, uint32_t now)
{
    struct session_answer *oldest = &s->answers[0];
    for (size_t i = 0; i < SESSION_MAX_ANSWERS; i++) {
        struct session_answer *x = &s->answers[i];
        if (!x->used)
            return x;
        if (now - x->at_ms > now - oldest->at_ms)
            oldest = x;
    }
    return oldest;
}

/* Sends the answer built in a->packet on the device's sample and keeps it for
 * the request's repeats. */
static void answer(struct session *s, struct session_answer *a, const uint8_t device[8], uint16_t mid, size_t len,
                   uint32_t now)
{
    if (len == 0) {
        a->used = false;
        say(s, "answer to MID %04x does not fit one frame: not sent", mid);
        return;
    }
    a->used = true;
    a->mid = mid;
    a->at_ms = a->sent_ms = now;
    a->len = (uint8_t)len;
    memcpy(a->device, device, 8);
    a->handle = send(s, true, device, LINK_TX_TRAIN_ANCHORED, a->packet, len);
}

/* The Uri-Path segments of a request; n is 0 for a path of more than 5. */
struct path {
    size_t n;
    const struct coap_option *seg[5];
};

static struct path path_of(const struct coap_msg *m)
{
    struct path p = {0};
    for (size_t i = 0; i < m->n_options; i++) {
        if (m->options[i].number != COAP_OPT_URI_PATH)
            continue;
        if (p.n == 5)
            return (struct path){0};
        p.seg[p.n++] = &m->options[i];
    }
    return p;
}

static bool seg_is(const struct coap_option *o, const char *text)
{
    return o->len == strlen(text) && memcmp(o->value, text, o->len) == 0;
}

/* The decimal value of a segment of 1..10 digits, or -1. */
static int64_t seg_number(const struct coap_option *o)
{
    if (o->len < 1 || o->len > 10)
        return -1;
    int64_t v = 0;
    for (size_t i = 0; i < o->len; i++) {
        if (o->value[i] < '0' || o->value[i] > '9')
            return -1;
        v = v * 10 + (o->value[i] - '0');
    }
    return v;
}

/* h/<our home>/z/<n>/<leaf>: the configured zone, with its leaf; NULL else. */
static struct session_zone *zone_path(struct session *s, const struct path *p, const struct coap_option **leaf)
{
    if (p->n != 5 || !seg_is(p->seg[0], "h") || !seg_is(p->seg[2], "z") || seg_number(p->seg[1]) != s->home_id)
        return NULL;
    int64_t zone = seg_number(p->seg[3]);
    *leaf = p->seg[4];
    return zone >= 0 && zone <= 0xff ? zone_find(s, (unsigned)zone) : NULL;
}

static bool overlay_over(const struct session_zone *z, uint32_t now)
{
    return z->overlay_for_ms != 0 && elapsed(now, z->overlay_start_ms, z->overlay_for_ms);
}

/* The zone's running overlay setpoint, or -1. */
static int32_t overlay_of(const struct session_zone *z, uint32_t now)
{
    return z->overlay && !overlay_over(z, now) ? z->overlay_setpoint : -1;
}

/* Builds in s->doc the configuration (d/<serial>/config), zone document
 * (h/<home>/z/<n>/config) or zone state (h/<home>/z/<n>/s) for a device of the
 * installation; returns its length, 0 for anything else. */
static size_t document_for(struct session *s, const uint8_t device[8], const struct path *p, uint32_t now)
{
    if (p->n == 3 && seg_is(p->seg[0], "d") && seg_is(p->seg[2], "config")) {
        struct tado_zone_role roles[SESSION_MAX_ZONES];
        size_t n = 0;
        for (size_t i = 0; i < SESSION_MAX_ZONES; i++)
            if (s->zones[i].used && memcmp(s->zones[i].owner, device, 8) == 0)
                roles[n++] = (struct tado_zone_role){TADO_ROLE_OWNER, s->zones[i].zone};
        return n ? tado_device_config(s->doc, sizeof s->doc, s->home_id, roles, n) : 0;
    }
    const struct coap_option *leaf;
    struct session_zone *z = zone_path(s, p, &leaf);
    if (z && seg_is(leaf, "config"))
        return tado_zone_with_owner(s->doc, sizeof s->doc, z->owner);
    if (z && seg_is(leaf, "s"))
        return tado_zone_state(s->doc, sizeof s->doc, z->setpoint, overlay_of(z, now));
    return 0;
}

/* PUT h/<home>/z/<n>/overlay from the zone's owner: a setpoint set on the device
 * (6240 manual, 6280 the setpoint), or 6240=0 when the device ended it. It
 * carries no end: the zone keeps it for the installation's overlay time. */
static void on_device_overlay(struct session *s, const uint8_t device[8], const struct coap_msg *m,
                              const struct path *p, uint32_t now)
{
    const struct coap_option *leaf;
    struct session_zone *z = zone_path(s, p, &leaf);
    if (p->n != 5 || !seg_is(p->seg[4], "overlay"))
        return;
    char e[17];
    if (!z || !seg_is(leaf, "overlay") || memcmp(z->owner, device, 8) != 0) {
        say(s, "overlay on %s from %s, which does not own it: ignored", z ? "its zone" : "a zone we do not serve",
            eui_str(device, e));
        return;
    }
    unsigned zone = z->zone;
    struct tado_report r;
    if (!tado_report_decode(m->payload, m->payload_len, &r)) {
        say(s, "overlay on zone %u: malformed TLVs", zone);
        return;
    }
    z->writing = false; /* the device's word is newer than ours */
    if ((r.fields & TADO_OVERLAY) && r.overlay == 0) {
        z->overlay = false;
        say(s, "zone %u: the device ended its overlay", zone);
    } else if (r.fields & TADO_OVERLAY_SETPOINT) {
        uint32_t c = r.overlay_setpoint;
        z->overlay = true;
        z->overlay_setpoint = (uint16_t)c;
        z->overlay_start_ms = now;
        z->overlay_for_ms = s->overlay_ms;
        if (z->overlay_for_ms)
            say(s, "zone %u: %lu.%02lu C set on the device, for %lu min", zone, (unsigned long)c / 100,
                (unsigned long)c % 100, (unsigned long)s->overlay_ms / 60000u);
        else
            say(s, "zone %u: %lu.%02lu C set on the device, until changed", zone, (unsigned long)c / 100,
                (unsigned long)c % 100);
    }
}

static void send_zone_write(struct session *s, struct session_zone *z, uint32_t now)
{
    z->write_attempts++;
    z->write_sent_ms = now;
    send(s, true, z->owner, LINK_TX_TRAIN_ANCHORED, z->write_packet, z->write_len);
}

/* Ends each overlay whose time is up and tells its device, which would else
 * hold it until its next read. */
static void end_overlays(struct session *s, uint32_t now)
{
    for (size_t i = 0; i < SESSION_MAX_ZONES; i++) {
        struct session_zone *z = &s->zones[i];
        if (!z->used || !z->overlay || !overlay_over(z, now))
            continue;
        z->overlay = false;
        say(s, "zone %u: the device's overlay ran out, back to %u.%02u C", z->zone, z->setpoint / 100u,
             z->setpoint % 100u);
        uint8_t state[64];
        size_t sn = tado_zone_state(state, sizeof state, z->setpoint, -1);
        z->write_mid = s->next_mid++;
        z->write_len = (uint8_t)tado_zone_state_write(z->write_packet, sizeof z->write_packet, z->zone, state, sn,
                                                      z->write_mid, s->coordinator, z->owner);
        z->writing = true;
        z->write_attempts = 0;
        send_zone_write(s, z, now);
    }
}

static void repeat_zone_writes(struct session *s, uint32_t now)
{
    char e[17];
    for (size_t i = 0; i < SESSION_MAX_ZONES; i++) {
        struct session_zone *z = &s->zones[i];
        if (!z->used || !z->writing || !elapsed(now, z->write_sent_ms, SESSION_ZONE_WRITE_TIMEOUT_MS))
            continue;
        if (z->write_attempts >= SESSION_ZONE_WRITE_ATTEMPTS) {
            say(s, "zone %u state write: no ACK from %s after %u attempts; it reads the state again within 20 min",
                 z->zone, eui_str(z->owner, e), z->write_attempts);
            z->writing = false;
            continue;
        }
        send_zone_write(s, z, now);
    }
}

/* --- CoAP ------------------------------------------------------------------- */

static void requested(struct session *s, const uint8_t device[8], const struct coap_msg *m, const char *path,
                      bool answered)
{
    if (s->ops.on_request)
        s->ops.on_request(s->ops.ctx, device, m->code, path, answered);
}

static void on_coap(struct session *s, const struct link_event *ev, const struct lowpan_datagram *d, uint32_t now)
{
    const uint8_t *src = ev->rx.src;
    char e[17], path[64], hex[96];
    struct coap_msg m;
    struct session_answer *a;
    if (!coap_decode(d->payload, d->payload_len, &m)) {
        say(s, "bad CoAP from %s", eui_str(src, e));
        return;
    }
    bool from_joiner = s->has_joiner && memcmp(s->joiner, src, 8) == 0;
    if (from_joiner && s->pair == SESSION_PAIR_PAIRING && is_pair_answer(s, &m)) {
        if (m.type == COAP_ACK && m.code == 0) {
            /* Empty ACK: the 2.04 follows as a separate response (RFC 7252 5.2.2). */
            say(s, "d/pair acknowledged, waiting for the separate answer");
            s->pair_deadline_set = true;
            s->pair_deadline_ms = now + SESSION_PAIR_ANSWER_TIMEOUT_MS;
            return;
        }
        if (m.type == COAP_CON) {
            /* Answer under the key it came in, so the ACK reaches it either way. */
            uint8_t out[SESSION_MAX_PACKET];
            size_t n = tado_empty_ack(out, sizeof out, &m, d, s->coordinator, src);
            send(s, true, src, LINK_TX_TRAIN_FULL | (ev->rx.pairing_key ? LINK_TX_PAIRING_KEY : 0), out, n);
        }
        on_pair_answer(s, ev->rx.pairing_key, &m, src);
        return;
    }
    if (ev->rx.pairing_key)
        return; /* only d/pair's answer matters before the device holds our key */

    for (size_t i = 0; m.type == COAP_ACK && i < SESSION_MAX_ZONES; i++) {
        struct session_zone *z = &s->zones[i];
        if (z->used && z->writing && z->write_mid == m.mid && memcmp(z->owner, src, 8) == 0) {
            z->writing = false;
            say(s, "zone %u state write: %u.%02u from %s", z->zone, m.code >> 5, m.code & 0x1f, eui_str(src, e));
            return;
        }
    }

    coap_path(&m, path, sizeof path);
    struct session_answer *earlier = m.type == COAP_CON ? answer_find(s, src, m.mid) : NULL;
    if (earlier) {
        /* A repeat: the same answer again, unless it is still on its way. A lost
         * TX_DONE must not hold the answer back for its whole lifetime. */
        if (earlier->handle < 0 || elapsed(now, earlier->sent_ms, SESSION_TX_DONE_TIMEOUT_MS)) {
            say(s, "%s from %s again, answering it again", path, eui_str(src, e));
            earlier->handle = send(s, true, src, LINK_TX_TRAIN_ANCHORED, earlier->packet, earlier->len);
            earlier->sent_ms = now;
        }
        return;
    }
    if (m.type != COAP_CON) {
        say(s, "CoAP %d %u.%02u %s from %s (not handled yet)", m.type, m.code >> 5, m.code & 0x1f, path,
             eui_str(src, e));
        return;
    }
    struct path p = path_of(&m);
    size_t doc_len = m.code == COAP_GET ? document_for(s, src, &p, now) : 0;
    size_t n;
    if (doc_len > 0) {
        const struct coap_option *b = coap_find(&m, COAP_OPT_BLOCK2);
        say(s, "GET %s from %s, block %s", path, eui_str(src, e), b ? hex_str(b->value, b->len, hex, sizeof hex) : "-");
        a = answer_slot(s, now);
        n = tado_block_answer(a->packet, sizeof a->packet, &m, s->doc, doc_len, d, s->coordinator, src);
    } else if (coap_path_is(&m, "auth/token") || coap_path_is(&m, "auth/key")) {
        say(s, "%s from %s, answering with a session", path, eui_str(src, e));
        uint8_t id[8];
        session_id(src, id);
        a = answer_slot(s, now);
        n = tado_auth_token_answer(a->packet, sizeof a->packet, &m, id, sizeof id, d, s->coordinator, src);
    } else if (coap_path_is(&m, "time")) {
        uint32_t unix = s->ops.unix_time(s->ops.ctx);
        if (unix == 0) {
            say(s, "time from %s: no wall-clock time yet, not answered", eui_str(src, e));
            requested(s, src, &m, path, false);
            return;
        }
        say(s, "time from %s", eui_str(src, e));
        a = answer_slot(s, now);
        n = tado_time_answer(a->packet, sizeof a->packet, &m, unix, d, s->coordinator, src);
    } else if (m.code == COAP_PUT || m.code == COAP_POST) {
        /* Reports (d/<serial>/..., z/p): the bridge's default 2.04, or the device retries. */
        say(s, "%u.%02u %s from %s, answering 2.04: %s", m.code >> 5, m.code & 0x1f, path, eui_str(src, e),
            hex_str(m.payload, m.payload_len, hex, sizeof hex));
        if (m.code == COAP_PUT)
            on_device_overlay(s, src, &m, &p, now);
        struct tado_report r;
        if (s->ops.on_report && tado_report_decode(m.payload, m.payload_len, &r))
            s->ops.on_report(s->ops.ctx, src, path, &r);
        a = answer_slot(s, now);
        n = tado_changed_answer(a->packet, sizeof a->packet, &m, d, s->coordinator, src);
    } else {
        say(s, "CoAP %u.%02u %s from %s (not handled yet)", m.code >> 5, m.code & 0x1f, path, eui_str(src, e));
        requested(s, src, &m, path, false);
        return;
    }
    requested(s, src, &m, path, true);
    answer(s, a, src, m.mid, n, now);
}

/* --- events ----------------------------------------------------------------- */

static void on_rx(struct session *s, const struct link_event *ev, uint32_t now)
{
    const uint8_t *whole;
    size_t len;
    char e[17];
    enum reasm_result r = reasm_add(&s->reasm, ev->rx.src, ev->rx.packet, ev->rx.len, now, &whole, &len);
    if (r == REASM_PENDING)
        return; /* a fragment of a datagram still incomplete */
    struct lowpan_datagram d;
    if (r == REASM_DROPPED || !lowpan_parse(whole, len, ev->rx.src, ev->rx.broadcast ? NULL : s->coordinator, &d)) {
        say(s, "unparsed datagram from %s", eui_str(ev->rx.src, e));
        return;
    }
    /* whole is valid only until the next reasm_add: act on it now. */
    if (lowpan_icmp_type(&d) == ICMP_ROUTER_SOLICITATION)
        on_router_solicitation(s, ev, now);
    else if (d.next_header == LOWPAN_NH_UDP && !ev->rx.broadcast)
        on_coap(s, ev, &d, now);
}

static void on_tx_done(struct session *s, const struct link_event *ev, uint32_t now)
{
    int handle = ev->tx_done.handle;
    for (size_t i = 0; i < SESSION_MAX_ANSWERS; i++)
        if (s->answers[i].used && s->answers[i].handle == handle)
            s->answers[i].handle = -1;
    if (handle == s->ra_handle) {
        ra_ended(s, now);
    } else if (handle == s->pair_handle) {
        static const char *const results[] = {"ACKED", "SENT", "NO_ACK", "RADIO_ERROR"};
        say(s, "d/pair %s", results[ev->tx_done.result & 3]);
        pair_left_radio(s, now);
    }
}

void session_on_event(struct session *s, const struct link_event *ev, uint32_t now_ms)
{
    char e[17];
    switch (ev->type) {
    case LINK_EV_BEACON_ANSWERED:
        say(s, "beacon answered for %s (rssi %d)", eui_str(ev->beacon.joiner, e), ev->beacon.rssi);
        if (s->pair == SESSION_PAIR_WAITING && !s->has_joiner) {
            memcpy(s->joiner, ev->beacon.joiner, 8);
            s->has_joiner = true;
        }
        break;
    case LINK_EV_RX:
        on_rx(s, ev, now_ms);
        break;
    case LINK_EV_TX_DONE:
        on_tx_done(s, ev, now_ms);
        break;
    case LINK_EV_LOG:
        say(s, "cc1312: %.*s", (int)ev->log.len, ev->log.text);
        break;
    }
}

void session_tick(struct session *s, uint32_t now)
{
    if (s->ra_handle >= 0 && elapsed(now, s->ra_sent_ms, SESSION_TX_DONE_TIMEOUT_MS)) {
        say(s, "no TX_DONE for the router advertisement, assuming it went out");
        ra_ended(s, now);
    }
    if (s->pair_handle >= 0 && elapsed(now, s->pair_sent_ms, SESSION_TX_DONE_TIMEOUT_MS)) {
        say(s, "no TX_DONE for d/pair, assuming it went out");
        pair_left_radio(s, now);
    }
    for (size_t i = 0; i < SESSION_MAX_ANSWERS; i++)
        if (s->answers[i].used && elapsed(now, s->answers[i].at_ms, SESSION_ANSWER_LIFETIME_MS))
            s->answers[i].used = false;
    end_overlays(s, now);
    repeat_zone_writes(s, now);
    if (s->ra_handle < 0 && (int32_t)(now - s->next_ra_ms) >= 0 && ra_allowed(s, now))
        send_router_advert(s, now);
    if (s->pair == SESSION_PAIR_PAIRING && s->pair_deadline_set && (int32_t)(now - s->pair_deadline_ms) >= 0) {
        s->pair_deadline_set = false;
        if (s->pair_attempts >= SESSION_PAIR_ATTEMPTS) {
            s->pair = SESSION_PAIR_FAILED;
            say(s, "no 2.04 to d/pair after %d attempts", SESSION_PAIR_ATTEMPTS);
        } else {
            say(s, "no 2.04 to d/pair yet, sending it again");
            send_pair(s, now);
        }
    }
}
