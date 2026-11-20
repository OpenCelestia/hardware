#
# Makefile for IR Driver (IRCUT & IRLED)
#

include $(COMPILATION_RULES)

MODULE_ROOT ?= $(shell pwd)
MODULES_INCLUDE = $(HARDWARE_ROOT)/include
BUILD_DIR ?= $(MODULE_ROOT)/build
SRC_DIR := $(MODULE_ROOT)

SRCS := $(SRC_DIR)/ir.c \
        $(wildcard $(SRC_DIR)/hal/$(CONFIG_SOC_PLATFORM)/*.c)

OBJS := $(patsubst $(SRC_DIR)/%,$(BUILD_DIR)/%,$(SRCS:.c=.o))

# ir_hal.c calls read_file()/get_conf_str()/get_conf_int() as external symbols. The SDK
# ships these only as globals in platform/sample/ir/ak_conf.c (sample code, not in any
# platform .a); system_config.h provides them only as static-inline (not exported). Compile
# ak_conf.c here so libhardware.a carries the symbols ir_hal.o needs — pulled on demand by
# archive semantics wherever libhardware.a is linked (tests + production).
AK_CONF_SRC := $(wildcard $(SOC_PLATFORM_DIR)/platform/sample/ir/ak_conf.c)
ifneq ($(strip $(AK_CONF_SRC)),)
OBJS += $(BUILD_DIR)/ak_conf.o
endif

MODULE_INCLUDE := $(HARDWARE_INCLUDE)
MODULE_INCLUDE += -I$(MODULE_ROOT)
MODULE_INCLUDE += -I$(HARDWARE_ROOT)/include
MODULE_INCLUDE += -I$(SYSTEM_CORE_INSTALL_DIR)/include

HAL_INCLUDE := $(HARDWARE_INCLUDE)
HAL_INCLUDE += -I$(HARDWARE_ROOT)/include
HAL_INCLUDE += -I$(MODULE_ROOT)
HAL_INCLUDE += -I$(MODULE_ROOT)/hal/$(CONFIG_SOC_PLATFORM)
HAL_INCLUDE += -I$(SYSTEM_ROOT)/$(CONFIG_SOC_PLATFORM)/include

CFLAGS := $(HARDWARE_CFLAGS:-Werror=-Wall)

.PHONY: all clean

all: $(OBJS)
	@echo "Build hardware/driver/ir completed!"

$(BUILD_DIR)/ir.o: $(SRC_DIR)/ir.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(MODULE_INCLUDE) -I$(SRC_DIR)/hal/$(CONFIG_SOC_PLATFORM) -c $< -o $@

$(BUILD_DIR)/hal/$(CONFIG_SOC_PLATFORM)/%.o: $(SRC_DIR)/hal/$(CONFIG_SOC_PLATFORM)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(HAL_INCLUDE) -c $< -o $@

# ak_conf.c lives under the SDK sample tree; ak_conf.h sits next to it (quoted include) and
# ak_common.h is on the SDK platform include (-I below + HARDWARE_INCLUDE).
$(BUILD_DIR)/ak_conf.o: $(AK_CONF_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(HAL_INCLUDE) -I$(SOC_PLATFORM_DIR)/platform/include -c $< -o $@

clean:
	$(RM) $(OBJS)
