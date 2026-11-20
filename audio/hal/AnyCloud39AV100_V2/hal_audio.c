// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/audio.
 *
 * HAL audio implementation for AnyCloud39AV100_V2 platform.
 * - Live (non-CALL): open, source, gain, start_capture, get_frame.
 *   start_capture always opens one Echo (AiCapturePcmThread). Do not call
 *   get_source_frame — that opens a second Echo (VqeUserId=1) and SIGSEGVs.
 *   Do not call enable_nr/agc/aec, set_aslc_attr, or set_*_attr once capture
 *   runs. Live also skips aslc_attr before start_capture: that write still
 *   enters _SD_Filter_Control and races FillAdcStream under load.
 * - Output only: ak_ao_open, no AI.
 * - Talk: LoadConf, ak_ai_start_capture, ak_ao_open, then get_frame.
 *   Capture must be up before ak_ao_open or AEC has no matching AI.
 *   hal_audio_echo_lock.c serializes the echo filter across threads.
 * - Talk teardown: stop get_frame, ak_ai_stop_capture, ak_ao_close, ak_ai_close.
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
#include <pthread.h>
#include "../../audio_hw.h"
#include "audio.h"
#include "hal_audio.h"

#include "ak_common.h"
#include "ak_audio_compat.h"
#include "hal_audio_config.h"

#include "../../hal_audio_api.h"

#include "log.h"
#define LOG_TAG "HAL AUDIO"

/* Device-tree base (on target Linux). Node name may be "adc_dac" or "adc_dac@..." */
#define DT_BASE                         "/proc/device-tree"

/*
 * Recursive lock around ak_ai_* / ak_ao_* that reach _SD_Filter_Control.
 * libakaudiofilter is not reentrant against FillAdcStream / GetDacStream.
 */
static pthread_mutex_t g_audio_ctrl_lock;
static pthread_once_t g_audio_ctrl_once = PTHREAD_ONCE_INIT;

static void hal_audio_ai_get_thread_stop(struct hal_audio_data *my_data);

static void hal_audio_ctrl_lock_init(void)
{
    pthread_mutexattr_t attr;

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_audio_ctrl_lock, &attr);
    pthread_mutexattr_destroy(&attr);
}

static void hal_audio_ctrl_lock(void)
{
    pthread_once(&g_audio_ctrl_once, hal_audio_ctrl_lock_init);
    pthread_mutex_lock(&g_audio_ctrl_lock);
}

static void hal_audio_ctrl_unlock(void)
{
    pthread_mutex_unlock(&g_audio_ctrl_lock);
}

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
    ak_sleep_ms(ms);
}

static enum ak_audio_sample_rate hal_map_sample_rate(int rate_hz)
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
 * @brief Initialize HAL audio context.
 * @param adev Audio device pointer.
 * @return HAL audio data pointer on success, NULL on failure.
 */
void *hal_audio_init(struct audio_device *adev)
{
    struct hal_audio_data *my_data;

    hal_audio_config_t *cfg = hal_audio_get_config();
    (void)cfg;

    log_info("AI version: %s", ak_ai_get_version());
    log_info("AO version: %s", ak_ao_get_version());
    log_info("AENC version: %s", ak_aenc_get_version());
    log_info("ADEC version: %s", ak_adec_get_version());


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
    my_data->start_capture_pending = false;
    
    // Initialize codec IDs to -1 to indicate "not opened"
    // 0 might be a valid codec id, so we use -1 to distinguish "not opened" from "opened with id 0"
    my_data->in_codec_id = -1;
    my_data->out_codec_id = -1;
    
    // Initialize device IDs to -1 to indicate "not opened"
    // This prevents invalid device ID errors when functions are called after close
    my_data->ai_handle_id = -1;
    my_data->ao_handle_id = -1;
    my_data->in_device_id = -1;
    my_data->in_channel_main_id = -1;
    my_data->out_device_id = -1;
    my_data->out_channel_id = -1;

    /* Survives open/close so a volume/mute request made while streaming is
     * applied on the next open instead of racing the Echo threads. */
    my_data->out_volume_req = 3;
    my_data->mic_mute_req = false;

    my_data->cfg = cfg;
    pthread_mutex_init(&my_data->ai_get_q_lock, NULL);
    my_data->ai_get_sleep_ms = 16;

    return  (void *)my_data;
}

void hal_audio_deinit(void *hal_audio)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;

    if (!my_data)
        return;

    hal_audio_ai_get_thread_stop(my_data);

    /* Mirror the input teardown audio_hw.c does (audio_hw.c:1466-1470):
     * AENC first (its thread pulls from the AI handle), then the AI handle.
     * Skipping these leaked an open ak_ai/ak_aenc on every deinit, and the
     * next ak_ai_open then failed with the handle still held by the SDK. */
    if (my_data->in_codec_id >= 0)
        hal_audio_codec_in_close(hal_audio);

    if (my_data->ai_handle_id >= 0)
        hal_audio_input_close(hal_audio);

    if (my_data->ao_handle_id >= 0)
        hal_audio_output_close(hal_audio);

    if (my_data->out_codec_id >= 0)
        hal_audio_codec_out_close(hal_audio);

    pthread_mutex_destroy(&my_data->ai_get_q_lock);
    free(my_data);
}

/* Match ak_ao_sample FAR ASLC defaults (volume in dB, roughly -90~20). */
static void hal_ao_apply_default_aslc(int ao_handle_id, int volume_db)
{
    struct ak_audio_aslc_attr aslc_attr = {32768, volume_db, 0};

    if (ak_ao_set_aslc_attr(ao_handle_id, &aslc_attr))
        log_warn("ak_ao_set_aslc_attr failed");
}

/*
 * Last writes to the AO handle for this open cycle; must run before the first
 * ak_ao_send_frame. With vqe_defer_ctrl the ASLC write is dropped: ak_ao_open()
 * already opened ASLC ("ASLC OPEN OK ena:1") and a second Filter_Control while
 * AoPlayPcmThread runs is what crashes libakaudiofilter. Far-end loudness then
 * comes from the AO LoadConf section instead.
 */
