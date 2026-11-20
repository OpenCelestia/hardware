// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/camera.
 *
 * Runtime configuration for camera HAL (chip + product tuning).
 * INI schema matches AnyCloud39AV200 so camera_hw / hal_camera_api stay one contract; V2 HAL
 * fills PDK types (SVP2) from strings. Tune VI/ISP using PDK ak_vi_sample.c as reference;
 * tune AI paths and model_type using platform/sample/svp2/ak_svp2_sample.c.
 */

#ifndef HARDWARE_HAL_CAMERA_CONFIG_HAL_ANYCLOUD39AV100_V2_HAL_CAMERA_CONFIG_H
#define HARDWARE_HAL_CAMERA_CONFIG_HAL_ANYCLOUD39AV100_V2_HAL_CAMERA_CONFIG_H

#include <stdint.h>

#include "hal_camera_chip_config.h"
#include "hal_camera.h"
#include "ak_svp2.h"

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

    int32_t ipu_que_id;                     /**< IPU queue ID */
    int32_t ipu_module_id;                  /**< IPU module ID */

    /** 1: VPSS channels created on first enable; 0: all channels at configure_streams */
    int32_t lazy_channel_create;

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

/**
 * @brief Map a VI_DATA_TYPE_E value to the SVP2 image type used by AI models.
 * @details YUV420SP stays YUV420SP; every other VI source is treated as RGB
 *          line-interlaced (SVP2 only supports YUV420SP / RGB_LI).
 *          Shared by INI parsing and the per-frame AI process path.
 */
AK_SVP2_IMG_TYPE_E hal_camera_svp_img_from_vi_data_type(int vi_data_type);

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_HAL_CAMERA_CONFIG_HAL_ANYCLOUD39AV100_V2_HAL_CAMERA_CONFIG_H */


