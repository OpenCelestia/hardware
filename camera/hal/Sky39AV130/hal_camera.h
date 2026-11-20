// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/camera.
 *
 * HAL camera implementation header for AnyCloud39AV100_V2 platform.
 * - Defines platform-specific camera HAL structures and types.
 * - Contains device, channel, codec, and AI configuration structures.
 */

#ifndef HARDWARE_HAL_CAMERA_HAL_ANYCLOUD39AV100_V2_HAL_CAMERA_H
#define HARDWARE_HAL_CAMERA_HAL_ANYCLOUD39AV100_V2_HAL_CAMERA_H

#include "../../camera_hw.h"
#include "camera.h"
#include "ak_vpss.h"
#include "ak_svp2.h"
/* VI helper types/functions are part of hal_camera.h/.c on this chip */
#include "ak_common.h"
#include "ak_vi.h"
#include "system_pixel_format.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration for IPU scale handle to avoid direct dependency */
typedef struct hal_camera_ipu_scale_handle hal_camera_ipu_scale_handle_t;

/**
 * @brief Camera codec structure.
 */
struct video_stream;

struct video_input_frame;

/**
 * Frame descriptor shared with legacy HAL/AI code (formerly VPSS_FRAME_INFO_S).
 * Populated from ak_vi_get_frame() via hal_camera_export_frame().
 */
typedef struct hal_camera_frame_buf {
    struct {
        unsigned char *pu8Data;
        unsigned int u32Len;
        unsigned long long u64Ts;
        unsigned long long u64SeqNo;
    } stFrame;
    int enOutputPixFmt;
    void *pvMdInfo;
    unsigned long long u64PhyAddr;
} VPSS_FRAME_INFO_S;

VI_CHN hal_camera_vi_chn(int device_id, int hal_channel_id);
VI_DATA_TYPE_E hal_camera_vi_data_type(pixel_format_t fmt);
void hal_camera_export_frame(VPSS_FRAME_INFO_S *dst, const struct video_input_frame *vin);
void hal_camera_codec_disable_channel(void *hal_camera, int device_id, int channel_id);

/*
 * Kernel-encode (VI SLICE_MODE) helpers, implemented in hal_camera_codec.c because they
 * need ak_video.h and the per-stream codec io lock.
 *
 * In slice mode the VI channel never hands full YUV frames to userspace: the kernel feeds
 * VI slices straight into the bound encoder, and userspace only pulls the bitstream with
 * ak_app_video_venc_get_stream(). hal_camera_stream_read() therefore must not call
 * ak_vi_get_frame() on such a channel — it calls hal_camera_codec_slice_read() instead.
 */
size_t hal_camera_codec_slice_read(void *hal_camera, int device_id, int channel_id,
                                  void **buffer, uint64_t *timestamp);
/* set_dst_chn_active(0): unblocks a get_stream in flight. Safe on a non-slice channel. */
void hal_camera_codec_slice_deactivate(void *hal_camera, int device_id, int channel_id);
/* Full teardown of the VI->VENC bind (deactivate + unbind). Safe if never bound. */
void hal_camera_codec_slice_unbind(void *hal_camera, int device_id, int channel_id);
/* Re-establish the bind after VI attrs were re-set. No-op unless it was bound before. */
int hal_camera_codec_slice_rebind(void *hal_camera, int device_id, int channel_id);

typedef struct {
    bool stream_buffer_release_flag;    /**< Stream buffer release flag */
    struct video_stream *pstStream;     /**< Encoder output (ak_venc) */
    int codec_slot;                     /**< Index for camera_codec_register_id / unregister */
    int frame_type;                     /**< Latest frame type from encoder (FRAME_TYPE_P=0, I=1, PI=2) */
} hal_camera_codec_t;


/**
 * @brief Camera channel structure.
 */
