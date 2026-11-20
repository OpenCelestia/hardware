/*
 * Local compatibility wrapper for AK audio headers.
 * Avoids symbol collision between:
 * - hardware/include/audio.h: struct audio_stream
 * - system/.../ak_common_audio.h: struct audio_stream
 */
#ifndef HARDWARE_HAL_AUDIO_HAL_ANYCLOUD39AV100_V2_AK_AUDIO_COMPAT_H
#define HARDWARE_HAL_AUDIO_HAL_ANYCLOUD39AV100_V2_AK_AUDIO_COMPAT_H

#define audio_stream ak_audio_stream

/*
 * PDK gates ak_audio_vad_npu_attr / ak_ai_set_vad_npu_attr on chip-series macros.
 * The hardware build may not pass them; default to this HAL tree's chip series.
 */
#if !defined(__CHIP_KM01A_SERIES) && !defined(__CHIP_AK3918AV100_SERIES) && \
    !defined(__CHIP_AK3918AV130_SERIES)
#define __CHIP_AK3918AV130_SERIES
#endif

#include "ak_ao.h"
#include "ak_ai.h"
#include "ak_aenc.h"
#include "ak_adec.h"
#undef audio_stream

/* ---------------------------------------------------------------------------
 * AK_MPI_* compatibility (AV200-style) for this HAL codebase
 * --------------------------------------------------------------------------- */
#ifndef AUDIO_SOUND_MODE_E
typedef enum {
    AUDIO_SOUND_MODE_MONO   = 1,
    AUDIO_SOUND_MODE_STEREO = 2
} AUDIO_SOUND_MODE_E;
#endif

/* Minimal AAC param placeholder (only needed as union member) */
#ifndef AENC_AAC_PARAM_S
typedef struct {
    int reserved;
} AENC_AAC_PARAM_S;
#endif

#ifndef AENC_ENCODE_S
typedef struct akAENC_ENCODE_S {
    int enType;
    void* (*pfnEncOpen)(void *pvOpenArg, void *pstAttr);
    int (*pfnEncProcess)(void *pvEncHandle, void *pvProcArg);
    int (*pfnEncClose)(void *pvEncHandle);
    int (*pfnEncReset)(void *pvEncHandle);
    int (*pfnEncFlush)(void *pvEncHandle);
} AENC_ENCODE_S;
#endif

#ifndef AENC_PARAM_S
typedef struct akAENC_PARAM_S {
    int enSampleRate;
    int enSampleBits;
    AUDIO_SOUND_MODE_E enSoundMode;
    int enType;
    union {
        AENC_AAC_PARAM_S stAacParam;
    } unAencAttr;
} AENC_PARAM_S;
#endif

/*
 * PDK uses plain struct names (e.g. struct frame / struct audio_stream),
 * while current HAL code uses AV200-style aliases (FRAME_S / AUDIO_STREAM_S).
 * Provide local aliases so HAL source stays unchanged.
 */
#define FRAME_S struct frame
/* PDK's struct audio_stream was renamed to struct ak_audio_stream during include */
#define AUDIO_STREAM_S struct ak_audio_stream
#define AUDIO_IN_PARAM_S struct ak_audio_in_param
#define AUDIO_OUT_PARAM_S struct ak_audio_out_param
#define AUDIO_SAVE_FILE_INFO_S struct ak_audio_save_file_info

#endif /* HARDWARE_HAL_AUDIO_HAL_ANYCLOUD39AV100_V2_AK_AUDIO_COMPAT_H */
