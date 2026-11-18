#ifndef HA_H
#define HA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tado.h"

/* MQTT topics and payloads for Home Assistant (docs/PLAN.md 3.7): retained
 * JSON state per device and per zone, and a discovery config per sensor.
 * M2 publishes sensors only; the zone's climate entity comes with its
 * setpoint command in M3.
 *
 * Writers return the length written (without the NUL), or 0 when it did not
 * fit cap. Temperatures go out in degrees C, two decimals, as reported. */

#define HA_STATUS_TOPIC "tado/gateway/status"
#define HA_LOG_TOPIC    "tado/gateway/log"
#define HA_PAIR_TOPIC   "tado/gateway/pair/set"
#define HA_ONLINE       "online"
#define HA_OFFLINE      "offline"

enum ha_entity {
    HA_TEMPERATURE,
    HA_HUMIDITY,
    HA_BATTERY,
    HA_SIGNAL,
    HA_LAST_SEEN,
    HA_ZONE_TEMPERATURE,
    HA_ZONE_SETPOINT,
    HA_ZONE_DEMAND,
};

/* What the gateway knows of a device: every report merged, newest wins. */
struct ha_device {
    char serial[16];         /* from d/<serial>/...; "" until one came */
    struct tado_report r;
    bool has_rssi;
    int8_t rssi;
    uint32_t last_seen;      /* Unix seconds, 0 when the clock was not set */
};

/* A serial as it may go into a topic: 4-15 letters and digits. */
bool ha_serial_ok(const char *serial, size_t len);

/* Copies the fields present in from into into. */
void ha_merge(struct tado_report *into, const struct tado_report *from);

/* The entities that have a value: bits of 1u << enum ha_entity. */
uint32_t ha_device_entities(const struct ha_device *d);
uint32_t ha_zone_entities(const struct tado_report *zone);

size_t ha_device_state_topic(char *out, size_t cap, const char *serial);
size_t ha_device_state(char *out, size_t cap, const struct ha_device *d);
size_t ha_zone_state_topic(char *out, size_t cap, unsigned zone);
size_t ha_zone_state(char *out, size_t cap, const struct tado_report *zone);

/* The discovery topic and config of one entity of a device (by serial) or a
 * zone; false when it does not fit or e belongs to the other kind. */
bool ha_device_discovery(char *topic, size_t tcap, char *payload, size_t pcap, const char *serial,
                         enum ha_entity e);
bool ha_zone_discovery(char *topic, size_t tcap, char *payload, size_t pcap, unsigned zone, enum ha_entity e);

#endif
