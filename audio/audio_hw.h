// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/audio.
 *
 * Audio hardware abstraction layer header.
 * - Defines audio device structures and types for HAL implementation.
 * - Contains stream management and configuration structures.
 */
#ifndef HARDWARE_AUDIO_HW_H
#define HARDWARE_AUDIO_HW_H

#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <sys/cdefs.h>
#include <sys/types.h>
#include <time.h>
#include <stdbool.h>

#include "hardware.h"
#include "audio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Type Definitions
 * ============================================================================ */

/**
 * @brief Audio device status enumeration.
 * @details Defines the possible states of an audio device.
 */
typedef enum {
    AUDIO_STATUS_DISABLED,                                        /**< Audio device is disabled */
    AUDIO_STATUS_ENABLED,                                        /**< Audio device is enabled and operational */
} audio_status_type_t; 

/**
 * @brief Base audio configuration structure.
 * @details Contains fundamental audio parameters for device configuration.
 */
struct audio_config_base {
    uint32_t sample_rate;                                         /**< Audio sample rate in Hz */
    audio_channel_mask_t channel_mask;                           /**< Channel configuration mask */
    audio_format_t format;                                       /**< Audio data format */
};

typedef struct audio_config_base audio_config_base_t;

/**
 * @brief Main audio device structure.
 * @details Represents a complete audio device with input/output streams and callbacks.
 */
struct audio_device {
    struct audio_hw_device device;                                 /**< Hardware device interface */
    audio_mode_t mode;                                             /**< Current audio mode (playback/record) */

    void *hal_audio;                                               /**< Hardware abstraction layer audio context */
    struct stream_out *out;                                         /**< Output stream pointer */
    struct stream_in *in;                                           /**< Input stream pointer */

    const audio_callback_ops_t *callback_ops;                     /**< Audio callback operations */

    pthread_mutex_t lock;                                          /**< Device synchronization mutex */
};

/**
 * @brief Audio output stream structure.
 * @details Manages audio playback stream with configuration and timing.
 */
struct stream_out {
    struct audio_stream_out stream;                                 /**< Output stream interface */
    struct audio_device *dev;                                       /**< Reference to parent audio device */

    int64_t last_write_time_us;                                     /**< Timestamp of last write operation in microseconds */

    uint32_t sample_rate;                                           /**< Output sample rate in Hz */
    audio_channel_mask_t channel_mask;                             /**< Output channel configuration */
    audio_format_t format;                                          /**< Output audio format */
    size_t frame_count;                                             /**< Number of audio frames in buffer */

    audio_io_handle_t handle;                                       /**< I/O handle for the stream */

    audio_devices_t devices;                                        /**< Associated audio devices */

    pthread_mutex_t lock;                                           /**< Stream synchronization mutex */
};

/**
 * @brief Audio input stream structure.
 * @details Manages audio recording stream with processing and effects.
 */
struct stream_in {
    struct audio_stream_in stream;                                    /**< Input stream interface */
    struct audio_device *dev;                                        /**< Reference to parent audio device */
    audio_io_handle_t handle;                                        /**< I/O handle for the stream */

    pthread_t thread_id;                                             /**< PCM and codec callback thread identifier */
    audio_status_type_t status;                                      /**< Current stream status */
    struct audio_config pcm_config;                                  /**< PCM configuration */
    struct audio_config input_config;                                /**< Input configuration */
    /** AENC output routing: must carry the encoded HAL format (e.g. ALAW), not input_config (often PCM/DEFAULT). */
    struct audio_config enc_out_config;
    audio_stream_buffer_t input_buffer;                              /**< Input data buffer */
    audio_stream_buffer_t output_codec_buffer;                      /**< Codec output buffer */
    audio_stream_buffer_t output_pcm_buffer;                        /**< PCM output buffer */

    pthread_mutex_t lock;                                           /**< Stream synchronization mutex */
    audio_devices_t devices;                                         /**< Associated audio devices */
    uint32_t in_frame_interval;                                      /**< Frame sleep interval */

    uint32_t sample_rate;                                            /**< Input sample rate in Hz */
    audio_channel_mask_t channel_mask;                               /**< Input channel configuration */
    audio_format_t format;                                           /**< Input audio format */

    size_t frame_count;                                               /**< Number of audio frames in buffer */
    int64_t last_read_time_us;                                       /**< Timestamp of last read operation in microseconds */

    int capture_started;                                             /**< Capture session status flag */
    bool standby;                                                    /**< Standby mode flag */
    bool reboot;                                                     /**< Reboot flag for stream restart */

    /* Audio effect detection parameters */
    audio_effect_detect_parameter cpu_cry_parameter;                 /**< CPU-based cry detection parameters */
    audio_effect_detect_parameter vad_parameter;                    /**< Voice Activity Detection parameters */
    audio_effect_detect_parameter npu_cry_parameter;                 /**< NPU-based cry detection parameters */
    audio_effect_data_parameter resample_parameter;                  /**< Audio resampling parameters */
    audio_effect_detect_parameter lssl_parameter;                    /**< LSSL (Low-Speed Speech Learning) parameters */
};

/**
 * Start the PCM/callback capture thread if not already running.
 * Call after input HAL open and after optional add_audio_effect (VAD/CRY/LSSL init)
 * so AK_MPI_AI_LoadConf/EnableChn are not concurrent with main-channel GetFrame.
 */
void audio_hw_stream_in_ensure_capture_started(struct audio_stream_in *stream_in);

/**
 * Join the PCM capture thread and stop SDK capture (ak_ai_stop_capture).
 * Leaves the AI handle open so the caller can close AO first, then ak_ai_close.
 * Echo FillAdcStream must be idle before Filter_Control in close.
 */
void audio_hw_stream_in_stop_sdk_capture(struct audio_stream_in *stream_in);

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_AUDIO_HW_H */
