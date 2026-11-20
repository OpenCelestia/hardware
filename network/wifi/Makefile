#
# Makefile for WiFi Module
#

include $(COMPILATION_RULES)
include $(PROJECT_CONFIG_FILE)

MODULE_ROOT ?= $(shell pwd)
BUILD_DIR ?= $(MODULE_ROOT)/build
SRC_DIR := $(MODULE_ROOT)

# CONFIG_WIFI_TYPE selects aic8800, wpa, or Aos uwlan.
ifndef CONFIG_WIFI_TYPE
PROJECT_DIR_WIFI := $(patsubst %/,%,$(dir $(PROJECT_CONFIG_FILE)))
BOARD_CONFIG_MK := $(PROJECT_DIR_WIFI)/system/$(CONFIG_SOC_PLATFORM)/$(CONFIG_SOC_SDK_VERSION)/board/$(CONFIG_BOARD)/config.mk
ifneq ($(wildcard $(BOARD_CONFIG_MK)),)
CONFIG_WIFI_TYPE := $(shell grep -E '^CONFIG_WIFI_TYPE[[:space:]]*[:?]?=' '$(BOARD_CONFIG_MK)' 2>/dev/null | tail -1 | cut -d= -f2 | tr -d ' \r\n')
endif
endif
CONFIG_WIFI_TYPE ?= NO_WIFI

WIFI_HAL_AIC_TYPES := aic8800 aic8800dl aic8800d40l
WPA_DIR := $(MODULE_ROOT)/wpa

ifeq ($(CONFIG_OS_NAME),aos)
WIFI_HAL_BACKEND_CFLAGS := -DWIFI_HAL_BACKEND_UWLAN -DWIFI_HAL_DEFAULT_IFACE=\"wlan0\"
WIFI_HAL_BACKEND_SRCS := $(MODULE_ROOT)/uwlan/wifi_backend_uwlan.c
# SDK root from this module (not HARDWARE_ROOT, which may be hardware/network).
SOC_SDK_ROOT := $(abspath $(MODULE_ROOT)/../../../system/$(CONFIG_SOC_PLATFORM)/$(CONFIG_SOC_SDK_VERSION))
UWLAN_SDK := $(SOC_SDK_ROOT)/apps/uwlan
UWLAN_SDK_DRIVER_INC := $(SOC_SDK_ROOT)/os/driver/include
UWLAN_SDK_AOS_EXT := $(SOC_SDK_ROOT)/platform/include/alios/external
UWLAN_SDK_CORE_INC := $(SOC_SDK_ROOT)/platform/include
UWLAN_SDK_OBJS := $(BUILD_DIR)/uwlan_mgnt.o $(BUILD_DIR)/uwlan_cmd.o $(BUILD_DIR)/uwlan_cfg.o
WIFI_HAL_WPA_SRCS :=
WIFI_HAL_WPA_INCLUDES := -I$(UWLAN_SDK) -I$(UWLAN_SDK_DRIVER_INC) -I$(UWLAN_SDK_AOS_EXT) -I$(UWLAN_SDK_CORE_INC)
WIFI_HAL_STALE_OBJS := $(BUILD_DIR)/wpa $(BUILD_DIR)/aic8800
$(info hardware/network/wifi: backend=uwlan CONFIG_OS_NAME=aos uwlan=$(UWLAN_SDK))
else ifeq ($(filter $(CONFIG_WIFI_TYPE),$(WIFI_HAL_AIC_TYPES)),$(CONFIG_WIFI_TYPE))
WIFI_HAL_BACKEND_CFLAGS := -DWIFI_HAL_BACKEND_AIC8800 -DWIFI_HAL_DEFAULT_IFACE=\"vnet0\"
WIFI_HAL_BACKEND_SRCS := \
	$(MODULE_ROOT)/aic8800/wifi_backend_aic8800.c
WIFI_HAL_WPA_SRCS :=
WIFI_HAL_WPA_INCLUDES :=
# Drop stale wpa backend objs (libhardware.so find-links all *.o under BUILD_DIR).
WIFI_HAL_STALE_OBJS := $(BUILD_DIR)/wpa
$(info hardware/network/wifi: backend=aic8800 CONFIG_WIFI_TYPE=$(CONFIG_WIFI_TYPE))
else
WIFI_HAL_BACKEND_CFLAGS := -DWIFI_HAL_BACKEND_WPA -DWIFI_HAL_DEFAULT_IFACE=\"wlan0\"
WIFI_HAL_BACKEND_SRCS := \
	$(WPA_DIR)/wifi_backend_wpa.c
