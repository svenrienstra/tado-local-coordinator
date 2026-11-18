#include "tado.h"

#include <stdio.h>
#include <string.h>

#define CONTENT_FORMAT_OCTETS 42
#define PAIR_KEY_TAG          0x12 /* plaintext network key (0x07 is cloud-KEK wrapped) */
#define SESSION_LIFETIME_MIN  1440
#define PROTOBUF_FIXED32_F1   0x0d
#define COAP_MAX_MESSAGE      256

/* A bounded writer: once something does not fit, the result is 0. */
struct out {
    uint8_t *p;
    size_t len, cap;
    bool overflow;
};

static void put(struct out *o, const void *b, size_t n)
{
    if (o->overflow || o->len + n > o->cap) {
        o->overflow = true;
        return;
    }
    memcpy(&o->p[o->len], b, n);
    o->len += n;
}

static void tlv(struct out *o, uint16_t tag, const void *value, size_t n)
{
    uint8_t head[3] = {(uint8_t)(tag >> 8), (uint8_t)tag, (uint8_t)n};
    if (n > 0xff) {
        o->overflow = true;
        return;
    }
    put(o, head, 3);
    put(o, value, n);
}

static void tlv_u8(struct out *o, uint16_t tag, uint8_t v) { tlv(o, tag, &v, 1); }

static void tlv_u16(struct out *o, uint16_t tag, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    tlv(o, tag, b, 2);
}

static size_t done(const struct out *o) { return o->overflow ? 0 : o->len; }

size_t tado_device_config(uint8_t *out, size_t cap, uint32_t home_id, const struct tado_zone_role *roles,
                          size_t n_roles)
{
    if (home_id == 0)
        return 0; /* a device applies nothing with 0 */
    struct out o = {.p = out, .cap = cap};
    uint8_t home[4] = {(uint8_t)(home_id >> 24), (uint8_t)(home_id >> 16), (uint8_t)(home_id >> 8),
                       (uint8_t)home_id};
    tlv_u8(&o, 0x0143, 0);
    tlv(&o, 0x015c, home, 4);
    tlv_u8(&o, 0x02b3, 0);
    for (size_t i = 0; i < n_roles; i++) {
        uint8_t r[2] = {roles[i].role, roles[i].zone};
        tlv(&o, 0x015e, r, 2);
    }
    tlv_u16(&o, 0x0158, 0x0200);
    return done(&o);
}

bool tado_zone_uri(char *out, size_t cap, const uint8_t device[8])
{
    uint8_t ll[16];
    lowpan_link_local(device, ll); /* the interface id is the EUI with the U/L bit flipped */
    int n = snprintf(out, cap, "coap://[fe80:0:0:0:%x:%x:%x:%x]/z/", ll[8] << 8 | ll[9], ll[10] << 8 | ll[11],
                     ll[12] << 8 | ll[13], ll[14] << 8 | ll[15]);
    return n > 0 && (size_t)n < cap;
}

static void tlv_uri(struct out *o, uint16_t tag, const char *base, const char *res)
{
    char s[64];
    int n = snprintf(s, sizeof s, "%s%s", base, res);
    if (n < 0 || (size_t)n >= sizeof s) {
        o->overflow = true;
        return;
    }
    tlv(o, tag, s, (size_t)n);
}

size_t tado_zone_with_owner(uint8_t *out, size_t cap, const uint8_t owner[8])
{
    char base[48];
    if (!tado_zone_uri(base, sizeof base, owner))
        return 0;
    struct out o = {.p = out, .cap = cap};
    tlv_u8(&o, 0x6020, 1); /* heating */
    tlv_u8(&o, 0x63e0, 1);
    tlv_uri(&o, 0x63a0, base, "s");
    tlv_uri(&o, 0x8400, base, "p");
    tlv_uri(&o, 0x8000, base, "s");
    tlv_uri(&o, 0x6040, base, "p");
    tlv_u16(&o, 0x6080, 1000); /* Kp */
    tlv_u16(&o, 0x60a0, 500);  /* Ki */
    tlv_u16(&o, 0x60c0, 1500); /* Kd */
    tlv_u8(&o, 0x60e0, 1);     /* open-window detection */
    tlv_u16(&o, 0x62c0, 900);  /* open-window time, s */
    tlv_u16(&o, 0x6380, 900);
    return done(&o);
}

