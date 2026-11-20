// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/vision/isp.
 *
 * ISP (Image Signal Processor) module public interface.
 * - Provides Android-style HAL interface: isp_module_t / isp_device_t.
 * - Supports day/night mode switching and software photosensor detection.
 */
#ifndef HARDWARE_ISP_H
#define HARDWARE_ISP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "hardware.h"
#include "system_error.h"

__BEGIN_DECLS

/* ============================================================================
 * TYPE DEFINITIONS
 * ============================================================================ */

typedef struct isp_device isp_device_t;
typedef struct isp_module isp_module_t;

/**
 * @brief ISP mode enumeration.
 */
typedef enum {
    ISP_MODE_DAY = 0,               /**< Day mode. */
    ISP_MODE_NIGHT,                 /**< Night mode. */
    ISP_MODE_USR_DEFINE_1,          /**< User-defined mode 1. */
    ISP_MODE_USR_DEFINE_2,          /**< User-defined mode 2. */
    ISP_MODE_USR_DEFINE_3,          /**< User-defined mode 3. */
} isp_mode_t;

/**
 * @brief ISP night type enumeration.
 */
typedef enum {
    ISP_NIGHT_TYPE_IR_LED = 0,      /**< IR LED night vision. */
    ISP_NIGHT_TYPE_WHITE_LED,       /**< White LED night vision. */
    ISP_NIGHT_TYPE_BLACKLIGHT,      /**< Blacklight night vision. */
} isp_night_type_t;

/** @brief ISP motion detection horizontal dimension max. */
#define ISP_MD_DIMENSION_H_MAX  64

/** @brief ISP motion detection vertical dimension max. */
#define ISP_MD_DIMENSION_V_MAX  36

/**
 * @brief ISP motion detection info structure.
 * @details Compatible with HAL ISP_MD_INFO_S structure.
 */
typedef struct {
    uint16_t u16Stat[ISP_MD_DIMENSION_V_MAX][ISP_MD_DIMENSION_H_MAX];
} isp_md_info_t;

/**
 * @brief ISP software photosensor parameters structure.
 */
typedef struct {
    uint32_t enNightType;              /**< Night type (isp_night_type_t). */
    uint32_t u32DayCheckFrameNum;      /**< Frames to check for day detection. */
    uint32_t u32NightCheckFrameNum;    /**< Frames to check for night detection. */
    uint32_t u32ExpStableCheckFrameNum; /**< Frames for exposure stability check. */
    int32_t  s32ExpStableTimeOutMs;    /**< Exposure stability timeout in ms. */
    struct {
        uint32_t u32DayToNightLum;     /**< Day to night luminance threshold. */
        uint32_t u32NightToDayLum;     /**< Night to day luminance threshold. */
        uint32_t u32AwbNightDisMax;    /**< AWB night distance max. */
        uint32_t u32AwbNightDisMin;    /**< AWB night distance min. */
        uint32_t u32GainR;             /**< Red channel gain. */
        uint32_t u32GainB;             /**< Blue channel gain. */
        int32_t  s32OffsetR;           /**< Red channel offset. */
        int32_t  s32OffsetB;           /**< Blue channel offset. */
    } stIrLedNightParm;                /**< IR LED night parameters. */
} isp_soft_ps_param_t;

/* ============================================================================
 * MODULE AND DEVICE INTERFACE
 * ============================================================================ */

/** @brief ISP hardware module ID. */
#define ISP_HARDWARE_MODULE_ID     "vision_isp"

/** @brief ISP hardware interface name. */
#define ISP_HARDWARE_INTERFACE     "vision_isp_hw_if"

/**
 * @brief ISP device structure.
 * @details Provides function pointers for ISP device operations.
 *          Framework obtains isp_module_t via hw_get_module, then opens device.
 */
struct isp_device {
    struct hw_device_t common;

    /**
     * @brief Initialize ISP device.
     * @param dev ISP device pointer.
     * @param config Configuration (can pass dev_id etc).
     * @return 0 on success, negative error code on failure.
     */
    int (*init)(isp_device_t *dev, void *config);

    /**
     * @brief Deinitialize ISP device.
     * @param dev ISP device pointer.
     * @return 0 on success, negative error code on failure.
     */
    int (*deinit)(isp_device_t *dev);

    /**
     * @brief Set ISP parameter.
     * @param dev ISP device pointer.
     * @param key Parameter key.
     * @param value Parameter value.
     * @return 0 on success, negative error code on failure.
     */
    int (*set_param)(isp_device_t *dev, int key, long value);

    /**
     * @brief Get frame data from ISP.
     * @param dev ISP device pointer.
     * @param frame_out Output frame buffer.
     * @return 0 on success, negative error code on failure.
     */
    int (*get_frame)(isp_device_t *dev, void *frame_out);

