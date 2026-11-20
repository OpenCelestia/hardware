// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/gpu.
 *
 * GPU module public interface.
 * - Provides Android-style HAL interface for GPU/2D graphics acceleration devices.
 * - Supports image copy, scale, transform, blend, and fill operations.
 */

#ifndef HARDWARE_GPU_H
#define HARDWARE_GPU_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/cdefs.h>

#include "hardware.h"
#include "system_pixel_format.h"
#include "system_error.h"

__BEGIN_DECLS

/*===========================================================================
 * Module Version Definitions
 *===========================================================================*/

#define GPU_MODULE_API_VERSION_0_1      HARDWARE_MODULE_API_VERSION(0, 1)
#define GPU_DEVICE_API_VERSION_1_0      HARDWARE_DEVICE_API_VERSION(1, 0)

/*===========================================================================
 * Module ID and Device Definitions
 *===========================================================================*/

/** @brief GPU hardware module ID */
#define GPU_HARDWARE_MODULE_ID          "gpu"

/** @brief GPU device name */
#define GPU_DEVICE_NAME                 "gpu"

/*===========================================================================
 * Constants
 *===========================================================================*/

/** @brief Minimum opacity value (below this will be transparent) */
#define IMAGE_OPACITY_MIN               2

/** @brief Maximum opacity value (above this will fully cover) */
#define IMAGE_OPACITY_MAX               253

/*===========================================================================
 * Type Definitions
 *===========================================================================*/

/**
 * @brief Color key structure for blend operations
 */
struct image_colorkey {
    uint8_t u8OpacityPositive;      /**< Opacity of positive selection */
    uint8_t u8OpacityInverse;       /**< Opacity of inverse selection */
    uint32_t u32ColorKey;           /**< Color key value */
    uint8_t u8Differ;               /**< Color key difference range */
};

/**
 * @brief Blend operation types
 */
enum blend_opt {
    /** Foreground: alpha optional, global opacity optional
     *  Background: solid color, no alpha
     *  Output: solid color, no alpha */
    eBLEND_SOLID_COLOR = 0,

    /** Foreground: with global opacity, alpha optional
     *  Background: with global opacity, alpha optional
     *  Output: alpha optional */
    eBLEND_ALPHA_BLEND = 2,

    /** Foreground: alpha optional, global opacity optional
     *  Background: pure color, no alpha
     *  Output: no alpha */
    eBLEND_PURE_BACKGROUND,

    /** Foreground: pure color, with alpha, global opacity optional
     *  Background: solid color, no alpha
     *  Output: no alpha */
    eBLEND_PURE_FOREGROUND,

    eBLEND_MAX
};

/**
 * @brief Blend configuration structure
 */
struct blend_config {
    enum blend_opt option;          /**< Blend option */

    /**
     * Color format:
     * | format/byte |-- 0 --|-- 1 --|-- 2 --|-- 3 --|
     * |  RGB/ARGB   |-- B --|-- G --|-- R --| alpha |
     * |  YUV/AYUV   |-- U --|-- Y --|-- V --| alpha |
     */
    union {
        uint32_t u8ColorBackGround;     /**< Pure color background (no alpha) */
        uint32_t u8ColorForeGround;     /**< Pure color foreground (with alpha) */
    };
};

/**
 * @brief Image descriptor structure
 */
typedef struct image_desc {
    uint32_t u32Width;              /**< Image width */
    uint32_t u32Height;             /**< Image height */
    rect_t rect;                    /**< Clip or blend rectangular area */
    uint8_t *vaddr;                 /**< Virtual address of image buffer */
    uint64_t paddr;                 /**< Physical address of image buffer */
    system_pixel_format_t format;   /**< Pixel format */
    transform_t transform;          /**< Transform operation */
    uint8_t u8GlobalOpacity;        /**< Global opacity value */

    int32_t addrs_cnt;              /**< Number of plane addresses */
    uint8_t *vaddrs[4];             /**< Virtual addresses for multi-plane */
    uint64_t paddrs[4];             /**< Physical addresses for multi-plane */
} image_desc_t;

/*===========================================================================
 * Module and Device Structures
 *===========================================================================*/

/**
 * @brief GPU module structure
 */
typedef struct gpu_module {
    struct hw_module_t common;      /**< Common module fields */
} gpu_module_t;

/**
 * @brief GPU device structure
 */
