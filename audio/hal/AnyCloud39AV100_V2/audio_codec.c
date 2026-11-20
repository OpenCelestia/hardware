// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/audio.
 *
 * Audio codec implementation for AnyCloud39AV100_V2 platform.
 * - AENC/ADEC via ak_aenc/ak_adec (AV200 uses AK_MPI_AENC/ADEC).
 * - Encode input is the PCM copy from pcm_read, not the SDK Echo frame.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../../audio_hw.h"
#include "audio.h"
#include "hal_audio.h"

#include "ak_common.h"
#include "ak_audio_compat.h"
#include "audio.h"
#include "adec_aac.h"
#include "adec_amr.h"
#include "adec_mp3.h"
#include "aenc_aac.h"
#include "aenc_amr.h"
#include "aenc_mp3.h"
#include "ak_thread.h"

#include "log.h"
#define LOG_TAG "HAL AUDIO"

static enum ak_audio_sample_rate codec_map_sample_rate(int rate_hz)
{
    switch (rate_hz) {
    case 8000:  return AK_AUDIO_SAMPLE_RATE_8000;
    case 9600:  return AK_AUDIO_SAMPLE_RATE_9600;
    case 11025: return AK_AUDIO_SAMPLE_RATE_11025;
    case 12000: return AK_AUDIO_SAMPLE_RATE_12000;
    case 16000: return AK_AUDIO_SAMPLE_RATE_16000;
    case 22050: return AK_AUDIO_SAMPLE_RATE_22050;
    case 24000: return AK_AUDIO_SAMPLE_RATE_24000;
    case 32000: return AK_AUDIO_SAMPLE_RATE_32000;
    case 44100: return AK_AUDIO_SAMPLE_RATE_44100;
    case 48000: return AK_AUDIO_SAMPLE_RATE_48000;
    case 96000: return AK_AUDIO_SAMPLE_RATE_96000;
    default:
        return (enum ak_audio_sample_rate)rate_hz;
    }
}

ak_pthread_t in_codec_thread;
ak_pthread_t out_codec_thread;
void *audio_codec_in_thread(void *arg);
void *audio_codec_out_thread(void *arg);

/* ak_aenc_sample.c: sleep_time = frame_interval ? (frame_interval / 2) : 16 (ms) */
static int aenc_poll_sleep_ms(const struct stream_in *in)
{
    int interval = in ? in->in_frame_interval : 0;

    if (interval <= 0)
        return 16;
    return interval / 2 > 0 ? interval / 2 : 16;
}

