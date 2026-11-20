// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/vision/motion_detect.
 *
 * Motion detect INI configuration helpers (internal to vision layer).
 * - Declares helpers for loading and saving motion detection configuration from/to INI files.
 * - Used by motion_detect_hw.c and should not be included by upper layers directly.
 */

#ifndef HARDWARE_MOTION_DETECT_CONFIG_H
#define HARDWARE_MOTION_DETECT_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#include "motion_detect.h"

/* Default area range for motion detect configuration. */
#define MOTION_DETECT_AREA_MIN 0
#define MOTION_DETECT_AREA_MAX 10000

/* Configuration file name for motion detection settings. */
#define MOTION_DETECT_SETTING_FILE "hal_md_config.ini"

/**
 * @brief Load motion detection configuration from INI file.
 *
 * @param config           Output configuration structure, must not be NULL.
 * @param sensitivity_table Output sensitivity mapping table (5 elements).
 * @return HARDWARE_MOTION_DETECT_OK on success, error code otherwise.
 */
int motion_detect_config_load_from_ini(motion_detect_config_t *config,
                                       double sensitivity_table[5]);

/**
 * @brief Save motion detection configuration to INI file.
 *
 * @param config Configuration to be saved, must not be NULL.
 * @return HARDWARE_MOTION_DETECT_OK on success, error code otherwise.
 */
int motion_detect_config_save_to_ini(const motion_detect_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_MOTION_DETECT_CONFIG_H */


