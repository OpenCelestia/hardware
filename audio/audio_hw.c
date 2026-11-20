// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/audio.
 *
 * Audio hardware abstraction layer implementation.
 * - Implements audio_hw_device_t methods for audio operations.
 * - Manages input/output streams and audio device lifecycle.
 * - Handles audio configuration, volume control, and effect processing.
 */
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <sys/cdefs.h>
#include <sys/types.h>
#include <stdbool.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#include "audio_hw.h"
#include "hal_audio_api.h"
#include "audio.h"
#include "log.h"

#define LOG_TAG "HAL AUDIO"

/* ============================================================================
 * Constants and Macros
 * ============================================================================ */



#define AUDIO_VOLUME_MIN 0.0f
#define AUDIO_VOLUME_MAX 1.0f


#define AUDIO_DEFAULT_INPUT_SAMPLE_RATE   16000  /* 16kHz required for audio effects (AEC, VAD, etc.) */
#define AUDIO_DEFAULT_OUTPUT_SAMPLE_RATE   44100

#define AUDIO_DEFAULT_AUDIO_FORMAT  AUDIO_FORMAT_PCM_16_BIT

#define AUDIO_INPUT_BUFFER_MILLISECONDS       (20)                /**< Input buffer size in milliseconds */
#define AUDIO_INPUT_DEFAULT_CHANNEL_MASK      AUDIO_CHANNEL_IN_MONO    /**< Default input channel mask */

#define AUDIO_OUTPUT_BUFFER_MILLISECONDS      (10)                /**< Output buffer size in milliseconds */
#define AUDIO_OUTPUT_DEFAULT_CHANNEL_MASK     AUDIO_CHANNEL_OUT_MONO   /**< Default output channel mask */

static int audio_pcm16_peak(const void *buf, size_t bytes)
{
    const int16_t *samples = (const int16_t *)buf;
    size_t n = bytes / sizeof(int16_t);
    int peak = 0;

    if (buf == NULL || bytes < sizeof(int16_t))
        return 0;

    for (size_t i = 0; i < n; i++) {
        int v = (int)samples[i];
        if (v < 0)
            v = -v;
        if (v > peak)
            peak = v;
    }
    return peak;
}

static void audio_log_preenc_pcm_peak(size_t bytes_read, int peak, int encoded_path)
{
    static unsigned int s_preenc_log_cnt;
    static int s_preenc_max_peak;

    if (peak > s_preenc_max_peak)
        s_preenc_max_peak = peak;
    /* Keep quiet even with DEBUG: first few + every ~500 frames. */
    if (s_preenc_log_cnt < 4u || (s_preenc_log_cnt % 500u) == 0u) {
        log_debug("audio_preenc_pcm #%u: bytes=%zu peak=%d max_peak=%d enc_path=%d",
                  s_preenc_log_cnt, bytes_read, peak, s_preenc_max_peak, encoded_path);
    }
    s_preenc_log_cnt++;
}

/**
 * @brief Get output stream sample rate.
 * @param stream Audio stream pointer.
 * @return Sample rate in Hz.
 */
static uint32_t out_get_sample_rate(const struct audio_stream *stream)
{
    const struct stream_out *out = (struct stream_out *)stream;

    log_debug("out_get_sample_rate: %u", out->sample_rate);
    return out->sample_rate;
}

/**
 * @brief Set output stream sample rate.
 * @param stream Audio stream pointer.
 * @param rate Sample rate in Hz.
 * @return 0 on success, negative error code on failure.
 */
static int out_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    struct stream_out *out = (struct stream_out *)stream;

    log_debug("out_set_sample_rate: %d", rate);
    out->sample_rate = rate;
    return HARDWARE_AUDIO_OK;
}


/**
 * @brief Get bytes per sample for given audio format.
 * @param format Audio format.
 * @return Bytes per sample.
 * @note Currently returns sizeof(int16_t) for all formats.
 */
static inline size_t audio_bytes_per_sample(audio_format_t format)
{
    (void)format;
    return sizeof(int16_t);
}

/**
 * @brief Get channel count from channel mask.
 * @param channel Channel mask.
 * @return Number of channels.
 * @note The return value is undefined if the channel mask is invalid.
 */
static inline uint32_t audio_channel_mask_get_bits(audio_channel_mask_t channel)
{
    return channel & ((1 << AUDIO_CHANNEL_COUNT_MAX) - 1);
}

typedef enum {
    AUDIO_CHANNEL_REPRESENTATION_POSITION   = 0x0u,
    AUDIO_CHANNEL_REPRESENTATION_INDEX      = 0x2u,
} audio_channel_representation_t;

/* Returns the number of channels from an output channel mask,
 * used in the context of audio output or playback.
 * If a channel bit is set which could _not_ correspond to an output channel,
 * it is excluded from the count.
 * Returns zero if the representation is invalid.
 */
static inline uint32_t audio_channel_count_from_out_mask(audio_channel_mask_t channel)
{
    uint32_t main = (channel & AUDIO_CHANNEL_OUT_MAIN_MASK) >> AUDIO_CHANNEL_OUT_MAIN_BIT;

    if (main > 0)
        return main;
    if (channel == AUDIO_CHANNEL_OUT_MONO)
        return 1;
    if (channel == AUDIO_CHANNEL_OUT_STEREO)
        return 2;
    return 0;
}


static inline size_t audio_bytes_per_frame(uint32_t channel_count, audio_format_t format)
{
    // cannot overflow for reasonable channel_count
    return channel_count * audio_bytes_per_sample(format);
}
/**
 * return the frame size (number of bytes per sample).
 *
 * Deprecated: use audio_stream_out_frame_size() or audio_stream_in_frame_size() instead.
 */
static inline size_t audio_stream_frame_size(const struct audio_stream *s)
{
    size_t chan_samp_sz;
    audio_format_t format = s->get_format(s);

    if (audio_has_proportional_frames(format)) {
        chan_samp_sz = audio_bytes_per_sample(format);
        return __builtin_popcount(s->get_channels(s)) * chan_samp_sz;
    }

    return sizeof(int8_t);
}

/**
 * return the frame size (number of bytes per sample) of an output stream.
 */
static inline size_t audio_stream_out_frame_size(const struct audio_stream_out *s)
{
    size_t chan_samp_sz;
    audio_format_t format = s->common.get_format(&s->common);

    if (audio_has_proportional_frames(format)) {
        chan_samp_sz = audio_bytes_per_sample(format);
        return audio_channel_count_from_out_mask(s->common.get_channels(&s->common)) * chan_samp_sz;
    }

    return sizeof(int8_t);
}
/* Returns the number of channels from an input channel mask,
 * used in the context of audio input or recording.
 * If a channel bit is set which could _not_ correspond to an input channel,
 * it is excluded from the count.
 * Returns zero if the representation is invalid.
 */
static inline  uint32_t audio_channel_count_from_in_mask(audio_channel_mask_t channel)
{
    uint32_t bits = audio_channel_mask_get_bits(channel);

        return __builtin_popcount(bits);

}
/**
 * return the frame size (number of bytes per sample) of an input stream.
 */