int hal_audio_codec_in_open(void *hal_audio, struct stream_in *in_arg)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    struct audio_device *adev = my_data->adev;
    struct stream_in *in = in_arg != NULL ? in_arg : adev->in;

    if (in == NULL) {
        log_error("hal_audio_codec_in_open: no input stream (in_arg=NULL and adev->in=NULL)");
        return false;
    }

    /* Uses ak_aenc_* API (not AK_MPI_AENC_*) */
    struct aenc_param stAencParam = {0};
    int s32AencUserId = 0;

    log_info("hal_audio_codec_in_open: format=0x%x, sample_rate=%d, channel_mask=0x%x",
             in->format, in->sample_rate, in->channel_mask);

    stAencParam.aenc_data_attr.sample_bits = AK_AUDIO_SMPLE_BIT_16;
    stAencParam.aenc_data_attr.sample_rate = in->sample_rate;

    audio_channel_mask_t main_channel = in->channel_mask & AUDIO_CHANNEL_IN_MAIN_MASK;

    if (main_channel != AUDIO_CHANNEL_IN_MONO && main_channel != AUDIO_CHANNEL_IN_STEREO) {
        log_warn("[AENC] main_channel=0x%x (full mask=0x%x) invalid; force MONO for encoder",
                 (unsigned)main_channel, (unsigned)in->channel_mask);
        in->channel_mask = (in->channel_mask & ~AUDIO_CHANNEL_IN_MAIN_MASK) | AUDIO_CHANNEL_IN_MONO;
        in->pcm_config.channel_mask = in->channel_mask;
        main_channel = AUDIO_CHANNEL_IN_MONO;
    }

    if(main_channel == AUDIO_CHANNEL_IN_MONO)
    {
        stAencParam.aenc_data_attr.channel_num = 1;
    }
    else if(main_channel == AUDIO_CHANNEL_IN_STEREO)
    {
        stAencParam.aenc_data_attr.channel_num = 2;
    }
    else
    {
        log_error("audio channel not support, input only support MONO and STEREO");
       return false;
    }

    /*use to register*/
    aenc_encode_s pstEncode = {0};

    switch (in->format & AUDIO_FORMAT_MAIN_MASK) {
        case AUDIO_FORMAT_MP3:
            log_info("set format to AK_AUDIO_TYPE_MP3");
            pstEncode.enType = AK_AUDIO_TYPE_MP3;
            stAencParam.type = AK_AUDIO_TYPE_MP3;
            pstEncode.pfnEncOpen = AENC_OpenMp3;
            pstEncode.pfnEncVersion = AENC_Mp3_GetVersionInfo;

            if (ak_aenc_registerencoder(&pstEncode)) {
                log_info("ak_aenc_registerencoder failed");
                return AK_FAILED;
            }
            break;
        case AUDIO_FORMAT_AAC:
            log_info("set format to  AK_AUDIO_TYPE_AAC");
            pstEncode.enType = AK_AUDIO_TYPE_AAC;
            stAencParam.type = AK_AUDIO_TYPE_AAC;
            pstEncode.pfnEncOpen = AENC_OpenAac;
            pstEncode.pfnEncVersion = AENC_Aac_GetVersionInfo;
            if (ak_aenc_registerencoder(&pstEncode)) {
                log_info("ak_aenc_registerencoder failed");
                return AK_FAILED;
            }
            break;
        case AUDIO_FORMAT_AMR:
            log_info("set format to AK_AUDIO_TYPE_AMR");
            pstEncode.enType = AK_AUDIO_TYPE_AMR;
            stAencParam.type = AK_AUDIO_TYPE_AMR;
            pstEncode.pfnEncOpen = AENC_OpenAmr;
            pstEncode.pfnEncVersion = AENC_Amr_GetVersionInfo;
            if (ak_aenc_registerencoder(&pstEncode)) {
                log_info("ak_aenc_registerencoder failed");
                return AK_FAILED;
            }
            break;
        case AUDIO_FORMAT_ALAW://don't need regist, just open adec
            log_info("set format to AUDIO_FORMAT_ALAW");
            pstEncode.enType = AK_AUDIO_TYPE_PCM_ALAW;
            stAencParam.type = AK_AUDIO_TYPE_PCM_ALAW;
            break;

        case AUDIO_FORMAT_ULAW://don't need regist, just open adec
            log_info("set format to AUDIO_FORMAT_ULAW");

            pstEncode.enType = AK_AUDIO_TYPE_PCM_ULAW;
            stAencParam.type = AK_AUDIO_TYPE_PCM_ULAW;
            break;

        case AUDIO_FORMAT_PCM://don't need regist, just open adec
            log_info("set format to AUDIO_FORMAT_PCM");

            pstEncode.enType = AK_AUDIO_TYPE_PCM;
            stAencParam.type = AK_AUDIO_TYPE_PCM;
            break;
        default:
            break;
    }

    log_info("hal_audio_codec_in_open: Opening AENC with type=%d (format=0x%x), sample_rate=%d",
             stAencParam.type, in->format, stAencParam.aenc_data_attr.sample_rate);
    
    if (ak_aenc_open(&stAencParam, &s32AencUserId) != 0) {
        log_error("ak_aenc_open failed: codec_type=%d, format=0x%x", stAencParam.type, in->format);
        my_data->in_codec_id = -1;  // Mark as not opened
        return AK_FAILED;
    }

    my_data->in_codec_id = s32AencUserId;
    log_info("AENC opened successfully: codec_id=%d, codec_type=%d (format=0x%x)", 
             my_data->in_codec_id, stAencParam.type, in->format);
    my_data->in_codec_buffer_release_flag = false;

    /* AAC: follow sample to set frame head attribute after open */
    if (stAencParam.type == AK_AUDIO_TYPE_AAC) {
        struct aenc_attr attr;
        attr.aac_head = AENC_AAC_SAVE_FRAME_HEAD;
        if (ak_aenc_set_attr(my_data->in_codec_id, &attr) != 0) {
            log_error("ak_aenc_set_attr failed: codec_id=%d", my_data->in_codec_id);
            return AK_FAILED;
        }
    }

    /* Framework matches streams by HAL format on this buffer; input_config is often PCM/DEFAULT. */
    memset(&in->enc_out_config, 0, sizeof(in->enc_out_config));
    in->enc_out_config.format = in->format;
    /* pcm_config may not be filled yet when codec opens before memcpy in adev_open_input_stream */
    in->enc_out_config.sample_rate = in->pcm_config.sample_rate ? in->pcm_config.sample_rate : in->sample_rate;
    in->enc_out_config.channel_mask = in->pcm_config.channel_mask ? in->pcm_config.channel_mask : in->channel_mask;

    return true;
}




