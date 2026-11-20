// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/driver/wdt.
 *
 * Watchdog timer driver module public interface.
 * - Provides Android-style HAL interface for watchdog operations.
 * - Supports start, feed, and stop operations.
 */
#ifndef HARDWARE_WDT_H
#define HARDWARE_WDT_H

#include <stdint.h>
#include <sys/cdefs.h>
#include <sys/time.h>

#include "hardware.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * MODULE IDENTIFICATION
 * ============================================================================ */
#define WDT_MODULE_API_VERSION_0_1      HARDWARE_MODULE_API_VERSION(0, 1)
#define WDT_DEVICE_API_VERSION_1_0      HARDWARE_DEVICE_API_VERSION(1, 0)
#define WDT_HARDWARE_MODULE_ID          "wdt"

/* ============================================================================
 * WDT MODULE STRUCTURE
 * ============================================================================ */

/**
 * @brief Watchdog timer module structure.
 */
typedef struct wdt_module {
    struct hw_module_t common;
} wdt_module_t;

/* ============================================================================
 * WDT DEVICE INTERFACE
 * ============================================================================ */

/**
 * @brief Watchdog timer device structure.
 * @details Provides function pointers for watchdog operations.
 */
typedef struct wdt_device {
    struct hw_device_t common;

    /**
     * @brief Start watchdog timer.
     * @param device Device context.
     * @param name Device name.
     * @param timeout Timeout value (default 4s).
     * @return 0 on success, negative error code on failure.
     */
    int (*start)(struct wdt_device *device, const char* name, struct timeval *timeout);

    /**
     * @brief Feed watchdog.
     * @param device Device context.
     * @param name Device name.
     * @return 0 on success, negative error code on failure.
     */
    int (*feed)(struct wdt_device *device, const char* name);

    /**
     * @brief Stop watchdog timer.
     * @param device Device context.
     * @param name Device name.
     * @return 0 on success, negative error code on failure.
     */
    int (*stop)(struct wdt_device *device, const char* name);
} wdt_device_t;

/* ============================================================================
 * MODULE FUNCTIONS
 * ============================================================================ */

/**
 * @brief Initialize watchdog timer module.
 * @return 0 on success, negative error code on failure.
 */
int hw_wdt_module_init(void);

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_WDT_H */