static inline size_t audio_stream_in_frame_size(const struct audio_stream_in *s)
{
    size_t chan_samp_sz;
    audio_format_t format = s->common.get_format(&s->common);

    if (audio_has_proportional_frames(format)) {
        chan_samp_sz = audio_bytes_per_sample(format);
        return audio_channel_count_from_in_mask(s->common.get_channels(&s->common)) * chan_samp_sz;
    }

    return sizeof(int8_t);
}


/**
 * @brief Get output stream buffer size.
 * @param stream Audio stream pointer.
 * @return Buffer size in bytes.
 */
static size_t out_get_buffer_size(const struct audio_stream *stream)
{
    const struct stream_out *out = (const struct stream_out *)stream;
    size_t buffer_size = out->frame_count *
                         audio_stream_out_frame_size(&out->stream);

    log_debug("out_get_buffer_size: %zu", buffer_size);
    return buffer_size;
}

static audio_channel_mask_t out_get_channels(const struct audio_stream *stream)
{
    const struct stream_out *out = (const struct stream_out *)stream;

    log_debug("out_get_channels: %x", out->channel_mask);
    return out->channel_mask;
}

static audio_format_t out_get_format(const struct audio_stream *stream)
{
    const struct stream_out *out = (const struct stream_out *)stream;

    log_debug("out_get_format: %d", out->format);
    return out->format;
}

/**
 * @brief Set output stream audio format.
 * @param stream Audio stream pointer.
 * @param format Audio format to set.
 * @return HARDWARE_AUDIO_OK on success, error code on failure.
 */
static int out_set_format(struct audio_stream *stream, audio_format_t format)
{
    struct stream_out *out = (struct stream_out *)stream;

    log_debug("out_set_format: %d", format);
    out->format = format;
    return HARDWARE_AUDIO_OK;
}


/**
 * @brief Enable/disable output stream dump to file.
 * @param stream Audio stream pointer.
 * @param file_path Directory path for dump file.
 * @param file_name File name for dump.
 * @param enable Enable flag (1=enabled, 0=disabled).
 * @return HARDWARE_AUDIO_OK on success, error code on failure.
 */
static int out_dump(const struct audio_stream *stream, char *file_path,char *file_name, int enable)
{
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;

    const bool locked = (pthread_mutex_trylock(&out->lock) == 0);

    hal_audio_out_dump(adev->hal_audio, file_path, file_name, enable);


    if (locked) {
        pthread_mutex_unlock(&out->lock);
    }

    log_debug("out_dump");
    return HARDWARE_AUDIO_OK;
}




/**
 * @brief Set output stream parameters.
 * @param stream Audio stream pointer.
 * @param name Parameter name (e.g., "Volume").
 * @param data Parameter value pointer.
 * @param len Parameter data length.
 * @return HARDWARE_AUDIO_OK on success, error code on failure.
 * @note Currently supports "Volume" parameter only.
 */
static int out_set_parameters(const struct audio_stream *stream,   const char* name, void *data, unsigned int len)
{
    log_debug("out_set_parameters");
    struct stream_out *out = (struct stream_out *)stream;
    struct str_parms *parms;
    char value[32];

    int ret;

    pthread_mutex_lock(&out->lock);

    if(strcmp("Volume",name)== 0)
    {
        float volume = *(float*)data;

        if(volume < AUDIO_VOLUME_MIN)
            volume = AUDIO_VOLUME_MIN;
        else if(volume > AUDIO_VOLUME_MAX)
            volume = AUDIO_VOLUME_MAX;

        ret = hal_audio_output_volume(out->dev->hal_audio, volume);
    }
    else
    {
        log_error( " set (%s) failed , not support !", value);
    }

    pthread_mutex_unlock(&out->lock);

    return HARDWARE_AUDIO_OK;
}

/**
 * @brief Get output stream parameters.
 * @param stream Audio stream pointer.
 * @param keys Parameter keys to query.
 * @return Parameter string (currently always returns NULL).
 */
static char * out_get_parameters(const struct audio_stream *stream, const char *keys)
{
    log_debug("out_get_parameters");
//    return strdup("");
    return NULL;
}

/**
 * @brief Get output stream latency.
 * @param stream Audio output stream pointer.
 * @return Latency in milliseconds (returns buffer size in ms).
 */
static uint32_t out_get_latency(const struct audio_stream_out *stream)
{
    log_debug("out_get_latency");
    return AUDIO_OUTPUT_BUFFER_MILLISECONDS;
}



/**
 * @brief Set output stream volume.
 * @param stream Audio output stream pointer.
 * @param volume Volume level (0.0 to 1.0, clamped to valid range).
 * @return HARDWARE_AUDIO_OK on success, error code on failure.
 */
static int out_set_volume(struct audio_stream_out *stream, float volume)
{
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;
    int ret;

    if(volume < AUDIO_VOLUME_MIN)
        volume = AUDIO_VOLUME_MIN;
    else if(volume > AUDIO_VOLUME_MAX)
        volume = AUDIO_VOLUME_MAX;

    ret = hal_audio_output_volume(adev->hal_audio, volume);

    return (ret == 0) ? HARDWARE_AUDIO_OK : HARDWARE_AUDIO_ERROR_OPERATION_FAILED;
}

/**
 * @brief Write audio data to output stream.
 * @param stream Audio output stream pointer.
 * @param buffer Audio data buffer.
 * @param bytes Number of bytes to write.
 * @return Number of bytes written (always equals bytes parameter).
 * @note Implements timing control to maintain proper playback rate.
 */
static ssize_t out_write(struct audio_stream_out *stream, void* buffer,
                         size_t bytes)
{
    struct stream_out *out = (struct stream_out *)stream;


    struct timespec t = { .tv_sec = 0, .tv_nsec = 0 };
    clock_gettime(CLOCK_MONOTONIC, &t);
    const int64_t now = (t.tv_sec * 1000000000LL + t.tv_nsec) / 1000;
    const int64_t elapsed_time_since_last_write = now - out->last_write_time_us;
    size_t frame_size = audio_stream_out_frame_size(stream);
    uint32_t rate = out_get_sample_rate(&stream->common);
    int64_t sleep_time = 0;

    if (frame_size > 0 && rate > 0) {
        sleep_time = (int64_t)bytes * 1000000LL / (int64_t)frame_size / (int64_t)rate
                     - elapsed_time_since_last_write;
    }


    if (sleep_time > 0) {
 //        usleep(sleep_time);
    } else {
        sleep_time = 0;
    }
    out->last_write_time_us = now + sleep_time;



    int written = hal_audio_pcm_write(out->dev->hal_audio, buffer, (int)bytes);
    if (written < 0)
        return -1;
    return written > 0 ? (ssize_t)written : (ssize_t)bytes;
}

