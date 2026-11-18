#include "mac.h"

#include <string.h>

#include "bytes.h"
#include "ccm.h"
#include "crc.h"

/* Frame Control fields, in wire byte order. */
#define FCF_DATA              0x69, 0xec
#define FCF_DATA_PENDING      0x79, 0xec
#define FCF_BCAST             0x49, 0xe8
#define FCF_BCAST_PENDING     0x59, 0xe8
#define FCF_BEACON_REQ        0x0b, 0xe8
#define FCF_ENH_BEACON        0x18, 0xec
#define FCF_ENH_ACK           0x42, 0xee
#define FCF_WAKEUP            0x25
#define FCF0_ACK_REQUEST      0x20
#define MAC_CMD_BEACON_REQ    0x07
/* Sealed part of a beacon request: counter(4) command(1) FCS(2). */
#define BEACON_REQ_PT_LEN     (MAC_BEACON_REQ_LEN - MAC_HEADER_LEN - CCM_MIC_LEN)

/* Header IEs: Rendezvous Time, CSL, Header Termination. */
static const uint8_t ie_rendezvous[2] = {0x82, 0x0e};
static const uint8_t ie_csl[2] = {0x04, 0x0d};
static const uint8_t ie_term[2] = {0x80, 0x3f};

const uint8_t MAC_TADO_EUI_SUFFIX[3] = {0xc5, 0x1b, 0x00};

static bool fcf_is(const uint8_t *f, uint8_t a, uint8_t b) { return f[0] == a && f[1] == b; }

/* The FCS of a secured frame covers the clear header and the plaintext before
 * it, and travels encrypted in the last two plaintext bytes (pt_len >= 2). */
static uint16_t frame_fcs(const uint8_t *header, const uint8_t *pt, size_t pt_len)
{
    return crc16_mac_fcs(crc16_mac_fcs(0, header, MAC_HEADER_LEN), pt, pt_len - 2);
}

/* Seals frame[MAC_HEADER_LEN, len) in place and appends the MIC. */
static size_t seal(uint8_t *frame, size_t len, const uint8_t key[16])
{
    put16(&frame[len - 2], frame_fcs(frame, &frame[MAC_HEADER_LEN], len - MAC_HEADER_LEN));
    ccm_seal(key, frame, frame, MAC_HEADER_LEN, &frame[MAC_HEADER_LEN], len - MAC_HEADER_LEN,
             &frame[len]);
    return len + CCM_MIC_LEN;
}

/* Opens a sealed frame into scratch; returns the plaintext length or -1. */
static int open_frame(const uint8_t *frame, size_t len, const uint8_t key[16], uint8_t *scratch)
{
    if (len < MAC_HEADER_LEN + 2 + CCM_MIC_LEN || len > MAC_MAX_FRAME)
        return -1;
    size_t pt_len = len - MAC_HEADER_LEN - CCM_MIC_LEN;
    memcpy(scratch, &frame[MAC_HEADER_LEN], pt_len);
    if (!ccm_open(key, frame, frame, MAC_HEADER_LEN, scratch, pt_len, &frame[len - CCM_MIC_LEN]))
        return -1;
    if (get16(&scratch[pt_len - 2]) != frame_fcs(frame, scratch, pt_len))
        return -1;
    return (int)pt_len;
}

enum mac_kind mac_classify(const uint8_t *f, size_t len)
{
    if (len < 2)
        return MAC_KIND_OTHER;
    if (fcf_is(f, FCF_BEACON_REQ) && len == MAC_BEACON_REQ_LEN)
        return MAC_KIND_BEACON_REQUEST;
    if (fcf_is(f, FCF_DATA) || fcf_is(f, FCF_DATA_PENDING))
        return MAC_KIND_UNICAST;
    if (fcf_is(f, FCF_BCAST) || fcf_is(f, FCF_BCAST_PENDING))
        return MAC_KIND_BROADCAST;
    if (fcf_is(f, FCF_ENH_ACK) && len == MAC_ENH_ACK_LEN)
        return MAC_KIND_ENH_ACK;
    return MAC_KIND_OTHER;
}

