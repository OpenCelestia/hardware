// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/audio.
 *
 * Audio Hardware Abstraction Layer (HAL) public interface.
 * - Provides Android-style HAL interface for audio devices.
 * - Supports audio input/output stream management and audio effect processing.
 */
#ifndef HARDWARE_AUDIO_H
#define HARDWARE_AUDIO_H

/* Standard C library includes */
#include <stdint.h>
#include <strings.h>
#include <sys/cdefs.h>
#include <sys/types.h>
#include <time.h>
#include <stdbool.h>

/* Project includes */
#include "hardware.h"
#include "system_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * MODULE IDENTIFICATION
 * ============================================================================ */

/** Module identifier for audio HAL */
#define AUDIO_HARDWARE_MODULE_ID "audio_hal"

/** Interface name for audio hardware devices */
#define AUDIO_HARDWARE_INTERFACE "audio_hw_if"

/* ============================================================================
 * API VERSION DEFINITIONS
 * ============================================================================ */

/**
 * @brief Audio module API version 0.1
 * @details Compatible with first generation audio HW module with version_major hardcoded to 1
 */
#define AUDIO_MODULE_API_VERSION_0_1 HARDWARE_MODULE_API_VERSION(0, 1)
#define AUDIO_MODULE_API_VERSION_CURRENT AUDIO_MODULE_API_VERSION_0_1

/**
 * @brief Audio device API version 0.1
 * @details First generation audio devices had version hardcoded to 0
 */
#define AUDIO_DEVICE_API_VERSION_0_1 HARDWARE_DEVICE_API_VERSION(0, 1)
#define AUDIO_DEVICE_API_VERSION_CURRENT AUDIO_DEVICE_API_VERSION_0_1
#define AUDIO_DEVICE_API_VERSION_MIN AUDIO_DEVICE_API_VERSION_0_1

/* ============================================================================
 * AUDIO DEVICE TYPES
 * ============================================================================ */

/**
 * @brief Audio device enumeration
 * @details Defines supported input and output devices
 */
typedef enum {
    AUDIO_DEVICE_NONE           = 0x0u,
    AUDIO_DEVICE_OUT_SPEAKER    = 0x1u,
    AUDIO_DEVICE_OUT_LINE       = 0x02u,
    AUDIO_DEVICE_OUT_USB_DEVICE = 0x04u,
    AUDIO_DEVICE_OUT_BUS        = 0x05,

    AUDIO_DEVICE_IN_AMIC        = 0x100u,
    AUDIO_DEVICE_IN_DMIC        = 0x101u,
    AUDIO_DEVICE_IN_LINE        = 0x102u,
    AUDIO_DEVICE_IN_BUS0        = 0x103, /**< IIS0 */
    AUDIO_DEVICE_IN_BUS1        = 0x104, /**< IIS1 */

    AUDIO_DEVICE_IN_USB_DEVICE  = 0x105u,
    AUDIO_DEVICE_IN_LOOPBACK    = 0x40000u
} audio_devices_t;

/* ============================================================================
 * AUDIO MODE DEFINITIONS
 * ============================================================================ */

/**
 * @brief Audio mode enumeration
 * @details Defines different audio operation modes
 */
typedef enum {
    AUDIO_MODE_NONE = 0x0,
    AUDIO_MODE_NORMAL,
    AUDIO_MODE_MUSIC,
    AUDIO_MODE_CALL,
    AUDIO_MODE_MAX,
} audio_mode_t;

/* ============================================================================
 * AUDIO EFFECT TYPES
 * ============================================================================ */

/**
 * @brief Audio effect type enumeration
 * @details Defines supported audio processing effects with bit masks for categorization
 */
typedef enum {
    AUDIO_EFFECT_NONE           = 0,
    AUDIO_EFFECT_SOUND_MASK     = 0xff,          /**< Bit 0-7: Sound processing effects */
    AUDIO_EFFECT_DETECT_MASK    = (0xff << 8),   /**< Bit 8-15: Detection effects */
    AUDIO_EFFECT_SSL_MASK       = (0xf << 16),   /**< Bit 16-19: SSL effects */

    /* Sound processing effects */
    AUDIO_EFFECT_ASLC           = 1 << 1,    /**< Automatic Sound Level Control */
    AUDIO_EFFECT_EQ             = 1 << 2,    /**< Equalizer */
    AUDIO_EFFECT_AEC            = 1 << 3,    /**< Acoustic Echo Cancellation */
    AUDIO_EFFECT_NS             = 1 << 4,    /**< Noise Suppression */
    AUDIO_EFFECT_RESAMPLE       = 1 << 5,    /**< Resampling */

    /* Detection effects */
    AUDIO_EFFECT_VAD            = 1 << 8,    /**< Voice Activity Detection */
    AUDIO_EFFECT_CPU_CRY        = 1 << 9,    /**< CPU-based cry detection */
    AUDIO_EFFECT_NPU_CRY        = 1 << 10,   /**< NPU-based cry detection */

    /* Sound Source Localization effects */
    AUDIO_EFFECT_SSL            = 1 << 16,   /**< Sound Source Localization */
    AUDIO_EFFECT_LSSL           = 1 << 17,   /**< Linear Sound Source Localization */
    AUDIO_EFFECT_CSSL           = 1 << 18,   /**< Circular Sound Source Localization */
} audio_effect_type_t;

/* ============================================================================
 * AUDIO EFFECT RESULT STRUCTURES
 * ============================================================================ */

/**
 * @brief Audio effect result data structure
 */
