// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/driver/ir.
 *
 * IR (IRCUT & IRLED) driver module public interface.
 * - Provides Android-style HAL interface for IR operations.
 * - Supports IRCUT day/night switching and IRLED control.
 */
#ifndef HARDWARE_IR_H
#define HARDWARE_IR_H

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
#define IR_MODULE_API_VERSION_0_1       HARDWARE_MODULE_API_VERSION(0, 1)
#define IR_DEVICE_API_VERSION_1_0       HARDWARE_DEVICE_API_VERSION(1, 0)
#define IR_HARDWARE_MODULE_ID           "ir"

/* ============================================================================
 * IR TYPES
 * ============================================================================ */

/**
 * @brief IRLED initialization parameters.
 */
typedef struct {
    int working_level;      /**< Working level: 0=low active, 1=high active. */
} irled_param_t;

/* ============================================================================
 * IR MODULE STRUCTURE
 * ============================================================================ */

/**
 * @brief IR module structure.
 */
typedef struct ir_module {
    struct hw_module_t common;
} ir_module_t;

/* ============================================================================
 * IR DEVICE INTERFACE
 * ============================================================================ */

/**
 * @brief IR device structure.
 * @details Provides function pointers for IRCUT and IRLED operations.
 */
typedef struct ir_device {
    struct hw_device_t common;

    /* IRCUT interface */

    /**
     * @brief Initialize IRCUT.
     * @param device Device context.
     * @param cfg_path Configuration file path.
     * @return 0 on success, negative error code on failure.
     */
    int (*ircut_init)(struct ir_device *device, const char* cfg_path);

    /**
     * @brief Set IRCUT status.
     * @param device Device context.
     * @param status Status: 0=day(color), 1=night(B&W).
     * @return 0 on success, negative error code on failure.
     */
    int (*ircut_set)(struct ir_device *device, int status);

    /**
     * @brief Set IRCUT switch time.
     * @param device Device context.
     * @param time_ms Switch time in milliseconds.
     * @return 0 on success, negative error code on failure.
     */
    int (*ircut_set_time)(struct ir_device *device, int time_ms);

    /**
     * @brief Get IRCUT switch time.
     * @param device Device context.
     * @param time_ms Output parameter for switch time.
     * @return 0 on success, negative error code on failure.
     */
    int (*ircut_get_time)(struct ir_device *device, int* time_ms);

    /* IRLED interface */

    /**
     * @brief Initialize IRLED.
     * @param device Device context.
     * @param param Initialization parameters (can be NULL for defaults).
     * @return 0 on success, negative error code on failure.
     */
    int (*irled_init)(struct ir_device *device, const irled_param_t* param);

    /**
     * @brief Get IRLED status.
     * @param device Device context.
     * @param status Output parameter: 0=off, 1=on.
     * @return 0 on success, negative error code on failure.
     */
    int (*irled_get_status)(struct ir_device *device, int* status);

    /**
     * @brief Set IRLED status.
     * @param device Device context.
     * @param status Status: 0=off, 1=on.
     * @return 0 on success, negative error code on failure.
     */
    int (*irled_set_status)(struct ir_device *device, int status);
} ir_device_t;

/* ============================================================================
 * MODULE FUNCTIONS
 * ============================================================================ */

/**
 * @brief Initialize IR module.
 * @return 0 on success, negative error code on failure.
 */
int hw_ir_module_init(void);

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_IR_H */

