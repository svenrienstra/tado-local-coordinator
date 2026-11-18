#include "reasm.h"

#include <string.h>

#include "lowpan.h"

#define FRAG1        0xc0
#define FRAGN        0xe0
#define FRAG1_HEADER 4
#define FRAGN_HEADER 5
#define IPV6_HEADER  40
#define UDP_HEADER   8

bool reasm_is_fragment(const uint8_t *p, size_t len)
{
    unsigned d = len > 0 ? p[0] & 0xf8 : 0;
    return d == FRAG1 || d == FRAGN;
}

static bool unit_held(const struct reasm_slot *s, size_t u) { return s->have[u / 8] >> (u % 8) & 1; }

/* Whether the units [from, to) of later[] are all held, or none. */
static int units_held(const struct reasm_slot *s, size_t from, size_t to)
{
    size_t n = 0;
    for (size_t u = from; u < to; u++)
        n += unit_held(s, u);
    return n == 0 ? 0 : n == to - from ? 1 : -1;
}

static bool add_first(struct reasm_slot *s, const uint8_t *body, size_t len)
{
    if (s->frag1_covers != 0)
        return s->frag1_len == len && memcmp(s->frag1, body, len) == 0; /* a repeat */
    if (len > sizeof s->frag1)
        return false;
    struct lowpan_datagram head;
    if (!lowpan_parse(body, len, s->src, NULL, &head))
        return false;
    size_t covers = IPV6_HEADER + (head.next_header == LOWPAN_NH_UDP ? UDP_HEADER : 0) + head.payload_len;
    if (covers > s->size || covers % 8 != 0)
        return false;
    /* No later piece may start inside what frag1 stands for. */
    if (units_held(s, 0, covers / 8) != 0)
        return false;
    memcpy(s->frag1, body, len);
    s->frag1_len = (uint8_t)len;
    s->frag1_covers = (uint16_t)covers;
    return true;
}

static bool add_later(struct reasm_slot *s, size_t offset, const uint8_t *data, size_t len)
{
    if (len == 0 || offset + len > s->size || offset < s->frag1_covers)
        return false;
    size_t from = offset / 8, to = (offset + len + 7) / 8;
    int held = units_held(s, from, to);
    if (held == 1)
        return memcmp(&s->later[offset], data, len) == 0; /* a repeat */
    if (held != 0)
        return false;
    memcpy(&s->later[offset], data, len);
    for (size_t u = from; u < to; u++)
        s->have[u / 8] |= (uint8_t)(1u << (u % 8));
    return true;
}

static bool complete(const struct reasm_slot *s)
{
    return s->frag1_covers != 0 && units_held(s, s->frag1_covers / 8, (s->size + 7u) / 8) == 1;
}

enum reasm_result reasm_add(struct reasm *r, const uint8_t src[8], const uint8_t *p, size_t len,
                            uint32_t now_ms, const uint8_t **out, size_t *out_len)
{
    if (!reasm_is_fragment(p, len)) {
        *out = p;
        *out_len = len;
        return REASM_WHOLE;
    }
    for (size_t i = 0; i < REASM_SLOTS; i++)
        if (r->slots[i].used && now_ms - r->slots[i].started_ms >= REASM_TIMEOUT_MS)
            r->slots[i].used = false;
    bool first = (p[0] & 0xf8) == FRAG1;
    if (len < (first ? FRAG1_HEADER : FRAGN_HEADER))
        return REASM_DROPPED;
    uint16_t size = (uint16_t)((p[0] & 0x07) << 8 | p[1]);
    uint16_t tag = (uint16_t)(p[2] << 8 | p[3]);
    if (size > REASM_MAX_SIZE || size <= IPV6_HEADER)
        return REASM_DROPPED;

    struct reasm_slot *s = NULL, *victim = &r->slots[0];
    for (size_t i = 0; i < REASM_SLOTS; i++) {
        struct reasm_slot *x = &r->slots[i];
        if (x->used && x->size == size && x->tag == tag && memcmp(x->src, src, 8) == 0)
            s = x;
        else if (!x->used || (victim->used && now_ms - x->started_ms > now_ms - victim->started_ms))
            victim = x; /* a free slot, else the oldest datagram */
    }
    if (!s) {
        s = victim;
        memset(s, 0, sizeof *s);
        s->used = true;
        memcpy(s->src, src, 8);
        s->size = size;
        s->tag = tag;
        s->started_ms = now_ms;
    }
    bool ok = first ? add_first(s, &p[FRAG1_HEADER], len - FRAG1_HEADER)
                    : add_later(s, (size_t)p[4] * 8, &p[FRAGN_HEADER], len - FRAGN_HEADER);
    if (!ok) {
        s->used = false;
        return REASM_DROPPED;
    }
    if (!complete(s))
        return REASM_PENDING;
    memcpy(r->out, s->frag1, s->frag1_len);
    memcpy(&r->out[s->frag1_len], &s->later[s->frag1_covers], s->size - s->frag1_covers);
    *out = r->out;
    *out_len = s->frag1_len + (size_t)(s->size - s->frag1_covers);
    s->used = false;
    return REASM_WHOLE;
}
