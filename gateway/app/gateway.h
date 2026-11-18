#ifndef GATEWAY_H
#define GATEWAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gwlink.h"
#include "ha.h"
#include "session.h"

/* The gateway application around the session (docs/PLAN.md 4.2), still
 * without any ESP-IDF: the ESP32 build gives it a UART, NVS, the clock and
 * MQTT through gateway_ops, and the Mac tests give it the CC1312 firmware.
 *
 * It configures the CC1312 and configures it again whenever it rebooted (it
 * keeps nothing across a reset), keeps the frame counter ahead in reserved
 * blocks, keeps the carrier the CC1312 tunes to, opens a pair window for a
 * zone on command and makes the device that joins the zone's owner, and
 * publishes what the devices report for Home Assistant.
 *
 * Not thread-safe: one task calls everything. */

/* Frame counters reserved at a time. The floor in flash stays a block ahead
 * of the CC1312, so a restart never reuses a counter. */
#define GATEWAY_COUNTER_BLOCK   65536u
/* How far a kept carrier may sit from GATEWAY_CHANNEL_KHZ (the firmware's
 * TUNE_MAX_HZ), as the Java host keeps it. */
#define GATEWAY_CHANNEL_KHZ     868335u
#define GATEWAY_TUNE_MAX_KHZ    40u
#define GATEWAY_MAX_DEVICES     16
#define GATEWAY_PAIR_WINDOW_S   120u
/* Counters the CC1312 may use on its own (beacon answers) between two TXs. */
#define GATEWAY_COUNTER_MARGIN  256u

struct gateway_zone {
    uint8_t zone;
    uint8_t owner[8];
    uint16_t setpoint; /* centi-degrees C */
};

/* What the gateway keeps in flash, loaded at boot. From then on the session
 * holds the zones; save_zones gets them whenever they change. */
struct gateway_identity {
    uint8_t coordinator[8];
    uint8_t network_key[16];
    uint32_t counter_floor;  /* the next unreserved frame counter */
    uint32_t channel_khz;
    uint32_t home_id;
    size_t n_zones;
    struct gateway_zone zones[SESSION_MAX_ZONES];
};

struct gateway_ops {
    void *ctx;
    void (*log)(void *ctx, const char *line);
    void (*rng)(void *ctx, uint8_t *out, size_t n);
    /* Unix seconds, 0 while the clock is not set. */
    uint32_t (*unix_time)(void *ctx);
    /* Dropped while MQTT is down: state is retained and sent again on the
     * next report or gateway_announce. */
    void (*publish)(void *ctx, const char *topic, const char *payload, bool retain);
    /* Persist; false when the write failed. */
    bool (*save_counter_floor)(void *ctx, uint32_t floor);
    void (*save_channel)(void *ctx, uint32_t khz);
    void (*save_zones)(void *ctx, const struct gateway_zone *zones, size_t n);
};

struct gateway {
    struct gateway_ops ops;
    struct gateway_identity id;
    struct gwlink link;
    struct session session;

    bool configured;          /* the CC1312 has our CONFIG since its last boot */
    uint32_t config_order;    /* gwlink order of that CONFIG's reply */
    uint32_t config_floor;    /* reserved for a CONFIG not yet taken, or 0 */
    uint32_t counter_next;    /* the next frame counter the CC1312 gives out */
    uint32_t next_config_ms;  /* when to try again after a failed CONFIG */
    struct gwlink_event ev;   /* the event being handled */

    bool pairing;             /* a pair window the gateway opened is running */
    uint8_t pair_zone;
    uint32_t pair_until_ms;

    /* What goes to Home Assistant: published at the end of gateway_run when
     * dirty, so a device's answer never waits for MQTT. */
    struct {
        bool used, dirty;
        uint8_t eui[8];
        struct ha_device d;
        uint32_t discovered;  /* entities whose discovery config went out */
    } devices[GATEWAY_MAX_DEVICES];
    struct {                  /* by the index of the session's zone */
        bool used, dirty;
        uint8_t zone;
        struct tado_report r;
        uint32_t discovered;
    } zones[SESSION_MAX_ZONES];
    char topic[96], payload[768]; /* publishing scratch */
};

/* A free-running millisecond clock comes in through link_ops.now_ms. */
void gateway_init(struct gateway *g, const struct gateway_identity *id, const struct gateway_ops *ops,
                  const struct gwlink_ops *link_ops);

/* Fills a new identity: a random coordinator EUI ending in c5 1b 00, a random
 * network key and home id, counter floor 1, the default carrier. */
void gateway_new_identity(struct gateway_identity *id, void (*rng)(void *ctx, uint8_t *out, size_t n), void *ctx);

/* Reads the link for up to timeout_ms, handles what came, and drives the
 * timers; configures the CC1312 first when it needs it. Call in a loop. */
void gateway_run(struct gateway *g, uint32_t timeout_ms);

/* Opens a pair window of `seconds` for a device that becomes zone's owner. */
bool gateway_pair(struct gateway *g, uint8_t zone, uint16_t seconds);

/* An MQTT message for the gateway: the pair command takes a zone number. */
void gateway_command(struct gateway *g, const char *topic, const char *payload, size_t len);

/* After MQTT (re)connects: discovery and state of everything known again. */
void gateway_announce(struct gateway *g);

#endif
