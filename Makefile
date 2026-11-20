#
# Makefile for Hardware Abstraction Layer (HAL)
#

include $(COMPILATION_RULES)
include $(PROJECT_CONFIG_FILE)
HARDWARE_SIZE_OPT := $(if $(strip $(CONFIG_HARDWARE_SIZE_OPT)),$(CONFIG_HARDWARE_SIZE_OPT),y)
HARDWARE_FORCE_MERGE_STATIC_LIBS := $(CONFIG_HARDWARE_FORCE_MERGE_STATIC_LIBS)

# ============================================================
# Build Configuration
# ============================================================
INSTALL_DIR = $(HARDWARE_INSTALL_DIR)
PROJ = hardware
MODULE_ROOT ?= $(shell pwd)
SRC_HARDWARE_DIR ?= $(MODULE_ROOT)
BUILD_HARDWARE_DIR ?= $(MODULE_ROOT)/build
BUILD_DIR ?= $(BUILD_HARDWARE_DIR)

# Library output
STATIC_LIB := lib$(PROJ).a
STATIC_HARDWARE_LIB := $(BUILD_DIR)/$(STATIC_LIB)
DYNAMIC_LIB := lib$(PROJ).so
DYNAMIC_HARDWARE_LIB := $(BUILD_DIR)/$(DYNAMIC_LIB)
HARDWARE_LIB := $(STATIC_HARDWARE_LIB) $(DYNAMIC_HARDWARE_LIB)

# Source files
HARDWARE_C_SRC := $(SRC_HARDWARE_DIR)/hardware.c
SOURCES := hardware.c
OBJS_HARDWARE = $(addprefix $(BUILD_DIR)/, $(SOURCES:.c=.o))

# ============================================================
# Module Selection (controlled by config.mk)
# ============================================================
obj-y =

# Core
obj-y += soc_platform/

# Multimedia
obj-$(CONFIG_HW_AUDIO) += audio/
obj-$(CONFIG_HW_CAMERA) += camera/
obj-$(CONFIG_HW_DISPLAY) += display/
obj-$(CONFIG_HW_GPU) += gpu/
obj-$(CONFIG_HW_OSD) += osd/
obj-$(CONFIG_HW_VISION_ISP) += vision/isp/
obj-$(CONFIG_HW_VISION_MOTION_DETECT) += vision/motion_detect/

# Network
obj-$(CONFIG_HW_WIFI) += network/wifi/
obj-$(CONFIG_HW_ETHERNET) += network/ethernet/

# Input (enabled if keyboard or touch is enabled)
obj-$(CONFIG_HW_INPUT_KEYBOARD) += input/evdev/
obj-$(CONFIG_HW_INPUT_TOUCH) += input/evdev/

# Drivers
obj-$(CONFIG_HW_DRIVER_GPIO) += driver/gpio/
obj-$(CONFIG_HW_DRIVER_ADC) += driver/adc/
obj-$(CONFIG_HW_DRIVER_PWM) += driver/pwm/
obj-$(CONFIG_HW_DRIVER_MOTOR) += driver/motor/
obj-$(CONFIG_HW_DRIVER_LED) += driver/led/
obj-$(CONFIG_HW_DRIVER_IR) += driver/ir/
obj-$(CONFIG_HW_DRIVER_WDT) += driver/wdt/
obj-$(CONFIG_HW_DRIVER_EFUSE) += driver/efuse/
obj-$(CONFIG_HW_DRIVER_BLKDEV) += driver/blkdev/
obj-$(CONFIG_HW_DRIVER_MMC) += driver/blkdev/

MODULES_DIR := $(obj-y)
TESTS_DIR := tests

# ============================================================
# Include Paths
# ============================================================
INCLUDE += -I$(MODULE_ROOT)/include
INCLUDE += -I$(MODULE_ROOT)/../framework/ui/common/include
INCLUDE += -I$(MODULE_ROOT)/network
INCLUDE += -I$(MODULE_ROOT)/network/wifi
INCLUDE += -I$(MODULE_ROOT)/network/ethernet
INCLUDE += -I$(SYSTEM_CORE_INSTALL_DIR)/include

