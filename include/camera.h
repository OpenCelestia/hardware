// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/camera.
 *
 * Camera Hardware Abstraction Layer (HAL) public interface.
 * - Provides Android-style HAL interface for camera devices.
 * - Supports stream management, encoding, and AI detection.
 */
#ifndef HARDWARE_CAMERA_H
#define HARDWARE_CAMERA_H

/* Standard C library includes */
#include <stdint.h>
#include <strings.h>
#include <sys/cdefs.h>
#include <sys/types.h>
#include <time.h>
#include <stdbool.h>

/* Project includes */
#include "hardware.h"
#include "system_pixel_format.h"
#include "system_error.h"

__BEGIN_DECLS

/*===========================================================================
 * Module Version Definitions
 *===========================================================================*/

#define CAMERA_MODULE_API_VERSION_1_0      HARDWARE_MODULE_API_VERSION(1, 0)
#define CAMERA_MODULE_API_VERSION_CURRENT  CAMERA_MODULE_API_VERSION_1_0

/** @brief Camera device API version 1.0 (deprecated) */
#define CAMERA_DEVICE_API_VERSION_1_0       HARDWARE_DEVICE_API_VERSION(1, 0)

/*===========================================================================
 * Module ID and Device Definitions
 *===========================================================================*/

/** @brief Camera hardware module ID */
#define CAMERA_HARDWARE_MODULE_ID          "camera"

/** @brief Camera hardware interface name */
#define CAMERA_HARDWARE_INTERFACE          "camera_hw_if"

/*===========================================================================
 * Type Definitions
 *===========================================================================*/

/**
 * @brief Camera rectangle structure
 * @details Defines a rectangular region with position and dimensions
 * @note Uses x, y, width, height to match system/core rect_t convention
 */
typedef struct  {
    int32_t x;         /**< X coordinate of the rectangle */
    int32_t y;         /**< Y coordinate of the rectangle */
    int32_t width;     /**< Width of the rectangle */
    int32_t height;    /**< Height of the rectangle */
} camera_rect_t;

/**
 * @brief Camera stream rotation enumeration
 * @details Defines the required counterclockwise rotation of camera stream
 */
typedef enum camera_rotation {
    CAMERA_ROTATION_0 = 0,     /**< No rotation */
    CAMERA_ROTATION_90 = 1,    /**< Rotate by 90 degrees counterclockwise */
    CAMERA_ROTATION_180 = 2,   /**< Rotate by 180 degrees counterclockwise */
    CAMERA_ROTATION_270 = 3    /**< Rotate by 270 degrees counterclockwise */
} camera_stream_rotation_t;

/**
 * @brief Camera codec mode enumeration
 * @details Defines supported video codec formats
 */
typedef enum camera_codec_mode {
    CAMERA_CODEC_MODE_AVC,     /**< H.264/AVC codec */
    CAMERA_CODEC_MODE_HEVC,    /**< H.265/HEVC codec */
    CAMERA_CODEC_MODE_MJPEG,   /**< Motion JPEG codec */
    CAMERA_CODEC_MODE_YUV,     /**< YUV uncompressed format */
    CAMERA_CODEC_MODE_RAW      /**< Raw sensor data format */
} camera_codec_mode_t;

/**
 * @brief Camera codec profile enumeration
 * @details Defines codec profile configurations for different compression standards
 */
typedef enum camera_codec_profile {
    /* AVC/H.264 profiles */
    AVC_PROFILE_BASE,          /**< AVC baseline profile */
    AVC_PROFILE_MAIN,          /**< AVC main profile */
    AVC_PROFILE_HIGH,          /**< AVC high profile */
    AVC_PROFILE_C_BASE,        /**< AVC constrained baseline profile */

    /* HEVC/H.265 profiles */
    HEVC_PROFILE_MAIN,         /**< HEVC main profile */
    HEVC_PROFILE_MAIN_STILL,   /**< HEVC main still picture profile */
    HEVC_PROFILE_MAIN_INTRA,   /**< HEVC main intra profile */
    HEVC_PROFILE_MAIN10_HDR10, /**< HEVC main 10-bit HDR10 profile */
    HEVC_PROFILE_MAIN10_HDR10Plus, /**< HEVC main 10-bit HDR10+ profile */

    JPEG_PROFILE,              /**< JPEG profile */
} camera_codec_profile_t;

/**
 * @brief Camera codec level enumeration
 * @details Defines codec level configurations for different performance tiers
 */
typedef enum camera_codec_level {
    /* AVC levels */
    AVC_LEVEL_1     = 10,      /**< AVC level 1.0 */
    AVC_LEVEL_11    = 11,      /**< AVC level 1.1 */
    AVC_LEVEL_12    = 12,      /**< AVC level 1.2 */
    AVC_LEVEL_13    = 13,      /**< AVC level 1.3 */
    AVC_LEVEL_2     = 20,      /**< AVC level 2.0 */
    AVC_LEVEL_21    = 21,      /**< AVC level 2.1 */
    AVC_LEVEL_22    = 22,      /**< AVC level 2.2 */
    AVC_LEVEL_3     = 30,      /**< AVC level 3.0 */
    AVC_LEVEL_31    = 31,      /**< AVC level 3.1 */
    AVC_LEVEL_32    = 32,      /**< AVC level 3.2 */
    AVC_LEVEL_4     = 40,      /**< AVC level 4.0 */
    AVC_LEVEL_41    = 41,      /**< AVC level 4.1 */
    AVC_LEVEL_42    = 42,      /**< AVC level 4.2 */
    AVC_LEVEL_5     = 50,      /**< AVC level 5.0 */
    AVC_LEVEL_51    = 51,      /**< AVC level 5.1 */
    AVC_LEVEL_52    = 52,      /**< AVC level 5.2 */
    AVC_LEVEL_6     = 60,      /**< AVC level 6.0 */
    AVC_LEVEL_61    = 61,      /**< AVC level 6.1 */
    AVC_LEVEL_62    = 62,      /**< AVC level 6.2 */

    /* HEVC levels */
    HEVC_LEVEL_1    = 10,      /**< HEVC level 1.0 */
    HEVC_LEVEL_2    = 20,      /**< HEVC level 2.0 */
    HEVC_LEVEL_21   = 21,      /**< HEVC level 2.1 */
    HEVC_LEVEL_3    = 30,      /**< HEVC level 3.0 */
    HEVC_LEVEL_31   = 31,      /**< HEVC level 3.1 */
    HEVC_LEVEL_4    = 40,      /**< HEVC level 4.0 */
    HEVC_LEVEL_41   = 41,      /**< HEVC level 4.1 */
    HEVC_LEVEL_5    = 50,      /**< HEVC level 5.0 */
    HEVC_LEVEL_51   = 51,      /**< HEVC level 5.1 */
    HEVC_LEVEL_52   = 52,      /**< HEVC level 5.2 */
    HEVC_LEVEL_6    = 60,      /**< HEVC level 6.0 */
    HEVC_LEVEL_61   = 61,      /**< HEVC level 6.1 */
    HEVC_LEVEL_62   = 62,      /**< HEVC level 6.2 */
} camera_codec_level_t;

