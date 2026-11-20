#
# Makefile for Block Device Driver (MMC/SD)
#

include $(COMPILATION_RULES)

MODULE_ROOT ?= $(shell pwd)
MODULES_INCLUDE = $(HARDWARE_ROOT)/include
BUILD_DIR ?= $(MODULE_ROOT)/build
SRC_DIR := $(MODULE_ROOT)

SRCS := $(SRC_DIR)/mmc.c
OBJS := $(patsubst $(SRC_DIR)/%,$(BUILD_DIR)/%,$(SRCS:.c=.o))

MODULE_INCLUDE := $(HARDWARE_INCLUDE)
MODULE_INCLUDE += -I$(MODULE_ROOT)
MODULE_INCLUDE += -I$(HARDWARE_ROOT)/include
MODULE_INCLUDE += -I$(SYSTEM_CORE_INSTALL_DIR)/include

CFLAGS := $(HARDWARE_CFLAGS:-Werror=-Wall)

.PHONY: all clean

all: $(OBJS)
	@echo "Build hardware/driver/blkdev completed!"

$(BUILD_DIR)/mmc.o: $(SRC_DIR)/mmc.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(MODULE_INCLUDE) -c $< -o $@

clean:
	$(RM) $(OBJS)
