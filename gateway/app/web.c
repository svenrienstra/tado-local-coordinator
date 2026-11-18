#include "web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hexstr.h"
#include "out.h"

/* --- answers ---------------------------------------------------------------- */

static void reset(struct out *o)
{
    o->len = 0;
    o->full = false;
}

static int answer(struct out *o, int status, const char *error)
{
    if (error) {
        reset(o);
        put(o, "{\"error\":\"%s\"}", error);
    }
    if (o->full) {
        reset(o);
        put(o, "{\"error\":\"answer too long\"}");
        return 500;
    }
    return status;
}

static int ok(struct out *o)
{
    put(o, "{}");
    return answer(o, 200, NULL);
}

/* What a ha_*_state writer wrote into o, in place. */
static void wrote(struct out *o, size_t n)
{
    if (n == 0)
        o->full = true;
    o->len += n;
}

/* --- the state -------------------------------------------------------------- */

static const char *pair_state(enum gateway_pair_state s)
{
    static const char *const names[] = {
        [GATEWAY_PAIR_IDLE] = "idle",     [GATEWAY_PAIR_OPEN] = "open",     [GATEWAY_PAIR_JOINING] = "joining",
        [GATEWAY_PAIR_JOINED] = "joined", [GATEWAY_PAIR_FAILED] = "failed", [GATEWAY_PAIR_CLOSED] = "closed",
    };
    return names[s];
}

static void put_state(struct gateway *g, struct out *o)
{
    char e[17];
    put(o, "{\"cc1312\":%s,\"clock\":%s,\"home\":%lu,\"away\":%s,\"stale_minutes\":%u,\"boost_setpoint\":%u,\"boost_minutes\":%u,\"setpoint_min\":%u,\"setpoint_max\":%u,",
        g->configured ? "true" : "false", g->ops.unix_time(g->ops.ctx) ? "true" : "false",
        (unsigned long)g->id.home_id, g->away ? "true" : "false", g->stale_minutes, GATEWAY_BOOST_SETPOINT, GATEWAY_BOOST_MS / 60000u, HA_SETPOINT_MIN, HA_SETPOINT_MAX);
    put(o, "\"pair\":{\"state\":\"%s\",\"left\":%lu", pair_state(g->pair), (unsigned long)gateway_pair_left(g));
    if (g->pair == GATEWAY_PAIR_JOINED)
        put(o, ",\"joined\":\"%s\"", eui_str(g->pair_joined, e));
    put(o, "},\"rooms\":[");
    bool first = true;
    for (size_t i = 0; i < INSTALL_MAX_ROOMS; i++) {
        const struct gateway_room *r = &g->rooms[i];
        if (!r->used)
            continue;
        put(o, "%s{\"zone\":%u,\"name\":\"%s\",\"setpoint\":%u,\"away_setpoint\":%u,\"boost_left\":%lu,\"state\":",
            first ? "" : ",", r->zone, r->name, r->setpoint, r->away_setpoint,
            (unsigned long)gateway_boost_left(g, r->zone));
        if (!o->full)
            wrote(o, ha_zone_state(o->buf + o->len, o->cap - o->len, &r->r, &r->target));
        put(o, "}");
        first = false;
    }
    put(o, "],\"devices\":[");
    first = true;
    for (size_t i = 0; i < INSTALL_MAX_DEVICES; i++) {
        const struct gateway_device *d = &g->devices[i];
        if (!d->used)
            continue;
        put(o, "%s{\"eui\":\"%s\",\"serial\":\"%s\",\"zone\":%u,\"waiting\":%s,\"state\":", first ? "" : ",",
            eui_str(d->eui, e), d->d.serial, d->zone, d->waiting ? "true" : "false");
        if (!o->full)
            wrote(o, ha_device_state(o->buf + o->len, o->cap - o->len, &d->d));
        put(o, "}");
        first = false;
    }
    put(o, "]}");
}

/* --- requests --------------------------------------------------------------- */

enum web_field web_form(const char *body, size_t len, const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    for (size_t at = 0; at < len;) {
        size_t end = at;
        while (end < len && body[end] != '&')
            end++;
        if (end - at > klen && memcmp(&body[at], key, klen) == 0 && body[at + klen] == '=') {
            size_t n = 0;
            for (size_t i = at + klen + 1; i < end; i++) {
                char c = body[i];
                if (c == '+') {
                    c = ' ';
                } else if (c == '%') {
                    int hi = i + 2 < end ? hex_digit(body[i + 1]) : -1, lo = hi >= 0 ? hex_digit(body[i + 2]) : -1;
                    if (lo < 0)
                        return WEB_FIELD_BAD;
                    c = (char)(hi << 4 | lo);
                    i += 2;
                }
                if (n + 1 >= cap || c == '\0')
                    return WEB_FIELD_BAD;
                out[n++] = c;
            }
            out[n] = '\0';
            return WEB_FIELD_OK;
        }
        at = end + 1;
    }
    return WEB_FIELD_MISSING;
}

