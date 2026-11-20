// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/vision/motion_detect.
 *
 * Motion detect vision sub-module internal declarations.
 * - Public header: hardware/include/motion_detect.h
 * - This header: used only inside vision layer (motion_detect_hw.c) and HAL API layer
 */

#ifndef HARDWARE_MOTION_DETECT_HW_H
#define HARDWARE_MOTION_DETECT_HW_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "motion_detect.h"
#include "gthread.h"

/**
 * @brief Motion detect internal runtime data
 * @details Extracted from the original motion_detect.c implementation.
 */
typedef struct {
    motion_detect_config_t config;  /**< Current motion detect configuration */
    float filter_ratio;             /**< Filter ratio derived from sensitivity */
    int day_night_status;           /**< -1=unknown, 0=day, 1=night */
    int md_vsize;                   /**< MD vertical dimension (line count) */
    GMutex mutex;                   /**< Mutex protecting configuration */
    int initialized;                /**< Initialization flag */

    motion_detect_callback_t callback; /**< Result callback */
    void *user_data;                   /**< User data passed to callback */
    
    motion_detect_day_night_callback_t day_night_callback; /**< Day/night status callback */
} motion_detect_data_t;

/**
 * @brief Motion detection rectangle (HAL API abstraction coordinate space)
 * @details Used by HAL API layer to communicate detection boxes.
 */
typedef struct {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
} hal_motion_detect_box_api_t;

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_MOTION_DETECT_HW_H */


