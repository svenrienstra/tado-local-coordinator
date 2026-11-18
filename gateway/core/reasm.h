#ifndef REASM_H
#define REASM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Reassembles RFC 4944 fragmented datagrams into one compressed (IPHC)
 * datagram.
 *
 * Size and offsets count bytes of the uncompressed IPv6 datagram. FRAG1
 * carries the compressed headers, which stand for the 40-byte IPv6 header
 * (plus 8 for UDP), and the first payload bytes; each FRAGN carries later
 * payload bytes at its offset. A MAC retransmission delivers a fragment twice,
 * which is ignored; a fragment that conflicts with one already held drops its
 * datagram. */

#define REASM_TIMEOUT_MS 60000u /* RFC 4944 */
#define REASM_MAX_SIZE   1280u
#define REASM_SLOTS      4
#define REASM_FRAG1_MAX  127u   /* a FRAG1 body comes from one MAC frame */

struct reasm_slot {
    bool used;
    uint8_t src[8];
    uint16_t size, tag;
    uint32_t started_ms;
    uint8_t frag1[REASM_FRAG1_MAX]; /* compressed headers and the first payload bytes */
    uint8_t frag1_len;
    uint16_t frag1_covers;          /* uncompressed bytes frag1 stands for; 0 until it came */
    uint8_t later[REASM_MAX_SIZE];  /* FRAGN payload at its uncompressed offset */
    uint8_t have[REASM_MAX_SIZE / 8]; /* 1 per 8-byte unit of later[] held */
};

struct reasm {
    struct reasm_slot slots[REASM_SLOTS];
    uint8_t out[REASM_FRAG1_MAX + REASM_MAX_SIZE];
};

enum reasm_result {
    REASM_WHOLE,    /* *out is a whole datagram: the packet itself or a reassembled one */
    REASM_PENDING,  /* a fragment of a datagram still incomplete */
    REASM_DROPPED,  /* a malformed or conflicting fragment; its datagram is dropped */
};

bool reasm_is_fragment(const uint8_t *p, size_t len);

/* Files one packet from src. A reassembled datagram is returned in r->out,
 * valid until the next call. */
enum reasm_result reasm_add(struct reasm *r, const uint8_t src[8], const uint8_t *p, size_t len,
                            uint32_t now_ms, const uint8_t **out, size_t *out_len);

#endif
