// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/vision/motion_detect.
 *
 * Motion Detection module public interface.
 * - Provides Android-style HAL interface: motion_detect_module_t / motion_detect_device_t.
 * - Supports MRD (Motion Region Detection) operations.
 */
#ifndef HARDWARE_MOTION_DETECT_H
#define HARDWARE_MOTION_DETECT_H

#include <stdint.h>
#include <stdbool.h>
#include "hardware.h"
#include "system_error.h"

__BEGIN_DECLS

/**
 * @brief Motion detection rectangle structure.
 * @details Uses x, y, width, height to match system/core rect_t convention.
 */
typedef struct {
    int32_t x;                      /**< X coordinate. */
    int32_t y;                      /**< Y coordinate. */
    int32_t width;                  /**< Width. */
    int32_t height;                 /**< Height. */
} motion_detect_rect_t;

/**
 * @brief Motion detection configuration structure.
 * @details Based on original md_info_t structure.
 */
typedef struct {
    int enable;                     /**< Enable/disable motion detection (0=disable, 1=enable). */
    int sensitivity;                /**< Sensitivity (0-100), maps to filter ratio. */
    motion_detect_rect_t valid_area; /**< Valid detection area. */
} motion_detect_config_t;

/**
 * @brief Motion detection result structure.
 * @details Based on original MRD_RECTANGLE detection results.
 */
typedef struct {
    motion_detect_rect_t rect;      /**< Detection bounding box. */
    float area_ratio;               /**< Area ratio (0.0-1.0). */
} motion_detect_result_t;

/**
 * @brief Motion detection callback function type
 * @param num_results Number of detection results
 * @param results Array of detection results
 * @param user_data User data pointer
 */
typedef void (*motion_detect_callback_t)(int num_results,
                                         const motion_detect_result_t *results,
                                         void *user_data);

/**
 * @brief Day/night status callback function type
 * @return 0 for day, 1 for night, negative for unknown/error
 * @note This callback is used to get day/night status from upper layer.
 *       Upper layer (e.g., framework) should register this callback during initialization.
 */
typedef int (*motion_detect_day_night_callback_t)(void);

/**
 * @brief MRD handle type for legacy interface compatibility.
 */
typedef void* motion_detect_mrd_handle_t;

/**
 * @brief Motion detection box structure for legacy interface.
 */
typedef struct {
    int left;                       /**< Left coordinate. */
    int top;                        /**< Top coordinate. */
    int right;                      /**< Right coordinate. */
    int bottom;                     /**< Bottom coordinate. */
    int count;                      /**< Reserved for compatibility. */
} motion_detect_box_t;

/**
 * @brief Motion detection device structure.
 * @details Framework layer uses this structure to interact with motion detection module.
 */
