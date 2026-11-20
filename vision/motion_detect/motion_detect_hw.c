// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/vision/motion_detect.
 *
 * Motion Detection vision sub-module implementation.
 * - Implements all methods of motion_detect_device_t.
 * - Uses hal_motion_detect_api.h to call chip-specific motion detection / MRD logic.
 * - Public API: hardware/include/motion_detect.h (motion_detect_device_t / module).
 */

#include "motion_detect.h"
#include "motion_detect_hw.h"
#include "motion_detect_config.h"
#include "hal_motion_detect_api.h"

#include "log.h"
#include "system_config.h"
#include "gthread.h"
#include "geometry.h"
#include "isp.h"  /* ISP_MD_INFO_S */

#include <string.h>

#define LOG_TAG "MOTION_DETECT"

/* Constants from original implementation */
#define MOTION_DETECT_MAX_BOXES 10

/* Sensitivity mapping table */
static double array_sensitity[5] = {
    0.15f, 0.10f, 0.06f, 0.04f, 0.02f
};

static motion_detect_data_t g_motion_detect_data = {0};

static inline int vision_status_from_hal(int ret)
{
    return (ret == 0) ? HARDWARE_MOTION_DETECT_OK : HARDWARE_MOTION_DETECT_ERROR_OPERATION_FAILED;
}

/* Forward declarations */
static int motion_detect_check_day_night(motion_detect_data_t *data);
static motion_detect_rect_t convert_hal_box_to_rect(const hal_motion_detect_box_api_t *box, int md_vsize);
static float calculate_area_ratio(const motion_detect_rect_t *rect);
static float calculate_filter_ratio(int sensitivity);
static int motion_detect_rect_intersection(const motion_detect_rect_t *rect1,
                                           const motion_detect_rect_t *rect2);
static const unsigned short *extract_md_stat(const void *md_info);
static int motion_detect_process_internal(const void *md_info);

/* ============================================================================
 * Day/Night Mode Check
 * ============================================================================ */

/**
 * @brief Check day/night status and update MRD mode.
 * @param data Motion detect data pointer.
 * @return 0 on success, negative on error.
 */
static int motion_detect_check_day_night(motion_detect_data_t *data)
{
    /* Get day/night status via registered callback (decoupled design).
     * Upper layer (e.g., framework) should register the callback during initialization.
     */
    int day_night_status = -1;
    
    if (data->day_night_callback) {
        day_night_status = data->day_night_callback();
    } else {
        /* Callback not registered, use cached status */
        return 0;
    }

    /* Normalize status: 0=day, 1=night, -1=unknown/error */
    if (day_night_status < 0) {
        day_night_status = -1;
    }

    if (day_night_status == data->day_night_status) {
        return 0;  /* No change */
    }

    /* Set status to HAL layer (HAL will update MRD mode) */
    if (day_night_status >= 0) {
        hal_motion_detect_set_day_night_status(day_night_status);
    }

    log_info("Set MRD mode to %s", day_night_status == 0 ? "day" : "night");

    data->day_night_status = day_night_status;
    return 0;
}

/* ============================================================================
 * Utility Helpers
 * ============================================================================ */

/**
 * @brief Convert HAL box to motion detect rectangle.
 * @param box HAL box structure.
 * @param md_vsize MD vertical dimension.
 * @return Motion detect rectangle.
 */
static motion_detect_rect_t convert_hal_box_to_rect(const hal_motion_detect_box_api_t *box, int md_vsize)
{
    motion_detect_rect_t rect = {0};

    rect.x       = box->left * MOTION_DETECT_AREA_MAX / ISP_MD_DIMENSION_H_MAX;
    rect.y       = box->top * MOTION_DETECT_AREA_MAX / md_vsize;
    rect.width   = (box->right - box->left + 1) * MOTION_DETECT_AREA_MAX / ISP_MD_DIMENSION_H_MAX;
    rect.height  = (box->bottom - box->top + 1) * MOTION_DETECT_AREA_MAX / md_vsize;

    return rect;
}

