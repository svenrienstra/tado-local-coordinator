#ifndef LOWPAN_H
#define LOWPAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 6LoWPAN (RFC 6282 IPHC) datagrams as tado nodes send them.
 *
 * The coordinator's own requests and answers use one form: link-local
 * addresses elided from the L2 addresses, hop limit 64, UDP ports and checksum
 * inline (7e 33 f0). Inbound datagrams are parsed for any stateless form;
 * addresses compressed against a context (zone plane) are not reconstructed.
 * EUIs are 8 bytes in wire (little-endian) order, as in MAC headers. */

#define LOWPAN_NH_UDP    17
#define LOWPAN_NH_ICMPV6 58
#define LOWPAN_COAP_PORT 5683
/* The coordinator's link-local form adds 9 bytes of headers to a UDP payload. */
#define LOWPAN_UDP_HEADER 9

/* fe80::/64 with the interface id from the big-endian EUI, U/L bit flipped. */
void lowpan_link_local(const uint8_t eui[8], uint8_t out[16]);

/* A link-local UDP datagram from src to dst, as the bridge builds it. Returns
 * its length, or 0 when it does not fit cap. */
size_t lowpan_udp(uint8_t *out, size_t cap, const uint8_t src[8], const uint8_t dst[8],
                  uint16_t src_port, uint16_t dst_port, const uint8_t *payload, size_t len);

/* An ICMPv6 datagram from src to ff02::group, hop limit 255 (RFC 4861). msg
 * holds a zero checksum field; the copy in out gets the checksum. Returns its
 * length, or 0 when it does not fit cap. */
size_t lowpan_icmpv6_to_link_multicast(uint8_t *out, size_t cap, const uint8_t src[8],
                                       uint8_t group, const uint8_t *msg, size_t len);

struct lowpan_datagram {
    int next_header;     /* LOWPAN_NH_UDP, LOWPAN_NH_ICMPV6, or another value, unparsed */
    bool has_src, has_dst;
    uint8_t src[16], dst[16];
    uint16_t src_port, dst_port; /* UDP only, else 0 */
    const uint8_t *payload;      /* the UDP payload, or the whole ICMPv6 message */
    size_t payload_len;          /* points into the parsed packet */
};

/* Parses an inbound datagram. l2_src and l2_dst are the MAC addresses the
 * elided IPv6 addresses derive from (l2_dst NULL for a broadcast). False for a
 * truncated or unsupported datagram. */
bool lowpan_parse(const uint8_t *p, size_t len, const uint8_t l2_src[8], const uint8_t *l2_dst,
                  struct lowpan_datagram *out);

/* The ICMPv6 type of an ICMPv6 datagram, else -1. */
int lowpan_icmp_type(const struct lowpan_datagram *d);

/* RFC 8200 upper-layer checksum over the pseudo-header and upper, whose
 * checksum field must be zero. A UDP result of 0 is sent as 0xffff. */
uint16_t lowpan_checksum(const uint8_t src[16], const uint8_t dst[16], uint8_t next_header,
                         const uint8_t *upper, size_t len);

#endif
