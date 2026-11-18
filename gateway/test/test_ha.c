/* Home Assistant topics and payloads: state JSON as reported, discovery
 * configs, and what may not go into a topic. */
#include "ha.h"
#include "harness.h"

static void a_discovery_config_byte_for_byte(void)
{
    char topic[96], payload[768];
    CHECK(ha_device_discovery(topic, sizeof topic, payload, sizeof payload, "SU2922080256", HA_TEMPERATURE));
    CHECK(strcmp(topic, "homeassistant/sensor/tado_SU2922080256/temperature/config") == 0);
    CHECK(strcmp(payload,
                 "{\"name\":\"Temperature\",\"unique_id\":\"tado_SU2922080256_temperature\","
                 "\"state_topic\":\"tado/device/SU2922080256/state\","
                 "\"value_template\":\"{{ value_json.temperature }}\",\"device_class\":\"temperature\","
                 "\"unit_of_measurement\":\"\xc2\xb0" "C\",\"state_class\":\"measurement\","
                 "\"availability_topic\":\"tado/gateway/status\",\"device\":{\"identifiers\":[\"tado_SU2922080256\"],"
                 "\"name\":\"tado SU2922080256\",\"manufacturer\":\"tado\",\"model\":\"Wireless Temperature Sensor\","
                 "\"serial_number\":\"SU2922080256\"}}") == 0);

    CHECK(ha_zone_discovery(topic, sizeof topic, payload, sizeof payload, 11, "Living", HA_ZONE_DEMAND));
    CHECK(strcmp(topic, "homeassistant/sensor/tado_zone_11/demand/config") == 0);
    CHECK(strstr(payload, "\"state_topic\":\"tado/zone/11/state\"") && strstr(payload, "\"icon\":\"mdi:radiator\""));

    /* A device entity is not a zone's, and the other way round. */
    CHECK(!ha_zone_discovery(topic, sizeof topic, payload, sizeof payload, 11, "Living", HA_TEMPERATURE));
    CHECK(!ha_device_discovery(topic, sizeof topic, payload, sizeof payload, "VA1", HA_ZONE_DEMAND));
    /* The longest serial allowed still makes a whole config. */
    CHECK(ha_device_discovery(topic, sizeof topic, payload, sizeof payload, "SU1234567890123", HA_BATTERY));
    CHECK(strstr(payload, "\"serial_number\":\"SU1234567890123\"}}") != NULL);

    /* Too small: nothing, rather than a cut config. */
    CHECK(!ha_device_discovery(topic, sizeof topic, payload, 100, "SU2922080256", HA_TEMPERATURE));
}

/* A serial comes from the device's request path and goes into topics. */
static void only_letters_and_digits_go_into_a_topic(void)
{
    CHECK(ha_serial_ok("VA1234567890", 12));
    CHECK(!ha_serial_ok("VA12/#", 6));
    CHECK(!ha_serial_ok("VA1+", 4));
    CHECK(!ha_serial_ok("VA1", 3));
    CHECK(!ha_serial_ok("VA12345678901234", 16));
    char topic[96], payload[768];
    CHECK(!ha_device_discovery(topic, sizeof topic, payload, sizeof payload, "VA\"12", HA_TEMPERATURE));
}

static void temperatures_below_zero_and_whole_ones(void)
{
    struct ha_device d = {.r = {.fields = TADO_TEMPERATURE, .temperature = -5}};
    char out[128];
    CHECK(ha_device_state(out, sizeof out, &d) && strcmp(out, "{\"temperature\":-0.05}") == 0);
    d.r.temperature = -1250;
    CHECK(ha_device_state(out, sizeof out, &d) && strcmp(out, "{\"temperature\":-12.50}") == 0);
    d.r.temperature = 2000;
    CHECK(ha_device_state(out, sizeof out, &d) && strcmp(out, "{\"temperature\":20.00}") == 0);
    d.r.fields = 0;
    CHECK(ha_device_state(out, sizeof out, &d) && strcmp(out, "{}") == 0);
    d.r.fields = TADO_TEMPERATURE;
    CHECK(ha_device_state(out, 10, &d) == 0);
}

/* The zone heats to a running overlay's setpoint; the z/s echo keeps 6280
 * after an overlay ended (docs/SENSOR.md), which then does not count. */