typedef struct audio_effect_result_data {
    uint8_t *buffer;    /**< Effect result data buffer */
    int bytes;          /**< Size of data in bytes */
} audio_effect_result_data_t;

/**
 * @brief Audio effect detection result structure
 */
typedef struct audio_effect_result_detect {
    int32_t value;      /**< Detection result value */
} audio_effect_result_detect_t;

/**
 * @brief Audio effect sound source localization result structure
 * @details All angles use 32Q15 fixed-point format (x.xx * pi/2)
 */
typedef struct audio_effect_result_ssl {
    int32_t azimuth;    /**< Azimuth angle in radians (32Q15 format) */
    int32_t elevation;  /**< Elevation angle in radians (32Q15 format) */
    int32_t radius;     /**< Distance in meters (32Q15 format) */
} audio_effect_result_ssl_t;

/* ============================================================================
 * AUDIO NOTIFICATION MESSAGE
 * ============================================================================ */

/**
 * @brief Audio notification message structure
 * @details Sent to audio_callback_ops_t.notify()
 */
typedef struct audio_notify_msg {
    audio_effect_type_t type;  /**< Message type - one of audio_effect_type_t values or private extension */

    union {
        audio_effect_result_detect_t detect;  /**< For NPU/CPU cry detection (1 = detected) */
        audio_effect_result_ssl_t ssl;        /**< Sound source localization results */
        uint8_t generic[32];                 /**< Generic message contents - ensures minimum size for custom message types */
    };
} audio_notify_msg_t;

/* ============================================================================
 * AUDIO EFFECT PARAMETERS
 * ============================================================================ */

/**
 * @brief Audio effect detection parameters
 */
typedef struct {
    float sensitivity;  /**< Detection sensitivity: 0.0 to 1.0 (0% to 100%) */
} audio_effect_detect_parameter;

/**
 * @brief Audio effect data parameters
 */
typedef struct {
    float sample_rate;  /**< Sample rate in Hz */
} audio_effect_data_parameter;

/* ============================================================================
 * AUDIO CHANNEL CONFIGURATION
 * ============================================================================ */

/**
 * @brief Audio channel mask enumeration
 * @details Defines input/output channel configurations
 */
typedef enum {
    AUDIO_CHANNEL_NONE = 0x00,
    
    /* Channel mask bit positions */
    AUDIO_CHANNEL_IN_MAIN_MASK      = 0x00FF0000u,
    AUDIO_CHANNEL_OUT_MAIN_MASK     = 0xFF000000u,
    AUDIO_CHANNEL_IN_MAIN_BIT       = 16,  /**< Right shift by 16 */
    AUDIO_CHANNEL_OUT_MAIN_BIT      = 24,  /**< Right shift by 24 */
    AUDIO_CHANNEL_SUB_MASK          = 0x0000FFFFu,

    /* Input channel configurations */
    AUDIO_CHANNEL_IN_MONO           = 0x010000,
    AUDIO_CHANNEL_IN_STEREO         = 0x020000,
    AUDIO_CHANNEL_IN_TRI            = 0x030000,
    AUDIO_CHANNEL_IN_QUAD           = 0x040000,

    /* Output channel configurations */
    AUDIO_CHANNEL_OUT_MONO          = 0x01000000,
    AUDIO_CHANNEL_OUT_STEREO        = 0x02000000,

    /* Individual channel masks for multi-channel configurations */
    AUDIO_CHANNEL_MASK_1            = (1<<1),
    AUDIO_CHANNEL_MASK_2            = (1<<2),
    AUDIO_CHANNEL_MASK_3            = (1<<3),
    AUDIO_CHANNEL_MASK_4            = (1<<4),

    AUDIO_CHANNEL_COUNT_MAX = 4     /**< Maximum number of channels */
} audio_channel_mask_t;

/* ============================================================================
 * AUDIO FORMAT DEFINITIONS
 * ============================================================================ */

/**
 * @brief Audio format enumeration
 * @details Defines supported audio codec formats
 */