# Platform include: install -> build -> source
PLATFORM_INCLUDE_DIR :=
ifneq ($(SOC_PLATFORM_INSTALL_DIR),)
    INSTALL_PLATFORM_INCLUDE := $(SOC_PLATFORM_INSTALL_DIR)/platform/include
    ifeq ($(shell test -d $(INSTALL_PLATFORM_INCLUDE) && echo yes),yes)
        PLATFORM_INCLUDE_DIR := $(INSTALL_PLATFORM_INCLUDE)
    endif
endif
ifeq ($(PLATFORM_INCLUDE_DIR),)
    ifneq ($(BUILD_DIR),)
        ifneq ($(REL_SOC_PLATFORM_DIR),)
            BUILD_PLATFORM_BASE := $(shell echo $(BUILD_DIR) | sed 's|/hardware.*||' | sed 's|/application.*||' | sed 's|/framework.*||')
            BUILD_PLATFORM_INCLUDE := $(BUILD_PLATFORM_BASE)/$(REL_SOC_PLATFORM_DIR)/platform/include
            ifeq ($(shell test -d $(BUILD_PLATFORM_INCLUDE) && echo yes),yes)
                PLATFORM_INCLUDE_DIR := $(BUILD_PLATFORM_INCLUDE)
            endif
        endif
    endif
endif
ifeq ($(PLATFORM_INCLUDE_DIR),)
    ifneq ($(SOC_PLATFORM_DIR),)
        PLATFORM_INCLUDE_DIR := $(SOC_PLATFORM_DIR)/platform/include
    endif
endif
ifneq ($(PLATFORM_INCLUDE_DIR),)
    INCLUDE += -I$(PLATFORM_INCLUDE_DIR)
ifneq ($(wildcard $(abspath $(PLATFORM_INCLUDE_DIR)/../include_3rd)),)
    INCLUDE += -I$(abspath $(PLATFORM_INCLUDE_DIR)/../include_3rd)
endif
else
    $(warning No platform include directory found)
endif

# Platform library: install -> source
PLATFORM_LIB_DIR :=
ifneq ($(SOC_PLATFORM_INSTALL_DIR),)
    INSTALL_PLATFORM_LIB := $(SOC_PLATFORM_INSTALL_DIR)/lib
    ifeq ($(shell test -d $(INSTALL_PLATFORM_LIB) && echo yes),yes)
        PLATFORM_LIB_DIR := $(INSTALL_PLATFORM_LIB)
    endif
endif
ifeq ($(PLATFORM_LIB_DIR),)
    ifneq ($(SOC_PLATFORM_DIR),)
        SRC_PLATFORM_LIB := $(SOC_PLATFORM_DIR)/platform/lib
        ifeq ($(shell test -d $(SRC_PLATFORM_LIB) && echo yes),yes)
            PLATFORM_LIB_DIR := $(SRC_PLATFORM_LIB)
        endif
    endif
