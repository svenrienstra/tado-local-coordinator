/* Customer configuration (CCFG, last flash page). Without a valid CCFG the ROM
 * boot code does not start the image. The ROM serial bootloader stays enabled
 * (it runs when the image is invalid). The backdoor pin is opt-in through
 * BL_BACKDOOR_PIN (`make BL_PIN=<dio>`, active low): on a board where that pin
 * reads low at reset, every boot would land in the bootloader and the radio
 * would look dead. Debug access stays at the SDK default (enabled); never
 * lock it. */
#define SET_CCFG_BL_CONFIG_BOOTLOADER_ENABLE 0xC5
#define SET_CCFG_BL_CONFIG_BL_LEVEL          0x0
#ifdef BL_BACKDOOR_PIN
#define SET_CCFG_BL_CONFIG_BL_PIN_NUMBER     BL_BACKDOOR_PIN
#define SET_CCFG_BL_CONFIG_BL_ENABLE         0xC5
#else
#define SET_CCFG_BL_CONFIG_BL_ENABLE         0xFF
#endif

#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(startup_files/ccfg.c)
