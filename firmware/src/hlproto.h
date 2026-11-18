#ifndef HLPROTO_H
#define HLPROTO_H

/* Host link v0 message types, statuses and flag bits (docs/HOSTLINK.md),
 * shared by the CC1312 and the gateway. Framing is in hlframe.h. */

#define HL_REPLY_FLAG 0x80

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

enum hl_status {
    HL_OK = 0x00,
    HL_BAD_ARG = 0x01,
    HL_NOT_CONFIGURED = 0x02,
    HL_BUSY = 0x03,
    HL_UNKNOWN = 0x04,
    HL_UNSUPPORTED = 0x05, /* silicon older than rev E (radio_supported) */
};

/* TX request flags. */
#define HL_TXF_PAIRING_KEY 0x01
#define HL_TXF_BROADCAST   0x02
#define HL_TXF_PENDING     0x04
#define HL_TXF_TRAIN_SHIFT 4
#define HL_TXF_TRAIN_MASK  0x30
#define HL_TRAIN_NONE      0
#define HL_TRAIN_FULL      1
#define HL_TRAIN_ANCHORED  2 /* short train on the device's sample; full when unknown */

/* RX event flags. */
#define HL_RXF_PAIRING_KEY 0x01
#define HL_RXF_BROADCAST   0x02
#define HL_RXF_ACKED       0x04

/* TX_DONE results. */
enum tx_result { TX_ACKED = 0, TX_SENT = 1, TX_NO_ACK = 2, TX_RADIO_ERROR = 3 };

#endif
