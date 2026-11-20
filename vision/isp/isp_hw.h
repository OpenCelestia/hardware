// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/vision/isp.
 *
 * ISP vision sub-module internal declarations.
 * - Public header: hardware/include/isp.h
 * - This header: used only inside vision layer (isp_hw.c) and HAL API layer
 */

#ifndef HARDWARE_ISP_HW_H
#define HARDWARE_ISP_HW_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "isp.h"

/**
 * @brief ISP internal context
 * @details Maintained by isp_hw.c, associated via device->common.reserved[0].
 */
typedef struct isp_context_internal {
    void *hal;   /**< HAL handle, concrete type is defined by HAL layer */
} isp_context_internal_t;

/**
 * @brief ISP working mode (HAL abstraction level).
 * @details Used by HAL API layer to communicate with HAL implementation.
 */
typedef enum {
    HAL_ISP_MODE_DAY = 0,
    HAL_ISP_MODE_NIGHT,
    HAL_ISP_MODE_USR_DEFINE_1,
    HAL_ISP_MODE_USR_DEFINE_2,
    HAL_ISP_MODE_USR_DEFINE_3,
} hal_isp_mode_t;

/**
 * @brief ISP soft photo sensor parameters (HAL abstraction structure).
 * @details Used by HAL API layer to communicate with HAL implementation.
 */
typedef struct {
    uint32_t enNightType;              /**< Night type */
    uint32_t u32DayCheckFrameNum;      /**< Frames to check for day detection */
    uint32_t u32NightCheckFrameNum;    /**< Frames to check for night detection */
    uint32_t u32ExpStableCheckFrameNum; /**< Frames for exposure stability check */
    int32_t  s32ExpStableTimeOutMs;    /**< Exposure stability timeout in ms */
    struct {
        uint32_t u32DayToNightLum;     /**< Day to night luminance threshold */
        uint32_t u32NightToDayLum;     /**< Night to day luminance threshold */
        uint32_t u32AwbNightDisMax;    /**< AWB night distance max */
        uint32_t u32AwbNightDisMin;    /**< AWB night distance min */
        uint32_t u32GainR;             /**< Red channel gain */
        uint32_t u32GainB;             /**< Blue channel gain */
        int32_t  s32OffsetR;           /**< Red channel offset */
        int32_t  s32OffsetB;           /**< Blue channel offset */
    } stIrLedNightParm;                /**< IR LED night parameters */
} hal_isp_soft_ps_param_t;

/* Module open interface (called by HAL framework) */
int isp_module_open(const isp_module_t *module, isp_device_t **device);

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_ISP_HW_H */