static float calculate_area_ratio(const motion_detect_rect_t *rect)
{
    int areatmp = MOTION_DETECT_AREA_MAX * MOTION_DETECT_AREA_MAX;
    int area = rect->height * rect->width;
    return (float)area / areatmp;
}

static float calculate_filter_ratio(int sensitivity)
{
    return percent_to_value(sensitivity,
                            array_sensitity,
                            sizeof(array_sensitity) / sizeof(array_sensitity[0]));
}

static int motion_detect_rect_intersection(const motion_detect_rect_t *rect1,
                                           const motion_detect_rect_t *rect2)
{
    rect_t sys_rect1 = {
        .x      = rect1->x,
        .y      = rect1->y,
        .width  = rect1->width,
        .height = rect1->height,
    };

    rect_t sys_rect2 = {
        .x      = rect2->x,
        .y      = rect2->y,
        .width  = rect2->width,
        .height = rect2->height,
    };

    return rect_intersection(&sys_rect1, &sys_rect2, NULL);
}

static const unsigned short *extract_md_stat(const void *md_info)
{
    const ISP_MD_INFO_S *md = (const ISP_MD_INFO_S *)md_info;
    if (!md) return NULL;
    /* Return pointer to the 2D array u16Stat for ak_mrd function. */
    return (const unsigned short *)md->u16Stat;
}

/* ============================================================================
 * MRD-Compatible Interfaces Implemented via HAL Wrappers
 * ============================================================================ */

/**
 * @brief Initialize MRD and return handle.
 * @param dev Device pointer.
 * @return MRD handle on success, NULL on failure.
 */
static motion_detect_mrd_handle_t motion_detect_mrd_init_impl(struct motion_detect_device *dev)
{
    (void)dev;
    return hal_mrd_init();
}

/**
 * @brief Destroy MRD handle.
 * @param dev Device pointer.
 * @param handle MRD handle.
 * @return 0 on success, -1 on failure.
 */
static int motion_detect_mrd_destroy_impl(struct motion_detect_device *dev,
                                          motion_detect_mrd_handle_t handle)
{
    (void)dev;
    return hal_mrd_destroy(handle);
}

/**
 * @brief Set filters for motion region detection.
 * @param dev Device pointer.
 * @param handle MRD handle.
 * @param flt_big Big filter to roughly detect motion region.
 * @param flt_small Small filter to expand rough detection result.
 * @return 0 on success, -1 on failure.
 */
static int motion_detect_mrd_set_filters_impl(struct motion_detect_device *dev,
                                              motion_detect_mrd_handle_t handle,
                                              unsigned short flt_big,
                                              unsigned short flt_small)
{
    (void)dev;
    return hal_mrd_set_filters(handle, flt_big, flt_small);
}

/**
 * @brief Set the range of detected motion region size.
 * @param dev Device pointer.
 * @param handle MRD handle.
 * @param size_min Minimum size of detected motion region.
 * @param size_max Maximum size of detected motion region.
 * @return 0 on success, -1 on failure.
 */
static int motion_detect_mrd_set_motion_region_size_impl(struct motion_detect_device *dev,
                                                         motion_detect_mrd_handle_t handle,
                                                         int size_min,
                                                         int size_max)
{
    (void)dev;
    return hal_mrd_set_motion_region_size(handle, size_min, size_max);
}

/**
 * @brief Set mode for motion region detection.
 * @param dev Device pointer.
 * @param handle MRD handle.
 * @param mode 0=multiple frames mode with floating wadding method, otherwise=single frame mode.
 * @return 0 on success, -1 on failure.
 */
static int motion_detect_mrd_set_mode_impl(struct motion_detect_device *dev,
                                           motion_detect_mrd_handle_t handle,
                                           unsigned char mode)
{
    (void)dev;
    return hal_mrd_set_mode(handle, mode);
}

/**
 * @brief Detect motion region.
 * @param dev Device pointer.
 * @param handle MRD handle.
 * @param md_stat Motion detection statistics from ISP (2D array: uint16_t md[][64]).
 * @param len VPSS_MD_DIMENSION in vertical direction.
 * @param boxes Output array for motion region boxes.
 * @return Number of detected boxes (>0), 0=no motion, -1=error.
 */
