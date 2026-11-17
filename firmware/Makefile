# CC1312R1 coordinator firmware.
#
#   make test    build and run the portable core's tests on the host (cc)
#   make all     build build/coordinator.hex (needs arm-none-eabi-gcc)
#   make flash   flash it with DSLite (UniFlash)
#   make clean

CONTIKI_NG ?= ../../contiki-ng
SDK        := $(abspath $(CONTIKI_NG))/arch/cpu/simplelink-cc13xx-cc26xx/lib/coresdk_cc13xx_cc26xx
DEVDIR     := $(SDK)/source/ti/devices/cc13x2_cc26x2
DRIVERLIB  := $(DEVDIR)/driverlib

# Arm GNU Toolchain from PATH (brew install --cask gcc-arm-embedded); override
# CROSS for another install, e.g. CROSS=/opt/arm/bin/arm-none-eabi-.
CROSS ?= arm-none-eabi-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE    := $(CROSS)size

CCXML  ?= CC1312R1F3.ccxml
# The DSLite binary itself, which takes the mode ("flash") as its first argument.
# Not the brew `dslite` wrapper: that adds "flash" on its own.
DSLITE ?= $(firstword $(wildcard \
  /Applications/TI/UniFlash/deskdb/content/TICloudAgent/osx/ccs_base/DebugServer/bin/DSLite \
  /Applications/ti/uniflash_*/deskdb/content/TICloudAgent/osx/ccs_base/DebugServer/bin/DSLite))

BUILD := build
CORE  := src/crc.c src/ccm.c src/mac.c src/hostlink.c src/coordinator.c

# --- host tests --------------------------------------------------------------

HOST_CC     ?= cc
HOST_CFLAGS := -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -Isrc

.PHONY: all test clean flash

test: $(BUILD)/test
	$(BUILD)/test ../golden/vectors.txt

$(BUILD)/test: $(CORE) test/test_main.c test/aes_soft.c $(wildcard src/*.h)
	@mkdir -p $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(CORE) test/test_main.c test/aes_soft.c

# --- target ------------------------------------------------------------------

DEFINES := -DDeviceFamily_CC13X2 -DDEVICE_LINE_CC13XX -DDEVICE_CC1312R

# ROM bootloader backdoor pin (DIO number, active low); empty = no backdoor.
# Only set it to a pin that is pulled high on the board, or every reset lands
# in the bootloader. `make clean` after changing it.
BL_PIN ?=
ifneq ($(BL_PIN),)
DEFINES += -DBL_BACKDOOR_PIN=$(BL_PIN)
endif

# SNIFF=1: report every frame the coordinator cannot open, overheard Enhanced
# ACKs included, with no rate limit; for timing captures on the bench.
# `make clean` after changing it.
ifneq ($(SNIFF),)
DEFINES += -DSNIFF
endif

CFLAGS := -std=c11 -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 \
  -Os -g -ffunction-sections -fdata-sections \
  $(DEFINES) -Isrc -Itarget -I$(SDK)/source -I$(SDK)/kernel/nortos -I$(SDK)/kernel/nortos/posix

LDFLAGS := -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 \
  -nostartfiles -Wl,--gc-sections -Wl,-T,target/cc1312r1.lds -Wl,-Map,$(BUILD)/coordinator.map \
  --specs=nano.specs --specs=nosys.specs

OWN_SRCS := $(CORE) target/main.c target/radio.c target/host_uart.c target/aes_hw.c \
  target/board.c target/rf_settings.c target/ccfg.c

NORTOS_DPL := ClockPTimer_nortos.c HwiPCC26XX_nortos.c MutexP_nortos.c \
  PowerCC26X2_nortos.c QueueP_nortos.c SemaphoreP_nortos.c SwiP_nortos.c SystemP_nortos.c \
  TimerPCC26XX_nortos.c

TARGET_SRCS := $(OWN_SRCS) \
  $(SDK)/kernel/nortos/startup/startup_cc13xx_cc26xx_gcc.c \
  $(SDK)/kernel/nortos/NoRTOS.c \
  $(addprefix $(SDK)/kernel/nortos/dpl/,$(NORTOS_DPL)) \
  $(SDK)/source/ti/drivers/rf/RFCC26X2_multiMode.c \
  $(SDK)/source/ti/drivers/power/PowerCC26X2.c \
  $(SDK)/source/ti/drivers/power/PowerCC26X2_calibrateRCOSC.c \
  $(SDK)/source/ti/drivers/utils/List.c \
  $(DRIVERLIB)/aes.c $(DRIVERLIB)/uart.c $(DRIVERLIB)/ioc.c $(DRIVERLIB)/prcm.c \
  $(DRIVERLIB)/sys_ctrl.c $(DRIVERLIB)/osc.c $(DRIVERLIB)/aon_batmon.c \
  $(DRIVERLIB)/setup.c $(DRIVERLIB)/setup_rom.c $(DRIVERLIB)/chipinfo.c \
  $(DRIVERLIB)/interrupt.c $(DRIVERLIB)/cpu.c $(DRIVERLIB)/rfc.c \
  $(DRIVERLIB)/ddi.c $(DRIVERLIB)/aux_sysif.c $(DRIVERLIB)/aon_rtc.c $(DRIVERLIB)/driverlib_release.c

TARGET_OBJS := $(patsubst %.c,$(BUILD)/obj/%.o,$(notdir $(TARGET_SRCS)))
vpath %.c $(sort $(dir $(TARGET_SRCS)))

# Warnings for our code only; the vendor sources are not ours to fix.
$(patsubst %.c,$(BUILD)/obj/%.o,$(notdir $(OWN_SRCS))): CFLAGS += -Wall -Wextra

all: $(BUILD)/coordinator.hex

$(BUILD)/obj/%.o: %.c
	@mkdir -p $(BUILD)/obj
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(TARGET_OBJS:.o=.d)

$(BUILD)/coordinator.elf: $(TARGET_OBJS) target/cc1312r1.lds
	$(CC) $(LDFLAGS) -o $@ $(TARGET_OBJS)
	$(SIZE) $@

$(BUILD)/coordinator.hex: $(BUILD)/coordinator.elf
	$(OBJCOPY) -O ihex $< $@

# Erase, program and verify, then pin-reset so the image starts from a clean
# boot rather than wherever the debugger left the core.
flash: $(BUILD)/coordinator.hex
	$(DSLITE) flash --config=$(CCXML) --flash --verify -e $<
	$(DSLITE) flash --config=$(CCXML) --post-flash-device-cmd=PinReset

clean:
	rm -rf $(BUILD)
