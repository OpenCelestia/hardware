#
# Makefile for Ethernet Module
#

include $(COMPILATION_RULES)
include $(PROJECT_CONFIG_FILE)

MODULE_ROOT ?= $(shell pwd)
MODULES_INCLUDE = $(HARDWARE_ROOT)/include
BUILD_DIR ?= $(MODULE_ROOT)/build
SRC_DIR := $(MODULE_ROOT)

SRCS := $(SRC_DIR)/ethernet_hw.c

# Net module: Ethernet compiles net.c (avoids duplicate if both WiFi and Ethernet enabled)
NET_SRC := $(SRC_DIR)/../net/net.c
NET_OBJ := $(BUILD_DIR)/../net/net.o

OBJS := $(patsubst $(SRC_DIR)/%,$(BUILD_DIR)/%,$(SRCS:.c=.o))
OBJS += $(NET_OBJ)

MODULE_INCLUDE := $(HARDWARE_INCLUDE)
MODULE_INCLUDE += -I$(MODULE_ROOT)
MODULE_INCLUDE += -I$(MODULE_ROOT)/../net
MODULE_INCLUDE += -I$(MODULE_ROOT)/../include
MODULE_INCLUDE += -I$(HARDWARE_ROOT)/include
MODULE_INCLUDE += -I$(SYSTEM_CORE_INSTALL_DIR)/include

CFLAGS := $(HARDWARE_CFLAGS)

.PHONY: all clean

all: $(OBJS)
	@echo "Build hardware/network/ethernet completed!"

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(MODULE_INCLUDE) -c $< -o $@

$(NET_OBJ): $(NET_SRC)
	@echo "Building network/net (required by Ethernet)..."
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(MODULE_INCLUDE) -c $< -o $@

clean:
	@echo "Cleaning network/ethernet build files..."
	$(RM) -rf $(BUILD_DIR)
	@find $(MODULE_ROOT) -name "*.o" -type f -delete 2>/dev/null || true
	@$(RM) -f $(BUILD_DIR)/../net/net.o 2>/dev/null || true