    /**
     * @brief Switch ISP mode (day/night).
     * @param dev ISP device pointer.
     * @param mode ISP mode (ISP_MODE_DAY / ISP_MODE_NIGHT etc).
     * @return 0 on success, negative error code on failure.
     */
    int (*switch_mode)(isp_device_t *dev, isp_mode_t mode);

    /**
     * @brief Set software photosensor parameters.
     * @param dev ISP device pointer.
     * @param param Photosensor parameters.
     * @return 0 on success, negative error code on failure.
     */
    int (*set_soft_ps_param)(isp_device_t *dev,
                             const isp_soft_ps_param_t *param);

    /**
     * @brief Get software photosensor detection result.
     * @param dev ISP device pointer.
     * @param pre_status Previous status (0=day, 1=night).
     * @param day_night Output current status (0=day, 1=night).
     * @return 0 on success, negative error code on failure.
     */
    int (*get_soft_ps_level)(isp_device_t *dev,
                             int pre_status,
                             int *day_night);
};

/**
 * @brief ISP module structure.
 */
struct isp_module {
    struct hw_module_t common;

    /**
     * @brief Open ISP device.
     * @param module ISP module pointer.
     * @param device Output device pointer.
     * @return 0 on success, negative error code on failure.
     */
    int (*open)(const isp_module_t *module, isp_device_t **device);
};

/**
 * @brief Initialize ISP hardware module.
 * @return 0 on success, negative error code on failure.
 */
int hw_isp_module_init(void);

/* ============================================================================
 * Error Code Definitions
 * ============================================================================ */

/** @brief Success */
#define HARDWARE_ISP_OK                       SYSTEM_SUCCESS

/** @brief Invalid parameter error */
#define HARDWARE_ISP_ERROR_INVALID_PARAM      SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_VISION, ERROR_INVALID_PARAM)

/** @brief Not initialized error */
#define HARDWARE_ISP_ERROR_NOT_INIT           SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_VISION, ERROR_NOT_INIT)

/** @brief Memory allocation error */
#define HARDWARE_ISP_ERROR_MEMORY             SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_VISION, ERROR_MEMORY)

/** @brief Not found error */
#define HARDWARE_ISP_ERROR_NOT_FOUND          SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_VISION, ERROR_NOT_FOUND)

/** @brief Operation not supported error */
#define HARDWARE_ISP_ERROR_NOT_SUPPORTED      SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_VISION, ERROR_NOT_SUPPORTED)

/** @brief Operation failed error */
#define HARDWARE_ISP_ERROR_OPERATION_FAILED   SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_VISION, ERROR_OPERATION_FAILED)


/* ============================================================================
 * COMPATIBILITY ALIASES
 * ============================================================================ */

/** @brief Legacy day mode alias. */
#define MODE_DAY    ISP_MODE_DAY

/** @brief Legacy night mode alias. */
#define MODE_NIGHT  ISP_MODE_NIGHT

/** @brief Legacy video device alias. */
#define VIDEO_DEV0  1

/** @brief Legacy IR LED night type alias. */
#define IR_LED_NIGHT        ISP_NIGHT_TYPE_IR_LED

/** @brief Legacy white LED night type alias. */
#define WHITE_LED_NIGHT     ISP_NIGHT_TYPE_WHITE_LED

/** @brief Legacy blacklight night type alias. */
#define BLACKLIGHT_NIGHT    ISP_NIGHT_TYPE_BLACKLIGHT

/** @brief Legacy ISP_MD_INFO_S type alias. */
typedef isp_md_info_t ISP_MD_INFO_S;

/* ============================================================================
 * DIRECT-CALL INTERFACES (for framework/driver use)
 * ============================================================================ */

/**
 * @brief Switch ISP mode directly (direct-call style API for framework/driver).
 * @param dev_id Device ID (usually VIDEO_DEV0).
 * @param mode ISP mode (ISP_MODE_DAY, ISP_MODE_NIGHT, etc.).
 * @return 0 on success, negative error code on failure.
 */
int isp_switch_mode(int dev_id, isp_mode_t mode);

/**
 * @brief Set ISP soft photo sensor parameters directly (direct-call style API).
 * @param dev_id Device ID (usually VIDEO_DEV0).
 * @param param Soft photo sensor parameters.
 * @return 0 on success, negative error code on failure.
 */
int isp_set_soft_ps_param(int dev_id, const isp_soft_ps_param_t *param);

/**
 * @brief Get ISP soft photo sensor detection result directly (direct-call style API).
 * @param dev_id Device ID (usually VIDEO_DEV0).
 * @param pre_status Previous status (0=day, 1=night).
 * @param day_night Output current status (0=day, 1=night).
 * @return 0 on success, negative error code on failure.
 */
int isp_get_soft_ps_level(int dev_id, int pre_status, int *day_night);

__END_DECLS

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_ISP_H */


