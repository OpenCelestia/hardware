// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/audio.
 *
 * HAL audio implementation for AnyCloud39AV200 platform.
 * - Implements platform-specific audio HAL operations.
 * - Provides audio I/O, effects, and codec management.
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <getopt.h>
#include <errno.h>
#include "../../audio_hw.h"
#include "audio.h"
#include "hal_audio.h"

#include "ak_ao.h"
#include "ak_ai.h"
#include "hal_audio_config.h"
#include "ak_aenc.h"
#include "ak_adec.h"

#include "../../hal_audio_api.h"

#include "log.h"
#define LOG_TAG "HAL AUDIO"

/* Device-tree base (on target Linux). Node name may be "adc_dac" or "adc_dac@..." */
#define DT_BASE                         "/proc/device-tree"

static int read_dt_u32_be(const char *path, uint32_t *out)
{
    if (!path || !out) return -1;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    uint8_t buf[4] = {0};
    ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n != (ssize_t)sizeof(buf)) return -1;
    *out = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) | ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
    return 0;
}

static int popcount_u32(uint32_t v)
{
    int c = 0;
    while (v) {
        v &= (v - 1);
        c++;
    }
    return c;
}

/* Scan dir for a subdir whose name is "adc_dac" or "adc_dac@...". */
static int scan_dir_for_adc_dac(const char *base, char *out_path, size_t out_size)
{
    DIR *dir = opendir(base);
    if (!dir) return -1;
    struct dirent *ent;
    int found = 0;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (strncmp(ent->d_name, "adc_dac", 7) != 0) continue;
        if (ent->d_name[7] != '\0' && ent->d_name[7] != '@') continue;
        if (strlen(base) + 1 + strlen(ent->d_name) >= out_size) { found = -1; break; }
        snprintf(out_path, out_size, "%s/%s", base, ent->d_name);
        found = 1;
        break;
    }
    closedir(dir);
    return found == 1 ? 0 : -1;
}

/* Find adc_dac node: /proc/device-tree/adc_dac* or /proc/device-tree/soc/adc_dac*. */
static int find_adc_dac_path(char *out_path, size_t out_size)
{
    if (scan_dir_for_adc_dac(DT_BASE, out_path, out_size) == 0)
        return 0;
    {
        char soc_path[64];
        snprintf(soc_path, sizeof(soc_path), "%s/soc", DT_BASE);
        if (scan_dir_for_adc_dac(soc_path, out_path, out_size) == 0)
            return 0;
    }
    return -1;
}

/* Auto-detect DMIC vs AMIC and mic count from DTB. */
static void hal_audio_detect_mic_hw(audio_devices_t requested_dev,
                                   audio_devices_t *detected_dev,
                                   int *detected_mic_num)
{
    audio_devices_t dev = AUDIO_DEVICE_IN_AMIC;
    int mic_num = 1;

    const hal_audio_config_t *cfg = hal_audio_get_config();

    if (cfg) {
        if (cfg->mic_type == 1) dev = AUDIO_DEVICE_IN_AMIC;
        else if (cfg->mic_type == 2) dev = AUDIO_DEVICE_IN_DMIC;
        if (cfg->mic_num > 0) mic_num = cfg->mic_num;
    }

    /* Auto-detect from DTB when type or num not forced by ini */
    if ((!cfg || cfg->mic_type == 0) || (!cfg || cfg->mic_num == 0)) {
        char adc_dac_path[128];
        if (find_adc_dac_path(adc_dac_path, sizeof(adc_dac_path)) == 0) {
            char dev3_path[160];
            char mic_info_path[160];
            snprintf(dev3_path, sizeof(dev3_path), "%s/dev3", adc_dac_path);
            snprintf(mic_info_path, sizeof(mic_info_path), "%s/mic-info", adc_dac_path);
            struct stat st;
            int has_dev3 = (stat(dev3_path, &st) == 0 && S_ISDIR(st.st_mode));
            if (!cfg || cfg->mic_type == 0)
                dev = has_dev3 ? AUDIO_DEVICE_IN_DMIC : AUDIO_DEVICE_IN_AMIC;
            if (!cfg || cfg->mic_num == 0) {
                uint32_t mic_info = 0;
                if (read_dt_u32_be(mic_info_path, &mic_info) == 0) {
                    int cnt = popcount_u32(mic_info);
                    if (cnt > 0) mic_num = cnt;
                }
            }
            log_info("[hal_audio_detect] DTB adc_dac: %s, dev3=%d => %s, mic_num=%d",
                     adc_dac_path, has_dev3,
                     dev == AUDIO_DEVICE_IN_DMIC ? "DMIC" : "AMIC", mic_num);
        } else {
            log_info("[hal_audio_detect] No adc_dac node in DTB, default AMIC mic_num=1");
        }
    }

    if (requested_dev == AUDIO_DEVICE_IN_BUS0 ||
        requested_dev == AUDIO_DEVICE_IN_BUS1 ||
        requested_dev == AUDIO_DEVICE_IN_USB_DEVICE) {
        dev = requested_dev;
    }

    if (detected_dev) *detected_dev = dev;
    if (detected_mic_num) *detected_mic_num = mic_num;
}


/**
 * @brief Millisecond sleep wrapper.
 * @param ms Milliseconds to sleep.
 */
