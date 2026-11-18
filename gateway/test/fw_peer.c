/* The CC1312 firmware's platform hooks for the Mac: firmware/src's
 * coordinator and host link run as the gateway tests' peer, with a stub
 * radio that records the jobs it is given. */
#include <string.h>

#include "coordinator.h"
#include "fw_peer.h"

uint32_t fw_ms, fw_rat;
uint8_t fw_out[8192]; /* bytes the firmware wrote to the host, not yet delivered */
size_t fw_out_len;
uint8_t job_frame[MAC_MAX_FRAME];
size_t job_len;
uint8_t job_token;
int jobs;

uint32_t platform_ms(void) { return fw_ms; }
uint32_t radio_rat_now(void) { return fw_rat; }
void radio_set_channel(uint32_t khz) { (void)khz; }
bool radio_send_at(const uint8_t *frame, size_t len, uint32_t rat_start)
{
    (void)frame, (void)len, (void)rat_start;
    return true;
}
bool radio_send_job(const uint8_t dst_short[2], uint8_t seq, uint16_t wake_frames, uint32_t rat_start,
                    bool broadcast, const uint8_t *frame, size_t len, uint8_t token)
{
    (void)dst_short, (void)seq, (void)wake_frames, (void)rat_start, (void)broadcast;
    memcpy(job_frame, frame, len);
    job_len = len;
    job_token = token;
    jobs++;
    return true;
}
unsigned radio_abort_job(void) { return 0; }
bool radio_busy(void) { return false; }
bool fw_unsupported; /* silicon older than rev E: CONFIG refused */
bool radio_supported(void) { return !fw_unsupported; }
void radio_rx_counts(uint32_t *ok, uint32_t *crc_fail)
{
    *ok = 7;
    *crc_fail = 1;
}
void hostlink_write(const uint8_t *data, size_t len)
{
    if (fw_out_len + len <= sizeof fw_out) {
        memcpy(&fw_out[fw_out_len], data, len);
        fw_out_len += len;
    }
}