/**
 * @brief Camera codec configuration structure
 * @details Contains codec parameters for video encoding
 */
typedef struct camera_codec_param
{
    camera_codec_mode_t codec_mode;    /**< Codec format type */
    camera_codec_profile_t profile;    /**< Codec profile */
    camera_codec_level_t level;        /**< Codec level */
} camera_codec_config_t;

/**
 * @brief Rate control mode enumeration
 * @details Defines different rate control strategies for video encoding
 */
typedef enum camera_rc_mode {
    CAMERA_CODEC_RC_MODE_CBR,      /**< Constant bitrate mode */
    CAMERA_CODEC_RC_MODE_VBR,      /**< Variable bitrate mode */
    CAMERA_CODEC_RC_MODE_AVBR,     /**< Adaptive variable bitrate mode */
    CAMERA_CODEC_RC_MODE_CVBR,     /**< Constrained variable bitrate mode */
    CAMERA_CODEC_RC_MODE_FIXQP,    /**< Fixed quantization parameter mode */
    CAMERA_CODEC_RC_MODE_QPMAP     /**< QP map mode for region-based quality control */
} camera_rc_mode_t;

/**
 * @brief CBR rate control parameters
 * @details Configuration for constant bitrate encoding
 */
typedef struct camera_rc_cbr_paramter
{
    uint16_t target_bitrate;       /**< Target bitrate in kbps */
} camera_rc_cbr_t;

/**
 * @brief VBR rate control parameters
 * @details Configuration for variable bitrate encoding
 */
typedef struct camera_rc_vbr_param
{
    uint16_t max_bitrate;          /**< Maximum bitrate in kbps */
    uint16_t max_qp;               /**< Maximum quantization parameter */
    uint16_t min_qp;               /**< Minimum quantization parameter */
    uint16_t change_pos;           /**< Bitrate change position */
} camera_rc_vbr_t;

/**
 * @brief AVBR rate control parameters
 * @details Configuration for adaptive variable bitrate encoding
 */
typedef struct camera_rc_avbr_paramter
{
    uint16_t max_bitrate;          /**< Maximum bitrate in kbps */
    uint16_t change_pos;           /**< Bitrate change position */
    uint16_t minstillpercent;      /**< Minimum still percentage for static scenes */
} camera_rc_avbr_t;

/**
 * @brief CVBR rate control parameters
 * @details Configuration for constrained variable bitrate encoding
 */
typedef struct camera_rc_cvbr_paramter
{
    uint16_t max_bitrate;          /**< Maximum bitrate in kbps */
    uint16_t max_qp;               /**< Maximum quantization parameter */
    uint16_t min_qp;               /**< Minimum quantization parameter */
    uint16_t change_pos;           /**< Bitrate change position */
    uint16_t min_psnr;             /**< Minimum PSNR value */
    uint16_t max_psnr;             /**< Maximum PSNR value */
} camera_rc_cvbr_t;

/**
 * @brief Fixed QP rate control parameters
 * @details Configuration for fixed quantization parameter encoding
 */
typedef struct camera_rc_fixqp_parameter
{
    uint16_t qp;                   /**< Fixed quantization parameter value */
} camera_rc_fixqp_t;

/**
 * @brief QP map rate control parameters
 * @details Configuration for region-based quality control
 */
typedef struct camera_rc_qp_map_paramter
{
    uint16_t roi_number;           /**< Number of regions of interest */
    uint32_t *quality;             /**< Quality values for each ROI */
    camera_rect_t *rect;           /**< Rectangle definitions for each ROI */
} camera_rc_qp_map_t;

/**
 * @brief AVC rate control configuration
 * @details Complete rate control settings for H.264/AVC encoding
 */
typedef struct camera_rc_avc_paramter{
    camera_rc_mode_t rc_mode;      /**< Rate control mode */

    union
    {
        camera_rc_cbr_t cbr;       /**< CBR parameters */
        camera_rc_vbr_t vbr;       /**< VBR parameters */
        camera_rc_avbr_t avbr;     /**< AVBR parameters */
        camera_rc_cvbr_t cvbr;     /**< CVBR parameters */
        camera_rc_fixqp_t fixqp;   /**< Fixed QP parameters */
        camera_rc_qp_map_t qpmap;  /**< QP map parameters */
    };
} camera_rc_avc_t;

/**
 * @brief HEVC rate control configuration
 * @details Complete rate control settings for H.265/HEVC encoding
 */
typedef struct camera_rc_hevc_paramter{
    camera_rc_mode_t rc_mode;      /**< Rate control mode */

    union
    {
        camera_rc_cbr_t cbr;       /**< CBR parameters */
        camera_rc_vbr_t vbr;       /**< VBR parameters */
        camera_rc_avbr_t avbr;     /**< AVBR parameters */
        camera_rc_cvbr_t cvbr;     /**< CVBR parameters */
        camera_rc_fixqp_t fixqp;   /**< Fixed QP parameters */
        camera_rc_qp_map_t qpmap;  /**< QP map parameters */
    };
} camera_rc_hevc_t;

/**
 * @brief MJPEG rate control configuration
 * @details Quality settings for Motion JPEG encoding
 */