/**
 * @brief Get render position from DSP.
 * @param stream Audio output stream pointer.
 * @param dsp_frames Output parameter for DSP frame count.
 * @return HARDWARE_AUDIO_ERROR_INVALID_PARAM (not implemented).
 */
static int out_get_render_position(const struct audio_stream_out *stream,
                                   uint32_t *dsp_frames)
{
    *dsp_frames = 0;
    log_debug("out_get_render_position: dsp_frames: %p", dsp_frames);
    return HARDWARE_AUDIO_ERROR_INVALID_PARAM;
}

/**
 * @brief Add audio effect to output stream.
 * @param stream Audio stream pointer.
 * @param effect Effect type to add.
 * @return HARDWARE_AUDIO_OK on success, error code on failure.
 * @note Note: Function incorrectly casts stream to stream_in, should be stream_out.
 */
static int out_add_audio_effect(const struct audio_stream *stream, audio_effect_type_t effect)
{
    struct stream_in *in = (struct stream_in *)stream;
    struct audio_device *adev = in->dev;
    log_debug("out_add_audio_effect: %p", effect);

    hal_audio_effect_out_add(adev->hal_audio, effect);

    return HARDWARE_AUDIO_OK;
}

/**
 * @brief Remove audio effect from output stream.
 * @param stream Audio stream pointer.
 * @param effect Effect type to remove.
 * @return HARDWARE_AUDIO_OK on success, error code on failure.
 * @note Note: Function incorrectly casts stream to stream_in, should be stream_out.
 */
static int out_remove_audio_effect(const struct audio_stream *stream, audio_effect_type_t effect)
{
    struct stream_in *in = (struct stream_in *)stream;
    struct audio_device *adev = in->dev;
    log_debug("out_remove_audio_effect: %p", effect);


    hal_audio_effect_out_remove(adev->hal_audio, effect);
    return HARDWARE_AUDIO_OK;
}

/**
 * @brief Get next write timestamp.
 * @param stream Audio output stream pointer.
 * @param timestamp Output parameter for timestamp.
 * @return HARDWARE_AUDIO_ERROR_INVALID_PARAM (not implemented).
 */
static int out_get_next_write_timestamp(const struct audio_stream_out *stream,
                                        int64_t *timestamp)
{
    *timestamp = 0;
    log_debug("out_get_next_write_timestamp: %ld", (long int)(*timestamp));
    return HARDWARE_AUDIO_ERROR_INVALID_PARAM;
}



/* ============================================================================
 * Input Stream Implementation
 * ============================================================================ */

/**
 * @brief Get input stream sample rate.
 * @param stream Audio stream pointer.
 * @return Sample rate in Hz.
 */
static uint32_t in_get_sample_rate(const struct audio_stream *stream)
{
    const struct stream_in *in = (const struct stream_in *)stream;

    log_debug("in_get_sample_rate: %u", in->sample_rate);
    return in->sample_rate;
}

/**
 * @brief Set input stream sample rate.
 * @param stream Audio stream pointer.
 * @param rate Sample rate in Hz.
 * @return HARDWARE_AUDIO_OK on success, error code on failure.
 */
static int in_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    struct stream_in *in = (struct stream_in *)stream;

    log_debug("in_set_sample_rate: %u", rate);
    in->sample_rate = rate;
    return HARDWARE_AUDIO_OK;
}

/**
 * @brief Get input stream buffer size.
 * @param stream Audio stream pointer.
 * @return Buffer size in bytes.
 */
static size_t in_get_buffer_size(const struct audio_stream *stream)
{
    const struct stream_in *in = (const struct stream_in *)stream;
    size_t buffer_size = in->frame_count *
                         audio_stream_in_frame_size(&in->stream);

    log_debug("in_get_buffer_size: %zu", buffer_size);
    return buffer_size;
}

/**
 * @brief Get input stream channel mask.
 * @param stream Audio stream pointer.
 * @return Channel mask value.
 */
static audio_channel_mask_t in_get_channels(const struct audio_stream *stream)
{
    const struct stream_in *in = (const struct stream_in *)stream;

    log_debug("in_get_channels: %x", in->channel_mask);
    return in->channel_mask;
}

/**
 * @brief Get input stream audio format.
 * @param stream Audio stream pointer.
 * @return Audio format value.
 */
static audio_format_t in_get_format(const struct audio_stream *stream)
{
    const struct stream_in *in = (const struct stream_in *)stream;

    log_debug("in_get_format: %d", in->format);
    return in->format;
}

/**
 * @brief Set input stream audio format.
 * @param stream Audio stream pointer.
 * @param format Audio format to set.
 * @return HARDWARE_AUDIO_OK on success, error code on failure.
 */
static int in_set_format(struct audio_stream *stream, audio_format_t format)
{
    struct stream_in *in = (struct stream_in *)stream;

    log_debug("in_set_format: %d", format);
    in->format = format;
    return HARDWARE_AUDIO_OK;
}

/**
 * @brief Put input stream into standby mode.
 * @param stream Audio stream pointer.
 * @return HARDWARE_AUDIO_OK on success, error code on failure.
 */
static int in_standby(struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;
    in->last_read_time_us = 0;
    in->standby = true;

    return HARDWARE_AUDIO_OK;
}

/**
 * @brief Enable/disable input stream dump to file.
 * @param stream Audio stream pointer.
 * @param file_path Directory path for dump file.
 * @param file_name File name for dump.
 * @param enable Enable flag (1=enabled, 0=disabled).
 * @return HARDWARE_AUDIO_OK on success, error code on failure.
 */
static int in_dump(const struct audio_stream *stream, char *file_path,char *file_name, int enable)
{
    struct stream_in *in = (struct stream_in *)stream;
    struct audio_device *adev = in->dev;

    hal_audio_in_dump(adev->hal_audio, file_path, file_name, enable);
    return HARDWARE_AUDIO_OK;
}

/**
 * @brief Set input stream parameters.
 * @param stream Audio stream pointer.
 * @param name Parameter name (e.g., "AIParam", "Vad", "NpuCry", "CpuCry", "Lssl", "AIReboot").
 * @param value Parameter value pointer.
 * @param len Parameter data length.
 * @return HARDWARE_AUDIO_OK on success, error code on failure.
 * @note Supported parameters:
 *   - "AIParam": Audio configuration (format, channel_mask, sample_rate)
 *   - "Vad": Voice Activity Detection sensitivity (0.0-1.0)
 *   - "NpuCry": NPU cry detection sensitivity (0.0-1.0)
 *   - "CpuCry": CPU cry detection sensitivity (0.0-1.0)
 *   - "Lssl": LSSL parameter sensitivity (0.0-1.0)
 *   - "AIReboot": AI reboot flag (1=reboot, 0=normal)
 */