endif
ifneq ($(PLATFORM_LIB_DIR),)
    LDFLAGS ?=
    LDFLAGS += -L$(PLATFORM_LIB_DIR)
    PLATFORM_LIBS := $(shell find $(PLATFORM_LIB_DIR) -maxdepth 1 -name "*.a" 2>/dev/null)
    ifneq ($(PLATFORM_LIBS),)
        # Some vendor archives are built with TLS relocations that cannot be
        # folded into libhardware.so. Keep those as shared-library deps instead
        # of forcing them into the HAL .so at link time.
        PLATFORM_TLS_UNSAFE_STATIC_LIBS := $(PLATFORM_LIB_DIR)/libakaudiofilter.a
        # Detect with absolute path; link via -L + -l:basename so DT_NEEDED is
        # "libakaudiofilter.so" (not the build-host path). Same pattern as
        # application/app_main/Makefile APP_MAIN_AKAUDIOFILTER_*.
        PLATFORM_AKAUDIOFILTER_FILE := $(firstword $(wildcard \
                $(PLATFORM_LIB_DIR)/libakaudiofilter.so \
                $(PLATFORM_LIB_DIR)/libakaudiofilter.so.*))
        PLATFORM_LIBS := $(filter-out $(PLATFORM_TLS_UNSAFE_STATIC_LIBS),$(PLATFORM_LIBS))
        # Wrap SDK static libs in --start-group/--end-group so ld rescans them to
        # resolve cross-library dependencies (e.g. libakaudiocodec.a <-> libplat_osal.a
        # <-> libakaudiofilter.a: audio codec refs osal, osal refs codec). Without
        # grouping, ld's single left-to-right pass leaves symbols UND in libhardware.so
        # -> runtime "Error relocating: symbol not found" on target.
        LDFLAGS += -Wl,--start-group $(PLATFORM_LIBS) -Wl,--end-group
        ifneq ($(strip $(PLATFORM_AKAUDIOFILTER_FILE)),)
            LDFLAGS += -Wl,--no-as-needed -Wl,-l:$(notdir $(PLATFORM_AKAUDIOFILTER_FILE)) -Wl,--as-needed
            # AV100 V2 only: hal_audio_echo_lock.c is not built for AV200.
            ifeq ($(CONFIG_SOC_PLATFORM),AnyCloud39AV100_V2)
            LDFLAGS += -Wl,--wrap=_SD_Echo_FillFarStream
            LDFLAGS += -Wl,--wrap=_SD_Echo_GetDacStream
            LDFLAGS += -Wl,--wrap=_SD_Echo_FillAdcStream
            LDFLAGS += -Wl,--wrap=_SD_Echo_FillDacLoopback
            LDFLAGS += -Wl,--wrap=_SD_Echo_GetResult
            LDFLAGS += -Wl,--wrap=_SD_Filter_Control
            endif
        endif
    endif
endif

# ============================================================
# OS-Specific Configuration
# ============================================================
ifeq ($(CONFIG_OS_NAME), aos)
CFLAGS += -DCONFIG_OS_RTOS=1
ifeq ($(CONFIG_SOC_PLATFORM),Sky39AV130)
CFLAGS += -D__CHIP_AK3918AV130_SERIES
endif
ifneq ($(SOC_PLATFORM_DIR),)
INCLUDE += -I$(SOC_PLATFORM_DIR)/os/driver/include
INCLUDE += -I$(SOC_PLATFORM_DIR)/platform/dep_lib/include
endif
ifneq ($(PLATFORM_INCLUDE_DIR),)
INCLUDE += -I$(PLATFORM_INCLUDE_DIR)/alios/internal/
INCLUDE += -I$(PLATFORM_INCLUDE_DIR)/alios/external/
INCLUDE += -I$(PLATFORM_INCLUDE_DIR)/alios/external/aos
INCLUDE += -I$(PLATFORM_INCLUDE_DIR)/alios/external/linux
endif
else ifeq ($(CONFIG_OS_NAME), linux)
CFLAGS += -DOS_NAME_LINUX=1
CFLAGS += -DCONFIG_OS_LINUX=1
OS_DRIVER_INCLUDE :=
ifneq ($(SOC_PLATFORM_INSTALL_DIR),)
    INSTALL_OS_DRIVER_INCLUDE := $(SOC_PLATFORM_INSTALL_DIR)/os/driver/external/include
    ifeq ($(shell test -d $(INSTALL_OS_DRIVER_INCLUDE) && echo yes),yes)
        OS_DRIVER_INCLUDE := $(INSTALL_OS_DRIVER_INCLUDE)
    endif
endif
ifeq ($(OS_DRIVER_INCLUDE),)
    ifneq ($(SOC_PLATFORM_DIR),)
        SRC_OS_DRIVER_INCLUDE := $(SOC_PLATFORM_DIR)/os/driver/external/include
        ifeq ($(shell test -d $(SRC_OS_DRIVER_INCLUDE) && echo yes),yes)
            OS_DRIVER_INCLUDE := $(SRC_OS_DRIVER_INCLUDE)
        else ifneq ($(BUILD_DIR),)
            ifneq ($(REL_SOC_PLATFORM_DIR),)
                BUILD_PLATFORM_BASE := $(shell echo $(BUILD_DIR) | sed 's|/hardware.*||' | sed 's|/application.*||' | sed 's|/framework.*||')
                BUILD_OS_DRIVER_INCLUDE := $(BUILD_PLATFORM_BASE)/$(REL_SOC_PLATFORM_DIR)/os/driver/external/include
                ifeq ($(shell test -d $(BUILD_OS_DRIVER_INCLUDE) && echo yes),yes)
                    OS_DRIVER_INCLUDE := $(BUILD_OS_DRIVER_INCLUDE)
                endif
            endif
        endif
    endif