void *audio_codec_in_thread(void *arg)
{
    ak_thread_set_name("aenc");
    struct hal_audio_data *my_data = (struct hal_audio_data *)arg;
    struct audio_device *adev = my_data->adev;
    struct stream_in *in = adev->in;
    FRAME_S AIFRAME  = {0};
    AK_S32 ret = AK_FAILED;

    size_t bytes_read = 0;

    log_debug( "%s", __func__);

//    AK_BASE_SleepMs(30); 

    audio_capture_result_t result = {};

    /* Operator precedence: `&` is lower than `>>` so the original
     * `mask & MAIN_MASK >> BIT` evaluated as `mask & (MAIN_MASK >> BIT)`.
     * Add explicit parentheses to extract the channel count correctly. */
    int sleep_time = aenc_poll_sleep_ms(in);

    // Check if codec is valid before entering loop
    if (my_data->in_codec_id < 0) {
        log_error("audio_codec_in_thread: AENC not opened (in_codec_id=%d), exiting thread", my_data->in_codec_id);
        ak_thread_exit();
        return NULL;
    }

    while(my_data->encoding_flag) {
        // Check if codec is still valid BEFORE any AENC operations
        // This prevents calling AENC functions with invalid codec_id
        if (my_data->in_codec_id < 0) {
            log_warn("audio_codec_in_thread: AENC closed during encoding (in_codec_id=%d), exiting gracefully", my_data->in_codec_id);
            break;
        }

        if(my_data->in_codec_buffer_release_flag == true)
        {
            // Double-check codec_id is still valid before releasing
            if (my_data->in_codec_id >= 0) {
                ak_aenc_release_stream(my_data->in_codec_id, &my_data->aenc_frame);
            }
            my_data->in_codec_buffer_release_flag = false;
        }

        memset(&my_data->aenc_frame, 0x00, sizeof(AUDIO_STREAM_S));

        // Double-check codec_id is still valid before calling AENC_GetStream
        if (my_data->in_codec_id < 0) {
            log_warn("audio_codec_in_thread: AENC closed before GetStream (in_codec_id=%d), exiting", my_data->in_codec_id);
            break;
        }

        ret = ak_aenc_get_stream(my_data->in_codec_id, &my_data->aenc_frame, 0);
        if (!ret && my_data->aenc_frame.data != NULL && my_data->aenc_frame.len > 0)
        {
            static int s_logged_first_aenc_stream;

            if (!s_logged_first_aenc_stream) {
                log_info("AENC first encoded frame: len=%u", my_data->aenc_frame.len);
                s_logged_first_aenc_stream = 1;
            }

            my_data->in_codec_buffer_release_flag = true;
            result.frame_number = my_data->aenc_frame.seq_no;
            result.timestamp = my_data->aenc_frame.ts;
            in->output_codec_buffer.config = &in->enc_out_config;
            in->output_codec_buffer.data = my_data->aenc_frame.data;
            in->output_codec_buffer.data_size = my_data->aenc_frame.len;

            log_debug("audio_codec_in_thread: ak_aenc_get_stream: codec_id=%d, format=0x%x, encoded_size=%u, seq=%lu",
                     my_data->in_codec_id, in->format, my_data->aenc_frame.len, my_data->aenc_frame.seq_no);

            result.output_buffer = &in->output_codec_buffer;

            if (adev->callback_ops != NULL && adev->callback_ops->process_capture_result != NULL)
                adev->callback_ops->process_capture_result(adev->callback_ops, &result);
        }
        else if (!ret)
        {
            continue;
        }
        else
        {
            if (ERROR_AENC_NO_DATA == ret) {
                ak_sleep_ms(sleep_time);
                continue;
            }
            else 
            {
                log_error("ak_aenc_get_stream error ret=%d", ret);
                break;
            }
        }
    }

    /* Drain encoded frames still in AENC after encoding_flag cleared. */
    for (int drain = 0; drain < 64 && my_data->in_codec_id >= 0; drain++) {
        if (my_data->in_codec_buffer_release_flag) {
            ak_aenc_release_stream(my_data->in_codec_id, &my_data->aenc_frame);
            my_data->in_codec_buffer_release_flag = false;
        }

        memset(&my_data->aenc_frame, 0x00, sizeof(AUDIO_STREAM_S));
        ret = ak_aenc_get_stream(my_data->in_codec_id, &my_data->aenc_frame, 0);
        if (!ret && my_data->aenc_frame.data != NULL && my_data->aenc_frame.len > 0) {
            my_data->in_codec_buffer_release_flag = true;
            result.frame_number = my_data->aenc_frame.seq_no;
            result.timestamp = my_data->aenc_frame.ts;
            in->output_codec_buffer.config = &in->enc_out_config;
            in->output_codec_buffer.data = my_data->aenc_frame.data;
            in->output_codec_buffer.data_size = my_data->aenc_frame.len;
            result.output_buffer = &in->output_codec_buffer;
            if (adev->callback_ops != NULL &&
                adev->callback_ops->process_capture_result != NULL)
                adev->callback_ops->process_capture_result(adev->callback_ops, &result);
            continue;
        }
        if (ERROR_AENC_NO_DATA == ret)
            break;
        if (!ret)
            continue;
        break;
    }
    
    ak_thread_exit();

    return NULL;
}



