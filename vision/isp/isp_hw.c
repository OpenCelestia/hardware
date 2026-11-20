// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/vision/isp.
 *
 * ISP vision sub-module implementation (common layer).
 * - Implements all methods of isp_device_t.
 * - Calls into the HAL layer via hal_isp_api.h to access chip-specific ISP.
 * - Public API: see hardware/include/isp.h (isp_device_t / isp_module_t).
 */

#include "isp.h"
#include "isp_hw.h"
#include "hal_isp_api.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define LOG_TAG "VISION_ISP"

static inline int vision_status_from_hal(int ret)
{
    return (ret == 0) ? HARDWARE_ISP_OK : HARDWARE_ISP_ERROR_OPERATION_FAILED;
}

/* ============================================================================
 * Device Method Implementations
 * ============================================================================ */

/**
 * @brief Initialize ISP device.
 * @param dev Device pointer.
 * @param config Configuration (can pass dev_id etc).
 * @return HARDWARE_ISP_OK on success, error code on failure.
 */
static int isp_device_init(isp_device_t *dev, void *config)
{
    if (!dev) {
        return HARDWARE_ISP_ERROR_INVALID_PARAM;
    }
    isp_context_internal_t *ctx = (isp_context_internal_t *)(uintptr_t)dev->common.reserved[0];
    if (!ctx) {
        return HARDWARE_ISP_ERROR_NOT_INIT;
    }

    /* Default to dev_id = 0; multi-device support can be added by parsing config. */
    int dev_id = 0;
    (void)config;

    ctx->hal = hal_isp_init(dev_id, config);
    return (ctx->hal != NULL) ? HARDWARE_ISP_OK
                              : HARDWARE_ISP_ERROR_OPERATION_FAILED;
}

/**
 * @brief Deinitialize ISP device.
 * @param dev Device pointer.
 * @return HARDWARE_ISP_OK on success, error code on failure.
 */
static int isp_device_deinit(isp_device_t *dev)
{
    if (!dev) {
        return HARDWARE_ISP_ERROR_INVALID_PARAM;
    }
    isp_context_internal_t *ctx = (isp_context_internal_t *)(uintptr_t)dev->common.reserved[0];
    if (ctx && ctx->hal) {
        hal_isp_deinit(ctx->hal);
        ctx->hal = NULL;
    }
    return HARDWARE_ISP_OK;
}

/**
 * @brief Set ISP parameter.
 * @param dev Device pointer.
 * @param key Parameter key.
 * @param value Parameter value.
 * @return HARDWARE_ISP_OK on success, error code on failure.
 */
static int isp_device_set_param(isp_device_t *dev, int key, long value)
{
    if (!dev) return HARDWARE_ISP_ERROR_INVALID_PARAM;
    isp_context_internal_t *ctx = (isp_context_internal_t *)(uintptr_t)dev->common.reserved[0];
    if (!ctx || !ctx->hal) return HARDWARE_ISP_ERROR_NOT_INIT;
    return vision_status_from_hal(hal_isp_set_param(ctx->hal, key, value));
}

/**
 * @brief Get frame data from ISP.
 * @param dev Device pointer.
 * @param frame_out Output frame buffer.
 * @return HARDWARE_ISP_OK on success, error code on failure.
 */
static int isp_device_get_frame(isp_device_t *dev, void *frame_out)
{
    if (!dev || !frame_out) return HARDWARE_ISP_ERROR_INVALID_PARAM;
    isp_context_internal_t *ctx = (isp_context_internal_t *)(uintptr_t)dev->common.reserved[0];
    if (!ctx || !ctx->hal) return HARDWARE_ISP_ERROR_NOT_INIT;
    return vision_status_from_hal(hal_isp_get_frame(ctx->hal, frame_out));
}

/* ============================================================================
 * Legacy Direct-Call Features Wrapped as Device Methods
 * ============================================================================ */

