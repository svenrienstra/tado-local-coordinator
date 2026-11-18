/* The ESP32-S3 gateway: ESP-IDF around ../../app/gateway.c, which does the
 * work and is tested on the Mac. This file only gives it a UART to the
 * CC1312, NVS, the clock, WiFi and MQTT (docs/PLAN.md 4.2).
 *
 * One task owns the gateway; MQTT's task hands it messages through a queue,
 * as the gateway is not thread-safe. */
#include <stdatomic.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "bootloader_random.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "bytes.h"
#include "gateway.h"

static const char *TAG = "tado";

#define CC1312_UART      UART_NUM_1
#define CC1312_BAUD      460800
#define NVS_NAMESPACE    "tado"
/* A zone in NVS: zone, owner EUI (wire order), setpoint (centi-C, LE). */
#define ZONE_RECORD_LEN  11
/* Before this the clock is not set (2025-01-01): no time answers. */
#define CLOCK_SET_AFTER  1735689600

/* --- NVS -------------------------------------------------------------------- */

static nvs_handle_t nvs;

/* Stops here rather than run with an identity the devices do not know. */
static void __attribute__((noreturn)) halt(const char *why)
{
    ESP_LOGE(TAG, "%s; not starting", why);
    for (;;)
        vTaskDelay(portMAX_DELAY);
}

static bool nvs_ok(esp_err_t err, const char *what)
{
    if (err != ESP_OK)
        ESP_LOGE(TAG, "nvs %s: %s", what, esp_err_to_name(err));
    return err == ESP_OK;
}

/* A set, then the commit that makes it last. */
static bool save(esp_err_t set, const char *what)
{
    return nvs_ok(set, what) && nvs_ok(nvs_commit(nvs), "commit");
}

static bool load_identity(struct gateway_identity *id)
{
    size_t eui_len = sizeof id->coordinator, key_len = sizeof id->network_key;
    esp_err_t eui = nvs_get_blob(nvs, "eui", id->coordinator, &eui_len);
    esp_err_t key = nvs_get_blob(nvs, "key", id->network_key, &key_len);
    if (eui == ESP_ERR_NVS_NOT_FOUND && key == ESP_ERR_NVS_NOT_FOUND)
        return false;
    if (eui != ESP_OK || key != ESP_OK || eui_len != 8 || key_len != 16) {
        /* Never mint a new key over a damaged one: joined devices need it. */
        halt("the identity in NVS is incomplete");
    }
    /* Left as they are when missing: no floor stops the gateway (gateway.c). */
    id->channel_khz = GATEWAY_CHANNEL_KHZ;
    nvs_get_u32(nvs, "floor", &id->counter_floor);
    nvs_get_u32(nvs, "channel", &id->channel_khz);
    if (nvs_get_u32(nvs, "home", &id->home_id) != ESP_OK || id->home_id == 0) {
        /* The devices know their home by its id: a new one would orphan them. */
        halt("no home id in NVS");
    }
    uint8_t zones[SESSION_MAX_ZONES * ZONE_RECORD_LEN];
    size_t len = sizeof zones;
    if (nvs_get_blob(nvs, "zones", zones, &len) == ESP_OK) {
        for (size_t i = 0; i + ZONE_RECORD_LEN <= len && id->n_zones < SESSION_MAX_ZONES; i += ZONE_RECORD_LEN) {
            struct gateway_zone *z = &id->zones[id->n_zones++];
            z->zone = zones[i];
            memcpy(z->owner, &zones[i + 1], 8);
            z->setpoint = get16(&zones[i + 9]);
        }
    }
    return true;
}

static void save_identity(const struct gateway_identity *id)
{
    bool ok = nvs_ok(nvs_set_blob(nvs, "eui", id->coordinator, 8), "eui") &&
              nvs_ok(nvs_set_blob(nvs, "key", id->network_key, 16), "key") &&
              nvs_ok(nvs_set_u32(nvs, "floor", id->counter_floor), "floor") &&
              nvs_ok(nvs_set_u32(nvs, "channel", id->channel_khz), "channel") &&
              save(nvs_set_u32(nvs, "home", id->home_id), "home");
    if (!ok)
        halt("could not save a new identity");
}

/* --- the gateway's platform ------------------------------------------------- */

