// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/audio.
 *
 * HAL audio API header.
 * - Defines platform-specific audio HAL interface functions.
 * - Provides audio initialization, I/O, effects, and codec operations.
 */
#ifndef HARDWARE_HAL_AUDIO_API_H
#define HARDWARE_HAL_AUDIO_API_H

#if defined(KERNEL_AOS)
#include <stdbool.h>
struct audio_device;
struct stream_in;
/* audio.h already has the effect enum. Keep the int alias only for
 * translation units that do not include that header. */
#if !defined(HARDWARE_AUDIO_H)
typedef int audio_effect_type_t;
#endif
#else
#include "audio_hw.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * HAL Audio Lifecycle
 * ============================================================================ */

/**
 * @brief Initialize HAL audio context.
 * @param adev Audio device pointer.
 * @return HAL audio context pointer on success, NULL on failure.
 */
void *hal_audio_init(struct audio_device *adev);

/**
 * @brief Deinitialize HAL audio context.
 * @param hal_audio HAL audio context pointer.
 */
void hal_audio_deinit(void *hal_audio);

/* ============================================================================
 * HAL Audio I/O Operations
 * ============================================================================ */

/**
 * @brief Open audio input stream.
 * @param hal_audio HAL audio context pointer.
 * @return 0 on success, negative error code on failure.
 */
int hal_audio_input_open(void *hal_audio);

/**
 * @brief Hook after framework open_input_stream completes.
 * @param hal_audio HAL audio context pointer.
 * @return 1 if framework should start callback capture thread, 0 otherwise.
 */
int hal_audio_on_input_stream_opened(void *hal_audio);

/**
 * @brief Stop AI capture without closing the device (unblocks capture thread).
 * @param hal_audio HAL audio context pointer.
 * @return 0 on success, negative error code on failure.
 */
int hal_audio_input_stop_capture(void *hal_audio);

/**
 * @brief Close audio input stream.
 * @param hal_audio HAL audio context pointer.
 * @return 0 on success, negative error code on failure.
 */
int hal_audio_input_close(void *hal_audio);

/**
 * @brief Open audio output stream.
 * @param hal_audio HAL audio context pointer.
 * @return 0 on success, negative error code on failure.
 */
int hal_audio_output_open(void *hal_audio);

/**
 * @brief Close audio output stream.
 * @param hal_audio HAL audio context pointer.
 * @return 0 on success, negative error code on failure.
 */
int hal_audio_output_close(void *hal_audio);

/**
 * @brief Write PCM data to output stream.
 * @param hal_audio HAL audio context pointer.
 * @param send_pcm_data PCM data buffer.
 * @param send_pcm_len PCM data length in bytes.
 * @return 0 on success, negative error code on failure.
 */
int hal_audio_pcm_write(void *hal_audio, unsigned char *send_pcm_data, int send_pcm_len);

/**
 * @brief Read PCM data from input stream.
 * @param hal_audio HAL audio context pointer.
 * @param buffer Output buffer pointer (set by function).
 * @param frame_number Output frame number pointer.
 * @param timestamp Output timestamp pointer.
 * @param bytes Requested bytes to read.
 * @return Actual bytes read.
 */
size_t hal_audio_pcm_read(void *hal_audio, void **buffer, uint32_t *frame_number,
                          uint64_t *timestamp, size_t bytes);

/**
 * @brief Enable/disable audio input dump to file.
 * @param hal_audio HAL audio context pointer.
 * @param file_path File path for dump.
 * @param file_name File name for dump.
 * @param enable Enable flag (1=enabled, 0=disabled).
 * @return 0 on success, negative error code on failure.
 */
int hal_audio_in_dump(void *hal_audio, char *file_path, char *file_name, int enable);

/**
 * @brief Enable/disable audio output dump to file.
 * @param hal_audio HAL audio context pointer.
 * @param file_path File path for dump.
 * @param file_name File name for dump.
 * @param enable Enable flag (1=enabled, 0=disabled).
 * @return 0 on success, negative error code on failure.
 */
int hal_audio_out_dump(void *hal_audio, char *file_path, char *file_name, int enable);

/**
 * @brief Get input frame length.
 * @param hal_audio HAL audio context pointer.
 * @return Frame length in bytes.
 */
size_t hal_audio_input_get_frame_len(void *hal_audio);

/* ============================================================================
 * HAL Audio Volume Control
 * ============================================================================ */

/**
 * @brief Set output volume.
 * @param hal_audio HAL audio context pointer.
 * @param volume Volume level (0.0 to 1.0).
 * @return 0 on success, negative error code on failure.
 */
int hal_audio_output_volume(void *hal_audio, float volume);

/**
 * @brief Set input volume.
 * @param hal_audio HAL audio context pointer.
 * @param volume Volume level (0.0 to 1.0).
 * @return 0 on success, negative error code on failure.
 */
int hal_audio_input_volume(void *hal_audio, float volume);

/**
 * @brief Enable audio loopback.
 * @param hal_audio HAL audio context pointer.
 * @return 0 on success, negative error code on failure.
 */