void msleep(const int ms)
{
    AK_BASE_SleepMs(ms);
}


static bool is_supported_format(audio_format_t format)
{
    audio_format_t main_format = format & AUDIO_FORMAT_MAIN_MASK;
    audio_format_t sub_format = format & AUDIO_FORMAT_SUB_MASK;

    if(main_format == AUDIO_FORMAT_PCM)
    {
        if((sub_format == AUDIO_FORMAT_PCM_SUB_8_BIT) || (sub_format == AUDIO_FORMAT_PCM_SUB_32_BIT))
        {
            log_error("only support 16 bit format");
            return false;
        }
        else
            return true;
    }
    else if (
        main_format == AUDIO_FORMAT_MP3 ||
        main_format == AUDIO_FORMAT_AAC ||
        main_format == AUDIO_FORMAT_WMA ||
        main_format == AUDIO_FORMAT_AMR ||
        main_format == AUDIO_FORMAT_ALAW ||
        main_format == AUDIO_FORMAT_ULAW )
           return true;

    return false;
}

/**
 * @brief Check if audio format is supported.
 * @param format Audio format to check.
 * @return true if supported, false otherwise.
 */
static bool is_supported_format(audio_format_t format);

/**
 * @brief Initialize HAL audio context.
 * @param adev Audio device pointer.
 * @return HAL audio data pointer on success, NULL on failure.
 */
void *hal_audio_init(struct audio_device *adev)
{
    struct hal_audio_data *my_data;

    hal_audio_config_t *cfg = hal_audio_get_config();
    AK_MPI_AI_SetFilterPrintLevel(cfg->ai_print_level);

    log_info("MPI AI version: %s", AK_MPI_AI_GetVersion());
    log_info("MPI AI Filter version:  %s", AK_MPI_AI_GetFilterVersion());
    log_info("MPI aenc version:  %s", AK_MPI_AENC_GetVersion());
    log_info("MPI AO version: %s", AK_MPI_AO_GetVersion());
    log_info("MPI AO Filter version: %s   ", AK_MPI_AO_GetFilterVersion());
    log_info("MPI adec version:  %s", AK_MPI_ADEC_GetVersion());


    my_data = calloc(1, sizeof(struct hal_audio_data));
    if (!my_data)
    {
        return 0;
    }

    my_data->adev = adev;
    my_data->out_mode = AUDIO_DEVICE_NONE;
    my_data->in_mode = AUDIO_DEVICE_NONE;
    my_data->playing_flag = false;
    my_data->recording_flag = false;
    
    // Initialize codec IDs to -1 to indicate "not opened"
    // 0 might be a valid codec id, so we use -1 to distinguish "not opened" from "opened with id 0"
    my_data->in_codec_id = -1;
    my_data->out_codec_id = -1;
    
    // Initialize device IDs to -1 to indicate "not opened"
    // This prevents invalid device ID errors when functions are called after close
    my_data->in_device_id = -1;
    my_data->in_channel_main_id = -1;
    my_data->out_device_id = -1;
    my_data->out_channel_id = -1;

    my_data->cfg = cfg;

    return  (void *)my_data;
}

void hal_audio_deinit(void *my_data)
{
    free(my_data);
}



