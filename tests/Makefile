#
# Makefile for Hardware Test Programs
#

include $(COMPILATION_RULES)
include $(PROJECT_CONFIG_FILE)

# ============================================================
# Directory Configuration
# ============================================================
MODULE_ROOT ?= $(shell pwd)
SRC_DIR := $(MODULE_ROOT)
# Object files under out/build/.../tests; binaries under out/install/hardware/tests/
OBJ_DIR := $(BUILD_DIR)
BIN_DIR := $(HARDWARE_INSTALL_DIR)/tests
NFS_DIR := ~/nfs/hardware
ROOTFS_DIR ?=
ROOTFS_TESTS_DIR ?= $(ROOTFS_DIR)/utils/usr/tests
# Board validation binaries: deploy to /usr/tests only; never package under /usr/sbin.
ROOTFS_SBIN_EXCLUDE := hal_wifi_test hal_display_test hal_evdev_test hal_mmc_test

ifndef HARDWARE_INSTALL_DIR
$(error HARDWARE_INSTALL_DIR is not defined)
endif

# ============================================================
# Library Paths
# ============================================================
# SDK platform libs (libplat_mem.a, libplat_common.a, ...) are kept in the
# source platform/lib dir and are NOT installed into the out/ tree. Pick the
# install dir if it exists, otherwise fall back to source (same logic as the
# hardware Makefile). Without this, tests link no platform libs and SDK
# symbols referenced by libhardware.a (e.g. AK_BASE_Mem* from hal_osd_ipu)
# go undefined at link time.
PLATFORM_LIB_DIR :=
ifneq ($(SOC_PLATFORM_INSTALL_DIR),)
    ifeq ($(shell test -d $(SOC_PLATFORM_INSTALL_DIR)/lib && echo yes),yes)
        PLATFORM_LIB_DIR := $(SOC_PLATFORM_INSTALL_DIR)/lib
    endif
endif
ifeq ($(PLATFORM_LIB_DIR),)
    ifneq ($(SOC_PLATFORM_DIR),)
        ifeq ($(shell test -d $(SOC_PLATFORM_DIR)/platform/lib && echo yes),yes)
            PLATFORM_LIB_DIR := $(SOC_PLATFORM_DIR)/platform/lib
        endif
    endif
endif
INIPARSER_LIB_DIR := $(THIRD_PARTY_INSTALL_DIR)/iniparser/lib
HARDWARE_LIB_DIR := $(HARDWARE_INSTALL_DIR)/lib
SYSTEM_CORE_LIB_DIR := $(SYSTEM_CORE_INSTALL_DIR)/lib
TS_LIB_DIR := $(THIRD_PARTY_INSTALL_DIR)/tslib/lib
ICONV_LIB_DIR := $(THIRD_PARTY_INSTALL_DIR)/iconv/lib
ICONV_LIB_DIR_FALLBACK := $(abspath $(MODULE_ROOT)/../../out/install/third_party/iconv/lib)

LIBS := $(shell find $(PLATFORM_LIB_DIR) -maxdepth 1 -name *.a 2>/dev/null)
LIBS += $(shell find $(INIPARSER_LIB_DIR) -maxdepth 1 -name *.a 2>/dev/null)
LIBS += $(shell find $(HARDWARE_LIB_DIR) -maxdepth 1 -name *.a 2>/dev/null)
LIBS += $(shell find $(SYSTEM_CORE_LIB_DIR) -maxdepth 1 -name *.a 2>/dev/null)
LIBS += $(shell find $(TS_LIB_DIR) -maxdepth 1 -name *.a 2>/dev/null)

# NOTE: hardware static libs may pull in hal_osd.o which depends on GNU libiconv (libiconv_* symbols).
# Link iconv AFTER the archive group so unresolved symbols are resolved.
ICONV_SO := $(firstword $(wildcard $(ICONV_LIB_DIR)/libiconv.so $(ICONV_LIB_DIR)/libiconv.so.*) $(wildcard $(ICONV_LIB_DIR_FALLBACK)/libiconv.so $(ICONV_LIB_DIR_FALLBACK)/libiconv.so.*))
ICONV_A := $(firstword $(wildcard $(ICONV_LIB_DIR)/libiconv.a) $(wildcard $(ICONV_LIB_DIR_FALLBACK)/libiconv.a))
ICONV_LDFLAGS :=
ifeq ($(filter y,$(strip $(CONFIG_THIRD_PARTY_ICONV))),y)
ifneq ($(strip $(ICONV_SO)),)
ICONV_LDFLAGS := -Wl,-rpath,/usr/lib -Wl,--no-as-needed $(ICONV_SO) -Wl,--as-needed
else ifneq ($(strip $(ICONV_A)),)
ICONV_LDFLAGS := $(ICONV_A)
else
$(warning hardware/tests: libiconv not found under $(ICONV_LIB_DIR); hal_*_test link may fail if CONFIG_HW_OSD=y)
endif
endif

