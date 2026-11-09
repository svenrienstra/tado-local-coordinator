/* Driver configuration the TI Power and RF drivers link against. */
#include <stddef.h>

#include <ti/drivers/Power.h>
#include <ti/drivers/power/PowerCC26X2.h>
#include <ti/drivers/rf/RF.h>

/* No standby policy: the coordinator receives continuously. */
const PowerCC26X2_Config PowerCC26X2_config = {
    .policyInitFxn = NULL,
    .policyFxn = NULL,
    .calibrateFxn = &PowerCC26XX_calibrate,
    .enablePolicy = false,
    .calibrateRCOSC_LF = true,
    .calibrateRCOSC_HF = true,
};

const RFCC26XX_HWAttrsV2 RFCC26XX_hwAttrs = {
    .hwiPriority = ~0,
    .swiPriority = 0,
    .xoscHfAlwaysNeeded = true,
    .globalCallback = NULL,
    .globalEventMask = 0,
};
