#ifndef INSTALL_H
#define INSTALL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "session.h"

/* The installation as it is kept in flash: the rooms and the devices joined
 * to us, each in a room or in none (docs/PLAN.md 3.8, 3.9).
 *
 * Encoded as version 3: the version byte, then records of tag, length and
 * value (little-endian), ending with an empty end record:
 *   settings: flags (bit 0 away), next_zone, stale_minutes u16
 *   room:     zone, setpoint u16, away_setpoint u16, name_len, name
 *   device:   eui[8], zone, serial_len, serial
 * A later version may append fields to a record or add records: a decoder
 * skips what it does not know, so rooms and devices survive an update.
 * A room's zone is 1-255, a device's 0 when it is in no room. */

#define INSTALL_MAX_ROOMS   SESSION_MAX_ZONES
#define INSTALL_MAX_DEVICES 16
#define INSTALL_NAME_MAX    23 /* bytes of UTF-8 */
#define INSTALL_SERIAL_MAX  15
#define INSTALL_VERSION     3 /* 1 and 2 never ran on a gateway: nothing to read them from */
#define INSTALL_TAG_END      0
#define INSTALL_TAG_SETTINGS 1
#define INSTALL_TAG_ROOM     2
#define INSTALL_TAG_DEVICE   3
#define INSTALL_FLAG_AWAY    0x01
/* After how long unconfirmed a room shows as unavailable (gateway.h). */
#define INSTALL_STALE_MINUTES_DEFAULT 30
#define INSTALL_STALE_MINUTES_MAX     1440
#define INSTALL_MAX_BLOB \
    (1 + 6 + INSTALL_MAX_ROOMS * (8 + INSTALL_NAME_MAX) + INSTALL_MAX_DEVICES * (12 + INSTALL_SERIAL_MAX) + 2)

struct install_room {
    uint8_t zone;
    uint16_t setpoint;      /* centi-degrees C, at home */
    uint16_t away_setpoint; /* in away; 0: off */
    char name[INSTALL_NAME_MAX + 1];
};

struct install_device {
    uint8_t eui[8];
    uint8_t zone;
    char serial[INSTALL_SERIAL_MAX + 1];
};

struct install {
    bool away;          /* the whole home */
    uint8_t next_zone; /* where a new room's number is looked for (gateway.h) */
    uint16_t stale_minutes; /* 0: never */
    size_t n_rooms;
    struct install_room rooms[INSTALL_MAX_ROOMS];
    size_t n_devices;
    struct install_device devices[INSTALL_MAX_DEVICES];
};

/* A room name: 1-23 bytes, no control characters, quotes or backslashes, so
 * it goes into JSON and MQTT payloads as it is. */
bool install_name_ok(const char *name, size_t len);

/* Returns the length, 0 when it does not fit cap. */
size_t install_encode(const struct install *in, uint8_t *out, size_t cap);

/* No rooms and no devices, and the default settings: before the first save. */
void install_empty(struct install *in);

/* False, with *in emptied (install_empty), for anything but a whole blob of
 * this version. */
bool install_decode(const uint8_t *blob, size_t len, struct install *in);

#endif
