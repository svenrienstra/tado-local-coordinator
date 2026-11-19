#ifndef MAC_H
#define MAC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* tado MAC frames, as freedo's tado/ builders produce them (see docs/JOIN.md).
 * All frames are without the PHY CRC, which the radio adds. EUIs are 8 bytes
 * in wire order and end in the tado suffix c5 1b 00. */

#define MAC_MAX_FRAME      127
#define MAC_PAN_ID         0xABCDu
#define MAC_CSL_PERIOD     3125u /* units of 320 us = 1.000 s */
#define MAC_HEADER_LEN     16    /* the aad; the nonce is its first 13 bytes */
#define MAC_SEC_CTRL       0x04
#define MAC_WAKEUP_LEN     12
#define MAC_TRAIN_FULL     254   /* wake-up frames of a full (one period) train */
#define MAC_ENH_ACK_LEN    29
#define MAC_ENH_BEACON_LEN 32
#define MAC_BEACON_REQ_LEN 27

/* The last 3 bytes of every tado EUI-64, as they go on air. */
extern const uint8_t MAC_TADO_EUI_SUFFIX[3];

/* Largest 6LoWPAN packet that still fits one frame. */
#define MAC_UNICAST_MAX_PACKET   (MAC_MAX_FRAME - MAC_HEADER_LEN - 8 - 2 - 4)
#define MAC_BROADCAST_MAX_PACKET (MAC_MAX_FRAME - MAC_HEADER_LEN - 2 - 2 - 4)

enum mac_kind {
    MAC_KIND_OTHER,
    MAC_KIND_BEACON_REQUEST,
    MAC_KIND_UNICAST,   /* Data 69ec / 79ec */
    MAC_KIND_BROADCAST, /* Data 49e8 / 59e8 */
    MAC_KIND_ENH_ACK,
};

enum mac_kind mac_classify(const uint8_t *frame, size_t len);

/* Wake-up short address of a wire EUI: {eui[1], eui[0]}. */
void mac_short_addr(const uint8_t eui[8], uint8_t out[2]);

void mac_build_wakeup(uint8_t out[MAC_WAKEUP_LEN], uint8_t seq, const uint8_t dst_short[2],
                      uint16_t rendezvous);

/* Rendezvous time of wake-up frame i of an n-frame train, as the commercial
 * bridge sends them: a full train spreads 3125 down to 0, a shorter one steps
 * 12.4 units (the 3968 us frame step), 248 down to 0 for 21 frames. */
uint16_t mac_train_rendezvous(int i, int n);

void mac_build_enh_ack(uint8_t out[MAC_ENH_ACK_LEN], uint8_t seq, const uint8_t dst[8],
                       const uint8_t src[8], uint16_t csl_phase);

void mac_build_enh_beacon(uint8_t out[MAC_ENH_BEACON_LEN], const uint8_t key[16], uint8_t seq,
                          const uint8_t dst[8], const uint8_t src[8], uint32_t counter);

/* Verifies an Enhanced Beacon Request under key. */
bool mac_parse_beacon_request(const uint8_t *frame, size_t len, const uint8_t key[16],
                              uint8_t *seq, uint8_t src[8], uint32_t *counter);

/* Builds the Enhanced Beacon Request a joiner broadcasts (device side). */
void mac_build_beacon_request(uint8_t out[MAC_BEACON_REQ_LEN], const uint8_t key[16], uint8_t seq,
                              const uint8_t src[8], uint32_t counter);

/* Seal a packet into a Data frame. Return the frame length, or 0 when the
 * packet does not fit. */
size_t mac_seal_unicast(uint8_t out[MAC_MAX_FRAME], const uint8_t key[16], bool pending,
                        uint8_t seq, const uint8_t dst[8], const uint8_t src[8],
                        uint32_t counter, const uint8_t *packet, size_t packet_len);
size_t mac_seal_broadcast(uint8_t out[MAC_MAX_FRAME], const uint8_t key[16], bool pending,
                          uint8_t seq, const uint8_t src[8], uint32_t counter,
                          const uint8_t *packet, size_t packet_len);

/* An opened (verified) Data frame. packet points into the caller's scratch. */
struct mac_data {
    bool broadcast;
    bool ack_request;
    uint8_t seq;
    uint8_t src[8];
    uint8_t dst[8]; /* ff.. for broadcast */
    uint32_t counter;
    const uint8_t *packet;
    size_t packet_len;
};

/* Whether a Data frame of the given kind is for eui: every broadcast, and a
 * unicast to eui. Reads the clear header only, so it is cheap before trying
 * keys. */
bool mac_data_addressed_to(const uint8_t *frame, size_t len, enum mac_kind kind,
                           const uint8_t eui[8]);

/* Verifies a unicast or broadcast Data frame under key, decrypting into
 * scratch (MAC_MAX_FRAME bytes) so the frame survives a failed key. */
bool mac_open_data(const uint8_t *frame, size_t len, const uint8_t key[16],
                   uint8_t scratch[MAC_MAX_FRAME], struct mac_data *out);

/* A unicast to eui that asks for an Enhanced ACK: sets its seq and the
 * sender's EUI. Reads the clear header only, so the ACK can go out before the
 * MIC is checked, as a device expects it within about 1 ms. The last three
 * bytes of a tado EUI, carried encrypted, are always c5 1b 00. */
bool mac_ack_request(const uint8_t *frame, size_t len, const uint8_t eui[8], uint8_t *seq,
                     uint8_t src[8]);

/* The CSL phase an Enhanced ACK carries: 320 us units from the sender's
 * last channel sample back from the end of the ACK. */
uint16_t mac_enh_ack_phase(const uint8_t ack[MAC_ENH_ACK_LEN]);

/* Reads an Enhanced ACK; false on a bad length or FCS. */
bool mac_parse_enh_ack(const uint8_t *frame, size_t len, uint8_t *seq, uint8_t dst[8]);

#endif
