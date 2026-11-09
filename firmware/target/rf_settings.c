/* Proprietary-mode radio setup for the tado PHY, matching freedo's working
 * CC1101 register set (reference/freedo/cc1101/cc1101.go): 50 kbps 2-GFSK,
 * 25 kHz deviation, 4-byte preamble, sync word D391 sent twice (CC1101
 * "30/32" sync mode), 1-byte length, no whitening, CC1101-style CRC-16.
 * Overrides are TI's CC1312R defaults for this rate (SmartRF Studio, as
 * shipped in contiki-ng's prop-settings.c). */
#include "rf_settings.h"

#include DeviceFamily_constructPath(driverlib/rf_mailbox.h)
#include DeviceFamily_constructPath(rf_patches/rf_patch_cpe_prop.h)

RF_Mode rf_prop_mode = {
    .rfMode = RF_MODE_AUTO,
    .cpePatchFxn = &rf_patch_cpe_prop,
    .mcePatchFxn = 0,
    .rfePatchFxn = 0,
};

static uint32_t overrides[] __attribute__((aligned(4))) = {
    /* DC/DC regulator: in TX use DCDCCTL5[3:0]=0x7 (DITHER_EN=0, IPEAK=7). */
    (uint32_t)0x00F788D3,
    /* TX: PA ramp time, PACTL2.RC=0x3. */
    ADI_2HALFREG_OVERRIDE(0, 16, 0x8, 0x8, 17, 0x1, 0x1),
    /* RX: AGC reference level 0x1A. */
    HW_REG_OVERRIDE(0x609C, 0x001A),
    /* RX: RSSI offset -1 dB. */
    (uint32_t)0x000188A3,
    /* RX: anti-aliasing filter bandwidth 0xD. */
    ADI_HALFREG_OVERRIDE(0, 61, 0xF, 0xD),
    /* TX: wait time before turning off the ramp 0x1A. */
    HW_REG_OVERRIDE(0x6028, 0x001A),
    (uint32_t)0xFFFFFFFF,
};

rfc_CMD_PROP_RADIO_DIV_SETUP_t rf_cmd_setup = {
    .commandNo = CMD_PROP_RADIO_DIV_SETUP,
    .status = IDLE,
    .startTrigger.triggerType = TRIG_NOW,
    .condition.rule = COND_NEVER,
    .modulation.modType = 0x1,      /* GFSK */
    .modulation.deviation = 0x64,   /* 100 x 250 Hz = 25 kHz */
    .modulation.deviationStepSz = 0x0,
    .symbolRate.preScale = 0xF,
    .symbolRate.rateWord = 0x8000,  /* 50 kbps */
    .symbolRate.decimMode = 0x0,
    .rxBw = 0x55,                   /* ~137 kHz; freedo's CC1101 uses 135 kHz */
    .preamConf.nPreamBytes = 0x4,
    .preamConf.preamMode = 0x1,     /* 1010..., as the CC1101 sends it */
    .formatConf.nSwBits = 0x20,     /* D391 D391 */
    .formatConf.bBitReversal = 0x0,
    .formatConf.bMsbFirst = 0x1,
    .formatConf.fecMode = 0x0,
    .formatConf.whitenMode = 0x0,   /* no whitening; default CRC-16 */
    .config.frontEndMode = 0x0,
    .config.biasMode = 0x1,
    .config.analogCfgMode = 0x0,
    .config.bNoFsPowerUp = 0x0,
    .txPower = 0xCC14,              /* ~+13 dBm on the CC1312R LaunchPad */
    .pRegOverride = overrides,
    .centerFreq = 0x0364,           /* 868 MHz */
    .intFreq = 0x8000,
    .loDivider = 0x05,
};

rfc_CMD_FS_t rf_cmd_fs = {
    .commandNo = CMD_FS,
    .status = IDLE,
    .startTrigger.triggerType = TRIG_NOW,
    .condition.rule = COND_NEVER,
    .frequency = 868,
    .fractFreq = 0x4CCD,            /* .300 MHz; set from CONFIG */
};