static void hal_ao_apply_playback_setup(struct hal_audio_data *my_data)
{
    hal_audio_config_t *cfg = my_data->cfg ? my_data->cfg : hal_audio_get_config();
    int ao_gain;

    /*
     * ak_ao_open already started AoPlayPcmThread (FillDacLoopback / GetDacStream).
     * set_gain / set_volume / enable_* enter _SD_Filter_Control and SIGSEGV
     * (talk crash right after talk_send_data). Far-end loudness stays on
     * [def_far] from LoadConf before open.
     */
    if (cfg->vqe_defer_ctrl) {
        log_info("AO: skip post-open gain/volume/enable (Echo thread running)");
        return;
    }

    ao_gain = cfg->ao_gain;
    if (ao_gain == 0)
        ao_gain = 6;
    if (ak_ao_set_gain(my_data->ao_handle_id, ao_gain))
        log_warn("ak_ao_set_gain failed");
    hal_ao_apply_default_aslc(my_data->ao_handle_id, my_data->out_volume);
    if (ak_ao_set_volume(my_data->ao_handle_id, my_data->out_volume))
        log_warn("ak_ao_set_volume failed");
}

/* Parse MPEG-4 AAC ADTS sample rate (Hz) at stream start; return -1 if invalid. */
static int hal_aac_adts_sample_rate_hz(const unsigned char *buf, int len)
{
    static const int k_rates[] = {
        96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
        16000, 12000, 11025, 8000, 7350,
    };
    int idx;

    if (len < 7 || buf[0] != 0xff || (buf[1] & 0xf0) != 0xf0)
        return -1;

    idx = (buf[2] & 0x3c) >> 2;
    if (idx < 0 || idx >= (int)(sizeof(k_rates) / sizeof(k_rates[0])))
        return -1;
    return k_rates[idx];
}

static int hal_audio_ao_reopen_sample_rate(struct hal_audio_data *my_data, int rate_hz)
{
    struct stream_out *out = my_data->adev->out;

    if ((int)out->sample_rate == rate_hz)
        return 0;

    log_info("AAC ADTS rate %d Hz, reopen AO (was %u Hz)", rate_hz, out->sample_rate);

    hal_audio_ctrl_lock();

    if (my_data->ao_handle_id >= 0) {
        ak_ao_close(my_data->ao_handle_id);
        my_data->ao_handle_id = -1;
    }

    out->sample_rate = (uint32_t)rate_hz;
    my_data->out_param.pcm_data_attr.sample_rate = hal_map_sample_rate(rate_hz);

    if (ak_ao_open(&my_data->out_param, &my_data->ao_handle_id)) {
        log_error("ak_ao_open failed after rate change to %d Hz", rate_hz);
        my_data->ao_handle_id = -1;
        hal_audio_ctrl_unlock();
        return -1;
    }

    hal_ao_apply_playback_setup(my_data);
    hal_audio_ctrl_unlock();
    return 0;
}

static void hal_audio_try_sync_aac_ao_rate(struct hal_audio_data *my_data,
                                          const unsigned char *data, int len)
{
    struct stream_out *out = my_data->adev->out;
    int adts_hz;

    if (my_data->aac_adts_rate_synced)
        return;
    if ((out->format & AUDIO_FORMAT_MAIN_MASK) != AUDIO_FORMAT_AAC)
        return;

    adts_hz = hal_aac_adts_sample_rate_hz(data, len);
    if (adts_hz <= 0)
        return;

    my_data->aac_adts_rate_synced = true;
    /* Only reopen when ADTS rate disagrees with stream config (avoid false sync in buffer). */
    if ((int)out->sample_rate != adts_hz) {
        log_info("AAC ADTS %d Hz vs stream %u Hz", adts_hz, out->sample_rate);
        hal_audio_ao_reopen_sample_rate(my_data, adts_hz);
    }
}



/* Live: do not call enable_nr/agc/aec/eq or set_*_attr after start_capture.
 * enable_*(0) still opens Echo (FillAdcStream/GetResult) and SIGSEGVs under
 * ISP+P2P load. Live also skips ak_ai_set_aslc_attr before start_capture. */

/* AEC / Echo loopback APIs are CALL-only. Live listen has no speaker. */
static int hal_audio_want_aec(audio_mode_t mode)
{
    return (mode == AUDIO_MODE_CALL) ? 1 : 0;
}

static int hal_audio_live_listen(audio_mode_t mode)
{
    return !hal_audio_want_aec(mode);
}

static void hal_audio_apply_pre_open_ai_tunables(hal_audio_config_t *cfg, audio_mode_t mode)
{
    if (ak_ai_set_driver_buf_periods(cfg->ai_drv_buf_periods)) {
        log_warn("[hal_audio_input_open] ak_ai_set_driver_buf_periods failed "
                 "(AI already running?); continuing with current driver buffering");
    }

    /* Capture thread priority is independent of AEC. Live start_capture still
     * creates AiCapturePcmThread / Echo; without this, venc IDR and P2P send
     * can starve FillAdcStream. Not a reason to skip ak_venc_request_idr. */
    if (ak_ai_set_thread_priority(cfg->ai_thread_priority))
        log_warn("[hal_audio_input_open] ak_ai_set_thread_priority failed; continuing");

    if (!hal_audio_want_aec(mode)) {
        log_info("[hal_audio_input_open] live AI: no VQE/AEC APIs (get_frame only)");
        return;
    }

    if (ak_ai_set_aec_driver_buf_periods(cfg->loopback_drv_buf_periods))
        log_warn("[hal_audio_input_open] ak_ai_set_aec_driver_buf_periods failed; continuing");
    if (ak_ai_set_aec_thread_priority(cfg->loopback_thread_priority))
        log_warn("[hal_audio_input_open] ak_ai_set_aec_thread_priority(%d) failed; continuing",
                 cfg->loopback_thread_priority);
}

/* Live skips ak_ai_set_aslc_attr. start_capture still opens Echo/ASLC;
 * writing attr here enters _SD_Filter_Control next to FillAdcStream. */

