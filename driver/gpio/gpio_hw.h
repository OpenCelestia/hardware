// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/driver/gpio.
 *
 * GPIO driver module internal header.
 * - Defines internal constants and helper function declarations.
 */
#ifndef HARDWARE_GPIO_HW_H
#define HARDWARE_GPIO_HW_H

#include "gpio.h"
#include "driver_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * INTERNAL CONSTANTS
 * ============================================================================ */
#define GPIO_ARRAY_MAX_LENGTH       64
#define GPIO_FILE_PATH_MAX_LENGTH   128

#if CONFIG_OS_RTOS
#define GPIO_DEVICE_PATH            "/dev/gpio"
#define GPIO_INVALID_HANDLE         (-1)
#endif

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_GPIO_HW_H */

