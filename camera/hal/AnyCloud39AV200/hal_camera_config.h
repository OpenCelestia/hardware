// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/camera.
 *
 * Runtime configuration for camera HAL (chip + product config).
 * - Combines legacy hal_camera_chip_config.h / hal_camera_product_config.h
 *   into a single INI-driven configuration, similar to audio_config.ini.
 */

#ifndef HARDWARE_HAL_CAMERA_CONFIG_HAL_ANYCLOUD39AV200_HAL_CAMERA_CONFIG_H
#define HARDWARE_HAL_CAMERA_CONFIG_HAL_ANYCLOUD39AV200_HAL_CAMERA_CONFIG_H

#include <stdint.h>

#include "hal_camera_chip_config.h"
#include "hal_camera.h"

/* AF defaults shared by config loader and AF runtime. */
#define AF_ENABLE_DEFAULT            0
#define FOCUS_POS_MIN_DEFAULT        0
#define FOCUS_POS_MAX_DEFAULT        64
#define AF_SETTLE_MS_DEFAULT         20
/** 0 = user hill-climb, 1 = kernel af_ctrl (default when enable=1) */
#define AF_RUN_MODE_USER             0
#define AF_RUN_MODE_KERNEL           1
#define AF_RUN_MODE_DEFAULT          AF_RUN_MODE_KERNEL

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HAL camera configuration structure.
 */
typedef struct {
    int32_t camera_number;                 /**< Number of cameras */

    int32_t interface_mode[CAMERA_MAX_NUM];  /**< Interface mode array */
    int32_t max_fps[CAMERA_MAX_NUM];        /**< Maximum FPS array */
    int32_t mirror[CAMERA_MAX_NUM];          /**< Mirror configuration array */
    int32_t flip[CAMERA_MAX_NUM];            /**< Flip configuration array */
    int32_t rotation[CAMERA_MAX_NUM];        /**< Software IPU rotation in degrees: 0/90/180/270 (default 0 = off) */

    int32_t ipu_que_id;                     /**< IPU queue ID */
    int32_t ipu_module_id;                  /**< IPU module ID */

    /** 1: VPSS channels created on first enable; 0: all channels at configure_streams */
    int32_t lazy_channel_create;

    /** AF/VCM (opt-in): missing [af] section or enable=0 → disabled; other products unchanged */
    int32_t af_enable;
    int32_t af_run_mode;                   /**< AF_RUN_MODE_USER / AF_RUN_MODE_KERNEL */
    int32_t focus_pos_min;
    int32_t focus_pos_max;
    int32_t af_settle_ms;
    char vcm_type[16];
    char vcm_subdev_path[128];

    /* Per-camera ISP config path, indexed by device_id */
    char isp_config_path[CAMERA_MAX_NUM][256];  /**< ISP configuration path array */
} hal_camera_config_t;

/**
 * @brief AI detect type ↔ sub-type mapping (from [ai_detect_subN] sections).
 *        Mirrors legacy AI_TYPE_DETECT_SUB array but loaded from INI.
 */
typedef hal_camera_ai_type_detect_sub_t hal_camera_ai_type_detect_sub_cfg_t;

/**
 * @brief AI model configuration (from [ai_modelN] sections).
 *        Mirrors legacy MODLE_CONFIG_SUPPORT array but loaded from INI.
 */
typedef hal_camera_ai_modle_config_support_t hal_camera_ai_modle_config_cfg_t;

/**
 * @brief Get camera HAL configuration from INI file.
 * @return Pointer to static configuration struct (never NULL).
 *         On error, returns struct filled with built-in defaults.
 */
const hal_camera_config_t *hal_camera_get_config(void);

/**
 * @brief Get AI detect ↔ sub-type mapping table.
 * @param num_entries_out Optional pointer to receive number of entries.
 * @return Pointer to internal static array; lifetime is entire process.
 */
const hal_camera_ai_type_detect_sub_cfg_t *
hal_camera_get_ai_detect_sub_table(int *num_entries_out);

/**
 * @brief Get AI model configuration table.
 * @param num_models_out Optional pointer to receive number of models.
 * @return Pointer to internal static array; lifetime is entire process.
 */
const hal_camera_ai_modle_config_cfg_t *
hal_camera_get_ai_model_table(int *num_models_out);

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_HAL_CAMERA_CONFIG_HAL_ANYCLOUD39AV200_HAL_CAMERA_CONFIG_H */