static struct gateway gw;
static esp_mqtt_client_handle_t mqtt;
static atomic_bool mqtt_up;

static void gw_log(void *ctx, const char *line)
{
    (void)ctx;
    ESP_LOGI(TAG, "%s", line);
}

static void gw_rng(void *ctx, uint8_t *out, size_t n)
{
    (void)ctx;
    esp_fill_random(out, n); /* a true RNG while WiFi runs (and see app_main) */
}

static uint32_t gw_unix(void *ctx)
{
    (void)ctx;
    time_t t = time(NULL);
    return t > CLOCK_SET_AFTER ? (uint32_t)t : 0;
}

static void gw_publish(void *ctx, const char *topic, const char *payload, bool retain)
{
    (void)ctx;
    /* Queued, not sent from this task: a slow broker must not hold up the
     * devices' answers (time has to go within 5 s). */
    if (atomic_load(&mqtt_up))
        esp_mqtt_client_enqueue(mqtt, topic, payload, 0, retain ? 1 : 0, retain, true);
}

static bool gw_save_floor(void *ctx, uint32_t floor)
{
    (void)ctx;
    return save(nvs_set_u32(nvs, "floor", floor), "floor");
}

static void gw_save_channel(void *ctx, uint32_t khz)
{
    (void)ctx;
    save(nvs_set_u32(nvs, "channel", khz), "channel");
}

static void gw_save_zones(void *ctx, const struct gateway_zone *zones, size_t n)
{
    (void)ctx;
    uint8_t blob[SESSION_MAX_ZONES * ZONE_RECORD_LEN];
    for (size_t i = 0; i < n; i++) {
        uint8_t *r = &blob[i * ZONE_RECORD_LEN];
        r[0] = zones[i].zone;
        memcpy(&r[1], zones[i].owner, 8);
        put16(&r[9], zones[i].setpoint);
    }
    save(nvs_set_blob(nvs, "zones", blob, n * ZONE_RECORD_LEN), "zones");
}

static void link_write(void *ctx, const uint8_t *bytes, size_t len)
{
    (void)ctx;
    uart_write_bytes(CC1312_UART, bytes, len);
}

/* Waits for the first byte only, then takes what else is buffered. */
static size_t link_read(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    (void)ctx;
    int n = uart_read_bytes(CC1312_UART, buf, 1, pdMS_TO_TICKS(timeout_ms));
    if (n <= 0)
        return 0;
    size_t more = 0;
    uart_get_buffered_data_len(CC1312_UART, &more);
    if (more > cap - 1)
        more = cap - 1;
    if (more) {
        int m = uart_read_bytes(CC1312_UART, buf + 1, more, 0);
        if (m > 0)
            n += m;
    }
    return (size_t)n;
}

