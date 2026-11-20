// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/driver/pwm.
 *
 * PWM driver module public interface.
 * - Provides Android-style HAL interface for PWM operations.
 * - Supports PWM configuration and enable/disable control.
 */
#ifndef HARDWARE_PWM_H
#define HARDWARE_PWM_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/cdefs.h>

#include "hardware.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * MODULE IDENTIFICATION
 * ============================================================================ */
#define PWM_MODULE_API_VERSION_0_1      HARDWARE_MODULE_API_VERSION(0, 1)
#define PWM_DEVICE_API_VERSION_1_0      HARDWARE_DEVICE_API_VERSION(1, 0)
#define PWM_HARDWARE_MODULE_ID          "pwm"

/* ============================================================================
 * PWM TYPES
 * ============================================================================ */

/**
 * @brief PWM configuration structure.
 */
typedef struct pwm_info {
    uint32_t period;    /**< Signal period in nanoseconds. */
    uint32_t duty;      /**< Signal duty cycle in nanoseconds. */
} pwm_info_t;

/* ============================================================================
 * PWM MODULE STRUCTURE
 * ============================================================================ */

/**
 * @brief PWM module structure.
 */
typedef struct pwm_module {
    struct hw_module_t common;
} pwm_module_t;

/* ============================================================================
 * PWM DEVICE INTERFACE
 * ============================================================================ */

/**
 * @brief PWM device structure.
 * @details Provides function pointers for PWM operations.
 */
typedef struct pwm_device {
    struct hw_device_t common;

    /**
     * @brief Configure PWM channel.
     * @param device Device context.
     * @param name PWM channel index as string.
     * @param info PWM configuration (period and duty).
     * @return 0 on success, negative error code on failure.
     */
    int (*config)(struct pwm_device *device, const char *name, struct pwm_info *info);

    /**
     * @brief Enable or disable PWM channel.
     * @param device Device context.
     * @param name PWM channel index as string.
     * @param enable Enable: 1=enable, 0=disable.
     * @return 0 on success, negative error code on failure.
     */
    int (*enable)(struct pwm_device *device, const char *name, uint32_t enable);
} pwm_device_t;

/* ============================================================================
 * MODULE FUNCTIONS
 * ============================================================================ */

/**
 * @brief Initialize PWM module.
 * @return 0 on success, negative error code on failure.
 */
int hw_pwm_module_init(void);

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_PWM_H */

