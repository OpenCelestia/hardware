// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/audio.
 *
 * HAL audio implementation header for AnyCloud39AV100_V2 platform.
 * - Same HAL data layout as AnyCloud39AV200, plus ak_ai/ak_ao handles.
 */

#ifndef HARDWARE_HAL_AUDIO_HAL_ANYCLOUD39AV100_V2_HAL_AUDIO_H
#define HARDWARE_HAL_AUDIO_HAL_ANYCLOUD39AV100_V2_HAL_AUDIO_H

#include <pthread.h>

#include "../../audio_hw.h"
#include "audio.h"
#include "ak_audio_compat.h"
#if defined(__has_include)
#if __has_include("ak_audio_effect.h")
#include "ak_audio_effect.h"
#endif
#endif
#include "hal_audio_config.h"

/* Sole userspace thread that calls ak_ai_get_frame / release_frame. */
#define HAL_AI_GET_Q_SLOTS  3

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Logical AI input channel IDs (AK_MPI_AI_EnableChn / GetFrame).
 * @details Matches multi-channel capture layout: MAIN is opened in hal_audio_input_open;
 *          VAD / NPU cry / LSSL / CPU cry use dedicated indices for algorithms in audio_effect.c.
 *          Framework effect bits map to: VAD→ch1, NPU_CRY→ch2, LSSL→ch3, CPU_CRY→ch4.
 */
typedef enum {
    AUDIO_IN_CHANNEL_MAIN_ID    = 0,       /**< Main capture (PCM path) */
    AUDIO_IN_CHANNEL_VAD_ID     = 1,       /**< Voice activity detection */
    AUDIO_IN_CHANNEL_NPU_CRY_ID = 2,       /**< NPU-based cry detection */
    AUDIO_IN_CHANNEL_LSSL_ID    = 3,       /**< Line-array SSL (LSSL) */
    AUDIO_IN_CHANNEL_CPU_CRY_ID = 4,       /**< CPU-based cry detection */

    AUDIO_IN_CHANNEL_ID_USED_MAX = 5,     /**< Channel count (0..4) */
} audio_ai_type_t;



/**
 * @brief HAL audio data structure.
 * @details Contains audio device context, configuration, and state information.
 */
struct hal_audio_data {
    struct audio_device *adev;              /**< Audio device pointer */
    struct audio_effect_data *effect;       /**< Audio effect data pointer */
    hal_audio_config_t *cfg;                /**< Audio configuration pointer */
    /** Handles from ak_ai_open / ak_ao_open (-1 if closed) */
    int ai_handle_id;
    int ao_handle_id;
    int in_device_id;                       /**< Input device ID */
    int in_codec_id;                        /**< Input codec ID */
    int out_device_id;                      /**< Output device ID */
    int out_channel_id;                     /**< Output channel ID */
    int out_codec_id;                       /**< Output codec ID */

    int in_channel_main_id;                 /**< Main input channel ID */

    AUDIO_IN_PARAM_S in_param;              /**< Audio input parameters */
    AUDIO_OUT_PARAM_S out_param;            /**< Audio output parameters */

    FRAME_S  ai_frame;                      /**< Describes pcm_copy_buf after queue pop */
    unsigned char pcm_copy_buf[MAX_FRAME_LEN]; /**< Consumer copy from ai_get queue */
    AUDIO_STREAM_S aenc_frame;              /**< Audio encoder stream structure */

    /*
     * Single get_frame thread: only this thread calls ak_ai_get_frame/release.
     * Framework capture thread only pops PCM copies.
     */
    pthread_t ai_get_tid;
    volatile int ai_get_run;
    int ai_get_started;
    int ai_get_sleep_ms;
    pthread_mutex_t ai_get_q_lock;
    int ai_get_q_r;
    int ai_get_q_w;
    int ai_get_q_n;
    struct {
        unsigned int len;
        unsigned long long ts;
        unsigned long seq_no;
        unsigned char data[MAX_FRAME_LEN];
    } ai_get_q[HAL_AI_GET_Q_SLOTS];

    AUDIO_SAVE_FILE_INFO_S  audio_in_save_file_info;   /**< Audio input save file information */
    AUDIO_SAVE_FILE_INFO_S  audio_out_save_file_info;  /**< Audio output save file information */

    int in_volume;                          /**< Input volume level */
    int in_gain;                            /**< Input gain level */

    int out_volume;                         /**< Output volume level */
    int out_gain;                           /**< Output gain level */

    int out_volume_req;                     /**< Deferred AO volume (applied on next open) */
    bool mic_mute_req;                      /**< Deferred mic mute (applied on next open) */

    audio_mode_t in_mode;                   /**< Input audio mode */
    audio_mode_t out_mode;                  /**< Output audio mode */

    audio_effect_type_t in_effect;          /**< Input audio effect type */
    audio_effect_type_t out_effect;         /**< Output audio effect type */

    bool pcm_buffer_release_flag;           /**< Copy in pcm_copy_buf is valid */
    bool in_codec_buffer_release_flag;      /**< Input codec buffer release flag */

    bool output_loadconf_flag;              /**< Output configuration load flag */
    bool input_loadconf_flag;               /**< Input configuration load flag */
    bool mic_mute_state;                    /**< Microphone mute state */
    bool playing_flag;                      /**< Playing flag */
    bool recording_flag;                     /**< Recording flag */
    bool start_capture_pending;              /**< CALL: start_capture deferred until AO open */
    bool encoding_flag;                      /**< Encoding flag */
    bool in_codec_thread_started;            /**< AENC get_stream thread was created */
    bool aac_adts_rate_synced;              /**< AO rate matched to AAC ADTS header */
};

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_HAL_AUDIO_HAL_ANYCLOUD39AV100_V2_HAL_AUDIO_H */