int hal_audio_input_open(void *hal_audio)
{
    int ret = AK_FAILED;
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    struct audio_device *adev = my_data->adev;
    struct stream_in *in = adev->in;

    log_info("[hal_audio_input_open] Starting: format=0x%x, channel_mask=0x%x, devices=0x%x, sample_rate=%d",
             in->format, in->channel_mask, in->devices, in->sample_rate);

    /* Detect AMIC/DMIC and mic count from DTB/ini so upper layers don't care. */
    audio_devices_t detected_dev = AUDIO_DEVICE_IN_AMIC;
    int detected_mic_num = 1;
    hal_audio_detect_mic_hw(in->devices, &detected_dev, &detected_mic_num);
    if (detected_mic_num < 1) detected_mic_num = 1;

    // Check if device is already enabled and channel is already enabled
    // If so, disable them first to avoid conflicts
    if (my_data->recording_flag) {
        log_warn("[hal_audio_input_open] Device already recording, disabling channel first");
        AK_MPI_AI_DisableChn(my_data->in_device_id, my_data->in_channel_main_id);
        AK_MPI_AI_Disable(my_data->in_device_id);
        my_data->recording_flag = false;
    }

    memset(&my_data->in_param, 0, sizeof(my_data->in_param));

    my_data->pcm_buffer_release_flag = false;
    my_data->in_codec_buffer_release_flag = false;

    my_data->in_param.enSampleRate = in->sample_rate; // set sample rate

    if(!is_supported_format(in->format))
    {
        log_error("[hal_audio_input_open] Audio format not support, only support PCM、AAC、MP3、WMA、ALAW、uLAW");
        return false;
    }
    my_data->in_param.enSampleBits = AK_AUDIO_SMPLE_BIT_16;    // sample bits only support 16 bit

    log_info("[hal_audio_input_open] Step 1: Setting channel parameters");
    audio_channel_mask_t main_channel = in->channel_mask & AUDIO_CHANNEL_IN_MAIN_MASK;
    
    /* Calculate required channel count based on potential audio effects.
     * Channel IDs (hal_audio.h): MAIN=0, VAD=1, NPU_CRY=2, LSSL=3, CPU_CRY=4
     * u32ChnCnt must be >= (max_channel_id + 1) to support all channels.
     * 
     * Since we don't know which effects will be enabled at this point,
     * we set u32ChnCnt to 5 to support all possible channels (0-4).
     * This allows effects to be enabled later via add_audio_effect without errors.
     * 
     * Note: Setting u32ChnCnt to 5 doesn't actually enable all channels,
     * it just reserves space for them. Channels are only enabled when
     * add_audio_effect is called for specific effects.
     */
    int required_chn_cnt = AUDIO_IN_CHANNEL_ID_USED_MAX;  // 5 channels (0-4)
    
    /* Clamp requested channels to hardware mic count. */
    if (main_channel == AUDIO_CHANNEL_IN_STEREO && detected_mic_num < 2) {
        log_warn("[hal_audio_input_open] Requested stereo but hardware mic_num=%d, fallback to mono", detected_mic_num);
        main_channel = AUDIO_CHANNEL_IN_MONO;
        /* keep in->channel_mask consistent for upper layers and effect logic */
        in->channel_mask = (in->channel_mask & ~AUDIO_CHANNEL_IN_MAIN_MASK) | AUDIO_CHANNEL_IN_MONO;
    }

    if(main_channel == AUDIO_CHANNEL_IN_MONO)
    {
        my_data->in_param.enSoundMode = AUDIO_SOUND_MODE_MONO;
        my_data->in_param.u32SrcCnt = 1;
        my_data->in_param.u32ChnCnt = required_chn_cnt;

        log_info("[hal_audio_input_open] Step 1: Set to mono, chn_cnt=%d (effect AI ch: VAD=1, NPU_CRY=2, LSSL=3, CPU_CRY=4)",
                 my_data->in_param.u32ChnCnt);
    }
    else if(main_channel == AUDIO_CHANNEL_IN_STEREO)
    {
        my_data->in_param.enSoundMode = AUDIO_SOUND_MODE_STEREO;
        my_data->in_param.u32SrcCnt = 1;
        my_data->in_param.u32ChnCnt = required_chn_cnt;

        log_info("[hal_audio_input_open] Step 1: Set to stereo, chn_cnt=%d (effect AI ch: VAD=1, NPU_CRY=2, LSSL=3, CPU_CRY=4)",
                 my_data->in_param.u32ChnCnt);
    }
    else
    {
        log_error("[hal_audio_input_open] Audio channel not support, only support MONO and STEREO");
        return false;
    }
    my_data->mic_mute_state = false;
    my_data->in_channel_main_id = 0;

    log_info("[hal_audio_input_open] Step 2: Setting device ID");
    /* Override devices for AMIC/DMIC/NONE based on detection so effects can pick correct config sections. */
    if (in->devices == AUDIO_DEVICE_NONE ||
        in->devices == AUDIO_DEVICE_IN_AMIC ||
        in->devices == AUDIO_DEVICE_IN_DMIC) {
        in->devices = detected_dev;
    }

    log_info("[hal_audio_input_open] Detected mic hw: devices=0x%x, mic_num=%d", in->devices, detected_mic_num);

    switch (in->devices)
    {
        case AUDIO_DEVICE_IN_AMIC:
        case AUDIO_DEVICE_NONE:
            my_data->in_device_id = DEV_ADC;
            log_info("[hal_audio_input_open] Step 2: Set to DEV_ADC");
        break;

        case AUDIO_DEVICE_IN_DMIC:
            my_data->in_device_id = DEV_PDM;
            log_info("[hal_audio_input_open] Step 2: Set to DEV_PDM");
        break;

        case AUDIO_DEVICE_IN_BUS0:
            my_data->in_device_id = DEV_I2S0_AI;//IIS device
            log_info("[hal_audio_input_open] Step 2: Set to DEV_I2S0_AI");
        break;

        case AUDIO_DEVICE_IN_BUS1:
            my_data->in_device_id = DEV_I2S1_AI;//IIS device
            log_info("[hal_audio_input_open] Step 2: Set to DEV_I2S1_AI");
        break;

        case AUDIO_DEVICE_IN_USB_DEVICE:
            //UAC
            log_info("[hal_audio_input_open] Step 2: Set to USB device (not implemented)");
        break;

        default :
            my_data->in_device_id = DEV_ADC;
            log_info("[hal_audio_input_open] Step 2: Default to DEV_ADC");
    }

    log_info("[hal_audio_input_open] Step 3: Setting public attributes: device_id=%d, sample_rate=%d, sample_bits=%d, sound_mode=%d, src_cnt=%d, chn_cnt=%d",
             my_data->in_device_id, my_data->in_param.enSampleRate, my_data->in_param.enSampleBits,
             my_data->in_param.enSoundMode, my_data->in_param.u32SrcCnt, my_data->in_param.u32ChnCnt);
    if ( AK_MPI_AI_SetPubAttr(my_data->in_device_id, &my_data->in_param) )
    {
        log_error("[hal_audio_input_open] Step 3: AK_MPI_AI_SetPubAttr failed");
        return ret;
    }
    log_info("[hal_audio_input_open] Step 3: Public attributes set successfully");

    log_info("[hal_audio_input_open] Step 4: Enabling AI device");
    if (AK_MPI_AI_Enable(my_data->in_device_id))
    {
        log_error("[hal_audio_input_open] Step 4: AK_MPI_AI_Enable failed for device_id=%d", my_data->in_device_id);

        /* Some kernels/drivers reject multi-channel capture at open() time (EINVAL),
         * even if the extra channels are only intended for effect sub-channels.
         * Fallback to 1 channel so main record can still work. */
        if (my_data->in_param.u32ChnCnt > 1) {
            log_warn("[hal_audio_input_open] Step 4: Fallback retry with chn_cnt=1 (was %d)",
                     my_data->in_param.u32ChnCnt);

            my_data->in_param.u32ChnCnt = 1;
            log_info("[hal_audio_input_open] Step 4b: Re-setting public attributes for fallback: device_id=%d, sample_rate=%d, sample_bits=%d, sound_mode=%d, src_cnt=%d, chn_cnt=%d",
                     my_data->in_device_id, my_data->in_param.enSampleRate, my_data->in_param.enSampleBits,
                     my_data->in_param.enSoundMode, my_data->in_param.u32SrcCnt, my_data->in_param.u32ChnCnt);
            if (AK_MPI_AI_SetPubAttr(my_data->in_device_id, &my_data->in_param)) {
                log_error("[hal_audio_input_open] Step 4b: AK_MPI_AI_SetPubAttr failed on fallback");
                return AK_FAILED;
            }
            if (AK_MPI_AI_Enable(my_data->in_device_id)) {
                log_error("[hal_audio_input_open] Step 4b: AK_MPI_AI_Enable failed on fallback for device_id=%d", my_data->in_device_id);
                return AK_FAILED;
            }
            log_info("[hal_audio_input_open] Step 4b: AI device enabled successfully with fallback chn_cnt=1");
        } else {
            return AK_FAILED;
        }
    }
    log_info("[hal_audio_input_open] Step 4: AI device enabled successfully");

    log_info("[hal_audio_input_open] Step 5: Getting frame length");
    AK_S32  s32FrameLen = 0;
    AK_MPI_AI_GetFrmLen(my_data->in_device_id, my_data->in_channel_main_id, &s32FrameLen);
    log_info("[hal_audio_input_open] Step 5: Frame length=%d", s32FrameLen);
    in->in_frame_interval = AudioLenToInterval(my_data->in_param.enSampleRate, my_data->in_param.enSampleBits,
                                          my_data->in_param.enSoundMode, s32FrameLen);

    log_info("[hal_audio_input_open] Step 6: Loading audio effect configuration");
    ret = hal_audio_effect_in_loadconf(adev->hal_audio);
    if (ret != 0) {
        log_warn("[hal_audio_input_open] Step 6: hal_audio_effect_in_loadconf failed: %d, continuing anyway", ret);
    } else {
        log_info("[hal_audio_input_open] Step 6: Audio effect configuration loaded");
    }

    hal_audio_config_t *cfg = hal_audio_get_config();

    if(in->devices == AUDIO_DEVICE_IN_AMIC)
    {
        log_info("[hal_audio_input_open] Step 7: Setting gain for AMIC: gain=%d", cfg->ai_gain);
        if(AK_MPI_AI_SetGain(my_data->in_device_id, my_data->in_channel_main_id, cfg->ai_gain))
        {
            log_error("[hal_audio_input_open] Step 7: AK_MPI_AI_SetGain failed");
            return AK_FAILED;
        }
        log_info("[hal_audio_input_open] Step 7: Gain set successfully");
    }

    log_info("[hal_audio_input_open] Step 8: Enabling AI channel: device_id=%d, channel_id=%d", 
             my_data->in_device_id, my_data->in_channel_main_id);
    ret = AK_MPI_AI_EnableChn(my_data->in_device_id, my_data->in_channel_main_id);
    if(ret)
    {
        log_error("[hal_audio_input_open] Step 8: AK_MPI_AI_EnableChn failed: device_id=%d, channel_id=%d, ret=%d", 
                  my_data->in_device_id, my_data->in_channel_main_id, ret);
        log_error("[hal_audio_input_open] Step 8: Parameters: u32ChnCnt=%d, u32SrcCnt=%d, sample_rate=%d, sound_mode=%d",
                  my_data->in_param.u32ChnCnt, my_data->in_param.u32SrcCnt,
                  my_data->in_param.enSampleRate, my_data->in_param.enSoundMode);
        // Try to disable device before returning
        AK_MPI_AI_Disable(my_data->in_device_id);
        return AK_FAILED;
    }
    log_info("[hal_audio_input_open] Step 8: AI channel enabled successfully");

    /* Digital gain query is valid only after EnableChn. */
    (void)AK_MPI_AI_GetDigitalGain(my_data->in_device_id, my_data->in_channel_main_id,
                                   &my_data->in_volume);

    my_data->recording_flag = true;
    log_info("audio input opened: device=%d, channel=%d, rate=%d",
             my_data->in_device_id, my_data->in_channel_main_id, in->sample_rate);

    return AK_SUCCESS;
}