static int motion_detect_mrd_process_impl(struct motion_detect_device *dev,
                                          motion_detect_mrd_handle_t handle,
                                          unsigned short md_stat[][64],
                                          int len,
                                          motion_detect_box_t *boxes)
{
    (void)dev;
    if (!handle || !boxes || len <= 0) {
        return -1;
    }

    return hal_mrd(handle, md_stat, len, (void *)boxes);
}

/* ============================================================================
 * Internal Processing Pipeline
 * ============================================================================ */

/**
 * @brief Process motion detection internally.
 * @param md_info ISP motion detection information.
 * @return 0 on success, negative on error.
 */
static int motion_detect_process_internal(const void *md_info)
{
    motion_detect_data_t *data = &g_motion_detect_data;

    if (!data->initialized || !data->config.enable) {
        return 0;
    }

    /* 1. Day/night mode check */
    motion_detect_check_day_night(data);

    /* 2. Get MD vertical size */
    int md_vsize = 0;
    hal_motion_detect_get_vsize(&md_vsize);
    data->md_vsize = md_vsize;

    /* 3. Run MRD algorithm */
    hal_motion_detect_box_api_t boxes[MOTION_DETECT_MAX_BOXES] = {0};

    const unsigned short *md_stat = extract_md_stat(md_info);
    if (!md_stat) {
        return 0;  /* No MD info available */
    }
    
    /* Cast to match function signature (const to non-const for ak_mrd compatibility) */
    int num_detected = hal_motion_detect_process_mrd((unsigned short (*)[ISP_MD_DIMENSION_H_MAX])md_stat,
                                                     md_vsize,
                                                     (void *)boxes,
                                                     MOTION_DETECT_MAX_BOXES);
    if (num_detected <= 0) {
        return 0;
    }

    /* 4. Filter motion detection results */
    motion_detect_result_t results[MOTION_DETECT_MAX_BOXES] = {0};
    int valid_count = 0;

    for (int i = 0; i < num_detected; i++) {
        motion_detect_rect_t rect = convert_hal_box_to_rect(&boxes[i], md_vsize);

        if (!motion_detect_rect_intersection(&data->config.valid_area, &rect)) {
            continue;
        }

        float area_ratio = calculate_area_ratio(&rect);
        if (area_ratio < data->filter_ratio) {
            continue;
        }

        results[valid_count].rect = rect;
        results[valid_count].area_ratio = area_ratio;
        valid_count++;
    }

    /* 5. Notify upper layer via callback */
    if (valid_count > 0 && data->callback) {
        data->callback(valid_count, results, data->user_data);
    }

    return 0;
}

/* ============================================================================
 * Device Method Implementations
 * ============================================================================ */

/**
 * @brief Initialize motion detection.
 * @param dev Device pointer.
 * @return HARDWARE_MOTION_DETECT_OK on success, error code on failure.
 */
static int motion_detect_device_init(struct motion_detect_device *dev)
{
    (void)dev;
    motion_detect_data_t *data = &g_motion_detect_data;

    if (data->initialized) {
        return HARDWARE_MOTION_DETECT_OK;
    }

    int ret = hal_motion_detect_init();
    if (ret != 0) {
        log_error("HAL init failed");
        return HARDWARE_MOTION_DETECT_ERROR_OPERATION_FAILED;
    }

    g_mutex_init(&data->mutex);

    ret = motion_detect_config_load_from_ini(&data->config, array_sensitity);
    if (ret != HARDWARE_MOTION_DETECT_OK) {
        g_mutex_clear(&data->mutex);
        hal_motion_detect_deinit();
        return ret;
    }

    data->filter_ratio = calculate_filter_ratio(data->config.sensitivity);
    data->day_night_status = -1;
    data->md_vsize = -1;
    data->initialized = 1;

    log_info("Motion detect initialized");
    return HARDWARE_MOTION_DETECT_OK;
}