typedef struct camera_rc_mjpeg_paramter{
    int quality;                   /**< JPEG quality value (0-100) */
    int max_bitrate;               /**< kbps; ak_venc_open_ex rejects 0 */
} camera_rc_mjpeg_t;

/**
 * @brief Rate control configuration union
 * @details FW fills one member (avc, hevc, or mjpeg) from JSON.
 */
typedef union camera_rc_config
{
    camera_rc_avc_t avc;           /**< AVC rate control settings */
    camera_rc_hevc_t hevc;         /**< HEVC rate control settings */
    camera_rc_mjpeg_t mjpeg;       /**< MJPEG rate control settings */
} camera_rc_config_t;

/**
 * @brief GOP configuration structure
 * @details Group of Pictures settings for video encoding
 */
typedef struct camera_gop_config
{
    uint16_t gop_ctrl_mode;        /**< GOP control mode */
    uint16_t gop_len;              /**< GOP length in frames */
    uint16_t enable_lt;            /**< Enable long-term reference */
    uint16_t freq_lt;              /**< Long-term reference frequency */
    uint16_t ltrc;                 /**< Long-term reference count */
} camera_gop_config_t;

/**
 * @brief Camera stream format enumeration
 * @details Defines how multiple camera streams are combined
 */
typedef enum camera_stream_format {
    CAMERA_STREAM_FORMAT_SINGLE       = 0x00,    /**< Single camera stream */
    CAMERA_STREAM_FORMAT_SPLICE_VER   = 0x1,     /**< Vertical splice of multiple cameras */
    CAMERA_STREAM_FORMAT_SPLICE_HOR   = 0x2,     /**< Horizontal splice of multiple cameras */
} camera_stream_format_t;

/**
 * @brief Camera device ID enumeration
 * @details Identifies different camera devices in the system
 */
typedef enum camera_id {
    CAMERA_ID_1       = 0x00,      /**< Camera device 1 */
    CAMERA_ID_2       = 0x01,      /**< Camera device 2 */
    CAMERA_ID_3       = 0x02,      /**< Camera device 3 */
    CAMERA_ID_4       = 0x03,      /**< Camera device 4 */
    CAMERA_ID_5       = 0x04,      /**< Camera device 5 */
    CAMERA_ID_MAX     = 0x05,      /**< Maximum camera device count */
} camera_id_t;

/**
 * @brief Camera channel ID enumeration
 * @details Identifies different video processing channels
 */
typedef enum camera_channel_id {
    CAMERA_CHANNEL_ID_MAIN = 0,    /**< Main channel */
    CAMERA_CHANNEL_ID_SUB,         /**< Sub channel */
    CAMERA_CHANNEL_ID_THIRD,       /**< Third channel */
    CAMERA_CHANNEL_ID_FOURTH,      /**< Fourth channel */
    CAMERA_CHANNEL_ID_MAX
} camera_channel_id_t;

/**
 * @brief Camera metadata structure
 * @details Contains frame metadata such as exposure time, gain, white balance
 */
typedef struct camera_metadata {
    size_t  size;                  /**< Total size of metadata */
    uint32_t version;              /**< Metadata version */
    uint32_t flags;                /**< Metadata flags */
    size_t  entry_count;           /**< Number of metadata entries */
    size_t  entry_capacity;        /**< Capacity for metadata entries */
    size_t  entries_start;         /**< Start offset of entries */
    size_t  data_count;            /**< Number of data items */
    size_t  data_capacity;         /**< Capacity for data items */
    size_t  data_start;            /**< Start offset of data */
    uint32_t  padding;             /**< Padding bytes */
    size_t  vendor_id;             /**< Vendor identifier */
} camera_metadata_t;

/**
 * @brief Camera scaling configuration
 * @details Defines output scaling parameters
 */
typedef struct camera_scale{
    int32_t output_width;          /**< Output width in pixels */
    int32_t output_height;         /**< Output height in pixels */
} camera_scale_t;

/**
 * @brief Camera cropping configuration
 * @details Defines input cropping region
 * @note Uses x, y, width, height to match system/core rect_t convention
 */
typedef struct camera_crop{
    int32_t x;                     /**< X crop coordinate */
    int32_t y;                     /**< Y crop coordinate */
    int32_t width;                 /**< Crop width */
    int32_t height;                /**< Crop height */
} camera_crop_t;

/**
 * @brief Camera channel configuration structure
 * @details Complete configuration for a camera processing channel
 */
typedef struct camera_channel_config {
    camera_id_t device_id;         /**< Associated camera device ID */
    uint32_t width;                /**< Channel width in pixels */
    uint32_t height;               /**< Channel height in pixels */
    bool    mirror_enabled;        /**< Mirror effect enabled flag */
    bool    flip_enabled;          /**< Flip effect enabled flag */
    uint16_t rotation;             /**< Stream rotation value */
    uint32_t max_buffers;          /**< Requested frame-buffer pool depth for this channel; the HAL maps it to driver buffer counts where applicable. */
    camera_crop_t crop;            /**< Input cropping configuration */
    camera_scale_t scale;          /**< Output scaling configuration */
    void *reserved[2];             /**< Reserved for future use */
} camera_channel_config_t;

/**
 * @brief Camera stream structure
 * @details Represents a camera input or output stream with configuration
 */
typedef struct camera_stream {
    camera_stream_format_t stream_format;     /**< Stream format (single/splice) */
    camera_channel_config_t *channel_config;  /**< Channel configuration array */
    int num_channel;                          /**< Number of channels */
    uint32_t fps;                             /**< Stream frame rate */
    int num_slices;                /**< Slice-capture switch, not a count: 1 (or 0) keeps the
                                        default full-frame path; >1 asks the HAL to put the
                                        channel into slice/kernel-encode mode where supported,
                                        so no full frame is mapped for userspace. The slice
                                        geometry is the driver's. Buffer depth stays
                                        max_buffers in both modes. */
    pixel_format_t format;                    /**< Pixel format */
    camera_codec_config_t codec_config;       /**< Codec configuration */
    camera_rc_config_t rc_config;             /**< Rate control configuration */
    camera_gop_config_t gop_config;           /**< GOP configuration */
    int priv_id;                              /**< the id for priv */
    void *puser;                              /**< User private data */
    void **priv;                              /**< Private data array for each channel */
    void *reserved[4];                        /**< Reserved for future use */
} camera_stream_t;

