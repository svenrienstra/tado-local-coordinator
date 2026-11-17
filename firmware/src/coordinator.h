#ifndef COORDINATOR_H
#define COORDINATOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hostlink.h"
#include "mac.h"

/* The coordinator: host-link requests, the pair window, the key fallback,
 * seq/counter allocation and the TX queue. Portable; the platform supplies the
 * hooks below and calls the coord_on_* entry points from its main loop. */

#define FW_VERSION 0x0002

/* Host link message types (docs/HOSTLINK.md). */
enum {
    HL_PING = 0x01,
    HL_CONFIG = 0x02,
    HL_PAIR_WINDOW = 0x03,
    HL_TX = 0x04,
    HL_STATS = 0x05,
    HL_EV_LOG = 0xC0,
    HL_EV_RX = 0xC1,
    HL_EV_BEACON_ANSWERED = 0xC2,
    HL_EV_RX_UNVERIFIED = 0xC3,
    HL_EV_TX_DONE = 0xC4,
};

enum tx_result { TX_ACKED = 0, TX_SENT = 1, TX_NO_ACK = 2, TX_RADIO_ERROR = 3 };

/* Platform hooks. RAT = 4 MHz radio timer. */
uint32_t platform_ms(void);
uint32_t radio_rat_now(void);
/* Retunes. A job on air is aborted and finishes with coord_on_job_sent(false). */
void radio_set_channel(uint32_t khz);
/* Sends a frame starting at the given RAT time (no wake-up train). False when
 * the radio is busy with a job or another frame; nothing is sent then. */
bool radio_send_at(const uint8_t *frame, size_t len, uint32_t rat_start);
/* Starts a job: wake_frames wake-up frames to dst_short carrying seq, the
 * first at RAT time rat_start, then the Data frame. Finishes with
 * coord_on_job_sent() carrying token. False when busy. */
bool radio_send_job(const uint8_t dst_short[2], uint8_t seq, uint16_t wake_frames,
                    uint32_t rat_start, const uint8_t *frame, size_t len, uint8_t token);
/* Drops the running job, if any; no coord_on_job_sent() follows for it.
 * Returns how many of its TX commands (wake-up frames, then Data) had run. */
unsigned radio_abort_job(void);
/* Whether anything is on air or posted: a job, an ACK or a beacon answer.
 * radio_set_channel would cut it off. */
bool radio_busy(void);
/* Frames the RF core received since boot with a good and with a bad CRC. */
void radio_rx_counts(uint32_t *ok, uint32_t *crc_fail);

/* Answer delays after the end of the received frame, in RAT ticks. The
 * commercial beacon starts 2.4-7.4 ms after the request; the Enhanced ACK
 * 0-1 ms after the Data frame. */
#define RAT_PER_MS            4000u
#define BEACON_DELAY_RAT      (3u * RAT_PER_MS)
/* Enhanced ACK start after the frame ends: the commercial bridge's is 0-1 ms.
 * The commercial bridge starts its ACK 0.35-0.45 ms after the frame (measured
 * on our radio). Ours cannot be that early: a frame reaches the main loop about
 * 0.49 ms after its end, and the ACK is posted from its clear header right
 * after, so 0.55 ms is the earliest start met every time. */
#define ACK_DELAY_RAT         (RAT_PER_MS * 11u / 20u)
#define ACK_WAIT_MS           30u
/* Wake-up frame spacing, 12.4 rendezvous units: the commercial bridge's,
 * measured on our radio (2026-11-17). Rounded up to whole ms for budgets. */
#define WAKE_STEP_US          3968u
#define WAKE_STEP_MS          4u
#define JOB_MARGIN_MS         50u /* chain setup, gap before Data, PHY overhead */
/* A job reaches the radio this long before its first frame: posting it aborts
 * the receiver, so a job waiting for a device's sample stays queued until then. */
#define JOB_LEAD_RAT          (5u * RAT_PER_MS)
#define CHAIN_LEAD_RAT        (2u * RAT_PER_MS) /* the radio's time to set up a chain */

/* CSL anchors, as freedo's firmware keeps them. A device samples the channel
 * every period; the phase in its Enhanced ACK says how long before the ACK's
 * end it last did. A short train centred on its next sample, 2 x the guard
 * long, reaches it: 40 ms each side gives the bridge's 21 frames. The guard
 * grows with the anchor's age for clock drift. After a miss the next job goes
 * behind a full train; ANCHOR_MISS_LIMIT misses drop the anchor. */
#define CSL_PERIOD_RAT        (1000u * RAT_PER_MS)
#define CSL_UNIT_RAT          1280u /* 320 us: phase and rendezvous unit */
#define ANCHOR_GUARD_RAT      (40u * RAT_PER_MS)
#define ANCHOR_GUARD_PPM      30u
#define ANCHOR_GUARD_MAX_RAT  (CSL_PERIOD_RAT / 2u)
#define ANCHOR_MISS_LIMIT     4
#define ANCHORS               16
/* A device repeats a unicast it saw no Enhanced ACK for under the same MAC seq,
 * 1-3 s later. Each copy is ACKed, but only the first reaches the host, or it
 * answers every copy behind a train of its own. */
#define DEDUP_SOURCES         16
#define DEDUP_WINDOW_MS       30000u

