#
# Makefile for Display Module
#

include $(COMPILATION_RULES)

MODULE_ROOT ?= $(shell pwd)
MODULES_INCLUDE = $(MODULE_ROOT)/../include
BUILD_DIR ?= $(MODULE_ROOT)/build
SRC_DIR := $(MODULE_ROOT)

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(patsubst $(SRC_DIR)/%,$(BUILD_DIR)/%,$(SRCS:.c=.o))

INCLUDE := $(HARDWARE_INCLUDE)
INCLUDE += -I$(MODULE_ROOT)
INCLUDE += -I$(MODULE_ROOT)/../include
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/gui
INCLUDE += -I$(SYSTEM_CORE_INSTALL_DIR)/include

CFLAGS := $(HARDWARE_CFLAGS)

$(shell cp -f $(MODULE_ROOT)/../include/display.h $(MODULES_INCLUDE))

.PHONY: all clean

all: $(OBJS)
	@echo "Build hardware/display completed!"

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDE) -c $< -o $@

clean:
	@echo "Cleaning display build files..."
	$(RM) -rf $(BUILD_DIR)
	@find $(MODULE_ROOT) -name "*.o" -type f -delete 2>/dev/null || true
	@find $(MODULE_ROOT) -name "*.d" -type f -delete 2>/dev/null || true
