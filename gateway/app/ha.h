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
/* Home or away, for the whole home: retained state, and the command. */
#define HA_HOME_STATE_TOPIC "tado/home/state"
#define HA_HOME_SET_TOPIC   "tado/home/set"
#define HA_HOME             "home"
#define HA_AWAY             "away"

enum ha_entity {
    HA_TEMPERATURE,
    HA_HUMIDITY,
    HA_BATTERY,
    HA_SIGNAL,
    HA_LAST_SEEN,
    HA_ZONE_TEMPERATURE,
    HA_ZONE_SETPOINT,
    HA_ZONE_DEMAND,
    HA_ZONE_CLIMATE, /* the room's thermostat: set its setpoint from Home Assistant */
};
#define HA_ZONE_FIRST HA_ZONE_TEMPERATURE
#define HA_ZONE_LAST  HA_ZONE_CLIMATE

/* What Home Assistant sends a room: tado/zone/<n>/setpoint/set (degrees C,
 * e.g. "21.5") and tado/zone/<n>/preset/set ("boost" or "none"). The
 * subscription for all, and what a topic is, with its zone. */
#define HA_ZONE_COMMANDS "tado/zone/+/+/set"
enum ha_zone_command { HA_ZONE_NO_COMMAND, HA_ZONE_SETPOINT_SET, HA_ZONE_PRESET_SET };
enum ha_zone_command ha_zone_command(const char *topic, unsigned *zone);
/* What tado's own app offers. */
#define HA_SETPOINT_MIN 500  /* centi-degrees C */
#define HA_SETPOINT_MAX 2500

static inline bool ha_setpoint_ok(int32_t centi) { return centi >= HA_SETPOINT_MIN && centi <= HA_SETPOINT_MAX; }

/* What the gateway knows of a device: every report merged, newest wins. */
struct ha_device {
    char serial[16];         /* from d/<serial>/...; "" until one came */
    struct tado_report r;
    bool has_rssi;
    int8_t rssi;
    uint32_t last_seen;      /* Unix seconds, 0 when the clock was not set */
};

/* A setpoint as Home Assistant and the page send it: degrees C ("21",
 * "21.5", "20.200000000000003"), spaces around allowed; centi-C, rounded, or
 * -1 for anything else. */
int32_t ha_parse_centi(const char *s, size_t len);

/* A serial as it may go into a topic: 4-15 letters and digits. */
bool ha_serial_ok(const char *serial, size_t len);

/* Copies the fields present in from into into. */
void ha_merge(struct tado_report *into, const struct tado_report *from);

/* The entities that have a value (a room's climate entity always): bits of
 * 1u << enum ha_entity. */
uint32_t ha_device_entities(const struct ha_device *d);
uint32_t ha_zone_entities(const struct tado_report *zone);

size_t ha_device_state_topic(char *out, size_t cap, const char *serial);
size_t ha_device_state(char *out, size_t cap, const struct ha_device *d);
size_t ha_zone_state_topic(char *out, size_t cap, unsigned zone);
/* What the gateway has a room heat to now (its setpoint, away temperature or
 * boost: its own word, which Home Assistant's climate entity shows), and
 * whether the device confirmed it: no write pending and its report says so.
 * Zeroed when there is none to show. */
struct ha_target {
    bool known;
    bool on;
    uint16_t setpoint; /* centi-degrees C, when on */
    bool confirmed;
    bool stale; /* unconfirmed for longer than the gateway's stale time */
    bool boost; /* a boost the gateway started runs */
};

/* The setpoint the device reported it heats to (an overlay's, else its own);
 * false when it reported none. */
bool ha_reported_setpoint(const struct tado_report *zone, uint16_t *centi);

size_t ha_zone_state(char *out, size_t cap, const struct tado_report *zone, const struct ha_target *target);

/* The discovery topic and config of one entity of a device (by serial) or a
 * zone, which Home Assistant shows under the room's name (no quotes or
 * backslashes: install_name_ok); false when it does not fit or e belongs to
 * the other kind. */
bool ha_device_discovery(char *topic, size_t tcap, char *payload, size_t pcap, const char *serial,
                         enum ha_entity e);
bool ha_zone_discovery(char *topic, size_t tcap, char *payload, size_t pcap, unsigned zone, const char *name,
                       enum ha_entity e);

/* The Away switch of the whole home, on the gateway's own device. */
bool ha_away_discovery(char *topic, size_t tcap, char *payload, size_t pcap);

#endif