endif
ifneq ($(OS_DRIVER_INCLUDE),)
    INCLUDE += -I$(OS_DRIVER_INCLUDE)
endif
endif

# ============================================================
# Platform and SDK Flags
# ============================================================
CFLAGS += -D$(CONFIG_SOC_PLATFORM)
DF_SDK_VERSION := $(subst .,_,$(CONFIG_SOC_SDK_VERSION))
CFLAGS += -D$(DF_SDK_VERSION)
# Auto-generated config.h now lives under out/<product>/build/project (PROJECT_GEN_INCLUDE_DIR,
# exported by build.sh). Fall back to the source project dir only for standalone make.
PROJECT_GEN_INCLUDE_DIR ?= $(PROJECT_DIR)/include
ifneq ($(wildcard $(PROJECT_GEN_INCLUDE_DIR)/config.h),)
CFLAGS += -include $(PROJECT_GEN_INCLUDE_DIR)/config.h
endif
ifeq ($(HARDWARE_SIZE_OPT), y)
CFLAGS += -ffunction-sections -fdata-sections
endif
INCLUDE += -I$(PROJECT_DIR)/include
INCLUDE += -I$(PROJECT_GEN_INCLUDE_DIR)

THIRD_PARTY_INSTALL_DIR ?= $(shell printf '%s' "$$THIRD_PARTY_INSTALL_DIR")

# third_party install: build.sh sets THIRD_PARTY_INSTALL_DIR; direct `make -C hardware` often omits it.
# layout: $(BUILD_DIR) typically .../out/build/hardware -> guess .../out/install/third_party
THIRD_PARTY_INSTALL_DIR_GUESS := $(abspath $(BUILD_DIR)/../../install/third_party)
THIRD_PARTY_RESOLVE := $(if $(strip $(THIRD_PARTY_INSTALL_DIR)),$(strip $(THIRD_PARTY_INSTALL_DIR)),$(THIRD_PARTY_INSTALL_DIR_GUESS))
ICONV_INST_INC := $(THIRD_PARTY_RESOLVE)/iconv/include
ICONV_INST_LIB := $(THIRD_PARTY_RESOLVE)/iconv/lib
ICONV_INST_LIB_FALLBACK := $(abspath $(MODULE_ROOT)/../../out/install/third_party/iconv/lib)
ifneq ($(wildcard $(ICONV_INST_INC)/iconv.h),)
INCLUDE += -I$(ICONV_INST_INC)
endif

# hal_osd -> libiconv_* : resolve libiconv at Make-parse time (explicit path; avoids empty shell find).
ICONV_SO_LIST := $(wildcard $(ICONV_INST_LIB)/libiconv.so $(ICONV_INST_LIB)/libiconv.so.*) $(wildcard $(ICONV_INST_LIB_FALLBACK)/libiconv.so $(ICONV_INST_LIB_FALLBACK)/libiconv.so.*)
ICONV_SO_FILE := $(firstword $(ICONV_SO_LIST))
HARDWARE_ICONV_LDFLAGS :=
ifeq ($(filter y,$(strip $(CONFIG_THIRD_PARTY_ICONV))),y)
ifneq ($(strip $(ICONV_SO_FILE)),)
HARDWARE_ICONV_LDFLAGS := -Wl,-rpath,/usr/lib -Wl,--no-as-needed $(ICONV_SO_FILE) -Wl,--as-needed
endif
endif