size_t hal_audio_in_codec_enable(void *hal_audio)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;

    if (my_data->encoding_flag)
        return 0;

    my_data->encoding_flag = true;
    ak_thread_create(&in_codec_thread, audio_codec_in_thread, my_data, ANYKA_THREAD_MIN_STACK_SIZE, 19);
    my_data->in_codec_thread_started = true;
    return 0;
}

size_t hal_audio_in_codec_disable(void *hal_audio)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;

    my_data->encoding_flag = false;

    return 0;
}


size_t hal_audio_in_codec_read(void *hal_audio, void **buffer, uint32_t *frame_number, uint64_t * timestamp, size_t bytes)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    struct audio_device *adev = my_data->adev;
    struct stream_in *in = adev->in;
    int ret;
    size_t bytes_read = 0;

    if (my_data->ai_handle_id < 0) {
        log_warn("hal_audio_in_codec_read: ai_handle invalid, audio input may be closed");
        return 0;
    }

    if (my_data->in_codec_id < 0) {
        log_debug("hal_audio_in_codec_read: AENC not opened (in_codec_id=%d)", my_data->in_codec_id);
        return 0;
    }

    if (!my_data->pcm_buffer_release_flag ||
        my_data->ai_frame.data == NULL || my_data->ai_frame.len <= 0) {
        return 0;
    }

    /* pcm_read already released the SDK frame; send the HAL copy. */
    int s32Ret;
    int retry;

    for (retry = 0; retry < 8; retry++) {
        s32Ret = ak_aenc_send_frame(my_data->in_codec_id, &my_data->ai_frame, 0);
        if (!s32Ret)
            break;
        if (s32Ret == ERROR_AENC_NO_DATA)
            return 0;
        usleep(10000);
    }
    if (s32Ret) {
        log_error("ak_aenc_send_frame error: ret=%d, codec_id=%d", s32Ret, my_data->in_codec_id);
        my_data->pcm_buffer_release_flag = false;
        return 0;
    }

    my_data->pcm_buffer_release_flag = false;
    return 0;
}