static int in_set_parameters(const struct audio_stream *stream,  const char* name, void *value, unsigned int len)
{
    int ret = 0, val = 0;
    struct stream_in *in = (struct stream_in *)stream;

    pthread_mutex_lock(&in->lock);

    if(strcmp("AIParam",name)== 0)
    {
            audio_config_base_t *param = (audio_config_base_t *) value;
            in->format = param->format;
            in->channel_mask = param->channel_mask;
            in->sample_rate  = param->sample_rate;
    }
    else if(strcmp("Vad",name)== 0)
    {
        audio_effect_detect_parameter *parameter = (audio_effect_detect_parameter*)value;
        if(parameter->sensitivity > 1) parameter->sensitivity = 1;
        else if (parameter->sensitivity < 0) parameter->sensitivity = 0;

        in->vad_parameter.sensitivity = parameter->sensitivity;
    }
    else if(strcmp("NpuCry",name)== 0)
    {
        audio_effect_detect_parameter *parameter = (audio_effect_detect_parameter*)value;

        if(parameter->sensitivity > 1) parameter->sensitivity = 1;
        else if (parameter->sensitivity<0) parameter->sensitivity = 0;

        in->npu_cry_parameter.sensitivity = parameter->sensitivity;
    }
    else if(strcmp("CpuCry",name)== 0)
    {
        audio_effect_detect_parameter *parameter = (audio_effect_detect_parameter*)value;

        if(parameter->sensitivity > 1) parameter->sensitivity = 1;
        else if (parameter->sensitivity<0) parameter->sensitivity = 0;

        in->cpu_cry_parameter.sensitivity = parameter->sensitivity;
    }
    else if(strcmp("Lssl",name)== 0)
    {
        audio_effect_detect_parameter *parameter = (audio_effect_detect_parameter*)value;

        if(parameter->sensitivity > 1) parameter->sensitivity = 1;
        else if (parameter->sensitivity<0) parameter->sensitivity = 0;

        in->lssl_parameter.sensitivity = parameter->sensitivity;
    }
    else if(strcmp("AIReboot",name)== 0)
    {
        val = atoi(value);
        if(val == 1)
            in->reboot = 1;
    }
    else
    {
        log_error( " set (%s) failed , not support !", name);
    }

    pthread_mutex_unlock(&in->lock);

    (void)ret;
    return HARDWARE_AUDIO_OK;
}

/**
 * @brief Get input stream parameters.
 * @param stream Audio stream pointer.
 * @param keys Parameter keys to query.
 * @return Parameter string (currently always returns NULL).
 */
static char * in_get_parameters(const struct audio_stream *stream,
                                const char *keys)
{
    return NULL;
//    return strdup("");
}

/**
 * @brief Set input stream volume.
 * @param stream Audio input stream pointer.
 * @param volume Volume level (0.0 to 1.0, clamped to valid range).
 * @return HARDWARE_AUDIO_OK on success, error code on failure.
 */
static int in_set_volume(struct audio_stream_in *stream, float volume)
{
    struct stream_in *in = (struct stream_in *)stream;
    struct audio_device *adev = in->dev;

    if(volume < AUDIO_VOLUME_MIN)
        volume = AUDIO_VOLUME_MIN;
    else if(volume > AUDIO_VOLUME_MAX)
        volume = AUDIO_VOLUME_MAX;

    int ret = hal_audio_input_volume(adev->hal_audio, volume);

    return (ret == 0) ? HARDWARE_AUDIO_OK : HARDWARE_AUDIO_ERROR_OPERATION_FAILED;
}




/**
 * @brief Read audio data from input stream.
 * @param stream Audio input stream pointer.
 * @param buffer Audio data buffer.
 * @param bytes Number of bytes to read.
 * @return Number of bytes actually read.
 * @note Handles both PCM and encoded formats. Applies audio effects processing.
 *       Implements timing control to maintain proper capture rate.
 */
static ssize_t in_read(struct audio_stream_in *stream, void* buffer,
                       size_t bytes)
{
    struct stream_in *in = (struct stream_in *)stream;
    struct audio_device *adev = in->dev;
    size_t bytes_read = 0;

    audio_hw_stream_in_ensure_capture_started(stream);
    uint32_t frame_number;
    uint64_t timestamp;

    bytes_read = hal_audio_pcm_read(adev->hal_audio, buffer,&frame_number, &timestamp,bytes);

    if((in->format & AUDIO_FORMAT_MAIN_MASK) != AUDIO_FORMAT_PCM)
    {
        bytes_read = hal_audio_in_codec_read(adev->hal_audio, buffer,&frame_number, &timestamp,bytes);
    }

//    bytes_read = hal_audio_pcm_read(adev->hal_audio, buffer, bytes);
//need check effect type

    if (adev->mode == AUDIO_MODE_CALL)
        hal_audio_effect_in_process(adev->hal_audio, buffer, bytes_read);

    /* timing for audio input */
     struct timespec t = { .tv_sec = 0, .tv_nsec = 0 };
    clock_gettime(CLOCK_MONOTONIC, &t);
    const int64_t now = (t.tv_sec * 1000000000LL + t.tv_nsec) / 1000;

    // a full sleep when exiting standby.
    const int64_t elapsed_time_since_last_read = in->last_read_time_us;
    int64_t sleep_time = bytes * 1000000LL / audio_stream_in_frame_size(stream) /
            in_get_sample_rate(&stream->common) - elapsed_time_since_last_read;

    if(sleep_time>10000)   sleep_time = 10000;


    if (sleep_time > 0) {
        usleep(sleep_time);
    } else {
        sleep_time = 0;
    }
    in->last_read_time_us = now + sleep_time;

    return bytes_read;
}


/**
 * @brief Calculate number of samples for given duration.
 * @param milliseconds Duration in milliseconds.
 * @param sample_rate Sample rate in Hz.
 * @param channel_count Number of audio channels.
 * @return Number of samples.
 */
static size_t samples_per_milliseconds(size_t milliseconds,
                                       uint32_t sample_rate,
                                       size_t channel_count)
{
    return milliseconds * sample_rate * channel_count / 1000;
}

/**
 * @brief Calculate number of samples (microseconds-based calculation).
 * @param milliseconds Duration in milliseconds (note: function name suggests microseconds but parameter is milliseconds).
 * @param sample_rate Sample rate in Hz.
 * @param channel_count Number of audio channels.
 * @return Number of samples.
 * @note Function name suggests microseconds but parameter is actually milliseconds.
 */
static size_t samples_per_us(size_t milliseconds,
                                       uint32_t sample_rate,
                                       size_t channel_count)
{
    return milliseconds * sample_rate * channel_count;
}




/**
 * @brief Audio capture processing thread function.
 * @param arg Input stream pointer (cast to struct stream_in*).
 * @return NULL on exit.
 * @details Continuously reads audio data from HAL, applies effects processing,
 *          and calls callback with processed audio buffers. Handles both PCM
 *          and encoded formats. Thread runs while stream status is AUDIO_STATUS_ENABLED.
 */