/**
 * @brief Switch ISP mode (day/night).
 * @param dev Device pointer.
 * @param mode ISP mode (ISP_MODE_DAY / ISP_MODE_NIGHT etc).
 * @return HARDWARE_ISP_OK on success, error code on failure.
 */
static int isp_device_switch_mode(isp_device_t *dev, isp_mode_t mode)
{
    (void)dev;

    hal_isp_mode_t hal_mode;
    switch (mode) {
    case ISP_MODE_DAY:
        hal_mode = HAL_ISP_MODE_DAY;
        break;
    case ISP_MODE_NIGHT:
        hal_mode = HAL_ISP_MODE_NIGHT;
        break;
    case ISP_MODE_USR_DEFINE_1:
        hal_mode = HAL_ISP_MODE_USR_DEFINE_1;
        break;
    case ISP_MODE_USR_DEFINE_2:
        hal_mode = HAL_ISP_MODE_USR_DEFINE_2;
        break;
    case ISP_MODE_USR_DEFINE_3:
        hal_mode = HAL_ISP_MODE_USR_DEFINE_3;
        break;
    default:
        log_error("Invalid ISP mode: %d", mode);
        return HARDWARE_ISP_ERROR_INVALID_PARAM;
    }

    /* Use unified HAL API with dev_id=VIDEO_DEV0 (single-device case). */
    return vision_status_from_hal(hal_isp_switch_mode_direct(VIDEO_DEV0, (int)hal_mode));
}

/**
 * @brief Set software photosensor parameters.
 * @param dev Device pointer.
 * @param param Photosensor parameters.
 * @return HARDWARE_ISP_OK on success, error code on failure.
 */
static int isp_device_set_soft_ps_param(isp_device_t *dev,
                                        const isp_soft_ps_param_t *param)
{
    (void)dev;
    if (!param) {
        log_error("Invalid parameter: param is NULL");
        return HARDWARE_ISP_ERROR_INVALID_PARAM;
    }

    /* Convert isp.h parameters to HAL-level structure. */
    hal_isp_soft_ps_param_t hal_param;
    memset(&hal_param, 0, sizeof(hal_param));

    hal_param.enNightType              = param->enNightType;
    hal_param.u32DayCheckFrameNum      = param->u32DayCheckFrameNum;
    hal_param.u32NightCheckFrameNum    = param->u32NightCheckFrameNum;
    hal_param.u32ExpStableCheckFrameNum = param->u32ExpStableCheckFrameNum;
    hal_param.s32ExpStableTimeOutMs    = param->s32ExpStableTimeOutMs;

    hal_param.stIrLedNightParm.u32DayToNightLum =
        param->stIrLedNightParm.u32DayToNightLum;
    hal_param.stIrLedNightParm.u32NightToDayLum =
        param->stIrLedNightParm.u32NightToDayLum;
    hal_param.stIrLedNightParm.u32AwbNightDisMax =
        param->stIrLedNightParm.u32AwbNightDisMax;
    hal_param.stIrLedNightParm.u32AwbNightDisMin =
        param->stIrLedNightParm.u32AwbNightDisMin;
    hal_param.stIrLedNightParm.u32GainR =
        param->stIrLedNightParm.u32GainR;
    hal_param.stIrLedNightParm.u32GainB =
        param->stIrLedNightParm.u32GainB;
    hal_param.stIrLedNightParm.s32OffsetR =
        param->stIrLedNightParm.s32OffsetR;
    hal_param.stIrLedNightParm.s32OffsetB =
        param->stIrLedNightParm.s32OffsetB;

    return vision_status_from_hal(
        hal_isp_set_soft_ps_param_direct(VIDEO_DEV0, (const void *)&hal_param));
}

/**
 * @brief Get software photosensor detection result.
 * @param dev Device pointer.
 * @param pre_status Previous status (0=day, 1=night).
 * @param day_night Output current status (0=day, 1=night).
 * @return HARDWARE_ISP_OK on success, error code on failure.
 */