/**
 * @brief Calculate total width for splice streams
 * @param stream Pointer to camera stream
 * @return Total width in pixels
 */
static inline int get_splice_stream_width(camera_stream_t *stream)
{
    int width;
    if(stream->stream_format == CAMERA_STREAM_FORMAT_SPLICE_VER)
    {
        width = 0;
        for(int i=0; i< stream->num_channel; i++)
        {
            width += stream->channel_config[i].width;
        }
    }
    else if(stream->stream_format == CAMERA_STREAM_FORMAT_SPLICE_HOR)
    {
        width = stream->channel_config[0].width;
    }
    else{
        width = stream->channel_config[0].width;
    }
    return width;
}

/**
 * @brief Calculate total height for splice streams
 * @param stream Pointer to camera stream
 * @return Total height in pixels
 */
static inline int get_splice_stream_height(camera_stream_t *stream)
{
    int height;
    if(stream->stream_format == CAMERA_STREAM_FORMAT_SPLICE_VER)
    {
        height = stream->channel_config[0].height;
    }
    else if(stream->stream_format == CAMERA_STREAM_FORMAT_SPLICE_HOR)
    {
        height = 0;
        for(int i=0; i< stream->num_channel; i++)
        {
            height += stream->channel_config[i].height;
        }
    }
    else{
        height = stream->channel_config[0].height;
    }
    return height;
}

/**
 * @brief Camera buffer status enumeration
 * @details Defines the current status of a stream buffer
 */
typedef enum camera_buffer_status {
    camera_buffer_status_OK = 0,       /**< Buffer is in normal state */
    camera_buffer_status_ERROR = 1     /**< Buffer contains invalid data */
} camera_buffer_status_t;

/**
 * @brief Camera stream buffer structure
 * @details Represents a single buffer from a camera stream
 */
typedef struct camera_stream_buffer {
    camera_stream_t *stream;           /**< Associated stream handle */
    void *data;                        /**< Buffer data pointer */
    uint32_t data_size;                /**< Size of buffer data */
    int status;                        /**< Buffer status */
    int acquire_fence;                 /**< Acquire sync fence */
    int release_fence;                 /**< Release sync fence */
    int frame_type;                    /**< Encoder frame type (0=P, 1=I for video) */
} camera_stream_buffer_t;

/**
 * @brief Camera stream buffer set structure
 * @details Complete set of buffers for a stream
 */
typedef struct camera_stream_buffer_set {
    camera_stream_t *stream;           /**< Associated stream handle */
    uint32_t num_buffers;             /**< Number of buffers in set */
} camera_stream_buffer_set_t;

/**
 * @brief JPEG blob structure
 * @details Transport header for compressed JPEG data
 */
typedef struct camera_jpeg_blob {
    uint16_t jpeg_blob_id;             /**< JPEG blob identifier */
    uint32_t jpeg_size;                /**< JPEG data size */
} camera_jpeg_blob_t;

/*===========================================================================
 * JPEG Blob Identifiers
 *===========================================================================*/

enum {
    CAMERA_JEPEG_BLOB_ID = 0x00FF,             /**< Standard JPEG blob ID */
    CAMERA_JPEG_APP_SEGMENTS_BLOB_ID = 0x0100, /**< JPEG APP segments blob ID */
};

/**
 * @brief AI performance level enumeration
 * @details Defines different AI processing performance levels
 */
typedef enum camera_ai_performance{
    AI_PERFORMANCE_LOW,                /**< Low performance (QVGA) */
    AI_PERFORMANCE_MEDIUM,             /**< Medium performance (VGA) */
    AI_PERFORMANCE_HIGH,               /**< High performance (720P) */
    AI_PERFORMANCE_VERY_HIGH,          /**< Very high performance */
    AI_PERFORMANCE_ULTRA               /**< Ultra performance */
} camera_ai_performance_t;

/**
 * @brief AI type enumeration
 * @details Defines different AI detection and recognition capabilities
 */
