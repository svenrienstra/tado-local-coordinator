#ifndef RF_SETTINGS_H
#define RF_SETTINGS_H

#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(driverlib/rf_common_cmd.h)
#include DeviceFamily_constructPath(driverlib/rf_prop_cmd.h)
#include <ti/drivers/rf/RF.h>

/* The on-air sync word, as the RX and TX commands carry it. */
#define RF_SYNC_WORD 0xD391D391u

extern RF_Mode rf_prop_mode;
extern rfc_CMD_PROP_RADIO_DIV_SETUP_t rf_cmd_setup;
extern rfc_CMD_FS_t rf_cmd_fs;

#endif