int hal_audio_on_input_stream_opened(void *hal_audio)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    struct audio_device *adev;
    struct stream_in *in;

    if (!my_data)
        return 0;

    adev = my_data->adev;
    if (!adev)
        return 0;

    in = adev->in;
    if (in == NULL || adev->callback_ops == NULL)
        return 0;

    if (!my_data->recording_flag || my_data->in_device_id < 0)
        return 0;

    /* Effect path starts capture in check_effect_init after add_audio_effect. */
    if (my_data->in_effect != AUDIO_EFFECT_NONE)
        return 0;

    return 1;
}

int hal_audio_input_stop_capture(void *hal_audio)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;

    if (!my_data || !my_data->recording_flag)
        return 0;
    if (my_data->in_device_id < 0 || my_data->in_device_id > 10)
        return 0;

    if (my_data->pcm_buffer_release_flag) {
        AK_MPI_AI_ReleaseFrame(my_data->in_device_id, my_data->in_channel_main_id,
                              &my_data->ai_frame, 0);
        my_data->pcm_buffer_release_flag = false;
    }
    if (AK_MPI_AI_DisableChn(my_data->in_device_id, my_data->in_channel_main_id))
        log_warn("hal_audio_input_stop_capture: DisableChn failed");
    my_data->recording_flag = false;
    return 0;
}

