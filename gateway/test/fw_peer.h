#ifndef FW_PEER_H
#define FW_PEER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mac.h"

/* The CC1312 firmware as a test peer (fw_peer.c): its clocks, the bytes it
 * wrote to the host and not yet delivered, and the last radio job. */
extern uint32_t fw_ms, fw_rat;
extern uint8_t fw_out[8192];
extern size_t fw_out_len;
extern uint8_t job_frame[MAC_MAX_FRAME];
extern size_t job_len;
extern uint8_t job_token;
extern int jobs;
extern bool fw_unsupported;

#endif