void* audio_process_capture_thread_function(void* arg) {
    struct stream_in *in = (struct stream_in *)arg;
    struct audio_device *adev = in->dev;
    size_t bytes_read = 0;

    log_info("audio_process_capture_thread enter");

    audio_capture_result_t result = {};
    uint32_t frame_number;
    uint64_t timestamp;
    void *buffer;
    int sleep_time;

    /* in_frame_interval is milliseconds (ak_audio_len_to_interval). Sample
     * uses ak_sleep_ms(); usleep() takes microseconds. Passing 16 here was
     * a 16us busy-poll of ak_ai_get_frame vs AiCapturePcmThread GetResult. */
    if (in->in_frame_interval > 0)
        sleep_time = (in->in_frame_interval / 2) > 0 ? (in->in_frame_interval / 2) : 16;
    else
        sleep_time = 16;

    if ((in->format & AUDIO_FORMAT_MAIN_MASK) != AUDIO_FORMAT_PCM)
        hal_audio_in_codec_enable(adev->hal_audio);

    while(in->status == AUDIO_STATUS_ENABLED)
    {

        bytes_read = hal_audio_pcm_read(adev->hal_audio, &buffer, &frame_number, &timestamp, 0);

        if (bytes_read > 0 && buffer != NULL)
        {
            /* Live: capture/AiCapturePcmThread already own Echo. effect_in_process
             * enters _SD_Filter_Control and SIGSEGVs on Speaker. Talk needs it. */
            if (adev->mode == AUDIO_MODE_CALL)
                hal_audio_effect_in_process(adev->hal_audio, buffer, bytes_read);
            audio_log_preenc_pcm_peak(bytes_read,
                audio_pcm16_peak(buffer, bytes_read),
                (in->format & AUDIO_FORMAT_MAIN_MASK) != AUDIO_FORMAT_PCM);

            if ((in->format & AUDIO_FORMAT_MAIN_MASK) != AUDIO_FORMAT_PCM) {
                /* ak_aenc_sample: get_frame_encode sends; get_stream delivers encoded data. */
                (void)hal_audio_in_codec_read(adev->hal_audio, &buffer, &frame_number, &timestamp, 0);
                continue;
            }

            result.frame_number = frame_number;
            result.timestamp = timestamp;
            in->output_pcm_buffer.config = &in->pcm_config;
            in->output_pcm_buffer.data = buffer;
            in->output_pcm_buffer.data_size = bytes_read;
            result.output_buffer = &in->output_pcm_buffer;

            if (adev->callback_ops != NULL && adev->callback_ops->process_capture_result != NULL)
                adev->callback_ops->process_capture_result(adev->callback_ops, &result);
        }
        else
        {
            usleep((useconds_t)sleep_time * 1000);
        }
    }

    if((in->format & AUDIO_FORMAT_MAIN_MASK) != AUDIO_FORMAT_PCM)
    {
        hal_audio_in_codec_disable(adev->hal_audio);
    }

    log_info("audio_process_capture_thread exit");
    return NULL;
}






/**
 * @brief Add audio effect to input stream.
 * @param stream Audio stream pointer.
 * @param effect Effect type to add (e.g., VAD, cry detection, resample).
 * @return HARDWARE_AUDIO_OK on success, error code on failure.
 */
static int in_add_audio_effect(const struct audio_stream *stream, audio_effect_type_t effect)
{
    struct stream_in *in = (struct stream_in *)stream;
    struct audio_device *adev = in->dev;

    pthread_mutex_lock(&in->lock);
    hal_audio_effect_in_add(adev->hal_audio, effect);
    pthread_mutex_unlock(&in->lock);

    return HARDWARE_AUDIO_OK;
}

/**
 * @brief Remove audio effect from input stream.
 * @param stream Audio stream pointer.
 * @param effect Effect type to remove.
 * @return HARDWARE_AUDIO_OK on success, error code on failure.
 */
static int in_remove_audio_effect(const struct audio_stream *stream, audio_effect_type_t effect)
{
    struct stream_in *in = (struct stream_in *)stream;
    struct audio_device *adev = in->dev;
    pthread_mutex_lock(&in->lock);
    hal_audio_effect_in_remove(adev->hal_audio, effect);
    pthread_mutex_unlock(&in->lock);

    return HARDWARE_AUDIO_OK;
}

/* ============================================================================
 * Audio Device Operations
 * ============================================================================ */

/**
 * @brief Open audio output stream.
 * @param dev Audio hardware device pointer.
 * @param handle I/O handle for the stream.
 * @param devices Audio device selection.
 * @param config Audio configuration (sample rate, format, channels).
 * @param stream_out Output parameter for opened stream.
 * @return HARDWARE_AUDIO_OK on success, error code on failure.
 * @note Creates and configures output stream, opens HAL output, loads effect configuration.
 */
static int adev_open_output_stream(struct audio_hw_device *dev,
                                   audio_io_handle_t handle,
                                   audio_devices_t devices,
                                   struct audio_config *config,
                                   struct audio_stream_out **stream_out)
{
    int ret = 0;
    log_debug("adev_open_output_stream.:%x",dev);

    *stream_out = NULL;
    struct stream_out *out =
            (struct stream_out *)calloc(1, sizeof(struct stream_out));
    if (!out)
        return HARDWARE_AUDIO_ERROR_MEMORY;

    struct audio_device *adev = (struct audio_device *)dev;

    if (devices == AUDIO_DEVICE_NONE)
        out->devices = AUDIO_DEVICE_OUT_SPEAKER;
    else
        out->devices = devices;

    //list_init(&out->device_list);
    //update_device_list(&out->device_list, devices, address, true /* add devices */);

    out->dev = adev;
    out->handle = handle;

    out->format =  config->format;
    out->channel_mask = config->channel_mask;
    out->sample_rate = config->sample_rate;

    /* Initialize output stream function pointers */
    out->stream.common.get_sample_rate = out_get_sample_rate;
    out->stream.common.set_sample_rate = out_set_sample_rate;
    out->stream.common.get_buffer_size = out_get_buffer_size;
    out->stream.common.get_channels = out_get_channels;
    out->stream.common.get_format = out_get_format;
    out->stream.common.set_format = out_set_format;
    out->stream.common.dump = out_dump;
    out->stream.common.set_parameters = out_set_parameters;
    out->stream.common.get_parameters = out_get_parameters;
    out->stream.common.add_audio_effect = out_add_audio_effect;
    out->stream.common.remove_audio_effect = out_remove_audio_effect;
    out->stream.set_volume = out_set_volume;
    out->stream.write = out_write;
    out->sample_rate = config->sample_rate;
    if (out->sample_rate == 0)
        out->sample_rate = AUDIO_DEFAULT_OUTPUT_SAMPLE_RATE;
    out->channel_mask = config->channel_mask;
    if (out->channel_mask == AUDIO_CHANNEL_NONE)
        out->channel_mask = AUDIO_OUTPUT_DEFAULT_CHANNEL_MASK;
    out->format = config->format;
    if (out->format == AUDIO_FORMAT_DEFAULT)
        out->format = AUDIO_DEFAULT_AUDIO_FORMAT;
    out->frame_count = samples_per_milliseconds(
                           AUDIO_OUTPUT_BUFFER_MILLISECONDS,
                           out->sample_rate, 1);