typedef enum {
    /* Format mask definitions */
    AUDIO_FORMAT_MAIN_MASK             = 0xFF000000u,
    AUDIO_FORMAT_SUB_MASK              = 0x00FFFFFFu,

    /* Hardware codec formats */
    AUDIO_FORMAT_DEFAULT               = 0u,
    AUDIO_FORMAT_PCM                   = 0u,
    AUDIO_FORMAT_MP3                   = 0x01000000u,
    AUDIO_FORMAT_AMR                   = 0x02000000u, /**< Default: Wideband */
    AUDIO_FORMAT_AMR_WB                = 0x02000000u, /**< NB: 8K, WB: 16K */
    AUDIO_FORMAT_AAC                   = 0x04000000u,
    AUDIO_FORMAT_WMA                   = 0x15000000u,
    AUDIO_FORMAT_ALAW                  = 0x34000000u, /**< G.711 A-law */
    AUDIO_FORMAT_ULAW                  = 0x35000000u, /**< G.711 μ-law */

    /* Software codec formats */
    AUDIO_FORMAT_AMR_NB                = 0x03000000u, /**< NB: 8K, WB: 16K */
    AUDIO_FORMAT_OPUS                  = 0x08000000u,
    AUDIO_FORMAT_HE_AAC_V1             = 0x05000000u,
    AUDIO_FORMAT_HE_AAC_V2             = 0x06000000u,
    AUDIO_FORMAT_VORBIS                = 0x07000000u,
    AUDIO_FORMAT_AC3                   = 0x09000000u,
    AUDIO_FORMAT_E_AC3                 = 0x0A000000u,
    AUDIO_FORMAT_DTS                   = 0x0B000000u,
    AUDIO_FORMAT_DTS_HD                = 0x0C000000u,
    AUDIO_FORMAT_IEC61937              = 0x0D000000u,
    AUDIO_FORMAT_DOLBY_TRUEHD          = 0x0E000000u,
    AUDIO_FORMAT_EVRC                  = 0x10000000u,
    AUDIO_FORMAT_EVRCB                 = 0x11000000u,
    AUDIO_FORMAT_EVRCWB                = 0x12000000u,
    AUDIO_FORMAT_EVRCNW                = 0x13000000u,
    AUDIO_FORMAT_AAC_ADIF              = 0x14000000u,
    AUDIO_FORMAT_WMA_PRO               = 0x16000000u,
    AUDIO_FORMAT_AMR_WB_PLUS           = 0x17000000u,
    AUDIO_FORMAT_MP2                   = 0x18000000u,
    AUDIO_FORMAT_QCELP                 = 0x19000000u,
    AUDIO_FORMAT_DSD                   = 0x1A000000u,
    AUDIO_FORMAT_FLAC                  = 0x1B000000u,
    AUDIO_FORMAT_ALAC                  = 0x1C000000u,
    AUDIO_FORMAT_APE                   = 0x1D000000u,
    AUDIO_FORMAT_AAC_ADTS              = 0x1E000000u,
    AUDIO_FORMAT_SBC                   = 0x1F000000u,
    AUDIO_FORMAT_APTX                  = 0x20000000u,
    AUDIO_FORMAT_APTX_HD               = 0x21000000u,
    AUDIO_FORMAT_AC4                   = 0x22000000u,
    AUDIO_FORMAT_LDAC                  = 0x23000000u,
    AUDIO_FORMAT_MAT                   = 0x24000000u,
    AUDIO_FORMAT_AAC_LATM              = 0x25000000u,
    AUDIO_FORMAT_CELT                  = 0x26000000u,
    AUDIO_FORMAT_APTX_ADAPTIVE         = 0x27000000u,
    AUDIO_FORMAT_LHDC                  = 0x28000000u,
    AUDIO_FORMAT_LHDC_LL               = 0x29000000u,
    AUDIO_FORMAT_APTX_TWSP             = 0x2A000000u,
    AUDIO_FORMAT_LC3                   = 0x2B000000u,
    AUDIO_FORMAT_MPEGH                 = 0x2C000000u,
    AUDIO_FORMAT_IEC60958              = 0x2D000000u,
    AUDIO_FORMAT_DTS_UHD               = 0x2E000000u,
    AUDIO_FORMAT_DRA                   = 0x2F000000u,
    AUDIO_FORMAT_APTX_ADAPTIVE_QLEA    = 0x30000000u,
    AUDIO_FORMAT_APTX_ADAPTIVE_R4      = 0x31000000u,
    AUDIO_FORMAT_DTS_HD_MA             = 0x32000000u,
    AUDIO_FORMAT_DTS_UHD_P2            = 0x33000000u,

    /* PCM sub-formats (Anyka platform supports 16-bit only) */
    AUDIO_FORMAT_PCM_SUB_16_BIT        = 0x1u,
    AUDIO_FORMAT_PCM_SUB_8_BIT         = 0x2u,
    AUDIO_FORMAT_PCM_SUB_32_BIT        = 0x3u,

    /* Combined PCM formats */
    AUDIO_FORMAT_PCM_16_BIT            = AUDIO_FORMAT_PCM | AUDIO_FORMAT_PCM_SUB_16_BIT,
    AUDIO_FORMAT_PCM_8_BIT             = AUDIO_FORMAT_PCM | AUDIO_FORMAT_PCM_SUB_8_BIT,
} audio_format_t;

/* ============================================================================
 * AUDIO CONFIGURATION STRUCTURES
 * ============================================================================ */

/**
 * @brief Audio configuration structure
 * @details Must be 8-byte aligned for Binder transmission
 */
struct __attribute__((aligned(8))) audio_config {
    uint32_t sample_rate;              /**< Sample rate in Hz */
    audio_channel_mask_t channel_mask; /**< Channel configuration */
    audio_format_t format;             /**< Audio format */
    uint32_t frame_count;              /**< Number of frames */
};

typedef struct audio_config_base audio_config_base_t;

/* ============================================================================
 * AUDIO DUAL MONO MODE
 * ============================================================================ */

/**
 * @brief Audio dual mono mode enumeration
 */
typedef enum {
    AUDIO_DUAL_MONO_MODE_OFF = 0,  /**< Dual mono disabled */
    AUDIO_DUAL_MONO_MODE_LR = 1,   /**< Left/Right mode */
    AUDIO_DUAL_MONO_MODE_LL = 2,   /**< Left/Left mode */
    AUDIO_DUAL_MONO_MODE_RR = 3,   /**< Right/Right mode */
} audio_dual_mono_mode_t;

/* ============================================================================
 * AUDIO TIMESTRETCH CONFIGURATION
 * ============================================================================ */

/**
 * @brief Timestretch fallback mode constants
 */
enum {
    HAL_AUDIO_TIMESTRETCH_FALLBACK_MUTE = 1,
    HAL_AUDIO_TIMESTRETCH_FALLBACK_FAIL = 2,
};

/**
 * @brief Audio timestretch fallback mode enumeration
 */
