// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/display.
 *
 * Display module public interface.
 * - Provides Android-style HAL interface for display devices (framebuffer, etc.).
 * - Supports resolution query, buffer management, and flush operations.
 */
#ifndef HARDWARE_DISPLAY_H
#define HARDWARE_DISPLAY_H

#include <stdint.h>
#include <sys/cdefs.h>

#include "hardware.h"
#include "system_pixel_format.h"
#include "system_error.h"
#include "display_color_swizzle.h"

__BEGIN_DECLS

/*===========================================================================
 * Module Version Definitions
 *===========================================================================*/

#define DISPLAY_MODULE_API_VERSION_0_1      HARDWARE_MODULE_API_VERSION(0, 1)
#define DISPLAY_DEVICE_FB_API_VERSION_1_0   HARDWARE_DEVICE_API_VERSION(1, 0)

/*===========================================================================
 * Module ID and Device Instance Definitions
 *===========================================================================*/

/** @brief Display hardware module ID */
#define DISPLAY_HARDWARE_MODULE_ID          "display"

/** @brief Framebuffer device instance name */
#define DISPLAY_DEVICE_INSTANCE_FB          "fb"

/** @brief Framebuffer device path */
#define DISPLAY_DEVICE_PATH_FB              "/dev/fb0"

/*===========================================================================
 * Type Definitions
 *===========================================================================*/

/**
 * @brief Display buffer structure
 * @details Contains virtual address, physical address and size of display buffer
 */
typedef struct display_buffer {
    void *vaddr;        /**< Virtual address of buffer */
    uint64_t paddr;     /**< Physical address of buffer */
    uint32_t size;      /**< Size of one offline frame in bytes */
    uint32_t total_size;/**< Total mmap size (1 or 2 frames) */
} display_buffer_t;

/*===========================================================================
 * Module and Device Structures
 *===========================================================================*/

/**
 * @brief Display module structure
 * @details Every hardware module must have a data structure named HAL_MODULE_INFO_SYM
 *          and the fields of this data structure must begin with hw_module_t
 *          followed by module specific information.
 */
typedef struct display_module {
    struct hw_module_t common;  /**< Common module fields */
} display_module_t;

/**
 * @brief Display device structure
 * @details Contains common device fields and display-specific methods
 */
typedef struct display_device {
    /**
     * Common methods of the device. This *must* be the first member of device
     * as users of this structure will cast a hw_device_t to display device
     * pointer in contexts where it's known the hw_device_t references a
     * display_device.
     */
    struct hw_device_t common;                      /**< Common device interface */

    void *priv;                                     /**< Private data pointer */

    /**
     * @brief Get the resolution of display device
     * @param device Device object
     * @param width Output parameter for width
     * @param height Output parameter for height
     * @return 0 on success, negative error code on failure
     */
    int (*get_resolution)(struct display_device *device,
                          uint32_t *width,
                          uint32_t *height);

    /**
     * @brief Get the offline buffer information of device
     * @param device Device object
     * @param buffer Output parameter for display buffer
     * @return 0 on success, negative error code on failure
     */
    int (*get_offline_buffer)(struct display_device *device,
                              display_buffer_t *buffer);

    /**
     * @brief Get pixel format of device
     * @param device Device object
     * @param format Output parameter for pixel format
     * @return 0 on success, negative error code on failure
     */
    int (*get_pixel_format)(struct display_device *device,
                            pixel_format_t *format);

    /**
     * @brief Get bits per pixel of device
     * @param device Device object
     * @param bpp Output parameter for bytes per pixel
     * @return 0 on success, negative error code on failure
     */
    int (*get_bpp)(struct display_device *device,
                   uint32_t *bpp);

    /**
     * @brief Flush display device
     * @param device Device object
     * @return 0 on success, negative error code on failure
     */
    int (*flush)(struct display_device *device);

} display_device_t;

/*===========================================================================
 * Module API Functions
 *===========================================================================*/

/**
 * @brief Get project-configured display color swizzle.
 */
display_color_swizzle_t display_get_color_swizzle(void);

/**
 * @brief Initialize display module
 * @return 0 on success, negative error code on failure
 */
int hw_display_module_init(void);

/**
 * @brief Get display module by instance name
 * @param module Output parameter for module pointer
 * @param inst Instance name (e.g., DISPLAY_DEVICE_INSTANCE_FB)
 * @return 0 on success, negative error code on failure
 */
int hw_display_get_module(const struct hw_module_t **module, const char *inst);

/*===========================================================================
 * Error Code Definitions
 *===========================================================================*/

/** @brief Success */
#define HARDWARE_DISPLAY_OK                        SYSTEM_SUCCESS

/** @brief Invalid parameter error */
#define HARDWARE_DISPLAY_ERROR_INVALID_PARAM       SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_DISPLAY, ERROR_INVALID_PARAM)

/** @brief Module not initialized error */
#define HARDWARE_DISPLAY_ERROR_NOT_INIT            SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_DISPLAY, ERROR_NOT_INIT)

/** @brief Memory allocation error */
#define HARDWARE_DISPLAY_ERROR_MEMORY              SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_DISPLAY, ERROR_MEMORY)

/** @brief I/O error */
#define HARDWARE_DISPLAY_ERROR_IO                  SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_DISPLAY, ERROR_IO)

/** @brief Device not found error */
#define HARDWARE_DISPLAY_ERROR_NOT_FOUND           SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_DISPLAY, ERROR_NOT_FOUND)

/** @brief Invalid state error */
#define HARDWARE_DISPLAY_ERROR_INVALID_STATE       SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_DISPLAY, ERROR_INVALID_STATE)

/** @brief Operation failed error */
#define HARDWARE_DISPLAY_ERROR_OPERATION_FAILED    SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_DISPLAY, ERROR_OPERATION_FAILED)

/** @brief Operation not supported error */
#define HARDWARE_DISPLAY_ERROR_NOT_SUPPORTED       SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_DISPLAY, ERROR_NOT_SUPPORTED)

/** @brief Device busy error */
#define HARDWARE_DISPLAY_ERROR_DEVICE_BUSY         SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_DISPLAY, 0x0017)

/** @brief Memory map failed error */
#define HARDWARE_DISPLAY_ERROR_MAP_FAILED          SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_DISPLAY, 0x0018)

/** @brief Flush failed error */
#define HARDWARE_DISPLAY_ERROR_FLUSH_FAILED        SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_DISPLAY, 0x0019)

__END_DECLS

#endif /* HARDWARE_DISPLAY_H */

