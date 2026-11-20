#
# Makefile for Audio Module
#

include $(COMPILATION_RULES)

MODULE_ROOT ?= $(shell pwd)
MODULES_INCLUDE = $(MODULE_ROOT)/../include
BUILD_DIR ?= $(MODULE_ROOT)/build
SRC_DIR := $(MODULE_ROOT)

SRCS := $(wildcard $(SRC_DIR)/*.c) \
        $(wildcard $(SRC_DIR)/hal/${CONFIG_SOC_PLATFORM}/*.c)

OBJS := $(patsubst $(SRC_DIR)/%,$(BUILD_DIR)/%,$(SRCS:.c=.o))

INCLUDE := $(HARDWARE_INCLUDE)
INCLUDE += -I$(MODULE_ROOT)
INCLUDE += -I$(MODULE_ROOT)/../include

# Unlike other modules, this Makefile builds with rules.mk CFLAGS only and never
# sees the top-level additions (HARDWARE_CFLAGS). Add the SDK version macros so
# per-SDK guards in the HAL sources work:
#   -DV2_04 / -DV2_05 ... per-release tag (legacy)
#   -DAK_SDK_VERSION_NUM=205 (V2.05 -> 205) for >= comparisons
ifneq (,$(CONFIG_SOC_SDK_VERSION))
CFLAGS += -D$(subst .,_,$(CONFIG_SOC_SDK_VERSION))
CFLAGS += -DAK_SDK_VERSION_NUM=$(subst .,,$(subst V,,$(CONFIG_SOC_SDK_VERSION)))
endif

$(shell cp -f $(MODULE_ROOT)/../include/audio.h $(MODULES_INCLUDE))

.PHONY: all clean

all: $(OBJS)
	@echo "Build hardware/audio completed!"

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDE) -c $< -o $@

$(BUILD_DIR)/hal/$(CONFIG_SOC_PLATFORM)/%.o: $(SRC_DIR)/hal/$(CONFIG_SOC_PLATFORM)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDE) -c $< -o $@

clean:
	@echo "Cleaning audio build files..."
	$(RM) -rf $(BUILD_DIR)
	@find $(MODULE_ROOT) -name "*.o" -type f -delete 2>/dev/null || true
	@find $(MODULE_ROOT) -name "*.d" -type f -delete 2>/dev/null || true

