#
# Makefile for Motion Detection Module
#

include $(COMPILATION_RULES)

MODULE_ROOT ?= $(shell pwd)
MODULES_INCLUDE = $(MODULE_ROOT)/../../include
BUILD_DIR ?= $(MODULE_ROOT)/build
SRC_DIR := $(MODULE_ROOT)

SRCS := $(wildcard $(SRC_DIR)/*.c) \
        $(wildcard $(SRC_DIR)/hal/$(CONFIG_SOC_PLATFORM)/*.c)

OBJS := $(patsubst $(SRC_DIR)/%,$(BUILD_DIR)/%,$(SRCS:.c=.o))

MODULE_INCLUDE := $(HARDWARE_INCLUDE)
MODULE_INCLUDE += -I$(MODULE_ROOT)/../../include
MODULE_INCLUDE += -I$(MODULE_ROOT)
MODULE_INCLUDE += -I$(SYSTEM_CORE_INSTALL_DIR)/include

HAL_INCLUDE := $(HARDWARE_INCLUDE)
HAL_INCLUDE += -I$(MODULE_ROOT)/../../include
HAL_INCLUDE += -I$(MODULE_ROOT)
HAL_INCLUDE += -I$(MODULE_ROOT)/hal/$(CONFIG_SOC_PLATFORM)
HAL_INCLUDE += -I$(SYSTEM_ROOT)/$(CONFIG_SOC_PLATFORM)/include

$(shell cp -f $(MODULE_ROOT)/../../include/motion_detect.h $(MODULES_INCLUDE))

.PHONY: all clean

all: $(OBJS)
	@echo "Build hardware/vision/motion_detect completed!"

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(MODULE_INCLUDE) -c $< -o $@

$(BUILD_DIR)/hal/$(CONFIG_SOC_PLATFORM)/%.o: $(SRC_DIR)/hal/$(CONFIG_SOC_PLATFORM)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(HAL_INCLUDE) -c $< -o $@

clean:
	@echo "Cleaning vision/motion_detect build files..."
	$(RM) -rf $(BUILD_DIR)
	@find $(MODULE_ROOT) -name "*.o" -type f -delete 2>/dev/null || true