/**
 * @brief Deinitialize motion detection.
 * @param dev Device pointer.
 * @return HARDWARE_MOTION_DETECT_OK on success, error code on failure.
 */
static int motion_detect_device_deinit(struct motion_detect_device *dev)
{
    (void)dev;
    motion_detect_data_t *data = &g_motion_detect_data;

    if (!data->initialized) {
        return HARDWARE_MOTION_DETECT_OK;
    }

    hal_motion_detect_deinit();
    g_mutex_clear(&data->mutex);
    memset(data, 0, sizeof(motion_detect_data_t));

    log_info("Motion detect deinitialized");
    return HARDWARE_MOTION_DETECT_OK;
}

/**
 * @brief Set motion detection configuration.
 * @param dev Device pointer.
 * @param config Configuration parameters.
 * @return HARDWARE_MOTION_DETECT_OK on success, error code on failure.
 */
static int motion_detect_device_set_config(struct motion_detect_device *dev,
                                           const motion_detect_config_t *config)
{
    (void)dev;
    motion_detect_data_t *data = &g_motion_detect_data;

    if (!data->initialized || !config) {
        return HARDWARE_MOTION_DETECT_ERROR_INVALID_PARAM;
    }

    g_mutex_lock(&data->mutex);
    data->config = *config;
    data->filter_ratio = calculate_filter_ratio(data->config.sensitivity);
    int ret = motion_detect_config_save_to_ini(&data->config);
    g_mutex_unlock(&data->mutex);

    if (ret != HARDWARE_MOTION_DETECT_OK) {
        return ret;
    }

    log_info("Motion detect config updated");
    return HARDWARE_MOTION_DETECT_OK;
}

/**
 * @brief Get motion detection configuration.
 * @param dev Device pointer.
 * @param config Output parameter for configuration.
 * @return HARDWARE_MOTION_DETECT_OK on success, error code on failure.
 */
static int motion_detect_device_get_config(struct motion_detect_device *dev,
                                           motion_detect_config_t *config)
{
    (void)dev;
    motion_detect_data_t *data = &g_motion_detect_data;

    if (!data->initialized || !config) {
        return HARDWARE_MOTION_DETECT_ERROR_INVALID_PARAM;
    }

    g_mutex_lock(&data->mutex);
    *config = data->config;
    g_mutex_unlock(&data->mutex);

    return HARDWARE_MOTION_DETECT_OK;
}

/**
 * @brief Set callback function for detection results.
 * @param dev Device pointer.
 * @param callback Callback function.
 * @param user_data User data pointer.
 * @return HARDWARE_MOTION_DETECT_OK on success, error code on failure.
 */
static int motion_detect_device_set_callback(struct motion_detect_device *dev,
                                             motion_detect_callback_t callback,
                                             void *user_data)
{
    (void)dev;
    motion_detect_data_t *data = &g_motion_detect_data;

    if (!data->initialized) {
        return HARDWARE_MOTION_DETECT_ERROR_NOT_INIT;
    }

    data->callback = callback;
    data->user_data = user_data;

    log_info("Motion detect callback set");
    return HARDWARE_MOTION_DETECT_OK;
}

/**
 * @brief Set callback function for day/night status query.
 * @param dev Device pointer.
 * @param callback Callback function that returns day/night status (0=day, 1=night, <0=error).
 * @return HARDWARE_MOTION_DETECT_OK on success, error code on failure.
 */
static int motion_detect_device_set_day_night_callback(struct motion_detect_device *dev,
                                                       motion_detect_day_night_callback_t callback)
{
    (void)dev;
    motion_detect_data_t *data = &g_motion_detect_data;

    if (!data->initialized) {
        return HARDWARE_MOTION_DETECT_ERROR_NOT_INIT;
    }

    data->day_night_callback = callback;

    log_info("Motion detect day/night callback set");
    return HARDWARE_MOTION_DETECT_OK;
}

/**
 * @brief Handle ISP motion detection information.
 * @param dev Device pointer.
 * @param md_info ISP motion detection information (ISP_MD_INFO_S*).
 * @return HARDWARE_MOTION_DETECT_OK on success, error code on failure.
 */