/* Carrier tracking. The RF core estimates each frame's carrier offset in 1/64
 * of the 25 kHz deviation. Only our own devices count: decrypted frames and
 * their ACKs to our jobs. After TUNE_WINDOW readings the median of the
 * per-device means (devices with TUNE_MIN_PER_SOURCE readings) is logged; past
 * the deadband the radio follows it, at most every TUNE_INTERVAL_MS, at most
 * TUNE_MAX_HZ from the CONFIG frequency, and only while the radio is idle.
 * Readings near the estimator's limit are dropped. */
#define FREQ_OFFSET_UNIT_HZ_X64 25000 /* Hz per unit, times 64 */
#define TUNE_SOURCES          8
#define TUNE_WINDOW           16
#define TUNE_MIN_PER_SOURCE   4
#define TUNE_CLIP             120
#define TUNE_DEADBAND_HZ      4000
#define TUNE_MAX_HZ           40000
#define TUNE_INTERVAL_MS      120000u
#define TX_QUEUE_LEN          4
#ifdef SNIFF
#define UNVERIFIED_PER_SECOND 1000 /* every frame of a 254-frame train */
#else
#define UNVERIFIED_PER_SECOND 10
#endif
#define LOG_PER_SECOND        10
#define LOG_MAX               64

struct tx_job {
    uint8_t handle;
    bool unicast;
    uint8_t seq;
    uint8_t dst_short[2];
    uint8_t train;        /* TRAIN_NONE, _FULL or _ANCHORED, as the host asked */
    bool scheduled;       /* wake_frames, start and anchored are set */
    bool anchored;        /* goes behind a short train on the device's sample */
    uint16_t wake_frames;
    uint64_t start;       /* RAT time of the first frame */
    uint8_t frame[MAC_MAX_FRAME];
    size_t len;
};

struct tune_source {
    bool used;
    uint8_t short_addr[2];
    int32_t sum; /* readings, in units */
    uint16_t n;
};

struct last_seq {
    bool valid;
    uint8_t src[8];
    uint8_t seq;
    uint32_t at_ms;
};

struct anchor {
    bool valid;
    uint8_t short_addr[2];
    uint64_t sample;  /* RAT time of one sample of the device */
    uint64_t refresh; /* when it was learnt */
    uint8_t misses;
};

/* STATS counters kept by the core, since boot (docs/HOSTLINK.md). */
struct coord_stats {
    uint32_t rx_unverified;
    uint32_t beacons_answered;
    uint32_t acks_sent;
    uint32_t acks_late; /* posted after their start time; the radio sent them late */
    uint32_t jobs_anchored; /* jobs sent behind a short train on the device's sample */
    uint32_t rx_duplicates; /* repeated unicasts (same source and MAC seq), ACKed, not passed on */
    uint32_t answers_refused;
    uint32_t jobs_ok;
    uint32_t jobs_failed;
    uint32_t jobs_timed_out;
    uint32_t radio_errors;
};

struct coordinator {
    struct hostlink link;
    bool configured;
    uint8_t eui[8];
    uint8_t network_key[16];
    uint8_t seq;
    uint32_t counter;
    uint32_t pair_until_ms; /* window open while now < this */
    bool pair_open;

    uint64_t rat_high; /* RAT wraps every 1074 s: its upper 32 bits */
    uint32_t rat_last;
    struct anchor anchors[ANCHORS];
    struct last_seq last_seqs[DEDUP_SOURCES];

    uint32_t channel_khz;  /* from CONFIG */
    int32_t tune_khz;      /* our correction on top of it */
    struct tune_source tune[TUNE_SOURCES];
    uint16_t tune_readings;
    bool tune_pending;
    int32_t tune_step_khz;
    bool tuned_once;
    uint32_t tune_last_ms;

    struct tx_job queue[TX_QUEUE_LEN];
    size_t queue_len;
    bool job_on_air;      /* queue[0] handed to the radio */
    uint32_t job_deadline_ms; /* the radio has lost the job after this */
    bool awaiting_ack;    /* queue[0] sent, waiting for its Enhanced ACK */
    uint32_t ack_deadline_ms;
    uint8_t next_handle;

    uint32_t unverified_window_ms;
    unsigned unverified_count;
    uint32_t log_window_ms;
    unsigned log_count;
    struct coord_stats stats;
};

extern const uint8_t PAIRING_KEY[16];

void coord_init(struct coordinator *c);
/* Logs the boot line; call once, after coord_init. reset_reason is a short word. */
void coord_boot(struct coordinator *c, const char *reset_reason);
/* Bytes from the host UART. */
void coord_on_host_bytes(struct coordinator *c, const uint8_t *data, size_t len);
/* A CRC-valid frame (without PHY CRC) that ended at rat_end. freq_offset is the
 * RF core's estimate of how far its carrier sits from ours, in 1/64 of the
 * deviation (390.625 Hz at 25 kHz). */
void coord_on_rx(struct coordinator *c, const uint8_t *frame, size_t len, int8_t rssi,
                 uint32_t rat_end, int8_t freq_offset);
/* The radio finished the Data frame of the job started with token. */
void coord_on_job_sent(struct coordinator *c, uint8_t token, bool ok);
/* The radio saw a command end with an error status. */
void coord_on_radio_error(struct coordinator *c, uint16_t status);
/* Timers and queue; call often. */
void coord_poll(struct coordinator *c);

#endif
