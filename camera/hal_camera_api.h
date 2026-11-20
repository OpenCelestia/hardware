// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/camera.
 *
 * Camera HAL API interface.
 * - Platform-agnostic HAL interface for camera operations.
 * - Defines HAL function prototypes for camera hardware abstraction.
 */

#ifndef HARDWARE_HAL_CAMERA_API_H
#define HARDWARE_HAL_CAMERA_API_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/cdefs.h>
#include <sys/types.h>

#include "camera_hw.h"
#include "camera.h"

__BEGIN_DECLS

/*===========================================================================
 * HAL Initialization and Deinitialization
 *===========================================================================*/

/**
 * @brief Initialize camera HAL
 * @details Allocates and initializes camera HAL context for the given device
 * @param cdev Camera device structure
 * @return HAL context pointer on success, NULL on failure
 */
void *hal_camera_init(struct camera_device *cdev);

/**
 * @brief Deinitialize camera HAL
 * @details Releases all resources associated with the HAL context
 * @param my_data HAL context pointer returned from hal_camera_init
 */
void hal_camera_deinit(void *my_data);

/*===========================================================================
 * Device and Channel Management
 *===========================================================================*/

/**
 * @brief Create camera device
 * @details Initializes a camera device with the specified ID
 * @param hal_camera HAL context pointer
 * @param device_id Camera device identifier
 * @return 0 on success, negative error code on failure
 */
int hal_camera_create_device(void *hal_camera, int device_id);

/**
 * @brief Create camera channel
 * @details Creates a video processing channel for the specified device
 * @param hal_camera HAL context pointer
 * @param device_id Camera device identifier
 * @param channel_id Channel identifier
 * @return 0 on success, negative error code on failure
 */
int hal_camera_create_channel(void *hal_camera, int device_id, int channel_id);

/**
 * @brief Register extra PDK VI channel when required (e.g. main-only sub_max).
 */
int hal_camera_sync_platform_vi_after_channels(void *hal_camera, int device_id);

/**
 * @brief Disable camera channel
 * @details Stops video processing on the specified channel
 * @param hal_camera HAL context pointer
 * @param device_id Camera device identifier
 * @param channel_id Channel identifier
 * @return 0 on success, negative error code on failure
 */
int hal_camera_disable_channel(void *hal_camera, int device_id, int channel_id);

/**
 * @brief Enable camera channel
 * @details Starts video processing on the specified channel
 * @param hal_camera HAL context pointer
 * @param device_id Camera device identifier
 * @param channel_id Channel identifier
 * @return 0 on success, negative error code on failure
 */
int hal_camera_enable_channel(void *hal_camera, int device_id, int channel_id);

/**
 * @brief Enable camera device
 * @details Activates the specified camera device
 * @param hal_camera HAL context pointer
 * @param device_id Camera device identifier
 * @return 0 on success, negative error code on failure
 */
int hal_camera_enable_device(void *hal_camera, int device_id);

/**
 * @brief Disable camera device
 * @details Deactivates the specified camera device
 * @param hal_camera HAL context pointer
 * @param device_id Camera device identifier
 * @return 0 on success, negative error code on failure
 */
int hal_camera_disable_device(void *hal_camera, int device_id);

/**
 * @brief Stop VI capture on a channel (disable VI chn to unblock get_frame; no codec).
 * @details Call before joining the capture thread so get_frame unblocks cleanly.
 */
int hal_camera_quiesce_channel(void *hal_camera, int device_id, int channel_id);

/*===========================================================================
 * Stream Operations
 *===========================================================================*/

/**
 * @brief Read data from camera stream
 * @details Retrieves frame data from the specified stream channel
 * @param hal_camera HAL context pointer
 * @param device_id Camera device identifier
 * @param channel_id Channel identifier
 * @param buffer Output pointer to receive buffer data pointer
 * @param timestamp Output pointer to receive frame timestamp
 * @param bytes Maximum bytes to read
 * @return Number of bytes read on success, 0 or negative error code on failure
 */
size_t hal_camera_stream_read(void *hal_camera, int device_id, int channel_id,
                             void **buffer, uint64_t *timestamp, size_t bytes);

/*===========================================================================
 * Codec Operations
 *===========================================================================*/

/**
 * @brief Initialize camera codec
 * @details Initializes video codec for the camera HAL
 * @param hal_camera HAL context pointer
 * @return 0 on success, negative error code on failure
 */
