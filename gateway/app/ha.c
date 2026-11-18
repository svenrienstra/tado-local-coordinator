#include "ha.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* --- writing into a bounded buffer ------------------------------------------ */

struct out {
    char *buf;
    size_t cap, len;
    bool full;
};

static void put(struct out *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void put(struct out *o, const char *fmt, ...)
{
    if (o->full)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->buf + o->len, o->cap - o->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= o->cap - o->len)
        o->full = true;
    else
        o->len += (size_t)n;
}

static size_t done(const struct out *o) { return o->full ? 0 : o->len; }

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
    return e;
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
};

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

bool ha_zone_discovery(char *topic, size_t tcap, char *payload, size_t pcap, unsigned zone, enum ha_entity e)
{
    if (e < HA_ZONE_TEMPERATURE || e > HA_ZONE_DEMAND)
        return false;
    char uid[24], state[32], device[96];
    snprintf(uid, sizeof uid, "tado_zone_%u", zone);
    ha_zone_state_topic(state, sizeof state, zone);
    snprintf(device, sizeof device, "{\"identifiers\":[\"%s\"],\"name\":\"tado zone %u\",\"manufacturer\":\"tado\"}",
             uid, zone);
    struct out t = {topic, tcap, 0, false}, p = {payload, pcap, 0, false};
    put(&t, "homeassistant/sensor/%s/%s/config", uid, ENTITIES[e].key);
    put_sensor(&p, &ENTITIES[e], uid, state, device);
    return done(&t) && done(&p);
}