    log_debug("adev_open_output_stream: sample_rate: %u, channels: %x, format: %d,"
          " frames: %zu", out->sample_rate, out->channel_mask, out->format,
          out->frame_count);
    *stream_out = &out->stream;

    pthread_mutex_lock(&out->lock);
    adev->out = out;
    if(adev->mode == AUDIO_MODE_NONE)
        adev->mode = AUDIO_MODE_NORMAL;

    if (hal_audio_output_open(adev->hal_audio) != 0) {
        pthread_mutex_unlock(&out->lock);
        free(out);
        adev->out = NULL;
        log_error("adev_open_output_stream: hal_audio_output_open failed");
        return HARDWARE_AUDIO_ERROR_OPERATION_FAILED;
    }

    hal_audio_effect_out_loadconf(adev->hal_audio);

    if((out->format & AUDIO_FORMAT_MAIN_MASK) != AUDIO_FORMAT_PCM)
    {
        hal_audio_codec_out_open(adev->hal_audio);
    }

    pthread_mutex_unlock(&out->lock);

    return HARDWARE_AUDIO_OK;
}

/**
 * @brief Close audio output stream.
 * @param dev Audio hardware device pointer.
 * @param stream Output stream to close.
 * @note Closes HAL output and codec (if used), frees stream resources.
 */
static void adev_close_output_stream(struct audio_hw_device *dev,
                                     struct audio_stream_out *stream)
{
    log_debug("adev_close_output_stream...");

    struct audio_device *adev = (struct audio_device *)dev;
    struct stream_out *out = (struct stream_out *)stream;

    /* For encoded playback, stop/join decode->AO feeder thread first. */
    if((out->format & AUDIO_FORMAT_MAIN_MASK) != AUDIO_FORMAT_PCM)
    {
        hal_audio_codec_out_close(adev->hal_audio);
    }

    hal_audio_output_close(adev->hal_audio);

    pthread_mutex_lock(&adev->lock);
    if (adev->out == out)
        adev->out = NULL;
    pthread_mutex_unlock(&adev->lock);

    free(stream);
}

/**
 * @brief Set audio callback operations.
 * @param dev Audio hardware device pointer.
 * @param callback_ops Callback operations structure.
 * @return HARDWARE_AUDIO_OK on success, error code on failure.
 * @note Callbacks are used for audio capture result notifications.
 */
static int adev_set_callback_ops(const struct audio_hw_device *dev,
        const audio_callback_ops_t *callback_ops)
{
	log_debug("audio device set callback ops");

    struct audio_device *adev = (struct audio_device *)dev;

    pthread_mutex_lock(&adev->lock);

    adev->callback_ops = callback_ops;

    pthread_mutex_unlock(&adev->lock);

	log_debug("adev->callback_ops:%x", adev->callback_ops);

    return HARDWARE_AUDIO_OK;
}


/**
 * @brief Set audio device parameters.
 * @param stream Audio stream pointer (unused in current implementation).
 * @param name Parameter name.
 * @param value Parameter value.
 * @param len Parameter data length.
 * @return HARDWARE_AUDIO_ERROR_NOT_SUPPORTED (not implemented).
 */
static int adev_set_parameters(const struct audio_stream *stream,  const char* name, void *value, unsigned int len)
{
//    struct audio_device *adev = (struct audio_device *)//;
//    struct str_parms *parms;
    log_debug("adev_set_parameters");
//    pthread_mutex_lock(&adev->lock);



//    pthread_mutex_unlock(&adev->lock);

    return HARDWARE_AUDIO_ERROR_NOT_SUPPORTED;
}

/**
 * @brief Get audio device parameters.
 * @param dev Audio hardware device pointer.
 * @param keys Parameter keys to query.
 * @return Parameter string (currently returns uninitialized value).
 * @note Implementation is incomplete.
 */
static char * adev_get_parameters(const struct audio_hw_device *dev,
                                  const char *keys)
{
    struct audio_device *adev = (struct audio_device *)dev;
    char *str = NULL;

//    pthread_mutex_lock(&adev->lock);


//    pthread_mutex_unlock(&adev->lock);

    log_debug("%s: exit: returns - %s", __func__, str ? str : "(null)");

    return str;
}



/**
 * @brief Set audio device mode.
 * @param dev Audio hardware device pointer.
 * @param mode Audio mode (e.g., AUDIO_MODE_NORMAL, AUDIO_MODE_RINGTONE).
 * @return HARDWARE_AUDIO_OK on success, error code on failure.
 * @note When mode changes, reloads effect configuration for active streams.
 */
static int adev_set_mode(struct audio_hw_device *dev, audio_mode_t mode)
{
    struct audio_device *adev = (struct audio_device *)dev;

    log_debug("set mode:%d,%d",adev->mode,mode);

    pthread_mutex_lock(&adev->lock);
    if (adev->mode != mode) {
        /* Update mode first so LoadConf sections follow the new mode. */
        log_info("%s: mode %d -> %d", __func__, (int)adev->mode, (int)mode);
        adev->mode = mode;

        /*
         * LoadConf enters _SD_Filter_Control. Skip while AI/AO Echo threads
         * run (FillAdcStream / FillDacLoopback). CALL LoadConf is applied
         * in input_open / output_open before start_capture / ak_ao_open.
         */
        if (adev->in != NULL)
            hal_audio_effect_in_loadconf(adev->hal_audio);

        if (adev->out != NULL)
            hal_audio_effect_out_loadconf(adev->hal_audio);
    }

    pthread_mutex_unlock(&adev->lock);

    return HARDWARE_AUDIO_OK;
}

/**
 * @brief Set microphone mute state.
 * @param dev Audio hardware device pointer.
 * @param state Mute state (true=muted, false=unmuted).
 * @return HARDWARE_AUDIO_OK on success, error code on failure.
 */
static int adev_set_mic_mute(struct audio_hw_device *dev, bool state)
{
    struct audio_device *adev = (struct audio_device *)dev;
    log_debug("adev_set_mic_mute: %d",state);

    pthread_mutex_lock(&adev->lock);

    hal_audio_set_mic_mute(adev->hal_audio, state);

    pthread_mutex_unlock(&adev->lock);

    return HARDWARE_AUDIO_OK;
}

/**
 * @brief Get microphone mute state.
 * @param dev Audio hardware device pointer.
 * @param state Output parameter for mute state.
 * @return HARDWARE_AUDIO_OK on success, error code on failure.
 */
static int adev_get_mic_mute(const struct audio_hw_device *dev, bool *state)
{
    struct audio_device *adev = (struct audio_device *)dev;

    *state = hal_audio_get_mic_mute(adev->hal_audio);

    log_debug("adev_get_mic_mute:device state:%d",*state);
    return HARDWARE_AUDIO_OK;
}

/**
 * @brief Get input buffer size for audio device.
 * @param dev Audio hardware device pointer.
 * @return Buffer size in bytes (obtained from HAL).
 */
