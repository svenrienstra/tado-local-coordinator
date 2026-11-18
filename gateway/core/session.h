#ifndef SESSION_H
#define SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "link.h"
#include "mac.h"
#include "reasm.h"
#include "tado.h"

/* The coordinator logic of the gateway, ported from the Java host's
 * JoinSession (host/.../join/JoinSession.java), which is its spec.
 *
 * It always serves the installation: Router Advertisements, each device's
 * configuration and zone documents, the zone state with any overlay set on a
 * device, and the write that ends that overlay. A pair window opens on top of
 * that on command: Router Advertisements then go under both keys, and the
 * joiner gets d/pair with the network key (docs/JOIN.md).
 *
 * Time comes in as now_ms, a free-running millisecond clock; everything else
 * goes through session_ops. Not thread-safe: feed events and ticks from one
 * task. */

/* RFC 4861 MIN_DELAY_BETWEEN_RAS, which the commercial bridge follows. */
#define SESSION_RA_MIN_GAP_MS        3000u
/* Unsolicited Router Advertisements every 200-600 s, as the bridge's. A device
 * that hears none for about 20 minutes stops reporting and only solicits. */
#define SESSION_RA_INTERVAL_MIN_MS   200000u
#define SESSION_RA_INTERVAL_MAX_MS   600000u
/* How long to wait for the 2.04 after d/pair left the radio before trying again. */
#define SESSION_PAIR_ANSWER_TIMEOUT_MS 6000u
#define SESSION_PAIR_ATTEMPTS        4
/* The host link may lose a TX_DONE. A frame still unreported after this long
 * (two 1 s trains plus queueing) is taken as having left the radio. */
#define SESSION_TX_DONE_TIMEOUT_MS   5000u
/* How long an answer is kept for a repeated request (same device and MID). */
#define SESSION_ANSWER_LIFETIME_MS   60000u
/* How long a zone state write waits for the device's ACK before going again. */
#define SESSION_ZONE_WRITE_TIMEOUT_MS 10000u
/* RFC 7252 MAX_RETRANSMIT: the first write and up to four repeats. */
#define SESSION_ZONE_WRITE_ATTEMPTS  5
/* An hour, as the tado app offers for a manual change. */
#define SESSION_DEFAULT_OVERLAY_MS   3600000u
#define SESSION_DEFAULT_SETPOINT     1800 /* centi-degrees C */

#define SESSION_MAX_ZONES   8
#define SESSION_MAX_ANSWERS 16
#define SESSION_MAX_PACKET  MAC_UNICAST_MAX_PACKET /* what one unicast frame carries */
#define SESSION_MAX_DOCUMENT 256 /* the zone document, the largest, is about 230 bytes */

enum session_pair_state {
    SESSION_PAIR_IDLE,        /* no pair window */
    SESSION_PAIR_WAITING,     /* window open, no joiner yet or d/pair not sent */
    SESSION_PAIR_PAIRING,     /* d/pair sent, waiting for its 2.04 */
    SESSION_PAIR_JOINED,      /* the joiner answered 2.04 under our network key */
    SESSION_PAIR_FAILED,
};

struct session_ops {
    void *ctx;
    /* Queues a frame and returns the CC1312's handle for it (0-255), or a
     * negative value when it was refused. It must not return before the
     * CC1312 answered: over the UART that means sending the TX through hlc,
     * feeding received bytes until its reply, and queueing the events that
     * come meanwhile for after the call; an HL_BUSY answer (queue full) is
     * retried there, every 100 ms for up to 8 s. The session calls it twice in
     * a row for a pair-window Router Advertisement. */
    int (*tx)(void *ctx, const struct link_tx *tx);
    /* A progress line, without a newline. */
    void (*log)(void *ctx, const char *line);
    void (*rng)(void *ctx, uint8_t *out, size_t n);
    /* Wall-clock Unix seconds, for the devices' time requests; 0 while the
     * clock is not set, and the request goes unanswered (the device asks
     * again) rather than setting it to 1970. */
    uint32_t (*unix_time)(void *ctx);
    /* The zone's own setpoint in centi-degrees C, kept by the caller: the one
     * place it is set from (Home Assistant, the setup page) and saved. */
    uint16_t (*setpoint)(void *ctx, uint8_t zone);
    /* Optional. A report a device PUT or POSTed (d/<serial>/sen,
     * h/<home>/z/<n>/act, ...) that decoded, with its Uri-Path. Called before
     * its 2.04 goes out; it must not call back into the session. */
    void (*on_report)(void *ctx, const uint8_t device[8], const char *path, const struct tado_report *r);
    /* Optional. Each new CON request a device makes under our key, with its
     * code and Uri-Path, and whether the session answers it: a
     * d/<serial>/config GET from a device in no zone goes unanswered. Called
     * before the answer goes out; it must not call back into the session. */
    void (*on_request)(void *ctx, const uint8_t device[8], uint8_t code, const char *path, bool answered);
    /* Optional. The joiner answered d/pair under our network key. */
    void (*on_joined)(void *ctx, const uint8_t device[8]);
};