static int motion_detect_device_handle_isp_md(struct motion_detect_device *dev,
                                              const void *md_info)
{
    (void)dev;
    int ret = motion_detect_process_internal(md_info);
    return (ret >= 0) ? HARDWARE_MOTION_DETECT_OK : HARDWARE_MOTION_DETECT_ERROR_OPERATION_FAILED;
}

/**
 * @brief Close motion detection device.
 * @param device Device pointer.
 * @return HARDWARE_MOTION_DETECT_OK on success, error code on failure.
 */
static int motion_detect_device_close(struct hw_device_t *device)
{
    motion_detect_device_t *dev = (motion_detect_device_t *)device;
    if (!dev) {
        return HARDWARE_MOTION_DETECT_ERROR_INVALID_PARAM;
    }

    motion_detect_device_deinit(dev);
    free(dev);
    return HARDWARE_MOTION_DETECT_OK;
}

/* ============================================================================
 * Module Open and Initialization
 * ============================================================================ */

/**
 * @brief Open motion detection device.
 * @param module Module pointer.
 * @param device Output parameter for device pointer.
 * @return HARDWARE_MOTION_DETECT_OK on success, error code on failure.
 */
static int motion_detect_module_open(const struct motion_detect_module *module,
                                     struct motion_detect_device **device)
{
    (void)module;
    if (!device) return HARDWARE_MOTION_DETECT_ERROR_INVALID_PARAM;

    motion_detect_device_t *dev =
        (motion_detect_device_t *)calloc(1, sizeof(*dev));
    if (!dev) return HARDWARE_MOTION_DETECT_ERROR_MEMORY;

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = HARDWARE_DEVICE_API_VERSION(1, 0);
    dev->common.close = motion_detect_device_close;

    dev->init        = motion_detect_device_init;
    dev->deinit      = motion_detect_device_deinit;
    dev->set_config  = motion_detect_device_set_config;
    dev->get_config  = motion_detect_device_get_config;
    dev->set_callback = motion_detect_device_set_callback;
    dev->set_day_night_callback = motion_detect_device_set_day_night_callback;
    dev->handle_isp_md = motion_detect_device_handle_isp_md;

    /* MRD compatible interfaces */
    dev->mrd_init                 = motion_detect_mrd_init_impl;
    dev->mrd_destroy              = motion_detect_mrd_destroy_impl;
    dev->mrd_set_filters          = motion_detect_mrd_set_filters_impl;
    dev->mrd_set_motion_region_size = motion_detect_mrd_set_motion_region_size_impl;
    dev->mrd_set_mode             = motion_detect_mrd_set_mode_impl;
    dev->mrd_process              = motion_detect_mrd_process_impl;

    *device = dev;
    return HARDWARE_MOTION_DETECT_OK;
}

/* ============================================================================
 * Module Definition and Registration
 * ============================================================================ */

static int motion_detect_module_methods_open(const struct hw_module_t *module,
                                             const char *id,
                                             struct hw_device_t **device)
{
    (void)id;
    return motion_detect_module_open((const motion_detect_module_t *)module,
                                     (motion_detect_device_t **)device);
}

static struct hw_module_methods_t motion_detect_module_methods = {
    .open = motion_detect_module_methods_open,
};

static motion_detect_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .version_major = 1,
        .version_minor = 0,
        .id = MOTION_DETECT_HARDWARE_MODULE_ID,
        .name = "Motion Detect HAL Module",
        .author = "Anyka",
        .methods = &motion_detect_module_methods,
    },
    .open = motion_detect_module_open,
};

/**
 * @brief Initialize motion detection hardware module.
 * @return HARDWARE_MOTION_DETECT_OK on success, error code on failure.
 */
int hw_motion_detect_module_init(void)
{
    int ret = hw_register_module(&(HAL_MODULE_INFO_SYM.common));
    if (ret != 0) {
        log_error("register motion_detect module failed: %d", ret);
        return ret;
    }
    log_info("motion_detect module registered");
    return HARDWARE_MOTION_DETECT_OK;
}
