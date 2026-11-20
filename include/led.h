// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/driver/led.
 *
 * LED driver module public interface.
 * - Provides Android-style HAL interface for LED operations.
 * - Supports state and brightness control.
 */
#ifndef HARDWARE_LED_H
#define HARDWARE_LED_H

#include <stdint.h>
#include <sys/cdefs.h>

#include "hardware.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * MODULE IDENTIFICATION
 * ============================================================================ */
#define LED_MODULE_API_VERSION_0_1      HARDWARE_MODULE_API_VERSION(0, 1)
#define LED_DEVICE_API_VERSION_1_0      HARDWARE_DEVICE_API_VERSION(1, 0)
#define LED_HARDWARE_MODULE_ID          "led"

/* ============================================================================
 * LED MODULE STRUCTURE
 * ============================================================================ */

/**
 * @brief LED module structure.
 */
typedef struct led_module {
    struct hw_module_t common;
} led_module_t;

/* ============================================================================
 * LED DEVICE INTERFACE
 * ============================================================================ */

/**
 * @brief LED device structure.
 * @details Provides function pointers for LED operations.
 */
typedef struct led_device {
    struct hw_device_t common;

    /**
     * @brief Set LED state.
     * @param device Device context.
     * @param name LED name.
     * @param state State: 0=off, 1=on.
     * @return 0 on success, negative error code on failure.
     */
    int (*set_state)(struct led_device *device, const char *name, uint32_t state);

    /**
     * @brief Get LED state.
     * @param device Device context.
     * @param name LED name.
     * @param state Output parameter for state value.
     * @return 0 on success, negative error code on failure.
     */
    int (*get_state)(struct led_device *device, const char *name, uint32_t *state);

    /**
     * @brief Set LED brightness.
     * @param device Device context.
     * @param name LED name.
     * @param brightness Brightness value.
     * @return 0 on success, negative error code on failure.
     */
    int (*set_brightness)(struct led_device *device, const char *name, uint32_t brightness);

    /**
     * @brief Get LED brightness.
     * @param device Device context.
     * @param name LED name.
     * @param brightness Output parameter for brightness value.
     * @return 0 on success, negative error code on failure.
     */
    int (*get_brightness)(struct led_device *device, const char *name, uint32_t *brightness);
} led_device_t;

/* ============================================================================
 * MODULE FUNCTIONS
 * ============================================================================ */

/**
 * @brief Initialize LED module.
 * @return 0 on success, negative error code on failure.
 */
int hw_led_module_init(void);

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_LED_H */

