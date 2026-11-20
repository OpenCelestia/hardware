// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/audio.
 *
 * Audio codec implementation for AnyCloud39AV200 platform.
 * - Implements audio encoder/decoder operations.
 * - Handles codec thread management and data flow.
 */

#include <stdio.h>
#include <string.h>

#include "../../audio_hw.h"
#include "audio.h"
#include "hal_audio.h"

#include "ak_aenc.h"
#include "ak_adec.h"
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

AK_PTHREAD_T in_codec_thread;
AK_PTHREAD_T out_codec_thread;
void *audio_codec_in_thread(void *arg);
void *audio_codec_out_thread(void *arg);

int hal_audio_codec_in_open(void *hal_audio, struct stream_in *in_arg)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    struct audio_device *adev = my_data->adev;
    struct stream_in *in = in_arg != NULL ? in_arg : adev->in;

    if (in == NULL) {
        log_error("hal_audio_codec_in_open: no input stream (in_arg=NULL and adev->in=NULL)");
        return false;
    }

    /*use to aenc open*/
    AENC_PARAM_S stAencParam = {0};
    int s32AencUserId =0;

    log_info("hal_audio_codec_in_open: format=0x%x, sample_rate=%d, channel_mask=0x%x",
             in->format, in->sample_rate, in->channel_mask);

    stAencParam.enSampleBits = AK_AUDIO_SMPLE_BIT_16;
    stAencParam.enSampleRate= in->sample_rate;

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
        stAencParam.enSoundMode = AUDIO_SOUND_MODE_MONO;
    }
    else if(main_channel == AUDIO_CHANNEL_IN_STEREO)
    {
        stAencParam.enSoundMode = AUDIO_SOUND_MODE_STEREO;
    }
    else
    {
        log_error("audio channel not support, input only support MONO and STEREO");
       return false;
    }

    /*use to register*/
    AENC_ENCODE_S pstEncode = {0};

    switch (in->format) {
        case AUDIO_FORMAT_MP3:
            log_info("set format to AK_AUDIO_TYPE_MP3");
            pstEncode.enType = AK_AUDIO_TYPE_MP3;
            stAencParam.enType = AK_AUDIO_TYPE_MP3;
            pstEncode.pfnEncOpen = AENC_OpenMp3;

            if (AK_MPI_AENC_RegisterEncoder(&pstEncode)) {
                log_info( "AK_MPI_AENC_RegisterEncoder failed");
                return AK_FAILED;
            }
            break;
        case AUDIO_FORMAT_AAC:
            log_info("set format to  AK_AUDIO_TYPE_AAC");
            pstEncode.enType = AK_AUDIO_TYPE_AAC;
            stAencParam.enType = AK_AUDIO_TYPE_AAC;
            /*set aac param*/
            stAencParam.unAencAttr.stAacParam.enAacFrmHead = AENC_AAC_SAVE_FRAME_HEAD;
            pstEncode.pfnEncOpen = AENC_OpenAac;
            if (AK_MPI_AENC_RegisterEncoder(&pstEncode)) {
                log_info(  "AK_MPI_AENC_RegisterEncoder failed");
                return AK_FAILED;
            }
            break;
        case AUDIO_FORMAT_AMR:
            log_info("set format to AK_AUDIO_TYPE_AMR");
            pstEncode.enType = AK_AUDIO_TYPE_AMR;
            stAencParam.enType = AK_AUDIO_TYPE_AMR;
            pstEncode.pfnEncOpen = AENC_OpenAmr;
            if (AK_MPI_AENC_RegisterEncoder(&pstEncode)) {
                log_info( "AK_MPI_AENC_RegisterEncoder failed");
                return AK_FAILED;
            }
            break;
        case AUDIO_FORMAT_ALAW://don't need regist, just open adec
            log_info("set format to AUDIO_FORMAT_ALAW");
            pstEncode.enType = AK_AUDIO_TYPE_PCM_ALAW;
            stAencParam.enType = AK_AUDIO_TYPE_PCM_ALAW;
            break;

        case AUDIO_FORMAT_ULAW://don't need regist, just open adec
            log_info("set format to AUDIO_FORMAT_ULAW");

            pstEncode.enType = AK_AUDIO_TYPE_PCM_ULAW;
            stAencParam.enType = AK_AUDIO_TYPE_PCM_ULAW;
            break;

        case AUDIO_FORMAT_PCM://don't need regist, just open adec
            log_info("set format to AUDIO_FORMAT_PCM");

            pstEncode.enType = AK_AUDIO_TYPE_PCM;
            stAencParam.enType = AK_AUDIO_TYPE_PCM;
            break;
        default:
            break;
    }

    log_info("hal_audio_codec_in_open: Opening AENC with type=%d (format=0x%x), sample_rate=%d",
             stAencParam.enType, in->format, stAencParam.enSampleRate);
    
    if(AK_MPI_AENC_Open(s32AencUserId, &stAencParam)) {
        log_error("AK_MPI_AENC_Open failed: codec_type=%d, format=0x%x", stAencParam.enType, in->format);
        my_data->in_codec_id = -1;  // Mark as not opened
        return AK_FAILED;
    }

    my_data->in_codec_id = s32AencUserId;
    log_info("AENC opened successfully: codec_id=%d, codec_type=%d (format=0x%x)", 
             my_data->in_codec_id, stAencParam.enType, in->format);
    my_data->in_codec_buffer_release_flag = false;

    /* Framework matches streams by HAL format on this buffer; input_config is often PCM/DEFAULT. */
    memset(&in->enc_out_config, 0, sizeof(in->enc_out_config));
    in->enc_out_config.format = in->format;
    /* pcm_config may not be filled yet when codec opens before memcpy in adev_open_input_stream */
    in->enc_out_config.sample_rate = in->pcm_config.sample_rate ? in->pcm_config.sample_rate : in->sample_rate;
    in->enc_out_config.channel_mask = in->pcm_config.channel_mask ? in->pcm_config.channel_mask : in->channel_mask;