static size_t adev_get_input_buffer_size(const struct audio_hw_device *dev)
{
    struct audio_device *adev = (struct audio_device *)dev;

/*
    size_t buffer_size = samples_per_milliseconds(
                             AUDIO_INPUT_BUFFER_MILLISECONDS,
                             config->sample_rate,
                             audio_channel_count_from_in_mask(
                                 config->channel_mask));

    if (!audio_has_proportional_frames(config->format)) {
        // Since the audio data is not proportional choose an arbitrary size for
        // the buffer.
        buffer_size *= 4;
    } else {
        buffer_size *= audio_bytes_per_sample(config->format);
    }
*/
    size_t buffer_size = hal_audio_input_get_frame_len(adev->hal_audio);


    log_debug("adev_get_input_buffer_size: %zu", buffer_size);
    return buffer_size;
}

/**
 * @brief Open audio input stream.
 * @param dev Audio hardware device pointer.
 * @param handle I/O handle for the stream.
 * @param devices Audio device selection.
 * @param config Audio configuration (sample rate, format, channels).
 * @param stream_in Output parameter for opened stream.
 * @return HARDWARE_AUDIO_OK on success, error code on failure.
 * @note Creates and configures input stream, opens HAL input and codec (if needed),
 *       starts capture processing thread, loads effect configuration.
 */
static int adev_open_input_stream(struct audio_hw_device *dev,
                                  audio_io_handle_t handle,
                                  audio_devices_t devices,
                                  struct audio_config *config,
                                  struct audio_stream_in **stream_in)
{
    log_debug("adev_open_input_stream");

    *stream_in = NULL;
    struct stream_in *in = (struct stream_in *)calloc(1, sizeof(struct stream_in));

    if (!in)
        return HARDWARE_AUDIO_ERROR_MEMORY;
    int ret;

    struct audio_device *adev = (struct audio_device *)dev;

    log_debug("%s: enter: sample_rate %u, channel_mask %#x, format %#x",
            __func__,
            config->sample_rate, config->channel_mask, config->format);

    in->dev = adev;
    in->handle = handle;

    in->format = config->format;
    in->channel_mask = config->channel_mask;
    in->sample_rate = config->sample_rate;

    if (devices == AUDIO_DEVICE_NONE)
        in->devices = AUDIO_DEVICE_IN_AMIC;
    else
        in->devices = devices;

    pthread_mutex_init(&in->lock, (const pthread_mutexattr_t *) NULL);

    in->stream.common.get_sample_rate = in_get_sample_rate;
    in->stream.common.set_sample_rate = in_set_sample_rate;
    in->stream.common.get_buffer_size = in_get_buffer_size;
    in->stream.common.get_channels = in_get_channels;
    in->stream.common.get_format = in_get_format;
    in->stream.common.set_format = in_set_format;
    in->stream.common.dump = in_dump;
    in->stream.common.set_parameters = in_set_parameters;
    in->stream.common.get_parameters = in_get_parameters;
    in->stream.common.add_audio_effect = in_add_audio_effect;
    in->stream.common.remove_audio_effect = in_remove_audio_effect;
    in->stream.set_volume = in_set_volume;
    in->stream.read = in_read;
    in->sample_rate = config->sample_rate;
    if (in->sample_rate == 0)
        in->sample_rate = AUDIO_DEFAULT_INPUT_SAMPLE_RATE;
    in->channel_mask = config->channel_mask;
    if (in->channel_mask == AUDIO_CHANNEL_NONE) {
        in->channel_mask = AUDIO_CHANNEL_IN_MONO;
    } else if ((in->channel_mask & AUDIO_CHANNEL_IN_MAIN_MASK) == 0) {
        /* e.g. only AUDIO_CHANNEL_MASK_n index bits — HAL input open may still work;
         * AENC path requires IN_MONO / IN_STEREO in the main field. */
        log_warn("adev_open_input_stream: channel_mask 0x%x has no input main bits; use MONO",
                 in->channel_mask);
        in->channel_mask = AUDIO_CHANNEL_IN_MONO;
    }
    in->format = config->format;
    if (in->format == AUDIO_FORMAT_DEFAULT)
        in->format = AUDIO_DEFAULT_AUDIO_FORMAT;
    in->frame_count = samples_per_milliseconds(
                          AUDIO_INPUT_BUFFER_MILLISECONDS, in->sample_rate, 1);

    log_debug("adev_open_input_stream: sample_rate: %u, channels: %x, format: %d,"
          "frames: %zu", in->sample_rate, in->channel_mask, in->format,
          in->frame_count);
    *stream_in = &in->stream;

    in->last_read_time_us = 0;

    /* Callback/effect paths may read pcm_config; fill before HAL open / codec. */
    memcpy(&in->input_config, config, sizeof(struct audio_config));
    memcpy(&in->pcm_config, config, sizeof(struct audio_config));
    in->pcm_config.format = AUDIO_FORMAT_PCM;
    /* PCM tap follows normalized capture routing, not a half-filled config copy */
    in->pcm_config.channel_mask = in->channel_mask;
    in->pcm_config.sample_rate = in->sample_rate;

    pthread_mutex_lock(&in->lock);
    adev->in = in;
    ret = hal_audio_input_open(adev->hal_audio);
    if (ret != 0) {
        log_error("adev_open_input_stream: hal_audio_input_open failed, ret=%d", ret);
        adev->in = NULL;
        pthread_mutex_unlock(&in->lock);
        free(in);
        *stream_in = NULL;
        return HARDWARE_AUDIO_ERROR_OPERATION_FAILED;
    }

    if ((in->format & AUDIO_FORMAT_MAIN_MASK) != AUDIO_FORMAT_PCM) {
        /* Returns 1 (true) on success; AK_FAILED / false are non-1 — do not use !ret (AK_FAILED is truthy). */
        if (hal_audio_codec_in_open(adev->hal_audio, in) != true) {
            log_error("adev_open_input_stream: hal_audio_codec_in_open failed");
            hal_audio_input_close(adev->hal_audio);
            adev->in = NULL;
            pthread_mutex_unlock(&in->lock);
            pthread_mutex_destroy(&in->lock);
            free(in);
            *stream_in = NULL;
            return HARDWARE_AUDIO_ERROR_OPERATION_FAILED;
        }
    }

//if not set mode,set to normal and load config,if set different mode, will set it again in set_mode
    if(adev->mode == AUDIO_MODE_NONE)
        adev->mode = AUDIO_MODE_NORMAL;

    in->status = AUDIO_STATUS_ENABLED;
    /* Defer pthread_create: framework calls audio_hw_stream_in_ensure_capture_started()
     * after add_audio_effect (algorithm init) so capture GetFrame does not race AI config. */

    pthread_mutex_unlock(&in->lock);

    return HARDWARE_AUDIO_OK;
}

