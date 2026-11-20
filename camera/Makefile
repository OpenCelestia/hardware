#
# Makefile for Camera Module
#

include $(COMPILATION_RULES)

# Parent hardware/Makefile passes config.mk flags (e.g. __CHIP_AK3918AV130_SERIES) here;
# rules.mk alone is not enough when the overlay file is missing or stale.
CFLAGS := $(HARDWARE_CFLAGS)

MODULE_ROOT ?= $(shell pwd)
MODULES_INCLUDE = $(MODULE_ROOT)/../include
BUILD_DIR ?= $(MODULE_ROOT)/build
SRC_DIR := $(MODULE_ROOT)

SRCS := $(wildcard $(SRC_DIR)/*.c) \
        $(wildcard $(SRC_DIR)/hal/${CONFIG_SOC_PLATFORM}/*.c)

OBJS := $(patsubst $(SRC_DIR)/%,$(BUILD_DIR)/%,$(SRCS:.c=.o))
DEPS := $(OBJS:.o=.d)

# Header dependency tracking. Without it an .o depends only on its own .c, so editing a
# private header (e.g. hal/<SoC>/hal_camera.h) rebuilds just the .c files that also changed
# and leaves the rest linked against the OLD struct layout. Adding a field to
# hal_camera_channel_t that way shifts codec[]/ai_config offsets in the stale objects only,
# which yields pointer fields read out of int fields and equally arbitrary corruption
# elsewhere -- silent, and not reproducible from the source alone. Hardening, not a fix for
# any one bug.
# -MP emits phony targets for the headers so a deleted/renamed header does not wedge make.
DEPFLAGS := -MMD -MP

INCLUDE := $(HARDWARE_INCLUDE)
INCLUDE += -I$(MODULE_ROOT)
INCLUDE += -I$(MODULE_ROOT)/../include
INCLUDE += -I$(MODULE_ROOT)/hal/$(CONFIG_SOC_PLATFORM)
INCLUDE += -I$(MODULE_ROOT)/../soc_platform
INCLUDE += -I$(HARDWARE_ROOT)/soc_platform
INCLUDE += -I$(HARDWARE_ROOT)/osd
INCLUDE += -I$(HARDWARE_ROOT)/osd/hal/$(CONFIG_SOC_PLATFORM)

$(shell cp -f $(MODULE_ROOT)/../include/camera.h $(MODULES_INCLUDE))

.PHONY: all clean

all: $(OBJS)
	@echo "Build hardware/camera completed!"

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDE) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/hal/$(CONFIG_SOC_PLATFORM)/%.o: $(SRC_DIR)/hal/$(CONFIG_SOC_PLATFORM)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDE) $(DEPFLAGS) -c $< -o $@

-include $(DEPS)

clean:
	@echo "Cleaning camera build files..."
	$(RM) -rf $(BUILD_DIR)
	@find $(MODULE_ROOT) -name "*.o" -type f -delete 2>/dev/null || true
	@find $(MODULE_ROOT) -name "*.d" -type f -delete 2>/dev/null || true

