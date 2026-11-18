#include "tado.h"

#include <stdio.h>
#include <string.h>

#include "bytes.h"

#define CONTENT_FORMAT_OCTETS 42
#define PAIR_KEY_TAG          0x12 /* plaintext network key (0x07 is cloud-KEK wrapped) */
#define SESSION_LIFETIME_MIN  1440
#define PROTOBUF_FIXED32_F1   0x0d

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

size_t tado_device_document(uint8_t *out, size_t cap, const struct tado_device_doc *d)
{
    if (d->home_id == 0)
        return 0; /* a device applies nothing with 0 */
    struct out o = {.p = out, .cap = cap};
    uint8_t home[4] = {(uint8_t)(d->home_id >> 24), (uint8_t)(d->home_id >> 16), (uint8_t)(d->home_id >> 8),
                       (uint8_t)d->home_id};
    tlv_u8(&o, 0x0143, 0); /* trace off */
    if (d->module)
        tlv_u16(&o, 0x015d, d->module);
    tlv(&o, 0x015c, home, 4);
    tlv_u8(&o, 0x02b3, d->offline_schedule);
    if (d->has_utc_offset)
        tlv_u16(&o, 0x02b2, (uint16_t)d->utc_offset_min);
    if (d->shape == TADO_DOC_VALVE) {
        tlv_u16(&o, 0x021a, 0); /* a constant no store reads */
        tlv_u8(&o, 0x0149, 0);  /* horizontal */
    }
    for (size_t i = 0; i < d->n_roles; i++) {
        uint8_t r[2] = {d->roles[i].role, d->roles[i].zone};
        tlv(&o, 0x015e, r, 2);
    }
    tlv_u16(&o, 0x0158, d->shape == TADO_DOC_THERMOSTAT ? 0x0000 : 0x0200); /* notify on a remote change */
    return done(&o);
}

size_t tado_device_config(uint8_t *out, size_t cap, uint32_t home_id, const struct tado_zone_role *roles,
                          size_t n_roles)
{
    struct tado_device_doc d = {.shape = TADO_DOC_SENSOR, .home_id = home_id, .roles = roles, .n_roles = n_roles};
    return tado_device_document(out, cap, &d);
}

const char *tado_module_family(uint16_t module)
{
    static const struct {
        uint16_t id;
        const char *family;
    } FAMILIES[] = {
        {71, "opentherm"},
        {0, "ebus_a"},   {1, "ebus_a"},   {5, "ebus_a"},   {6, "ebus_a"},   {7, "ebus_a"},   {9, "ebus_a"},
        {80, "ebus_a"},  {83, "ebus_a"},  {200, "ebus_a"},
        {121, "ebus_b"},
        {4, "ebus_c"},   {122, "ebus_c"},
        {20, "km"},      {22, "km"},      {23, "km"},      {24, "km"},      {50, "km"},
        {41, "ht_bus"},  {43, "ht_bus"},  {44, "ht_bus"},
        {120, "ems"},
        {100, "bsb"},    {110, "bsb"},    {111, "bsb"},    {123, "bsb"},
        {31, "scom"},    {32, "scom"},
        {3, "analog"},   {10, "analog"},  {11, "analog"},  {12, "analog"},  {30, "analog"},
        {40, "analog"},  {42, "analog"},  {60, "analog"},  {61, "analog"},
        {202, "stub"}, /* drives no boiler bus */
    };
    for (size_t i = 0; i < sizeof FAMILIES / sizeof FAMILIES[0]; i++)
        if (FAMILIES[i].id == module)
            return FAMILIES[i].family;
    return NULL;
}