static uint32_t link_now(void *ctx)
{
    (void)ctx;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* --- the CC1312 ------------------------------------------------------------- */

static void cc1312_init(void)
{
    const uart_config_t uart = {
        .baud_rate = CC1312_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(CC1312_UART, 4096, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(CC1312_UART, &uart));
    ESP_ERROR_CHECK(uart_set_pin(CC1312_UART, CONFIG_TADO_CC1312_TX_GPIO, CONFIG_TADO_CC1312_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    /* nRST: open drain, only ever pulled low or released (docs/WIRING.md). */
    const gpio_config_t reset = {
        .pin_bit_mask = 1ull << CONFIG_TADO_CC1312_RESET_GPIO,
        .mode = GPIO_MODE_OUTPUT_OD,
    };
    ESP_ERROR_CHECK(gpio_set_level(CONFIG_TADO_CC1312_RESET_GPIO, 1));
    ESP_ERROR_CHECK(gpio_config(&reset));
    /* Start it from reset, so its state is known; the gateway configures it. */
    gpio_set_level(CONFIG_TADO_CC1312_RESET_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(CONFIG_TADO_CC1312_RESET_GPIO, 1);
}

/* --- MQTT and its messages to the gateway task ------------------------------ */

enum command_kind { COMMAND_ANNOUNCE, COMMAND_MESSAGE };

struct command {
    enum command_kind kind;
    char topic[48];
    char payload[16];
    size_t len;
};

static QueueHandle_t commands;

static void on_mqtt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg, (void)base;
    esp_mqtt_event_handle_t ev = data;
    struct command c = {0};
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "mqtt connected");
        atomic_store(&mqtt_up, true);
        esp_mqtt_client_subscribe(mqtt, HA_PAIR_TOPIC, 1);
        c.kind = COMMAND_ANNOUNCE;
        xQueueSend(commands, &c, 0);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "mqtt disconnected");
        atomic_store(&mqtt_up, false);
        break;
    case MQTT_EVENT_DATA:
        /* Whole short messages only: a command is a few bytes. */
        if (ev->topic_len <= 0 || (size_t)ev->topic_len >= sizeof c.topic || ev->data_len > ev->total_data_len ||
            ev->current_data_offset != 0 || (size_t)ev->data_len > sizeof c.payload)
            break;
        c.kind = COMMAND_MESSAGE;
        memcpy(c.topic, ev->topic, (size_t)ev->topic_len);
        memcpy(c.payload, ev->data, (size_t)ev->data_len);
        c.len = (size_t)ev->data_len;
        xQueueSend(commands, &c, 0);
        break;
    default:
        break;
    }
}

static void mqtt_start(void)
{
    const esp_mqtt_client_config_t cfg = {
        .broker.address.uri = CONFIG_TADO_MQTT_URI,
        .credentials.username = CONFIG_TADO_MQTT_USERNAME,
        .credentials.authentication.password = CONFIG_TADO_MQTT_PASSWORD,
        .credentials.client_id = "tado-gateway",
        .session.last_will = {.topic = HA_STATUS_TOPIC, .msg = HA_OFFLINE, .qos = 1, .retain = 1},
    };
    mqtt = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(mqtt, ESP_EVENT_ANY_ID, on_mqtt, NULL);
    ESP_ERROR_CHECK(esp_mqtt_client_start(mqtt));
}

/* --- WiFi and time ---------------------------------------------------------- */

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg, (void)data;
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) {
        /* The gateway keeps serving the devices without WiFi (PLAN.md 3.2). */
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "wifi up");
    }
}

static void wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL));
    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, CONFIG_TADO_WIFI_SSID, sizeof cfg.sta.ssid);
    strncpy((char *)cfg.sta.password, CONFIG_TADO_WIFI_PASSWORD, sizeof cfg.sta.password);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_TADO_SNTP_SERVER);
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp));
}

/* --- the gateway task ------------------------------------------------------- */

static void gateway_task(void *arg)
{
    (void)arg;
    struct command c;
    for (;;) {
        while (xQueueReceive(commands, &c, 0) == pdTRUE) {
            if (c.kind == COMMAND_ANNOUNCE)
                gateway_announce(&gw);
            else
                gateway_command(&gw, c.topic, c.payload, c.len);
        }
        gateway_run(&gw, 50);
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        /* Not erased here: the partition holds the network key. */
        ESP_LOGE(TAG, "nvs_flash_init: %s; not starting", esp_err_to_name(err));
        return;
    }
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs));
    commands = xQueueCreate(8, sizeof(struct command));

    struct gateway_identity id = {0};
    if (!load_identity(&id)) {
        /* The network key must be truly random: without WiFi running yet, the
         * RNG needs its entropy source switched on (esp_random.h). */
        bootloader_random_enable();
        gateway_new_identity(&id, gw_rng, NULL);
        bootloader_random_disable();
        save_identity(&id);
        ESP_LOGW(TAG, "new identity: home %lu; devices must be paired to it", (unsigned long)id.home_id);
    }
    ESP_LOGI(TAG, "home %lu, %u zone(s), counter floor %lu", (unsigned long)id.home_id, (unsigned)id.n_zones,
             (unsigned long)id.counter_floor);

    wifi_start();
    cc1312_init();
    static const struct gateway_ops ops = {.log = gw_log,
                                           .rng = gw_rng,
                                           .unix_time = gw_unix,
                                           .publish = gw_publish,
                                           .save_counter_floor = gw_save_floor,
                                           .save_channel = gw_save_channel,
                                           .save_zones = gw_save_zones};
    static const struct gwlink_ops link = {.write = link_write, .read = link_read, .now_ms = link_now};
    gateway_init(&gw, &id, &ops, &link);

    mqtt_start();
    xTaskCreate(gateway_task, "gateway", 16384, NULL, 5, NULL);
}
