#include "install.h"

#include <string.h>

#include "bytes.h"
#include "ha.h"

bool install_name_ok(const char *name, size_t len)
{
    if (len < 1 || len > INSTALL_NAME_MAX)
        return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == 0x7f || c == '"' || c == '\\')
            return false;
    }
    return true;
}

/* A record: tag, length, value. */
struct w {
    uint8_t *out;
    size_t cap, n;
    bool full;
};

static void w_byte(struct w *w, uint8_t b)
{
    if (w->n >= w->cap)
        w->full = true;
    else
        w->out[w->n++] = b;
}

static void w_u16(struct w *w, uint16_t v)
{
    w_byte(w, (uint8_t)v);
    w_byte(w, (uint8_t)(v >> 8));
}

static void w_bytes(struct w *w, const void *b, size_t len)
{
    for (size_t i = 0; i < len; i++)
        w_byte(w, ((const uint8_t *)b)[i]);
}

/* Starts a record; record_end fills in its length. */
static size_t record(struct w *w, uint8_t tag)
{
    w_byte(w, tag);
    w_byte(w, 0);
    return w->n;
}

static void record_end(struct w *w, size_t start)
{
    if (!w->full)
        w->out[start - 1] = (uint8_t)(w->n - start);
}

size_t install_encode(const struct install *in, uint8_t *out, size_t cap)
{
    struct w w = {out, cap, 0, false};
    w_byte(&w, INSTALL_VERSION);
    size_t r = record(&w, INSTALL_TAG_SETTINGS);
    w_byte(&w, in->away ? INSTALL_FLAG_AWAY : 0);
    w_byte(&w, in->next_zone);
    w_u16(&w, in->stale_minutes);
    record_end(&w, r);
    for (size_t i = 0; i < in->n_rooms; i++) {
        const struct install_room *room = &in->rooms[i];
        size_t len = strlen(room->name);
        r = record(&w, INSTALL_TAG_ROOM);
        w_byte(&w, room->zone);
        w_u16(&w, room->setpoint);
        w_u16(&w, room->away_setpoint);
        w_byte(&w, (uint8_t)len);
        w_bytes(&w, room->name, len);
        record_end(&w, r);
    }
    for (size_t i = 0; i < in->n_devices; i++) {
        const struct install_device *d = &in->devices[i];
        size_t len = strlen(d->serial);
        r = record(&w, INSTALL_TAG_DEVICE);
        w_bytes(&w, d->eui, 8);
        w_byte(&w, d->zone);
        w_byte(&w, (uint8_t)len);
        w_bytes(&w, d->serial, len);
        record_end(&w, r);
    }
    record(&w, INSTALL_TAG_END);
    return w.full ? 0 : w.n;
}

/* A length-prefixed string of at most max bytes at v[*at], within len. */
static bool take_string(const uint8_t *v, size_t len, size_t *at, char *out, size_t max)
{
    if (*at >= len || v[*at] > max || len - *at - 1 < v[*at])
        return false;
    size_t n = v[(*at)++];
    memcpy(out, &v[*at], n);
    out[n] = '\0';
    *at += n;
    return true;
}

/* Each known record reads what it knows; bytes after that are a later
 * version's and are skipped, as are records of an unknown tag. */
static bool decode(const uint8_t *blob, size_t len, struct install *in)
{
    if (len < 1 || blob[0] != INSTALL_VERSION)
        return false;
    bool settings = false;
    for (size_t at = 1; at + 2 <= len;) {
        uint8_t tag = blob[at], n = blob[at + 1];
        const uint8_t *v = &blob[at + 2];
        if (len - at - 2 < n)
            return false;
        at += 2 + n;
        size_t k = 0;
        switch (tag) {
        case INSTALL_TAG_END:
            return settings && at == len;
        case INSTALL_TAG_SETTINGS:
            if (n < 4 || get16(&v[2]) > INSTALL_STALE_MINUTES_MAX)
                return false;
            in->away = v[0] & INSTALL_FLAG_AWAY; /* other bits: a later version's */
            in->next_zone = v[1];
            in->stale_minutes = get16(&v[2]);
            settings = true;
            break;
        case INSTALL_TAG_ROOM: {
            if (n < 5 || in->n_rooms == INSTALL_MAX_ROOMS)
                return false;
            struct install_room *r = &in->rooms[in->n_rooms++];
            r->zone = v[0];
            r->setpoint = get16(&v[1]);
            r->away_setpoint = get16(&v[3]);
            k = 5;
            if (r->zone == 0 || !take_string(v, n, &k, r->name, INSTALL_NAME_MAX) ||
                !install_name_ok(r->name, strlen(r->name)))
                return false;
            break;
        }
        case INSTALL_TAG_DEVICE: {
            if (n < 9 || in->n_devices == INSTALL_MAX_DEVICES)
                return false;
            struct install_device *d = &in->devices[in->n_devices++];
            memcpy(d->eui, v, 8);
            d->zone = v[8];
            k = 9;
            if (!take_string(v, n, &k, d->serial, INSTALL_SERIAL_MAX) ||
                (d->serial[0] && !ha_serial_ok(d->serial, strlen(d->serial))))
                return false;
            break;
        }
        default:
            break; /* a later version's record */
        }
    }
    return false; /* no end record: cut short */
}

void install_empty(struct install *in)
{
    memset(in, 0, sizeof *in);
    in->stale_minutes = INSTALL_STALE_MINUTES_DEFAULT;
}

bool install_decode(const uint8_t *blob, size_t len, struct install *in)
{
    memset(in, 0, sizeof *in);
    if (decode(blob, len, in))
        return true;
    install_empty(in);
    return false;
}
