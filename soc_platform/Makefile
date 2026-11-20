#
# Makefile for SoC Platform Module
#

include $(COMPILATION_RULES)

MODULE_ROOT ?= $(shell pwd)
MODULES_INCLUDE = $(MODULE_ROOT)/../../include/modules
BUILD_DIR ?= $(MODULE_ROOT)/build
SRC_DIR := $(MODULE_ROOT)

SRCS := $(wildcard $(SRC_DIR)/*.c) \
        $(wildcard $(SRC_DIR)/hal/${CONFIG_SOC_PLATFORM}/*.c)

# Conditionally compile IPU HAL if camera or gpu module is enabled
IPU_HAL_SRC := $(wildcard $(SRC_DIR)/hal/${CONFIG_SOC_PLATFORM}/hal_ipu.c)
ifneq ($(IPU_HAL_SRC),)
ifeq ($(CONFIG_HW_CAMERA),y)
SRCS += $(IPU_HAL_SRC)
endif
ifeq ($(CONFIG_HW_GPU),y)
SRCS += $(IPU_HAL_SRC)
endif
else
ifeq ($(CONFIG_HW_CAMERA),y)
SRCS += $(SRC_DIR)/hal_ipu_weak_stub.c
endif
ifeq ($(CONFIG_HW_GPU),y)
SRCS += $(SRC_DIR)/hal_ipu_weak_stub.c
endif
endif

SRCS := $(sort $(SRCS))
OBJS := $(patsubst $(SRC_DIR)/%,$(BUILD_DIR)/%,$(SRCS:.c=.o))

INCLUDE := $(HARDWARE_INCLUDE)
INCLUDE += -I$(MODULE_ROOT)

.PHONY: all clean

all: $(OBJS)
	@echo "Build hardware/soc_platform completed!"

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDE) -c $< -o $@

$(BUILD_DIR)/hal/$(CONFIG_SOC_PLATFORM)/%.o: $(SRC_DIR)/hal/$(CONFIG_SOC_PLATFORM)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDE) -c $< -o $@

clean:
	@echo "Cleaning soc_platform build files..."
	$(RM) -rf $(BUILD_DIR)
	@find $(MODULE_ROOT) -name "*.o" -type f -delete 2>/dev/null || true
	@find $(MODULE_ROOT) -name "*.d" -type f -delete 2>/dev/null || true