/* Maps to AV200 AK_MPI_AI_EnableChn. */
static void *hal_audio_ai_get_thread(void *arg)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)arg;
    FRAME_S sdk_frame;
    int ret;
    unsigned int copy_len;
    int sleep_ms;

    log_info("ai_get_frame thread enter (sole ak_ai_get_frame caller)");

    while (my_data->ai_get_run) {
        if (my_data->ai_handle_id < 0 || !my_data->recording_flag) {
            ak_sleep_ms(10);
            continue;
        }

        memset(&sdk_frame, 0x00, sizeof(sdk_frame));
        /* Same consumer as ak_ai_sample: get_frame, copy, release, loop.
         * Sleep only on NO_DATA. Sleeping after success under-runs Echo and
         * SIGSEGVs in ~2s (FillAdcStream overflow vs Filter_Control). */
        ret = ak_ai_get_frame(my_data->ai_handle_id, &sdk_frame, 0);
        if (ret != 0 || sdk_frame.data == NULL || sdk_frame.len <= 0) {
            sleep_ms = my_data->ai_get_sleep_ms > 0 ? my_data->ai_get_sleep_ms : 16;
            ak_sleep_ms((unsigned int)sleep_ms);
            continue;
        }

        copy_len = sdk_frame.len;
        if (copy_len > MAX_FRAME_LEN)
            copy_len = MAX_FRAME_LEN;

        pthread_mutex_lock(&my_data->ai_get_q_lock);
        if (my_data->ai_get_q_n >= HAL_AI_GET_Q_SLOTS) {
            my_data->ai_get_q_r = (my_data->ai_get_q_r + 1) % HAL_AI_GET_Q_SLOTS;
            my_data->ai_get_q_n--;
        }
        {
            int w = my_data->ai_get_q_w;

            memcpy(my_data->ai_get_q[w].data, sdk_frame.data, copy_len);
            my_data->ai_get_q[w].len = copy_len;
            my_data->ai_get_q[w].ts = sdk_frame.ts;
            /* Consumed by hal_audio_input_read as *frame_number; without this
             * every captured frame was reported to the framework as seq 0. */
            my_data->ai_get_q[w].seq_no = sdk_frame.seq_no;
            my_data->ai_get_q_w = (w + 1) % HAL_AI_GET_Q_SLOTS;
            my_data->ai_get_q_n++;
        }
        pthread_mutex_unlock(&my_data->ai_get_q_lock);

        if (ak_ai_release_frame(my_data->ai_handle_id, &sdk_frame))
            log_warn("ai_get_frame thread: ak_ai_release_frame failed");
    }

    log_info("ai_get_frame thread exit");
    return NULL;
}

static int hal_audio_ai_get_thread_start(struct hal_audio_data *my_data)
{
    int err;

    if (!my_data || my_data->ai_get_started)
        return AK_SUCCESS;

    my_data->ai_get_q_r = 0;
    my_data->ai_get_q_w = 0;
    my_data->ai_get_q_n = 0;
    my_data->ai_get_run = 1;
    err = pthread_create(&my_data->ai_get_tid, NULL, hal_audio_ai_get_thread, my_data);
    if (err) {
        my_data->ai_get_run = 0;
        log_error("ai_get_frame thread create failed: %d", err);
        return AK_FAILED;
    }
    my_data->ai_get_started = 1;
    log_info("ai_get_frame thread started");
    return AK_SUCCESS;
}

static void hal_audio_ai_get_thread_stop(struct hal_audio_data *my_data)
{
    if (!my_data || !my_data->ai_get_started)
        return;

    my_data->ai_get_run = 0;
    pthread_join(my_data->ai_get_tid, NULL);
    my_data->ai_get_tid = 0;
    my_data->ai_get_started = 0;
    pthread_mutex_lock(&my_data->ai_get_q_lock);
    my_data->ai_get_q_r = 0;
    my_data->ai_get_q_w = 0;
    my_data->ai_get_q_n = 0;
    pthread_mutex_unlock(&my_data->ai_get_q_lock);
    log_info("ai_get_frame thread stopped");
}

/* ak_ai_start_capture only. Does not start the userspace get_frame thread. */
static int hal_audio_sdk_start_capture_locked(struct hal_audio_data *my_data)
{
    int ret;

    if (!my_data || my_data->ai_handle_id < 0)
        return AK_FAILED;
    if (my_data->recording_flag)
        return AK_SUCCESS;

    log_info("[hal_audio_input_open] Step 8: ak_ai_start_capture handle=%d",
             my_data->ai_handle_id);
    ret = ak_ai_start_capture(my_data->ai_handle_id);
    log_info("[hal_audio_input_open] Step 8: ak_ai_start_capture ret=%d", ret);
    if (ret) {
        log_error("[hal_audio_input_open] Step 8: ak_ai_start_capture failed: ret=%d", ret);
        return AK_FAILED;
    }
    my_data->recording_flag = true;
    my_data->start_capture_pending = false;
    return AK_SUCCESS;
}

static int hal_audio_attach_get_thread_locked(struct hal_audio_data *my_data)
{
    /* Let AiCapturePcmThread produce one frame before our get_frame thread. */
    ak_sleep_ms(32);
    if (hal_audio_ai_get_thread_start(my_data) != AK_SUCCESS) {
        log_error("[hal_audio_input_open] ai_get_frame thread start failed");
        return AK_FAILED;
    }
    return AK_SUCCESS;
}

static int hal_audio_start_capture_locked(struct hal_audio_data *my_data)
{
    if (!my_data || my_data->ai_handle_id < 0)
        return AK_FAILED;
    if (my_data->recording_flag)
        return AK_SUCCESS;
    if (hal_audio_sdk_start_capture_locked(my_data) != AK_SUCCESS)
        return AK_FAILED;
    if (hal_audio_attach_get_thread_locked(my_data) != AK_SUCCESS) {
        ak_ai_stop_capture(my_data->ai_handle_id);
        my_data->recording_flag = false;
        return AK_FAILED;
    }
    return AK_SUCCESS;
}

/**
 * Close an AI handle that the SDK still holds but HAL is not tracking.
 * Without this, a later ak_ai_open() fails with "Device already enabled" and
 * recording/RTSP fall back to video-only.
 */
static void hal_audio_reclaim_orphan_ai(struct hal_audio_data *my_data, int dev_id)
{
    int orphan_id = -1;

    if (!my_data)
        return;

    if (my_data->recording_flag && my_data->ai_handle_id >= 0) {
        log_warn("[hal_audio_input_open] Device already recording, closing AI first handle=%d",
                 my_data->ai_handle_id);
        /* get_thread does not take ctrl_lock; safe to join under open lock. */
        hal_audio_ai_get_thread_stop(my_data);
        (void)ak_ai_stop_capture(my_data->ai_handle_id);
        if (ak_ai_close(my_data->ai_handle_id))
            log_warn("[hal_audio_input_open] ak_ai_close failed handle=%d", my_data->ai_handle_id);
        my_data->ai_handle_id = -1;
        my_data->recording_flag = false;
        return;
    }

    /* SDK reports handle 0 when ADC is not enabled; do not treat that as orphan. */
    if (ak_ai_get_handle_id(dev_id, &orphan_id) == 0 && orphan_id >= 0) {
        if (ak_ai_stop_capture(orphan_id) != 0)
            return;
        log_warn("[hal_audio_input_open] reclaim orphan AI handle=%d (HAL was not tracking)",
                 orphan_id);
        if (ak_ai_close(orphan_id))
            log_warn("[hal_audio_input_open] orphan ak_ai_close failed handle=%d", orphan_id);
        my_data->ai_handle_id = -1;
        my_data->recording_flag = false;
    }
}

