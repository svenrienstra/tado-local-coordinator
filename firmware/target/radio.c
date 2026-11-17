/* The radio on the TI RF driver, proprietary mode. RX runs continuously; every
 * transmission is a command chain [TX...] -> RX, posted after aborting the
 * running RX, so the radio is back in RX as soon as the last frame is out.
 * Received frames are copied out of the RF queue in the driver callback and
 * handed to the coordinator from the main loop. */
#include <string.h>

#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(driverlib/rf_data_entry.h)
#include DeviceFamily_constructPath(driverlib/rf_mailbox.h)
#include DeviceFamily_constructPath(driverlib/rf_prop_mailbox.h)
#include <ti/drivers/dpl/HwiP.h>

#include "coordinator.h"
#include "platform.h"
#include "rf_settings.h"

#define RAT_PER_US         4u
#define RAT_PER_BYTE       (8u * 20u * RAT_PER_US) /* 50 kbps */
#define WAKE_STEP_RAT      (WAKE_STEP_US * RAT_PER_US)
/* The Data frame starts 0.52 ms after the last wake-up frame ends, as the
 * commercial bridge's after a full train: a device wakes on the rendezvous the
 * train announced and listens only briefly. A wake-up frame is preamble 4 +
 * sync 4 + length 1 + 12 + CRC 2 bytes on air. */
#define WAKE_AIR_RAT       ((4u + 4u + 1u + MAC_WAKEUP_LEN + 2u) * RAT_PER_BYTE)
#define DATA_AFTER_WAKE    (WAKE_AIR_RAT + 520u * RAT_PER_US)
#define CHAIN_LEAD_RAT     (2u * RAT_PER_MS)       /* time to set up a chain */

/* RF queue entries: length byte, frame, RSSI, 4-byte RAT timestamp. */
#define ENTRY_DATA  (1 + MAC_MAX_FRAME + 1 + 4)
#define ENTRY_SIZE  ((sizeof(rfc_dataEntryGeneral_t) - 1 + ENTRY_DATA + 3) & ~3u)
#define RX_ENTRIES  4
#define RX_SLOTS    8
#define STATUS_ERROR 0x0800u /* set in every ERROR_* and PROP_ERROR_* code */
#define CHAIN_END   (RF_EventLastCmdDone | RF_EventCmdAborted | RF_EventCmdStopped | \
                     RF_EventCmdCancelled)

static RF_Object rf_object;
static RF_Handle rf;
static volatile RF_CmdHandle current; /* the chain that ends in RX */

static uint8_t rx_entries[RX_ENTRIES][ENTRY_SIZE] __attribute__((aligned(4)));
static dataQueue_t rx_queue;
static rfc_dataEntryGeneral_t *rx_read; /* the next entry the radio finishes */
static rfc_CMD_PROP_RX_t rx_cmd;
/* The RF core's RX statistics are 16-bit and restart with every RX command;
 * they are folded into 32-bit totals before each new chain and on reading. */
static rfc_propRxOutput_t rx_out;
static uint16_t rx_ok_seen, rx_nok_seen;
static uint32_t rx_ok_total, rx_nok_total;

/* Frames copied out of the RF queue, waiting for the main loop. */
struct rx_slot {
    uint8_t frame[MAC_MAX_FRAME];
    uint8_t len;
    int8_t rssi;
    uint32_t rat_end;
};
static struct rx_slot slots[RX_SLOTS];
static volatile uint32_t slot_head, slot_tail;

/* One chain: up to a full wake-up train plus the Data frame. */
static rfc_CMD_PROP_TX_t tx_cmds[MAC_TRAIN_FULL + 1];
static uint8_t wake_frames[MAC_TRAIN_FULL][MAC_WAKEUP_LEN];
static uint8_t tx_frame[MAC_MAX_FRAME];
static volatile unsigned chain_tx;  /* TX commands in the current chain */
static volatile bool rx_restart;
static unsigned tx_checked;         /* TX commands of the chain checked for errors */