//...
//    AK_BASE_ThreadCreate(&in_codec_thread, audio_codec_in_thread, my_data, ANYKA_THREAD_MIN_STACK_SIZE, 19);

    return true;
}




void *audio_codec_in_thread(void *arg)
{
    AK_BASE_ThreadSetName("aenc");
    struct hal_audio_data *my_data = (struct hal_audio_data *)arg;
    struct audio_device *adev = my_data->adev;
    struct stream_in *in = adev->in;
    FRAME_S AIFRAME  = {0};
    AK_S32 ret = AK_FAILED;

    size_t bytes_read = 0;

    log_debug( "%s", __func__);

//    AK_BASE_SleepMs(30); 

    audio_capture_result_t result = {};

    int channel_count = in->channel_mask&AUDIO_CHANNEL_IN_MAIN_MASK >> AUDIO_CHANNEL_IN_MAIN_BIT;
    if(channel_count <= 0)
        channel_count = 1; 
    int sleep_time = in->pcm_config.sample_rate * channel_count /2;
    if(sleep_time > 10)
        sleep_time = 10;

    // Check if codec is valid before entering loop
    if (my_data->in_codec_id < 0) {
        log_error("audio_codec_in_thread: AENC not opened (in_codec_id=%d), exiting thread", my_data->in_codec_id);
        AK_BASE_ThreadExit();
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
                AK_MPI_AENC_ReleaseStream(my_data->in_codec_id, &my_data->aenc_frame);
            }
            my_data->in_codec_buffer_release_flag = false;
        }

        memset(&my_data->aenc_frame, 0x00, sizeof(AUDIO_STREAM_S));

        // Double-check codec_id is still valid before calling AENC_GetStream
        if (my_data->in_codec_id < 0) {
            log_warn("audio_codec_in_thread: AENC closed before GetStream (in_codec_id=%d), exiting", my_data->in_codec_id);
            break;
        }

        ret = AK_MPI_AENC_GetStream(my_data->in_codec_id,&my_data->aenc_frame, 0);
        if(!ret)
        {
            my_data->in_codec_buffer_release_flag = true;
            result.frame_number = my_data->aenc_frame.u64SeqNo;
            result.timestamp =  my_data->aenc_frame.u64Ts;
            in->output_codec_buffer.config = &in->enc_out_config;
            in->output_codec_buffer.data = my_data->aenc_frame.pu8Data;
            in->output_codec_buffer.data_size = my_data->aenc_frame.u32Len;
            
            // Debug: Log encoded data size and format to verify encoding is working
            log_debug("audio_codec_in_thread: AENC_GetStream: codec_id=%d, format=0x%x, encoded_size=%d, seq=%llu",
                     my_data->in_codec_id, in->format, my_data->aenc_frame.u32Len, my_data->aenc_frame.u64SeqNo);

            result.output_buffer = &in->output_codec_buffer;
 //               result.input_buffer = &in->input_buffer;

            adev->callback_ops->process_capture_result(adev->callback_ops, &result);
        }
        else
        {
            if (ERROR_AENC_NO_DATA == ret) {
                AK_BASE_SleepMs(sleep_time);
                continue;
            }
            else 
            {
                log_error("AK_MPI_AENC_GetStream error ret = %d \n",ret);
                break;
            }
        }
    }
    
    // Send end of frame only if codec is still valid
    if (my_data->in_codec_id >= 0) {
        AK_MPI_AENC_SendEndOfFrame(my_data->in_codec_id);
    } else {
        log_warn("audio_codec_in_thread: Skipping AENC_SendEndOfFrame, codec already closed (in_codec_id=%d)", my_data->in_codec_id);
    }
    
    AK_BASE_ThreadExit();

    return NULL;
}