static int hal_audio_input_open_locked(struct hal_audio_data *my_data)
{
    int ret = AK_FAILED;
    struct audio_device *adev = my_data->adev;
    struct stream_in *in = adev->in;

    log_info("[hal_audio_input_open] Starting: format=0x%x, channel_mask=0x%x, devices=0x%x, sample_rate=%d",
             in->format, in->channel_mask, in->devices, in->sample_rate);

    /* Detect AMIC/DMIC and mic count from DTB/ini so upper layers don't care. */
    audio_devices_t detected_dev = AUDIO_DEVICE_IN_AMIC;
    int detected_mic_num = 1;
    hal_audio_detect_mic_hw(in->devices, &detected_dev, &detected_mic_num);
    if (detected_mic_num < 1) detected_mic_num = 1;

    memset(&my_data->in_param, 0, sizeof(my_data->in_param));

    my_data->pcm_buffer_release_flag = false;
    my_data->in_codec_buffer_release_flag = false;

    if(!is_supported_format(in->format))
    {
        log_error("[hal_audio_input_open] Audio format not support, only support PCM、AAC、MP3、WMA、ALAW、uLAW");
        return AK_FAILED;
    }

    log_info("[hal_audio_input_open] Step 1: Setting channel parameters");
    audio_channel_mask_t main_channel = in->channel_mask & AUDIO_CHANNEL_IN_MAIN_MASK;

    /* Clamp requested channels to hardware mic count. */
    if (main_channel == AUDIO_CHANNEL_IN_STEREO && detected_mic_num < 2) {
        log_warn("[hal_audio_input_open] Requested stereo but hardware mic_num=%d, fallback to mono", detected_mic_num);
        main_channel = AUDIO_CHANNEL_IN_MONO;
        in->channel_mask = (in->channel_mask & ~AUDIO_CHANNEL_IN_MAIN_MASK) | AUDIO_CHANNEL_IN_MONO;
    }

    my_data->in_param.pcm_data_attr.sample_rate = hal_map_sample_rate(in->sample_rate);
    my_data->in_param.pcm_data_attr.sample_bits = AK_AUDIO_SMPLE_BIT_16;

    if(main_channel == AUDIO_CHANNEL_IN_MONO)
    {
        my_data->in_param.pcm_data_attr.channel_num = AUDIO_CHANNEL_MONO;
        log_info("[hal_audio_input_open] Step 1: Set to mono (ak_ai single capture path)");
    }
    else if(main_channel == AUDIO_CHANNEL_IN_STEREO)
    {
        my_data->in_param.pcm_data_attr.channel_num = AUDIO_CHANNEL_STEREO;
        log_info("[hal_audio_input_open] Step 1: Set to stereo");
    }
    else
    {
        log_error("[hal_audio_input_open] Audio channel not support, only support MONO and STEREO");
        return AK_FAILED;
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

    my_data->in_param.dev_id = my_data->in_device_id;

    /* Must reclaim before set_driver_buf_periods / ak_ai_open: those APIs fail
     * hard when the SDK device is already enabled but HAL ai_handle_id is -1. */
    hal_audio_reclaim_orphan_ai(my_data, my_data->in_device_id);

    hal_audio_config_t *cfg_open = hal_audio_get_config();
    hal_audio_apply_pre_open_ai_tunables(cfg_open, adev->mode);

    log_info("[hal_audio_input_open] Step 3: ak_ai_open dev_id=%d rate=%d ch=%d",
             my_data->in_param.dev_id, (int)my_data->in_param.pcm_data_attr.sample_rate,
             (int)my_data->in_param.pcm_data_attr.channel_num);
    if (ak_ai_open(&my_data->in_param, &my_data->ai_handle_id)) {
        /* Race: another path may have enabled AI between reclaim and open. */
        log_warn("[hal_audio_input_open] ak_ai_open failed; reclaim+retry once");
        my_data->ai_handle_id = -1;
        my_data->recording_flag = false;
        hal_audio_reclaim_orphan_ai(my_data, my_data->in_device_id);
        if (ak_ai_open(&my_data->in_param, &my_data->ai_handle_id)) {
            log_error("[hal_audio_input_open] ak_ai_open failed after reclaim");
            my_data->ai_handle_id = -1;
            return AK_FAILED;
        }
    }

    if (ak_ai_set_lib_print_level(my_data->ai_handle_id, cfg_open->ai_print_level))
        log_warn("[hal_audio_input_open] ak_ai_set_lib_print_level failed");

    if (my_data->in_device_id == DEV_ADC) {
        if (ak_ai_set_source(my_data->ai_handle_id, AI_SOURCE_MIC)) {
            log_error("[hal_audio_input_open] ak_ai_set_source failed");
            ak_ai_close(my_data->ai_handle_id);
            my_data->ai_handle_id = -1;
            return AK_FAILED;
        }
    }

    /*
     * Live: no VQE attrs / LoadConf. start_capture still opens one Echo;
     * get_frame is the only userspace consumer (same as ak_ai_sample).
     * CALL: LoadConf [talk_near] for AEC.
     */
    if (hal_audio_live_listen(adev->mode)) {
        log_info("[hal_audio_input_open] Step 6: live PCM (no VQE/LoadConf, get_frame)");
    } else if (cfg_open->vqe_defer_ctrl && my_data->playing_flag) {
        log_info("[hal_audio_input_open] Step 6: skip LoadConf (AO Echo running)");
    } else {
        log_info("[hal_audio_input_open] Step 6: Loading audio effect configuration");
        ret = hal_audio_effect_in_loadconf(adev->hal_audio);
        if (ret != 0)
            log_warn("[hal_audio_input_open] Step 6: LoadConf failed: %d, continuing", ret);
        else
            log_info("[hal_audio_input_open] Step 6: Audio effect configuration loaded");
    }

    log_info("[hal_audio_input_open] Step 5: Getting frame length");
    {
        int s32FrameLen = 0;
        if (ak_ai_get_frame_length(my_data->ai_handle_id, &s32FrameLen)) {
            log_error("[hal_audio_input_open] ak_ai_get_frame_length failed");
            ak_ai_close(my_data->ai_handle_id);
            my_data->ai_handle_id = -1;
            return AK_FAILED;
        }
        log_info("[hal_audio_input_open] Step 5: Frame length=%d", s32FrameLen);
        in->in_frame_interval = (int)ak_audio_len_to_interval(&my_data->in_param.pcm_data_attr,
                                                               (unsigned int)s32FrameLen);
        /* Same poll period as ak_ai_sample: frame_interval/2 ms. */
        my_data->ai_get_sleep_ms = (in->in_frame_interval / 2) > 0
                                   ? (in->in_frame_interval / 2) : 16;
    }

    /* Step 7 SetGain — after VQE attrs / LoadConf, matching sample and AV200. */
    if (my_data->in_device_id != DEV_PDM) {
        int ai_gain = cfg_open->ai_gain;
        if (ai_gain < 0)
            ai_gain = 0;
        if (ai_gain > 8)
            ai_gain = 8;
        my_data->in_gain = ai_gain;
        if (cfg_open->vqe_defer_ctrl && my_data->mic_mute_req) {
            my_data->mic_mute_state = true;
            ai_gain = 0;
            log_info("[hal_audio_input_open] Step 7: applying deferred mic mute");
        }
        if (cfg_open->vqe_defer_ctrl && my_data->playing_flag) {
            log_info("[hal_audio_input_open] Step 7: skip set_gain (AO Echo running)");
        } else {
            log_info("[hal_audio_input_open] Step 7: Setting gain: gain=%d", ai_gain);
            if (ak_ai_set_gain(my_data->ai_handle_id, ai_gain)) {
                log_error("[hal_audio_input_open] Step 7: ak_ai_set_gain failed");
                ak_ai_close(my_data->ai_handle_id);
                my_data->ai_handle_id = -1;
                return AK_FAILED;
            }
            log_info("[hal_audio_input_open] Step 7: Gain set successfully");
        }
    } else {
        log_warn("[hal_audio_input_open] ak_ai_set_gain not supported on PDM");
    }

    /*
     * Step 7b: CALL gets ASLC from LoadConf. Live skips ak_ai_set_aslc_attr:
     * start_capture still opens Echo/ASLC, and this write enters
     * _SD_Filter_Control next to FillAdcStream under 1440P P2P.
     */
    if (hal_audio_live_listen(adev->mode))
        log_info("[hal_audio_input_open] Step 7b: skip ASLC attr (live get_frame)");

    /*
     * Step 8. Live starts capture and get_frame now.
     * CALL leaves capture pending so output open can start it before ak_ao_open.
     * Opening CALL input while AO is already playing cannot bind AEC: the match
     * already ran (or would race FillDacLoopback). Caller must close AO first.
     */
    if (cfg_open->vqe_defer_ctrl && adev->mode == AUDIO_MODE_CALL) {
        if (my_data->playing_flag) {
            log_error("[hal_audio_input_open] CALL while AO is already open; close AO first");
            ak_ai_close(my_data->ai_handle_id);
            my_data->ai_handle_id = -1;
            return AK_FAILED;
        }
        my_data->start_capture_pending = true;
        log_info("[hal_audio_input_open] Step 8: defer start_capture until before ak_ao_open");
        log_info("audio input opened (capture pending): dev_id=%d, ai_handle=%d, rate=%d",
                 my_data->in_device_id, my_data->ai_handle_id, in->sample_rate);
        return AK_SUCCESS;
    }

    ret = hal_audio_start_capture_locked(my_data);
    if (ret != AK_SUCCESS) {
        ak_ai_close(my_data->ai_handle_id);
        my_data->ai_handle_id = -1;
        return AK_FAILED;
    }

    log_info("audio input opened: dev_id=%d, ai_handle=%d, rate=%d",
             my_data->in_device_id, my_data->ai_handle_id, in->sample_rate);

    return AK_SUCCESS;
}

int hal_audio_input_open(void *hal_audio)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    int ret;

    if (!my_data)
        return AK_FAILED;

    hal_audio_ctrl_lock();
    ret = hal_audio_input_open_locked(my_data);
    hal_audio_ctrl_unlock();
    return ret;
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
    if (in == NULL || adev->callback_ops == NULL || my_data->ai_handle_id < 0)
        return 0;

    /* Effect path starts capture in check_effect_init after add_audio_effect. */
    if (my_data->in_effect != AUDIO_EFFECT_NONE)
        return 0;

    return 1;
}