/* RF command errors, counted by the callback and reported by radio_poll. */
static volatile uint32_t rf_errors;
static volatile uint16_t rf_error_status;
static uint32_t rf_errors_reported;

/* The coordinator job of the chain: set on air by radio_send_job, finished by
 * the callback or radio_set_channel, handed back and cleared by radio_poll, or
 * dropped by radio_abort_job. */
static volatile enum { JOB_NONE, JOB_ON_AIR, JOB_OK, JOB_FAILED } job;
static uint8_t job_token; /* the coordinator's, handed back with the outcome */

/* Written by the radio CPU, behind the compiler's back. */
static uint16_t tx_status(unsigned i)
{
    return *(volatile uint16_t *)&tx_cmds[i].status;
}

/* The chain's last TX has not run yet. */
static bool tx_pending(void)
{
    return chain_tx > 0 && tx_status(chain_tx - 1) <= ACTIVE;
}

static void note_status(uint16_t st)
{
    if (st & STATUS_ERROR) {
        rf_error_status = st;
        rf_errors++;
    }
}

/* Main context only, like post_chain. */
static void account_rx(void)
{
    uint16_t ok = *(volatile uint16_t *)&rx_out.nRxOk;
    uint16_t nok = *(volatile uint16_t *)&rx_out.nRxNok;
    rx_ok_total += (uint16_t)(ok - rx_ok_seen);
    rx_nok_total += (uint16_t)(nok - rx_nok_seen);
    rx_ok_seen = ok;
    rx_nok_seen = nok;
}

/* The radio fills the ring in order, so frames are taken in order from the
 * read entry up to the first one it has not finished. */
static void drain_queue(void)
{
    while (rx_read->status == DATA_ENTRY_FINISHED) {
        rfc_dataEntryGeneral_t *e = rx_read;
        uint8_t *d = &e->data;
        uint8_t stored = d[0];
        if (stored >= 1 + 4 && stored - 5 <= MAC_MAX_FRAME && slot_head - slot_tail < RX_SLOTS) {
            struct rx_slot *s = &slots[slot_head % RX_SLOTS];
            s->len = (uint8_t)(stored - 5);
            memcpy(s->frame, &d[1], s->len);
            s->rssi = (int8_t)d[1 + s->len];
            uint32_t ts;
            memcpy(&ts, &d[2 + s->len], 4);
            /* The RF core dates a packet at its first preamble bit, though it
             * detects the sync word (TI, "Time synchronization over the air").
             * The frame ends after preamble, sync word, length byte, frame and
             * 2-byte CRC. The callback runs about 0.47 ms after that end. */
            unsigned head = rf_cmd_setup.preamConf.nPreamBytes + rf_cmd_setup.formatConf.nSwBits / 8u;
            s->rat_end = ts + (uint32_t)(head + 1 + s->len + 2) * RAT_PER_BYTE;
            slot_head++;
        }
        e->status = DATA_ENTRY_PENDING;
        rx_read = (rfc_dataEntryGeneral_t *)e->pNextEntry;
    }
}

static void callback(RF_Handle h, RF_CmdHandle ch, RF_EventMask ev)
{
    (void)h;
    if (ev & RF_EventRxEntryDone)
        drain_queue();
    if (ch != current)
        return; /* an aborted chain reporting late */
    /* A TX command's status is final once it has run. */
    while (tx_checked < chain_tx && tx_status(tx_checked) > ACTIVE)
        note_status(tx_status(tx_checked++));
    /* The job ends with its Data frame, the chain's last TX. Its status is read
     * on any event rather than counting CmdDone events, which can merge. */
    if (job == JOB_ON_AIR) {
        uint16_t st = tx_status(chain_tx - 1);
        if (st == PROP_DONE_OK)
            job = JOB_OK;
        else if (st > ACTIVE || (ev & CHAIN_END))
            job = JOB_FAILED;
    }
    /* The RX at the end of the chain never ends on its own, but on an error
     * such as a full RX queue. */
    if (ev & CHAIN_END) {
        note_status(*(volatile uint16_t *)&rx_cmd.status);
        rx_restart = true;
    }
}