size_t hal_audio_in_codec_enable(void *hal_audio)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;

    my_data->encoding_flag = true;

    AK_BASE_ThreadCreate(&in_codec_thread, audio_codec_in_thread, my_data, ANYKA_THREAD_MIN_STACK_SIZE, 19);
    return 0;
}

size_t hal_audio_in_codec_disable(void *hal_audio)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;

    my_data->encoding_flag = false;

    return 0;
}


size_t hal_audio_in_codec_read(void *hal_audio, char **buffer, uint32_t *frame_number, uint64_t * timestamp, size_t bytes)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    struct audio_device *adev = my_data->adev;
    struct stream_in *in = adev->in;
    int ret;
    size_t bytes_read = 0;

    // Check if audio input device is still valid before any operations
    // in_device_id should be in valid range (typically 0-3), 65536 indicates uninitialized/invalid
    if (my_data->in_device_id < 0 || my_data->in_device_id > 10) {
        log_warn("hal_audio_in_codec_read: Invalid in_device_id=%d, audio input may be closed", my_data->in_device_id);
        return 0;
    }

    // Check if codec is valid before sending frame
    if (my_data->in_codec_id < 0) {
        log_debug("hal_audio_in_codec_read: AENC not opened (in_codec_id=%d), releasing frame", my_data->in_codec_id);
        // Only release frame if device is still valid
        if (my_data->in_device_id >= 0 && my_data->in_device_id <= 10) {
            AK_MPI_AI_ReleaseFrame(my_data->in_device_id, my_data->in_channel_main_id, &my_data->ai_frame, 0);
        }
        return 0;
    }

    /*send frame to encoder*/
    int s32Ret = AK_MPI_AENC_SendFrame(my_data->in_codec_id, &my_data->ai_frame, 2000);
    if(s32Ret) {
        // Only release frame if device is still valid
        if (my_data->in_device_id >= 0 && my_data->in_device_id <= 10) {
            AK_MPI_AI_ReleaseFrame(my_data->in_device_id, my_data->in_channel_main_id, &my_data->ai_frame, 0);
        } else {
            log_warn("hal_audio_in_codec_read: Skipping AI_ReleaseFrame, invalid in_device_id=%d", my_data->in_device_id);
        }
        log_error("AK_MPI_AENC_SendFrame error: ret=%d, codec_id=%d", s32Ret, my_data->in_codec_id);
    }

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

    // First, stop encoding thread by setting encoding_flag to false
    // This will cause audio_codec_in_thread to exit its loop gracefully
    my_data->encoding_flag = false;
    
    // Wait for encoding thread to exit using ThreadJoin
    // This ensures the thread has fully exited before we close the codec
    // Note: in_codec_thread is a global variable, so we can join it here
    AK_BASE_ThreadJoin(in_codec_thread);  // Wait for thread to exit
    
    // Release any pending stream buffer (only if codec is still valid)
    if(my_data->in_codec_buffer_release_flag == true && my_data->in_codec_id >= 0)
    {
        AK_MPI_AENC_ReleaseStream(my_data->in_codec_id, &my_data->aenc_frame);
        my_data->in_codec_buffer_release_flag = false;
    }

    // Save codec_id before marking as closed (for error logging)
    int codec_id = my_data->in_codec_id;
    
    // Mark codec as closed AFTER thread has exited (thread is no longer using it)
    my_data->in_codec_id = -1;
    
    int ret = AK_MPI_AENC_Close(codec_id);
    if (ret != 0) {
        log_warn("AK_MPI_AENC_Close failed: ret=%d, codec_id=%d", ret, codec_id);
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

    ADEC_PARAM_S stAdecParam = {0};

    my_data->out_param.enSampleBits = AK_AUDIO_SMPLE_BIT_16;    // sample bits only support 16 bit

    audio_channel_mask_t main_channel = out->channel_mask & AUDIO_CHANNEL_OUT_MAIN_MASK;
    if(main_channel == AUDIO_CHANNEL_OUT_MONO)
    {
        my_data->out_param.enSoundMode = AUDIO_SOUND_MODE_MONO;
        my_data->out_param.u32ChnCnt = 1;
        my_data->out_param.u32SrcCnt = 1;
    }
    else if(main_channel == AUDIO_CHANNEL_OUT_STEREO)
    {
        my_data->out_param.enSoundMode = AUDIO_SOUND_MODE_STEREO;

        my_data->out_param.u32ChnCnt = 1; //set to 1
        my_data->out_param.u32SrcCnt = 1;
    }
    else
    {
        log_error("audio channel not support, output only support MONO and STEREO");
       return false;
    }

    stAdecParam.enSampleBits = AK_AUDIO_SMPLE_BIT_16;
    stAdecParam.enSampleRate = out->sample_rate;

    if(main_channel == AUDIO_CHANNEL_OUT_MONO)
    {
        stAdecParam.enSoundMode = AUDIO_SOUND_MODE_MONO;
    }
    else if(main_channel == AUDIO_CHANNEL_OUT_STEREO)
    {
        stAdecParam.enSoundMode = AUDIO_SOUND_MODE_STEREO;
    }
    else
    {
        log_error("audio channel not support, output only support MONO and STEREO");
       return false;
    }

    log_debug("stAdecParam.enSoundMode:%d",stAdecParam.enSoundMode);

    ADEC_DECODE_S stDecCaBck = {0};

    switch (out->format) {
        case AUDIO_FORMAT_MP3:
            // register mp3 decoder
            log_info("AUDIO_FORMAT_MP3");
            stAdecParam.enType = AK_AUDIO_TYPE_MP3;
            stDecCaBck.enType = AK_AUDIO_TYPE_MP3;
            stDecCaBck.pfnDecOpen = ADEC_OpenMp3;

            if (AK_MPI_ADEC_RegisterDecoder(&stDecCaBck)) {
                log_error(  "MP3 AK_MPI_ADEC_RegisterDecoder failed");
                return AK_FAILED;
            }
            break;
        case AUDIO_FORMAT_AAC:
            // register aac decoder
            log_info("AK_AUDIO_TYPE_AAC");
            stAdecParam.enType = AK_AUDIO_TYPE_AAC;
            stDecCaBck.enType = AK_AUDIO_TYPE_AAC;
            stAdecParam.unAdecAttr.stAacParam.u16AacStreamFormat = 0;
            stDecCaBck.pfnDecOpen = ADEC_OpenAac;
            if (AK_MPI_ADEC_RegisterDecoder(&stDecCaBck)) {
                log_error(  "AAC AK_MPI_ADEC_RegisterDecoder failed");
                return AK_FAILED;
            }
            break;
        case AUDIO_FORMAT_AMR:
            // register amr decoder
            log_info("AK_AUDIO_TYPE_AMR");
            stAdecParam.enType = AK_AUDIO_TYPE_AMR;
            stDecCaBck.enType = AK_AUDIO_TYPE_AMR;
            stDecCaBck.pfnDecOpen = ADEC_OpenAmr;
            if (AK_MPI_ADEC_RegisterDecoder(&stDecCaBck)) {
                log_error(  "AMR AK_MPI_ADEC_RegisterDecoder failed");
                return AK_FAILED;
            }
            break;
        case AUDIO_FORMAT_ALAW://don't need regist, just open  adec
            log_info("AUDIO_FORMAT_ALAW");
            stAdecParam.enType = AK_AUDIO_TYPE_PCM_ALAW;
            stDecCaBck.enType = AK_AUDIO_TYPE_PCM_ALAW;
            break;

        case AUDIO_FORMAT_ULAW://don't need regist, just open  adec
            log_error("AUDIO_FORMAT_ULAW");
            stAdecParam.enType = AK_AUDIO_TYPE_PCM_ULAW;
            stDecCaBck.enType = AK_AUDIO_TYPE_PCM_ULAW;
            break;

        case AUDIO_FORMAT_PCM://don't need regist, just open  adec
            log_info("AUDIO_FORMAT_PCM");
            stAdecParam.enType = AK_AUDIO_TYPE_PCM;
            stDecCaBck.enType = AK_AUDIO_TYPE_PCM;
            break;
        default:
            break;
    }

    /* open adec*/
    if (AK_MPI_ADEC_Open(s32AdecUserId, &stAdecParam)) {
        log_error(  "AK_MPI_ADEC_Open failed");
        my_data->out_codec_id = -1;  // Mark as not opened
        return AK_FAILED;
    }

    my_data->out_codec_id = s32AdecUserId;
    log_debug("ADEC opened: codec_id=%d", my_data->out_codec_id);


    AK_BASE_ThreadCreate(&out_codec_thread, audio_codec_out_thread, my_data, ANYKA_THREAD_MIN_STACK_SIZE, 11);

    return true;
}