size_t tado_zone_state(uint8_t *out, size_t cap, uint16_t setpoint, int32_t overlay)
{
    struct out o = {.p = out, .cap = cap};
    tlv_u8(&o, 0x6160, 1); /* home */
    tlv_u8(&o, 0x6180, 0);
    tlv_u8(&o, 0x6020, 1); /* heating */
    tlv_u8(&o, 0x61e0, 1); /* on */
    tlv_u16(&o, 0x6200, setpoint);
    if (overlay >= 0) {
        tlv_u8(&o, 0x6240, TADO_OVERLAY_MANUAL);
        tlv_u8(&o, 0x6260, 1); /* on */
        tlv_u16(&o, 0x6280, (uint16_t)overlay);
    } else {
        tlv_u8(&o, 0x6240, 0);
    }
    tlv_u8(&o, 0x62e0, 0); /* window closed */
    return done(&o);
}

bool tado_tlv_find(const uint8_t *body, size_t len, uint16_t tag, const uint8_t **value, size_t *value_len)
{
    bool found = false;
    for (size_t i = 0; i + 3 <= len;) {
        size_t n = body[i + 2];
        if (i + 3 + n > len)
            return false;
        if ((body[i] << 8 | body[i + 1]) == tag) {
            *value = &body[i + 3];
            *value_len = n;
            found = true;
        }
        i += 3 + n;
    }
    return found;
}

uint32_t tado_tlv_uint(const uint8_t *value, size_t len)
{
    uint32_t v = 0;
    for (size_t i = 0; i < len && i < 4; i++)
        v = v << 8 | value[i];
    return v;
}

void tado_etag(const uint8_t *doc, size_t len, uint8_t out[TADO_ETAG_LEN])
{
    uint64_t h = 0xcbf29ce484222325u;
    for (size_t i = 0; i < len; i++) {
        h ^= doc[i];
        h *= 0x100000001b3u;
    }
    for (int i = 7; i >= 0; i--, h >>= 8)
        out[i] = (uint8_t)h;
}

/* A Block2 value in its shortest form (0 is empty). */
static size_t block_value(uint32_t num, bool more, unsigned szx, uint8_t b[3])
{
    uint32_t v = num << 4 | (more ? 8u : 0u) | szx;
    if (v == 0)
        return 0;
    if (v < 0x100) {
        b[0] = (uint8_t)v;
        return 1;
    }
    if (v < 0x10000) {
        b[0] = (uint8_t)(v >> 8);
        b[1] = (uint8_t)v;
        return 2;
    }
    b[0] = (uint8_t)(v >> 16);
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)v;
    return 3;
}

/* Encodes m as a UDP datagram from the coordinator to the device. */
static size_t datagram(uint8_t *out, size_t cap, const struct coap_msg *m, uint16_t src_port, uint16_t dst_port,
                       const uint8_t coordinator[8], const uint8_t device[8])
{
    uint8_t coap[COAP_MAX_MESSAGE];
    size_t n = coap_encode(m, coap, sizeof coap);
    return n ? lowpan_udp(out, cap, coordinator, device, src_port, dst_port, coap, n) : 0;
}

/* A piggybacked ACK echoing MID and token, ports swapped, with no options. */
static size_t answer(uint8_t *out, size_t cap, const struct coap_msg *request, uint8_t code, const uint8_t *body,
                     size_t body_len, const struct lowpan_datagram *from, const uint8_t coordinator[8],
                     const uint8_t device[8])
{
    struct coap_msg m = {.type = COAP_ACK, .code = code, .mid = request->mid, .tkl = request->tkl,
                         .payload = body, .payload_len = body_len};
    memcpy(m.token, request->token, request->tkl);
    return datagram(out, cap, &m, from->dst_port, from->src_port, coordinator, device);
}