static void init_rx(void)
{
    for (int i = 0; i < RX_ENTRIES; i++) {
        rfc_dataEntryGeneral_t *e = (rfc_dataEntryGeneral_t *)rx_entries[i];
        e->pNextEntry = rx_entries[(i + 1) % RX_ENTRIES];
        e->status = DATA_ENTRY_PENDING;
        e->config.type = DATA_ENTRY_TYPE_GEN;
        e->config.lenSz = 1;
        e->length = ENTRY_DATA;
    }
    rx_queue.pCurrEntry = rx_entries[0];
    rx_read = (rfc_dataEntryGeneral_t *)rx_entries[0];
    rx_queue.pLastEntry = NULL;

    rx_cmd = (rfc_CMD_PROP_RX_t){
        .commandNo = CMD_PROP_RX,
        .startTrigger.triggerType = TRIG_NOW,
        .condition.rule = COND_NEVER,
        .pktConf.bRepeatOk = 1,
        .pktConf.bRepeatNok = 1,
        .pktConf.bUseCrc = 1,
        .pktConf.bVarLen = 1,
        .rxConf.bAutoFlushIgnored = 1,
        .rxConf.bAutoFlushCrcErr = 1,
        .rxConf.bAppendRssi = 1,
        .rxConf.bAppendTimestamp = 1,
        .syncWord = RF_SYNC_WORD,
        .maxPktLen = MAC_MAX_FRAME,
        .endTrigger.triggerType = TRIG_NEVER,
        .pQueue = &rx_queue,
        .pOutput = (uint8_t *)&rx_out,
    };
}

/* Cancels the running chain. current is cleared first, so the callbacks the
 * cancel raises count as late and are ignored. */
static void abort_current(void)
{
    RF_CmdHandle old = current;
    current = RF_SCHEDULE_CMD_ERROR;
    if (old >= 0)
        RF_cancelCmd(rf, old, 0);
}

/* Aborts the running RX and posts n TX commands followed by RX. */
static void post_chain(unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        tx_cmds[i].pNextOp = (rfc_radioOp_t *)(i + 1 < n ? (void *)&tx_cmds[i + 1] : (void *)&rx_cmd);
    rx_cmd.status = IDLE;
    rx_restart = false;
    abort_current();
    chain_tx = n;
    tx_checked = 0;
    account_rx();
    rx_out = (rfc_propRxOutput_t){0};
    rx_ok_seen = rx_nok_seen = 0;
    rfc_radioOp_t *head = n ? (rfc_radioOp_t *)&tx_cmds[0] : (rfc_radioOp_t *)&rx_cmd;
    /* CmdDone fires as each command of the chain ends. TxDone does not fire
     * for CMD_PROP_TX, and LastCmdDone waits for the trailing RX to stop. */
    current = RF_postCmd(rf, head, RF_PriorityNormal, callback,
                         RF_EventRxEntryDone | RF_EventCmdDone | RF_EventLastCmdDone);
}

static void set_tx(rfc_CMD_PROP_TX_t *cmd, const uint8_t *frame, size_t len, uint32_t start)
{
    *cmd = (rfc_CMD_PROP_TX_t){
        .commandNo = CMD_PROP_TX,
        .startTrigger.triggerType = TRIG_ABSTIME,
        .startTrigger.pastTrig = 1, /* late is better than never */
        .startTime = start,
        .condition.rule = COND_ALWAYS,
        .pktConf.bUseCrc = 1,
        .pktConf.bVarLen = 1,
        .pktLen = (uint8_t)len,
        .syncWord = RF_SYNC_WORD,
        .pPkt = (uint8_t *)frame,
    };
}