static int isp_device_get_soft_ps_level(isp_device_t *dev,
                                        int pre_status,
                                        int *day_night)
{
    (void)dev;
    if (!day_night) {
        log_error("Invalid parameter: day_night is NULL");
        return HARDWARE_ISP_ERROR_INVALID_PARAM;
    }

    return vision_status_from_hal(
        hal_isp_get_soft_ps_level_direct(VIDEO_DEV0, pre_status, day_night));
}

/* ============================================================================
 * Device Close
 * ============================================================================ */

/**
 * @brief Close ISP device.
 * @param device Device pointer.
 * @return HARDWARE_ISP_OK on success, error code on failure.
 */
static int isp_device_close(struct hw_device_t *device)
{
    if (!device) return HARDWARE_ISP_ERROR_INVALID_PARAM;
    isp_device_t *dev = (isp_device_t *)device;
    isp_device_deinit(dev);
    isp_context_internal_t *ctx = (isp_context_internal_t *)(uintptr_t)dev->common.reserved[0];
    if (ctx) free(ctx);
    free(dev);
    return HARDWARE_ISP_OK;
}

/* ============================================================================
 * Module Open and Initialization
 * ============================================================================ */

/**
 * @brief Open ISP device.
 * @param module ISP module pointer.
 * @param device Output device pointer.
 * @return HARDWARE_ISP_OK on success, error code on failure.
 */
int isp_module_open(const isp_module_t *module, isp_device_t **device)
{
    (void)module;
    if (!device) return HARDWARE_ISP_ERROR_INVALID_PARAM;

    isp_device_t *dev = (isp_device_t *)calloc(1, sizeof(*dev));
    if (!dev) return HARDWARE_ISP_ERROR_MEMORY;
    isp_context_internal_t *ctx =
        (isp_context_internal_t *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        free(dev);
        return HARDWARE_ISP_ERROR_MEMORY;
    }

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = HARDWARE_DEVICE_API_VERSION(1, 0);
    dev->common.close = isp_device_close;
    dev->common.reserved[0] = (uint64_t)(uintptr_t)ctx;

    dev->init              = isp_device_init;
    dev->deinit            = isp_device_deinit;
    dev->set_param         = isp_device_set_param;
    dev->get_frame         = isp_device_get_frame;
    dev->switch_mode       = isp_device_switch_mode;
    dev->set_soft_ps_param = isp_device_set_soft_ps_param;
    dev->get_soft_ps_level = isp_device_get_soft_ps_level;

    *device = dev;
    return HARDWARE_ISP_OK;
}

/* ============================================================================
 * Module Definition and Registration
 * ============================================================================ */

static int isp_module_methods_open(const struct hw_module_t *module,
                                   const char *id,
                                   struct hw_device_t **device)
{
    (void)id;
    return isp_module_open((const isp_module_t *)module, (isp_device_t **)device);
}

static struct hw_module_methods_t isp_module_methods = {
    .open = isp_module_methods_open,
};

static isp_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .version_major = 1,
        .version_minor = 0,
        .id = ISP_HARDWARE_MODULE_ID,
        .name = "Vision ISP HAL Module",
        .author = "Anyka",
        .methods = &isp_module_methods,
    },
    .open = isp_module_open,
};

/**
 * @brief Initialize ISP hardware module.
 * @return HARDWARE_ISP_OK on success, error code on failure.
 */
int hw_isp_module_init(void)
{
    int ret = hw_register_module(&(HAL_MODULE_INFO_SYM.common));
    if (ret != 0) {
        log_error("register isp module failed: %d", ret);
        return ret;
    }
    log_info("isp module registered");
    return HARDWARE_ISP_OK;
}

/* ============================================================================
 * Direct-Call Interfaces (for framework/driver use)
 * ============================================================================ */

#if defined(__GNUC__)
#define ISP_WEAK __attribute__((weak))
#else
#define ISP_WEAK
#endif