WIFI_HAL_WPA_SRCS := \
	$(WPA_DIR)/wpa_ctrl/wpa_ctrl.c \
	$(WPA_DIR)/utils/wpabuf.c \
	$(WPA_DIR)/utils/wpa_debug.c \
	$(WPA_DIR)/utils/os_unix.c \
	$(WPA_DIR)/utils/trace.c
WIFI_HAL_WPA_INCLUDES := -I$(WPA_DIR)/wpa_ctrl -I$(WPA_DIR)/utils
# Drop stale aic8800 backend objs from previous product builds.
WIFI_HAL_STALE_OBJS := $(BUILD_DIR)/aic8800
$(info hardware/network/wifi: backend=wpa CONFIG_WIFI_TYPE=$(CONFIG_WIFI_TYPE))
endif

# Remove inactive backend objects before compile/link (CONFIG_WIFI_TYPE switch).
$(shell rm -rf $(WIFI_HAL_STALE_OBJS))

SRCS := $(SRC_DIR)/wifi_hw.c \
        $(WIFI_HAL_BACKEND_SRCS) \
        $(WIFI_HAL_WPA_SRCS)

NET_SRC := $(SRC_DIR)/../net/net.c
NET_OBJ := $(BUILD_DIR)/../net/net.o

OBJS := $(patsubst $(SRC_DIR)/%,$(BUILD_DIR)/%,$(SRCS:.c=.o))
ifeq ($(CONFIG_HW_ETHERNET), n)
OBJS += $(NET_OBJ)
endif
ifeq ($(CONFIG_OS_NAME),aos)
OBJS += $(UWLAN_SDK_OBJS)
endif

MODULE_INCLUDE := $(HARDWARE_INCLUDE)
MODULE_INCLUDE += -I$(MODULE_ROOT)
MODULE_INCLUDE += -I$(MODULE_ROOT)/../net
MODULE_INCLUDE += -I$(HARDWARE_ROOT)/include
MODULE_INCLUDE += -I$(SYSTEM_CORE_INSTALL_DIR)/include
MODULE_INCLUDE += $(WIFI_HAL_WPA_INCLUDES)

CFLAGS := $(HARDWARE_CFLAGS) $(WIFI_HAL_BACKEND_CFLAGS)
ifeq ($(CONFIG_OS_NAME),aos)
CFLAGS += $(WIFI_HAL_WPA_INCLUDES)
endif

.PHONY: all clean

all: $(OBJS)
	@echo "Build hardware/network/wifi completed!"

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(MODULE_INCLUDE) $(WIFI_HAL_WPA_INCLUDES) -c $< -o $@

ifeq ($(CONFIG_OS_NAME),aos)
$(BUILD_DIR)/uwlan_mgnt.o: $(UWLAN_SDK)/uwlan_mgnt.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(MODULE_INCLUDE) $(WIFI_HAL_WPA_INCLUDES) -c $< -o $@
$(BUILD_DIR)/uwlan_cmd.o: $(UWLAN_SDK)/uwlan_cmd.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(MODULE_INCLUDE) $(WIFI_HAL_WPA_INCLUDES) -c $< -o $@
$(BUILD_DIR)/uwlan_cfg.o: $(UWLAN_SDK)/uwlan_cfg.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(MODULE_INCLUDE) $(WIFI_HAL_WPA_INCLUDES) -c $< -o $@
endif

ifeq ($(CONFIG_HW_ETHERNET), n)
$(NET_OBJ): $(NET_SRC)
	@echo "Building network/net (required by WiFi)..."
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(MODULE_INCLUDE) -c $< -o $@
endif

clean:
	@echo "Cleaning network/wifi build files..."
	$(RM) -rf $(BUILD_DIR)
	@find $(MODULE_ROOT) -name "*.o" -type f -delete 2>/dev/null || true
	@$(RM) -f $(BUILD_DIR)/../net/net.o 2>/dev/null || true