int hal_audio_loopback(void *hal_audio);

/**
 * @brief Set microphone mute state.
 * @param hal_audio HAL audio context pointer.
 * @param mute_state Mute state (true=muted, false=unmuted).
 * @return 0 on success, negative error code on failure.
 */
int hal_audio_set_mic_mute(void *hal_audio, bool mute_state);

/**
 * @brief Get microphone mute state.
 * @param hal_audio HAL audio context pointer.
 * @return true if muted, false otherwise.
 */
bool hal_audio_get_mic_mute(void *hal_audio);

/* ============================================================================
 * HAL Audio Effects
 * ============================================================================ */

/**
 * @brief Add audio effect to input stream.
 * @param hal_audio HAL audio context pointer.
 * @param effect Effect type to add.
 * @return 0 on success, negative error code on failure.
 */
int hal_audio_effect_in_add(void *hal_audio, audio_effect_type_t effect);

/**
 * @brief Remove audio effect from input stream.
 * @param hal_audio HAL audio context pointer.
 * @param effect Effect type to remove.
 * @return 0 on success, negative error code on failure.
 */
int hal_audio_effect_in_remove(void *hal_audio, audio_effect_type_t effect);

/**
 * @brief Add audio effect to output stream.
 * @param hal_audio HAL audio context pointer.
 * @param effect Effect type to add.
 * @return 0 on success, negative error code on failure.
 */
int hal_audio_effect_out_add(void *hal_audio, audio_effect_type_t effect);

/**
 * @brief Remove audio effect from output stream.
 * @param hal_audio HAL audio context pointer.
 * @param effect Effect type to remove.
 * @return 0 on success, negative error code on failure.
 */
int hal_audio_effect_out_remove(void *hal_audio, audio_effect_type_t effect);

/**
 * @brief Process audio effects on input buffer.
 * @param hal_audio HAL audio context pointer.
 * @param buffer Audio buffer to process.
 * @param bytes Buffer size in bytes.
 * @return 0 on success, negative error code on failure.
 */
int hal_audio_effect_in_process(void *hal_audio, void *buffer, size_t bytes);

/**
 * @brief Process audio effects on output buffer.
 * @param hal_audio HAL audio context pointer.
 * @param buffer Audio buffer to process.
 * @param bytes Buffer size in bytes.
 * @return 0 on success, negative error code on failure.
 */
int hal_audio_effect_out_process(void *hal_audio, void *buffer, size_t bytes);

/**
 * @brief Load input effect configuration.
 * @param hal_audio HAL audio context pointer.
 * @return 0 on success, negative error code on failure.
 */
int hal_audio_effect_in_loadconf(void *hal_audio);

/**
 * @brief Load output effect configuration.
 * @param hal_audio HAL audio context pointer.
 * @return 0 on success, negative error code on failure.
 */
int hal_audio_effect_out_loadconf(void *hal_audio);

/* ============================================================================
 * HAL Audio Codec Operations
 * ============================================================================ */

/**
 * @brief Open audio codec for output.
 * @param hal_audio HAL audio context pointer.
 * @return 0 on success, negative error code on failure.
 */
int hal_audio_codec_out_open(void *hal_audio);

/**
 * @brief Open audio codec for input.
 * @param hal_audio HAL audio context pointer.
 * @param in Input stream being encoded (must match the stream passed to hal_audio_input_open; avoids races on adev->in).
 * @return 0 on success, negative error code on failure.
 */
int hal_audio_codec_in_open(void *hal_audio, struct stream_in *in);

/**
 * @brief Close audio codec for output.
 * @param hal_audio HAL audio context pointer.
 * @return 0 on success, negative error code on failure.
 */
int hal_audio_codec_out_close(void *hal_audio);

/**
 * @brief Close audio codec for input.
 * @param hal_audio HAL audio context pointer.
 * @return 0 on success, negative error code on failure.
 */
int hal_audio_codec_in_close(void *hal_audio);

/**
 * @brief Read encoded audio data from codec.
 * @param hal_audio HAL audio context pointer.
 * @param buffer Output buffer pointer (set by function).
 * @param frame_number Output frame number pointer.
 * @param timestamp Output timestamp pointer.
 * @param bytes Requested bytes to read.
 * @return Actual bytes read.
 */
size_t hal_audio_in_codec_read(void *hal_audio, void **buffer, uint32_t *frame_number,
                               uint64_t *timestamp, size_t bytes);

/**
 * @brief Enable audio codec input.
 * @param hal_audio HAL audio context pointer.
 * @return 0 on success, negative error code on failure.
 */
size_t hal_audio_in_codec_enable(void *hal_audio);

/**
 * @brief Disable audio codec input.
 * @param hal_audio HAL audio context pointer.
 * @return 0 on success, negative error code on failure.
 */
size_t hal_audio_in_codec_disable(void *hal_audio);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/**
 * @brief Sleep for specified milliseconds.
 * @param ms Milliseconds to sleep.
 */
void msleep(const int ms);

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_HAL_AUDIO_API_H */