void radio_init(void)
{
    RF_Params params;
    RF_Params_init(&params);
    rf = RF_open(&rf_object, &rf_prop_mode, (RF_RadioSetup *)&rf_cmd_setup, &params);
    RF_runCmd(rf, (RF_Op *)&rf_cmd_fs, RF_PriorityNormal, NULL, 0);
    init_rx();
    current = RF_SCHEDULE_CMD_ERROR;
    post_chain(0);
}

void radio_set_channel(uint32_t khz)
{
    rf_cmd_fs.frequency = (uint16_t)(khz / 1000u);
    rf_cmd_fs.fractFreq = (uint16_t)(((khz % 1000u) << 16) / 1000u);
    abort_current();
    /* The aborted chain's own events now count as late; fail its job here. */
    if (job == JOB_ON_AIR)
        job = JOB_FAILED;
    RF_runCmd(rf, (RF_Op *)&rf_cmd_fs, RF_PriorityNormal, NULL, 0);
    post_chain(0);
}

uint32_t radio_rat_now(void)
{
    return RF_getCurrentTime();
}

bool radio_send_at(const uint8_t *frame, size_t len, uint32_t rat_start)
{
    /* Half duplex: an answer that collides with a running job is refused;
     * the device retries. */
    if (job != JOB_NONE || tx_pending())
        return false;
    memcpy(tx_frame, frame, len);
    set_tx(&tx_cmds[0], tx_frame, len, rat_start);
    post_chain(1);
    return true;
}

bool radio_send_job(const uint8_t dst_short[2], uint8_t seq, uint16_t n_wake,
                    const uint8_t *frame, size_t len, uint8_t token)
{
    if (job != JOB_NONE || tx_pending() || n_wake > MAC_TRAIN_FULL)
        return false;
    uint32_t t = RF_getCurrentTime() + CHAIN_LEAD_RAT;
    for (unsigned i = 0; i < n_wake; i++) {
        mac_build_wakeup(wake_frames[i], seq, dst_short, mac_train_rendezvous((int)i));
        set_tx(&tx_cmds[i], wake_frames[i], MAC_WAKEUP_LEN, t + i * WAKE_STEP_RAT);
    }
    if (n_wake > 0)
        t += (n_wake - 1u) * WAKE_STEP_RAT + DATA_AFTER_WAKE;
    memcpy(tx_frame, frame, len);
    set_tx(&tx_cmds[n_wake], tx_frame, len, t);
    job_token = token;
    post_chain(n_wake + 1u);
    /* Only now: until post_chain the previous chain is current, and its events
     * would be read against this job's commands. */
    job = JOB_ON_AIR;
    return true;
}

unsigned radio_abort_job(void)
{
    unsigned ran = 0;
    while (ran < chain_tx && tx_status(ran) > ACTIVE)
        ran++;
    job = JOB_NONE;
    post_chain(0);
    return ran;
}

void radio_rx_counts(uint32_t *ok, uint32_t *crc_fail)
{
    account_rx();
    *ok = rx_ok_total;
    *crc_fail = rx_nok_total;
}

void radio_poll(struct coordinator *c)
{
    while (rf_errors_reported != rf_errors) {
        rf_errors_reported++;
        coord_on_radio_error(c, rf_error_status);
    }
    /* Completion first, so an Enhanced ACK drained below finds its job sent. */
    if (job == JOB_OK || job == JOB_FAILED) {
        bool ok = job == JOB_OK;
        job = JOB_NONE;
        coord_on_job_sent(c, job_token, ok);
    }
    while (slot_tail != slot_head) {
        struct rx_slot *s = &slots[slot_tail % RX_SLOTS];
        coord_on_rx(c, s->frame, s->len, s->rssi, s->rat_end);
        slot_tail++;
    }
    if (rx_restart && job == JOB_NONE)
        post_chain(0);
}