int hal_audio_input_close(void *hal_audio)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    struct audio_device *adev = my_data->adev;
    struct stream_in *in = adev->in;

    /* Effects are removed by audio_hw.c after hal_audio_input_close().
     * Keep hal layer close focused on AI main channel/device teardown. */

    // Check if device ID is valid before attempting to release frame or disable
    if (my_data->in_device_id >= 0 && my_data->in_device_id <= 10) {
        hal_audio_input_stop_capture(hal_audio);

        if (AK_MPI_AI_Disable(my_data->in_device_id))
            log_error("AK_MPI_AI_Disable failed.");
    } else {
        log_warn("hal_audio_input_close: Invalid in_device_id=%d, skipping AI operations", my_data->in_device_id);
    }

    // Mark device as closed by setting invalid ID
    my_data->in_device_id = -1;
    my_data->in_channel_main_id = -1;

    /* Next open must run AK_MPI_AI_LoadConf again: the SDK requires it when mic_num>1,
     * but hal_audio_effect_in_loadconf skips LoadConf when adev->mode == in_mode.
     * After close, adev->mode often still matches the cached in_mode — reset so reopen works. */
    my_data->in_mode = AUDIO_MODE_NONE;

    log_info("audio input closed");
    return 0;
}


int hal_audio_output_open(void *hal_audio)
{
    int ret = AK_FAILED;
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    struct audio_device *adev = my_data->adev;
    struct stream_out *out = adev->out;

    memset(&my_data->out_param, 0, sizeof(my_data->out_param));

    my_data->out_param.enSampleRate = out->sample_rate; // set sample rate

    log_debug("hal_audio_output_open: format=0x%08x, main_format=0x%08x", 
              out->format, out->format & AUDIO_FORMAT_MAIN_MASK);
    
    if(!is_supported_format(out->format))
    {
        log_error("audio format not support (format=0x%08x, main_format=0x%08x), only support PCM、AAC、MP3、WMA、ALAW、uLAW, please check",
                  out->format, out->format & AUDIO_FORMAT_MAIN_MASK);
        return false;
    }

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

    my_data->out_channel_id = 0;

    switch (out->devices)
    {
        case AUDIO_DEVICE_OUT_SPEAKER:
            my_data->out_device_id = DEV_DAC;
        break;

        case AUDIO_DEVICE_OUT_BUS:
            my_data->out_device_id = DEV_I2S0_AO;//will add IIS device later
        break;

        case AUDIO_DEVICE_OUT_USB_DEVICE:
            //will add IIS device later
        break;

//        case AUDIO_DEVICE_OUT_WIRED_HEADSET:
            //will add wired headset later
 //       break;

        default :
            my_data->out_device_id = DEV_DAC;
    }

    /*set pub attr*/
    if (AK_MPI_AO_SetPubAttr(my_data->out_device_id, &my_data->out_param)) {
        /* The driver refuses pub attr while the channel is still enabled
         * ("chn alreadly enabled, can't set!"). That happens whenever a previous
         * owner lost track of its stream (orphaned AO after a config reload or a
         * leaked audio-manager reference) instead of closing it. Reclaim the
         * channel and retry once, so one app's leak cannot silence the next one. */
        log_warn("AK_MPI_AO_SetPubAttr failed (device=%d), reclaiming channel and retrying",
                 my_data->out_device_id);
        (void)AK_MPI_AO_DisableChn(my_data->out_device_id, my_data->out_channel_id);
        (void)AK_MPI_AO_Disable(my_data->out_device_id);
        if (AK_MPI_AO_SetPubAttr(my_data->out_device_id, &my_data->out_param)) {
            log_error("AK_MPI_AO_SetPubAttr failed after reclaim: sample_bits=%d, sound_mode=%d, sample_rate=%d, chn_cnt=%d, device_id=%d",
                      my_data->out_param.enSampleBits, my_data->out_param.enSoundMode,
                      my_data->out_param.enSampleRate, my_data->out_param.u32ChnCnt,
                      my_data->out_device_id);
            return AK_FAILED;
        }
        log_info("AO channel reclaimed; pub attr applied");
    }
    /*ao enable*/
    if (AK_MPI_AO_Enable(my_data->out_device_id)) {
        log_error("AK_MPI_AO_Enable failed ");
        return AK_FAILED;
    }

    /* AV200 DAC analog gain is signed dB in [-24, 0]; 0 = loudest, -24 = quietest.
     * Some boards reject mid values (e.g. -20 → EPERM); fall back to 0 and continue.
     * Never abort AO open solely on SetGain — that leaves talk with no playback path. */
    {
        hal_audio_config_t *cfg_gain = hal_audio_get_config();
        int ao_gain = cfg_gain ? cfg_gain->ao_gain : 0;

        if (ao_gain > 0)
            ao_gain = 0;
        if (ao_gain < -24)
            ao_gain = -24;
        if (AK_MPI_AO_SetGain(my_data->out_device_id, my_data->out_channel_id, ao_gain)) {
            log_warn("AK_MPI_AO_SetGain failed (gain=%d), retry gain=0", ao_gain);
            ao_gain = 0;
            if (AK_MPI_AO_SetGain(my_data->out_device_id, my_data->out_channel_id, ao_gain)) {
                log_warn("AK_MPI_AO_SetGain(0) also failed; continue with driver default");
            } else {
                log_info("AO analog gain fallback set to 0");
            }
        } else {
            log_info("AO analog gain set to %d (range [-24,0])", ao_gain);
        }
        my_data->out_gain = ao_gain;
    }

    hal_audio_config_t *cfg = hal_audio_get_config();
    if (AK_MPI_AO_SetDrvBufPeriods(my_data->out_device_id, AUDIO_DRV_BUF_AO, cfg->ao_drv_buf_periods))
    {
        log_error("AK_MPO_AO_SetDrvBufPeriods failed");
        return AK_FAILED;
    }

    if (AK_MPI_AO_SetThreadPriority(my_data->out_device_id, AUDIO_THREAD_AO, cfg->ao_thread_priority))
    {
        log_error("AK_MPI_AO_SetThreadPriority failed");
        return AK_FAILED;
    }


    if (AK_MPI_AO_EnableChn(my_data->out_device_id, my_data->out_channel_id)) {
        log_error("AK_MPI_AO_EnableChn failed ");
        return AK_FAILED;
    }

    my_data->playing_flag = true;
    log_info("audio output opened: device=%d, channel=%d, rate=%d",
             my_data->out_device_id, my_data->out_channel_id, out->sample_rate);

    return AK_SUCCESS;
}


