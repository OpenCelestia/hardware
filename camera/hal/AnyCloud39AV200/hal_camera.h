// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/camera.
 *
 * HAL camera implementation header for AnyCloud39AV200 platform.
 * - Defines platform-specific camera HAL structures and types.
 * - Contains device, channel, codec, and AI configuration structures.
 */

#ifndef HARDWARE_HAL_CAMERA_HAL_ANYCLOUD39AV200_HAL_CAMERA_H
#define HARDWARE_HAL_CAMERA_HAL_ANYCLOUD39AV200_HAL_CAMERA_H

#include <stdint.h>
#include <stddef.h>

#include "../../camera_hw.h"
#include "camera.h"
#include "ak_vi.h"
#include "ak_vpss.h"
#include "ak_svp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration for IPU scale handle to avoid direct dependency */
typedef struct hal_camera_ipu_scale_handle hal_camera_ipu_scale_handle_t;

/**
 * @brief Camera codec structure.
 */
typedef struct {
    bool stream_buffer_release_flag;    /**< Stream buffer release flag */
    VIDEO_STREAM_S *pstStream;          /**< Video stream pointer */
    int frame_type;                     /**< Latest frame type from encoder (FRAME_TYPE_P=0, I=1, PI=2) */
} hal_camera_codec_t;


/**
 * @brief Camera channel structure.
 */
typedef struct {
    int channel_buffer_release_flag;    /**< Channel buffer release flag */
    VPSS_FRAME_INFO_S *pstFrame;       /**< Frame data for AI detection */

    /* Software IPU rotation state (0 rotation = all fields unused). */
    int rotation;                       /**< 0/90/180/270; 0 = no rotation for this channel */
    int rot_ipu_color_fmt;              /**< IPU_COLOR_FMT_E for the VPSS output of this channel */
    int rot_ipu_opened;                 /**< 1 if this channel holds an hal_ipu_open() reference */
    unsigned int rot_src_w;             /**< VPSS output width (IPU input) */
    unsigned int rot_src_h;             /**< VPSS output height (IPU input) */
    unsigned int rot_dst_w;             /**< rotated output width (IPU output, = configured width) */
    unsigned int rot_dst_h;             /**< rotated output height (IPU output, = configured height) */
    void *rot_buf[2];                   /**< ping-pong DMA buffers holding rotated frames (virtual) */
    uint64_t rot_buf_phy[2];            /**< physical addresses of rot_buf */
    int rot_buf_idx;                    /**< index of the buffer used by the last delivered frame */
    size_t rot_buf_len;                 /**< allocated size of each rot_buf */
    int frame_rotated;                  /**< 1 if pstFrame currently points at a rotated buffer */
    VPSS_FRAME_INFO_S vpss_frame_saved; /**< original VPSS frame info, restored before ReleaseFrame */

    /* Detection results saved for AI recognition */
    int num_ai_recognize;               /**< Number of AI recognition results */
    camera_notify_msg_t ai_detect_result_for_recognize[CAMERA_AI_MAX_RECOGNIZE_PER_STREAM];  /**< AI detection results for recognition */
    int ai_detect_modle_id_for_recognize[CAMERA_AI_MAX_RECOGNIZE_PER_STREAM];  /**< AI detection model IDs for recognition */

    hal_camera_codec_t codec[CAMERA_MAX_STREAM_PER_CHANNEL];  /**< Codec array for each stream */
} hal_camera_channel_t;

void hal_camera_codec_disable_channel(void *hal_camera, int device_id, int channel_id);

/**
 * @brief Camera device structure.
 */
typedef struct {
    int vi_channel_id;                  /**< VI channel ID */
    int vpss_group_id;                  /**< VPSS group ID */

    int flip_enabled;                   /**< Flip enabled flag */
    int mirror_enabled;                 /**< Mirror enabled flag */

    hal_camera_channel_t channel[CAMERA_MAX_CHANNEL];  /**< Channel array */
} hal_camera_device_t;