typedef enum {
    CAMERA_AI_NONE   = 0x00,           /**< No AI processing */
    CAMERA_AI_MAIN_MASK             = 0xFFFFFFF0,  /**< Main AI category mask */
    CAMERA_AI_SUB_MASK              = 0x0000000F,  /**< Sub AI category mask */
    CAMERA_AI_DETECT_MASK           = 0x03FFFFF0,  /**< Detection category mask */
    CAMERA_AI_DETECT_CLASSIFY_MASK  = 0x03FC0000,  /**< Classify after Detection category mask */
    CAMERA_AI_RECOGNIZE_MASK        = 0xFC000000,  /**< Recognition category mask */

//    CAMERA_AI_OPTION_IMAGE_SPLIT    = 0x80000000,  /**< image split to 4, call 4 times detect model */

    /* Detection types */
    CAMERA_AI_DETECT_HUMAN_SHAPE    = 0x10,        /**< Human shape detection */
    CAMERA_AI_DETECT_HUMAN_FACE     = 0x20,        /**< Face detection */
    CAMERA_AI_DETECT_UPPER_BODY     = 0x40,       /**< Upper body detection */
    CAMERA_AI_DETECT_GESTURE        = 0x80,       /**< Hand gesture detection */
    CAMERA_AI_DETECT_PET            = 0x100,      /**< Pet detection */
    CAMERA_AI_DETECT_PACKAGE        = 0x200,      /**< Package detection */
    CAMERA_AI_DETECT_FIRE           = 0x400,      /**< Fire detection */
    CAMERA_AI_DETECT_VEHICLE        = 0x1000,     /**< Vehicle detection */
    CAMERA_AI_DETECT_NON_MOTOR_VEHICLE = 0x2000,  /**< Non-motor vehicle detection */
    CAMERA_AI_DETECT_MOTION         = 0x4000,     /**< Motion detection */

    /* recognition for classify from detect*/
    CAMERA_AI_DETECT_CLASSIFY_GESTURE  = 0x040000,  /**< Hand gesture recognition , return detect*/
    CAMERA_AI_DETECT_CLASSIFY_ANTI_SPOOFING_FACE = 0x080000,

    /* Recognition types */
    CAMERA_AI_RECOGNIZE_HUMAN_FACE  = 0x04000000,  /**< Face recognition */
    CAMERA_AI_RECOGNIZE_UPPER_BODY  = 0x08000000,  /**< upper body recognition */

/*******************
** Sub-categories 
*********************/
    /* Sub-categories for non motor vehicle*/
    CAMERA_AI_DETECT_NON_MOTOR_VEHICLE_MOTORCYCLE = 0x01,  /**< Motor vehicle sub-category */
    CAMERA_AI_DETECT_NON_MOTOR_VEHICLE_BICYCLE = 0x02,   /**< Bicycle sub-category */
    CAMERA_AI_DETECT_NON_MOTOR_VEHICLE_TRICYCLE = 0x03,   /**< Bicycle sub-category */

    /* Sub-categories for vehicle*/
    CAMERA_AI_DETECT_VEHICLE_BUS = 0x01,
    CAMERA_AI_DETECT_VEHICLE_CAR = 0x02,
    CAMERA_AI_DETECT_VEHICLE_TRUCK = 0x03,

    /*Sub-categories for detect gesture*/
    CAMERA_AI_DETECT_GESTURE_UNCLASSIFY = 0x01,
    CAMERA_AI_DETECT_GESTURE_OK = 0x02,
    CAMERA_AI_DETECT_GESTURE_SIX = 0x03,
    CAMERA_AI_DETECT_GESTURE_STOP = 0x04,
    CAMERA_AI_DETECT_GESTURE_THUMB_UP = 0x05,
    CAMERA_AI_DETECT_GESTURE_TWO = 0x06,

    /*Sub-categories for detect gesture*/
    CAMERA_AI_DETECT_ANTI_SPOOFING_FACE_LIVING = 0x01,
    CAMERA_AI_DETECT_ANTI_SPOOFING_FACE_NON_LIVING = 0x02,

    /*Sub-categories for detect pet*/
    CAMERA_AI_DETECT_PET_CAT = 0x01,
    CAMERA_AI_DETECT_PET_DOG = 0x02,

    CAMERA_AI_MAX = 13,              /**< Maximum AI types */
} camera_ai_type_t;

/**
 * @brief AI detection rectangle structure
 * @details Contains detection result with confidence score and bounding box
 */
typedef struct camera_ai_detect_rect {
    int              score;          /**< Detection confidence score */
} camera_ai_object_detect_t;

/**
 * @brief AI recognize rectangle structure
 * @details Contains recognize result with confidence score and bounding box
 */
typedef struct camera_ai_recognize_rect {
    int  data_len;                   /**< Feature data length */
    int16_t *data;                      /**< Feature data values */
} camera_ai_object_recognize_t;

/**
 * @brief Camera AI object structure
 * @details Contains AI detection or recognition result with bounding box
 */
typedef struct camera_ai_object {
    camera_ai_type_t ai_type;                    /**< AI sub type for AI-related messages */
    camera_rect_t rect;                          /**< Detection bounding box */

    union {
        camera_ai_object_detect_t detect;        /**< Detection result data */
        camera_ai_object_recognize_t recognize; /**< Recognition result data */
    };
} camera_ai_object_t;

/**
 * @brief Camera notification message structure
 * @details Unified message structure for all notification types
 */
typedef struct camera_notify_msg {
    uint64_t u64Ts;                 /**< Timestamp in milliseconds */
    int num_total;                   /**< Total number of AI objects */
    camera_ai_object_t *ai_object;   /**< Array of AI object results */
} camera_notify_msg_t;

/**
 * @brief AI input configuration structure
 * @details Configuration parameters for AI processing
 */
typedef struct camera_ai_input {
    camera_ai_type_t ai_type;        /**< AI processing type */
    camera_stream_t *stream;          /**< Input stream for AI processing */
    int classify_threshold;          /**< Classification threshold */
    int iou_threshold;               /**< Intersection over Union threshold */
} camera_ai_input_t;

/**
 * @brief AI tile overlap configuration structure
 * @details Configuration parameters for tile-based AI processing with overlap
 */
typedef struct camera_ai_tile_overlap {
    camera_stream_t *stream;         /**< Input stream for AI processing */
    int num_cols;                    /**< Tile column number (e.g., 2) */
    int num_rows;                    /**< Tile row number (e.g., 2) */
    float overlap_ratio;            /**< Overlap ratio between tiles (0.0 to 1.0, e.g., 0.06) */
} camera_ai_tile_overlap_t;

/**
 * @brief Camera request template enumeration
 * @details Defines different camera operation templates
 */
typedef enum camera_request_template {
    CAMERA_TEMPLATE_NONE = 0,        /**< No template */
    CAMERA_TEMPLATE_PREVIEW,         /**< Preview template for monitoring */
    CAMERA_TEMPLATE_VIDEO_RECORD,    /**< Video recording template */
    CAMERA_TEMPLATE_VIDEO_CALL,      /**< Video call template */
    CAMERA_TEMPLATE_QR_SCAN,         /**< QR code scanning template */
    CAMERA_TEMPLATE_PICTURE,         /**< Picture capture template */
    CAMERA_TEMPLATE_METADATA,        /**< Raw data template */
    CAMERA_TEMPLATE_AI,              /**< AI analysis template */
    CAMERA_TEMPLATE_CUSTOM,          /**< Custom template */
    CAMERA_TEMPLATE_COUNT            /**< Number of templates */
} camera_request_template_t;

/**
 * @brief Camera capture request structure
 * @details Represents a single capture request sent to the camera HAL
 */
typedef struct camera_capture_request {
    uint32_t frame_number;                       /**< Unique frame identifier */
    camera_request_template_t request_template;  /**< Request template type */
    uint32_t num_output_buffers;                 /**< Number of output buffers */
    camera_stream_buffer_t *output_buffers;      /**< Output buffer array */
    camera_metadata_t *settings;                 /**< Capture settings */
    camera_stream_buffer_t *input_buffer;        /**< Input buffer for reprocessing */
} camera_capture_request_t;