int hal_audio_codec_out_close( void *hal_audio)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;

    // Check if codec was actually opened (out_codec_id >= 0 means it was opened)
    // In recorder mode, codec is never opened, so out_codec_id remains -1 (initial value)
    if (my_data->out_codec_id < 0) {
        return 0;  // Not an error - codec was never opened
    }

    /* Stop the ADEC->AO feeder before tearing anything down. Leaving it running
     * let it outlive this stream: it kept blocking in ADEC_GetFrame, and when it
     * finally timed out it flushed AO — which by then belonged to the *next*
     * playback. That is why switching tracks went silent or hiccuped. */
    my_data->playing_flag = false;

    // Send end of stream before closing codec
    int send_ret = AK_MPI_ADEC_SendEndofStream(my_data->out_codec_id);
    if (send_ret != 0) {
        log_warn("ADEC_SendEndofStream failed: ret=%d, codec_id=%d", send_ret, my_data->out_codec_id);
    }

    /* Join while the codec is still open: the feeder dereferences out_codec_id
     * during its drain tail. Bounded by the 2000 ms GetFrame timeout. */
    AK_BASE_ThreadJoin(out_codec_thread);

    int ret = AK_MPI_ADEC_Close(my_data->out_codec_id);
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

    AK_BASE_ThreadSetName("[adec play]attachment");
    AK_BASE_SleepMs(1000); //?
    DEV_BUF_STATUS_S stBufStatus;
    FRAME_S stPcmFrame = {0};
    AK_S32 play_len = 0;
    AK_S32 ret = 0;
    while(my_data->playing_flag) {
        // get frame out block mode

        ret = AK_MPI_ADEC_GetFrame(my_data->out_codec_id, &stPcmFrame,2000);
        if(ret) {
            /* Timeout/no-data during shutdown is expected; avoid error-level noise on normal exit. */
            log_debug("ADEC get frame end ret=%d, exiting output thread", ret);
            break;
        }

        if (stPcmFrame.u32Len == 0 || stPcmFrame.pu8Data == NULL) {
            continue;
        }

        /* Close may start after GetFrame returns; guard before sending to AO/VQE. */
        //if (!my_data->playing_flag || my_data->out_device_id < 0 || my_data->out_channel_id < 0) {
        //    AK_MPI_ADEC_ReleaseFrame(my_data->out_codec_id, &stPcmFrame);
        //    break;
        //}

        // send frame out block mode
        if (AK_MPI_AO_SendFrame(my_data->out_device_id,  my_data->out_channel_id, stPcmFrame.pu8Data, stPcmFrame.u32Len, &play_len, 2000)) {
            // Release frame using codec_id (not device_id) - ADEC_ReleaseFrame requires codec user id
            AK_MPI_ADEC_ReleaseFrame(my_data->out_codec_id, &stPcmFrame);
            break;
        }


        // release frame
        AK_MPI_ADEC_ReleaseFrame(my_data->out_codec_id, &stPcmFrame);
    }

    /*get remain data in vqe*/
    if (AK_MPI_AO_GetBufStatus(my_data->out_device_id, my_data->out_codec_id, &stBufStatus)) {
        log_error("AK_MPI_AO_GetBufStatus failed ");
    }
    int AoRemainLen = stBufStatus.s32VqeBufRemainLen + stBufStatus.s32DevBufRemainLen;

    audio_channel_mask_t main_channel = out->channel_mask & AUDIO_CHANNEL_OUT_MAIN_MASK;
    int s32SoundMode;
    if(main_channel == AUDIO_CHANNEL_IN_MONO)
    {
        s32SoundMode = AUDIO_SOUND_MODE_MONO;
    }
    else if(main_channel == AUDIO_CHANNEL_IN_STEREO)
    {
        s32SoundMode = AUDIO_SOUND_MODE_STEREO;
    }

    /*get flush time to play remain data*/
    int FlushTime =  AudioLenToInterval(out->sample_rate, AK_AUDIO_SMPLE_BIT_16, s32SoundMode, AoRemainLen);
    if (AK_MPI_AO_Flush(my_data->out_device_id, my_data->out_codec_id)) {
        log_error("AK_MPI_AO_Flush failed ");
    }
    /*play remain data*/
    AK_BASE_SleepMs(FlushTime);

    log_info( "%s exit", __func__);
    AK_BASE_ThreadExit();

    return NULL;
}