int hal_audio_output_close(void *hal_audio)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    struct audio_device *adev = my_data->adev;
    struct stream_out *out = adev->out;

    my_data->playing_flag = false;

    // Check if format is PCM - if so, codec was never opened, so don't call SendEndofStream
    // Only non-PCM formats (MP3, AAC, etc.) use codec, and codec is opened in hal_audio_codec_out_open
    if((out->format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_PCM)
    {
        // PCM format - codec was never opened, so don't call SendEndofStream
    }
    else
    {
        // Non-PCM format - codec was opened, flush the output
        AK_MPI_AO_Flush(my_data->out_device_id, my_data->out_channel_id);
        // Note: SendEndofStream should be called in hal_audio_codec_out_close, not here
        // because codec close happens after this function in adev_close_output_stream
    }

    if ( AK_MPI_AO_DisableChn(my_data->out_device_id, my_data->out_channel_id) )
    {
        log_error( "AK_MPI_AO_DisableChn failed.");
        AK_MPI_AI_Disable(my_data->out_device_id);
        return -1;
    }

    if ( AK_MPI_AO_Disable(my_data->out_device_id) )
    {
        log_error( "AK_MPI_AO_Disable failed.");
        return -1;
    }

    /* Mark output ids invalid so late writers fail fast. */
    my_data->out_device_id = -1;
    my_data->out_channel_id = -1;

    /* Next open must run AK_MPI_AO_LoadConf + SetMute(0) + SetDigitalGain again:
     * out_loadconf skips all three when adev->mode == out_mode. Without this
     * reset the second talk start inherited whatever mute/ASLC/vol_dB the
     * previous session left behind, and far-end audio came out silent.
     * in_mode is reset the same way in hal_audio_input_close(). */
    my_data->out_mode = AUDIO_MODE_NONE;

    log_info("audio output closed");
    return AK_SUCCESS;
}


static AK_VOID PrintPlayingDot(AK_VOID)
{
    static AK_U8 u8FirstFlag = 1;
    static TIME_VAL_S stCurTime;
    static TIME_VAL_S stPrintTime;

    if (u8FirstFlag) {
        u8FirstFlag = 0;
        AK_BASE_GetOstime(&stCurTime);
        AK_BASE_GetOstime(&stPrintTime);
    }

    /* Update timestamp every 10 seconds */
    AK_BASE_GetOstime(&stCurTime);
    if (AK_BASE_DiffMsTime(&stCurTime, &stPrintTime) >= 10000) {
        AK_BASE_GetOstime(&stPrintTime);
    }
}

