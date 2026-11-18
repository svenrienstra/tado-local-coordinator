#ifndef GATEWAY_HARNESS_H
#define GATEWAY_HARNESS_H

/* The gateway's tests: the shared harness plus the suites in other files. */
#include "../../firmware/test/harness.h"

void run_session_tests(void);
void run_hlc_tests(void);

#endif
