#ifndef GATEWAY_H
#define GATEWAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gwlink.h"
#include "ha.h"
#include "install.h"
#include "session.h"

/* The gateway application around the session (docs/PLAN.md 4.2), still
 * without any ESP-IDF: the ESP32 build gives it a UART, NVS, the clock and
 * MQTT through gateway_ops, and the Mac tests give it the CC1312 firmware.
 *
 * It configures the CC1312 and configures it again whenever it rebooted (it
 * keeps nothing across a reset), keeps the frame counter ahead in reserved
 * blocks, and keeps the carrier the CC1312 tunes to. It keeps the rooms and
 * the joined devices: a device is paired first and then put in a room, as in
 * the tado app (docs/PLAN.md 3.9), and the room's zone is served from then on.
 * It publishes what the devices report for Home Assistant.
 *
 * Not thread-safe: one task calls everything. */

/* Frame counters reserved at a time. The floor in flash stays a block ahead
 * of the CC1312, so a restart never reuses a counter. */
#define GATEWAY_COUNTER_BLOCK   65536u
/* How far a kept carrier may sit from GATEWAY_CHANNEL_KHZ (the firmware's
 * TUNE_MAX_HZ), as the Java host keeps it. */
#define GATEWAY_CHANNEL_KHZ     868335u
#define GATEWAY_TUNE_MAX_KHZ    40u
#define GATEWAY_PAIR_WINDOW_S   120u
/* Counters the CC1312 may use on its own (beacon answers) between two TXs. */
#define GATEWAY_COUNTER_MARGIN  256u

/* What the gateway keeps in flash, loaded at boot. */
struct gateway_identity {
    uint8_t coordinator[8];
    uint8_t network_key[16];
    uint32_t counter_floor;  /* the next unreserved frame counter */
    uint32_t channel_khz;
    uint32_t home_id;
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
    /* The installation, encoded (install.h), whenever it changed. */
    void (*save_install)(void *ctx, const uint8_t *blob, size_t len);
};

enum gateway_pair_state {
    GATEWAY_PAIR_IDLE,
    GATEWAY_PAIR_OPEN,     /* the window runs: long-press the device */
    GATEWAY_PAIR_JOINING,  /* d/pair is out */
    GATEWAY_PAIR_JOINED,   /* the last window ended with pair_joined joining */
    GATEWAY_PAIR_FAILED,
    GATEWAY_PAIR_CLOSED,   /* the last window ended with no device */
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

    enum gateway_pair_state pair;
    uint32_t pair_until_ms;
    uint8_t pair_joined[8];
    uint8_t refused[8];       /* the last device the full table turned away */
    uint8_t next_zone;        /* where a new room's number is looked for */

    /* The joined devices and the rooms, with what goes to Home Assistant:
     * published at the end of gateway_run when dirty, so a device's answer
     * never waits for MQTT. */
    struct gateway_device {
        bool used, dirty;
        uint8_t eui[8];
        uint8_t zone;         /* its room, 0 for none */
        bool waiting;         /* asked for its config, which needs a room */
        struct ha_device d;   /* with its serial, "" until a request named it */
        uint32_t discovered;  /* entities whose discovery config went out */
    } devices[INSTALL_MAX_DEVICES];
    struct gateway_room {
        bool used, dirty;
        uint8_t zone;
        uint16_t setpoint;    /* centi-degrees C */
        char name[INSTALL_NAME_MAX + 1];
        struct tado_report r; /* its owner's reports */
        uint32_t discovered;
    } rooms[INSTALL_MAX_ROOMS];
    char topic[96], payload[768]; /* publishing scratch */
};

/* A free-running millisecond clock comes in through link_ops.now_ms. */
/* in: the rooms and devices saved last (save_install), read here only. */
void gateway_init(struct gateway *g, const struct gateway_identity *id, const struct install *in,
                  const struct gateway_ops *ops, const struct gwlink_ops *link_ops);

/* Fills a new identity: a random coordinator EUI ending in c5 1b 00, a random
 * network key and home id, counter floor 1, the default carrier. */
void gateway_new_identity(struct gateway_identity *id, void (*rng)(void *ctx, uint8_t *out, size_t n), void *ctx);

/* Reads the link for up to timeout_ms, handles what came, and drives the
 * timers; configures the CC1312 first when it needs it. Call in a loop. */
void gateway_run(struct gateway *g, uint32_t timeout_ms);

/* Opens a pair window of `seconds`; the device that joins goes in no room.
 * False while a device joins or the device table is full. The setup page
 * and tado/gateway/pair/set call it. */
bool gateway_pair(struct gateway *g, uint16_t seconds);
void gateway_close_pair(struct gateway *g);

/* Seconds the pair window has left, 0 when none runs. */
uint32_t gateway_pair_left(struct gateway *g);

/* Rooms (setup page). A new room takes the next free zone number (numbers
 * of deleted rooms come again only after 255) and the default setpoint; it returns that number, 0 when the table is full or the
 * name is not one install_name_ok takes. Deleting a room puts its devices in
 * no room. False when there is no such room or the name is bad. */
uint8_t gateway_add_room(struct gateway *g, const char *name);
bool gateway_rename_room(struct gateway *g, uint8_t zone, const char *name);
bool gateway_delete_room(struct gateway *g, uint8_t zone);

/* Sets the room's setpoint (centi-degrees C, HA_SETPOINT_MIN..MAX), keeps it,
 * and writes it to the room's device in its next window; that ends an
 * overlay set on the device. The device's next report confirms it. False
 * for no such room or a setpoint out of range. */
bool gateway_set_setpoint(struct gateway *g, uint8_t zone, uint16_t centi);

/* Puts a joined device in a room (zone 0: none). One device per room until
 * zones get members (PLAN.md M4): false when the room has another device,
 * or there is no such room or device. The device takes it at its next config
 * read; one waiting for a room asks about once a minute. */
bool gateway_assign(struct gateway *g, const uint8_t eui[8], uint8_t zone);

struct gateway_device *gateway_device(struct gateway *g, const uint8_t eui[8]);
struct gateway_room *gateway_room(struct gateway *g, uint8_t zone);

/* An MQTT message for the gateway: tado/gateway/pair/set opens a pair window,
 * tado/zone/<n>/setpoint/set sets a room's setpoint (degrees C). Empty and
 * retained messages are ignored: an old command, or its clearing. */
void gateway_command(struct gateway *g, const char *topic, const char *payload, size_t len, bool retained);

/* After MQTT (re)connects: discovery and state of everything known again. */
void gateway_announce(struct gateway *g);

#endif