# hal input/touch (touch.c) uses tslib (ts_open/ts_read/ts_config/ts_close).
# Link libts.a statically into libhardware.so so the ts_* symbols are internalized
# (no runtime NEEDED on libts.so; tslib plugins still load via dlopen at runtime).
TSLIB_INST_LIB := $(THIRD_PARTY_RESOLVE)/tslib/lib
TSLIB_STATIC := $(wildcard $(TSLIB_INST_LIB)/libts.a)
HARDWARE_TSLIB_LDFLAGS :=
ifeq ($(filter y,$(strip $(CONFIG_THIRD_PARTY_TSLIB))),y)
ifneq ($(strip $(TSLIB_STATIC)),)
HARDWARE_TSLIB_LDFLAGS := -Wl,--start-group $(TSLIB_STATIC) -Wl,--end-group
endif
endif

# Build-time diagnostics (prints once per invocation).
ifeq ($(filter y,$(strip $(CONFIG_THIRD_PARTY_ICONV))),y)
$(warning iconv: THIRD_PARTY_INSTALL_DIR=$(THIRD_PARTY_INSTALL_DIR))
$(warning iconv: THIRD_PARTY_RESOLVE=$(THIRD_PARTY_RESOLVE))
$(warning iconv: ICONV_INST_LIB=$(ICONV_INST_LIB))
$(warning iconv: ICONV_INST_LIB_FALLBACK=$(ICONV_INST_LIB_FALLBACK))
$(warning iconv: ICONV_SO_FILE=$(ICONV_SO_FILE))
$(warning iconv: HARDWARE_ICONV_LDFLAGS=$(HARDWARE_ICONV_LDFLAGS))
endif
ifeq ($(filter y,$(strip $(CONFIG_HW_OSD))),y)
ifeq ($(filter y,$(strip $(CONFIG_THIRD_PARTY_ICONV))),y)
ifeq ($(strip $(ICONV_SO_FILE)),)
$(error OSD needs libiconv.so under $(ICONV_INST_LIB) or $(ICONV_INST_LIB_FALLBACK). Build third_party/iconv or set THIRD_PARTY_INSTALL_DIR.)
endif
endif
endif

# ============================================================
# Input Module Config Flags
# ============================================================
CFLAGS += $(if $(filter y,$(CONFIG_HW_INPUT_KEYBOARD) $(CONFIG_HW_INPUT_TOUCH)),-DCONFIG_HW_INPUT=1,)
CFLAGS += $(if $(filter y,$(CONFIG_HW_INPUT_KEYBOARD)),-DCONFIG_HW_INPUT_KEYBOARD=1,)
CFLAGS += $(if $(filter y,$(CONFIG_HW_INPUT_TOUCH)),-DCONFIG_HW_INPUT_TOUCH=1,)

$(shell mkdir -p $(BUILD_DIR))

# ============================================================
# Build Targets
# ============================================================
.PHONY: all clean install tests

all: install tests 

# ============================================================
# System Core Dependency
# ============================================================
SRC_DIR ?= $(shell dirname $(MODULE_ROOT))
SYSTEM_CORE_BUILD_DIR ?= $(shell echo $(BUILD_DIR) | sed 's|/hardware.*||')/system/core

system_core_dep:
	@if [ ! -f "$(SYSTEM_CORE_INSTALL_DIR)/lib/libsystem_core.a" ]; then \
		echo "Building system/core..."; \
		SYSTEM_CORE_BUILD_PATH="$(SYSTEM_CORE_BUILD_DIR)"; \
		if [ -z "$$SYSTEM_CORE_BUILD_PATH" ]; then \
			SYSTEM_CORE_BUILD_PATH="$$(echo $(BUILD_DIR) | sed 's|/hardware.*||')/system/core"; \
		fi; \
		SRC_ROOT="$$(dirname $(MODULE_ROOT))"; \
		$(MAKE) -C $$SRC_ROOT/system/core install \
			SYSTEM_CORE_INSTALL_DIR=$(SYSTEM_CORE_INSTALL_DIR) \
			BUILD_DIR=$$SYSTEM_CORE_BUILD_PATH \
			COMPILATION_RULES=$(COMPILATION_RULES) \
			PROJECT_CONFIG_FILE=$(PROJECT_CONFIG_FILE); \
	fi

