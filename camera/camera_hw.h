// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/camera.
 *
 * Camera hardware abstraction layer header.
 * - Defines camera device structures and types for HAL implementation.
 * - Contains device, channel, and stream management structures.
 */
#ifndef HARDWARE_CAMERA_HW_H
#define HARDWARE_CAMERA_HW_H

#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <sys/cdefs.h>
#include <sys/types.h>
#include <time.h>
#include <stdbool.h>

#include "hardware.h"
#include "camera.h"
#include "hal_camera_chip_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Type Definitions
 * ============================================================================ */


/**
 * @brief Camera status enumeration.
 * @details Status transitions:
 *   - device_info, channel_info: uninitialized -> created -> enable -> idle
 *   - stream_info: uninitialized -> enable -> idle -> reset -> enable
 */
typedef enum {
    CAMERA_STATUS_UNINITIALIZE    = 0,              /**< Uninitialized for device_info, channel_info, stream_info */
    CAMERA_STATUS_CREATE,                           /**< Created for device_info, channel_info */
    CAMERA_STATUS_ENABLE,                           /**< Enable device or enable channel, for device_info, channel_info, stream_info */
    CAMERA_STATUS_DISABLE,                          /**< After enable and finished, set by callback, for device_info, channel_info, stream_info */
    CAMERA_STATUS_REMOVE,                           /**< Don't need after reset, uninitialized/enable/create/idle -> remove */
    CAMERA_STATUS_BUSY,                             /**< Busy */
    CAMERA_STATUS_READY,                            /**< Ready for read data */
    CAMERA_STATUS_ERROR,                            /**< Error */
    CAMERA_STATUS_WORK_MASK         = 0xFF,         /**< Work status mask */
    CAMERA_STATUS_RESET             = 0x0100,       /**< Set by setting, for stream_info, channel_info. Example: change width*height */
} camera_status_type_t;


/** Maximum output buffers stored per stream_info (must cover max streams per process_capture_request). */
#define STREAM_INFO_MAX_OUTPUT_BUFFERS  10

/**
 * @brief Stream information structure.
 * @details Saves stream info pointer to stream->channel_config[].priv.
 */
typedef struct {
    camera_stream_t *stream;                        /**< Pointer to stream[index] for codec info */
    int stream_channel_config_id;                   /**< stream[].channel_config[] stream_channel_config_id */

    int status;                                     /**< -1 for stream disable, others for template */
    bool stream_release_flag;                       /**< Stream release flag */

    camera_stream_buffer_t *request_output_buffer;  /**< Request output buffer */
    camera_capture_request_t request;               /**< Capture request (output_buffers always points to owned_output_buffers) */
    /** Deep copy of output_buffers — owned by this stream_info, lifetime equals stream_info itself.
     *  camera_process_capture_request() copies the incoming output_buffers array here and sets
     *  request.output_buffers = owned_output_buffers, so the capture thread never holds a pointer
     *  into framework-layer memory that may be freed between successive process_capture_request calls. */
    camera_stream_buffer_t owned_output_buffers[STREAM_INFO_MAX_OUTPUT_BUFFERS];
    int request_template;                           /**< Request template, sync while change */

    pixel_format_t format;                          /**< Save format for stream reset */

    int device_id;                                  /**< Device ID */
    int channel_id;                                 /**< Channel ID */
    int stream_id;                                  /**< Stream ID */
    int codec_id;                                   /**< Codec ID */

    camera_ai_type_t ai_type;                       /**< Only used for AI request template */
    camera_status_type_t ai_status;                 /**< AI status */
    pthread_t ai_thread_id;                         /**< AI thread identifier */

    bool ai_tile_overlap_enable;                    /**< Enable or disable AI tile overlap */
    camera_ai_tile_overlap_t ai_tile_overlap;      /**< Store AI tile overlap data */

    uint32_t ai_stream_tile_width;                  /**< Save AI source stream width */
    uint32_t ai_stream_tile_height;                 /**< Save AI source stream height */
} stream_info_t;