struct session_answer {
    bool used;
    uint8_t device[8];
    uint16_t mid;
    int handle;          /* -1 once the radio is done with it */
    uint32_t at_ms;      /* when it was first sent */
    uint32_t sent_ms;    /* when it last went to the radio */
    uint8_t len;
    uint8_t packet[SESSION_MAX_PACKET];
};

struct session_zone {
    bool used;
    uint8_t zone;
    uint8_t owner[8];
    /* A setpoint set on the owner, which the zone follows until it ends. */
    bool overlay;
    uint16_t overlay_setpoint;
    /* Compared as elapsed time: any length < 2^32 ms; 0 lasts until changed. */
    uint32_t overlay_start_ms, overlay_for_ms;
    /* The write that ends an overlay, until the device ACKs it. */
    bool writing;
    uint16_t write_mid;
    int write_handle;               /* the write's frame still on the CC1312, or -1 */
    bool write_newer;               /* write_packet changed while that frame waits: send on its TX_DONE */
    uint8_t write_attempts;
    uint32_t write_sent_ms;
    uint8_t write_len;
    uint8_t write_packet[SESSION_MAX_PACKET];
};

struct session {
    uint8_t coordinator[8];
    uint8_t network_key[16];
    struct session_ops ops;
    struct reasm reasm;

    uint32_t home_id;
    uint32_t overlay_ms;            /* 0: an overlay lasts until changed */
    struct session_zone zones[SESSION_MAX_ZONES];
    struct session_answer answers[SESSION_MAX_ANSWERS];
    uint8_t doc[SESSION_MAX_DOCUMENT]; /* the document a GET is answered from */
    uint16_t next_mid;

    int ra_handle;                  /* the last RA copy in flight, or -1 */
    uint32_t ra_sent_ms;
    bool ra_ended_once;
    uint32_t ra_ended_ms;
    uint32_t next_ra_ms;

    enum session_pair_state pair;
    bool has_joiner;
    uint8_t joiner[8];
    int pair_handle;
    uint32_t pair_sent_ms;
    uint16_t pair_mids[SESSION_PAIR_ATTEMPTS];
    int pair_attempts;
    bool pair_deadline_set;
    uint32_t pair_deadline_ms;
};

void session_init(struct session *s, const uint8_t coordinator[8], const uint8_t network_key[16],
                  const struct session_ops *ops, uint32_t now_ms);

/* The home of every h/<home>/... path (1..2^32-1), and how long a setpoint set
 * on a device lasts (0: until changed). */
void session_set_home(struct session *s, uint32_t home_id, uint32_t overlay_ms);

/* Makes owner the zone's owner and measuring device (its setpoint comes from
 * session_ops.setpoint). False when the zone table is full. */
bool session_set_zone(struct session *s, uint8_t zone, const uint8_t owner[8]);

/* The zone's setpoint changed (session_ops.setpoint has the new one): writes
 * the zone state to its owner in its next window, which ends an overlay set
 * on the device, and repeats it until the device ACKs. One write per zone
 * waits on the CC1312 at a time: a newer one replaces it and goes when that
 * frame left. False when the zone is not served. */
bool session_write_zone(struct session *s, uint8_t zone, uint32_t now_ms);

/* The zone, or NULL when it is not served. */
const struct session_zone *session_zone(const struct session *s, uint8_t zone);

/* Forgets the zone: it is no longer served. */
void session_drop_zone(struct session *s, uint8_t zone);

/* Opens a pair window here only: the caller opens the CC1312's own with
 * PAIR_WINDOW over the host link. */
void session_open_pair_window(struct session *s);
void session_close_pair_window(struct session *s);
enum session_pair_state session_pair_state(const struct session *s);

void session_on_event(struct session *s, const struct link_event *ev, uint32_t now_ms);

/* Call periodically (every few hundred ms) to drive retries and timers. */
void session_tick(struct session *s, uint32_t now_ms);

/* Stable per device, so a restart serves the same id: FNV-1a 64 of its EUI. */
void session_id(const uint8_t device[8], uint8_t out[8]);

#endif
