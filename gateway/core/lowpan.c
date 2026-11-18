#include "lowpan.h"

#include <string.h>

#define IPHC_LINK_LOCAL_0    0x7e /* TF elided, NH compressed, hop limit 64 */
#define IPHC_LINK_LOCAL_1    0x33 /* SAM=3, DAM=3: both from L2 */
#define NHC_UDP_PORTS_INLINE 0xf0
#define IPHC_ICMP_0          0x7b /* TF elided, NH inline, hop limit 255 */
#define IPHC_MCAST_8BIT_1    0x3b /* SAM=3 from L2; M=1, DAM=3: ff02::00XX */

void lowpan_link_local(const uint8_t eui[8], uint8_t out[16])
{
    memset(out, 0, 16);
    out[0] = 0xfe;
    out[1] = 0x80;
    for (int i = 0; i < 8; i++)
        out[8 + i] = eui[7 - i];
    out[8] ^= 0x02;
}

static void link_scope_multicast(uint8_t group, uint8_t out[16])
{
    memset(out, 0, 16);
    out[0] = 0xff;
    out[1] = 0x02;
    out[15] = group;
}

static uint32_t sum16(const uint8_t *b, size_t len)
{
    uint32_t s = 0;
    for (size_t i = 0; i + 1 < len; i += 2)
        s += (uint32_t)(b[i] << 8 | b[i + 1]);
    if (len & 1)
        s += (uint32_t)b[len - 1] << 8;
    return s;
}

uint16_t lowpan_checksum(const uint8_t src[16], const uint8_t dst[16], uint8_t next_header,
                         const uint8_t *upper, size_t len)
{
    uint32_t sum = sum16(src, 16) + sum16(dst, 16);
    sum += (uint32_t)(len >> 16) + (uint32_t)(len & 0xffff);
    sum += next_header;
    sum += sum16(upper, len);
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    uint16_t c = (uint16_t)~sum;
    return next_header == LOWPAN_NH_UDP && c == 0 ? 0xffff : c;
}

size_t lowpan_udp(uint8_t *out, size_t cap, const uint8_t src[8], const uint8_t dst[8],
                  uint16_t src_port, uint16_t dst_port, const uint8_t *payload, size_t len)
{
    size_t total = LOWPAN_UDP_HEADER + len;
    if (total > cap || 8 + len > 0xffff)
        return 0;
    /* The UDP header with a zero checksum goes where the checksum is summed:
     * out[1..8], before the payload at out[9]. */
    uint8_t *udp = &out[1];
    udp[0] = (uint8_t)(src_port >> 8);
    udp[1] = (uint8_t)src_port;
    udp[2] = (uint8_t)(dst_port >> 8);
    udp[3] = (uint8_t)dst_port;
    udp[4] = (uint8_t)((8 + len) >> 8);
    udp[5] = (uint8_t)(8 + len);
    udp[6] = udp[7] = 0;
    memmove(&out[9], payload, len);
    uint8_t s[16], d[16];
    lowpan_link_local(src, s);
    lowpan_link_local(dst, d);
    uint16_t ck = lowpan_checksum(s, d, LOWPAN_NH_UDP, udp, 8 + len);
    /* Compressed: IPHC, NHC, ports, checksum; the length is elided. */
    out[0] = IPHC_LINK_LOCAL_0;
    out[1] = IPHC_LINK_LOCAL_1;
    out[2] = NHC_UDP_PORTS_INLINE;
    out[3] = (uint8_t)(src_port >> 8);
    out[4] = (uint8_t)src_port;
    out[5] = (uint8_t)(dst_port >> 8);
    out[6] = (uint8_t)dst_port;
    out[7] = (uint8_t)(ck >> 8);
    out[8] = (uint8_t)ck;
    return total;
}

size_t lowpan_icmpv6_to_link_multicast(uint8_t *out, size_t cap, const uint8_t src[8],
                                       uint8_t group, const uint8_t *msg, size_t len)
{
    if (4 + len > cap || len < 4)
        return 0;
    memmove(&out[4], msg, len);
    uint8_t *m = &out[4];
    m[2] = m[3] = 0;
    uint8_t s[16], d[16];
    lowpan_link_local(src, s);
    link_scope_multicast(group, d);
    uint16_t ck = lowpan_checksum(s, d, LOWPAN_NH_ICMPV6, m, len);
    m[2] = (uint8_t)(ck >> 8);
    m[3] = (uint8_t)ck;
    out[0] = IPHC_ICMP_0;
    out[1] = IPHC_MCAST_8BIT_1;
    out[2] = LOWPAN_NH_ICMPV6;
    out[3] = group;
    return 4 + len;
}

struct reader {
    const uint8_t *b;
    size_t len, i;
    bool bad;
};