# ============================================================
# Library Building
# ============================================================
$(DYNAMIC_HARDWARE_LIB): build_modules $(OBJS_HARDWARE)
ifeq ($(CONFIG_OS_NAME), linux)
	@mkdir -p $(BUILD_DIR); \
	MODULES_OBJS=$$(find $(BUILD_DIR) -type f -name "*.o" 2>/dev/null | grep -v "/hardware\.o$$" || true); \
	HW_MERGE_LIBS=""; \
	for libn in $(HARDWARE_FORCE_MERGE_STATIC_LIBS); do \
		libp="$$(find "$(THIRD_PARTY_RESOLVE)" -maxdepth 4 -type f -name "$$libn" 2>/dev/null | head -1)"; \
		if [ -n "$$libp" ]; then \
			HW_MERGE_LIBS="$$HW_MERGE_LIBS -Wl,--whole-archive $$libp -Wl,--no-whole-archive"; \
		fi; \
	done; \
	echo "Link libhardware.so with iconv flags: $(HARDWARE_ICONV_LDFLAGS)"; \
	$(CC) -shared -o $@ $$MODULES_OBJS $(OBJS_HARDWARE) $$HW_MERGE_LIBS $(HARDWARE_ICONV_LDFLAGS) $(HARDWARE_TSLIB_LDFLAGS) $(LDFLAGS)
endif

$(STATIC_HARDWARE_LIB): build_modules $(OBJS_HARDWARE) system_core_dep
	@mkdir -p $(BUILD_DIR)
	@MODULES_OBJS=$$(find $(BUILD_DIR) -type f -name "*.o" 2>/dev/null | grep -v "/hardware\.o$$" | grep -v "^$(BUILD_DIR)/hardware\.o$$" || true); \
	if [ -z "$$MODULES_OBJS" ]; then \
		echo "WARNING: No module object files found in $(BUILD_DIR)"; \
	fi; \
	rm -f $@; \
	$(AR) -rsv $@ $$MODULES_OBJS $(OBJS_HARDWARE)

# ============================================================
# Object File Compilation
# ============================================================
# hardware.c force-includes config.h (see -include above) and gates every hw_*_module_init()
# call behind #if CONFIG_HW_*. Without this dependency, a regenerated config.h (e.g. after a
# config.mk edit) does NOT retrigger hardware.o, so stale module registrations ship in
# libhardware — e.g. camera HAL never registers -> hw_get_module("camera") returns -1.
_HWRW_CFG_H := $(wildcard $(PROJECT_GEN_INCLUDE_DIR)/config.h)
$(BUILD_DIR)/hardware.o: $(HARDWARE_C_SRC) $(_HWRW_CFG_H)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDE) -c $< -o $@

# ============================================================
# Module Building
# ============================================================
build_modules:
	@SRC_PATH="$$(cd $(SRC_HARDWARE_DIR) 2>/dev/null && pwd || echo $(SRC_HARDWARE_DIR))"; \
	BUILD_PATH="$$(cd $(BUILD_DIR) 2>/dev/null && pwd || echo $(BUILD_DIR))"; \
	echo "Building modules: SRC=$$SRC_PATH, BUILD=$$BUILD_PATH"; \
	mkdir -p $(BUILD_DIR);
	@for dir in $(obj-y); do \
		dir_clean=$$(echo $$dir | sed 's|/*$$||'); \
		if [ -d "$(SRC_HARDWARE_DIR)/$$dir_clean" ]; then \
			echo "Building module: $$dir_clean"; \
			$(MAKE) -C $(SRC_HARDWARE_DIR)/$$dir_clean \
				HARDWARE_CFLAGS="$(CFLAGS)" \
				HARDWARE_INCLUDE="$(INCLUDE)" \
				HARDWARE_INSTALL_DIR=$(HARDWARE_INSTALL_DIR) \
				HARDWARE_ROOT=$(SRC_HARDWARE_DIR) \
				BUILD_DIR=$(BUILD_DIR)/$$dir_clean \
				SYSTEM_CORE_INSTALL_DIR=$(SYSTEM_CORE_INSTALL_DIR) \
				COMPILATION_RULES=$(COMPILATION_RULES) \
				PROJECT_CONFIG_FILE=$(PROJECT_CONFIG_FILE); \
			if [ $$? -ne 0 ]; then \
				echo "ERROR: Failed to build module $$dir_clean"; \
				exit 1; \
			fi; \
		else \
			echo "WARNING: Module directory not found: $(SRC_HARDWARE_DIR)/$$dir_clean"; \
		fi; \
	done