int hal_audio_codec_in_close( void *hal_audio)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;

    // Check if codec was actually opened
    if (my_data->in_codec_id < 0) {
        log_debug("hal_audio_codec_in_close: AENC not opened (in_codec_id=%d), skipping close", my_data->in_codec_id);
        return 0;  // Not an error - codec was never opened
    }

    my_data->encoding_flag = false;

    if (my_data->in_codec_thread_started) {
        ak_thread_join(in_codec_thread);
        my_data->in_codec_thread_started = false;
    }
    
    // Release any pending stream buffer (only if codec is still valid)
    if(my_data->in_codec_buffer_release_flag == true && my_data->in_codec_id >= 0)
    {
        ak_aenc_release_stream(my_data->in_codec_id, &my_data->aenc_frame);
        my_data->in_codec_buffer_release_flag = false;
    }

    // Save codec_id before marking as closed (for error logging)
    int codec_id = my_data->in_codec_id;
    
    // Mark codec as closed AFTER thread has exited (thread is no longer using it)
    my_data->in_codec_id = -1;
    
    int ret = ak_aenc_close(codec_id);
    if (ret != 0) {
        log_warn("ak_aenc_close failed: ret=%d, codec_id=%d", ret, codec_id);
    }
    
    log_debug("hal_audio_codec_in_close: AENC closed, codec_id=%d", codec_id);
    
    return ret;
}

