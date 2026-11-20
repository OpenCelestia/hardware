// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/driver/blkdev.
 *
 * MMC/SD card driver module public interface.
 * - Provides Android-style HAL interface for MMC operations.
 * - Supports mount, unmount, format, and fsck operations.
 */
#ifndef HARDWARE_MMC_H
#define HARDWARE_MMC_H

#include <stdint.h>
#include <sys/cdefs.h>
#if CONFIG_OS_LINUX
#include <linux/kernel.h>
#endif

#include "hardware.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * MODULE IDENTIFICATION
 * ============================================================================ */
#define MMC_MODULE_API_VERSION_0_1      HARDWARE_MODULE_API_VERSION(0, 1)
#define MMC_DEVICE_API_VERSION_1_0      HARDWARE_DEVICE_API_VERSION(1, 0)
#define MMC_HARDWARE_MODULE_ID          "mmc"

/* ============================================================================
 * MMC CONSTANTS
 * ============================================================================ */
#define MMC_DEVICE_NAME_BLK0            "/dev/mmcblk0"
#define MMC_DEVICE_NAME_BLK0P1          "/dev/mmcblk0p1"
#define MMC_DEVICE_NAME_BLK1            "/dev/mmcblk1"
#define MMC_DEVICE_NAME_BLK1P1          "/dev/mmcblk1p1"
#define MMC_DEVICE_MOUNT_FS             "vfat"
#define MMC_MOUNT_PATH_DEFAULT          "/tmp/sd"

/* ============================================================================
 * MMC MODULE STRUCTURE
 * ============================================================================ */

/**
 * @brief MMC module structure.
 */
typedef struct mmc_module {
    struct hw_module_t common;
} mmc_module_t;

/* ============================================================================
 * MMC DEVICE INTERFACE
 * ============================================================================ */

/**
 * @brief MMC device structure.
 * @details Provides function pointers for MMC operations.
 */
typedef struct mmc_device {
    struct hw_device_t common;

    /**
     * @brief Mount MMC device.
     * @param device Device context.
     * @param name Device name.
     * @param path Mount path.
     * @param fs File system type (vfat/exfat).
     * @return 0 on success, negative error code on failure.
     */
    int (*mount)(struct mmc_device *device, const char *name, char *path, char *fs);

    /**
     * @brief Unmount MMC device.
     * @param device Device context.
     * @param path Mount path.
     * @return 0 on success, negative error code on failure.
     */
    int (*unmount)(struct mmc_device *device, char *path);

    /**
     * @brief Format MMC device.
     * @param device Device context.
     * @param name Device name.
     * @param fs File system type.
     * @return 0 on success, negative error code on failure.
     */
    int (*format)(struct mmc_device *device, const char *name, char *fs);

    /**
     * @brief File system check.
     * @param device Device context.
     * @param name Device name.
     * @param fs File system type.
     * @return 0 on success, negative error code on failure.
     */
    int (*fsck)(struct mmc_device *device, const char *name, char *fs);
} mmc_device_t;

/* ============================================================================
 * MODULE FUNCTIONS
 * ============================================================================ */

/**
 * @brief Initialize MMC module.
 * @return 0 on success, negative error code on failure.
 */
int hw_mmc_module_init(void);

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_MMC_H */