typedef enum {
#ifndef AUDIO_NO_SYSTEM_DECLARATIONS
    AUDIO_TIMESTRETCH_FALLBACK_CUT_REPEAT = -1, /**< Framework only: truncate frames for speed <1.0, repeat for speed >1.0 */
    AUDIO_TIMESTRETCH_FALLBACK_DEFAULT    = 0,  /**< Framework only: system determines behavior */
#endif
    AUDIO_TIMESTRETCH_FALLBACK_MUTE       = HAL_AUDIO_TIMESTRETCH_FALLBACK_MUTE,  /**< Set all processed frames to zero */
    AUDIO_TIMESTRETCH_FALLBACK_FAIL       = HAL_AUDIO_TIMESTRETCH_FALLBACK_FAIL,  /**< Stop processing and indicate error */
} audio_timestretch_fallback_mode_t;

/**
 * @brief Audio timestretch stretch mode enumeration
 */
typedef enum {
    AUDIO_TIMESTRETCH_STRETCH_DEFAULT = 0,  /**< Default stretch mode */
    AUDIO_TIMESTRETCH_STRETCH_VOICE = 1,    /**< Voice-optimized stretch mode */
} audio_timestretch_stretch_mode_t;

/**
 * @brief Audio playback rate structure
 */
struct audio_playback_rate {
    float mSpeed;                                    /**< Playback speed multiplier */
    float mPitch;                                    /**< Pitch adjustment */
    audio_timestretch_stretch_mode_t mStretchMode;   /**< Stretch mode */
    audio_timestretch_fallback_mode_t mFallbackMode; /**< Fallback behavior */
};

typedef struct audio_playback_rate audio_playback_rate_t;

/* ============================================================================
 * TIMESTRETCH CONSTANTS
 * ============================================================================ */

#define AUDIO_TIMESTRETCH_SPEED_MIN        0.01f
#define AUDIO_TIMESTRETCH_SPEED_MAX        20.0f
#define AUDIO_TIMESTRETCH_SPEED_NORMAL     1.0f
#define AUDIO_TIMESTRETCH_SPEED_MIN_DELTA  0.0001f
#define AUDIO_TIMESTRETCH_PITCH_MIN        0.25f
#define AUDIO_TIMESTRETCH_PITCH_MAX        4.0f
#define AUDIO_TIMESTRETCH_PITCH_NORMAL     1.0f
#define AUDIO_TIMESTRETCH_PITCH_MIN_DELTA  0.0001f

/**
 * @brief Initial audio playback rate configuration
 */
static const audio_playback_rate_t AUDIO_PLAYBACK_RATE_INITIALIZER = {
    .mSpeed = AUDIO_TIMESTRETCH_SPEED_NORMAL,
    .mPitch = AUDIO_TIMESTRETCH_PITCH_NORMAL,
    .mStretchMode = AUDIO_TIMESTRETCH_STRETCH_DEFAULT,
    .mFallbackMode = AUDIO_TIMESTRETCH_FALLBACK_FAIL
};

/* ============================================================================
 * AUDIO I/O HANDLE TYPE
 * ============================================================================ */

/** Audio I/O handle type used by AudioFlinger and AudioPolicy services */
typedef int audio_io_handle_t;

/* ============================================================================
 * AUDIO FORMAT UTILITY FUNCTIONS
 * ============================================================================ */

/**
 * @brief Extract the primary format (e.g., PCM, AC3, etc.)
 * @param format Audio format to extract main format from
 * @return Primary audio format (main format bits)
 * @details Extracts the main format bits from a combined audio format value
 */
static inline audio_format_t audio_get_main_format(audio_format_t format)
{
    return (audio_format_t)(format & AUDIO_FORMAT_MAIN_MASK);
}

/**
 * @brief Check if the format is linear PCM that can be scaled and mixed
 * @param format Audio format to check
 * @return true if format is linear PCM, false otherwise
 * @details Determines if the audio format is linear PCM that can be scaled and mixed
 */
static inline bool audio_is_linear_pcm(audio_format_t format)
{
    return (audio_get_main_format(format) == AUDIO_FORMAT_PCM);
}

/**
 * @brief Check if the format has proportional frames
 * @param format Audio format to check
 * @return true if format has proportional frames, false otherwise
 * @details Returns true for PCM and IEC61937 formats that have proportional frame sizes
 */
static inline bool audio_has_proportional_frames(audio_format_t format)
{
    audio_format_t mainFormat = audio_get_main_format(format);
    return (mainFormat == AUDIO_FORMAT_PCM || mainFormat == AUDIO_FORMAT_IEC61937);
}

/* ============================================================================
 * AUDIO STREAM INTERFACE
 * ============================================================================ */

/**
 * @brief Common audio stream interface
 * @details Provides basic audio stream operations
 */
struct audio_stream {
    /**
     * @brief Get the sampling rate in Hz (e.g., 44100)
     * @param stream Pointer to the audio stream
     * @return Current sample rate in Hz
     * @details Returns the current sample rate configured for this stream
     */
    uint32_t (*get_sample_rate)(const struct audio_stream *stream);

    /**
     * @brief Set the sampling rate
     * @param stream Pointer to the audio stream
     * @param rate New sample rate in Hz
     * @return 0 on success, negative error code on failure
     * @details Currently unused - use set_parameters with AUDIO_PARAMETER_STREAM_SAMPLING_RATE key
     */
    int (*set_sample_rate)(struct audio_stream *stream, uint32_t rate);

    /**
     * @brief Get the buffer size in bytes for this stream (e.g., 4800)
     * @param stream Pointer to the audio stream
     * @return Buffer size in bytes
     * @details Must be a multiple of the frame size. Calculated as frame_count * frame_size
     */
    size_t (*get_buffer_size)(const struct audio_stream *stream);

