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

size_t install_encode(const struct install *in, uint8_t *out, size_t cap)
{
    size_t n = 0;
#define PUT(byte)                     \
    do {                              \
        if (n >= cap)                 \
            return 0;                 \
        out[n++] = (uint8_t)(byte);   \
    } while (0)
    PUT(INSTALL_VERSION);
    PUT(in->away ? 1 : 0);
    PUT(in->next_zone);
    PUT(in->stale_minutes);
    PUT(in->stale_minutes >> 8);
    PUT(in->n_rooms);
    for (size_t i = 0; i < in->n_rooms; i++) {
        const struct install_room *r = &in->rooms[i];
        size_t len = strlen(r->name);
        PUT(r->zone);
        if (cap - n < 4)
            return 0;
        put16(&out[n], r->setpoint);
        put16(&out[n + 2], r->away_setpoint);
        n += 4;
        PUT(len);
        for (size_t k = 0; k < len; k++)
            PUT(r->name[k]);
    }
    PUT(in->n_devices);
    for (size_t i = 0; i < in->n_devices; i++) {
        const struct install_device *d = &in->devices[i];
        size_t len = strlen(d->serial);
        for (size_t k = 0; k < 8; k++)
            PUT(d->eui[k]);
        PUT(d->zone);
        PUT(len);
        for (size_t k = 0; k < len; k++)
            PUT(d->serial[k]);
    }
#undef PUT
    return n;
}

/* A length-prefixed string of at most max bytes into out. */
static bool take_string(const uint8_t *blob, size_t len, size_t *at, char *out, size_t max)
{
    if (*at >= len || blob[*at] > max || len - *at - 1 < blob[*at])
        return false;
    size_t n = blob[(*at)++];
    memcpy(out, &blob[*at], n);
    out[n] = '\0';
    *at += n;
    return true;
}

static bool decode(const uint8_t *blob, size_t len, struct install *in)
{
    size_t at = 6;
    if (len < 6 || blob[0] != INSTALL_VERSION || blob[1] > 1 || get16(&blob[3]) > INSTALL_STALE_MINUTES_MAX ||
        blob[5] > INSTALL_MAX_ROOMS)
        return false;
    in->away = blob[1] & 1;
    in->next_zone = blob[2];
    in->stale_minutes = get16(&blob[3]);
    in->n_rooms = blob[5];
    for (size_t i = 0; i < in->n_rooms; i++) {
        struct install_room *r = &in->rooms[i];
        if (len - at < 5)
            return false;
        r->zone = blob[at];
        r->setpoint = get16(&blob[at + 1]);
        r->away_setpoint = get16(&blob[at + 3]);
        at += 5;
        if (r->zone == 0 || !take_string(blob, len, &at, r->name, INSTALL_NAME_MAX) ||
            !install_name_ok(r->name, strlen(r->name)))
            return false;
    }
    if (at >= len || blob[at] > INSTALL_MAX_DEVICES)
        return false;
    in->n_devices = blob[at++];
    for (size_t i = 0; i < in->n_devices; i++) {
        struct install_device *d = &in->devices[i];
        if (len - at < 9)
            return false;
        memcpy(d->eui, &blob[at], 8);
        d->zone = blob[at + 8];
        at += 9;
        if (!take_string(blob, len, &at, d->serial, INSTALL_SERIAL_MAX) ||
            (d->serial[0] && !ha_serial_ok(d->serial, strlen(d->serial))))
            return false;
    }
    return at == len;
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