/**
 * @brief Channel information structure.
 */
typedef struct {
    int num_stream;                                 /**< Stream number config */
    stream_info_t stream_info[CAMERA_MAX_STREAM_PER_CHANNEL];  /**< For codec */
    int status;                                     /**< Channel enable or disable */

    camera_channel_config_t channel_config;        /**< Save stream->channel_config info */

    pthread_t thread_id;                            /**< Thread identifier */
    camera_hw_device_t *dev;                        /**< Camera hardware device pointer */
} channel_info_t;


/**
 * @brief Device information structure.
 */
typedef struct {
    int num_channel;                                /**< Channel number config */
    channel_info_t channel_info[CAMERA_MAX_CHANNEL];  /**< Channel info */

    int device_id;                                  /**< Camera device ID */
    int status;                                     /**< Request status */
} device_info_t;


/**
 * @brief Camera device structure.
 */
typedef struct camera_device {
    camera_hw_device_t device;                      /**< Camera hardware device interface */

    void *hal_camera;                               /**< HAL camera context */
    const camera_callback_ops_t *callback_ops;      /**< Callback operations */

    /* For streams input */
    camera_metadata_t *DefaultMetadata[CAMERA_TEMPLATE_COUNT];  /**< Save request template setting value */
    camera_stream_configuration_t *stream_list;     /**< Stream list */

    /* For device info, format: device_info[].channel_info[].stream_info[] */
    device_info_t device_info[CAMERA_MAX_NUM];      /**< Device info array */
    int num_device;                                 /**< Number of devices */

    /* For AI */
    int num_camera_ai;                              /**< Number of camera AI */
    camera_ai_input_t ai_input[CAMERA_AI_MAX];      /**< AI input array */
    camera_notify_msg_t ai_output_result;           /**< AI output result */

    /* For others */
    pthread_mutex_t lock;                           /**< Device synchronization mutex */
    pthread_mutex_t channel_lock;                   /**< Channel synchronization mutex */
    volatile int closing;                           /**< 1 while close is in progress */

    int format;                                     /**< Format */

    /* Undetermined parameters */
    camera_stream_t *streams[CAMERA_MAX_CHANNEL];   /**< Stream array */
    pthread_t stream_capture_thread[CAMERA_MAX_CHANNEL];  /**< Stream capture thread array */
} camera_device_t;

/**
 * @brief Camera stream reconfiguration structure.
 */
typedef struct {
    camera_stream_format_t format;                  /**< Stream format */
    camera_stream_t *stream;                        /**< Stream pointer */

    int stream_channel_config_id;                   /**< Stream channel config ID */
    camera_capture_request_t request;               /**< Capture request */
    int status;                                     /**< Status */
} camera_reconfigure_stream_t;

/**
 * @brief Camera channel reconfiguration structure.
 */
typedef struct {
    int width;                                      /**< Width */
    int height;                                     /**< Height */
    camera_channel_config_t channel_config;        /**< Channel configuration */

    int channel_id;                                 /**< If put on existing channel, channel_id is saved ID, otherwise -1 */
    int status;                                     /**< Status */
    int num_stream;                                 /**< Number of streams */
    camera_reconfigure_stream_t stream_reconfigure[CAMERA_MAX_STREAM_PER_CHANNEL];  /**< Stream reconfiguration array */
} camera_reconfigure_channel_t;

/**
 * @brief Camera device reconfiguration structure.
 */
typedef struct {
    int device_id;                                 /**< Save device_id, -1 if none. If device_id changed, don't reconfigure */

    int num_channel;                                /**< Number of channels */
    camera_reconfigure_channel_t channel[CAMERA_MAX_CHANNEL];  /**< Channel reconfiguration array */
} camera_reconfigure_device_t;







#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_CAMERA_HW_H */