    /**
     * @brief Get the channel mask (e.g., AUDIO_CHANNEL_OUT_STEREO)
     * @param stream Pointer to the audio stream
     * @return Channel mask configuration
     * @details Returns the current channel configuration for this stream
     */
    audio_channel_mask_t (*get_channels)(const struct audio_stream *stream);

    /**
     * @brief Get the audio format (e.g., AUDIO_FORMAT_PCM_16_BIT)
     * @param stream Pointer to the audio stream
     * @return Audio format configuration
     * @details Returns the current audio format for this stream
     */
    audio_format_t (*get_format)(const struct audio_stream *stream);

    /**
     * @brief Set the audio format
     * @param stream Pointer to the audio stream
     * @param format New audio format
     * @return 0 on success, negative error code on failure
     * @details Currently unused - use set_parameters with AUDIO_PARAMETER_STREAM_FORMAT key
     */
    int (*set_format)(struct audio_stream *stream, audio_format_t format);

    /**
     * @brief Dump the state of the audio input/output device
     * @param stream Pointer to the audio stream
     * @param file_path Path for dump file
     * @param file_name Name of dump file
     * @param enable Enable/disable dump functionality
     * @return 0 on success, negative error code on failure
     * @details Dumps internal state and configuration to specified file for debugging
     */
    int (*dump)(const struct audio_stream *stream, char *file_path, char *file_name, int enable);

    /**
     * @brief Get the set of devices this stream is connected to
     * @param stream Pointer to the audio stream
     * @return Device mask indicating connected devices
     * @details Returns bitmask of audio_devices_t values for connected devices
     */
    audio_devices_t (*get_device)(const struct audio_stream *stream);

    /**
     * @brief Set the device
     * @param stream Pointer to the audio stream
     * @param device Device to connect to
     * @return 0 on success, negative error code on failure
     * @details Currently unused - use set_parameters with AUDIO_PARAMETER_STREAM_ROUTING key
     */
    int (*set_device)(struct audio_stream *stream, audio_devices_t device);

    /**
     * @brief Set audio stream parameters
     * @param stream Pointer to the audio stream
     * @param name Parameter name (e.g., "Volume", "AIParam", "Vad", "NpuCry", "CpuCry", "Lssl", "AIReboot")
     * @param value Parameter value pointer
     * @param len Length of parameter value
     * @return 0 on success, negative error code on failure
     * @details Accepts parameter key-value pairs. Returns -ENOSYS if parameter change not accepted while stream is active.
     * Supported parameters include volume control, audio input parameters, VAD sensitivity, cry detection sensitivity, SSL parameters, and AI reboot control.
     */
    int (*set_parameters)(const struct audio_stream *stream, const char* name, void *value, unsigned int len);

    /**
     * @brief Get audio stream parameters
     * @param stream Pointer to the audio stream
     * @param keys Parameter keys to retrieve
     * @return Heap-allocated string with parameters, NULL if not supported
     * @details Returns heap-allocated string - caller must free with free()
     */
    char * (*get_parameters)(const struct audio_stream *stream, const char *keys);

    /**
     * @brief Add audio effect to the stream
     * @param stream Pointer to the audio stream
     * @param effect Audio effect type to add
     * @return 0 on success, negative error code on failure
     * @details Adds specified audio effect (VAD, cry detection, SSL, etc.) to the stream processing pipeline
     */
    int (*add_audio_effect)(const struct audio_stream *stream, audio_effect_type_t effect);

    /**
     * @brief Remove audio effect from the stream
     * @param stream Pointer to the audio stream
     * @param effect Audio effect type to remove
     * @return 0 on success, negative error code on failure
     * @details Removes specified audio effect from the stream processing pipeline
     */
    int (*remove_audio_effect)(const struct audio_stream *stream, audio_effect_type_t effect);
};

typedef struct audio_stream audio_stream_t;

/* ============================================================================
 * STREAM CALLBACK DEFINITIONS
 * ============================================================================ */

/**
 * @brief Stream callback event types
 * @details Mutually exclusive event types
 */
typedef enum {
    STREAM_CBK_EVENT_WRITE_READY,  /**< Non-blocking write completed */
    STREAM_CBK_EVENT_DRAIN_READY,  /**< Drain completed */
    STREAM_CBK_EVENT_ERROR,        /**< Stream error - let AudioFlinger handle */
} stream_callback_event_t;

/**
 * @brief Stream event callback types
 */
typedef enum {
    STREAM_EVENT_CBK_TYPE_CODEC_FORMAT_CHANGED,  /**< Codec format of stream changed */
} stream_event_callback_type_t;

/**
 * @brief Stream callback function type
 * @param event Stream callback event type
 * @param param Event-specific parameter data
 * @param cookie User data passed during callback registration
 * @return 0 on success, negative error code on failure
 * @details Callback function for stream events like write completion, drain completion, and errors
 */
typedef int (*stream_callback_t)(stream_callback_event_t event, void *param, void *cookie);

/**
 * @brief Stream event callback function type
 * @param event Stream event callback type
 * @param param Event-specific parameter data
 * @param cookie User data passed during callback registration
 * @return 0 on success, negative error code on failure
 * @details Callback function for stream events like codec format changes
 */
typedef int (*stream_event_callback_t)(stream_event_callback_type_t event,
                                       void *param, void *cookie);

/* ============================================================================
 * AUDIO STREAM BUFFER
 * ============================================================================ */

/**
 * @brief Audio stream buffer structure
 * @details Represents a single buffer from an audio stream
 */
typedef struct audio_stream_buffer {
    struct audio_config *config;  /**< Stream configuration */
    void *data;                   /**< Buffer data pointer */
    uint32_t data_size;           /**< Size of data in bytes */
    int status;                   /**< Buffer status (audio_buffer_status_t) */
} audio_stream_buffer_t;

