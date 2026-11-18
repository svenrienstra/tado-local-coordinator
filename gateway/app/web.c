#include "web.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hexstr.h"

/* --- writing JSON into a bounded buffer ------------------------------------- */

struct out {
    char *buf;
    size_t cap, len;
    bool full;
};

static void put(struct out *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void put(struct out *o, const char *fmt, ...)
{
    if (o->full)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->buf + o->len, o->cap - o->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= o->cap - o->len)
        o->full = true;
    else
        o->len += (size_t)n;
}

static int answer(struct out *o, int status, const char *error)
{
    if (error) {
        o->len = 0;
        o->full = false;
        put(o, "{\"error\":\"%s\"}", error);
    }
    if (o->full) {
        o->len = 0;
        o->full = false;
        put(o, "{\"error\":\"answer too long\"}");
        return 500;
    }
    return status;
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
    char e[17], json[512];
    put(o, "{\"cc1312\":%s,\"clock\":%s,\"home\":%lu,", g->configured ? "true" : "false",
        g->ops.unix_time(g->ops.ctx) ? "true" : "false", (unsigned long)g->id.home_id);
    put(o, "\"pair\":{\"state\":\"%s\",\"left\":%lu", pair_state(g->pair), (unsigned long)gateway_pair_left(g));
    if (g->pair == GATEWAY_PAIR_JOINED)
        put(o, ",\"joined\":\"%s\"", eui_str(g->pair_joined, e));
    put(o, "},\"rooms\":[");
    bool first = true;
    for (size_t i = 0; i < INSTALL_MAX_ROOMS; i++) {
        const struct gateway_room *r = &g->rooms[i];
        if (!r->used)
            continue;
        if (!ha_zone_state(json, sizeof json, &r->r))
            strcpy(json, "{}");
        put(o, "%s{\"zone\":%u,\"name\":\"%s\",\"setpoint\":%u,\"state\":%s}", first ? "" : ",", r->zone, r->name,
            r->setpoint, json);
        first = false;
    }
    put(o, "],\"devices\":[");
    first = true;
    for (size_t i = 0; i < INSTALL_MAX_DEVICES; i++) {
        const struct gateway_device *d = &g->devices[i];
        if (!d->used)
            continue;
        if (!ha_device_state(json, sizeof json, &d->d))
            strcpy(json, "{}");
        put(o, "%s{\"eui\":\"%s\",\"serial\":\"%s\",\"zone\":%u,\"waiting\":%s,\"state\":%s}", first ? "" : ",",
            eui_str(d->eui, e), d->d.serial, d->zone, d->waiting ? "true" : "false", json);
        first = false;
    }
    put(o, "]}");
}

/* --- requests --------------------------------------------------------------- */

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool web_form(const char *body, size_t len, const char *key, char *out, size_t cap)
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
                        return false;
                    c = (char)(hi << 4 | lo);
                    i += 2;
                }
                if (n + 1 >= cap || c == '\0')
                    return false;
                out[n++] = c;
            }
            out[n] = '\0';
            return true;
        }
        at = end + 1;
    }
    return false;
}

/* A decimal number 0-255 that is the whole string. */
static int number(const char *s)
{
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    return *s && *end == '\0' && v <= 255 && s[0] != '-' && s[0] != '+' ? (int)v : -1;
}

static bool parse_eui(const char *s, size_t len, uint8_t eui[8])
{
    if (len != 16)
        return false;
    for (size_t i = 0; i < 8; i++) {
        int hi = hex_digit(s[2 * i]), lo = hex_digit(s[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        eui[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

static int rooms(struct gateway *g, const char *rest, const char *body, size_t len, struct out *o)
{
    char name[INSTALL_NAME_MAX + 2];
    bool named = web_form(body, len, "name", name, sizeof name);
    if (*rest == '\0') {
        if (!named)
            return answer(o, 400, "name required");
        uint8_t zone = gateway_add_room(g, name);
        if (!zone)
            return answer(o, 400, "bad name, or no room left");
        put(o, "{\"zone\":%u}", zone);
        return answer(o, 200, NULL);
    }
    if (*rest++ != '/')
        return answer(o, 404, "not found");
    char digits[4] = {0};
    size_t n = strcspn(rest, "/");
    if (n == 0 || n >= sizeof digits)
        return answer(o, 404, "no such room");
    memcpy(digits, rest, n);
    int zone = number(digits);
    if (zone <= 0 || !gateway_room(g, (uint8_t)zone))
        return answer(o, 404, "no such room");
    rest += n;
    if (strcmp(rest, "/delete") == 0) {
        gateway_delete_room(g, (uint8_t)zone);
    } else if (*rest == '\0') {
        if (!named || !gateway_rename_room(g, (uint8_t)zone, name))
            return answer(o, 400, "bad name");
    } else {
        return answer(o, 404, "not found");
    }
    put(o, "{}");
    return answer(o, 200, NULL);
}

static int devices(struct gateway *g, const char *rest, const char *body, size_t len, struct out *o)
{
    uint8_t eui[8];
    char value[8];
    if (*rest++ != '/' || !parse_eui(rest, strlen(rest), eui) || !gateway_device(g, eui))
        return answer(o, 404, "no such device");
    int zone = web_form(body, len, "zone", value, sizeof value) ? number(value) : -1;
    if (zone < 0)
        return answer(o, 400, "zone required");
    if (!gateway_assign(g, eui, (uint8_t)zone))
        return answer(o, 409, "no such room, or it has a device");
    put(o, "{}");
    return answer(o, 200, NULL);
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
            return answer(&o, 409, "no pair window: the cc1312 is not ready or a device is joining");
        put(&o, "{}");
        return answer(&o, 200, NULL);
    }
    if (strcmp(path, "/api/pair/close") == 0) {
        gateway_close_pair(g);
        put(&o, "{}");
        return answer(&o, 200, NULL);
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