void mac_short_addr(const uint8_t eui[8], uint8_t out[2])
{
    out[0] = eui[1];
    out[1] = eui[0];
}

void mac_build_wakeup(uint8_t out[MAC_WAKEUP_LEN], uint8_t seq, const uint8_t dst_short[2],
                      uint16_t rendezvous)
{
    out[0] = FCF_WAKEUP;
    out[1] = seq;
    put16(&out[2], MAC_PAN_ID);
    memcpy(&out[4], dst_short, 2);
    memcpy(&out[6], ie_rendezvous, 2);
    put16(&out[8], rendezvous);
    memcpy(&out[10], ie_term, 2);
}

uint16_t mac_train_rendezvous(int i, int n)
{
    if (i >= n - 1)
        return 0;
    if (n == MAC_TRAIN_FULL) /* 253 frames spread 3125..1 evenly, then 0 */
        return (uint16_t)(1 + (uint32_t)(MAC_CSL_PERIOD - 1) * (uint32_t)(MAC_TRAIN_FULL - 2 - i) /
                                  (MAC_TRAIN_FULL - 2));
    return (uint16_t)((uint32_t)(n - 1 - i) * 62u / 5u);
}

uint16_t mac_enh_ack_phase(const uint8_t ack[MAC_ENH_ACK_LEN])
{
    return get16(&ack[21]);
}

void mac_build_enh_ack(uint8_t out[MAC_ENH_ACK_LEN], uint8_t seq, const uint8_t dst[8],
                       const uint8_t src[8], uint16_t csl_phase)
{
    static const uint8_t fcf[2] = {FCF_ENH_ACK};
    memcpy(out, fcf, 2);
    out[2] = seq;
    memcpy(&out[3], dst, 8);
    memcpy(&out[11], src, 8);
    memcpy(&out[19], ie_csl, 2);
    put16(&out[21], csl_phase);
    put16(&out[23], MAC_CSL_PERIOD);
    memcpy(&out[25], ie_term, 2);
    put16(&out[27], crc16_mac_fcs(0, out, 27));
}

bool mac_parse_enh_ack(const uint8_t *f, size_t len, uint8_t *seq, uint8_t dst[8])
{
    if (mac_classify(f, len) != MAC_KIND_ENH_ACK || get16(&f[27]) != crc16_mac_fcs(0, f, 27))
        return false;
    *seq = f[2];
    memcpy(dst, &f[3], 8);
    return true;
}

void mac_build_enh_beacon(uint8_t out[MAC_ENH_BEACON_LEN], const uint8_t key[16], uint8_t seq,
                          const uint8_t dst[8], const uint8_t src[8], uint32_t counter)
{
    /* No IE, no payload: the source EUI runs past the clear header into the
     * sealed part, followed by the security control and the counter. */
    static const uint8_t fcf[2] = {FCF_ENH_BEACON};
    memcpy(out, fcf, 2);
    out[2] = seq;
    put16(&out[3], MAC_PAN_ID);
    memcpy(&out[5], dst, 8);
    memcpy(&out[13], src, 8);
    out[21] = MAC_SEC_CTRL;
    put32(&out[22], counter);
    seal(out, 28, key);
}

bool mac_parse_beacon_request(const uint8_t *f, size_t len, const uint8_t key[16],
                              uint8_t *seq, uint8_t src[8], uint32_t *counter)
{
    uint8_t pt[BEACON_REQ_PT_LEN];
    /* The classify pins len, so the plaintext fits pt exactly. */
    if (mac_classify(f, len) != MAC_KIND_BEACON_REQUEST)
        return false;
    if (open_frame(f, len, key, pt) != BEACON_REQ_PT_LEN || pt[4] != MAC_CMD_BEACON_REQ)
        return false;
    *seq = f[2];
    memcpy(src, &f[7], 8);
    *counter = get32(pt);
    return true;
}