/* ============================================================================
 * AUDIO CAPTURE RESULT
 * ============================================================================ */

/**
 * @brief Audio capture result structure
 * @details Contains results from audio capture operations
 */
typedef struct audio_capture_result {
    uint32_t frame_number;        /**< Frame number - incrementing integer to uniquely identify this capture */
    uint64_t timestamp;           /**< Timestamp for start of capture */
    audio_stream_buffer_t *output_buffer;  /**< Output stream buffer handles for this capture */
    audio_stream_buffer_t *input_buffer;   /**< Input stream buffer handle for this capture */
} audio_capture_result_t;

/* ============================================================================
 * AUDIO CALLBACK OPERATIONS
 * ============================================================================ */

/**
 * @brief Audio callback operations structure
 * @details Defines HAL-to-framework communication
 */
typedef struct audio_callback_ops {
    /**
     * @brief Send results from completed capture to framework
     * @param callback_ops Pointer to callback operations structure
     * @param result Capture result data
     * @details May be called multiple times by HAL in response to single capture request.
     * Used to send audio capture results back to the framework.
     */
    void (*process_capture_result)(const struct audio_callback_ops *,
            const audio_capture_result_t *result);

    /**
     * @brief Asynchronous notification callback from HAL
     * @param callback_ops Pointer to callback operations structure
     * @param msg Notification message
     * @details Used for information independent of frame capture or requiring specific timing.
     * Message structure ownership remains with HAL - only valid during call duration.
     * Used for VAD, cry detection, SSL results, and other asynchronous notifications.
     */
    void (*notify)(const struct audio_callback_ops *,
            const audio_notify_msg_t *msg);
} audio_callback_ops_t;

/* ============================================================================
 * AUDIO OUTPUT STREAM
 * ============================================================================ */

/**
 * @brief Audio output stream interface
 * @details Abstraction for audio output hardware
 */
struct audio_stream_out {
    struct audio_stream common;   /**< Common audio stream methods - must be first member for proper casting */

    /**
     * @brief Set volume directly on hardware
     * @param stream Pointer to the audio output stream
     * @param volume Volume level (0.0 to 1.0)
     * @return 0 on success, negative error code on failure
     * @details May produce multiple PCM outputs or hardware-accelerated codecs. Clamps volume to valid range.
     */
    int (*set_volume)(struct audio_stream_out *stream, float volume);

    /**
     * @brief Write audio buffer to driver
     * @param stream Pointer to the audio output stream
     * @param buffer Audio data buffer
     * @param bytes Number of bytes to write
     * @return Bytes written or negative status_t
     * @details If set_callback() was called, write() must be non-blocking. Includes timing control for playback rate.
     */
    ssize_t (*write)(struct audio_stream_out *stream, void* buffer, size_t bytes);

    /**
     * @brief Set callback function for non-blocking write and drain completion
     * @param stream Pointer to the audio output stream
     * @param callback Callback function pointer
     * @param cookie User data passed to callback
     * @return 0 on success, negative error code on failure
     * @details Calling this implies all future write() and drain() must be non-blocking
     */
    int (*set_callback)(struct audio_stream_out *stream,
            stream_callback_t callback, void *cookie);

    /**
     * @brief Pause playback while retaining queued buffers
     * @param stream Pointer to the audio output stream
     * @return 0 on success, negative error code on failure
     * @details Useful for pause/resume implementation, mandatory for offloaded playback
     */
    int (*pause)(struct audio_stream_out* stream);

    /**
     * @brief Resume playback following a pause
     * @param stream Pointer to the audio output stream
     * @return 0 on success, negative error code on failure
     * @details Returns error if called without matching pause, mandatory for offloaded playback
     */
    int (*resume)(struct audio_stream_out* stream);

    /**
     * @brief Set callback function for output stream events
     * @param stream Pointer to the audio output stream
     * @param callback Event callback function pointer
     * @param cookie User data passed to callback
     * @return 0 on success, negative error code on failure
     * @details Sets callback for stream events like codec format changes
     */
    int (*set_event_callback)(struct audio_stream_out *stream,
                              stream_event_callback_t callback,
                              void *cookie);

    /**
     * @brief Get Audio Description Mix level in dB
     * @param stream Pointer to the audio output stream
     * @param leveldB Pointer to store mix level in dB
     * @return 0 on success, -EINVAL for invalid args, -ENOSYS if not available
     * @details Retrieves current audio description mix level
     */
    int (*get_audio_description_mix_level)(struct audio_stream_out *stream, float *leveldB);

    /**
     * @brief Set Audio Description Mix level in dB
     * @param stream Pointer to the audio output stream
     * @param leveldB Mix level in dB to set
     * @return 0 on success, -EINVAL for invalid args, -ENOSYS if not available
     * @details Sets audio description mix level
     */
    int (*set_audio_description_mix_level)(struct audio_stream_out *stream, const float leveldB);

    /**
     * @brief Get current playback rate parameters
     * @param stream Pointer to the audio output stream
     * @param playbackRate Pointer to store playback rate parameters
     * @return 0 on success, -EINVAL for invalid args, -ENOSYS if not available
     * @details Retrieves current playback speed, pitch, stretch mode, and fallback mode
     */
    int (*get_playback_rate_parameters)(struct audio_stream_out *stream,
                                        audio_playback_rate_t *playbackRate);

