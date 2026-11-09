/* CC1312R1 coordinator: one main loop, no RTOS. */
#include <stdint.h>

#include <NoRTOS.h>
#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(driverlib/aon_rtc.h)
#include DeviceFamily_constructPath(driverlib/sys_ctrl.h)
#include <ti/drivers/Power.h>

#include "coordinator.h"
#include "platform.h"

static struct coordinator coord;

uint32_t platform_ms(void)
{
    /* AON RTC: 32.32 fixed-point seconds; keep 32.16 so x1000 fits 64 bits.
     * The truncation to 32 bits wraps cleanly, which before() relies on. */
    uint64_t t = AONRTCCurrent64BitValueGet() >> 16;
    return (uint32_t)((t * 1000u) >> 16);
}

static const char *reset_reason(void)
{
    switch (SysCtrlResetSourceGet()) {
    case RSTSRC_PWR_ON: return "por";
    case RSTSRC_PIN_RESET: return "pin";
    case RSTSRC_VDDS_LOSS: return "brownout";
    case RSTSRC_VDDR_LOSS: return "vddr";
    case RSTSRC_CLK_LOSS: return "clk_loss";
    case RSTSRC_SYSRESET: return "sw";
    case RSTSRC_WARMRESET: return "warm"; /* watchdog, lockup or CPU reset */
    case RSTSRC_WAKEUP_FROM_SHUTDOWN: return "shutdown";
    default: return "other";
    }
}

int main(void)
{
    Power_init();
    NoRTOS_start();
    uart_init();
    aes_hw_init();
    radio_init();
    coord_init(&coord);
    coord_boot(&coord, reset_reason());

    for (;;) {
        uint8_t buf[64];
        size_t n = uart_read(buf, sizeof buf);
        if (n)
            coord_on_host_bytes(&coord, buf, n);
        radio_poll(&coord);
        coord_poll(&coord);
        uart_flush();
    }
}