# ============================================================
# Test Programs
# ============================================================
tests: $(STATIC_HARDWARE_LIB) $(DYNAMIC_HARDWARE_LIB)
ifneq ($(filter y,$(CONFIG_HW_TEST)),)
	@if [ -d "$(SRC_HARDWARE_DIR)/tests" ]; then \
		echo "***************Building hardware/tests...***************"; \
		echo "  install dir: $(HARDWARE_INSTALL_DIR)/tests/"; \
		$(MAKE) -C $(SRC_HARDWARE_DIR)/tests all \
			BUILD_DIR="$(BUILD_DIR)/tests" \
			HARDWARE_INCLUDE="$(INCLUDE)" \
			HARDWARE_CFLAGS="$(CFLAGS)" \
			HARDWARE_ROOT="$(SRC_HARDWARE_DIR)" \
			COMPILATION_RULES="$(COMPILATION_RULES)" \
			PROJECT_CONFIG_FILE="$(PROJECT_CONFIG_FILE)" \
			SOC_PLATFORM_INSTALL_DIR="$(SOC_PLATFORM_INSTALL_DIR)" \
			SOC_PLATFORM_DIR="$(SOC_PLATFORM_DIR)" \
			THIRD_PARTY_INSTALL_DIR="$(THIRD_PARTY_INSTALL_DIR)" \
			HARDWARE_INSTALL_DIR="$(HARDWARE_INSTALL_DIR)" \
			SYSTEM_CORE_INSTALL_DIR="$(SYSTEM_CORE_INSTALL_DIR)" || exit 1; \
	else \
		echo "WARNING: hardware/tests directory not found"; \
	fi
else
	@echo "hardware/tests skipped (CONFIG_HW_TEST is not y in $(PROJECT_CONFIG_FILE))"
endif

# ============================================================
# Installation
# ============================================================
install: $(STATIC_HARDWARE_LIB) $(DYNAMIC_HARDWARE_LIB)
	@mkdir -p $(INSTALL_DIR)/lib
	@cp -f $(DYNAMIC_HARDWARE_LIB) $(INSTALL_DIR)/lib 2>/dev/null || true
	@cp -f $(STATIC_HARDWARE_LIB) $(INSTALL_DIR)/lib
	@rm -rf $(INSTALL_DIR)/include
	@mkdir -p $(INSTALL_DIR)/include
	@cp -rf $(MODULE_ROOT)/include/*.h $(INSTALL_DIR)/include/ 2>/dev/null || true
	@cp -f $(MODULE_ROOT)/../framework/ui/common/include/display_color_swizzle.h $(INSTALL_DIR)/include/ 2>/dev/null || true
	@if [ -d "$(SUB_MODULES_INCLUDE)" ]; then \
		cp -rf $(SUB_MODULES_INCLUDE)/*.h $(INSTALL_DIR)/include/ 2>/dev/null || true; \
	fi

# ============================================================
# Clean
# ============================================================
clean:
	@echo "Cleaning hardware build files..."
	@rm -rf $(BUILD_DIR)
	@rm -rf $(BUILD_HARDWARE_DIR)
	@find $(MODULE_ROOT) -name "*.o" -type f -delete 2>/dev/null || true
	@find $(MODULE_ROOT) -name "*.d" -type f -delete 2>/dev/null || true
	@find $(MODULE_ROOT) -name "*.a" -type f -delete 2>/dev/null || true
	@find $(MODULE_ROOT) -name "*.so" -type f -delete 2>/dev/null || true