int hal_camera_codec_init(void *hal_camera);

/**
 * @brief Enable codec for stream
 * @details Enables video encoding for the specified stream
 * @param hal_camera HAL context pointer
 * @param stream Camera stream configuration
 * @return 0 on success, negative error code on failure
 */
int hal_camera_codec_enable(void *hal_camera, camera_stream_t *stream);

/**
 * @brief Disable codec for stream
 * @details Disables video encoding for the specified stream
 * @param hal_camera HAL context pointer
 * @param stream Camera stream configuration
 * @return 0 on success, negative error code on failure
 */
int hal_camera_codec_disable(void *hal_camera, camera_stream_t *stream);

/**
 * @brief Read encoded data from codec
 * @details Retrieves encoded frame data from the codec
 * @param hal_camera HAL context pointer
 * @param device_id Camera device identifier
 * @param channel_id Channel identifier
 * @param stream_id Stream identifier
 * @param buffer Output pointer to receive buffer data pointer
 * @param timestamp Output pointer to receive frame timestamp
 * @param bytes Maximum bytes to read
 * @return Number of bytes read on success, 0 or negative error code on failure
 */
size_t hal_camera_codec_read(void *hal_camera, int device_id, int channel_id,
                             int stream_id, void **buffer, uint64_t *timestamp,
                             size_t bytes);

/**
 * Capture thread has finished reading the buffer from the last successful
 * hal_camera_codec_read(). codec_disable waits for this before releasing it.
 */
void hal_camera_codec_read_done(void *hal_camera, int device_id, int channel_id,
                                int stream_id);

/**
 * @brief Request an IDR/I-frame on the encoder for the given stream.
 * @return 0 on success, negative error code on failure
 */
int hal_camera_codec_request_idr(void *hal_camera, int device_id, int channel_id,
                                 int stream_id);

/**
 * @brief Get the frame type of the most recent encoded frame for a stream.
 * @param hal_camera HAL camera instance handle
 * @param device_id Camera device identifier
 * @param channel_id Channel identifier
 * @param stream_id Stream identifier
 * @return Frame type (FRAME_TYPE_P=0, FRAME_TYPE_I=1, FRAME_TYPE_PI=2),
 *         or -1 if the stream/codec has not produced any frame yet.
 */
int hal_camera_codec_get_frame_type(void *hal_camera, int device_id, int channel_id,
                                    int stream_id);

/*===========================================================================
 * Stream Configuration
 *===========================================================================*/

/**
 * @brief Get maximum number of streams
 * @details Returns the maximum number of concurrent streams supported
 * @return Maximum number of streams
 */
int hal_camera_get_max_stream(void);

/**
 * @brief Set callback operations
 * @details Registers callback functions for HAL to communicate with framework
 * @param hal_camera HAL context pointer
 * @param callback_ops Callback operations structure
 * @return 0 on success, negative error code on failure
 */
int hal_camera_set_callback_ops(void *hal_camera,
                                 const camera_callback_ops_t *callback_ops);

/**
 * @brief Process capture request
 * @details Handles a capture request from the framework
 * @param hal_camera HAL context pointer
 * @param request Capture request structure
 * @return 0 on success, negative error code on failure
 */
int hal_camera_process_capture_request(void *hal_camera,
                                       camera_capture_request_t *request);

/*===========================================================================
 * Camera Information
 *===========================================================================*/

/**
 * @brief Get number of cameras
 * @details Returns the total number of available camera devices
 * @return Number of camera devices
 */
int hal_camera_get_number(void);

/**
 * @brief Get number of camera groups
 * @details Returns the number of camera groups in the system
 * @return Number of camera groups
 */
int hal_camera_get_group_number(void);

/**
 * @brief Set torch mode
 * @details Controls the flash torch mode for the specified camera
 * @param camera_id Camera device identifier string
 * @param on True to enable torch mode, false to disable
 */
void hal_camera_set_torch_mode(const char *camera_id, bool on);

/**
 * @brief Get camera information
 * @details Retrieves static information about the specified camera device
 * @param camera_id Camera device identifier
 * @param info Output pointer to camera info structure
 * @return 0 on success, negative error code on failure
 */
int hal_camera_get_info(uint32_t camera_id, struct camera_info *info);

/*===========================================================================
 * AI Processing
 *===========================================================================*/