int hal_audio_input_stop_capture(void *hal_audio)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;

    if (!my_data)
        return 0;

    /* Join get_frame thread before touching ak_ai_stop_capture. */
    hal_audio_ai_get_thread_stop(my_data);

    hal_audio_ctrl_lock();
    if (my_data->start_capture_pending) {
        my_data->start_capture_pending = false;
        hal_audio_ctrl_unlock();
        return 0;
    }
    if (my_data->ai_handle_id < 0 || !my_data->recording_flag) {
        hal_audio_ctrl_unlock();
        return 0;
    }

    my_data->pcm_buffer_release_flag = false;
    memset(&my_data->ai_frame, 0x00, sizeof(my_data->ai_frame));
    ak_ai_stop_capture(my_data->ai_handle_id);
    my_data->recording_flag = false;
    hal_audio_ctrl_unlock();
    {
        hal_audio_config_t *cfg = my_data->cfg ? my_data->cfg : hal_audio_get_config();

        /* Let AiCapturePcmThread leave FillAdcStream before AO close. */
        if (cfg != NULL && cfg->vqe_drain_close_ms > 0)
            ak_sleep_ms((unsigned int)cfg->vqe_drain_close_ms);
    }
    return 0;
}

int hal_audio_input_close(void *hal_audio)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    struct audio_device *adev = my_data->adev;
    struct stream_in *in = adev->in;
    hal_audio_config_t *cfg = my_data->cfg ? my_data->cfg : hal_audio_get_config();

    (void)in;

    /* Stop get_frame thread without holding ctrl_lock (join may sleep). */
    hal_audio_ai_get_thread_stop(my_data);

    hal_audio_ctrl_lock();

    if (my_data->ai_handle_id >= 0) {
        if (my_data->start_capture_pending) {
            my_data->start_capture_pending = false;
        } else if (my_data->recording_flag) {
            my_data->pcm_buffer_release_flag = false;
            memset(&my_data->ai_frame, 0x00, sizeof(my_data->ai_frame));
            ak_ai_stop_capture(my_data->ai_handle_id);
            my_data->recording_flag = false;
        }
        /* Closing while the AI Echo still holds samples logs
         * "PcmBuf close while N B pending" and leaves a dangling filter that a
         * later AO open trips over. Give the SDK thread a bounded settle time. */
        if (cfg->vqe_drain_close_ms > 0)
            ak_sleep_ms((unsigned int)cfg->vqe_drain_close_ms);
        if (ak_ai_close(my_data->ai_handle_id)) {
            log_error("ak_ai_close failed");
        }
        my_data->ai_handle_id = -1;
    }

    my_data->in_device_id = -1;
    my_data->in_channel_main_id = -1;
    my_data->start_capture_pending = false;

    my_data->in_mode = AUDIO_MODE_NONE;

    log_info("audio input closed");
    hal_audio_ctrl_unlock();
    return 0;
}


