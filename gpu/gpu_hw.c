// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/gpu.
 *
 * GPU module implementation.
 * - Implements gpu_device_t methods for image processing operations.
 * - Provides image copy, scale, transform, blend and fill operations.
 */

#include "gpu.h"
#include "gpu_hw.h"
#include "hal_gpu_api.h"

#include <linux/kernel.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <stdio.h>

#include "log.h"

/*===========================================================================
 * Private Constants
 *===========================================================================*/

#define LOG_TAG "HW_GPU"

/*===========================================================================
 * Device Method Implementations
 *===========================================================================*/

/**
 * @brief Close GPU device.
 */
static int gpu_device_close(struct hw_device_t *device)
{
    if (!device) {
        return HARDWARE_GPU_ERROR_INVALID_PARAM;
    }
    return gpu_status_from_hal(hal_gpu_close(device));
}

/**
 * @brief Copy image.
 */
static int gpu_device_image_copy(struct gpu_device *device,
                                 struct image_desc *pInputImage,
                                 struct image_desc *pOutputImage)
{
    if (!device || !pInputImage || !pOutputImage) {
        return HARDWARE_GPU_ERROR_INVALID_PARAM;
    }
    return gpu_status_from_hal(hal_gpu_image_copy(device, pInputImage, pOutputImage));
}

/**
 * @brief Scale image.
 */
static int gpu_device_image_scale(struct gpu_device *device,
                                  struct image_desc *pInputImage,
                                  struct image_desc *pOutputImage)
{
    if (!device || !pInputImage || !pOutputImage) {
        return HARDWARE_GPU_ERROR_INVALID_PARAM;
    }
    return gpu_status_from_hal(hal_gpu_image_scale(device, pInputImage, pOutputImage));
}

/**
 * @brief Transform image.
 */
static int gpu_device_image_transform(struct gpu_device *device,
                                      struct image_desc *pInputImage,
                                      struct image_desc *pOutputImage,
                                      transform_t transform)
{
    if (!device || !pInputImage || !pOutputImage) {
        return HARDWARE_GPU_ERROR_INVALID_PARAM;
    }
    return gpu_status_from_hal(hal_gpu_image_transform(device, pInputImage,
                                                        pOutputImage, transform));
}

/**
 * @brief Blend images.
 */
static int gpu_device_image_blend(struct gpu_device *device,
                                  struct image_desc *pForeGround,
                                  struct image_desc *pBackGround,
                                  struct image_desc *pOutputImage,
                                  struct blend_config *config)
{
    if (!device || !pForeGround || !pBackGround || !pOutputImage || !config) {
        return HARDWARE_GPU_ERROR_INVALID_PARAM;
    }
    return gpu_status_from_hal(hal_gpu_image_blend(device, pForeGround,
                                                    pBackGround, pOutputImage, config));
}

/**
 * @brief Blend image with color key.
 */
static int gpu_device_image_blend_colorkey(struct gpu_device *device,
                                           struct image_desc *pInputImage,
                                           struct image_desc *pOutputImage,
                                           struct image_colorkey *colorkey)
{
    if (!device || !pInputImage || !pOutputImage || !colorkey) {
        return HARDWARE_GPU_ERROR_INVALID_PARAM;
    }
    return gpu_status_from_hal(hal_gpu_image_blend_colorkey(device, pInputImage,
                                                            pOutputImage, colorkey));
}

/**
 * @brief Fill image with color.
 */
static int gpu_device_image_fill(struct gpu_device *device,
                                 struct image_desc *pOutputImage,
                                 uint32_t color)
{
    if (!device || !pOutputImage) {
        return HARDWARE_GPU_ERROR_INVALID_PARAM;
    }
    return gpu_status_from_hal(hal_gpu_image_fill(device, pOutputImage, color));
}

/**
 * @brief Check input format support.
 */
static bool gpu_device_check_input_format(pixel_format_t format)
{
    return hal_gpu_check_input_format(format);
}

/**
 * @brief Check output format support.
 */
static bool gpu_device_check_output_format(pixel_format_t format)
{
    return hal_gpu_check_output_format(format);
}

/**
 * @brief Get minimum supported width.
 */
static int32_t gpu_device_supported_min_width(void)
{
    return hal_gpu_supported_min_width();
}

/**
 * @brief Get minimum supported height.
 */
static int32_t gpu_device_supported_min_height(void)
{
    return hal_gpu_supported_min_height();
}

/*===========================================================================
 * Module Implementation
 *===========================================================================*/

/**
 * @brief Open GPU device.
 * @details Initializes GPU HAL and creates a gpu_device instance.
 * @param module  Pointer to the GPU hardware module.
 * @param name    Device name, must be GPU_DEVICE_NAME.
 * @param device  Output pointer to the opened device handle.
 * @return 0 on success, negative error code on failure.
 */
int hw_gpu_open(const hw_module_t *module, const char *name, hw_device_t **device)
{
    *device = NULL;

    if ((NULL == module) || (NULL == name) || (NULL == device)) {
        log_error("Invalid parameter");
        return HARDWARE_GPU_ERROR_INVALID_PARAM;
    }

    if (0 != strcmp(name, GPU_DEVICE_NAME)) {
        log_error("Unsupported device: %s", name);
        return HARDWARE_GPU_ERROR_NOT_SUPPORTED;
    }

    /* Initialize HAL (AV100_V2 may lack ak_ipu.h — operations stay stubbed) */
    if (hal_gpu_open() != 0) {
        log_warn("hal_gpu_open failed; IPU may be unavailable on this SoC");
    }

    /* Allocate device structure */
    struct gpu_device *dev = calloc(1, sizeof(struct gpu_device));
    if (NULL == dev) {
        log_error("Failed to allocate device structure");
        return HARDWARE_GPU_ERROR_MEMORY;
    }

    /* Initialize device structure */
    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = GPU_DEVICE_API_VERSION_1_0;
    dev->common.module = (struct hw_module_t *)module;
    dev->common.close = gpu_device_close;

    /* Assign device methods */
    dev->image_copy = gpu_device_image_copy;
    dev->image_scale = gpu_device_image_scale;
    dev->image_transform = gpu_device_image_transform;
    dev->image_blend = gpu_device_image_blend;
    dev->image_blend_colorkey = gpu_device_image_blend_colorkey;
    dev->image_fill = gpu_device_image_fill;
    dev->check_input_format = gpu_device_check_input_format;
    dev->check_output_format = gpu_device_check_output_format;
    dev->supported_min_width = gpu_device_supported_min_width;
    dev->supported_min_height = gpu_device_supported_min_height;

    *device = &dev->common;

    return HARDWARE_GPU_OK;
}

/*===========================================================================
 * Module Definition
 *===========================================================================*/

static struct hw_module_methods_t hw_gpu_module_methods = {
    .open = hw_gpu_open,
};

static struct gpu_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = GPU_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = GPU_HARDWARE_MODULE_ID,
        .name = "GPU Module",
        .author = "Anyka",
        .methods = &hw_gpu_module_methods,
    },
};

/*===========================================================================
 * Public Functions
 *===========================================================================*/

/**
 * @brief Initialize GPU module.
 * @details Registers the GPU module with the hardware module system.
 * @return 0 on success, negative error code on failure.
 */
int hw_gpu_module_init(void)
{
    int ret = hw_register_module(&(HAL_MODULE_INFO_SYM.common));
    return (ret == 0) ? HARDWARE_GPU_OK : HARDWARE_GPU_ERROR_OPERATION_FAILED;
}

