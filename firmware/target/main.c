/* CC1312R1 coordinator: one main loop, no RTOS. */
#include <stdint.h>
#include <string.h>

#include <NoRTOS.h>
#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(inc/hw_ints.h)
#include DeviceFamily_constructPath(inc/hw_nvic.h)
#include DeviceFamily_constructPath(inc/hw_types.h)
#include DeviceFamily_constructPath(driverlib/aon_rtc.h)
#include DeviceFamily_constructPath(driverlib/sys_ctrl.h)
#include <ti/drivers/Power.h>

#include "coordinator.h"
#include "platform.h"

static struct coordinator coord;

static void unhandled_irq(void)
{
    for (;;) {
    }
}

/* The NoRTOS startup's flash table holds only the 16 system vectors, but
 * driverlib's IntRegister seeds its RAM table with NUM_INTERRUPTS entries
 * copied from VTOR on the first HwiP_create: the peripheral entries would be
 * code bytes, and an IRQ without a handler would jump into them. Point VTOR
 * at a complete table first, so such an IRQ stops in unhandled_irq. */
static void vectors_init(void)
{
    static uint32_t table[NUM_INTERRUPTS] __attribute__((aligned(256)));
    memcpy(table, (const void *)HWREG(NVIC_VTABLE), 16 * sizeof table[0]);
    for (unsigned i = 16; i < NUM_INTERRUPTS; i++)
        table[i] = (uint32_t)unhandled_irq;
    HWREG(NVIC_VTABLE) = (uint32_t)table;
}

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
    vectors_init();
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