static int hal_audio_output_open_locked(struct hal_audio_data *my_data)
{
    struct audio_device *adev = my_data->adev;
    struct stream_out *out = adev->out;
    int talk_bind = 0;

    if (my_data->playing_flag && my_data->ao_handle_id >= 0) {
        ak_ao_close(my_data->ao_handle_id);
        my_data->ao_handle_id = -1;
        my_data->playing_flag = false;
    }

    memset(&my_data->out_param, 0, sizeof(my_data->out_param));

    log_debug("hal_audio_output_open: format=0x%08x, main_format=0x%08x",
              out->format, out->format & AUDIO_FORMAT_MAIN_MASK);

    if(!is_supported_format(out->format))
    {
        log_error("audio format not support (format=0x%08x, main_format=0x%08x), only support PCM、AAC、MP3、WMA、ALAW、uLAW, please check",
                  out->format, out->format & AUDIO_FORMAT_MAIN_MASK);
        return AK_FAILED;
    }

    my_data->out_param.pcm_data_attr.sample_rate = hal_map_sample_rate(out->sample_rate);
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

    my_data->out_channel_id = 0;

    switch (out->devices)
    {
        case AUDIO_DEVICE_OUT_SPEAKER:
            my_data->out_device_id = DEV_DAC;
        break;

        case AUDIO_DEVICE_OUT_BUS:
            my_data->out_device_id = DEV_I2S0_AO;
        break;

        case AUDIO_DEVICE_OUT_USB_DEVICE:
        break;

        default :
            my_data->out_device_id = DEV_DAC;
    }

    my_data->out_param.dev_id = my_data->out_device_id;

    hal_audio_config_t *cfg = hal_audio_get_config();
    /*
     * ak_ao_open starts Echo / AoPlayPcmThread. LoadConf after that is
     * Filter_Control vs GetDacStream. Load the far-end section on the
     * device before open (talk: [def_far]).
     * ak_ao_load_dev_conf is V2.05-only (no declaration/symbol in V2.04);
     * the audio Makefile passes AK_SDK_VERSION_NUM from CONFIG_SOC_SDK_VERSION.
     */
#if defined(AK_SDK_VERSION_NUM) && (AK_SDK_VERSION_NUM >= 205)
    if (ak_ao_load_dev_conf(my_data->out_device_id, cfg->loadconf_file_path,
                            cfg->loadconf_out_normal_section)) {
        log_warn("ak_ao_load_dev_conf failed section=%s; continuing",
                 cfg->loadconf_out_normal_section);
    } else {
        my_data->out_mode = adev->mode;
        log_info("AO: LoadConf section=%s (mode=%d, before open)",
                 cfg->loadconf_out_normal_section, (int)adev->mode);
    }
#endif

    if (ak_ao_set_driver_buf_periods(cfg->ao_drv_buf_periods))
    {
        log_error("ak_ao_set_driver_buf_periods failed");
        return AK_FAILED;
    }

    if (ak_ao_set_thread_priority(cfg->ao_thread_priority))
    {
        log_error("ak_ao_set_thread_priority failed");
        return AK_FAILED;
    }

    /*
     * CALL input is open but capture is pending. VqeAiOpen (inside
     * ak_ai_start_capture) must finish before ak_ao_open, or
     * AoGetAiUserIdAboutAec logs "There is no matching AI".
     * get_frame stays down until this open returns.
     */
    talk_bind = my_data->start_capture_pending;

    if (talk_bind) {
        log_info("AO open: start near capture before ak_ao_open (AEC match)");
        if (hal_audio_sdk_start_capture_locked(my_data) != AK_SUCCESS) {
            log_error("AO open: start_capture before ak_ao_open failed; skip AO");
            my_data->start_capture_pending = false;
            return AK_FAILED;
        }
    }

    if (ak_ao_open(&my_data->out_param, &my_data->ao_handle_id)) {
        log_error("ak_ao_open failed: dev_id=%d rate=%d ch=%d",
                  my_data->out_param.dev_id,
                  (int)my_data->out_param.pcm_data_attr.sample_rate,
                  (int)my_data->out_param.pcm_data_attr.channel_num);
        my_data->ao_handle_id = -1;
        if (talk_bind && my_data->recording_flag && my_data->ai_handle_id >= 0) {
            (void)ak_ai_stop_capture(my_data->ai_handle_id);
            my_data->recording_flag = false;
        }
        return AK_FAILED;
    }

    my_data->aac_adts_rate_synced = false;
    my_data->out_volume = cfg->vqe_defer_ctrl ? my_data->out_volume_req : 3;
    my_data->out_gain = cfg->ao_gain;
    /* Echo / FillDacLoopback start in ak_ao_open. Mark playing first so
     * apply_playback_setup and later set_gain/volume skip Filter_Control. */
    my_data->playing_flag = true;
    hal_ao_apply_playback_setup(my_data);

    if (talk_bind &&
        hal_audio_attach_get_thread_locked(my_data) != AK_SUCCESS) {
        log_error("AO open: get_frame thread failed after AEC bind; closing AO");
        my_data->playing_flag = false;
        if (my_data->ao_handle_id >= 0) {
            ak_sleep_ms(cfg->vqe_drain_close_ms ? cfg->vqe_drain_close_ms : 50);
            if (ak_ao_close(my_data->ao_handle_id))
                log_error("ak_ao_close after get_frame start failed");
            my_data->ao_handle_id = -1;
        }
        if (my_data->recording_flag && my_data->ai_handle_id >= 0) {
            (void)ak_ai_stop_capture(my_data->ai_handle_id);
            my_data->recording_flag = false;
        }
        my_data->out_mode = AUDIO_MODE_NONE;
        return AK_FAILED;
    }

    log_info("audio output opened: dev_id=%d, ao_handle=%d, rate=%d",
             my_data->out_device_id, my_data->ao_handle_id, out->sample_rate);

    return AK_SUCCESS;
}

