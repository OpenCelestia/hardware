// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/driver/efuse.
 *
 * eFuse driver module public interface.
 * - Provides Android-style HAL interface for eFuse operations.
 * - Supports reading and writing chip ID.
 */
#ifndef HARDWARE_EFUSE_H
#define HARDWARE_EFUSE_H

#include <stdint.h>
#include <sys/cdefs.h>

#include "hardware.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * MODULE IDENTIFICATION
 * ============================================================================ */
#define EFUSE_MODULE_API_VERSION_0_1    HARDWARE_MODULE_API_VERSION(0, 1)
#define EFUSE_DEVICE_API_VERSION_1_0    HARDWARE_DEVICE_API_VERSION(1, 0)
#define EFUSE_HARDWARE_MODULE_ID        "efuse"

/* ============================================================================
 * EFUSE MODULE STRUCTURE
 * ============================================================================ */

/**
 * @brief eFuse module structure.
 */
typedef struct efuse_module {
    struct hw_module_t common;
} efuse_module_t;

/* ============================================================================
 * EFUSE DEVICE INTERFACE
 * ============================================================================ */

/**
 * @brief eFuse device structure.
 * @details Provides function pointers for eFuse operations.
 */
typedef struct efuse_device {
    struct hw_device_t common;

    /**
     * @brief Get ID length from eFuse.
     * @param device Device context.
     * @param name Device name.
     * @param option Option: "global" or "customer".
     * @param length Output parameter for ID length.
     * @return 0 on success, negative error code on failure.
     */
    int (*get_length)(struct efuse_device *device, const char *name, const char *option, uint32_t *length);

    /**
     * @brief Get ID from eFuse.
     * @param device Device context.
     * @param name Device name.
     * @param option Option: "global" or "customer".
     * @param buffer Buffer to store the ID.
     * @return 0 on success, negative error code on failure.
     */
    int (*get_id)(struct efuse_device *device, const char *name, const char *option, char *buffer);

    /**
     * @brief Set ID to eFuse.
     * @param device Device context.
     * @param name Device name.
     * @param option Option: only "customer" supported.
     * @param buffer Customer ID buffer (hex).
     * @return 0 on success, negative error code on failure.
     */
    int (*set_id)(struct efuse_device *device, const char *name, const char *option, char *buffer);
} efuse_device_t;

/* ============================================================================
 * MODULE FUNCTIONS
 * ============================================================================ */

/**
 * @brief Initialize eFuse module.
 * @return 0 on success, negative error code on failure.
 */
int hw_efuse_module_init(void);

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_EFUSE_H */