static void a_zone_shows_the_setpoint_it_heats_to(void)
{
    struct tado_report z = {0}, r = {0};
    r.fields = TADO_SETPOINT | TADO_OVERLAY | TADO_OVERLAY_SETPOINT;
    r.setpoint = 1800;
    r.overlay = 0;
    r.overlay_setpoint = 2100;
    ha_merge(&z, &r);
    char out[128];
    CHECK(ha_zone_state(out, sizeof out, &z) && strcmp(out, "{\"setpoint\":18.00}") == 0);

    struct tado_report overlay = {.fields = TADO_OVERLAY | TADO_OVERLAY_SETPOINT, .overlay = 2,
                                  .overlay_setpoint = 2100};
    ha_merge(&z, &overlay);
    CHECK(ha_zone_state(out, sizeof out, &z) && strcmp(out, "{\"setpoint\":21.00}") == 0);

    /* The zone's own measure wins over the owner's temperature. */
    struct tado_report m = {.fields = TADO_TEMPERATURE | TADO_ZONE_TEMPERATURE, .temperature = 1900,
                            .zone_temperature = 1875};
    ha_merge(&z, &m);
    CHECK(ha_zone_state(out, sizeof out, &z) && strcmp(out, "{\"temperature\":18.75,\"setpoint\":21.00}") == 0);
    CHECK(ha_zone_entities(&z) == (1u << HA_ZONE_TEMPERATURE | 1u << HA_ZONE_SETPOINT | 1u << HA_ZONE_CLIMATE));
}

/* The room's thermostat: what it shows comes from the room's state, what is
 * set goes to tado/zone/<n>/setpoint/set. */
static void a_room_is_a_climate_entity(void)
{
    char topic[96], payload[1024];
    CHECK(ha_zone_discovery(topic, sizeof topic, payload, sizeof payload, 11, "Living", HA_ZONE_CLIMATE));
    CHECK(strcmp(topic, "homeassistant/climate/tado_zone_11/config") == 0);
    CHECK(strcmp(payload,
                 "{\"name\":null,\"unique_id\":\"tado_zone_11_climate\",\"modes\":[\"heat\"],"
                 "\"temperature_command_topic\":\"tado/zone/11/setpoint/set\","
                 "\"temperature_state_topic\":\"tado/zone/11/state\","
                 "\"temperature_state_template\":\"{{ value_json.setpoint | default('None') }}\","
                 "\"current_temperature_topic\":\"tado/zone/11/state\","
                 "\"current_temperature_template\":\"{{ value_json.temperature | default('None') }}\","
                 "\"action_topic\":\"tado/zone/11/state\","
                 "\"action_template\":\"{{ 'heating' if value_json.demand | default(0) > 0 else 'idle' }}\","
                 "\"min_temp\":5,\"max_temp\":25,\"temp_step\":0.1,\"precision\":0.1,\"temperature_unit\":\"C\","
                 "\"availability_topic\":\"tado/gateway/status\","
                 "\"device\":{\"identifiers\":[\"tado_zone_11\"],\"name\":\"Living\",\"manufacturer\":\"tado\"}}") == 0);
    struct tado_report none = {0};
    CHECK(ha_zone_entities(&none) == 1u << HA_ZONE_CLIMATE); /* from the start */
}

/* What Home Assistant sends (temp_step 0.1) and people type on the page. */
static void reads_a_setpoint(void)
{
    CHECK(ha_parse_centi("21", 2) == 2100);
    CHECK(ha_parse_centi("21.5", 4) == 2150);
    CHECK(ha_parse_centi("21.25", 5) == 2125);
    CHECK(ha_parse_centi(" 7.0\n", 5) == 700);
    CHECK(ha_parse_centi("21.255", 6) == -1);
    CHECK(ha_parse_centi("-5", 2) == -1);
    CHECK(ha_parse_centi("21,5", 4) == -1);
    CHECK(ha_parse_centi(".5", 2) == -1);
    CHECK(ha_parse_centi("1000", 4) == -1);
    CHECK(ha_parse_centi("", 0) == -1);
}

void run_ha_tests(void)
{
    a_room_is_a_climate_entity();
    reads_a_setpoint();
    a_discovery_config_byte_for_byte();
    only_letters_and_digits_go_into_a_topic();
    temperatures_below_zero_and_whole_ones();
    a_zone_shows_the_setpoint_it_heats_to();
}