int hal_audio_output_open(void *hal_audio)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    int ret;

    if (!my_data)
        return AK_FAILED;

    hal_audio_ctrl_lock();
    ret = hal_audio_output_open_locked(my_data);
    hal_audio_ctrl_unlock();
    return ret;
}


int hal_audio_output_close(void *hal_audio)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    struct audio_device *adev = my_data->adev;
    struct stream_out *out = adev->out;

    hal_audio_ctrl_lock();

    my_data->playing_flag = false;
    my_data->aac_adts_rate_synced = false;

    /*
     * Do not ak_ao_flush here. Flush enters _SD_Filter_Control while
     * FillDacLoopback still holds Echo (speaker-stop SIGSEGV).
     */
    if (my_data->ao_handle_id >= 0)
        ak_sleep_ms(50);

    if (my_data->ao_handle_id >= 0) {
        if (ak_ao_close(my_data->ao_handle_id)) {
            log_error("ak_ao_close failed.");
        }
        my_data->ao_handle_id = -1;
    }

    my_data->out_device_id = -1;
    my_data->out_channel_id = -1;
    my_data->out_mode = AUDIO_MODE_NONE;

    log_info("audio output closed");
    hal_audio_ctrl_unlock();
    return AK_SUCCESS;
}


static AK_VOID PrintPlayingDot(AK_VOID)
{
    static AK_U8 u8FirstFlag = 1;
    static struct ak_timeval stCurTime;
    static struct ak_timeval stPrintTime;

    if (u8FirstFlag) {
        u8FirstFlag = 0;
        ak_get_ostime(&stCurTime);
        ak_get_ostime(&stPrintTime);
    }

    /* Update timestamp every 10 seconds */
    ak_get_ostime(&stCurTime);
    if (ak_diff_ms_time(&stCurTime, &stPrintTime) >= 10000) {
        ak_get_ostime(&stPrintTime);
    }
}

int hal_audio_pcm_write(void *hal_audio, unsigned char *send_pcm_data,int send_pcm_len)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    struct audio_device *adev = my_data->adev;
    struct stream_out *out = adev->out;
    int play_pcm_len = 0;
    int ret;

    if (!my_data->playing_flag || my_data->ao_handle_id < 0) {
        return -1;
    }

    if((out->format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_PCM)
    {
        /* Data path only. Do not call set_gain / set_volume / LoadConf here:
         * FillDacLoopback is already in AoPlayPcmThread. */
        ret = ak_ao_send_frame(my_data->ao_handle_id, send_pcm_data, send_pcm_len, &play_pcm_len);
        if (!ret)
            ak_sleep_ms(10);
        if (ret)
            return -1;
        return play_pcm_len > 0 ? play_pcm_len : send_pcm_len;
    }

    if (my_data->out_codec_id < 0) {
        return -1;
    }
    hal_audio_try_sync_aac_ao_rate(my_data, send_pcm_data, send_pcm_len);
    /* ak_adec_sample: blocking send (last arg = 1). */
    ret = ak_adec_send_stream(my_data->out_codec_id, send_pcm_data,
                              (unsigned int)send_pcm_len, 1);
    if (ret) {
        if (ret == ERROR_ADEC_SEND_STREAM_NO_SPACE) {
            log_warn("send stream timeout");
        } else {
            log_error("ak_adec_send_stream failed: ret=%d", ret);
        }
        return -1;
    }
    ak_sleep_ms(10);
    PrintPlayingDot();
    /* ADEC does not fill play_pcm_len. Returning 0 made out_write treat G711
     * talkback as failure even when send_stream succeeded. */
    return send_pcm_len;
}

size_t hal_audio_pcm_read(void *hal_audio, void **buffer,uint32_t *frame_number, uint64_t * timestamp,size_t bytes)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    unsigned int copy_len = 0;
    unsigned long long ts = 0;
    unsigned long seq_no = 0;
    int got = 0;

    (void)bytes;

    if (!my_data || my_data->ai_handle_id < 0) {
        log_warn("hal_audio_pcm_read: ai_handle invalid, audio input may be closed");
        return 0;
    }

    /*
     * Do not call ak_ai_get_frame here. Sole get_frame thread pushes PCM
     * copies into ai_get_q; this path only pops (Opus/MISS stay off Echo).
     */
    pthread_mutex_lock(&my_data->ai_get_q_lock);
    if (my_data->ai_get_q_n > 0) {
        int r = my_data->ai_get_q_r;

        copy_len = my_data->ai_get_q[r].len;
        ts = my_data->ai_get_q[r].ts;
        seq_no = my_data->ai_get_q[r].seq_no;
        if (copy_len > MAX_FRAME_LEN)
            copy_len = MAX_FRAME_LEN;
        memcpy(my_data->pcm_copy_buf, my_data->ai_get_q[r].data, copy_len);
        my_data->ai_get_q_r = (r + 1) % HAL_AI_GET_Q_SLOTS;
        my_data->ai_get_q_n--;
        got = 1;
    }
    pthread_mutex_unlock(&my_data->ai_get_q_lock);

    if (!got)
        return 0;

    my_data->ai_frame.data = my_data->pcm_copy_buf;
    my_data->ai_frame.len = copy_len;
    my_data->ai_frame.ts = ts;
    my_data->ai_frame.seq_no = seq_no;
    my_data->pcm_buffer_release_flag = true;

    if (buffer)
        *buffer = my_data->pcm_copy_buf;
    if (timestamp)
        *timestamp = ts;
    if (frame_number)
        *frame_number = (uint32_t)seq_no;

    return (size_t)copy_len;
}



/* volume: 0.0~1.0 mapped to ak_* digital volume about -90~20 dB */
int hal_audio_output_volume(void *hal_audio, float volume)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    hal_audio_config_t *cfg;

    if (my_data->ao_handle_id < 0)
        return -1;

    cfg = my_data->cfg ? my_data->cfg : hal_audio_get_config();
    my_data->out_volume_req = (int)(volume * 100) - 80;

    /* ak_ao_set_volume / ak_ao_set_gain enter _SD_Filter_Control, which races
     * AoPlayPcmThread's GetDacStream. Record only; hal_ao_apply_playback_setup
     * applies it on the next open. */
    if (cfg->vqe_defer_ctrl && my_data->playing_flag) {
        log_info("AO busy: defer volume %d dB to next open", my_data->out_volume_req);
        return 0;
    }

    hal_audio_ctrl_lock();
    if((volume == 0) && (my_data->out_volume != -80))
    {
        ak_ao_get_gain(my_data->ao_handle_id, &my_data->out_gain);
        ak_ao_set_gain(my_data->ao_handle_id, 0);
    }
    else if((volume != 0) && (my_data->out_volume <= -80))
    {
        ak_ao_set_gain(my_data->ao_handle_id, my_data->out_gain);
    }

    my_data->out_volume = my_data->out_volume_req;
    ak_ao_set_volume(my_data->ao_handle_id, my_data->out_volume);
    hal_audio_ctrl_unlock();

    return 0;
}