/**
 * @brief Process AI detection
 * @details Performs AI processing on the specified stream
 * @param hal_camera HAL context pointer
 * @param device_id Camera device identifier
 * @param channel_id Channel identifier
 * @param stream_id Stream identifier
 * @return 0 on success, negative error code on failure
 */
int hal_camera_ai_process(void *hal_camera, int device_id, int channel_id,
                           int stream_id);

/**
 * @brief Add AI processing type
 * @details Enables AI processing for the specified AI type
 * @param hal_camera HAL context pointer
 * @param ai AI type to enable
 * @return 0 on success, negative error code on failure
 */
int hal_camera_ai_add(void *hal_camera, camera_ai_type_t ai);

/**
 * @brief Remove AI processing type
 * @details Disables AI processing for the specified AI type
 * @param hal_camera HAL context pointer
 * @param ai AI type to disable
 * @return 0 on success, negative error code on failure
 */
int hal_camera_ai_remove(void *hal_camera, camera_ai_type_t ai);

/**
 * @brief Destroy all SVP channels and join AI worker threads (device close path).
 */
void hal_camera_ai_shutdown_all(void *hal_camera);

/**
 * @brief Calculate input size for tile overlap
 * @details Calculates the required input image size for tile overlap processing
 * @param tile_width Tile width in pixels
 * @param tile_height Tile height in pixels
 * @param num_rows Number of tile rows
 * @param num_cols Number of tile columns
 * @param overlap_ratio Overlap ratio between tiles (0.0 to 1.0)
 * @param input_width Output pointer to receive calculated input width
 * @param input_height Output pointer to receive calculated input height
 * @return 0 on success, negative error code on failure
 */
int hal_camera_ai_tile_overlap_calculate_input_size(int tile_width,
                                                     int tile_height,
                                                     int num_rows, int num_cols,
                                                     float overlap_ratio,
                                                     int *input_width,
                                                     int *input_height);

/**
 * @brief Enable tile overlap processing
 * @details Enables AI tile overlap processing mode
 * @param hal_camera HAL context pointer
 * @return 0 on success, negative error code on failure
 */
int hal_camera_ai_tile_overlap_enable(void *hal_camera);

/**
 * @brief Disable tile overlap processing
 * @details Disables AI tile overlap processing mode
 * @param hal_camera HAL context pointer
 * @return 0 on success, negative error code on failure
 */
int hal_camera_ai_tile_overlap_disable(void *hal_camera);

/**
 * @brief Whether VPSS channels are created on first enable (1) or at configure_streams (0).
 */
int hal_camera_lazy_channel_create_enabled(void);

/**
 * @brief Whether the platform VPSS/VI channel attributes are already configured.
 * @return 1 if configured, 0 if not (or on query failure).
 */
int hal_camera_vpss_chn_is_configured(void *hal_camera, int device_id, int channel_id);

/**
 * @brief Whether a channel's attrs can be (re)configured while the device is running.
 * @details AV100_V2 (VI) supports re-registering a channel via ak_vi_set_chn_attr_ex
 *          after the device is enabled, so a channel disabled on camera-app exit can be
 *          re-created on re-entry while the device is kept open. AV200 (VPSS) cannot:
 *          AK_MPI_VPSS_SetChnAttr after AK_MPI_VPSS_StartGrp hangs, so it returns 0.
 * @return 1 if live per-channel reconfigure is safe, 0 otherwise.
 */
int hal_camera_supports_live_chn_reconfigure(void *hal_camera, int device_id);

/*===========================================================================
 * Auto Focus (hardware internal API; framework reaches this via camera_hw_device_t)
 *===========================================================================*/

int hal_camera_af_is_supported(int device_id);
int hal_camera_af_get_sharpness(int device_id, uint32_t *score_out);
int hal_camera_af_set_focus_pos(int device_id, int pos);
int hal_camera_af_get_focus_pos(int device_id, int *pos_out);
int hal_camera_af_get_pos_range(int device_id, int *min_out, int *max_out);
int hal_camera_af_get_settle_ms(int device_id);
/** Unified AF entry (CAMERA_AF_CMD_*); see hardware/include/camera.h */
int hal_camera_af_control(int device_id, int cmd, void *arg);
/** Release AF/VCM handles (call from camera deinit). */
void hal_camera_af_release(void);

__END_DECLS

#endif /* HARDWARE_HAL_CAMERA_API_H */