size_t mac_seal_unicast(uint8_t out[MAC_MAX_FRAME], const uint8_t key[16], bool pending,
                        uint8_t seq, const uint8_t dst[8], const uint8_t src[8],
                        uint32_t counter, const uint8_t *packet, size_t packet_len)
{
    if (packet_len > MAC_UNICAST_MAX_PACKET)
        return 0;
    static const uint8_t data[2] = {FCF_DATA}, data_pending[2] = {FCF_DATA_PENDING};
    memcpy(out, pending ? data_pending : data, 2);
    out[2] = seq;
    memcpy(&out[3], dst, 8);
    /* Header: first 5 source bytes. Plaintext: the other 3 (the tado suffix),
     * security control, counter, packet, FCS. */
    memcpy(&out[11], src, 8);
    out[19] = MAC_SEC_CTRL;
    put32(&out[20], counter);
    memcpy(&out[24], packet, packet_len);
    return seal(out, 24 + packet_len + 2, key);
}

size_t mac_seal_broadcast(uint8_t out[MAC_MAX_FRAME], const uint8_t key[16], bool pending,
                          uint8_t seq, const uint8_t src[8], uint32_t counter,
                          const uint8_t *packet, size_t packet_len)
{
    if (packet_len > MAC_BROADCAST_MAX_PACKET)
        return 0;
    static const uint8_t bc[2] = {FCF_BCAST}, bc_pending[2] = {FCF_BCAST_PENDING};
    memcpy(out, pending ? bc_pending : bc, 2);
    out[2] = seq;
    out[3] = out[4] = 0xff;
    memcpy(&out[5], src, 8);
    out[13] = MAC_SEC_CTRL;
    /* The counter is split: low half in the clear header, high half first in
     * the plaintext. */
    put16(&out[14], (uint16_t)counter);
    put16(&out[16], (uint16_t)(counter >> 16));
    memcpy(&out[18], packet, packet_len);
    return seal(out, 18 + packet_len + 2, key);
}

bool mac_data_addressed_to(const uint8_t *f, size_t len, enum mac_kind kind,
                           const uint8_t eui[8])
{
    if (len <= MAC_HEADER_LEN)
        return false;
    return kind == MAC_KIND_BROADCAST || (kind == MAC_KIND_UNICAST && memcmp(&f[3], eui, 8) == 0);
}

bool mac_ack_request(const uint8_t *f, size_t len, const uint8_t eui[8], uint8_t *seq,
                     uint8_t src[8])
{
    if (mac_classify(f, len) != MAC_KIND_UNICAST ||
        !mac_data_addressed_to(f, len, MAC_KIND_UNICAST, eui) || !(f[0] & FCF0_ACK_REQUEST))
        return false;
    *seq = f[2];
    memcpy(src, &f[11], 5);
    memcpy(&src[5], MAC_TADO_EUI_SUFFIX, 3);
    return true;
}

bool mac_open_data(const uint8_t *f, size_t len, const uint8_t key[16],
                   uint8_t scratch[MAC_MAX_FRAME], struct mac_data *out)
{
    enum mac_kind kind = mac_classify(f, len);
    if (kind != MAC_KIND_UNICAST && kind != MAC_KIND_BROADCAST)
        return false;
    int pt_len = open_frame(f, len, key, scratch);
    out->seq = f[2];
    if (kind == MAC_KIND_UNICAST) {
        if (pt_len < 8 + 2)
            return false;
        out->broadcast = false;
        out->ack_request = (f[0] & FCF0_ACK_REQUEST) != 0;
        memcpy(out->dst, &f[3], 8);
        memcpy(out->src, &f[11], 5);
        memcpy(&out->src[5], scratch, 3);
        out->counter = get32(&scratch[4]);
        out->packet = &scratch[8];
        out->packet_len = (size_t)pt_len - 8 - 2;
    } else {
        if (pt_len < 2 + 2)
            return false;
        out->broadcast = true;
        out->ack_request = false;
        memset(out->dst, 0xff, 8);
        memcpy(out->src, &f[5], 8);
        out->counter = get16(&f[14]) | (uint32_t)get16(scratch) << 16;
        out->packet = &scratch[2];
        out->packet_len = (size_t)pt_len - 2 - 2;
    }
    return true;
}