int hal_audio_codec_out_open( void *hal_audio)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    struct audio_device *adev = my_data->adev;
    struct stream_out *out = adev->out;
    int s32AdecUserId = 0;

    log_debug("audio_codec_out_open");

    struct adec_param stAdecParam = {0};

    my_data->out_param.pcm_data_attr.sample_bits = AK_AUDIO_SMPLE_BIT_16;

    audio_channel_mask_t main_channel = out->channel_mask & AUDIO_CHANNEL_OUT_MAIN_MASK;
    if(main_channel == AUDIO_CHANNEL_OUT_MONO)
    {
        my_data->out_param.pcm_data_attr.channel_num = AUDIO_CHANNEL_MONO;
    }
    else if(main_channel == AUDIO_CHANNEL_OUT_STEREO)
    {
        my_data->out_param.pcm_data_attr.channel_num = AUDIO_CHANNEL_STEREO;
    }
    else
    {
        log_error("audio channel not support, output only support MONO and STEREO");
        return AK_FAILED;
    }

    my_data->out_param.pcm_data_attr.sample_rate = codec_map_sample_rate(out->sample_rate);

    stAdecParam.adec_data_attr.sample_bits = AK_AUDIO_SMPLE_BIT_16;
    stAdecParam.adec_data_attr.sample_rate = my_data->out_param.pcm_data_attr.sample_rate;
    stAdecParam.adec_data_attr.channel_num = my_data->out_param.pcm_data_attr.channel_num;

    adec_decode_s stDecCaBck = {0};

    switch (out->format) {
        case AUDIO_FORMAT_MP3:
            // register mp3 decoder
            log_info("AUDIO_FORMAT_MP3");
            stAdecParam.type = AK_AUDIO_TYPE_MP3;
            stDecCaBck.enType = AK_AUDIO_TYPE_MP3;
            stDecCaBck.pfnDecOpen = ADEC_OpenMp3;
            stDecCaBck.pfnDecVersion = ADEC_Mp3_GetVersionInfo;

            if (ak_adec_registerdecoder(&stDecCaBck)) {
                log_error("MP3 ak_adec_registerdecoder failed");
                return AK_FAILED;
            }
            break;
        case AUDIO_FORMAT_AAC:
            // register aac decoder
            log_info("AK_AUDIO_TYPE_AAC");
            stAdecParam.type = AK_AUDIO_TYPE_AAC;
            stDecCaBck.enType = AK_AUDIO_TYPE_AAC;
            stDecCaBck.pfnDecOpen = ADEC_OpenAac;
            stDecCaBck.pfnDecVersion = ADEC_Aac_GetVersionInfo;
            if (ak_adec_registerdecoder(&stDecCaBck)) {
                log_error("AAC ak_adec_registerdecoder failed");
                return AK_FAILED;
            }
            break;
        case AUDIO_FORMAT_AMR:
            // register amr decoder
            log_info("AK_AUDIO_TYPE_AMR");
            stAdecParam.type = AK_AUDIO_TYPE_AMR;
            stDecCaBck.enType = AK_AUDIO_TYPE_AMR;
            stDecCaBck.pfnDecOpen = ADEC_OpenAmr;
            stDecCaBck.pfnDecVersion = ADEC_Amr_GetVersionInfo;
            if (ak_adec_registerdecoder(&stDecCaBck)) {
                log_error("AMR ak_adec_registerdecoder failed");
                return AK_FAILED;
            }
            break;
        case AUDIO_FORMAT_ALAW://don't need regist, just open  adec
            log_info("AUDIO_FORMAT_ALAW");
            stAdecParam.type = AK_AUDIO_TYPE_PCM_ALAW;
            stDecCaBck.enType = AK_AUDIO_TYPE_PCM_ALAW;
            break;

        case AUDIO_FORMAT_ULAW://don't need regist, just open  adec
            log_error("AUDIO_FORMAT_ULAW");
            stAdecParam.type = AK_AUDIO_TYPE_PCM_ULAW;
            stDecCaBck.enType = AK_AUDIO_TYPE_PCM_ULAW;
            break;

        case AUDIO_FORMAT_PCM://don't need regist, just open  adec
            log_info("AUDIO_FORMAT_PCM");
            stAdecParam.type = AK_AUDIO_TYPE_PCM;
            stDecCaBck.enType = AK_AUDIO_TYPE_PCM;
            break;
        default:
            break;
    }

    /* open adec */
    if (ak_adec_open(&stAdecParam, &s32AdecUserId)) {
        log_error("ak_adec_open failed");
        my_data->out_codec_id = -1;  // Mark as not opened
        return AK_FAILED;
    }

    my_data->out_codec_id = s32AdecUserId;
    log_info("ADEC opened: codec_id=%d, rate=%d Hz", my_data->out_codec_id, out->sample_rate);

    if (stAdecParam.type == AK_AUDIO_TYPE_AAC) {
        struct adec_aac_dec_param aac_param = {1}; /* 1 = ADTS */

        if (ak_adec_set_aac_dec_param(s32AdecUserId, &aac_param))
            log_warn("ak_adec_set_aac_dec_param (ADTS) failed");
    }

    ak_thread_create(&out_codec_thread, audio_codec_out_thread, my_data, ANYKA_THREAD_MIN_STACK_SIZE, 11);

    return AK_SUCCESS;
}