typedef struct {
    int channel_buffer_release_flag;    /**< Channel buffer release flag */
    int channel_stopping;               /**< 1 while stop path runs (suppress get_frame noise) */
    VPSS_FRAME_INFO_S *pstFrame;       /**< Frame data for AI detection */
    struct video_input_frame pending_vin_frame; /**< Pending VI frame for ak_vi_release_frame */

    /*
     * VI SLICE_MODE / KERNEL_ENCODE_MODE state (num_slices > 1 in the channel config).
     * Only the main channel can run this way, and only one encoder may be bound to a VI
     * channel, so the bind is tracked per channel, not per stream.
     *
     * APP_CHN_S is not stored here on purpose: it lives in ak_video.h, which pulls in
     * ak_venc.h and its `struct rect` (which collides with rect_t elsewhere). The two
     * oids below are all that is needed to rebuild {MODULE_ID_VI, vi_chn} and
     * {MODULE_ID_VENC, handle} inside hal_camera_codec.c.
     */
    int slice_mode;             /**< 1 when ak_vi_set_chn_attr_ex used SLICE_MODE */
    int slice_stream_id;        /**< stream that owns the bind; -1 when none */
    int slice_vi_chn;           /**< src oid (VI_CHN) of the bind */
    int slice_venc_handle;      /**< dst oid (ak_venc handle) of the bind; -1 when none */
    int slice_bound;            /**< 1 between ak_app_video_bind_chn and unbind */
    int slice_active;           /**< 1 between set_dst_chn_active(1) and set_dst_chn_active(0) */
    bool slice_stream_held;     /**< a get_stream result is pending ak_venc_release_stream */

    /* Detection results saved for AI recognition */
    int num_ai_recognize;               /**< Number of AI recognition results */
    camera_notify_msg_t ai_detect_result_for_recognize[CAMERA_AI_MAX_RECOGNIZE_PER_STREAM];  /**< AI detection results for recognition */
    int ai_detect_modle_id_for_recognize[CAMERA_AI_MAX_RECOGNIZE_PER_STREAM];  /**< AI detection model IDs for recognition */

    hal_camera_codec_t codec[CAMERA_MAX_STREAM_PER_CHANNEL];  /**< Codec array for each stream */
} hal_camera_channel_t;


/**
 * @brief Camera device structure.
 */
typedef struct {
    int vi_channel_id;                  /**< VI channel ID */
    int vpss_group_id;                  /**< VPSS group ID */

    int flip_enabled;                   /**< Flip enabled flag */
    int mirror_enabled;                 /**< Mirror enabled flag */

    /** Bitmask: bit N set after ak_vi_set_chn_attr_ex for HAL channel N (0..2). */
    unsigned int vi_hal_chn_registered;

    /** VI_DEV_ATTR sub_max (for PDK; main-only uses scaled sub, no app sub stream). */
    int vi_dev_sub_max_w;
    int vi_dev_sub_max_h;

    /** 1: main-only PDK placeholder ch1 (sub_max); app captures ch0 only. */
    int vi_main_only_extra_vi_ch1;

    /** VI VICAP/ISP working crop (ak_vi_sample -r); may be smaller than channel output (-m). */
    int vicap_crop_w;
    int vicap_crop_h;

    int vi_open;        /**< ak_vi_open succeeded */
    int vi_dev_enabled; /**< ak_vi_enable_dev succeeded */

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
    AK_SVP2_MODEL_E enModelType;        /**< Model type */
    AK_SVP2_IMG_TYPE_E enImgType;       /**< Image type */
    int classify_threshold;             /**< HD classify threshold from INI; <0 = use AiConfig at open */
    int iou_threshold;                  /**< HD IoU threshold from INI; <0 = use AiConfig at open */
    int target_type;                    /**< AK_SVP2_TARGET_TYPE_E from INI; <0 = default HUMAN_SHAPE */

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
    AK_SVP2_IMG_INFO_T stInput;    /**< Image information for detection */
    AK_SVP2_CONF_INFO conf_info;   /**< Model conf (path/buffer) */
    AK_SVP2_CHN_ATTR_T *attr;      /**< Channel attributes */

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

#endif /* HARDWARE_HAL_CAMERA_HAL_ANYCLOUD39AV100_V2_HAL_CAMERA_H */
