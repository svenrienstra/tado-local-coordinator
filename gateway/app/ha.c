#include "ha.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "out.h"

/* A fixed-point value with `decimals` digits after the point: 1866, 2 -> 18.66. */
static void put_fixed(struct out *o, const char *key, int32_t value, unsigned decimals)
{
    static const uint32_t scale[] = {1, 10, 100, 1000};
    uint32_t mag = value < 0 ? (uint32_t)(-(int64_t)value) : (uint32_t)value;
    if (decimals == 0)
        put(o, "%s\"%s\":%s%lu", o->len > 1 ? "," : "", key, value < 0 ? "-" : "", (unsigned long)mag);
    else
        put(o, "%s\"%s\":%s%lu.%0*lu", o->len > 1 ? "," : "", key, value < 0 ? "-" : "",
            (unsigned long)(mag / scale[decimals]), (int)decimals, (unsigned long)(mag % scale[decimals]));
}

/* --- state ------------------------------------------------------------------ */

bool ha_serial_ok(const char *serial, size_t len)
{
    if (len < 4 || len > 15)
        return false;
    for (size_t i = 0; i < len; i++) {
        char c = serial[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')))
            return false;
    }
    return true;
}

void ha_merge(struct tado_report *into, const struct tado_report *from)
{
    uint32_t f = from->fields;
    if (f & TADO_TEMPERATURE)
        into->temperature = from->temperature;
    if (f & TADO_TEMPERATURE_RAW)
        into->temperature_raw = from->temperature_raw;
    if (f & TADO_ZONE_TEMPERATURE)
        into->zone_temperature = from->zone_temperature;
    if (f & TADO_HUMIDITY)
        into->humidity = from->humidity;
    if (f & TADO_BATTERY)
        into->battery_mv = from->battery_mv;
    if (f & TADO_LIGHT)
        into->light = from->light;
    if (f & TADO_DEMAND)
        into->demand = from->demand;
    if (f & TADO_SETPOINT)
        into->setpoint = from->setpoint;
    if (f & TADO_OVERLAY)
        into->overlay = from->overlay;
    if (f & TADO_OVERLAY_ON)
        into->overlay_on = from->overlay_on;
    if (f & TADO_OVERLAY_SETPOINT)
        into->overlay_setpoint = from->overlay_setpoint;
    into->fields |= f;
}

uint32_t ha_device_entities(const struct ha_device *d)
{
    uint32_t e = 0;
    if (d->r.fields & TADO_TEMPERATURE)
        e |= 1u << HA_TEMPERATURE;
    if (d->r.fields & TADO_HUMIDITY)
        e |= 1u << HA_HUMIDITY;
    if (d->r.fields & TADO_BATTERY)
        e |= 1u << HA_BATTERY;
    if (d->has_rssi)
        e |= 1u << HA_SIGNAL;
    if (d->last_seen)
        e |= 1u << HA_LAST_SEEN;
    return e;
}

/* The zone's measure: its own (4060) when the owner sends one, else the
 * owner's temperature. */
static bool zone_temperature(const struct tado_report *z, int16_t *t)
{
    if (z->fields & TADO_ZONE_TEMPERATURE)
        *t = z->zone_temperature;
    else if (z->fields & TADO_TEMPERATURE)
        *t = z->temperature;
    else
        return false;
    return true;
}

/* What the zone heats to: a running overlay's setpoint, else its own. */
static bool zone_setpoint(const struct tado_report *z, uint16_t *c)
{
    if ((z->fields & TADO_OVERLAY) && z->overlay != 0 && (z->fields & TADO_OVERLAY_SETPOINT))
        *c = z->overlay_setpoint;
    else if (z->fields & TADO_SETPOINT)
        *c = z->setpoint;
    else
        return false;
    return true;
}

uint32_t ha_zone_entities(const struct tado_report *z)
{
    int16_t t;
    uint16_t c;
    uint32_t e = 0;
    if (zone_temperature(z, &t))
        e |= 1u << HA_ZONE_TEMPERATURE;
    if (zone_setpoint(z, &c))
        e |= 1u << HA_ZONE_SETPOINT;
    if (z->fields & TADO_DEMAND)
        e |= 1u << HA_ZONE_DEMAND;
    return e | 1u << HA_ZONE_CLIMATE;
}

/* Degrees C with at most two decimals ("21", "21.5", "21.25") in centi-C;
 * -1 for anything else. */
bool ha_setpoint_topic(const char *topic, unsigned *zone)
{
    int end = 0;
    return sscanf(topic, "tado/zone/%3u/setpoint/set%n", zone, &end) == 1 && end > 0 && topic[end] == '\0' &&
           *zone >= 1 && *zone <= 255;
}

int32_t ha_parse_centi(const char *s, size_t len)
{
    while (len && (*s == ' ' || *s == '\n' || *s == '\r'))
        s++, len--;
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\n' || s[len - 1] == '\r'))
        len--;
    int32_t whole = 0, frac = 0;
    size_t i = 0, digits = 0, decimals = 0;
    bool round_up = false;
    for (; i < len && s[i] >= '0' && s[i] <= '9' && digits < 3; i++, digits++)
        whole = whole * 10 + (s[i] - '0');
    if (i < len && s[i] == '.')
        for (i++; i < len && s[i] >= '0' && s[i] <= '9'; i++, decimals++) {
            if (decimals < 2)
                frac = frac * 10 + (s[i] - '0');
            else if (decimals == 2)
                round_up = s[i] >= '5'; /* a float as Home Assistant prints it: 20.200000000000003 */
        }
    if (digits == 0 || i != len)
        return -1;
    return whole * 100 + (decimals == 1 ? frac * 10 : frac) + round_up;
}

size_t ha_device_state_topic(char *out, size_t cap, const char *serial)
{
    struct out o = {out, cap, 0, false};
    put(&o, "tado/device/%s/state", serial);
    return done(&o);
}

size_t ha_device_state(char *out, size_t cap, const struct ha_device *d)
{
    struct out o = {out, cap, 0, false};
    put(&o, "{");
    if (d->r.fields & TADO_TEMPERATURE)
        put_fixed(&o, "temperature", d->r.temperature, 2);
    if (d->r.fields & TADO_HUMIDITY)
        put_fixed(&o, "humidity", d->r.humidity, 1);
    if (d->r.fields & TADO_BATTERY)
        put_fixed(&o, "battery", d->r.battery_mv, 3);
    if (d->has_rssi)
        put_fixed(&o, "rssi", d->rssi, 0);
    if (d->last_seen) {
        time_t t = (time_t)d->last_seen;
        struct tm tm;
        char iso[24];
        gmtime_r(&t, &tm);
        strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%SZ", &tm);
        put(&o, "%s\"last_seen\":\"%s\"", o.len > 1 ? "," : "", iso);
    }
    put(&o, "}");
    return done(&o);
}

size_t ha_zone_state_topic(char *out, size_t cap, unsigned zone)
{
    struct out o = {out, cap, 0, false};
    put(&o, "tado/zone/%u/state", zone);
    return done(&o);
}

size_t ha_zone_state(char *out, size_t cap, const struct tado_report *z)
{
    struct out o = {out, cap, 0, false};
    int16_t t;
    uint16_t c;
    put(&o, "{");
    if (zone_temperature(z, &t))
        put_fixed(&o, "temperature", t, 2);
    if (zone_setpoint(z, &c))
        put_fixed(&o, "setpoint", c, 2);
    if (z->fields & TADO_DEMAND)
        put_fixed(&o, "demand", z->demand, 0);
    put(&o, "}");
    return done(&o);
}

/* --- discovery -------------------------------------------------------------- */

struct entity {
    const char *key;   /* object id and JSON key */
    const char *name;
    const char *device_class;
    const char *unit;
    bool measurement;
    bool diagnostic;
    const char *icon;
};

#define DEGREES_C "\xc2\xb0" "C"

static const struct entity ENTITIES[] = {
    [HA_TEMPERATURE] = {"temperature", "Temperature", "temperature", DEGREES_C, true, false, NULL},
    [HA_HUMIDITY] = {"humidity", "Humidity", "humidity", "%", true, false, NULL},
    [HA_BATTERY] = {"battery", "Battery", "voltage", "V", true, true, NULL},
    [HA_SIGNAL] = {"rssi", "Signal", "signal_strength", "dBm", true, true, NULL},
    [HA_LAST_SEEN] = {"last_seen", "Last seen", "timestamp", NULL, false, true, NULL},
    [HA_ZONE_TEMPERATURE] = {"temperature", "Temperature", "temperature", DEGREES_C, true, false, NULL},
    [HA_ZONE_SETPOINT] = {"setpoint", "Setpoint", "temperature", DEGREES_C, true, false, NULL},
    [HA_ZONE_DEMAND] = {"demand", "Heating demand", NULL, "%", true, false, "mdi:radiator"},
    [HA_ZONE_CLIMATE] = {"climate", NULL, NULL, NULL, false, false, NULL},
};

/* A thermostat for the room. Target and current temperature and the action
 * come from the room's state, which holds what the device reported, so Home
 * Assistant shows the setpoint the device took, not the one asked for. A key
 * the device has not reported yet reads as None (unknown). */
static void put_climate(struct out *o, unsigned zone, const char *uid, const char *state_topic, const char *device)
{
    char set[40];
    snprintf(set, sizeof set, HA_SETPOINT_TOPIC_FMT, zone);
    put(o, "{\"name\":null,\"unique_id\":\"%s_climate\",\"modes\":[\"heat\"],", uid);
    /* Its only mode, or Home Assistant shows it as off. */
    put(o, "\"mode_state_topic\":\"%s\",\"mode_state_template\":\"heat\",", state_topic);
    put(o, "\"temperature_command_topic\":\"%s\",", set);
    put(o, "\"temperature_state_topic\":\"%s\",\"temperature_state_template\":\"{{ value_json.setpoint | default('None') }}\",",
        state_topic);
    put(o, "\"current_temperature_topic\":\"%s\",\"current_temperature_template\":"
           "\"{{ value_json.temperature | default('None') }}\",",
        state_topic);
    put(o, "\"action_topic\":\"%s\",\"action_template\":"
           "\"{{ 'heating' if value_json.demand | default(0) > 0 else 'idle' }}\",",
        state_topic);
    put(o, "\"min_temp\":%u,\"max_temp\":%u,\"temp_step\":0.1,\"precision\":0.1,\"temperature_unit\":\"C\",",
        HA_SETPOINT_MIN / 100, HA_SETPOINT_MAX / 100);
    put(o, "\"availability_topic\":\"" HA_STATUS_TOPIC "\",\"device\":%s}", device);
}

/* The parts every sensor config shares; device is the "device" object. */
static void put_sensor(struct out *o, const struct entity *e, const char *uid, const char *state_topic,
                       const char *device)
{
    put(o, "{\"name\":\"%s\",\"unique_id\":\"%s_%s\",\"state_topic\":\"%s\",", e->name, uid, e->key, state_topic);
    put(o, "\"value_template\":\"{{ value_json.%s }}\"", e->key);
    if (e->device_class)
        put(o, ",\"device_class\":\"%s\"", e->device_class);
    if (e->unit)
        put(o, ",\"unit_of_measurement\":\"%s\"", e->unit);
    if (e->measurement)
        put(o, ",\"state_class\":\"measurement\"");
    if (e->diagnostic)
        put(o, ",\"entity_category\":\"diagnostic\"");
    if (e->icon)
        put(o, ",\"icon\":\"%s\"", e->icon);
    put(o, ",\"availability_topic\":\"" HA_STATUS_TOPIC "\",\"device\":%s}", device);
}

/* What the serial's two letters say the device is (freedo's naming). */
static const char *model_of(const char *serial)
{
    if (strncmp(serial, "VA", 2) == 0)
        return "Smart Radiator Thermostat";
    if (strncmp(serial, "RU", 2) == 0)
        return "Smart Thermostat";
    if (strncmp(serial, "SU", 2) == 0)
        return "Wireless Temperature Sensor";
    return "unknown";
}

bool ha_device_discovery(char *topic, size_t tcap, char *payload, size_t pcap, const char *serial,
                         enum ha_entity e)
{
    if (e > HA_LAST_SEEN || !ha_serial_ok(serial, strlen(serial)))
        return false;
    char uid[32], state[48], device[256];
    snprintf(uid, sizeof uid, "tado_%s", serial);
    ha_device_state_topic(state, sizeof state, serial);
    int n = snprintf(device, sizeof device,
             "{\"identifiers\":[\"%s\"],\"name\":\"tado %s\",\"manufacturer\":\"tado\",\"model\":\"%s\","
             "\"serial_number\":\"%s\"}",
             uid, serial, model_of(serial), serial);
    if (n < 0 || (size_t)n >= sizeof device)
        return false;
    struct out t = {topic, tcap, 0, false}, p = {payload, pcap, 0, false};
    put(&t, "homeassistant/sensor/%s/%s/config", uid, ENTITIES[e].key);
    put_sensor(&p, &ENTITIES[e], uid, state, device);
    return done(&t) && done(&p);
}

bool ha_zone_discovery(char *topic, size_t tcap, char *payload, size_t pcap, unsigned zone, const char *name,
                       enum ha_entity e)
{
    if (e < HA_ZONE_TEMPERATURE || e > HA_ZONE_CLIMATE)
        return false;
    char uid[24], state[32], device[160];
    snprintf(uid, sizeof uid, "tado_zone_%u", zone);
    ha_zone_state_topic(state, sizeof state, zone);
    int n = snprintf(device, sizeof device, "{\"identifiers\":[\"%s\"],\"name\":\"%s\",\"manufacturer\":\"tado\"}",
                     uid, name);
    if (n < 0 || (size_t)n >= sizeof device)
        return false;
    struct out t = {topic, tcap, 0, false}, p = {payload, pcap, 0, false};
    if (e == HA_ZONE_CLIMATE) {
        put(&t, "homeassistant/climate/%s/config", uid);
        put_climate(&p, zone, uid, state, device);
    } else {
        put(&t, "homeassistant/sensor/%s/%s/config", uid, ENTITIES[e].key);
        put_sensor(&p, &ENTITIES[e], uid, state, device);
    }
    return done(&t) && done(&p);
}
