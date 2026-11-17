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
/* Starts a job: wake_frames wake-up frames to dst_short carrying seq, then
 * the Data frame. Finishes with coord_on_job_sent() carrying token. False
 * when busy. */
bool radio_send_job(const uint8_t dst_short[2], uint8_t seq, uint16_t wake_frames,
                    const uint8_t *frame, size_t len, uint8_t token);
/* Drops the running job, if any; no coord_on_job_sent() follows for it.
 * Returns how many of its TX commands (wake-up frames, then Data) had run. */
unsigned radio_abort_job(void);
/* Frames the RF core received since boot with a good and with a bad CRC. */
void radio_rx_counts(uint32_t *ok, uint32_t *crc_fail);

/* Answer delays after the end of the received frame, in RAT ticks. The
 * commercial beacon starts 2.4-7.4 ms after the request; the Enhanced ACK
 * 0-1 ms after the Data frame. */
#define RAT_PER_MS            4000u
#define BEACON_DELAY_RAT      (3u * RAT_PER_MS)
#define ACK_DELAY_RAT         (RAT_PER_MS / 2u)
#define ACK_WAIT_MS           30u
#define WAKE_STEP_MS          4u  /* wake-up frame spacing */
#define JOB_MARGIN_MS         50u /* chain setup, gap before Data, PHY overhead */
#define TX_QUEUE_LEN          4
#define UNVERIFIED_PER_SECOND 10
#define LOG_PER_SECOND        10
#define LOG_MAX               64

struct tx_job {
    uint8_t handle;
    bool unicast;
    uint8_t seq;
    uint8_t dst_short[2];
    uint16_t wake_frames;
    uint8_t frame[MAC_MAX_FRAME];
    size_t len;
};

/* STATS counters kept by the core, since boot (docs/HOSTLINK.md). */
struct coord_stats {
    uint32_t rx_unverified;
    uint32_t beacons_answered;
    uint32_t acks_sent;
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
/* A CRC-valid frame (without PHY CRC) that ended at rat_end. */
void coord_on_rx(struct coordinator *c, const uint8_t *frame, size_t len, int8_t rssi,
                 uint32_t rat_end);
/* The radio finished the Data frame of the job started with token. */
void coord_on_job_sent(struct coordinator *c, uint8_t token, bool ok);
/* The radio saw a command end with an error status. */
void coord_on_radio_error(struct coordinator *c, uint16_t status);
/* Timers and queue; call often. */
void coord_poll(struct coordinator *c);

#endif
