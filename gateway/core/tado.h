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
#define TADO_OVERLAY_TIMER  3
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

/* A heating zone's state as a device reads it (GET h/<home>/z/<n>/s) and as
 * the coordinator writes it (PUT z/s?id=<n>), in the captured tag order
 * (freedo PROTOCOL.md "Setpoint write path"):
 *   6160 6180 6020 61e0 [6200] 6240 [6260] [6280] 62e0 [6440]
 * Window closed (62e0=0). */
struct tado_zone_block {
    bool away;                 /* 6160: 1 home, 2 away (the whole home) */
    bool on;                   /* 61e0; 6200 only when on */
    uint16_t setpoint;         /* 6200: what the zone follows without an overlay, centi-C */
    uint8_t overlay;           /* 6240: 0 none, TADO_OVERLAY_MANUAL, TADO_OVERLAY_TIMER */
    bool overlay_on;           /* 6260, with an overlay */
    uint16_t overlay_setpoint; /* 6280, with an overlay that is on */
    bool marker;               /* 6440=0001: the write starts or ends an overlay, or changes the target */
};
size_t tado_zone_block(uint8_t *out, size_t cap, const struct tado_zone_block *b);


/* coap://[fe80:0:0:0:..]/z/ for the device; false when it does not fit cap. */
bool tado_zone_uri(char *out, size_t cap, const uint8_t device[8]);

/* What a device reports: d/<serial>/sen, z/p, h/<home>/z/<n>/act, the z/s
 * echo and h/<home>/z/<n>/overlay. Names as freedo's tag table (tado/tlv.go). */
enum tado_report_field {
    TADO_TEMPERATURE = 1u << 0,      /* 012d, calibrated, centi-C */
    TADO_TEMPERATURE_RAW = 1u << 1,  /* 012e, the sensor's own, centi-C */
    TADO_ZONE_TEMPERATURE = 1u << 2, /* 4060, the zone's measured temperature, centi-C */
    TADO_HUMIDITY = 1u << 3,         /* 0135, 0.1 % */
    TADO_BATTERY = 1u << 4,          /* 0162, mV */
    TADO_LIGHT = 1u << 5,            /* 0136, ambient light % */
    TADO_DEMAND = 1u << 6,           /* 40a0, heating demand % */
    TADO_SETPOINT = 1u << 7,         /* 6200, the zone's own (scheduled) setpoint, centi-C */
    TADO_OVERLAY = 1u << 8,          /* 6240: 0 none, 1 until auto change, 2 manual, 3 timer */
    TADO_OVERLAY_ON = 1u << 9,       /* 6260 */
    TADO_OVERLAY_SETPOINT = 1u << 10, /* 6280, centi-C */
    TADO_ON = 1u << 11,              /* 61e0: the zone's own program is on */
};

struct tado_report {
    uint32_t fields; /* enum tado_report_field bits present */
    int16_t temperature, temperature_raw, zone_temperature;
    uint16_t humidity, battery_mv;
    uint8_t light, demand;
    uint16_t setpoint, overlay_setpoint;
    uint8_t overlay;
    bool overlay_on, on;
};

/* Decodes the known tags of a report body; others are skipped. False for a
 * malformed body. */
bool tado_report_decode(const uint8_t *body, size_t len, struct tado_report *r);

/* The document's ETag: FNV-1a 64, big-endian. The device only compares it. */
void tado_etag(const uint8_t *doc, size_t len, uint8_t out[TADO_ETAG_LEN]);

/* The answer to a GET of doc: 2.03 with the ETag when the request holds it for
 * block 0, else 2.05 with the ETag and the block asked for, at most 64 bytes.
 * NUM counts 64-byte blocks whatever size the device asks in, as the sensor
 * numbers them. */
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
