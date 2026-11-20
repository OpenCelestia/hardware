// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/driver/motor.
 *
 * Motor driver module public interface.
 * - Provides Android-style HAL interface for stepper motor control.
 * - Supports step/angle control, limit setting, and status query.
 */
#ifndef HARDWARE_MOTOR_H
#define HARDWARE_MOTOR_H

#include <stdint.h>
#include <sys/cdefs.h>
#include <stdbool.h>

#include "hardware.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * MODULE IDENTIFICATION
 * ============================================================================ */
#define MOTOR_MODULE_API_VERSION_0_1    HARDWARE_MODULE_API_VERSION(0, 1)
#define MOTOR_DEVICE_API_VERSION_1_0    HARDWARE_DEVICE_API_VERSION(1, 0)
#define MOTOR_HARDWARE_MODULE_ID        "motor"
#define MOTOR_DEVICE_NAME_MOTOR0        "motor0"
#define MOTOR_DEVICE_NAME_MOTOR1        "motor1"

/* ============================================================================
 * MOTOR TYPES
 * ============================================================================ */

/**
 * @brief Motor status enumeration.
 */
typedef enum motor_status_t {
    eMOTOR_IS_STOP = 0,         /**< Motor is stopped. */
    eMOTOR_IS_RUNNING,          /**< Motor is running. */
} motor_status_t;

/**
 * @brief Motor configuration structure.
 */
typedef struct motor_config {
    int pos;                    /**< Current position. */
    int speed_step;             /**< Speed in steps per second (Hz). */
    int steps_one_circle;       /**< Steps per full rotation. */
    int total_steps;            /**< Total steps motor can run. */
    int boundary_steps;         /**< Reserved boundary steps. */
} motor_config_t;

/**
 * @brief Motor information structure.
 */
typedef struct motor_info {
    motor_status_t status;      /**< Motor working status. */
    int pos;                    /**< Current position. */
    int speed_step;             /**< Speed in steps per second (Hz). */
    int speed_angle;            /**< Speed in angle per second. */
    int steps_one_circle;       /**< Steps per full rotation. */
    int total_steps;            /**< Total steps motor can run. */
    int boundary_steps;         /**< Reserved boundary steps. */
    int attach_timer;           /**< Attached to hardware timer. */
} motor_info_t;

/* ============================================================================
 * MOTOR MODULE STRUCTURE
 * ============================================================================ */

/**
 * @brief Motor module structure.
 */
typedef struct motor_module {
    struct hw_module_t common;
} motor_module_t;

/* ============================================================================
 * MOTOR DEVICE INTERFACE
 * ============================================================================ */

/**
 * @brief Motor device structure.
 * @details Provides function pointers for motor operations.
 */
typedef struct motor_device {
    struct hw_device_t common;

    /**
     * @brief Configure motor device.
     * @param device Device context.
     * @param name Device name.
     * @param config Motor configuration.
     * @return 0 on success, negative error code on failure.
     */
    int (*config)(struct motor_device *device, const char* name, struct motor_config *config);

    /**
     * @brief Set step value.
     * @param device Device context.
     * @param name Device name.
     * @param value Step value.
     * @return 0 on success, negative error code on failure.
     */
    int (*set_step)(struct motor_device *device, const char* name, int value);

    /**
     * @brief Set angle value.
     * @param device Device context.
     * @param name Device name.
     * @param value Angle value.
     * @return 0 on success, negative error code on failure.
     */
    int (*set_angle)(struct motor_device *device, const char* name, int value);

    /**
     * @brief Set limit.
     * @param device Device context.
     * @param name Device name.
     * @param enable Enable limit: true=enable, false=disable.
     * @param value Limit value: >0=clockwise, <0=anticlockwise.
     * @return 0 on success, negative error code on failure.
     */
    int (*set_limit)(struct motor_device *device, const char* name, bool enable, int value);

    /**
     * @brief Stop motor and wait for completion.
     * @param device Device context.
     * @param name Device name.
     * @return 0 on success, negative error code on failure.
     */
    int (*stop)(struct motor_device *device, const char* name);

    /**
     * @brief Get motor information.
     * @param device Device context.
     * @param name Device name.
     * @param info Output motor information.
     * @return 0 on success, negative error code on failure.
     */
    int (*get_info)(struct motor_device *device, const char* name, struct motor_info *info);
} motor_device_t;

/* ============================================================================
 * MODULE FUNCTIONS
 * ============================================================================ */

/**
 * @brief Initialize motor module.
 * @return 0 on success, negative error code on failure.
 */
int hw_motor_module_init(void);

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_MOTOR_H */