/* A decimal number 0..max that is the whole string, or -1. */
static long number(const char *s, unsigned long max)
{
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    return s[0] >= '0' && s[0] <= '9' && *end == '\0' && v <= max ? (long)v : -1;
}

/* A form field of 1 or 0. */
static enum web_field form_flag(const char *body, size_t len, const char *key, bool *out)
{
    char v[4];
    enum web_field f = web_form(body, len, key, v, sizeof v);
    if (f == WEB_FIELD_OK && (strcmp(v, "1") == 0 || strcmp(v, "0") == 0))
        *out = v[0] == '1';
    else if (f == WEB_FIELD_OK)
        f = WEB_FIELD_BAD;
    return f;
}

/* A form field of degrees C in the setpoint range, in centi-C; with allow_off
 * also "off" (0). */
static enum web_field form_centi(const char *body, size_t len, const char *key, bool allow_off, int32_t *out)
{
    char v[32];
    enum web_field f = web_form(body, len, key, v, sizeof v);
    if (f != WEB_FIELD_OK)
        return f;
    *out = allow_off && strcmp(v, "off") == 0 ? 0 : ha_parse_centi(v, strlen(v));
    return (*out == 0 && allow_off) || ha_setpoint_ok(*out) ? WEB_FIELD_OK : WEB_FIELD_BAD;
}

static int range_error(struct out *o, const char *key)
{
    char error[64];
    snprintf(error, sizeof error, "%s: %u-%u degrees C", key, HA_SETPOINT_MIN / 100, HA_SETPOINT_MAX / 100);
    return answer(o, 400, error);
}

static int rooms(struct gateway *g, const char *rest, const char *body, size_t len, struct out *o)
{
    char name[INSTALL_NAME_MAX + 2];
    enum web_field named = web_form(body, len, "name", name, sizeof name);
    if (*rest == '\0') {
        if (named != WEB_FIELD_OK)
            return answer(o, 400, "name required");
        uint8_t zone = gateway_add_room(g, name);
        if (!zone)
            return answer(o, 400, "bad name, or no room left");
        put(o, "{\"zone\":%u}", zone);
        return answer(o, 200, NULL);
    }
    if (*rest++ != '/')
        return answer(o, 404, "not found");
    char *end;
    unsigned long zone = rest[0] >= '0' && rest[0] <= '9' ? strtoul(rest, &end, 10) : 0;
    if (zone == 0 || zone > 255 || (*end != '\0' && *end != '/') || !gateway_room(g, (uint8_t)zone))
        return answer(o, 404, "no such room");
    rest = end;
    if (strcmp(rest, "/delete") == 0) {
        gateway_delete_room(g, (uint8_t)zone);
    } else if (*rest == '\0') {
        /* All checked before any changes anything. */
        int32_t c = 0, a = 0;
        bool boost = false;
        enum web_field setpoint = form_centi(body, len, "setpoint", false, &c);
        enum web_field away = form_centi(body, len, "away_setpoint", true, &a);
        enum web_field boosting = form_flag(body, len, "boost", &boost);
        if (named == WEB_FIELD_OK && !install_name_ok(name, strlen(name)))
            named = WEB_FIELD_BAD;
        if (named == WEB_FIELD_MISSING && setpoint == WEB_FIELD_MISSING && away == WEB_FIELD_MISSING &&
            boosting == WEB_FIELD_MISSING)
            return answer(o, 400, "name, setpoint, away_setpoint or boost required");
        if (named == WEB_FIELD_BAD)
            return answer(o, 400, "bad name");
        if (setpoint == WEB_FIELD_BAD)
            return range_error(o, "setpoint");
        if (away == WEB_FIELD_BAD)
            return range_error(o, "away_setpoint");
        if (boosting == WEB_FIELD_BAD)
            return answer(o, 400, "boost: 1 or 0");
        /* A setpoint ends a boost: the two together would contradict. */
        if (boosting == WEB_FIELD_OK && setpoint == WEB_FIELD_OK)
            return answer(o, 400, "boost and setpoint: one at a time");
        /* The boost first: the only change that can still be refused. */
        if (boosting == WEB_FIELD_OK && !gateway_boost(g, (uint8_t)zone, boost))
            return answer(o, 409, boost ? "boost: the room has no device" : "boost: none runs");
        if (named == WEB_FIELD_OK)
            gateway_rename_room(g, (uint8_t)zone, name);
        if (setpoint == WEB_FIELD_OK)
            gateway_set_setpoint(g, (uint8_t)zone, (uint16_t)c);
        if (away == WEB_FIELD_OK)
            gateway_set_away_setpoint(g, (uint8_t)zone, (uint16_t)a);
    } else {
        return answer(o, 404, "not found");
    }
    return ok(o);
}