size_t tado_block_answer(uint8_t *out, size_t cap, const struct coap_msg *request, const uint8_t *doc,
                         size_t doc_len, const struct lowpan_datagram *from, const uint8_t coordinator[8],
                         const uint8_t device[8])
{
    uint8_t tag[TADO_ETAG_LEN];
    tado_etag(doc, doc_len, tag);
    const struct coap_option *b2 = coap_find(request, COAP_OPT_BLOCK2);
    uint32_t block = b2 ? coap_uint(b2) : TADO_MAX_SZX;
    unsigned asked = block & 7, szx = asked < TADO_MAX_SZX ? asked : TADO_MAX_SZX;
    if (asked == 7)
        return 0; /* reserved block size */
    /* RFC 7959 2.4: in smaller blocks, the asked block starts at our block NUM << (size step). */
    uint32_t num = (block >> 4) << (asked - szx);
    const struct coap_option *held = coap_find(request, COAP_OPT_ETAG);

    struct coap_msg m = {.type = COAP_ACK, .mid = request->mid, .tkl = request->tkl};
    memcpy(m.token, request->token, request->tkl);
    coap_add_option(&m, COAP_OPT_ETAG, tag, sizeof tag);
    uint8_t bv[3];
    if (num == 0 && held && held->len == sizeof tag && memcmp(held->value, tag, sizeof tag) == 0) {
        m.code = COAP_CODE(2, 3);
        coap_add_option(&m, COAP_OPT_BLOCK2, bv, block_value(0, false, szx, bv));
    } else {
        size_t size = 16u << szx;
        size_t from0 = num * size < doc_len ? num * size : doc_len;
        size_t to = from0 + size < doc_len ? from0 + size : doc_len;
        m.code = COAP_CODE(2, 5);
        coap_add_option(&m, COAP_OPT_BLOCK2, bv, block_value(num, to < doc_len, szx, bv));
        m.payload = &doc[from0];
        m.payload_len = to - from0;
    }
    return datagram(out, cap, &m, from->dst_port, from->src_port, coordinator, device);
}

size_t tado_pair_request(uint8_t *out, size_t cap, const uint8_t key[16], uint16_t mid, const uint8_t session[8],
                         const uint8_t coordinator[8], const uint8_t joiner[8])
{
    uint8_t body[18] = {PAIR_KEY_TAG, 16};
    memcpy(&body[2], key, 16);
    static const uint8_t cf = CONTENT_FORMAT_OCTETS;
    struct coap_msg m = {.type = COAP_CON, .code = COAP_POST, .mid = mid, .payload = body,
                         .payload_len = sizeof body};
    coap_add_path(&m, "d/pair");
    coap_add_option(&m, COAP_OPT_CONTENT_FORMAT, &cf, 1);
    coap_add_option(&m, COAP_OPT_SESSION, session, 8);
    return datagram(out, cap, &m, LOWPAN_COAP_PORT, LOWPAN_COAP_PORT, coordinator, joiner);
}

size_t tado_auth_token_answer(uint8_t *out, size_t cap, const struct coap_msg *request, const uint8_t *session_id,
                              size_t session_len, const struct lowpan_datagram *from,
                              const uint8_t coordinator[8], const uint8_t device[8])
{
    if (session_len < 4 || session_len > 8)
        return 0;
    /* Never TLV 0261, the permanent token: the device decrypts it with a
     * per-device key we do not hold and would store garbage. */
    uint8_t body[3 + 8 + 3 + 2];
    struct out o = {.p = body, .cap = sizeof body};
    tlv(&o, 0x025e, session_id, session_len);
    tlv_u16(&o, 0x025f, SESSION_LIFETIME_MIN);
    return answer(out, cap, request, COAP_CODE(2, 5), body, done(&o), from, coordinator, device);
}