int hal_audio_pcm_write(void *hal_audio, unsigned char *send_pcm_data,int send_pcm_len)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    struct audio_device *adev = my_data->adev;
    struct stream_out *out = adev->out;
    int play_pcm_len = 0;
    int ret;
    hal_audio_config_t *cfg = hal_audio_get_config();

    /* Exit early while output is closing/closed to avoid AO/VQE shutdown races. */
    if (!my_data->playing_flag || my_data->out_device_id < 0 || my_data->out_channel_id < 0) {
        return -1;
    }

    if((out->format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_PCM)
    {
        ret = AK_MPI_AO_SendFrame(my_data->out_device_id,  my_data->out_channel_id, 
                 send_pcm_data, send_pcm_len, &play_pcm_len, cfg->send_frame_timeout_ms);
    }
    else
    {
        if (my_data->out_codec_id < 0) {
            static unsigned s_adec_missing_log;
            if (s_adec_missing_log < 5) {
                s_adec_missing_log++;
                log_error("hal_audio_pcm_write: ADEC not open (format=0x%x codec_id=%d)",
                          out->format, my_data->out_codec_id);
            }
            return -1;
        }
        ret = AK_MPI_ADEC_SendStream(my_data->out_codec_id, send_pcm_data, send_pcm_len,
                                     cfg->send_frame_timeout_ms);
        if (ret) {
            if (ret == ERROR_ADEC_SEND_STREAM_NO_SPACE) {
                log_warn("send stream timeout");
            } else {
                log_error( "AK_MPI_ADEC_SendStream failed");
            }
        } else {
            play_pcm_len = send_pcm_len;
        }
        PrintPlayingDot();

    }

    if (ret) {
        static unsigned s_pcm_write_fail_log;
        if (s_pcm_write_fail_log < 8) {
            s_pcm_write_fail_log++;
            log_warn("hal_audio_pcm_write SendFrame failed: ret=%d bytes=%d playing=%d dev=%d ch=%d",
                     ret, send_pcm_len, (int)my_data->playing_flag,
                     my_data->out_device_id, my_data->out_channel_id);
        }
        return -1;
    }

    if ((out->format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_PCM && play_pcm_len <= 0) {
        static unsigned s_pcm_zero_play_log;
        if (s_pcm_zero_play_log < 8) {
            s_pcm_zero_play_log++;
            log_warn("hal_audio_pcm_write: AO accepted 0 bytes (send=%d dev=%d ch=%d)",
                     send_pcm_len, my_data->out_device_id, my_data->out_channel_id);
        }
        return -1;
    }

    {
        static unsigned s_pcm_write_ok_log;
        if (s_pcm_write_ok_log < 5) {
            s_pcm_write_ok_log++;
            log_info("hal_audio_pcm_write ok #%u: send=%d play=%d dev=%d ch=%d",
                     s_pcm_write_ok_log, send_pcm_len, play_pcm_len,
                     my_data->out_device_id, my_data->out_channel_id);
        }
    }

    return play_pcm_len;
}

size_t hal_audio_pcm_read(void *hal_audio, void **buffer,uint32_t *frame_number, uint64_t * timestamp,size_t bytes)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    struct audio_device *adev = my_data->adev;
    struct stream_in *in = adev->in;
    int ret;
    size_t bytes_read = 0;

    // Check if device ID is valid before any AI operations
    if (my_data->in_device_id < 0 || my_data->in_device_id > 10) {
        log_warn("hal_audio_pcm_read: Invalid in_device_id=%d, audio input may be closed", my_data->in_device_id);
        return 0;
    }

    if(my_data->pcm_buffer_release_flag == true)
    {
        AK_MPI_AI_ReleaseFrame(my_data->in_device_id, my_data->in_channel_main_id, &my_data->ai_frame, 0);
        my_data->pcm_buffer_release_flag = false;
    }

    memset(&my_data->ai_frame, 0x00, sizeof(FRAME_S));

    const hal_audio_config_t *cfg = hal_audio_get_config();
    ret = AK_MPI_AI_GetFrame(my_data->in_device_id, my_data->in_channel_main_id,
                             &my_data->ai_frame, cfg->receive_frame_timeout_ms, 0);
    if(!ret)
    {
        my_data->pcm_buffer_release_flag = true;

        *buffer = my_data->ai_frame.pu8Data;
        *timestamp = my_data->ai_frame.u64Ts;
        *frame_number = my_data->ai_frame.u64SeqNo;
        return my_data->ai_frame.u32Len;
    }

    return ret;
}



// volume: 0.0~1.0 → ASLC dB [-80, 20].
// IMPORTANT (ak_ao.h): SetDigitalGain(0) means "no use aslc", NOT 0dB.
// Soft mute uses -80. Avoid writing 0 except when caller explicitly wants ASLC off.
int hal_audio_output_volume(void *hal_audio, float volume)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    int dgain;

    if (volume < 0.0f)
        volume = 0.0f;
    if (volume > 1.0f)
        volume = 1.0f;

    if (volume <= 0.001f) {
        dgain = -80; /* mute */
    } else {
        dgain = (int)(volume * 100.0f) - 80;
        if (dgain == 0)
            dgain = 1; /* 0 is SDK sentinel "disable ASLC"; nudge to +1dB */
        if (dgain < -90)
            dgain = -90;
        if (dgain > 20)
            dgain = 20;
    }

    my_data->out_volume = dgain;
    AK_MPI_AO_SetDigitalGain(my_data->out_device_id, my_data->out_channel_id, dgain);

    return 0;
}