int hal_audio_input_volume(void *hal_audio, float volume)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    hal_audio_config_t *cfg;

    if (my_data->ai_handle_id < 0)
        return -1;

    cfg = my_data->cfg ? my_data->cfg : hal_audio_get_config();
    if (cfg->vqe_defer_ctrl && my_data->recording_flag) {
        log_info("AI busy: skip input volume (Filter_Control vs FillAdcStream)");
        return 0;
    }

    hal_audio_ctrl_lock();
    if((volume == 0) && (my_data->in_volume != -80))
    {
        ak_ai_get_gain(my_data->ai_handle_id, &my_data->in_gain);
        ak_ai_set_gain(my_data->ai_handle_id, 0);
    }
    else if((volume != 0) && (my_data->in_volume <= -80))
    {
        ak_ai_set_gain(my_data->ai_handle_id, my_data->in_gain);
    }

    my_data->in_volume = (int)(volume * 100) - 80;
    ak_ai_set_volume(my_data->ai_handle_id, my_data->in_volume);
    hal_audio_ctrl_unlock();

    return 0;
}


int hal_audio_in_dump(void *hal_audio, char *file_path, char *file_name, int enable)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    if (my_data->ai_handle_id < 0)
        return AK_FAILED;

    hal_audio_config_t *cfg = hal_audio_get_config();
    const char *dump_path = (file_path && file_path[0] != '\0') ? file_path : cfg->save_pcm_file_path;
    const char *dump_name = (file_name && file_name[0] != '\0') ? file_name : cfg->input_save_pcm_file_name;

    my_data->audio_in_save_file_info.enable = enable;
    strncpy(my_data->audio_in_save_file_info.file_path, dump_path,
            sizeof(my_data->audio_in_save_file_info.file_path) - 1);
    my_data->audio_in_save_file_info.file_path[sizeof(my_data->audio_in_save_file_info.file_path) - 1] = '\0';
    strncpy(my_data->audio_in_save_file_info.file_name, dump_name,
            sizeof(my_data->audio_in_save_file_info.file_name) - 1);
    my_data->audio_in_save_file_info.file_name[sizeof(my_data->audio_in_save_file_info.file_name) - 1] = '\0';
    my_data->audio_in_save_file_info.save_second = cfg->input_save_pcm_file_second;

    if (ak_ai_save_file(my_data->ai_handle_id, &my_data->audio_in_save_file_info)) {
        log_error("ak_ai_save_file failed");
        return AK_FAILED;
    }
    return AK_SUCCESS;

}


int hal_audio_out_dump(void *hal_audio, char *file_path, char *file_name, int enable)
{
   struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    if (my_data->ao_handle_id < 0)
        return AK_FAILED;

    hal_audio_config_t *cfg = hal_audio_get_config();
    const char *dump_path = (file_path && file_path[0] != '\0') ? file_path : cfg->save_pcm_file_path;
    const char *dump_name = (file_name && file_name[0] != '\0') ? file_name : cfg->output_save_pcm_file_name;

    my_data->audio_out_save_file_info.enable = enable;
    my_data->audio_out_save_file_info.save_second = cfg->output_save_pcm_file_second;
    strncpy(my_data->audio_out_save_file_info.file_path, dump_path,
            sizeof(my_data->audio_out_save_file_info.file_path) - 1);
    my_data->audio_out_save_file_info.file_path[sizeof(my_data->audio_out_save_file_info.file_path) - 1] = '\0';
    strncpy(my_data->audio_out_save_file_info.file_name, dump_name,
            sizeof(my_data->audio_out_save_file_info.file_name) - 1);
    my_data->audio_out_save_file_info.file_name[sizeof(my_data->audio_out_save_file_info.file_name) - 1] = '\0';

    if (ak_ao_save_file(my_data->ao_handle_id, &my_data->audio_out_save_file_info)) {
        log_error("ak_ao_save_file failed");
        return AK_FAILED;
    }
    return AK_SUCCESS;
}


int hal_audio_loopback( void *hal_audio )
{



    return AK_SUCCESS;

}


int hal_audio_set_mic_mute( void *hal_audio , bool mute_state)
{
   struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
   hal_audio_config_t *cfg;

   if(my_data->mic_mute_state == mute_state)
    return 0;

    if (my_data->ai_handle_id < 0)
        return -1;

    cfg = my_data->cfg ? my_data->cfg : hal_audio_get_config();
    my_data->mic_mute_req = mute_state;

    /* ak_ai_set_gain enters _SD_Filter_Control, which races the capture
     * thread's FillAdcStream. Record only; hal_audio_input_open applies it. */
    if (cfg->vqe_defer_ctrl && my_data->recording_flag) {
        log_info("AI busy: defer mic mute=%d to next open", (int)mute_state);
        return 0;
    }

    hal_audio_ctrl_lock();
    if(mute_state == true)
    {
        ak_ai_get_gain(my_data->ai_handle_id, &my_data->in_gain);
        ak_ai_set_gain(my_data->ai_handle_id, 0);
    }
    else{
        ak_ai_set_gain(my_data->ai_handle_id, my_data->in_gain);
    }

    my_data->mic_mute_state = mute_state;
    hal_audio_ctrl_unlock();
    return 0;
 }


bool hal_audio_get_mic_mute( void *hal_audio)
{
   struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;

   return my_data->mic_mute_state;
}

size_t hal_audio_input_get_frame_len(void *hal_audio)
{
    int len = 0;
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    struct audio_device *adev = my_data->adev;
    struct stream_in *in = adev->in;

    audio_format_t main_format = in->format & AUDIO_FORMAT_MAIN_MASK;

    if(main_format == AUDIO_FORMAT_PCM)
    {
        if (my_data->ai_handle_id >= 0 &&
            ak_ai_get_frame_length(my_data->ai_handle_id, &len)) {
            log_error("ak_ai_get_frame_length failed");
            len = 0;
        }
    }
    else {
        struct aenc_out_info enc_len = {0};
        if (my_data->in_codec_id >= 0 &&
            ak_aenc_get_enc_info(my_data->in_codec_id, &enc_len) == 0) {
            len = (int)enc_len.frame_size;
        }
    }
    return (size_t)len;

}