    /**
     * @brief Set playback rate parameters that control playback behavior
     * @param stream Pointer to the audio output stream
     * @param playbackRate Playback rate parameters to set
     * @return 0 on success, -EINVAL for invalid args, -ENOSYS if not available
     * @details Sets playback speed, pitch, stretch mode, and fallback mode for timestretch functionality
     */
    int (*set_playback_rate_parameters)(struct audio_stream_out *stream,
                                        const audio_playback_rate_t *playbackRate);
};

typedef struct audio_stream_out audio_stream_out_t;

/* ============================================================================
 * AUDIO INPUT STREAM
 * ============================================================================ */

/**
 * @brief Audio input stream interface
 * @details Abstraction for audio input hardware
 */
struct audio_stream_in {
    struct audio_stream common;   /**< Common audio stream methods - must be first member for proper casting */

    /**
     * @brief Set input gain for audio driver
     * @param stream Pointer to the audio input stream
     * @param volume Input gain level (0.0 to 1.0)
     * @return 0 on success, negative error code on failure
     * @details For future use. Clamps volume to valid range and applies to input gain control.
     */
    int (*set_volume)(struct audio_stream_in *stream, float volume);

    /**
     * @brief Read audio buffer from audio driver
     * @param stream Pointer to the audio input stream
     * @param buffer Audio data buffer
     * @param bytes Number of bytes to read
     * @return Bytes read or negative status_t
     * @details Should return successful byte count if at least one frame was read before error.
     * Includes timing control for capture rate and audio effect processing.
     */
    ssize_t (*read)(struct audio_stream_in *stream, void* buffer, size_t bytes);
};

typedef struct audio_stream_in audio_stream_in_t;

/* ============================================================================
 * AUDIO MODULE AND DEVICE STRUCTURES
 * ============================================================================ */

/**
 * @brief Audio module structure
 * @details Must begin with hw_module_t followed by module-specific info
 */
struct audio_module {
    struct hw_module_t common;
};

/* ============================================================================
 * AUDIO ERROR CODES
 * ============================================================================ */

/**
 * @brief Audio module error code definitions
 * @details Error codes for audio hardware operations
 */
#define HARDWARE_AUDIO_OK                      SYSTEM_SUCCESS
#define HARDWARE_AUDIO_ERROR_INVALID_PARAM     SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_AUDIO, ERROR_INVALID_PARAM)
#define HARDWARE_AUDIO_ERROR_NOT_INIT          SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_AUDIO, ERROR_NOT_INIT)
#define HARDWARE_AUDIO_ERROR_MEMORY            SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_AUDIO, ERROR_MEMORY)
#define HARDWARE_AUDIO_ERROR_TIMEOUT           SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_AUDIO, ERROR_TIMEOUT)
#define HARDWARE_AUDIO_ERROR_NOT_FOUND         SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_AUDIO, ERROR_NOT_FOUND)
#define HARDWARE_AUDIO_ERROR_BUSY              SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_AUDIO, ERROR_BUSY)
#define HARDWARE_AUDIO_ERROR_IO                SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_AUDIO, ERROR_IO)
#define HARDWARE_AUDIO_ERROR_PERMISSION        SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_AUDIO, ERROR_PERMISSION_DENIED)
#define HARDWARE_AUDIO_ERROR_NOT_SUPPORTED     SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_AUDIO, ERROR_NOT_SUPPORTED)
#define HARDWARE_AUDIO_ERROR_INVALID_STATE     SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_AUDIO, ERROR_INVALID_STATE)
#define HARDWARE_AUDIO_ERROR_OPERATION_FAILED  SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_AUDIO, ERROR_OPERATION_FAILED)

/* Audio-specific error codes (0x0017-0x001F) */
#define HARDWARE_AUDIO_ERROR_STREAM_OPEN_FAILED   SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_AUDIO, 0x0017)
#define HARDWARE_AUDIO_ERROR_STREAM_PARAM_INVALID SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_AUDIO, 0x0018)
#define HARDWARE_AUDIO_ERROR_CODEC_UNSUPPORTED    SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_AUDIO, 0x0019)
#define HARDWARE_AUDIO_ERROR_DEVICE_BUSY          SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_AUDIO, 0x001A)
#define HARDWARE_AUDIO_ERROR_DEVICE_NOT_AVAILABLE SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_AUDIO, 0x001B)

/* ============================================================================
 * AUDIO MODULE AND DEVICE STRUCTURES
 * ============================================================================ */

/**
 * @brief Audio hardware device structure
 * @details Main interface for audio HAL operations
 */
struct audio_hw_device {
    struct hw_device_t common;    /**< Common hardware device methods - must be first member for proper casting */

    /**
     * @brief One-time initialization to pass framework callback function pointers to HAL
     * @param dev Pointer to the audio hardware device
     * @param callback_ops Framework callback operations structure
     * @return 0 on success, negative error code on failure
     * @details Called once after successful open() call, before any other functions.
     * Stores callback operations for HAL-to-framework communication.
     */
    int (*set_callback_ops)(const struct audio_hw_device *dev,
            const audio_callback_ops_t *callback_ops);

    /**
     * @brief Get supported devices bitmask
     * @param dev Pointer to the audio hardware device
     * @return Bitmask of supported audio devices
     * @details Used by AudioFlinger to enumerate supported devices for each HAL implementation.
     * Return value is bitmask of audio_devices_t values.
     */
    uint32_t (*get_supported_devices)(const struct audio_hw_device *dev);

    /**
     * @brief Get current master volume value for HAL
     * @param dev Pointer to the audio hardware device
     * @param volume Pointer to store master volume value
     * @return 0 on success, negative error code on failure
     * @details AudioFlinger queries this from primary audio HAL at service start.
     * HALs not supporting this may leave it set to NULL.
     */
    int (*get_master_volume)(struct audio_hw_device *dev, float *volume);

