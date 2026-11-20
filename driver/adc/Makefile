#
# Makefile for ADC Driver
#

include $(COMPILATION_RULES)

MODULE_ROOT ?= $(shell pwd)
MODULES_INCLUDE = $(HARDWARE_ROOT)/include
BUILD_DIR ?= $(MODULE_ROOT)/build
SRC_DIR := $(MODULE_ROOT)

SRCS := $(SRC_DIR)/adc.c
OBJS := $(patsubst $(SRC_DIR)/%,$(BUILD_DIR)/%,$(SRCS:.c=.o))

MODULE_INCLUDE := $(HARDWARE_INCLUDE)
MODULE_INCLUDE += -I$(MODULE_ROOT)
MODULE_INCLUDE += -I$(HARDWARE_ROOT)/include
MODULE_INCLUDE += -I$(SYSTEM_CORE_INSTALL_DIR)/include

CFLAGS := $(HARDWARE_CFLAGS)

.PHONY: all clean

all: $(OBJS)
	@echo "Build hardware/driver/adc completed!"

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(MODULE_INCLUDE) -c $< -o $@

clean:
	$(RM) -rf $(BUILD_DIR)
	@find $(MODULE_ROOT) -name "*.o" -type f -delete 2>/dev/null || true
