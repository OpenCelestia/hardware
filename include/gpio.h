// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/driver/gpio.
 *
 * GPIO driver module public interface.
 * - Provides Android-style HAL interface for GPIO operations.
 * - Supports direction, level, drive strength, pull, and slew rate control.
 */
#ifndef HARDWARE_GPIO_H
#define HARDWARE_GPIO_H

#include <stdint.h>
#include <sys/cdefs.h>

#include "hardware.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * MODULE IDENTIFICATION
 * ============================================================================ */
#define GPIO_MODULE_API_VERSION_0_1     HARDWARE_MODULE_API_VERSION(0, 1)
#define GPIO_DEVICE_API_VERSION_1_0     HARDWARE_DEVICE_API_VERSION(1, 0)
#define GPIO_HARDWARE_MODULE_ID         "gpio"

/* ============================================================================
 * GPIO MODULE STRUCTURE
 * ============================================================================ */

/**
 * @brief GPIO module structure.
 */
typedef struct gpio_module {
    struct hw_module_t common;
} gpio_module_t;

/* ============================================================================
 * GPIO DEVICE INTERFACE
 * ============================================================================ */

/**
 * @brief GPIO device structure.
 * @details Provides function pointers for GPIO operations including
 *          direction, level, drive strength, pull, and slew rate control.
 */
typedef struct gpio_device {
    struct hw_device_t common;

    /**
     * @brief Set GPIO direction.
     * @param device Device context.
     * @param name GPIO number as string.
     * @param dir Direction: 0=input, 1=output.
     * @return 0 on success, negative error code on failure.
     */
    int (*set_direction)(struct gpio_device *device, const char* name, uint32_t dir);

    /**
     * @brief Set GPIO output level.
     * @param device Device context.
     * @param name GPIO number as string.
     * @param level Level: 0=low, 1=high.
     * @return 0 on success, negative error code on failure.
     */
    int (*set_output_level)(struct gpio_device *device, const char* name, uint32_t level);

    /**
     * @brief Get GPIO input level.
     * @param device Device context.
     * @param name GPIO number as string.
     * @param level Output parameter for level value.
     * @return 0 on success, negative error code on failure.
     */
    int (*get_input_level)(struct gpio_device *device, const char* name, uint32_t *level);

    /**
     * @brief Set GPIO drive strength.
     * @param device Device context.
     * @param name GPIO number as string.
     * @param strength Drive strength: 0-3.
     * @return 0 on success, negative error code on failure.
     */
    int (*set_drive_strength)(struct gpio_device *device, const char* name, uint32_t strength);

    /**
     * @brief Enable or disable GPIO pull.
     * @param device Device context.
     * @param name GPIO number as string.
     * @param enable Enable: 0=disable, 1=enable.
     * @return 0 on success, negative error code on failure.
     */
    int (*enable_pull)(struct gpio_device *device, const char* name, uint32_t enable);

    /**
     * @brief Set GPIO pull polarity.
     * @param device Device context.
     * @param name GPIO number as string.
     * @param polarity Polarity: 0=pull-down, 1=pull-up.
     * @return 0 on success, negative error code on failure.
     */
    int (*set_pull_polarity)(struct gpio_device *device, const char* name, uint32_t polarity);

    /**
     * @brief Set GPIO slew rate.
     * @param device Device context.
     * @param name GPIO number as string.
     * @param rate Slew rate: 0=slow, 1=fast.
     * @return 0 on success, negative error code on failure.
     */
    int (*set_slew_rate)(struct gpio_device *device, const char* name, uint32_t rate);
} gpio_device_t;

/* ============================================================================
 * MODULE FUNCTIONS
 * ============================================================================ */

/**
 * @brief Initialize GPIO module.
 * @return 0 on success, negative error code on failure.
 */
int hw_gpio_module_init(void);

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_GPIO_H */