/**
 * @brief Switch ISP mode directly (direct-call style API for framework/driver).
 * @param dev_id Device ID (usually VIDEO_DEV0).
 * @param mode ISP mode (ISP_MODE_DAY, ISP_MODE_NIGHT, etc.).
 * @return HARDWARE_ISP_OK on success, error code on failure.
 */
ISP_WEAK int isp_switch_mode(int dev_id, isp_mode_t mode)
{
    hal_isp_mode_t hal_mode;
    switch (mode) {
    case ISP_MODE_DAY:
        hal_mode = HAL_ISP_MODE_DAY;
        break;
    case ISP_MODE_NIGHT:
        hal_mode = HAL_ISP_MODE_NIGHT;
        break;
    case ISP_MODE_USR_DEFINE_1:
        hal_mode = HAL_ISP_MODE_USR_DEFINE_1;
        break;
    case ISP_MODE_USR_DEFINE_2:
        hal_mode = HAL_ISP_MODE_USR_DEFINE_2;
        break;
    case ISP_MODE_USR_DEFINE_3:
        hal_mode = HAL_ISP_MODE_USR_DEFINE_3;
        break;
    default:
        log_error("Invalid ISP mode: %d", mode);
        return HARDWARE_ISP_ERROR_INVALID_PARAM;
    }

    return vision_status_from_hal(hal_isp_switch_mode_direct(dev_id, (int)hal_mode));
}

/**
 * @brief Set software photosensor parameters directly (direct-call style API).
 * @param dev_id Device ID (usually VIDEO_DEV0).
 * @param param Photosensor parameters.
 * @return HARDWARE_ISP_OK on success, error code on failure.
 */
ISP_WEAK int isp_set_soft_ps_param(int dev_id, const isp_soft_ps_param_t *param)
{
    if (!param) {
        log_error("Invalid parameter: param is NULL");
        return HARDWARE_ISP_ERROR_INVALID_PARAM;
    }

    /* Convert isp.h parameters to HAL-level structure. */
    hal_isp_soft_ps_param_t hal_param;
    memset(&hal_param, 0, sizeof(hal_param));

    hal_param.enNightType              = param->enNightType;
    hal_param.u32DayCheckFrameNum      = param->u32DayCheckFrameNum;
    hal_param.u32NightCheckFrameNum    = param->u32NightCheckFrameNum;
    hal_param.u32ExpStableCheckFrameNum = param->u32ExpStableCheckFrameNum;
    hal_param.s32ExpStableTimeOutMs    = param->s32ExpStableTimeOutMs;

    hal_param.stIrLedNightParm.u32DayToNightLum =
        param->stIrLedNightParm.u32DayToNightLum;
    hal_param.stIrLedNightParm.u32NightToDayLum =
        param->stIrLedNightParm.u32NightToDayLum;
    hal_param.stIrLedNightParm.u32AwbNightDisMax =
        param->stIrLedNightParm.u32AwbNightDisMax;
    hal_param.stIrLedNightParm.u32AwbNightDisMin =
        param->stIrLedNightParm.u32AwbNightDisMin;
    hal_param.stIrLedNightParm.u32GainR =
        param->stIrLedNightParm.u32GainR;
    hal_param.stIrLedNightParm.u32GainB =
        param->stIrLedNightParm.u32GainB;
    hal_param.stIrLedNightParm.s32OffsetR =
        param->stIrLedNightParm.s32OffsetR;
    hal_param.stIrLedNightParm.s32OffsetB =
        param->stIrLedNightParm.s32OffsetB;

    return vision_status_from_hal(
        hal_isp_set_soft_ps_param_direct(dev_id, (const void *)&hal_param));
}

ISP_WEAK int isp_get_soft_ps_level(int dev_id, int pre_status, int *day_night)
{
    if (!day_night) {
        log_error("Invalid parameter: day_night is NULL");
        return HARDWARE_ISP_ERROR_INVALID_PARAM;
    }

    return vision_status_from_hal(
        hal_isp_get_soft_ps_level_direct(dev_id, pre_status, day_night));
}

#undef ISP_WEAK