//volume: 0.0~1.0
//AK_MPI_AI_SetDigitalGain volume source:-90~20, set to -80~20
int hal_audio_input_volume(void *hal_audio, float volume)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;

    //volume=0, set gain mute
    if((volume == 0) && (my_data->in_volume != -80))
    {
        //set mute
        AK_MPI_AI_GetGain(my_data->in_device_id, my_data->in_channel_main_id,&my_data->in_gain);
        AK_MPI_AI_SetGain(my_data->in_device_id, my_data->in_channel_main_id, 0 );
    }
    else if((volume != 0) && (my_data->in_volume <=- 80))
    {
        //return gain
        AK_MPI_AI_SetGain(my_data->in_device_id, my_data->in_channel_main_id, my_data->in_gain );
    }


    my_data->in_volume =  ( int )(volume*100) -80;
    AK_MPI_AI_SetDigitalGain(my_data->in_device_id, my_data->in_channel_main_id, my_data->in_volume );

    return 0;
}


AK_VOID in_dump_callback(AK_S32 item_id, AK_S32 size, AK_VOID *data, AK_VOID *meta, AK_VOID *cb_param)
{
// Not implemented yet

}

int hal_audio_in_dump(void *hal_audio, char *file_path, char *file_name, int enable)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    my_data->audio_in_save_file_info.s32Enable = enable;
    hal_audio_config_t *cfg = hal_audio_get_config();
    strcpy(my_data->audio_in_save_file_info.achFilePath , cfg->save_pcm_file_path); // Temporary: interface only has path, but fd is passed, hardcoded for now
    strcpy(my_data->audio_in_save_file_info.achFileName , cfg->input_save_pcm_file_name);
    my_data->audio_in_save_file_info.s32SaveSecond = cfg->input_save_pcm_file_second;

    AK_MPI_AI_SaveFile(my_data->in_device_id, my_data->in_channel_main_id, &my_data->audio_in_save_file_info);

    AK_MPI_AI_SetDumpCb(my_data->in_device_id, my_data->in_channel_main_id, in_dump_callback);
    AK_MPI_AI_Dump(my_data->in_device_id, my_data->in_channel_main_id,  enable);
    return AK_SUCCESS;

}


AK_VOID out_dump_callback(AK_S32 item_id, AK_S32 size, AK_VOID *data, AK_VOID *meta, AK_VOID *cb_param)
{
// Not implemented yet

}

int hal_audio_out_dump(void *hal_audio, char *file_path, char *file_name, int enable)
{
   struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    my_data->audio_out_save_file_info.s32Enable = enable;
    hal_audio_config_t *cfg = hal_audio_get_config();
    my_data->audio_out_save_file_info.s32SaveSecond = cfg->output_save_pcm_file_second;
    strcpy(my_data->audio_out_save_file_info.achFilePath , cfg->save_pcm_file_path); // Temporary: interface only has path, but fd is passed, hardcoded for now
    strcpy(my_data->audio_out_save_file_info.achFileName , cfg->output_save_pcm_file_name);
    AK_MPI_AO_SaveFile(my_data->in_device_id, my_data->in_channel_main_id, &my_data->audio_out_save_file_info);

    AK_MPI_AO_SetDumpCb(my_data->in_device_id, my_data->in_channel_main_id, out_dump_callback);

    AK_MPI_AO_Dump(my_data->in_device_id, my_data->in_channel_main_id,  enable);
    return AK_SUCCESS;
}


int hal_audio_loopback( void *hal_audio )
{



    return AK_SUCCESS;

}


int hal_audio_set_mic_mute( void *hal_audio , bool mute_state)
{
   struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;

   if(my_data->mic_mute_state == mute_state)
    return 0;

    if(mute_state == true)
    {
        AK_MPI_AI_GetGain(my_data->in_device_id, my_data->in_channel_main_id,&my_data->in_gain);
        AK_MPI_AI_SetGain(my_data->in_device_id, my_data->in_channel_main_id, 0 );
    }
    else{
        AK_MPI_AI_SetGain(my_data->in_device_id, my_data->in_channel_main_id, my_data->in_gain);
    }

    my_data->mic_mute_state = mute_state;
    return 0;
 }


bool hal_audio_get_mic_mute( void *hal_audio)
{
   struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;

   return my_data->mic_mute_state;
}

size_t hal_audio_input_get_frame_len(void *hal_audio)
{
    int len;
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    struct audio_device *adev = my_data->adev;
    struct stream_in *in = adev->in;
    ENC_OUT_INFO stEncLenPam;

    audio_format_t main_format = in->format & AUDIO_FORMAT_MAIN_MASK;

    if(main_format == AUDIO_FORMAT_PCM)
    {
        AK_MPI_AI_GetFrmLen(my_data->in_device_id, my_data->in_channel_main_id, &len);
    }
    else {
        AK_MPI_AENC_GetEncParam(my_data->in_codec_id, &stEncLenPam);
        len = stEncLenPam.s32FrameSize;
    }
    return len;

}


