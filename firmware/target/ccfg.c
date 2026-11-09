/* Customer configuration (CCFG, last flash page). Without a valid CCFG the ROM
 * boot code does not start the image. Keep the ROM serial bootloader enabled,
 * with the backdoor on BTN-1 (DIO13, active low): holding BTN-1 through a
 * reset enters the bootloader even if the image is broken. Debug access stays
 * at the SDK default (enabled); never lock it. */
#define SET_CCFG_BL_CONFIG_BOOTLOADER_ENABLE 0xC5
#define SET_CCFG_BL_CONFIG_BL_LEVEL          0x0
#define SET_CCFG_BL_CONFIG_BL_PIN_NUMBER     13
#define SET_CCFG_BL_CONFIG_BL_ENABLE         0xC5

#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(startup_files/ccfg.c)
