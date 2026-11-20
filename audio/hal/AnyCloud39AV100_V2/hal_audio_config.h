// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/audio.
 *
 * HAL audio configuration header.
 * - Defines audio HAL configuration structure.
 * - Provides configuration loading from INI file.
 */

#ifndef HARDWARE_HAL_AUDIO_CONFIG_HAL_ANYCLOUD39AV100_V2_HAL_AUDIO_CONFIG_H
#define HARDWARE_HAL_AUDIO_CONFIG_HAL_ANYCLOUD39AV100_V2_HAL_AUDIO_CONFIG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HAL audio configuration structure.
 * @details Contains all configuration parameters for audio HAL operations.
 */
typedef struct {
    int32_t ai_print_level;                 /**< Audio input print level */
    int32_t ai_gain;                        /**< Audio input gain */
    int32_t ai_drv_buf_periods;            /**< Audio input driver buffer periods */
    int32_t ai_thread_priority;            /**< Audio input thread priority */
    /*
     * ak_ai_set_aslc_attr before start_capture (ak_ai_sample sequence).
     * Q15 limiter + dB only; does not set Echo maxLenin (SDK default stays 128).
     */
    int32_t ai_aslc_limit;                 /**< ASLC limit (Q15 amplitude); 0=skip ak_ai_set_aslc_attr */
    int32_t ai_aslc_db;                    /**< ASLC volume in dB (-90~20) */

    int32_t ao_gain;                        /**< Audio output gain */
    int32_t ao_drv_buf_periods;            /**< Audio output driver buffer periods */
    int32_t ao_thread_priority;            /**< Audio output thread priority */

    int32_t loopback_drv_buf_periods;      /**< Loopback driver buffer periods */
    int32_t loopback_thread_priority;      /**< Loopback thread priority */

    int32_t send_frame_timeout_ms;          /**< Send frame timeout in milliseconds */
    int32_t receive_frame_timeout_ms;        /**< Receive frame timeout in milliseconds */

    /*
     * Audio config INI used by ak_ai_load_ch_conf / ak_ao_load_dev_conf
     * (same role as AV200 AK_MPI_AI/AO_LoadConf).
     */
    char loadconf_file_path[256];          /**< Full path to audio_config.ini for effect loadconf path */

    char loadconf_in_normal_section[64];    /**< Input normal section name */
    char loadconf_echo_section[64];         /**< Echo section name */
    char loadconf_out_normal_section[64];   /**< Output normal section name */

    char loadconf_in_dmic_section[64];      /**< Input DMIC section name */
    char loadconf_in_vad_section[64];       /**< Input VAD section name */
    char loadconf_in_cpu_cry_section[64];   /**< Input CPU cry section name */
    char loadconf_in_npu_cry_section[64];   /**< Input NPU cry section name */
    char loadconf_in_lssl_section[64];      /**< Input LSSL section name */

    /* AUDIO_EFFECT_* section names */
    char effect_npu_cry_section[64];       /**< NPU cry effect section name */
    char effect_cpu_cry_section[64];        /**< CPU cry effect section name */
    char effect_vad_section[64];            /**< VAD effect section name */
    char effect_lssl_section[64];           /**< LSSL effect section name */
    char effect_resample_section[64];       /**< Resample effect section name */

    /* Save/dump and frame length parameters */
    char save_pcm_file_path[128];          /**< Save PCM file path */
    char input_save_pcm_file_name[64];     /**< Input save PCM file name */
    char output_save_pcm_file_name[64];    /**< Output save PCM file name */
    int32_t input_save_pcm_file_second;    /**< Input save PCM file duration in seconds */
    int32_t output_save_pcm_file_second;    /**< Output save PCM file duration in seconds */
    int32_t send_frame_len;                /**< Send frame length */

    /* Mic hardware selection (optional; can be auto-detected from DTB at runtime) */
    int32_t mic_type;                      /**< 0=auto, 1=amic(adc), 2=dmic(pdm) */
    int32_t mic_num;                       /**< 0=auto, otherwise force mic count */

    /*
     * libakaudiofilter is not reentrant against FillAdcStream / GetDacStream.
     * 1 = postpone volume/mute/ASLC writes until the next open; drain before close.
     */
    int32_t vqe_defer_ctrl;                /**< 1=postpone volume/mute/ASLC writes to next open */
    int32_t vqe_drain_close_ms;            /**< >0=max ms to drain Echo PCM buffer before close */
} hal_audio_config_t;

/**
 * @brief Load audio HAL configuration from INI file.
 * @return Pointer to static configuration struct (never NULL).
 *         On error, returns struct filled with built-in defaults.
 */
hal_audio_config_t *hal_audio_get_config(void);

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_HAL_AUDIO_CONFIG_HAL_ANYCLOUD39AV100_V2_HAL_AUDIO_CONFIG_H */