/**
 * @brief Camera capture result structure
 * @details Contains results from a completed capture operation
 */
typedef struct camera_capture_result {
    uint32_t frame_number;                       /**< Frame number from request */
    uint64_t timestamp;                          /**< Capture timestamp */
    const camera_metadata_t *result;             /**< Result metadata */
    camera_stream_buffer_t *output_buffers;      /**< Output buffers */
    const camera_stream_buffer_t *input_buffer;  /**< Input buffer */
    uint32_t num_physcam_metadata;               /**< Number of physical camera metadata */
    const camera_metadata_t **physcam_metadata;  /**< Physical camera metadata array */
    uint32_t num_output_buffers;                 /**< Number of output buffers */
    uint32_t partial_result;                     /**< Partial result number */
    const char **physcam_ids;                    /**< Physical camera IDs */
} camera_capture_result_t;

/**
 * @brief Camera callback operations structure
 * @details Defines callback functions for HAL to communicate with framework
 */
typedef struct camera_callback_ops {
    /**
     * @brief Process capture result callback
     * @details Called by HAL to return completed capture results including frame data,
     *          metadata, and timestamps to the framework
     * @param ops Callback operations structure
     * @param result Capture result containing frame number, buffers, and metadata
     */
    void (*process_capture_result)(const struct camera_callback_ops *,
            const camera_capture_result_t *result);

    /**
     * @brief Notification callback
     * @details Called by HAL for asynchronous notifications such as AI detection results,
     *          recognition results, and anti-spoofing results
     * @param ops Callback operations structure
     * @param msg Notification message containing AI results or other events
     */
    void (*notify)(const struct camera_callback_ops *,
            const camera_notify_msg_t *msg);

} camera_callback_ops_t;

/**
 * @brief Camera information structure
 * @details Contains static information about a camera device
 */
typedef struct camera_info {
    int orientation;                             /**< Camera image orientation */
    uint32_t device_version;                     /**< Device API version */
    const camera_metadata_t *static_camera_characteristics;  /**< Static characteristics */
    uint32_t max_frame_rate;                     /**< Maximum frame rate */
    uint32_t max_width;                          /**< Maximum width */
    uint32_t max_height;                         /**< Maximum height */
    camera_rect_t crop;                          /**< Crop information */
} camera_info_t;

/**
 * @brief Torch mode status enumeration
 * @details Defines the current status of camera flash torch mode
 */
typedef enum torch_mode_status {
    TORCH_MODE_STATUS_NOT_AVAILABLE = 0,         /**< Torch mode not available */
    TORCH_MODE_STATUS_AVAILABLE_OFF = 1,         /**< Torch mode available but off */
    TORCH_MODE_STATUS_AVAILABLE_ON = 2,          /**< Torch mode available and on */
} torch_mode_status_t;

/**
 * @brief Camera module callbacks structure
 * @details Defines callback functions for camera module events
 */
typedef struct camera_module_callbacks {
    /**
     * @brief Camera device status change callback
     * @details Notifies framework of camera device state changes such as device
     *          connection/disconnection or availability status updates
     * @param callbacks Camera module callbacks structure
     * @param camera_id Camera device identifier
     * @param new_status New device status (connected, disconnected, etc.)
     */
    void (*camera_device_status_change)(const struct camera_module_callbacks*,
            int camera_id,
            int new_status);

    /**
     * @brief Torch mode status change callback
     * @details Notifies framework of flash torch mode changes including availability
     *          and current state (on/off)
     * @param callbacks Camera module callbacks structure
     * @param camera_id Camera device identifier
     * @param new_status New torch mode status (available, unavailable, on, off)
     */
    void (*torch_mode_status_change)(const struct camera_module_callbacks*,
            const char* camera_id,
            int new_status);
} camera_module_callbacks_t;

/**
 * @brief Camera stream configuration structure
 * @details Defines complete stream configuration for camera operations
 */
typedef struct camera_stream_configuration {
    uint32_t num_streams;                        /**< Number of streams */
    camera_stream_t **streams;                   /**< Stream array */
    const camera_metadata_t *session_parameters; /**< Session parameters */
} camera_stream_configuration_t;

/**
 * @brief Camera AF mode for high-level device control.
 */
typedef enum {
    CAMERA_AF_MODE_OFF = 0,
    CAMERA_AF_MODE_ONCE = 1,
} camera_af_mode_t;

/**
 * @brief Camera AF state exposed to framework/app layers.
 */
typedef enum {
    CAMERA_AF_STATE_IDLE = 0,
    CAMERA_AF_STATE_SCANNING = 1,
    CAMERA_AF_STATE_LOCKED = 2,
    CAMERA_AF_STATE_FAILED = 3,
    CAMERA_AF_STATE_UNSUPPORTED = 4,
} camera_af_state_t;

/**
 * @brief Camera hardware device structure
 * @details Main camera device interface with all required operations
 */
