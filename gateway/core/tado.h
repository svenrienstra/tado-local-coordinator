#ifndef TADO_H
#define TADO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "coap.h"
#include "lowpan.h"

/* The tado resources the coordinator serves and the documents a device reads,
 * byte for byte as the Java host builds them (golden/host_vectors.txt) and,
 * where freedo builds them too, as freedo does (golden/vectors.txt).
 *
 * Builders write a whole 6LoWPAN datagram (or a document) to out and return
 * its length, or 0 when it does not fit cap. EUIs are in wire order. TLVs are
 * a 2-byte tag, a 1-byte length and a big-endian value. */

/* Role byte of a 015e record: an elected owner (bit 3) of its own zone. */
#define TADO_ROLE_OWNER  0x0d
#define TADO_ROLE_MEMBER 0x05
/* 6240: 0 no overlay, 1 until the next automatic change, 2 manual, 3 timer. */
#define TADO_OVERLAY_MANUAL 2
/* 64-byte blocks: a 128-byte one does not fit one frame (97 bytes of packet). */
#define TADO_MAX_SZX 2
/* The source port of the bridge's own requests to a device. */
#define TADO_BRIDGE_PORT 4005
#define TADO_ETAG_LEN 8

struct tado_zone_role {
    uint8_t role;
    uint8_t zone;
};

/* A device's configuration: trace off, home_id (1..2^32-1), no offline
 * schedule, its zone roles, notification on remote overlay changes. */
size_t tado_device_config(uint8_t *out, size_t cap, uint32_t home_id, const struct tado_zone_role *roles,
                          size_t n_roles);

/* A heating zone with owner as owner and measuring endpoint, no circuit,
 * freedo's PID defaults and open-window detection on. */
size_t tado_zone_with_owner(uint8_t *out, size_t cap, const uint8_t owner[8]);

/* A zone's state as a device reads it (GET h/<home>/z/<n>/s) and as the
 * coordinator writes it (PUT z/s?id=<n>): at home, heating, on, setpoint in
 * centi-degrees C, window closed. With overlay >= 0 a manual overlay at that
 * setpoint; without (-1), 6240=0, which ends one the device holds. */
size_t tado_zone_state(uint8_t *out, size_t cap, uint16_t setpoint, int32_t overlay);

/* coap://[fe80:0:0:0:..]/z/ for the device; false when it does not fit cap. */
bool tado_zone_uri(char *out, size_t cap, const uint8_t device[8]);

/* The last TLV with that tag in body; false when absent or the body is
 * malformed. */
bool tado_tlv_find(const uint8_t *body, size_t len, uint16_t tag, const uint8_t **value, size_t *value_len);

/* A TLV value as an unsigned big-endian integer. */
uint32_t tado_tlv_uint(const uint8_t *value, size_t len);

/* The document's ETag: FNV-1a 64, big-endian. The device only compares it. */
void tado_etag(const uint8_t *doc, size_t len, uint8_t out[TADO_ETAG_LEN]);

/* The answer to a GET of doc: 2.03 with the ETag when the request holds it for
 * block 0, else 2.05 with the ETag and the block asked for, at most 64 bytes
 * (a block asked in a larger size is scaled, RFC 7959 2.4). */
size_t tado_block_answer(uint8_t *out, size_t cap, const struct coap_msg *request, const uint8_t *doc,
                         size_t doc_len, const struct lowpan_datagram *from, const uint8_t coordinator[8],
                         const uint8_t device[8]);

/* The d/pair CON POST that hands a joiner the network key, sent under the
 * pairing key; session is the single-use value of option 2048. */
size_t tado_pair_request(uint8_t *out, size_t cap, const uint8_t key[16], uint16_t mid, const uint8_t session[8],
                         const uint8_t coordinator[8], const uint8_t joiner[8]);

/* The 2.05 to auth/token: session id (4..8 bytes) and a 1440 min lifetime. */
size_t tado_auth_token_answer(uint8_t *out, size_t cap, const struct coap_msg *request, const uint8_t *session_id,
                              size_t session_len, const struct lowpan_datagram *from,
                              const uint8_t coordinator[8], const uint8_t device[8]);

/* The 2.05 to time: protobuf field 1 fixed32 = Unix seconds. */
size_t tado_time_answer(uint8_t *out, size_t cap, const struct coap_msg *request, uint32_t unix_seconds,
                        const struct lowpan_datagram *from, const uint8_t coordinator[8], const uint8_t device[8]);

/* Router Advertisement to ff02::1 as the bridge sends it. */
size_t tado_router_advert(uint8_t *out, size_t cap, const uint8_t coordinator[8], const uint8_t gateway_v4[4],
                          const uint8_t dns_v4[4]);

/* The empty ACK (0.00) that confirms a CON response, echoing its MID. */
size_t tado_empty_ack(uint8_t *out, size_t cap, const struct coap_msg *con, const struct lowpan_datagram *from,
                      const uint8_t coordinator[8], const uint8_t device[8]);

/* The bridge's default answer to a PUT or POST: a piggybacked 2.04. */
size_t tado_changed_answer(uint8_t *out, size_t cap, const struct coap_msg *request,
                           const struct lowpan_datagram *from, const uint8_t coordinator[8], const uint8_t device[8]);

/* CON PUT z/s?id=<zone> from port 4005, Uri-Port ffff, Content-Format 42. */
size_t tado_zone_state_write(uint8_t *out, size_t cap, uint8_t zone, const uint8_t *state, size_t state_len,
                             uint16_t mid, const uint8_t coordinator[8], const uint8_t device[8]);

#endif
