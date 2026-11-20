// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/gpu.
 *
 * GPU HAL API interface.
 * - Platform-agnostic HAL interface for GPU operations.
 * - Defines HAL function prototypes for GPU hardware abstraction.
 */

#ifndef HARDWARE_HAL_GPU_API_H
#define HARDWARE_HAL_GPU_API_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/cdefs.h>

#include "gpu.h"

__BEGIN_DECLS

/*===========================================================================
 * HAL API Functions
 *===========================================================================*/

/**
 * @brief Open GPU HAL device
 * @return 0 on success, negative error code on failure
 */
int hal_gpu_open(void);

/**
 * @brief Close GPU HAL device
 * @param device Device handle
 * @return 0 on success, negative error code on failure
 */
int hal_gpu_close(struct hw_device_t *device);

/**
 * @brief Copy image
 * @param device Device context
 * @param pInputImage Input image descriptor
 * @param pOutputImage Output image descriptor
 * @return 0 on success, negative error code on failure
 */
int hal_gpu_image_copy(struct gpu_device *device,
                       struct image_desc *pInputImage,
                       struct image_desc *pOutputImage);

/**
 * @brief Scale image
 * @param device Device context
 * @param pInputImage Input image descriptor
 * @param pOutputImage Output image descriptor
 * @return 0 on success, negative error code on failure
 */
int hal_gpu_image_scale(struct gpu_device *device,
                        struct image_desc *pInputImage,
                        struct image_desc *pOutputImage);

/**
 * @brief Transform image
 * @param device Device context
 * @param pInputImage Input image descriptor
 * @param pOutputImage Output image descriptor
 * @param transform Transform operation
 * @return 0 on success, negative error code on failure
 */
int hal_gpu_image_transform(struct gpu_device *device,
                            struct image_desc *pInputImage,
                            struct image_desc *pOutputImage,
                            transform_t transform);

/**
 * @brief Blend images
 * @param device Device context
 * @param pForeGround Foreground image descriptor
 * @param pBackGround Background image descriptor
 * @param pOutputImage Output image descriptor
 * @param config Blend configuration
 * @return 0 on success, negative error code on failure
 */
int hal_gpu_image_blend(struct gpu_device *device,
                        struct image_desc *pForeGround,
                        struct image_desc *pBackGround,
                        struct image_desc *pOutputImage,
                        struct blend_config *config);

/**
 * @brief Blend image with color key
 * @param device Device context
 * @param pInputImage Input image descriptor
 * @param pOutputImage Output image descriptor
 * @param colorkey Color key configuration
 * @return 0 on success, negative error code on failure
 */
int hal_gpu_image_blend_colorkey(struct gpu_device *device,
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
int hal_gpu_image_fill(struct gpu_device *device,
                       struct image_desc *pOutputImage,
                       uint32_t color);

/**
 * @brief Check if input pixel format is supported
 * @param format Pixel format to check
 * @return true if supported, false otherwise
 */
bool hal_gpu_check_input_format(system_pixel_format_t format);

/**
 * @brief Check if output pixel format is supported
 * @param format Pixel format to check
 * @return true if supported, false otherwise
 */
bool hal_gpu_check_output_format(system_pixel_format_t format);

/**
 * @brief Get minimum supported width
 * @return Minimum width in pixels
 */
int32_t hal_gpu_supported_min_width(void);

/**
 * @brief Get minimum supported height
 * @return Minimum height in pixels
 */
int32_t hal_gpu_supported_min_height(void);

/**
 * @brief General GPU processing operation
 * @details Performs general image processing operation based on input and output descriptors
 * @param device Device context
 * @param pInputImage Input image descriptor
 * @param pOutputImage Output image descriptor
 * @return 0 on success, negative error code on failure
 */
int hal_gpu_general_process(struct gpu_device *device,
                            struct image_desc *pInputImage,
                            struct image_desc *pOutputImage);

__END_DECLS

#endif /* HARDWARE_HAL_GPU_API_H */