static int devices(struct gateway *g, const char *rest, const char *body, size_t len, struct out *o)
{
    uint8_t eui[8];
    char value[8];
    if (*rest++ != '/' || !eui_parse(rest, strlen(rest), eui) || !gateway_device(g, eui))
        return answer(o, 404, "no such device");
    long zone = web_form(body, len, "zone", value, sizeof value) == WEB_FIELD_OK ? number(value, 255) : -1;
    if (zone < 0)
        return answer(o, 400, "zone required");
    if (!gateway_assign(g, eui, (uint8_t)zone))
        return answer(o, 409, "no such room, or it has a device");
    return ok(o);
}

int web_handle(struct gateway *g, const char *method, const char *path, const char *body, size_t body_len,
               char *out, size_t cap)
{
    struct out o = {out, cap, 0, false};
    out[0] = '\0';
    bool get = strcmp(method, "GET") == 0, post = strcmp(method, "POST") == 0;
    if (get && strcmp(path, "/api/state") == 0) {
        put_state(g, &o);
        return answer(&o, 200, NULL);
    }
    if (!post)
        return answer(&o, get ? 404 : 405, get ? "not found" : "method not allowed");
    if (strcmp(path, "/api/pair") == 0) {
        if (!gateway_pair(g, GATEWAY_PAIR_WINDOW_S))
            return answer(&o, 409, "no pair window: the radio is not ready, a device is joining, or the device "
                                   "table is full");
        return ok(&o);
    }
    if (strcmp(path, "/api/pair/close") == 0) {
        gateway_close_pair(g);
        return ok(&o);
    }
    if (strcmp(path, "/api/settings") == 0) {
        char value[8];
        long minutes = web_form(body, body_len, "stale_minutes", value, sizeof value) == WEB_FIELD_OK
                           ? number(value, INSTALL_STALE_MINUTES_MAX) : -1;
        if (minutes < 0 || !gateway_set_stale_minutes(g, (uint16_t)minutes)) {
            char error[48];
            snprintf(error, sizeof error, "stale_minutes: 0-%u", INSTALL_STALE_MINUTES_MAX);
            return answer(&o, 400, error);
        }
        return ok(&o);
    }
    if (strcmp(path, "/api/home") == 0) {
        bool away;
        if (form_flag(body, body_len, "away", &away) != WEB_FIELD_OK)
            return answer(&o, 400, "away: 1 or 0");
        gateway_set_away(g, away);
        return ok(&o);
    }
    if (strncmp(path, "/api/rooms", 10) == 0)
        return rooms(g, path + 10, body, body_len, &o);
    if (strncmp(path, "/api/devices", 12) == 0)
        return devices(g, path + 12, body, body_len, &o);
    return answer(&o, 404, "not found");
}

/* --- authorization ---------------------------------------------------------- */

static size_t base64(const uint8_t *in, size_t len, char *out, size_t cap)
{
    static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < len ? (uint32_t)in[i + 1] << 8 : 0) |
                     (i + 2 < len ? in[i + 2] : 0);
        if (n + 4 >= cap)
            return 0;
        out[n++] = abc[v >> 18 & 63];
        out[n++] = abc[v >> 12 & 63];
        out[n++] = i + 1 < len ? abc[v >> 6 & 63] : '=';
        out[n++] = i + 2 < len ? abc[v & 63] : '=';
    }
    out[n] = '\0';
    return n;
}

bool web_auth_ok(const char *header, const char *user, const char *password)
{
    char plain[128], want[200];
    int n = snprintf(plain, sizeof plain, "%s:%s", user, password);
    if (!header || !*password || n < 0 || (size_t)n >= sizeof plain ||
        !base64((const uint8_t *)plain, (size_t)n, want, sizeof want) || strncmp(header, "Basic ", 6) != 0)
        return false;
    /* Compared in full whatever matches, so the time says nothing. */
    const char *got = header + 6;
    size_t wl = strlen(want), gl = strlen(got);
    unsigned diff = wl != gl;
    for (size_t i = 0; i < wl; i++)
        diff |= (unsigned)(want[i] ^ (i < gl ? got[i] : 0));
    return diff == 0;
}
