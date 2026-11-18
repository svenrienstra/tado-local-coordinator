# Gateway protocol core: plain C, built and tested on the Mac (PLAN.md M1).
# The ESP-IDF application around it comes with M2.

BUILD   := build
FW      := ../firmware/src
# Shared with the CC1312 firmware, not copied (PLAN.md 4.2).
SHARED  := $(FW)/crc.c $(FW)/ccm.c $(FW)/mac.c
CORE    := core/coap.c core/lowpan.c core/reasm.c core/tado.c core/session.c
FREEDO  := ../reference/freedo/tado/testdata

HOST_CC     ?= cc
HOST_CFLAGS := -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined \
               -Icore -I$(FW)

.PHONY: test clean

test: $(BUILD)/test
	$(BUILD)/test ../golden $(FREEDO)

$(BUILD)/test: $(CORE) $(SHARED) test/test_main.c test/test_session.c test/harness.h ../firmware/test/aes_soft.c $(wildcard core/*.h) $(wildcard $(FW)/*.h)
	@mkdir -p $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(CORE) $(SHARED) test/test_main.c test/test_session.c ../firmware/test/aes_soft.c

clean:
	rm -rf $(BUILD)
