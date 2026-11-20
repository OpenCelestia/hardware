// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware.
 *
 * Hardware module aggregation header.
 * - Provides a single include point for all enabled hardware modules.
 * - Module inclusion is controlled by CONFIG_HW_* macros.
 */
#ifndef HARDWARE_HEADER_H
#define HARDWARE_HEADER_H


#if CONFIG_HW_AUDIO
#include "audio.h"
#endif

#if CONFIG_HW_CAMERA
#include "camera.h"
#endif

#if CONFIG_HW_DISPLAY
#include "display.h"
#endif

#if CONFIG_HW_OSD
#include "osd.h"
#endif

#if CONFIG_HW_GPU
#include "gpu.h"
#endif

#if CONFIG_HW_DRIVER_MMC
#include "mmc.h"
#endif

#if CONFIG_HW_DRIVER_ADC
#include "adc.h"
#endif

#if CONFIG_HW_DRIVER_GPIO
#include "gpio.h"
#endif

#if CONFIG_HW_DRIVER_PWM
#include "pwm.h"
#endif

#if CONFIG_HW_DRIVER_WDT
#include "wdt.h"
#endif


#if CONFIG_HW_DRIVER_MOTOR
#include "motor.h"
#endif


#if CONFIG_HW_DRIVER_EFUSE
#include "efuse.h"
#endif

#if CONFIG_HW_INPUT
#include "input_evdev.h"
#endif

#if CONFIG_HW_DRIVER_LED
#include "led.h"
#endif

#if CONFIG_HW_DRIVER_IR
#include "ir.h"
#endif

#if CONFIG_HW_WIFI
#include "wifi.h"
#endif

#if CONFIG_HW_ETHERNET
#include "ethernet.h"
#endif

#if CONFIG_HW_VISION_ISP
#include "isp.h"
#endif

#if CONFIG_HW_VISION_MOTION_DETECT
#include "motion_detect.h"
#endif

#endif /* HARDWARE_HEADER_H */