void audio_hw_stream_in_ensure_capture_started(struct audio_stream_in *stream_in)
{
    struct stream_in *in = (struct stream_in *)stream_in;

    if (!in) {
        return;
    }
    pthread_mutex_lock(&in->lock);
    log_info("ensure_capture_started: thread_id=%lu status=%d",
             (unsigned long)in->thread_id, (int)in->status);
    if (in->thread_id == 0 && in->status == AUDIO_STATUS_ENABLED) {
        if (pthread_create(&in->thread_id, NULL, audio_process_capture_thread_function, in) != 0) {
            in->thread_id = 0;
            log_error("audio_hw_stream_in_ensure_capture_started: pthread_create failed");
        } else {
            log_info("capture thread started (post effect init)");
        }
    }
    pthread_mutex_unlock(&in->lock);
}

static void audio_hw_stream_in_stop_capture(struct audio_stream_in *stream_in)
{
    struct stream_in *in = (struct stream_in *)stream_in;

    if (!in) {
        return;
    }

    pthread_mutex_lock(&in->lock);
    in->status = AUDIO_STATUS_DISABLED;
    pthread_mutex_unlock(&in->lock);

    /* Stop only the PCM capture thread here. Keep recording_flag set until
     * hal_audio_effect_in_remove() phase-1 can join VAD/CRY/SSL workers and
     * disable effect AI sub-channels before hal_audio_input_close(). */
    if (in->thread_id != 0) {
        pthread_join(in->thread_id, NULL);
        in->thread_id = 0;
        log_info("capture thread stopped (pre effect remove)");
    }
}

void audio_hw_stream_in_stop_sdk_capture(struct audio_stream_in *stream_in)
{
    struct stream_in *in = (struct stream_in *)stream_in;

    audio_hw_stream_in_stop_capture(stream_in);
    if (in != NULL && in->dev != NULL && in->dev->hal_audio != NULL)
        (void)hal_audio_input_stop_capture(in->dev->hal_audio);
}

/**
 * @brief Close audio input stream.
 * @param dev Audio hardware device pointer.
 * @param stream Input stream to close.
 * @note Stops capture thread, closes HAL input and codec (if used).
 */
static void adev_close_input_stream(struct audio_hw_device *dev,
                                   struct audio_stream_in *stream)
{
    struct audio_device *adev = (struct audio_device *)dev;
    struct stream_in *in = (struct stream_in *)stream;

    log_debug("adev_close_input_stream...");

    audio_hw_stream_in_stop_capture(stream);
    /* Stop effect worker threads by clearing effect status in existing remove flow,
     * then run effect deinit in the same path. */
    (void)hal_audio_effect_in_remove(adev->hal_audio, (audio_effect_type_t)(
        AUDIO_EFFECT_VAD | AUDIO_EFFECT_NPU_CRY | AUDIO_EFFECT_CPU_CRY |
        AUDIO_EFFECT_LSSL | AUDIO_EFFECT_RESAMPLE));

    pthread_mutex_lock(&in->lock);

    hal_audio_input_close(adev->hal_audio);

    if ((in->format & AUDIO_FORMAT_MAIN_MASK) != AUDIO_FORMAT_PCM)
    {
        hal_audio_codec_in_close(adev->hal_audio);
    }
    pthread_mutex_unlock(&in->lock);

    /* Run phase-2 deinit on existing interface after AI close. */
    (void)hal_audio_effect_in_remove(adev->hal_audio, (audio_effect_type_t)(
        AUDIO_EFFECT_VAD | AUDIO_EFFECT_NPU_CRY | AUDIO_EFFECT_CPU_CRY |
        AUDIO_EFFECT_LSSL | AUDIO_EFFECT_RESAMPLE));

    pthread_mutex_destroy(&in->lock);
    adev->in = NULL;
    free(in);

    return;
}


/**
 * @brief Close audio device.
 * @param device Hardware device pointer.
 * @return HARDWARE_AUDIO_OK on success, error code on failure.
 * @note Deinitializes HAL audio context and frees device resources.
 */
static int adev_close(hw_device_t *device)
{
    log_debug("adev_close");
    struct audio_device *adev = (struct audio_device *)device;

    hal_audio_deinit(adev->hal_audio);

    free(device);

    return HARDWARE_AUDIO_OK;
}

/**
 * @brief Open audio hardware device.
 * @param module Hardware module pointer.
 * @param name Device interface name (must be AUDIO_HARDWARE_INTERFACE).
 * @param device Output parameter for opened device.
 * @return HARDWARE_AUDIO_OK on success, error code on failure.
 * @note Initializes audio device structure, HAL audio context, and sets up device methods.
 */
static int adev_open(const hw_module_t* module, const char* name,
                     hw_device_t** device)
{
    log_debug("adev_open: %s", name);

    struct audio_device *adev;

    if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0)
        return HARDWARE_AUDIO_ERROR_INVALID_PARAM;

    adev = calloc(1, sizeof(struct audio_device));
    if (!adev)
        return HARDWARE_AUDIO_ERROR_MEMORY;

    pthread_mutex_init(&adev->lock, (const pthread_mutexattr_t *) NULL);

    pthread_mutex_lock(&adev->lock);

    adev->hal_audio = hal_audio_init(adev);

    if(!adev->hal_audio) {
        pthread_mutex_unlock(&adev->lock);
        free(adev);
        return HARDWARE_AUDIO_ERROR_MEMORY;
    }
    pthread_mutex_unlock(&adev->lock);


    adev->device.common.tag = HARDWARE_DEVICE_TAG;
    adev->device.common.version = AUDIO_DEVICE_API_VERSION_0_1;
    adev->device.common.module = (struct hw_module_t *) module;
    adev->device.common.close = adev_close;


    adev->device.set_mode = adev_set_mode;
    adev->device.set_mic_mute = adev_set_mic_mute;
    adev->device.get_mic_mute = adev_get_mic_mute;

    adev->device.set_parameters = adev_set_parameters;
    adev->device.get_parameters = adev_get_parameters;
    adev->device.get_input_buffer_size = adev_get_input_buffer_size;

    adev->device.open_output_stream = adev_open_output_stream;
    adev->device.close_output_stream = adev_close_output_stream;

    adev->device.open_input_stream = adev_open_input_stream;
    adev->device.close_input_stream = adev_close_input_stream;

    adev->device.set_callback_ops = adev_set_callback_ops;


    *device = &adev->device.common;
    adev->mode = AUDIO_MODE_NONE;

    adev->in = NULL;
    adev->out = NULL;

    return HARDWARE_AUDIO_OK;
}

static struct hw_module_methods_t hal_module_methods = {
    .open = adev_open,
};

struct audio_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = AUDIO_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = AUDIO_HARDWARE_MODULE_ID,
        .name = "Celestia audio HW HAL",
        .author = "AnyKa Celestia project",
        .methods = &hal_module_methods,
    },
};

/**
 * @brief Initialize audio hardware module.
 * @return HARDWARE_AUDIO_OK on success, error code on failure.
 * @note Registers the audio HAL module with the hardware module system.
 */
int hw_audio_module_init(void)
{
    return hw_register_module(&(HAL_MODULE_INFO_SYM.common));
}