/**
 * @brief AI type detect and sub-type mapping structure.
 * @details Used for hal_camera_product_config.h.
 */
typedef struct hal_camera_ai_type_detect_sub {
    camera_ai_type_t detect_type;      /**< Detection type */
    camera_ai_type_t sub_type;          /**< Sub type */
} hal_camera_ai_type_detect_sub_t;

/**
 * @brief AI model configuration support structure.
 * @details Used for hal_camera_product_config.h.
 */
typedef struct hal_camera_ai_modle_config_support {
    char modle_name[64];                /**< Model name */
    int width;                          /**< Model input width */
    int height;                         /**< Model input height */
    SVP_MODEL_E enModelType;            /**< Model type */
    SVP_IMG_TYPE_E enImgType;           /**< Image type */

    int num_camera_ai_modle;            /**< Number of recognition models */
    camera_ai_type_t ai_type[CAMERA_AI_MAX_LABLE_PER_MODEL];  /**< AI types array (separated for main class + sub class display) */
} hal_camera_ai_modle_config_support_t;


/**
 * @brief Detection model information structure.
 */
typedef struct hal_camera_modle_detect {
    camera_stream_t *streams;       /**< Stream pointer (one model uses one stream, multiple algorithms using one model share the same stream) */
} hal_camera_modle_detect_t;

/**
 * @brief Recognition model information structure.
 */
typedef struct hal_camera_modle_secondary {
    void *output_buffer;                         /**< Recognition algorithm buffer */
    uint64_t output_buffer_phy_addr;            /**< Recognition algorithm buffer physical address */
} hal_camera_modle_secondary_t;

/**
 * @brief Camera AI model structure.
 */
typedef struct hal_camera_modle {
    camera_ai_type_t ai_type;   /**< AI algorithm enabled in this model (0 disables the model) */
    int ai_id;                  /**< Registered ID */
    const hal_camera_ai_modle_config_support_t *support_modle;  /**< Associated model information */
    SVP_IMG_INFO_S stInput;        /**< Image information for detection */
    SVP_CONF_INFO_S conf_info;     /**< Model information */
    SVP_CHN_ATTR_S *attr;          /**< Channel attributes */

    union {
        hal_camera_modle_detect_t detect;         /**< Detection config */
        hal_camera_modle_secondary_t secondary;    /**< Recognition config */
    };
} hal_ai_modle_t;



/**
 * @brief Camera AI configuration structure.
 */
typedef struct hal_camera_ai_config {
    camera_ai_type_t camera_ai_type_flag;  /**< Camera AI type flag */

    int num_modle;                         /**< Number of models to enable simultaneously */
    hal_ai_modle_t modle[CAMERA_AI_MAX_MODLE];  /**< Model array */

    camera_status_type_t status;            /**< Status (CAMERA_STATUS_UNINITIALIZE, CAMERA_STATUS_ENABLE) */

    camera_notify_msg_t ai_result;         /**< AI result */
} hal_camera_ai_config_t;


/**
 * @brief Camera AI thread information structure.
 */
typedef struct hal_camera_ai_thread_info {
    struct camera_device *cdev;             /**< Camera device pointer */
    camera_stream_t *stream;                /**< Camera stream pointer */
} hal_camera_ai_thread_info_t;

/**
 * @brief HAL camera data structure.
 */
struct hal_camera_data {
    struct camera_device *cdev;             /**< Camera device pointer */

    hal_camera_device_t hal_camera_device[CAMERA_MAX_NUM];  /**< HAL camera device array */

    hal_camera_ai_config_t ai_config;      /**< AI configuration */
    hal_camera_ai_thread_info_t ai_thread_info;  /**< AI thread information */

    camera_ai_type_t ai_type;               /**< All enabled algorithms */
    int codec_id_status[CAMERA_CODEC_MAX_NUM];  /**< Codec ID status array */
    int ai_id_status[CAMERA_AI_MAX_MODLE];     /**< AI ID status array */
};







#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_HAL_CAMERA_HAL_ANYCLOUD39AV200_HAL_CAMERA_H */