typedef struct camera_hw_device {
    hw_device_t common;                          /**< Common hardware device interface */

    /**
     * @brief Set callback operations
     * @details Initialize framework callback function pointers for HAL to communicate with framework
     * @param dev Camera hardware device
     * @param callback_ops Callback operations structure containing function pointers
     * @return 0 on success, negative error code on failure
     */
    int (*set_callback_ops)(const struct camera_hw_device *,
            const camera_callback_ops_t *callback_ops);

    /**
     * @brief Configure streams
     * @details Set up input and output streams for camera operations, including stream format,
     *          channel configuration, codec settings, and buffer allocation
     * @param dev Camera hardware device
     * @param stream_list Stream configuration containing multiple streams with their parameters
     * @return 0 on success, negative error code on failure
     */
    int (*configure_streams)(const struct camera_hw_device *,
            camera_stream_configuration_t *stream_list);

    /**
     * @brief Reconfigure streams
     * @details Reset and reconfigure stream parameters when stream properties change,
     *          handling channel addition/removal and codec reset for unchanged dimensions
     * @param dev Camera hardware device
     * @param stream_list Updated stream configuration
     * @return 0 on success, negative error code on failure
     */
    int (*reconfigure_streams)(const struct camera_hw_device *,
            camera_stream_configuration_t *stream_list);

    /**
     * @brief Set global camera parameters
     * @details Set camera-wide configuration parameters such as AI configuration,
     *          including AI type, stream binding, and detection thresholds
     * @param dev Camera hardware device
     * @param name Parameter name (e.g., "AiConfig")
     * @param value Parameter value pointer
     * @param len Parameter value length
     * @return 0 on success, negative error code on failure
     */
    int (*set_parameters)(const struct camera_hw_device *,
            const char* name, void *value, unsigned int len);

    /**
     * @brief Get global camera parameters
     * @details Retrieve camera-wide configuration parameters
     * @param dev Camera hardware device
     * @param keys Parameter keys to retrieve
     * @return Parameter string on success, NULL on failure
     */
    char * (*get_parameters)(const struct camera_hw_device *,
                             const char *keys);

    /**
     * @brief Add camera AI processing
     * @details Enable AI processing for specified type by calling HAL AI add function
     * @param dev Camera hardware device
     * @param ai_type AI processing type to enable
     * @return 0 on success, negative error code on failure
     */
    int (*add_camera_ai)(const struct camera_hw_device *,
                             camera_ai_type_t ai_type);

    /**
     * @brief Remove camera AI processing
     * @details Disable AI processing for specified type by calling HAL AI remove function
     * @param dev Camera hardware device
     * @param ai_type AI processing type to disable
     * @return 0 on success, negative error code on failure
     */
    int (*remove_camera_ai)(const struct camera_hw_device *,
                             camera_ai_type_t ai_type);

    /**
     * @brief Enable tile overlap function
     * @details Enable AI processing for specified type by calling HAL AI add function
     * @param dev Camera hardware device
     * @return 0 on success, negative error code on failure
     */
    int (*enable_camera_ai_tile_overlap)(const struct camera_hw_device *,
                camera_stream_t *stream);

    /**
     * @brief Disable tile overlap function
     * @details Disable AI processing for specified type by calling HAL AI remove function
     * @param dev Camera hardware device
     * @return 0 on success, negative error code on failure
     */
    int (*disable_camera_ai_tile_overlap)(const struct camera_hw_device *, 
                camera_stream_t *stream);


    /**
     * @brief Construct default request settings
     * @details Create default settings for specified request template (preview, capture, etc.)
     * @param dev Camera hardware device
     * @param type Request template type
     * @return Default metadata settings on success, NULL on failure
     */
    const camera_metadata_t* (*construct_default_request_settings)(
            const struct camera_hw_device *,
            int type);

    /**
     * @brief Process capture request
     * @details Handle new capture request from framework, including buffer management,
     *          stream processing, and result delivery through callbacks
     * @param dev Camera hardware device
     * @param request Capture request containing frame number, template, and buffers
     * @return 0 on success, negative error code on failure
     */
    int (*process_capture_request)(const struct camera_hw_device *,
            camera_capture_request_t *request);

    /**
     * @brief Dump debug information
     * @details Print debugging state for camera device to specified file descriptor
     * @param dev Camera hardware device
     * @param fd File descriptor for debug output
     */
    void (*dump)(const struct camera_hw_device *, int fd);

    /**
     * @brief Flush camera pipeline
     * @details Flush all in-process captures and buffers, clearing the processing queue
     * @param dev Camera hardware device
     * @return 0 on success, negative error code on failure
     */
    int (*flush)(const struct camera_hw_device *);

    /**
     * @brief Signal stream flush
     * @details Notify HAL to return all buffers of designated streams immediately
     * @param dev Camera hardware device
     * @param num_streams Number of streams to flush
     * @param streams Array of stream pointers to flush
     */
    void (*signal_stream_flush)(const struct camera_hw_device*,
            uint32_t num_streams,
            const camera_stream_t* const* streams);

    /**
     * @brief Check reconfiguration requirement
     * @details Determine if stream reconfiguration is needed based on session parameter changes
     * @param dev Camera hardware device
     * @param old_session_params Previous session parameters
     * @param new_session_params New session parameters
     * @return 1 if reconfiguration required, 0 otherwise
     */
    int (*is_reconfiguration_required)(const struct camera_hw_device*,
            const camera_metadata_t* old_session_params,
            const camera_metadata_t* new_session_params);

    /*
     * AF: single af_control replaces 5 ops. Slot count must stay 6 pointers
     * (af_control + reserved[5]) so sizeof(camera_hw_device_t) does not grow —
     * camera_device.device_info is embedded after this struct.
     */
    int (*af_control)(const struct camera_hw_device *, int camera_id, int cmd, void *arg);
    void *reserved[5];                           /**< Keep ABI size (was 5 AF ops + reserved[1]) */
    void *priv;                                  /**< Private data */
} camera_hw_device_t;

/** Commands for camera_hw_device_t::af_control */
enum {
    CAMERA_AF_CMD_IS_SUPPORTED = 1,   /**< arg: int *out (0/1) */
    CAMERA_AF_CMD_GET_SHARPNESS,      /**< arg: uint32_t *score */
    CAMERA_AF_CMD_SET_FOCUS_POS,      /**< arg: int *pos (user mode) */
    CAMERA_AF_CMD_GET_FOCUS_POS,      /**< arg: int *pos */
    CAMERA_AF_CMD_GET_POS_RANGE,      /**< arg: int min_max[2] */
    CAMERA_AF_CMD_TRIGGER,            /**< arg: camera_af_trigger_arg_t * (nullable → center) */
    CAMERA_AF_CMD_GET_HW_STATE,       /**< arg: camera_af_hw_state_t * */
    CAMERA_AF_CMD_GET_RUN_MODE,       /**< arg: int *mode (CAMERA_AF_RUN_MODE_*) */
    CAMERA_AF_CMD_CANCEL,             /**< arg: unused */
};

enum {
    CAMERA_AF_RUN_MODE_USER = 0,
    CAMERA_AF_RUN_MODE_KERNEL = 1,
};

enum {
    CAMERA_AF_HW_STATE_IDLE = 0,
    CAMERA_AF_HW_STATE_RUNNING = 1,
    CAMERA_AF_HW_STATE_STABLE = 2,
};

/** Normalized ROI in preview FOV; all-zero or NULL trigger → center 1/8 window */
typedef struct camera_af_roi {
    float nx;
    float ny;
    float nw;
    float nh;
} camera_af_roi_t;