int hal_audio_codec_out_close( void *hal_audio)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;

    // Check if codec was actually opened (out_codec_id >= 0 means it was opened)
    // In recorder mode, codec is never opened, so out_codec_id remains -1 (initial value)
    if (my_data->out_codec_id < 0) {
        return 0;  // Not an error - codec was never opened
    }

    /* ak_adec_sample: send_stream_end then drain get_frame in play thread before close. */
    int send_ret = ak_adec_send_stream_end(my_data->out_codec_id);
    if (send_ret != 0) {
        log_warn("ADEC_SendEndofStream failed: ret=%d, codec_id=%d", send_ret, my_data->out_codec_id);
    }

    ak_thread_join(out_codec_thread);

    int ret = ak_adec_close(my_data->out_codec_id);
    if (ret != 0) {
        log_warn("ADEC_Close failed: ret=%d, codec_id=%d", ret, my_data->out_codec_id);
    }
    
    // Mark codec as closed
    my_data->out_codec_id = -1;
    
    return ret;
}


void *audio_codec_out_thread(void *arg)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)arg;
    struct audio_device *adev = my_data->adev;
    struct stream_out *out = adev->out;

    log_debug( "%s", __func__);

    ak_thread_set_name("[adec play]attachment");
    struct ak_dev_buf_status stBufStatus;
    FRAME_S stPcmFrame = {0};
    int play_len = 0;
    int ret = 0;
    while(my_data->playing_flag) {
        // get frame out block mode

        ret = ak_adec_get_frame(my_data->out_codec_id, &stPcmFrame, 1);
        if(ret) {
            /* Timeout/no-data during shutdown is expected; avoid error-level noise on normal exit. */
            log_debug("ADEC get frame end ret=%d, exiting output thread", ret);
            break;
        }

        if (stPcmFrame.len == 0 || stPcmFrame.data == NULL) {
            continue;
        }

        if (my_data->ao_handle_id < 0) {
            ak_adec_release_frame(my_data->out_codec_id, &stPcmFrame);
            break;
        }
        if (ak_ao_send_frame(my_data->ao_handle_id, stPcmFrame.data, (int)stPcmFrame.len, &play_len)) {
            ak_adec_release_frame(my_data->out_codec_id, &stPcmFrame);
            break;
        }


        // release frame
        ak_adec_release_frame(my_data->out_codec_id, &stPcmFrame);
    }

    if (my_data->ao_handle_id >= 0) {
        if (ak_ao_get_buf_status(my_data->ao_handle_id, &stBufStatus)) {
            log_error("ak_ao_get_buf_status failed ");
        }
        int AoRemainLen = stBufStatus.buf_remain_len + stBufStatus.devbuf_remain_len;

        struct ak_audio_data_attr pcm_attr;
        if (ak_ao_get_params(my_data->ao_handle_id, &pcm_attr) != 0) {
            pcm_attr.sample_rate = codec_map_sample_rate(out->sample_rate);
            pcm_attr.sample_bits = AK_AUDIO_SMPLE_BIT_16;
            pcm_attr.channel_num = ((out->channel_mask & AUDIO_CHANNEL_OUT_MAIN_MASK) == AUDIO_CHANNEL_OUT_STEREO)
                ? AUDIO_CHANNEL_STEREO : AUDIO_CHANNEL_MONO;
        }

        int FlushTime = (int)ak_audio_len_to_interval(&pcm_attr, (unsigned int)AoRemainLen);
        if (ak_ao_flush(my_data->ao_handle_id)) {
            log_error("ak_ao_flush failed ");
        }
        ak_sleep_ms((unsigned int)FlushTime);
    }

    log_info( "%s exit", __func__);
    ak_thread_exit();

    return NULL;
}