typedef struct motion_detect_device {
    struct hw_device_t common;      /**< Common hardware device structure. */

    /**
     * @brief Initialize motion detection
     * @return 0 on success, negative error code on failure
     */
    int (*init)(struct motion_detect_device *dev);

    /**
     * @brief Deinitialize motion detection
     * @return 0 on success, negative error code on failure
     */
    int (*deinit)(struct motion_detect_device *dev);

    /**
     * @brief Set motion detection configuration
     * @param config Configuration parameters
     * @return 0 on success, negative error code on failure
     */
    int (*set_config)(struct motion_detect_device *dev,
                      const motion_detect_config_t *config);

    /**
     * @brief Get motion detection configuration
     * @param config Output parameter for configuration
     * @return 0 on success, negative error code on failure
     */
    int (*get_config)(struct motion_detect_device *dev,
                      motion_detect_config_t *config);

    /**
     * @brief Set callback function for detection results
     * @param callback Callback function
     * @param user_data User data pointer
     * @return 0 on success, negative error code on failure
     */
    int (*set_callback)(struct motion_detect_device *dev,
                        motion_detect_callback_t callback,
                        void *user_data);

    /**
     * @brief Set callback function for day/night status query
     * @param callback Callback function that returns day/night status (0=day, 1=night, <0=error)
     * @return 0 on success, negative error code on failure
     * @note Upper layer (e.g., framework) should register this callback to provide day/night status.
     *       This allows motion_detect module to be decoupled from other modules.
     */
    int (*set_day_night_callback)(struct motion_detect_device *dev,
                                  motion_detect_day_night_callback_t callback);

    /**
     * @brief Handle ISP motion detection information
     * @details Framework layer calls this function in ISP MD callback
     * @param md_info ISP motion detection information (ISP_MD_INFO_S*)
     * @return 0 on success, negative error code on failure
     */
    int (*handle_isp_md)(struct motion_detect_device *dev,
                         const void *md_info);

    /**
     * @brief Initialize MRD and return handle.
     * @param dev Motion detection device pointer.
     * @return MRD handle on success, NULL on failure.
     */
    motion_detect_mrd_handle_t (*mrd_init)(struct motion_detect_device *dev);

    /**
     * @brief Destroy MRD handle
     * @param handle MRD handle
     * @return 0 on success, -1 on failure
     */
    int (*mrd_destroy)(struct motion_detect_device *dev,
                       motion_detect_mrd_handle_t handle);

    /**
     * @brief Set filters for motion region detection
     * @param handle MRD handle
     * @param flt_big Big filter to roughly detect motion region
     * @param flt_small Small filter to expand rough detection result
     * @return 0 on success, -1 on failure
     */
    int (*mrd_set_filters)(struct motion_detect_device *dev,
                           motion_detect_mrd_handle_t handle,
                           unsigned short flt_big,
                           unsigned short flt_small);

    /**
     * @brief Set the range of detected motion region size
     * @param handle MRD handle
     * @param size_min Minimum size of detected motion region
     * @param size_max Maximum size of detected motion region
     * @return 0 on success, -1 on failure
     */
    int (*mrd_set_motion_region_size)(struct motion_detect_device *dev,
                                      motion_detect_mrd_handle_t handle,
                                      int size_min,
                                      int size_max);

    /**
     * @brief Set mode for motion region detection
     * @param handle MRD handle
     * @param mode 0=multiple frames mode with floating wadding method, otherwise=single frame mode
     * @return 0 on success, -1 on failure
     */
    int (*mrd_set_mode)(struct motion_detect_device *dev,
                        motion_detect_mrd_handle_t handle,
                        unsigned char mode);

    /**
     * @brief Detect motion region
     * @param handle MRD handle
     * @param md_stat Motion detection statistics from ISP (2D array: uint16_t md[][64])
     * @param len VPSS_MD_DIMENSION in vertical direction
     * @param boxes Output array for motion region boxes
     * @return Number of detected boxes (>0), 0=no motion, -1=error
     */
    int (*mrd_process)(struct motion_detect_device *dev,
                       motion_detect_mrd_handle_t handle,
                       unsigned short md_stat[][64],
                       int len,
                       motion_detect_box_t *boxes);
} motion_detect_device_t;

/**
 * @brief Motion detection module structure.
 */
typedef struct motion_detect_module {
    struct hw_module_t common;       /**< Common module structure. */

    /**
     * @brief Open motion detection device
     * @param device Output parameter for device pointer
     * @return 0 on success, negative error code on failure
     */
    int (*open)(const struct motion_detect_module *module,
                struct motion_detect_device **device);
} motion_detect_module_t;

#define MOTION_DETECT_HARDWARE_MODULE_ID "motion_detect"
#define MOTION_DETECT_HARDWARE_INTERFACE "motion_detect_hw_if"

/**
 * @brief Initialize motion detection hardware module
 * @return 0 on success, negative error code on failure
 */
int hw_motion_detect_module_init(void);

/* ============================================================================
 * Error Code Definitions
 * ============================================================================ */

/** @brief Success */
#define HARDWARE_MOTION_DETECT_OK                       SYSTEM_SUCCESS

/** @brief Invalid parameter error */
#define HARDWARE_MOTION_DETECT_ERROR_INVALID_PARAM      SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_VISION, ERROR_INVALID_PARAM)

/** @brief Not initialized error */
#define HARDWARE_MOTION_DETECT_ERROR_NOT_INIT           SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_VISION, ERROR_NOT_INIT)

/** @brief Memory allocation error */
#define HARDWARE_MOTION_DETECT_ERROR_MEMORY             SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_VISION, ERROR_MEMORY)

/** @brief Not found error */
#define HARDWARE_MOTION_DETECT_ERROR_NOT_FOUND          SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_VISION, ERROR_NOT_FOUND)

/** @brief Operation not supported error */
#define HARDWARE_MOTION_DETECT_ERROR_NOT_SUPPORTED      SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_VISION, ERROR_NOT_SUPPORTED)

/** @brief Operation failed error */
#define HARDWARE_MOTION_DETECT_ERROR_OPERATION_FAILED   SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_VISION, ERROR_OPERATION_FAILED)


/* ============================================================================
 * COMPATIBILITY MACROS
 * ============================================================================ */

/** @brief Legacy MRD_RECTANGLE type alias (not when SDK ak_mrd.h defines MRD_RECTANGLE). */
#if !defined(KERNEL_AOS)
#define MRD_RECTANGLE           motion_detect_box_t
#endif

/* ISP motion detection dimension constants are defined in platform headers (ak_vpss_isp.h) */
/* Use platform definitions: ISP_MD_DIMENSION_H_MAX and ISP_MD_DIMENSION_V_MAX */

__END_DECLS

#endif /* HARDWARE_MOTION_DETECT_H */