bool tado_thermostat_module_ok(uint16_t module)
{
    return module != 0 && module != 0x7fff && module != TADO_VALVE_MODULE && tado_module_family(module) != NULL;
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

/* A device's endpoint coap://[fe80::...]/z/<res>; false when it does not fit. */
static bool endpoint(struct out *o, uint16_t tag, const uint8_t eui[8], const char *res)
{
    char base[48];
    if (!tado_zone_uri(base, sizeof base, eui))
        return false;
    tlv_uri(o, tag, base, res);
    return true;
}

size_t tado_zone_document(uint8_t *out, size_t cap, const struct tado_zone_doc *z)
{
    if (z->n_members == 0)
        return 0;
    const uint8_t *owner = z->members[0].eui;
    struct out o = {.p = out, .cap = cap};
    bool ok = true;
    tlv_u8(&o, 0x6020, z->hot_water ? 2 : 1);
    tlv_u8(&o, 0x63e0, 1); /* enabled */
    ok &= endpoint(&o, 0x63a0, owner, "s");
    for (size_t i = 0; !z->hot_water && i < z->n_members; i++)
        if (z->members[i].valve)
            ok &= endpoint(&o, 0x8200, z->members[i].eui, "cpe");
    for (size_t i = 0; i < z->n_members; i++)
        ok &= endpoint(&o, 0x8400, z->members[i].eui, "p");
    if (z->hot_water)
        ok &= endpoint(&o, 0x8400, owner, "p");
    bool circuit = !z->hot_water && z->heat_source;
    bool source_is_member = false;
    for (size_t i = 0; circuit && i < z->n_members; i++)
        source_is_member |= memcmp(z->members[i].eui, z->heat_source, 8) == 0;
    if (circuit && !source_is_member)
        ok &= endpoint(&o, 0x8000, z->heat_source, "s");
    for (size_t i = 0; i < z->n_members; i++)
        ok &= endpoint(&o, 0x8000, z->members[i].eui, "s");
    if (!z->hot_water) {
        if (circuit)
            tlv_u8(&o, 0x6060, z->circuit);
        ok &= endpoint(&o, 0x6040, owner, "p");
        tlv_u16(&o, 0x6080, 1000); /* Kp */
        tlv_u16(&o, 0x60a0, 500);  /* Ki */
        tlv_u16(&o, 0x60c0, 1500); /* Kd */
        tlv_u8(&o, 0x60e0, 1);     /* open-window detection */
        tlv_u16(&o, 0x62c0, 900);  /* open-window time, s */
        tlv_u16(&o, 0x6380, 900);  /* refresh, s */
    }
    return ok ? done(&o) : 0;
}

size_t tado_zone_with_owner(uint8_t *out, size_t cap, const uint8_t owner[8])
{
    struct tado_zone_member m = {.valve = false};
    memcpy(m.eui, owner, 8);
    struct tado_zone_doc z = {.members = &m, .n_members = 1};
    return tado_zone_document(out, cap, &z);
}

size_t tado_zone_block(uint8_t *out, size_t cap, const struct tado_zone_block *b)
{
    struct out o = {.p = out, .cap = cap};
    tlv_u8(&o, 0x6160, b->away ? 2 : 1);
    tlv_u8(&o, 0x6180, 0);
    tlv_u8(&o, 0x6020, 1); /* heating */
    tlv_u8(&o, 0x61e0, b->on);
    if (b->on)
        tlv_u16(&o, 0x6200, b->setpoint);
    tlv_u8(&o, 0x6240, b->overlay);
    if (b->overlay) {
        tlv_u8(&o, 0x6260, b->overlay_on);
        if (b->overlay_on)
            tlv_u16(&o, 0x6280, b->overlay_setpoint);
    }
    tlv_u8(&o, 0x62e0, 0); /* window closed */
    if (b->marker)
        tlv_u16(&o, 0x6440, 1);
    return done(&o);
}

bool tado_report_decode(const uint8_t *body, size_t len, struct tado_report *r)
{
    memset(r, 0, sizeof *r);
    size_t i = 0;
    while (i + 3 <= len) {
        unsigned tag = (unsigned)(body[i] << 8 | body[i + 1]);
        size_t n = body[i + 2];
        if (i + 3 + n > len)
            return false;
        const uint8_t *v = &body[i + 3];
        uint32_t u = get_be(v, n);
        i += 3 + n;
        if (n == 0 || n > 2)
            continue; /* every known tag is 1 or 2 bytes */
        switch (tag) {
        case 0x012d: r->temperature = (int16_t)u; r->fields |= TADO_TEMPERATURE; break;
        case 0x012e: r->temperature_raw = (int16_t)u; r->fields |= TADO_TEMPERATURE_RAW; break;
        case 0x4060: r->zone_temperature = (int16_t)u; r->fields |= TADO_ZONE_TEMPERATURE; break;
        case 0x0135: r->humidity = (uint16_t)u; r->fields |= TADO_HUMIDITY; break;
        case 0x0162: r->battery_mv = (uint16_t)u; r->fields |= TADO_BATTERY; break;
        case 0x0136: r->light = (uint8_t)u; r->fields |= TADO_LIGHT; break;
        case 0x40a0: r->demand = (uint8_t)u; r->fields |= TADO_DEMAND; break;
        case 0x6200: r->setpoint = (uint16_t)u; r->fields |= TADO_SETPOINT; break;
        case 0x6240: r->overlay = (uint8_t)u; r->fields |= TADO_OVERLAY; break;
        case 0x6260: r->overlay_on = u != 0; r->fields |= TADO_OVERLAY_ON; break;
        case 0x6280: r->overlay_setpoint = (uint16_t)u; r->fields |= TADO_OVERLAY_SETPOINT; break;
        case 0x61e0: r->on = u != 0; r->fields |= TADO_ON; break;
        default: break;
        }
    }
    return i == len;
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

/* Encodes m as a UDP datagram from the coordinator to the device: the CoAP
 * message goes straight where the datagram carries it, behind its headers. */
static size_t datagram(uint8_t *out, size_t cap, const struct coap_msg *m, uint16_t src_port, uint16_t dst_port,
                       const uint8_t coordinator[8], const uint8_t device[8])
{
    if (cap <= LOWPAN_UDP_HEADER)
        return 0;
    uint8_t *coap = &out[LOWPAN_UDP_HEADER];
    size_t n = coap_encode(m, coap, cap - LOWPAN_UDP_HEADER);
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
    /* The sensor keeps asking with SZX 3 but numbers the blocks in the 64-byte
     * size we answer (03 13 23 33 for a 226-byte document, captured
     * 2026-11-17), so NUM is ours as it comes. Scaling it as RFC 7959 2.4
     * reads (block 1 of 128 = our block 2) made it start over. */
    uint32_t num = block >> 4;
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
    uint8_t body[5] = {PROTOBUF_FIXED32_F1};
    put32(&body[1], unix_seconds);
    return answer(out, cap, request, COAP_CODE(2, 5), body, sizeof body, from, coordinator, device);
}

size_t tado_router_advert(uint8_t *out, size_t cap, const uint8_t coordinator[8], const uint8_t gateway_v4[4],
                          const uint8_t dns_v4[4])
{
    /* Hop limit 64, O flag, lifetime 1800 s; SLLAO, MTU 1500, and option 25
     * with the IPv4 netmask, gateway and DNS as IPv4-mapped addresses. Built
     * where the datagram carries it, behind its 4 header bytes. */
    if (cap < 4 + 80)
        return 0;
    uint8_t *m = &out[4];
    static const uint8_t head[8] = {134, 0, 0, 0, 64, 0x40, 0x07, 0x08};
    memset(m, 0, 80);
    memcpy(m, head, sizeof head);
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
