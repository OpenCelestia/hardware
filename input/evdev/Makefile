#
# Makefile for Input evdev Module
#

include $(COMPILATION_RULES)
include $(PROJECT_CONFIG_FILE)

MODULE_ROOT ?= $(shell pwd)
BUILD_DIR ?= $(MODULE_ROOT)
_CELESTIA_ROOT := $(abspath $(dir $(PROJECT_CONFIG_FILE))/../..)
# Rootfs staging paths (inlined; build.sh exports SOC_PLATFORM_BUILD_DIR/ROOTFS_DIR).
REL_SOC_PLATFORM_DIR ?= system/$(CONFIG_SOC_PLATFORM)/$(CONFIG_SOC_SDK_VERSION)
ifndef SOC_PLATFORM_BUILD_DIR
PRODUCT_NAME ?= $(notdir $(patsubst %/,%,$(dir $(PROJECT_CONFIG_FILE))))
_CELESTIA_ROOT ?= $(abspath $(dir $(PROJECT_CONFIG_FILE))/../..)
SOC_PLATFORM_BUILD_DIR := $(_CELESTIA_ROOT)/out/$(PRODUCT_NAME)/build/$(REL_SOC_PLATFORM_DIR)
endif
ROOTFS_DIR ?= $(SOC_PLATFORM_BUILD_DIR)/rootfs

MODULES_INCLUDE = $(HARDWARE_ROOT)/include

SRCS := $(addprefix $(MODULE_ROOT)/, evdev.c) \
        $(MODULE_ROOT)/../input_module.c
OBJS := $(BUILD_DIR)/evdev.o \
        $(BUILD_DIR)/input_module.o

ifeq ($(CONFIG_HW_INPUT_KEYBOARD),y)
SRCS += $(MODULE_ROOT)/keyboard.c
OBJS += $(BUILD_DIR)/keyboard.o
endif

ifeq ($(CONFIG_HW_INPUT_TOUCH),y)
SRCS += $(MODULE_ROOT)/touch.c
OBJS += $(BUILD_DIR)/touch.o
endif

MODULE_INCLUDE := $(HARDWARE_INCLUDE)
MODULE_INCLUDE += -I$(MODULE_ROOT)
MODULE_INCLUDE += -I$(HARDWARE_ROOT)/include
MODULE_INCLUDE += -I$(SYSTEM_CORE_INSTALL_DIR)/include
ifeq ($(CONFIG_HW_INPUT_TOUCH),y)
MODULE_INCLUDE += -I$(THIRD_PARTY_INSTALL_DIR)/tslib/include
endif

CFLAGS := $(HARDWARE_CFLAGS)

.PHONY: all clean

all: $(OBJS)
	@echo "Build hardware/input/evdev completed!"

$(BUILD_DIR)/%.o: $(MODULE_ROOT)/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(MODULE_INCLUDE) -c $< -o $@

$(BUILD_DIR)/input_module.o: $(MODULE_ROOT)/../input_module.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(MODULE_INCLUDE) -c $< -o $@

ifeq ($(CONFIG_THIRD_PARTY_TSLIB), y)
install_tslib:
	@mkdir -p $(ROOTFS_DIR)/utils/usr/bin
	@if [ -d $(ROOTFS_DIR)/utils/usr/bin/tslib ]; then \
		rm -rf $(ROOTFS_DIR)/utils/usr/bin/tslib; \
	fi
	cp -rf $(THIRD_PARTY_INSTALL_DIR)/tslib $(ROOTFS_DIR)/utils/usr/bin/
endif

clean:
	$(RM) -rf $(BUILD_DIR)/*.o