static unsigned u8(struct reader *r)
{
    if (r->i >= r->len) {
        r->bad = true;
        return 0;
    }
    return r->b[r->i++];
}

static unsigned u16(struct reader *r)
{
    unsigned hi = u8(r);
    return hi << 8 | u8(r);
}

static void take(struct reader *r, uint8_t *to, size_t n)
{
    if (r->i + n > r->len) {
        r->bad = true;
        return;
    }
    if (to)
        memcpy(to, &r->b[r->i], n);
    r->i += n;
}

/* One address of the IPHC header; false when it is not reconstructed
 * (context-based) or the mode is reserved (sets r->bad). */
static bool address(struct reader *r, bool stateful, bool multicast, unsigned mode,
                    const uint8_t *l2, uint8_t a[16])
{
    memset(a, 0, 16);
    if (multicast && stateful) {
        /* RFC 6282: only DAM=00 is defined (48 bits inline, prefix from a context). */
        if (mode != 0)
            r->bad = true;
        take(r, NULL, 6);
        return false;
    }
    if (multicast) {
        a[0] = 0xff;
        switch (mode) {
        case 0: take(r, a, 16); break;
        case 1: a[1] = (uint8_t)u8(r); take(r, &a[11], 5); break;
        case 2: a[1] = (uint8_t)u8(r); take(r, &a[13], 3); break;
        default: link_scope_multicast((uint8_t)u8(r), a); break;
        }
        return true;
    }
    if (stateful) {
        /* Context-based: the prefix lives in a context we do not track. */
        take(r, NULL, mode == 1 ? 8 : mode == 2 ? 2 : 0);
        return false;
    }
    a[0] = 0xfe;
    a[1] = 0x80;
    switch (mode) {
    case 0: take(r, a, 16); return true;
    case 1: take(r, &a[8], 8); return true;
    case 2: a[11] = 0xff; a[12] = 0xfe; take(r, &a[14], 2); return true;
    default:
        if (!l2)
            return false;
        lowpan_link_local(l2, a);
        return true;
    }
}

bool lowpan_parse(const uint8_t *p, size_t len, const uint8_t l2_src[8], const uint8_t *l2_dst,
                  struct lowpan_datagram *out)
{
    struct reader r = {.b = p, .len = len};
    memset(out, 0, sizeof *out);
    unsigned b0 = u8(&r), b1 = u8(&r);
    if (r.bad || (b0 & 0xe0) != 0x60)
        return false;
    if (b1 & 0x80)
        take(&r, NULL, 1); /* context identifier extension */
    unsigned tf = (b0 >> 3) & 3;
    take(&r, NULL, tf == 0 ? 4 : tf == 1 ? 3 : tf == 2 ? 1 : 0);
    bool nh_compressed = b0 & 0x04;
    int next_header = nh_compressed ? -1 : (int)u8(&r);
    if ((b0 & 3) == 0)
        take(&r, NULL, 1); /* inline hop limit */

    out->has_src = address(&r, b1 & 0x40, false, (b1 >> 4) & 3, l2_src, out->src);
    out->has_dst = address(&r, b1 & 0x04, b1 & 0x08, b1 & 3, l2_dst, out->dst);

    if (nh_compressed) {
        unsigned nhc = u8(&r);
        if ((nhc & 0xf8) != 0xf0)
            return false; /* only UDP NHC */
        unsigned sp, dp, b;
        switch (nhc & 3) {
        case 0: sp = u16(&r); dp = u16(&r); break;
        case 1: sp = u16(&r); dp = 0xf000 | u8(&r); break;
        case 2: sp = 0xf000 | u8(&r); dp = u16(&r); break;
        default: b = u8(&r); sp = 0xf0b0 | b >> 4; dp = 0xf0b0 | (b & 0xf); break;
        }
        if (!(nhc & 0x04))
            take(&r, NULL, 2); /* inline UDP checksum; the MIC already covers the frame */
        next_header = LOWPAN_NH_UDP;
        out->src_port = (uint16_t)sp;
        out->dst_port = (uint16_t)dp;
    } else if (next_header == LOWPAN_NH_UDP) {
        out->src_port = (uint16_t)u16(&r);
        out->dst_port = (uint16_t)u16(&r);
        take(&r, NULL, 4); /* length and checksum: the frame and its MIC bound the datagram */
    }
    if (r.bad)
        return false;
    out->next_header = next_header;
    out->payload = &p[r.i];
    out->payload_len = len - r.i;
    return true;
}

int lowpan_icmp_type(const struct lowpan_datagram *d)
{
    return d->next_header == LOWPAN_NH_ICMPV6 && d->payload_len > 0 ? d->payload[0] : -1;
}
