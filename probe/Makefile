# Join probe (docs/PROBE.md) — the device side of the join, for reading what a
# coordinator sends in d/pair. NOT the coordinator firmware; see ../firmware.
#
#   make test    build and run the host tests (cc); no hardware
#
# The on-air build (its own target/main.c + flashing, like ../firmware) comes
# with the hardware step and is not here yet.

FW := ../firmware

# Shared, not copied: the same MAC/crypto the coordinator and the bridge use.
CORE := $(FW)/src/crc.c $(FW)/src/ccm.c $(FW)/src/mac.c
SRCS := joiner.c $(CORE) test/test_joiner.c $(FW)/test/harness.c $(FW)/test/aes_soft.c

HOST_CC     ?= cc
HOST_CFLAGS := -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined \
  -I. -I$(FW)/src -I$(FW)/test

BUILD := build

.PHONY: test clean

test: $(BUILD)/test
	$(BUILD)/test ../golden/vectors.txt

$(BUILD)/test: $(SRCS) joiner.h $(wildcard $(FW)/src/*.h) $(FW)/test/harness.h
	@mkdir -p $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(SRCS)

clean:
	rm -rf $(BUILD)