    /**
     * @brief Set audio mode when mode changes
     * @param dev Pointer to the audio hardware device
     * @param mode New audio mode to set
     * @return 0 on success, negative error code on failure
     * @details AUDIO_MODE_NORMAL: standard audio playback
     * AUDIO_MODE_MUSIC: music playback
     * AUDIO_MODE_CALL: call in progress
     * Reloads effect configurations when mode changes.
     */
    int (*set_mode)(struct audio_hw_device *dev, audio_mode_t mode);

    /**
     * @brief Microphone mute control
     * @param dev Pointer to the audio hardware device
     * @param state Mute state to set (true = muted, false = unmuted)
     * @return 0 on success, negative error code on failure
     * @details Controls microphone mute state through hardware interface
     */
    int (*set_mic_mute)(struct audio_hw_device *dev, bool state);

    /**
     * @brief Get microphone mute state
     * @param dev Pointer to the audio hardware device
     * @param state Pointer to store mute state
     * @return 0 on success, negative error code on failure
     * @details Retrieves current microphone mute state from hardware
     */
    int (*get_mic_mute)(const struct audio_hw_device *dev, bool *state);

    /**
     * @brief Set global audio parameters
     * @param stream Pointer to the audio stream
     * @param name Parameter name
     * @param value Parameter value pointer
     * @param len Length of parameter value
     * @return 0 on success, negative error code on failure
     * @details Sets global audio parameters for the device
     */
    int (*set_parameters)(const struct audio_stream *stream, const char* name, void *value, unsigned int len);

    /**
     * @brief Get global audio parameters
     * @param dev Pointer to the audio hardware device
     * @param keys Parameter keys to retrieve
     * @return Heap-allocated string with parameters, NULL if not supported
     * @details Returns heap-allocated string - caller must free with free()
     */
    char * (*get_parameters)(const struct audio_hw_device *dev, const char *keys);

    /**
     * @brief Get audio input buffer size according to parameters
     * @param dev Pointer to the audio hardware device
     * @return Buffer size in bytes, 0 if any parameter is not supported
     * @details Returns the recommended input buffer size based on current configuration
     */
    size_t (*get_input_buffer_size)(const struct audio_hw_device *dev);

    /**
     * @brief Create and open audio hardware output stream
     * @param dev Pointer to the audio hardware device
     * @param handle Audio I/O handle
     * @param devices Output devices to use
     * @param config Audio configuration
     * @param stream_out Pointer to store created output stream
     * @return 0 on success, negative error code on failure
     * @details Creates and initializes an audio output stream with specified configuration.
     * Opens hardware output interface and loads effect configurations.
     */
    int (*open_output_stream)(struct audio_hw_device *dev,
                              audio_io_handle_t handle,
                              audio_devices_t devices,
                              struct audio_config *config,
                              struct audio_stream_out **stream_out);

    /**
     * @brief Close and destroy audio output stream
     * @param dev Pointer to the audio hardware device
     * @param stream_out Output stream to close
     * @details Closes hardware output interface and frees stream resources
     */
    void (*close_output_stream)(struct audio_hw_device *dev,
                                struct audio_stream_out* stream_out);

    /**
     * @brief Create and open audio hardware input stream
     * @param dev Pointer to the audio hardware device
     * @param handle Audio I/O handle
     * @param devices Input devices to use
     * @param config Audio configuration
     * @param stream_in Pointer to store created input stream
     * @return 0 on success, negative error code on failure
     * @details Creates and initializes an audio input stream with specified configuration.
     * Opens hardware input interface, starts capture thread, and loads effect configurations.
     */
    int (*open_input_stream)(struct audio_hw_device *dev,
                             audio_io_handle_t handle,
                             audio_devices_t devices,
                             struct audio_config *config,
                             struct audio_stream_in **stream_in);

    /**
     * @brief Close and destroy audio input stream
     * @param dev Pointer to the audio hardware device
     * @param stream_in Input stream to close
     * @details Closes hardware input interface, stops capture thread, and frees stream resources
     */
    void (*close_input_stream)(struct audio_hw_device *dev,
                               struct audio_stream_in *stream_in);
};

typedef struct audio_hw_device audio_hw_device_t;

/* ============================================================================
 * CONVENIENCE FUNCTIONS
 * ============================================================================ */

/**
 * @brief Convenience function to open a supported audio hardware device
 * @param module Hardware module pointer
 * @param device Pointer to store opened audio hardware device
 * @return 0 on success, negative error code on failure
 * @details Opens audio hardware device using the standard hardware module interface
 */
static inline int audio_hw_device_open(const struct hw_module_t* module,
                                       struct audio_hw_device** device)
{
    return module->methods->open(module, AUDIO_HARDWARE_INTERFACE,
                                 TO_HW_DEVICE_T_OPEN(device));
}

/**
 * @brief Convenience function to close an audio hardware device
 * @param device Audio hardware device to close
 * @return 0 on success, negative error code on failure
 * @details Closes audio hardware device and frees associated resources
 */
static inline int audio_hw_device_close(struct audio_hw_device* device)
{
    return device->common.close(&device->common);
}

/* ============================================================================
 * MODULE INITIALIZATION
 * ============================================================================ */

/**
 * @brief Initialize the audio hardware module
 * @return 0 on success, negative error code on failure
 * @details Initializes the audio hardware module and registers it with the system.
 * This function is called during system startup to set up the audio HAL.
 */
int hw_audio_module_init(void);

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_AUDIO_H */
