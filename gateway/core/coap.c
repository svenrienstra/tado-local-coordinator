#include "coap.h"

#include <string.h>

#include "bytes.h"

/* Reads an extended option delta or length; false when truncated or reserved. */
static bool extended(const uint8_t *b, size_t len, size_t *i, unsigned nib, unsigned *v)
{
    switch (nib) {
    case 13:
        if (*i + 1 > len)
            return false;
        *v = b[*i] + 13u;
        *i += 1;
        return true;
    case 14:
        if (*i + 2 > len)
            return false;
        *v = (unsigned)(b[*i] << 8 | b[*i + 1]) + 269u;
        *i += 2;
        return true;
    case 15:
        return false;
    default:
        *v = nib;
        return true;
    }
}

bool coap_decode(const uint8_t *b, size_t len, struct coap_msg *m)
{
    memset(m, 0, sizeof *m);
    if (len < 4 || (b[0] & 0xc0) != 0x40)
        return false;
    m->type = (enum coap_type)((b[0] >> 4) & 3);
    m->tkl = b[0] & 0x0f;
    if (m->tkl > 8 || 4u + m->tkl > len)
        return false;
    m->code = b[1];
    m->mid = (uint16_t)(b[2] << 8 | b[3]);
    memcpy(m->token, &b[4], m->tkl);
    size_t i = 4u + m->tkl;
    unsigned number = 0;
    while (i < len && b[i] != 0xff) {
        unsigned head = b[i++], delta, olen;
        if (!extended(b, len, &i, head >> 4, &delta) || !extended(b, len, &i, head & 0x0f, &olen))
            return false;
        if (i + olen > len || m->n_options == COAP_MAX_OPTIONS || number + delta > 0xffff)
            return false;
        number += delta;
        m->options[m->n_options++] = (struct coap_option){
            .number = (uint16_t)number, .len = (uint16_t)olen, .value = &b[i]};
        i += olen;
    }
    if (i < len) {
        m->payload = &b[i + 1];
        m->payload_len = len - i - 1;
    }
    return true;
}

static unsigned nibble(unsigned v) { return v < 13 ? v : v < 269 ? 13 : 14; }

static size_t put_extended(uint8_t *out, unsigned v)
{
    if (v >= 269) {
        out[0] = (uint8_t)((v - 269) >> 8);
        out[1] = (uint8_t)(v - 269);
        return 2;
    }
    if (v >= 13) {
        out[0] = (uint8_t)(v - 13);
        return 1;
    }
    return 0;
}

size_t coap_encode(const struct coap_msg *m, uint8_t *out, size_t cap)
{
    if (m->tkl > 8 || m->n_options > COAP_MAX_OPTIONS)
        return 0;
    /* Stable order by number: insertion sort of indices. */
    size_t order[COAP_MAX_OPTIONS];
    for (size_t i = 0; i < m->n_options; i++) {
        size_t j = i;
        while (j > 0 && m->options[order[j - 1]].number > m->options[i].number) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = i;
    }
    size_t n = 4u + m->tkl;
    if (n > cap)
        return 0;
    out[0] = (uint8_t)(0x40 | m->type << 4 | m->tkl);
    out[1] = m->code;
    out[2] = (uint8_t)(m->mid >> 8);
    out[3] = (uint8_t)m->mid;
    memcpy(&out[4], m->token, m->tkl);
    unsigned last = 0;
    for (size_t k = 0; k < m->n_options; k++) {
        const struct coap_option *o = &m->options[order[k]];
        unsigned delta = o->number - last;
        if (n + 5 + o->len > cap)
            return 0;
        out[n++] = (uint8_t)(nibble(delta) << 4 | nibble(o->len));
        n += put_extended(&out[n], delta);
        n += put_extended(&out[n], o->len);
        memcpy(&out[n], o->value, o->len);
        n += o->len;
        last = o->number;
    }
    if (m->payload_len > 0) {
        if (n + 1 + m->payload_len > cap)
            return 0;
        out[n++] = 0xff;
        memcpy(&out[n], m->payload, m->payload_len);
        n += m->payload_len;
    }
    return n;
}

bool coap_add_option(struct coap_msg *m, uint16_t number, const void *value, size_t len)
{
    if (m->n_options == COAP_MAX_OPTIONS || len > 0xffff)
        return false;
    m->options[m->n_options++] =
        (struct coap_option){.number = number, .len = (uint16_t)len, .value = value};
    return true;
}

bool coap_add_path(struct coap_msg *m, const char *path)
{
    const char *s = path;
    for (;;) {
        const char *e = strchr(s, '/');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        if (!coap_add_option(m, COAP_OPT_URI_PATH, s, n))
            return false;
        if (!e)
            return true;
        s = e + 1;
    }
}

const struct coap_option *coap_find(const struct coap_msg *m, uint16_t number)
{
    for (size_t i = 0; i < m->n_options; i++)
        if (m->options[i].number == number)
            return &m->options[i];
    return NULL;
}

uint32_t coap_uint(const struct coap_option *o) { return get_be(o->value, o->len); }

bool coap_path(const struct coap_msg *m, char *out, size_t cap)
{
    size_t n = 0;
    bool first = true, fits = cap > 0;
    for (size_t i = 0; i < m->n_options && fits; i++) {
        const struct coap_option *o = &m->options[i];
        if (o->number != COAP_OPT_URI_PATH)
            continue;
        size_t need = (first ? 0 : 1) + o->len;
        if (n + need + 1 > cap) {
            fits = false;
            break;
        }
        if (!first)
            out[n++] = '/';
        memcpy(&out[n], o->value, o->len);
        n += o->len;
        first = false;
    }
    if (cap > 0)
        out[n] = '\0';
    return fits;
}

bool coap_path_is(const struct coap_msg *m, const char *path)
{
    const char *s = path;
    bool any = false;
    for (size_t i = 0; i < m->n_options; i++) {
        const struct coap_option *o = &m->options[i];
        if (o->number != COAP_OPT_URI_PATH)
            continue;
        if (any) {
            if (*s != '/')
                return false;
            s++;
        }
        if (strnlen(s, o->len) < o->len || memcmp(s, o->value, o->len) != 0)
            return false;
        s += o->len;
        any = true;
    }
    return *s == '\0';
}
