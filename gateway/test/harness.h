#ifndef GATEWAY_HARNESS_H
#define GATEWAY_HARNESS_H

/* The gateway's tests: the shared harness plus the suites in other files. */
#include "../../firmware/test/harness.h"
#include "tado.h"

/* A zone at home and on at setpoint (centi-C); with overlay >= 0 a manual
 * overlay at that setpoint, else 6240=0: the shape most tests need. */
static inline size_t home_state(uint8_t *out, size_t cap, uint16_t setpoint, int32_t overlay)
{
    struct tado_zone_block b = {.on = true, .setpoint = setpoint};
    if (overlay >= 0) {
        b.overlay = TADO_OVERLAY_MANUAL;
        b.overlay_on = true;
        b.overlay_setpoint = (uint16_t)overlay;
    }
    return tado_zone_block(out, cap, &b);
}

void run_session_tests(void);
void run_hlc_tests(void);
void run_gateway_tests(void);
void run_ha_tests(void);
void run_web_tests(void);

#endif