size_t tado_time_answer(uint8_t *out, size_t cap, const struct coap_msg *request, uint32_t unix_seconds,
                        const struct lowpan_datagram *from, const uint8_t coordinator[8], const uint8_t device[8])
{
    uint8_t body[5] = {PROTOBUF_FIXED32_F1, (uint8_t)unix_seconds, (uint8_t)(unix_seconds >> 8),
                       (uint8_t)(unix_seconds >> 16), (uint8_t)(unix_seconds >> 24)};
    return answer(out, cap, request, COAP_CODE(2, 5), body, sizeof body, from, coordinator, device);
}

size_t tado_router_advert(uint8_t *out, size_t cap, const uint8_t coordinator[8], const uint8_t gateway_v4[4],
                          const uint8_t dns_v4[4])
{
    /* Hop limit 64, O flag, lifetime 1800 s; SLLAO, MTU 1500, and option 25
     * with the IPv4 netmask, gateway and DNS as IPv4-mapped addresses. */
    uint8_t m[80] = {134, 0, 0, 0, 64, 0x40, 0x07, 0x08};
    size_t n = 16;
    m[n++] = 1;
    m[n++] = 2;
    for (int i = 7; i >= 0; i--)
        m[n++] = coordinator[i];
    n += 6;
    static const uint8_t mtu[8] = {5, 1, 0, 0, 0, 0, 0x05, 0xdc};
    static const uint8_t opt25[8] = {25, 5, 0, 0, 0xff, 0xff, 0xff, 0xff};
    memcpy(&m[n], mtu, 8);
    n += 8;
    memcpy(&m[n], opt25, 8);
    n += 8;
    const uint8_t *v4[2] = {gateway_v4, dns_v4};
    for (int k = 0; k < 2; k++) {
        m[n + 10] = m[n + 11] = 0xff;
        memcpy(&m[n + 12], v4[k], 4);
        n += 16;
    }
    return lowpan_icmpv6_to_link_multicast(out, cap, coordinator, 1, m, n);
}

size_t tado_empty_ack(uint8_t *out, size_t cap, const struct coap_msg *con, const struct lowpan_datagram *from,
                      const uint8_t coordinator[8], const uint8_t device[8])
{
    struct coap_msg m = {.type = COAP_ACK, .code = 0, .mid = con->mid};
    return datagram(out, cap, &m, from->dst_port, from->src_port, coordinator, device);
}

size_t tado_changed_answer(uint8_t *out, size_t cap, const struct coap_msg *request,
                           const struct lowpan_datagram *from, const uint8_t coordinator[8], const uint8_t device[8])
{
    return answer(out, cap, request, COAP_CODE(2, 4), NULL, 0, from, coordinator, device);
}

size_t tado_zone_state_write(uint8_t *out, size_t cap, uint8_t zone, const uint8_t *state, size_t state_len,
                             uint16_t mid, const uint8_t coordinator[8], const uint8_t device[8])
{
    static const uint8_t port[2] = {0xff, 0xff}, cf = CONTENT_FORMAT_OCTETS;
    char query[8];
    int qn = snprintf(query, sizeof query, "id=%u", zone);
    struct coap_msg m = {.type = COAP_CON, .code = COAP_PUT, .mid = mid, .payload = state,
                         .payload_len = state_len};
    coap_add_option(&m, COAP_OPT_URI_PORT, port, 2);
    coap_add_path(&m, "z/s");
    coap_add_option(&m, COAP_OPT_CONTENT_FORMAT, &cf, 1);
    coap_add_option(&m, COAP_OPT_URI_QUERY, query, (size_t)qn);
    return datagram(out, cap, &m, TADO_BRIDGE_PORT, LOWPAN_COAP_PORT, coordinator, device);
}