typedef struct camera_af_trigger_arg {
    const camera_af_roi_t *roi; /**< NULL = center default */
} camera_af_trigger_arg_t;

typedef struct camera_af_hw_state {
    int state;       /**< CAMERA_AF_HW_STATE_* */
    int af_update;   /**< kernel af_update flag */
    int focus_pos;
} camera_af_hw_state_t;

/**
 * @brief Camera module structure
 * @details Camera module interface with device management operations
 */
typedef struct camera_module {
    hw_module_t common;                          /**< Common module interface */

    /**
     * @brief Get number of cameras
     * @details Return the number of accessible camera devices by calling HAL camera get number function
     * @return Number of available camera devices
     */
    int (*get_number_of_cameras)(void);

    /**
     * @brief Get camera information
     * @details Return static information for specified camera device including orientation,
     *          device version, characteristics, and capabilities
     * @param camera_id Camera device identifier
     * @param info Pointer to camera info structure to be filled
     * @return 0 on success, negative error code on failure
     */
    int (*get_camera_info)(int camera_id, struct camera_info *info);

    /**
     * @brief Set camera information
     * @details Update camera information for specified device with new configuration
     * @param camera_id Camera device identifier
     * @param info Pointer to camera info structure containing new information
     * @return 0 on success, negative error code on failure
     */
    int (*set_camera_info)(int camera_id, struct camera_info *info);

    /**
     * @brief Set module callbacks
     * @details Provide callback functions for asynchronous events such as device status changes
     * @param callbacks Pointer to callback functions structure
     * @return 0 on success, negative error code on failure
     */
    int (*set_callbacks)(const camera_module_callbacks_t *callbacks);

    /**
     * @brief Set torch mode
     * @details Control flash torch mode for specified camera by calling HAL torch mode function
     * @param camera_id Camera device identifier
     * @param enabled True to enable torch mode, false to disable
     * @return 0 on success, negative error code on failure
     */
    int (*set_torch_mode)(const char* camera_id, bool enabled);

    /**
     * @brief Notify device state change
     * @details Inform module of overall device state changes
     * @param deviceState Device state flags
     */
    void (*notify_device_state_change)(uint64_t deviceState);

    /**
     * @brief Get camera device version
     * @details Return device version for specified camera
     * @param camera_id Camera device identifier
     * @param version Pointer to store device version
     * @return 0 on success, negative error code on failure
     */
    int (*get_camera_device_version)(int camera_id, uint32_t *version);

    void* reserved[1];                           /**< Reserved for future use */
} camera_module_t;

/**
 * @brief Open camera hardware device
 * @param module Hardware module
 * @param device Pointer to camera device
 * @return 0 on success, negative error code on failure
 */
static inline int camera_hw_device_open(const struct hw_module_t* module,
                                       struct camera_hw_device** device)
{
    return module->methods->open(module, CAMERA_HARDWARE_INTERFACE,
                                 TO_HW_DEVICE_T_OPEN(device));
}

/**
 * @brief Close camera hardware device
 * @param device Camera device to close
 * @return 0 on success, negative error code on failure
 */
static inline int camera_hw_device_close(struct camera_hw_device* device)
{
    return device->common.close(&device->common);
}

/*===========================================================================
 * Module API Functions
 *===========================================================================*/

/**
 * @brief Initialize camera hardware module
 * @return 0 on success, negative error code on failure
 */
int hw_camera_module_init(void);

/*===========================================================================
 * Error Code Definitions
 *===========================================================================*/

/** @brief Success */
#define HARDWARE_CAMERA_OK                      SYSTEM_SUCCESS

/** @brief Invalid parameter error */
#define HARDWARE_CAMERA_ERROR_INVALID_PARAM     SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_CAMERA, ERROR_INVALID_PARAM)

/** @brief Not initialized error */
#define HARDWARE_CAMERA_ERROR_NOT_INIT          SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_CAMERA, ERROR_NOT_INIT)

/** @brief Memory allocation error */
#define HARDWARE_CAMERA_ERROR_MEMORY            SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_CAMERA, ERROR_MEMORY)

/** @brief Timeout error */
#define HARDWARE_CAMERA_ERROR_TIMEOUT           SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_CAMERA, ERROR_TIMEOUT)

/** @brief Not found error */
#define HARDWARE_CAMERA_ERROR_NOT_FOUND         SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_CAMERA, ERROR_NOT_FOUND)

/** @brief Device busy error */
#define HARDWARE_CAMERA_ERROR_BUSY              SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_CAMERA, ERROR_BUSY)

/** @brief I/O error */
#define HARDWARE_CAMERA_ERROR_IO                SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_CAMERA, ERROR_IO)

/** @brief Operation not supported error */
#define HARDWARE_CAMERA_ERROR_NOT_SUPPORTED     SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_CAMERA, ERROR_NOT_SUPPORTED)

/** @brief Invalid state error */
#define HARDWARE_CAMERA_ERROR_INVALID_STATE     SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_CAMERA, ERROR_INVALID_STATE)

/** @brief Operation failed error */
#define HARDWARE_CAMERA_ERROR_OPERATION_FAILED  SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_CAMERA, ERROR_OPERATION_FAILED)

/* Camera-specific error codes */
/** @brief Device limit exceeded error */
#define HARDWARE_CAMERA_ERROR_DEVICE_LIMIT      SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_CAMERA, 0x0017)

/** @brief Channel limit exceeded error */
#define HARDWARE_CAMERA_ERROR_CHANNEL_LIMIT     SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_CAMERA, 0x0018)

/** @brief Stream limit exceeded error */
#define HARDWARE_CAMERA_ERROR_STREAM_LIMIT      SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_CAMERA, 0x0019)

/** @brief Stream configuration error */
#define HARDWARE_CAMERA_ERROR_STREAM_CONFIG     SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_CAMERA, 0x001A)

/** @brief AI not available error */
#define HARDWARE_CAMERA_ERROR_AI_NOT_AVAILABLE  SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_CAMERA, 0x001B)

__END_DECLS

#endif /* HARDWARE_CAMERA_H */