typedef struct gpu_device {
    /**
     * Common methods of the device. This *must* be the first member of device
     * as users of this structure will cast a hw_device_t to device pointer.
     */
    struct hw_device_t common;

    /**
     * @brief Copy image
     * @param device Device context
     * @param pInputImage Input image descriptor
     * @param pOutputImage Output image descriptor
     * @return 0 on success, negative error code on failure
     */
    int (*image_copy)(struct gpu_device *device,
                      struct image_desc *pInputImage,
                      struct image_desc *pOutputImage);

    /**
     * @brief Scale image
     * @param device Device context
     * @param pInputImage Input image descriptor
     * @param pOutputImage Output image descriptor
     * @return 0 on success, negative error code on failure
     */
    int (*image_scale)(struct gpu_device *device,
                       struct image_desc *pInputImage,
                       struct image_desc *pOutputImage);

    /**
     * @brief Transform image (rotate, flip)
     * @param device Device context
     * @param pInputImage Input image descriptor
     * @param pOutputImage Output image descriptor
     * @param transform Transform operation
     * @return 0 on success, negative error code on failure
     */
    int (*image_transform)(struct gpu_device *device,
                           struct image_desc *pInputImage,
                           struct image_desc *pOutputImage,
                           transform_t transform);

    /**
     * @brief Blend images
     * @param device Device context
     * @param pForeGround Foreground image descriptor
     * @param pBackGround Background image descriptor
     * @param pOutputImage Output image descriptor (NULL to blend in-place)
     * @param config Blend configuration
     * @return 0 on success, negative error code on failure
     */
    int (*image_blend)(struct gpu_device *device,
                       struct image_desc *pForeGround,
                       struct image_desc *pBackGround,
                       struct image_desc *pOutputImage,
                       struct blend_config *config);

    /**
     * @brief Blend image with color key
     * @param device Device context
     * @param pInputImage Input image descriptor (no alpha)
     * @param pOutputImage Output image descriptor (no alpha)
     * @param colorkey Color key configuration
     * @return 0 on success, negative error code on failure
     */
    int (*image_blend_colorkey)(struct gpu_device *device,
                                struct image_desc *pInputImage,
                                struct image_desc *pOutputImage,
                                struct image_colorkey *colorkey);

    /**
     * @brief Fill image with color
     * @param device Device context
     * @param pOutputImage Output image descriptor
     * @param color Fill color value
     * @return 0 on success, negative error code on failure
     */
    int (*image_fill)(struct gpu_device *device,
                      struct image_desc *pOutputImage,
                      uint32_t color);

    /**
     * @brief Check if input pixel format is supported
     * @param format Pixel format to check
     * @return true if supported, false otherwise
     */
    bool (*check_input_format)(system_pixel_format_t format);

    /**
     * @brief Check if output pixel format is supported
     * @param format Pixel format to check
     * @return true if supported, false otherwise
     */
    bool (*check_output_format)(system_pixel_format_t format);

    /**
     * @brief Get minimum supported width
     * @return Minimum width in pixels
     */
    int32_t (*supported_min_width)(void);

    /**
     * @brief Get minimum supported height
     * @return Minimum height in pixels
     */
    int32_t (*supported_min_height)(void);

} gpu_device_t;

/*===========================================================================
 * Module API Functions
 *===========================================================================*/

/**
 * @brief Initialize GPU module
 * @return 0 on success, negative error code on failure
 */
int hw_gpu_module_init(void);

/*===========================================================================
 * Error Code Definitions
 *===========================================================================*/

/** @brief Success */
#define HARDWARE_GPU_OK                       SYSTEM_SUCCESS

/** @brief Invalid parameter error */
#define HARDWARE_GPU_ERROR_INVALID_PARAM      SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_GPU, ERROR_INVALID_PARAM)

/** @brief Operation not supported error */
#define HARDWARE_GPU_ERROR_NOT_SUPPORTED      SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_GPU, ERROR_NOT_SUPPORTED)

/** @brief Memory allocation error */
#define HARDWARE_GPU_ERROR_MEMORY             SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_GPU, ERROR_MEMORY)

/** @brief Operation failed error */
#define HARDWARE_GPU_ERROR_OPERATION_FAILED   SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_GPU, ERROR_OPERATION_FAILED)

__END_DECLS

#endif /* HARDWARE_GPU_H */