LDFLAGS = -Xlinker "-(" $(LIBS) -Xlinker "-)" $(ICONV_LDFLAGS) -lc

# ============================================================
# Compiler Flags
# ============================================================
CFLAGS := $(HARDWARE_CFLAGS)
CFLAGS += $(if $(filter y,$(CONFIG_HW_DRIVER_ADC)),-DCONFIG_HW_ADC=1,)
CFLAGS += $(if $(filter y,$(CONFIG_HW_DRIVER_MMC) $(CONFIG_HW_DRIVER_BLKDEV)),-DCONFIG_HW_MMC=1,)
CFLAGS += $(if $(filter y,$(CONFIG_HW_INPUT_KEYBOARD) $(CONFIG_HW_INPUT_TOUCH)),-DCONFIG_HW_INPUT=1,)
CFLAGS += $(if $(filter y,$(CONFIG_HW_INPUT_KEYBOARD)),-DCONFIG_HW_INPUT_KEYBOARD=1,)
CFLAGS += $(if $(filter y,$(CONFIG_HW_INPUT_TOUCH)),-DCONFIG_HW_INPUT_TOUCH=1,)
CFLAGS += $(if $(filter y,$(CONFIG_HW_DRIVER_EFUSE)),-DCONFIG_HW_DRIVER_UUID=1,)

# ============================================================
# Include Paths
# ============================================================
INCLUDE := $(HARDWARE_INCLUDE)
INCLUDE += -I$(MODULE_ROOT)
INCLUDE += -I$(THIRD_PARTY_INSTALL_DIR)/iniparser/include

# Explicit test SoC flags (also auto-detected via -D$(CONFIG_SOC_PLATFORM) in HARDWARE_CFLAGS)
ifeq ($(CONFIG_SOC_PLATFORM),AnyCloud39AV100_V2)
CFLAGS += -DCAMERA_TEST_SOC_AV100_V2=1
else ifeq ($(CONFIG_SOC_PLATFORM),AnyCloud39AV200)
CFLAGS += -DCAMERA_TEST_SOC_AV200=1
endif

# ============================================================
# Source Files (conditional on module config.mk, hal_*_test.c naming)
# ============================================================
SOURCES := $(if $(filter y,$(CONFIG_HW_SOC_PLATFORM)),hal_soc_platform_test.c,)
SOURCES += $(if $(filter y,$(CONFIG_HW_AUDIO)),hal_audio_test.c,)
SOURCES += $(if $(filter y,$(CONFIG_HW_CAMERA)),hal_camera_test.c,)
ifeq ($(CONFIG_HW_CAMERA),y)
CAMERA_TEST_PROFILE_OBJ := $(OBJ_DIR)/hal_camera_test_profile.o
else
CAMERA_TEST_PROFILE_OBJ :=
endif
SOURCES += $(if $(filter y,$(CONFIG_HW_DISPLAY)),hal_display_test.c,)
SOURCES += $(if $(filter y,$(CONFIG_HW_GPU)),hal_gpu_test.c,)
SOURCES += $(if $(filter y,$(CONFIG_HW_VISION_ISP)),hal_isp_test.c,)
SOURCES += $(if $(filter y,$(CONFIG_HW_VISION_MOTION_DETECT)),hal_motion_detect_test.c,)
SOURCES += $(if $(filter y,$(CONFIG_HW_DRIVER_IR)),hal_ir_test.c,)
SOURCES += $(if $(filter y,$(CONFIG_HW_WIFI)),hal_wifi_test.c,)
SOURCES += $(if $(filter y,$(CONFIG_HW_ETHERNET)),hal_ethernet_test.c,)
SOURCES += $(if $(filter y,$(CONFIG_HW_INPUT_KEYBOARD) $(CONFIG_HW_INPUT_TOUCH)),hal_evdev_test.c,)
SOURCES += $(if $(filter y,$(CONFIG_HW_DRIVER_GPIO)),hal_gpio_test.c,)
SOURCES += $(if $(filter y,$(CONFIG_HW_DRIVER_ADC)),hal_adc_test.c,)
SOURCES += $(if $(filter y,$(CONFIG_HW_DRIVER_PWM)),hal_pwm_test.c,)
SOURCES += $(if $(filter y,$(CONFIG_HW_DRIVER_MOTOR)),hal_motor_test.c,)
SOURCES += $(if $(filter y,$(CONFIG_HW_DRIVER_WDT)),hal_wdt_test.c,)
SOURCES += $(if $(filter y,$(CONFIG_HW_DRIVER_LED)),hal_led_test.c,)
SOURCES += $(if $(filter y,$(CONFIG_HW_DRIVER_MMC) $(CONFIG_HW_DRIVER_BLKDEV)),hal_mmc_test.c,)
SOURCES += $(if $(filter y,$(CONFIG_HW_DRIVER_EFUSE)),hal_efuse_test.c,)

OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(SOURCES))
OBJS += $(CAMERA_TEST_PROFILE_OBJ)
TARGET := $(patsubst %.c,$(BIN_DIR)/%,$(SOURCES))

# ============================================================
# Build Targets
# ============================================================
.PHONY: all copy_nfs install_rootfs clean

all: $(TARGET) copy_nfs
	@echo "OBJ_DIR: $(OBJ_DIR)"
	@echo "BIN_DIR: $(BIN_DIR)"
	@echo "CONFIG_HW_AUDIO=$(CONFIG_HW_AUDIO) CONFIG_HW_TEST=$(CONFIG_HW_TEST)"
	@echo "SOURCES: $(SOURCES)"
	@echo "TARGET: $(TARGET)"
	@if [ -z "$(strip $(TARGET))" ]; then \
		echo "ERROR: no hardware test binaries were built (check CONFIG_HW_TEST / CONFIG_HW_AUDIO in $(PROJECT_CONFIG_FILE))"; \
		exit 1; \
	fi
	@echo "Build hardware/tests completed!"

# Compile .c to .o
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDE) -c $< -o $@

# hal_camera_test links profile object (not a standalone binary)
$(BIN_DIR)/hal_camera_test: $(OBJ_DIR)/hal_camera_test.o $(CAMERA_TEST_PROFILE_OBJ)
	@mkdir -p $(BIN_DIR)
	@echo "Linking $@ ..."
	$(CC) -o $@ $^ $(LDFLAGS)
	$(STRIP) $@

# Link .o to executable (binary name matches source basename, e.g. hal_gpio_test)
$(BIN_DIR)/%: $(OBJ_DIR)/%.o
	@mkdir -p $(BIN_DIR)
	@echo "Linking $@ ..."
	$(CC) -o $@ $< $(LDFLAGS)
	$(STRIP) $@

copy_nfs: $(TARGET)
	@mkdir -p $(NFS_DIR)
	@if [ -n "$(TARGET)" ]; then \
		for bin in $(TARGET); do \
			if [ -f "$$bin" ]; then \
				cp -f $$bin $(NFS_DIR)/; \
				echo "Copied $$bin to $(NFS_DIR)/"; \
			fi; \
		done; \
	fi

# Stage hal_*_test under rootfs/utils/usr/tests (→ /usr/tests after process_utils).
# ROOTFS_SBIN_EXCLUDE must not appear under usr/sbin (NFS/dev deploy only for those).
install_rootfs:
	@if [ -n "$(ROOTFS_DIR)" ] && [ -d "$(ROOTFS_DIR)" ] && [ -d "$(BIN_DIR)" ]; then \
		mkdir -p "$(ROOTFS_TESTS_DIR)"; \
		for bin in $(BIN_DIR)/hal_*_test; do \
			[ -f "$$bin" ] || continue; \
			cp -f "$$bin" "$(ROOTFS_TESTS_DIR)/"; \
		done; \
		for skip in $(ROOTFS_SBIN_EXCLUDE); do \
			rm -f "$(ROOTFS_DIR)/utils/usr/sbin/$$skip" "$(ROOTFS_DIR)/usr/sbin/$$skip" 2>/dev/null || true; \
		done; \
		echo "Installed hardware tests to $(ROOTFS_TESTS_DIR) (not in usr/sbin: $(ROOTFS_SBIN_EXCLUDE))"; \
	fi

clean:
	@echo "Cleaning hardware tests..."
	$(RM) -rf $(OBJ_DIR)
	$(RM) -rf $(HARDWARE_INSTALL_DIR)/tests
