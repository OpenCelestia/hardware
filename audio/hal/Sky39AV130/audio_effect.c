// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/audio.
 *
 * Audio effect implementation for AnyCloud39AV100_V2 platform.
 * - LoadConf section selection matches AnyCloud39AV200 (NORMAL / CALL / DMIC).
 * - Without ak_audio_effect.h, VAD/cry use ak_ai_* on a single handle.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include "pthread.h"

#include "../../audio_hw.h"
#include "../../hal_audio_api.h"
#include "hal_audio_config.h"
#include "audio.h"

#include "ak_common.h"
#include "ak_audio_compat.h"
#if defined(__has_include)
#if __has_include("ak_audio_effect.h")
#define HAL_HAS_AK_AUDIO_EFFECT 1
#else
#define HAL_HAS_AK_AUDIO_EFFECT 0
#endif
#else
#define HAL_HAS_AK_AUDIO_EFFECT 1
#endif
#if HAL_HAS_AK_AUDIO_EFFECT
#include "ak_audio_effect.h"
#include "effect_aiCryDetect.h"
#include "effect_Res.h"
#include "effect_Vad.h"
#include "effect_Cry.h"
#include "ak_npu.h"
#include "effect_LineArraySsl.h"
#endif

#include "hal_audio_config.h"
#include "hal_audio.h"
#include "log.h"
#define LOG_TAG "HAL AUDIO"

#if !HAL_HAS_AK_AUDIO_EFFECT
/*
 * ak_ai_set_vad_attr / ak_ai_get_vad_status_ex (ak_vad_sample.c),
 * ak_ai_set_vad_npu_attr + ak_AIcry_register (ak_baby_screaming_sample.c).
 * Single ai_handle_id — no AK_MPI multi-channel path.
 */
#include "ak_reg_vad.h"
#include "ak_reg_AIcry.h"
#include "ak_npu.h"

#define AK32Q15(x)        ((signed long)((x) * (1 << 15)))
#define DEFAULT_CRY_MODEL "/etc/config/model_aicrydet_V1.00.02.bin"
/* cry_mode 33 = NPU (0x21), 17 = CPU SW (0x11) — ak_baby_screaming_sample.c */
#define CRY_MODE_NPU      AK_VAD_TYPE_PARAM(AK_VAD_DET_AFTER_NR, AK_VAD_DET_BABY_SCREAM_AI, 0, 0)
#define CRY_MODE_CPU      AK_VAD_TYPE_PARAM(AK_VAD_DET_AFTER_NR, AK_VAD_DET_BABY_SCREAM, 0, 0)

struct effect_ctx {
    audio_effect_type_t audio_effect_type_flag;
    audio_effect_type_t pending_deinit_effect_flag;
    volatile int poll_thread_run;
    pthread_t poll_thread_id;
    bool vad_registered;
    bool npu_cry_registered;
    bool npu_inited;
    int last_vad_trigger;
    unsigned long long last_vad_ts;
    unsigned long long last_npu_cry_ts;
    unsigned long long last_cpu_cry_ts;
    audio_notify_msg_t msg_vad;
    audio_notify_msg_t msg_npu_cry;
    audio_notify_msg_t msg_cpu_cry;
};

static struct effect_ctx audio_effect;

static int ai_handle(struct hal_audio_data *my_data)
{
    return my_data ? my_data->ai_handle_id : -1;
}

static int effect_vad_enable(int ai_handle_id)
{
    struct ak_audio_vad_attr vad_attr;

    memset(&vad_attr, 0, sizeof(vad_attr));
    vad_attr.enable = 1;
    vad_attr.vad_thresh = AK32Q15(0.05);
    vad_attr.vad_level = 0;
    vad_attr.vad_type = AK_VAD_TYPE_PARAM(AK_VAD_DET_AFTER_ADC, AK_VAD_DET_SOUND_LEVEL, 0, 0);

    if (!audio_effect.vad_registered) {
        ak_vad_register();
        audio_effect.vad_registered = true;
    }
    return ak_ai_set_vad_attr(ai_handle_id, &vad_attr);
}

static int effect_vad_disable(int ai_handle_id)
{
    struct ak_audio_vad_attr vad_attr;

    memset(&vad_attr, 0, sizeof(vad_attr));
    vad_attr.enable = 0;
    vad_attr.vad_type = AK_VAD_TYPE_PARAM(AK_VAD_DET_AFTER_ADC, AK_VAD_DET_SOUND_LEVEL, 0, 0);
    return ak_ai_set_vad_attr(ai_handle_id, &vad_attr);
}

static int effect_cpu_cry_enable(int ai_handle_id)
{
    struct ak_audio_vad_attr vad_attr;

    memset(&vad_attr, 0, sizeof(vad_attr));
    vad_attr.enable = 1;
    vad_attr.vad_level = 0;
    vad_attr.vad_thresh = AK32Q15(0.05);
    vad_attr.vad_type = CRY_MODE_CPU;
    return ak_ai_set_vad_attr(ai_handle_id, &vad_attr);
}

static int effect_cpu_cry_disable(int ai_handle_id)
{
    struct ak_audio_vad_attr vad_attr;

    memset(&vad_attr, 0, sizeof(vad_attr));
    vad_attr.enable = 0;
    vad_attr.vad_type = CRY_MODE_CPU;
    return ak_ai_set_vad_attr(ai_handle_id, &vad_attr);
}

static int effect_poll_sleep_ms(const struct stream_in *in)
{
    int interval = in ? in->in_frame_interval : 0;

    if (interval <= 0)
        return 16;
    return interval / 2 > 0 ? interval / 2 : 16;
}

static int effect_npu_cry_enable(int ai_handle_id, struct stream_in *in)
{
    struct ak_audio_vad_npu_attr vad_npu_attr;
    const char *model = DEFAULT_CRY_MODEL;

    if (in->sample_rate != 16000) {
        log_warn("NPU cry prefers 16 kHz (current %d), see ak_baby_screaming_sample", in->sample_rate);
    }

    if (!audio_effect.npu_inited) {
        if (ak_npu_init()) {
            log_error("ak_npu_init failed");
            return AK_FAILED;
        }
        audio_effect.npu_inited = true;
    }
    if (!audio_effect.npu_cry_registered) {
        /* ak_baby_screaming_sample: register before ak_ai_set_vad_npu_attr */
        ak_AIcry_register();
        audio_effect.npu_cry_registered = true;
    }

    memset(&vad_npu_attr, 0, sizeof(vad_npu_attr));
    vad_npu_attr.vad_attr.enable = 1;
    vad_npu_attr.vad_attr.vad_level = 0;
    vad_npu_attr.vad_attr.vad_thresh = 0; /* NPU path: thresh must be 0 per sample */
    vad_npu_attr.vad_attr.vad_type = CRY_MODE_NPU;
    vad_npu_attr.model_name = (char *)model;

    return ak_ai_set_vad_npu_attr(ai_handle_id, &vad_npu_attr);
}

static int effect_npu_cry_disable(int ai_handle_id)
{
    struct ak_audio_vad_npu_attr vad_npu_attr;
    const char *model = DEFAULT_CRY_MODEL;

    memset(&vad_npu_attr, 0, sizeof(vad_npu_attr));
    vad_npu_attr.vad_attr.enable = 0;
    vad_npu_attr.vad_attr.vad_type = CRY_MODE_NPU;
    vad_npu_attr.model_name = (char *)model;
    return ak_ai_set_vad_npu_attr(ai_handle_id, &vad_npu_attr);
}

static void npu_cry_deinit_if_unused(struct hal_audio_data *my_data)
{
    if (!audio_effect.npu_inited)
        return;
    if (my_data->in_effect & AUDIO_EFFECT_NPU_CRY)
        return;
    if (audio_effect.audio_effect_type_flag & AUDIO_EFFECT_NPU_CRY)
        return;

    ak_npu_deinit();
    audio_effect.npu_inited = false;
    audio_effect.npu_cry_registered = false;
}

static void effect_notify_vad(struct stream_in *in, struct ak_audio_vad_status_ex *st,
                                    audio_effect_type_t effect_type, audio_notify_msg_t *msg)
{
    const audio_callback_ops_t *ops = in->dev->callback_ops;

    if (!ops || !ops->notify || st->trigger == 0)
        return;

    msg->type = effect_type;
    msg->detect.value = 1;
    ops->notify(ops, msg);
}

static void *effect_poll_thread(void *arg)
{
    struct stream_in *in = (struct stream_in *)arg;
    struct hal_audio_data *my_data = in->dev->hal_audio;
    int ai_handle_id = ai_handle(my_data);
    struct ak_audio_vad_status_ex vad_st;

    int poll_ms = effect_poll_sleep_ms(in);

    log_info("effect poll thread enter (poll_ms=%d)", poll_ms);
    while (audio_effect.poll_thread_run) {
        if (ai_handle_id < 0) {
            ak_sleep_ms(poll_ms);
            continue;
        }

        if (audio_effect.audio_effect_type_flag & AUDIO_EFFECT_VAD) {
            memset(&vad_st, 0, sizeof(vad_st));
            vad_st.vadType = AK_VAD_TYPE_PARAM(AK_VAD_DET_AFTER_ADC, AK_VAD_DET_SOUND_LEVEL, 0, 0);
            if (!ak_ai_get_vad_status_ex(ai_handle_id, &vad_st)) {
                if (vad_st.ts != audio_effect.last_vad_ts || vad_st.trigger != audio_effect.last_vad_trigger) {
                    audio_effect.last_vad_ts = vad_st.ts;
                    audio_effect.last_vad_trigger = vad_st.trigger;
effect_notify_vad(in, &vad_st, AUDIO_EFFECT_VAD, &audio_effect.msg_vad);
                }
            }
        }

        if (audio_effect.audio_effect_type_flag & AUDIO_EFFECT_NPU_CRY) {
            memset(&vad_st, 0, sizeof(vad_st));
            vad_st.vadType = CRY_MODE_NPU;
            if (!ak_ai_get_vad_status_ex(ai_handle_id, &vad_st) &&
                vad_st.ts != audio_effect.last_npu_cry_ts) {
                audio_effect.last_npu_cry_ts = vad_st.ts;
                if (vad_st.trigger)
effect_notify_vad(in, &vad_st, AUDIO_EFFECT_NPU_CRY, &audio_effect.msg_npu_cry);
            }
        }

        if (audio_effect.audio_effect_type_flag & AUDIO_EFFECT_CPU_CRY) {
            memset(&vad_st, 0, sizeof(vad_st));
            vad_st.vadType = CRY_MODE_CPU;
            if (!ak_ai_get_vad_status_ex(ai_handle_id, &vad_st) &&
                vad_st.ts != audio_effect.last_cpu_cry_ts) {
                audio_effect.last_cpu_cry_ts = vad_st.ts;
                if (vad_st.trigger)
effect_notify_vad(in, &vad_st, AUDIO_EFFECT_CPU_CRY, &audio_effect.msg_cpu_cry);
            }
        }

        ak_sleep_ms(poll_ms);
    }

    log_info("effect poll thread exit");
    return NULL;
}

static void effect_poll_thread_start(struct stream_in *in)
{
    if (audio_effect.poll_thread_id != 0)
        return;

    audio_effect.poll_thread_run = 1;
    if (pthread_create(&audio_effect.poll_thread_id, NULL, effect_poll_thread, in) != 0) {
        audio_effect.poll_thread_run = 0;
        audio_effect.poll_thread_id = 0;
        log_error("effect poll thread create failed");
    }
}

static void effect_poll_thread_stop(void)
{
    if (audio_effect.poll_thread_id == 0)
        return;

    audio_effect.poll_thread_run = 0;
    pthread_join(audio_effect.poll_thread_id, NULL);
    audio_effect.poll_thread_id = 0;
}

static int effect_apply_algo(audio_effect_type_t effect, struct stream_in *in, int enable)
{
    struct hal_audio_data *my_data = in->dev->hal_audio;
    int ai_handle_id = ai_handle(my_data);
    int ret = AK_SUCCESS;

    if (ai_handle_id < 0) {
        log_error("effect_apply_algo: ai not open");
        return AK_FAILED;
    }

    if (effect & AUDIO_EFFECT_VAD) {
        ret = enable ? effect_vad_enable(ai_handle_id) : effect_vad_disable(ai_handle_id);
        if (ret) {
            log_error("VAD %s failed", enable ? "enable" : "disable");
            return ret;
        }
    }

    if (effect & AUDIO_EFFECT_CPU_CRY) {
        ret = enable ? effect_cpu_cry_enable(ai_handle_id)
                     : effect_cpu_cry_disable(ai_handle_id);
        if (ret) {
            log_error("CPU cry %s failed", enable ? "enable" : "disable");
            return ret;
        }
    }

    if (effect & AUDIO_EFFECT_NPU_CRY) {
        ret = enable ? effect_npu_cry_enable(ai_handle_id, in)
                     : effect_npu_cry_disable(ai_handle_id);
        if (ret) {
            log_error("NPU cry %s failed", enable ? "enable" : "disable");
            return ret;
        }
    }

    if (effect & AUDIO_EFFECT_LSSL) {
        log_warn("LSSL not supported (no platform sample); ignored");
    }

    return AK_SUCCESS;
}

static void check_effect_init(audio_effect_type_t effect, struct stream_in *in)
{
    const int algo_mask = (int)(AUDIO_EFFECT_VAD | AUDIO_EFFECT_NPU_CRY |
                                AUDIO_EFFECT_CPU_CRY | AUDIO_EFFECT_LSSL);
    int want = effect & algo_mask;
    int active = audio_effect.audio_effect_type_flag;
    int to_add = want & ~active;
    int to_del = active & ~want;
    struct hal_audio_data *my_data = in->dev->hal_audio;

    if (my_data->recording_flag || my_data->playing_flag) {
        log_info("effect_init skip Filter_Control (echo thread running rec=%d play=%d)",
                 (int)my_data->recording_flag, (int)my_data->playing_flag);
        if (my_data->ai_handle_id >= 0 && in->status == AUDIO_STATUS_ENABLED &&
            in->dev->callback_ops != NULL)
            audio_hw_stream_in_ensure_capture_started(&in->stream);
        return;
    }

    if (to_del) {
        (void)effect_apply_algo(to_del, in, 0);
        active &= ~to_del;
    }
    if (to_add) {
        if (effect_apply_algo(to_add, in, 1) == AK_SUCCESS)
            active |= to_add;
        else
            log_warn("effect enable failed for mask 0x%x", to_add);
    }

    audio_effect.audio_effect_type_flag = active;
npu_cry_deinit_if_unused(my_data);

    if (want & AUDIO_EFFECT_LSSL)
        log_warn("LSSL not supported; ignored");

    if (active & (AUDIO_EFFECT_VAD | AUDIO_EFFECT_NPU_CRY | AUDIO_EFFECT_CPU_CRY))
effect_poll_thread_start(in);
    else
effect_poll_thread_stop();

    if (my_data->ai_handle_id >= 0 && in->status == AUDIO_STATUS_ENABLED &&
        in->dev->callback_ops != NULL)
        audio_hw_stream_in_ensure_capture_started(&in->stream);
}

void audio_effect_out_process(audio_effect_type_t effect, void *buffer, size_t bytes, uint32_t sample_rate)
{
    (void)effect;
    (void)buffer;
    (void)bytes;
    (void)sample_rate;
}

int hal_audio_effect_out_add(void *hal_audio, audio_effect_type_t effect)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    my_data->out_effect |= effect;
    return AK_SUCCESS;
}

int hal_audio_effect_out_remove(void *hal_audio, audio_effect_type_t effect)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    my_data->out_effect &= ~effect;
    return AK_SUCCESS;
}

int hal_audio_effect_in_add(void *hal_audio, audio_effect_type_t effect)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    struct audio_device *adev = my_data->adev;
    struct stream_in *in = adev->in;

    if (!in || my_data->ai_handle_id < 0) {
        log_error("hal_audio_effect_in_add: input not open");
        return AK_FAILED;
    }

    my_data->in_effect |= effect;
check_effect_init(my_data->in_effect, in);
    log_info("hal_audio_effect_in_add: effect=0x%x active=0x%x", effect, my_data->in_effect);
    return AK_SUCCESS;
}

int hal_audio_effect_in_remove(void *hal_audio, audio_effect_type_t effect)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    struct audio_device *adev = my_data->adev;
    struct stream_in *in = adev->in;
    audio_effect_type_t active = audio_effect.audio_effect_type_flag & effect;

    if (!in)
        return AK_SUCCESS;

    if (my_data->recording_flag && active) {
        audio_effect.pending_deinit_effect_flag |= active;
        my_data->in_effect &= ~effect;
        (void)effect_apply_algo(active, in, 0);
        audio_effect.audio_effect_type_flag &= ~active;
        if (!(audio_effect.audio_effect_type_flag &
              (AUDIO_EFFECT_VAD | AUDIO_EFFECT_NPU_CRY | AUDIO_EFFECT_CPU_CRY)))
effect_poll_thread_stop();
        if (audio_effect.npu_inited && !(my_data->in_effect & AUDIO_EFFECT_NPU_CRY)) {
            ak_npu_deinit();
            audio_effect.npu_inited = false;
            audio_effect.npu_cry_registered = false;
        }
        return AK_SUCCESS;
    }

    my_data->in_effect &= ~effect;
check_effect_init(my_data->in_effect, in);
    return AK_SUCCESS;
}

int hal_audio_effect_in_process(void *hal_audio, void *buffer, size_t bytes)
{
    (void)hal_audio;
    (void)buffer;
    (void)bytes;
    return AK_SUCCESS;
}

int hal_audio_effect_out_process(void *hal_audio, void *buffer, size_t bytes)
{
    (void)hal_audio;
    (void)buffer;
    (void)bytes;
    return AK_SUCCESS;
}

int hal_audio_effect_in_loadconf(void *hal_audio)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    struct audio_device *adev;
    struct stream_in *in;
    hal_audio_config_t *cfg;
    const char *section;
    int ret;

    if (!my_data || !my_data->adev)
        return 0;

    adev = my_data->adev;
    in = adev->in;
    if (!in)
        return 0;

    if (adev->mode == my_data->in_mode)
        return 0;

    /* LoadConf enters Filter_Control; never after start_capture / ao_open. */
    if (my_data->recording_flag) {
        log_info("AI: skip LoadConf (capture running, mode=%d)", (int)adev->mode);
        return 0;
    }
    if (my_data->playing_flag) {
        log_info("AI: skip LoadConf (AO Echo running, mode=%d)", (int)adev->mode);
        return 0;
    }

    if (my_data->ai_handle_id < 0)
        return 0;

    cfg = my_data->cfg ? my_data->cfg : hal_audio_get_config();

    /*
     * AEC LoadConf [talk_near] is CALL-only. Live uses nr/agc in
     * hal_audio_input_open and must not load echo / live_near (aecEna).
     * DMIC → in_dmic_section (no AEC).
     */
    if (in->devices == AUDIO_DEVICE_IN_DMIC)
        section = cfg->loadconf_in_dmic_section;
    else if (adev->mode == AUDIO_MODE_CALL)
        section = cfg->loadconf_echo_section;
    else {
        log_info("AI: skip LoadConf (non-CALL, no AEC)");
        my_data->in_mode = adev->mode;
        return 0;
    }

    log_info("AI: LoadConf section=%s (mode=%d, before capture)",
             section, (int)adev->mode);
#if !defined(V2_04)
    ret = ak_ai_load_ch_conf(my_data->ai_handle_id, cfg->loadconf_file_path,
                             (char *)section);
#else
    /* SDK V2.04 has no ak_ai_load_ch_conf; keep the pre-LoadConf behavior. */
    ret = 0;
    log_info("AI: LoadConf unsupported on this SDK (path=%s), skip",
             cfg->loadconf_file_path);
#endif
    if (ret) {
        log_warn("ak_ai_load_ch_conf failed section=%s ret=%d", section, ret);
        return ret;
    }
    log_info("Audio input config loaded: path=%s, section=%s",
             cfg->loadconf_file_path, section);

    my_data->in_mode = adev->mode;
    return 0;
}

int hal_audio_effect_out_loadconf(void *hal_audio)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    struct audio_device *adev;
    hal_audio_config_t *cfg;
    int ret;

    if (!my_data || !my_data->adev)
        return 0;

    adev = my_data->adev;
    if (adev->mode == my_data->out_mode)
        return 0;

    if (my_data->playing_flag) {
        log_info("AO: skip LoadConf (playback running, mode=%d)", (int)adev->mode);
        return 0;
    }

    cfg = my_data->cfg ? my_data->cfg : hal_audio_get_config();
#if !defined(V2_04)
    if (my_data->ao_handle_id >= 0) {
        log_info("AO: LoadConf section=%s (mode=%d)",
                 cfg->loadconf_out_normal_section, (int)adev->mode);
        ret = ak_ao_load_ch_conf(my_data->ao_handle_id, cfg->loadconf_file_path,
                                 cfg->loadconf_out_normal_section);
    } else if (my_data->out_device_id >= 0) {
        log_info("AO: LoadConf(dev) section=%s (mode=%d)",
                 cfg->loadconf_out_normal_section, (int)adev->mode);
        ret = ak_ao_load_dev_conf(my_data->out_device_id, cfg->loadconf_file_path,
                                  cfg->loadconf_out_normal_section);
    } else {
        return 0;
    }
#else
    /* SDK V2.04 has no ak_ao_load_ch_conf / ak_ao_load_dev_conf. */
    ret = 0;
    log_info("AO: LoadConf unsupported on this SDK (section=%s), skip",
             cfg->loadconf_out_normal_section);
#endif

    if (ret)
        log_warn("ak_ao_load_*_conf failed section=%s ret=%d",
                 cfg->loadconf_out_normal_section, ret);

    my_data->out_mode = adev->mode;
    return 0;
}

#else

#define AUDIO_EFFECT_RESAMPLE_ID 0
#define AUDIO_EFFECT_CRY_ID 5
#define AUDIO_EFFECT_VAD_ID 3
#define AUDIO_EFFECT_LSSL_ID 2

#define CPU_CRY_THRESH_MIN 0
#define CPU_CRY_THRESH_MAX 32768
#define CPU_CRY_LEVEL_MIN 0
#define CPU_CRY_LEVEL_MAX 4

#define VAD_THRESH_MIN 0
#define VAD_THRESH_MAX 32768
#define VAD_LEVEL_MIN 0
#define VAD_LEVEL_MAX 4
#define EFFECT_AI_GETFRAME_TIMEOUT_MS 200

/*
 * AI sub-channel layout (hal_audio.h audio_ai_type_t):
 *   MAIN=0 (enabled in hal_audio_input_open), VAD=1, NPU_CRY=2, LSSL=3, CPU_CRY=4.
 * Per-effect AI path: hal_audio_effect_algorithm_ai_init() (LoadConf + EnableChn), then
 * hal_audio_effect_algorithm_effects_init(). Capture thread starts after add_audio_effect (audio_service).
 */

static bool s_ai_ch_vad_ready;
static bool s_ai_ch_npu_cry_ready;
static bool s_ai_ch_cpu_cry_ready;
static bool s_ai_ch_lssl_ready;

static void* audio_process_cpu_cry_effect_thread_function(void* arg) ;
static void* audio_process_npu_cry_effect_thread_function(void* arg) ;
static void* audio_process_vad_effect_thread_function(void* arg) ;
static void* audio_process_lssl_effect_thread_function(void* arg) ;

struct audio_effect_data {
    AUDIO_EFFECT_RESULT_S stVadResult;
    AUDIO_EFFECT_RESULT_S stCryResult;
    AUDIO_EFFECT_RESULT_S stAiCryResult;
    AUDIO_EFFECT_RESULT_S stResResult;
    AUDIO_EFFECT_RESULT_S stLsslResult;
    audio_effect_type_t audio_effect_type_flag;
    bool resample_for_audio_detect_flag;
    bool resample_for_audio_detect_inited;
    audio_notify_msg_t msg_npu_cry;
    audio_notify_msg_t msg_cpu_cry;
    audio_notify_msg_t msg_vad;
    audio_notify_msg_t msg_lssl;

    pthread_t in_cpu_cry_effect_thread_id;
    pthread_t in_vad_effect_thread_id;
    pthread_t in_npu_cry_effect_thread_id;
    pthread_t in_lssl_effect_thread_id;

    struct audio_config resample_config;
    audio_effect_type_t pending_deinit_effect_flag;
};
    
struct audio_effect_data audio_effect =
{
  .stVadResult =  {.u32EffectType = AK_AUDIO_VAD, .u32UnionBufSize = sizeof(AUDIO_VAD_RESULT_S)},
  .stAiCryResult = {.u32EffectType = AK_AUDIO_AI_CRY, .u32UnionBufSize = sizeof(AUDIO_AICRY_RESULT_S)},
  .stCryResult = {.u32EffectType = AK_AUDIO_CRY, .u32UnionBufSize = sizeof(AUDIO_CRY_RESULT_S)},
  .stResResult = {.u32EffectType = AK_AUDIO_RESAMPLE, .u32UnionBufSize = sizeof(AUDIO_EFFECT_DATA_PROCESS_RESULT_S)},
  .stLsslResult = {.u32EffectType = AK_AUDIO_LSSL, .u32UnionBufSize = sizeof(AUDIO_SSL_RESULT_S)},
  .audio_effect_type_flag = AUDIO_EFFECT_NONE,
  .resample_for_audio_detect_flag = false,
  .resample_for_audio_detect_inited = false,
  .pending_deinit_effect_flag = AUDIO_EFFECT_NONE
};

static int effect_getframe_timeout_ms(struct hal_audio_data *my_data)
{
    int timeout_ms = EFFECT_AI_GETFRAME_TIMEOUT_MS;
    if (my_data && my_data->cfg && my_data->cfg->receive_frame_timeout_ms > 0 &&
        my_data->cfg->receive_frame_timeout_ms < timeout_ms) {
        timeout_ms = my_data->cfg->receive_frame_timeout_ms;
    }
    return timeout_ms;
}

/**
 * @brief Initialize NPU-based cry detection effect.
 * @param in Input stream pointer.
 * @return AK_SUCCESS on success, AK_FAILED on failure.
 */
static int  audio_effect_npu_cry_init ( struct stream_in *in )
{
    AK_S32 s32Ret = AK_SUCCESS;
    struct audio_device *adev = in->dev;
    struct hal_audio_data *my_data = adev->hal_audio;

    AUDIO_DATA_ATTR_S   stAudioDataAttrCry = {in->sample_rate, AK_AUDIO_SMPLE_BIT_16, AUDIO_SOUND_MODE_MONO};
    AUDIO_EFFECT_REG_S      stAudioEffectAiCry = {  AK_AUDIO_AI_CRY, Effect_AICRYDET_Open, Effect_AICRYDET_Close,
                                                    Effect_AICRYDET_Process, Effect_AICRYDET_SetAttr };
     log_info("audio_effect_npu_cry_init 1");
    /* NPU_Init + AI ch2 LoadConf + EnableChn: hal_audio_effect_algorithm_ai_init() */

    s32Ret = AK_MPI_AUDIO_EFFECT_Register(&stAudioEffectAiCry);
    if (s32Ret) {
        log_error("ERROR: AK_MPI_AUDIO_EFFECT_Register AiCry s32Ret = %d", s32Ret);
        return s32Ret;
    }
    log_info("audio_effect_npu_cry_init 3");

    s32Ret = AK_MPI_AUDIO_EFFECT_Init(
        AUDIO_EFFECT_CRY_ID,
        &stAudioDataAttrCry,
        AK_AUDIO_AI_CRY,
        my_data->cfg->loadconf_file_path,
        my_data->cfg->effect_npu_cry_section);
        log_info("audio_effect_npu_cry_init 4");

    if (s32Ret) {
        log_error("ERROR: AK_MPI_AUDIO_EFFECT_Init AiCry s32Ret = %d", s32Ret);
        return s32Ret;
    }
    log_info("audio_effect_npu_cry_init 5");

    log_info("AUDIO_EFFECT_NPU_CRY init, type:%d, sizeof:%d",AK_AUDIO_AI_CRY,audio_effect.stCryResult.u32UnionBufSize);


    return s32Ret;
}

/**
 * @brief Deinitialize NPU-based cry detection effect.
 * @param in Input stream pointer.
 * @return 0 on success, error code on failure.
 */
static int audio_effect_npu_cry_deinit(  struct stream_in *in  )
{
    struct audio_device *adev = in->dev;
    struct hal_audio_data *my_data = adev->hal_audio;

    if (my_data->in_device_id >= 0) {
        AK_MPI_AI_DisableChn(my_data->in_device_id, AUDIO_IN_CHANNEL_NPU_CRY_ID);
    }
    s_ai_ch_npu_cry_ready = false;

    if (audio_effect.in_npu_cry_effect_thread_id != 0) {
        pthread_join(audio_effect.in_npu_cry_effect_thread_id, NULL);
        audio_effect.in_npu_cry_effect_thread_id = 0;
    }

    int  ret = AK_MPI_AUDIO_EFFECT_DeInit(AUDIO_EFFECT_CRY_ID);
    int npu_ret = AK_MPI_NPU_Deinit();
    log_info("AK_MPI_NPU_Deinit ret=%d", npu_ret);

    log_info("AUDIO_EFFECT_NPU_CRY deinit");

    return ret;
}

/**
 * @brief Initialize CPU-based cry detection effect.
 * @param in Input stream pointer.
 * @return AK_SUCCESS on success, error code on failure.
 */
static int  audio_effect_cpu_cry_init (struct stream_in *in)
{
    AK_S32 s32Ret = AK_SUCCESS;
    struct audio_device *adev = in->dev;
    struct hal_audio_data *my_data = adev->hal_audio;

    AUDIO_DATA_ATTR_S   stAudioDataAttrCry = {in->sample_rate, AK_AUDIO_SMPLE_BIT_16, AUDIO_SOUND_MODE_MONO};

    AUDIO_EFFECT_REG_S     stAudioEffectCry = {AK_AUDIO_CRY, Effect_CRY_Open,
                                Effect_CRY_Close, Effect_CRY_Process, Effect_CRY_SetAttr};

    s32Ret = AK_MPI_AUDIO_EFFECT_Register(&stAudioEffectCry);
    if (s32Ret) {
        log_error( "ERROR: AK_MPI_AUDIO_EFFECT_Register Cry s32Ret = %d", s32Ret);
        return s32Ret;
    }

    s32Ret = AK_MPI_AUDIO_EFFECT_Init(
        AUDIO_EFFECT_CRY_ID,
        &stAudioDataAttrCry,
        AK_AUDIO_CRY,
        my_data->cfg->loadconf_file_path,
        my_data->cfg->effect_cpu_cry_section);

    if (s32Ret) {
        log_error("ERROR: AK_MPI_AUDIO_EFFECT_Init Cry s32Ret = %d", s32Ret);
        return s32Ret;
    }

    log_info("AUDIO_EFFECT_CPU_CRY init, type:%d, sizeof:%d",AK_AUDIO_CRY,audio_effect.stCryResult.u32UnionBufSize);

    return s32Ret;
}

/**
 * @brief Deinitialize CPU-based cry detection effect.
 * @param in Input stream pointer.
 * @return 0 on success, error code on failure.
 */
static int audio_effect_cpu_cry_deinit(  struct stream_in *in  )
{
    struct audio_device *adev = in->dev;
    struct hal_audio_data *my_data = adev->hal_audio;

    if (my_data->in_device_id >= 0) {
        AK_MPI_AI_DisableChn(my_data->in_device_id, AUDIO_IN_CHANNEL_CPU_CRY_ID);
    }
    s_ai_ch_cpu_cry_ready = false;

    if (audio_effect.in_cpu_cry_effect_thread_id != 0) {
        pthread_join(audio_effect.in_cpu_cry_effect_thread_id, NULL);
        audio_effect.in_cpu_cry_effect_thread_id = 0;
    }

    int  ret = AK_MPI_AUDIO_EFFECT_DeInit(AUDIO_EFFECT_CRY_ID);
    log_info("AUDIO_EFFECT_CPU_CRY deinit");
    return ret;
}

/**
 * @brief Initialize VAD (Voice Activity Detection) effect.
 * @param in Input stream pointer.
 * @return AK_SUCCESS on success, error code on failure.
 */
static int  audio_effect_vad_init ( struct stream_in *in)
{
    AK_S32 s32Ret = AK_SUCCESS;
    struct audio_device *adev = in->dev;
    struct hal_audio_data *my_data = adev->hal_audio;

    AUDIO_DATA_ATTR_S	stAudioDataAttrVad = {in->sample_rate, AK_AUDIO_SMPLE_BIT_16, AUDIO_SOUND_MODE_MONO};
    AUDIO_EFFECT_REG_S  stAudioEffectVad = {AK_AUDIO_VAD, Effect_VAD_Open, Effect_VAD_Close,
                                            Effect_VAD_Process, Effect_VAD_SetAttr};

    s32Ret = AK_MPI_AUDIO_EFFECT_Register(&stAudioEffectVad);
    if (s32Ret) {
       log_error("ERROR: AK_MPI_AUDIO_EFFECT_Register Vad s32Ret = %d", s32Ret);
       return s32Ret;
    }

    s32Ret = AK_MPI_AUDIO_EFFECT_Init(
        AUDIO_EFFECT_VAD_ID,
        &stAudioDataAttrVad,
        AK_AUDIO_VAD,
        my_data->cfg->loadconf_file_path,
        my_data->cfg->effect_vad_section);
    log_info("audio_effect_vad_init 4");
    if (s32Ret) {
        log_error("AK_MPI_AUDIO_EFFECT_Init Vad s32Ret = %d", s32Ret);
        return s32Ret;
    }

    log_info("AUDIO_EFFECT_VAD init,effect type: %d,size:%d",AK_AUDIO_VAD,audio_effect.stVadResult.u32UnionBufSize);

    return AK_SUCCESS;
}

/**
 * @brief Deinitialize VAD effect.
 * @param in Input stream pointer.
 * @return 0 on success, error code on failure.
 */
static int audio_effect_vad_deinit(  struct stream_in *in  )
{
    struct audio_device *adev = in->dev;
    struct hal_audio_data *my_data = adev->hal_audio;

    if (my_data->in_device_id >= 0) {
        AK_MPI_AI_DisableChn(my_data->in_device_id, AUDIO_IN_CHANNEL_VAD_ID);
    }
    s_ai_ch_vad_ready = false;

    if (audio_effect.in_vad_effect_thread_id != 0) {
        pthread_join(audio_effect.in_vad_effect_thread_id, NULL);
        audio_effect.in_vad_effect_thread_id = 0;
    }

    AK_S32 ret = AK_MPI_AUDIO_EFFECT_DeInit(AUDIO_EFFECT_VAD_ID);

    log_info("AUDIO_EFFECT_VAD deinit");
    return ret;
}

/**
 * @brief Initialize LSSL (Linear Sound Source Localization) effect.
 * @param in Input stream pointer.
 * @return AK_SUCCESS on success, error code on failure.
 */
static int  audio_effect_lssl_init ( struct stream_in *in)
{
    struct audio_device *adev = in->dev;
    struct hal_audio_data *my_data = adev->hal_audio;
    AK_S32 s32Ret = AK_SUCCESS;

    AUDIO_DATA_ATTR_S       stAudioDataAttrSsl = {in->sample_rate, AK_AUDIO_SMPLE_BIT_16, AUDIO_SOUND_MODE_MONO};
    AUDIO_EFFECT_REG_S      stAudioEffectSsl = {AK_AUDIO_LSSL, Effect_LineArraySSL_Open, \
    Effect_LineArraySSL_Close, Effect_LineArraySSL_Process, Effect_LineArraySSL_SetAttr};
    log_info("audio_effect_lssl_init 1");

    s32Ret = AK_MPI_AUDIO_EFFECT_Register(&stAudioEffectSsl);
    if(s32Ret){
        log_error( "ERROR: AK_MPI_AUDIO_EFFECT_Register Ssl s32Ret = %d", s32Ret);
        return s32Ret;
    }
    log_info("audio_effect_lssl_init 3");

    s32Ret = AK_MPI_AUDIO_EFFECT_Init(
        AUDIO_EFFECT_LSSL_ID,
        &stAudioDataAttrSsl,
        AK_AUDIO_LSSL,
        my_data->cfg->loadconf_file_path,
        my_data->cfg->effect_lssl_section );
    log_info("audio_effect_lssl_init 5");

    if(s32Ret){
        log_error(" AK_MPI_AUDIO_EFFECT_Init lssl s32Ret = %d", s32Ret);
        return s32Ret;
    }


    return AK_SUCCESS;
}

/**
 * @brief Deinitialize LSSL effect.
 * @param in Input stream pointer.
 * @return 0 on success, error code on failure.
 */
static int audio_effect_lssl_deinit( struct stream_in *in )
{
    struct audio_device *adev = in->dev;
    struct hal_audio_data *my_data = adev->hal_audio;

    if (my_data->in_device_id >= 0) {
        AK_MPI_AI_DisableChn(my_data->in_device_id, AUDIO_IN_CHANNEL_LSSL_ID);
    }
    s_ai_ch_lssl_ready = false;

    if (audio_effect.in_lssl_effect_thread_id != 0) {
        pthread_join(audio_effect.in_lssl_effect_thread_id, NULL);
        audio_effect.in_lssl_effect_thread_id = 0;
    }

    AK_S32 s32Ret = AK_MPI_AUDIO_EFFECT_DeInit(AUDIO_EFFECT_LSSL_ID);
    if (s32Ret) {
        log_error( "AK_MPI_AUDIO_EFFECT_DeInit, lssl s32Ret = %d", s32Ret);
    }

    return s32Ret;
}

/**
 * @brief Initialize audio resample effect.
 * @param in Input stream pointer.
 * @param enSampleRateOut Target sample rate for resampling.
 * @return AK_SUCCESS on success, error code on failure.
 */
static int  audio_effect_resample_init ( struct stream_in *in , int enSampleRateOut)
{
    AK_S32 s32Ret = AK_SUCCESS;
    struct audio_device *adev = in->dev;
    struct hal_audio_data *my_data = adev->hal_audio;
    AUDIO_EFFECT_REG_S stAudioEffectRes = {
        AK_AUDIO_RESAMPLE,
        Effect_RES_Open,
        Effect_RES_Close,
        Effect_RES_Process,
        Effect_RES_SetAttr
    };

    AUDIO_DATA_ATTR_S	stAudioDataAttrRes = {in->sample_rate, AK_AUDIO_SMPLE_BIT_16,	AUDIO_SOUND_MODE_MONO};

    s32Ret = AK_MPI_AUDIO_EFFECT_Register(&stAudioEffectRes);
    if (s32Ret) {
        log_error( "ERROR: AK_MPI_AUDIO_EFFECT_Register Resample s32Ret = %d", s32Ret);
        return s32Ret;
    }

    s32Ret = AK_MPI_AUDIO_EFFECT_Init(
        AUDIO_EFFECT_RESAMPLE_ID,
        &stAudioDataAttrRes,
        AK_AUDIO_RESAMPLE,
        my_data->cfg->loadconf_file_path,
        my_data->cfg->effect_resample_section);

    if (s32Ret) {
        log_error("ERROR: AK_MPI_AUDIO_EFFECT_Init Resample s32Ret = %d", s32Ret);
        return s32Ret;
    }

    return s32Ret;
}

static int audio_effect_resample_deinit(   struct stream_in *in   )
{
    AK_S32 ret = AK_MPI_AUDIO_EFFECT_DeInit(AUDIO_EFFECT_RESAMPLE_ID);

    return ret;
}

/**
 * Per-channel AK_MPI_AI_LoadConf then AK_MPI_AI_EnableChn.
 * Channel ids: VAD=1, NPU_CRY=2, LSSL=3, CPU_CRY=4 (MAIN=0 opened in hal_audio_input_open).
 * NPU_CRY: AK_MPI_NPU_Init() first, then LoadConf / EnableChn on channel 2.
 */
static int hal_audio_effect_algorithm_ai_init(struct stream_in *in, audio_effect_type_t effect)
{
    struct audio_device *adev = in->dev;
    struct hal_audio_data *my_data = adev->hal_audio;
    AK_S32 last_err = AK_SUCCESS;
    AK_S32 r;
    int effect_change = effect ^ audio_effect.audio_effect_type_flag;

    if ((effect_change & AUDIO_EFFECT_VAD) && (effect & AUDIO_EFFECT_VAD)) {
        s_ai_ch_vad_ready = false;
        r = AK_MPI_AI_LoadConf(my_data->in_device_id, AUDIO_IN_CHANNEL_VAD_ID,
                               my_data->cfg->loadconf_file_path, my_data->cfg->loadconf_in_vad_section);
        if (r) {
            log_error("AK_MPI_AI_LoadConf VAD ch%d failed, ret=%d", AUDIO_IN_CHANNEL_VAD_ID, r);
            last_err = r;
        } else {
            r = AK_MPI_AI_EnableChn(my_data->in_device_id, AUDIO_IN_CHANNEL_VAD_ID);
            if (r) {
                log_error("AK_MPI_AI_EnableChn VAD ch%d failed, ret=%d", AUDIO_IN_CHANNEL_VAD_ID, r);
                last_err = r;
            } else {
                s_ai_ch_vad_ready = true;
            }
        }
    }

    if ((effect_change & AUDIO_EFFECT_NPU_CRY) && (effect & AUDIO_EFFECT_NPU_CRY)) {
        s_ai_ch_npu_cry_ready = false;
        r = AK_MPI_NPU_Init();
        if (r) {
            log_warn("AK_MPI_NPU_Init failed (ret=%d), NPU_CRY AI path skipped", r);
            last_err = r;
        } else {
            log_info("AK_MPI_NPU_Init success");
            r = AK_MPI_AI_LoadConf(my_data->in_device_id, AUDIO_IN_CHANNEL_NPU_CRY_ID,
                                   my_data->cfg->loadconf_file_path,
                                   my_data->cfg->loadconf_in_npu_cry_section);
            if (r) {
                log_error("AK_MPI_AI_LoadConf NPU_CRY ch%d failed, ret=%d",
                          AUDIO_IN_CHANNEL_NPU_CRY_ID, r);
                last_err = r;
            } else {
                r = AK_MPI_AI_EnableChn(my_data->in_device_id, AUDIO_IN_CHANNEL_NPU_CRY_ID);
                if (r) {
                    log_error("AK_MPI_AI_EnableChn NPU_CRY ch%d failed, ret=%d",
                              AUDIO_IN_CHANNEL_NPU_CRY_ID, r);
                    last_err = r;
                } else {
                    s_ai_ch_npu_cry_ready = true;
                }
            }
        }
    } else if ((effect_change & AUDIO_EFFECT_CPU_CRY) && (effect & AUDIO_EFFECT_CPU_CRY)) {
        s_ai_ch_cpu_cry_ready = false;
        r = AK_MPI_AI_LoadConf(my_data->in_device_id, AUDIO_IN_CHANNEL_CPU_CRY_ID,
                               my_data->cfg->loadconf_file_path,
                               my_data->cfg->loadconf_in_cpu_cry_section);
        if (r) {
            log_error("AK_MPI_AI_LoadConf CPU_CRY ch%d failed, ret=%d",
                      AUDIO_IN_CHANNEL_CPU_CRY_ID, r);
            last_err = r;
        } else {
            r = AK_MPI_AI_EnableChn(my_data->in_device_id, AUDIO_IN_CHANNEL_CPU_CRY_ID);
            if (r) {
                log_error("AK_MPI_AI_EnableChn CPU_CRY ch%d failed, ret=%d",
                          AUDIO_IN_CHANNEL_CPU_CRY_ID, r);
                last_err = r;
            } else {
                s_ai_ch_cpu_cry_ready = true;
            }
        }
    }

    if ((effect_change & AUDIO_EFFECT_LSSL) && (effect & AUDIO_EFFECT_LSSL)) {
        s_ai_ch_lssl_ready = false;
        r = AK_MPI_AI_LoadConf(my_data->in_device_id, AUDIO_IN_CHANNEL_LSSL_ID,
                               my_data->cfg->loadconf_file_path, my_data->cfg->loadconf_in_lssl_section);
        if (r) {
            log_error("AK_MPI_AI_LoadConf LSSL ch%d failed, ret=%d", AUDIO_IN_CHANNEL_LSSL_ID, r);
            last_err = r;
        } else {
            r = AK_MPI_AI_EnableChn(my_data->in_device_id, AUDIO_IN_CHANNEL_LSSL_ID);
            if (r) {
                log_error("AK_MPI_AI_EnableChn LSSL ch%d failed, ret=%d", AUDIO_IN_CHANNEL_LSSL_ID, r);
                last_err = r;
            } else {
                s_ai_ch_lssl_ready = true;
            }
        }
    }

    return last_err;
}

/**
 * After hal_audio_effect_algorithm_ai_init: Register/Deinit effect libs (VAD, NPU/CPU CRY, LSSL)
 * and update audio_effect.audio_effect_type_flag / resample_for_audio_detect_flag.
 */
static void hal_audio_effect_algorithm_effects_init(struct stream_in *in, audio_effect_type_t effect)
{
    int effect_change = effect ^ audio_effect.audio_effect_type_flag;

    if (effect_change & AUDIO_EFFECT_VAD) {
        if (effect & AUDIO_EFFECT_VAD) {
            if (!s_ai_ch_vad_ready) {
                log_warn("VAD skipped: AI ch%u enable/loadconf failed", (unsigned)AUDIO_IN_CHANNEL_VAD_ID);
            } else {
                int rv = audio_effect_vad_init(in);
                if (rv == AK_SUCCESS) {
                    audio_effect.audio_effect_type_flag |= AUDIO_EFFECT_VAD;
                    log_info("audio effect vad init");
                } else {
                    log_warn("audio_effect_vad_init failed, ret=%d", rv);
                }
            }
        } else {
            audio_effect.audio_effect_type_flag &= ~AUDIO_EFFECT_VAD;
            audio_effect_vad_deinit(in);
        }
    }

    if (effect_change & AUDIO_EFFECT_NPU_CRY) {
        if (effect & AUDIO_EFFECT_NPU_CRY) {
            if (!s_ai_ch_npu_cry_ready) {
                log_warn("NPU_CRY skipped: NPU init or AI ch%u enable/loadconf failed",
                         (unsigned)AUDIO_IN_CHANNEL_NPU_CRY_ID);
            } else {
                int ret = audio_effect_npu_cry_init(in);
                if (ret == AK_SUCCESS) {
                    audio_effect.audio_effect_type_flag |= AUDIO_EFFECT_NPU_CRY;
                    audio_effect.resample_for_audio_detect_flag = true;
                    log_info("audio effect npu cry init");
                } else {
                    log_warn("NPU_CRY effect init failed (NPU may not be available), continuing without NPU_CRY");
                }
            }
        } else {
            audio_effect.audio_effect_type_flag &= ~AUDIO_EFFECT_NPU_CRY;
            audio_effect.resample_for_audio_detect_flag = false;
            audio_effect_npu_cry_deinit(in);
        }
    } else if (effect_change & AUDIO_EFFECT_CPU_CRY) {
        if (effect & AUDIO_EFFECT_CPU_CRY) {
            if (!s_ai_ch_cpu_cry_ready) {
                log_warn("CPU_CRY skipped: AI ch%u enable/loadconf failed",
                         (unsigned)AUDIO_IN_CHANNEL_CPU_CRY_ID);
            } else {
                int rv = audio_effect_cpu_cry_init(in);
                if (rv == AK_SUCCESS) {
                    audio_effect.audio_effect_type_flag |= AUDIO_EFFECT_CPU_CRY;
                } else {
                    log_warn("audio_effect_cpu_cry_init failed, ret=%d", rv);
                }
            }
        } else {
            audio_effect.audio_effect_type_flag &= ~AUDIO_EFFECT_CPU_CRY;
            audio_effect_cpu_cry_deinit(in);
            log_info("AUDIO_EFFECT_CPU_CRY disable");
        }
    }

    if (effect_change & AUDIO_EFFECT_LSSL) {
        if (effect & AUDIO_EFFECT_LSSL) {
            if (!s_ai_ch_lssl_ready) {
                log_warn("LSSL skipped: AI ch%u enable/loadconf failed", (unsigned)AUDIO_IN_CHANNEL_LSSL_ID);
            } else {
                int rv = audio_effect_lssl_init(in);
                if (rv == AK_SUCCESS) {
                    audio_effect.audio_effect_type_flag |= AUDIO_EFFECT_LSSL;
                    log_info("AUDIO_EFFECT_LSSL enable");
                } else {
                    log_warn("audio_effect_lssl_init failed, ret=%d", rv);
                }
            }
        } else {
            audio_effect.audio_effect_type_flag &= ~AUDIO_EFFECT_LSSL;
            audio_effect_lssl_deinit(in);
            log_info("AUDIO_EFFECT_LSSL disable");
        }
    }
}


static void check_effect_init( audio_effect_type_t effect, struct stream_in *in)
{
    int effect_change = effect ^ audio_effect.audio_effect_type_flag;
    /* AI LoadConf/EnableChn + effect Register/Init only touch these bits */
    const int algo_effect_change = effect_change & (int)(AUDIO_EFFECT_VAD | AUDIO_EFFECT_NPU_CRY |
                                                         AUDIO_EFFECT_CPU_CRY | AUDIO_EFFECT_LSSL);

    if (algo_effect_change != 0) {
        (void)hal_audio_effect_algorithm_ai_init(in, effect);
        hal_audio_effect_algorithm_effects_init(in, effect);
    }

    // Open thread after all initializations succeed
    if( (effect_change & AUDIO_EFFECT_VAD) && (effect & AUDIO_EFFECT_VAD) &&
        (audio_effect.audio_effect_type_flag & AUDIO_EFFECT_VAD))
    {
        pthread_create(&audio_effect.in_vad_effect_thread_id, NULL, audio_process_vad_effect_thread_function, in);
    }
    if( (effect_change & AUDIO_EFFECT_NPU_CRY) && (effect & AUDIO_EFFECT_NPU_CRY) && 
        (audio_effect.audio_effect_type_flag & AUDIO_EFFECT_NPU_CRY))
    {
        pthread_create(&audio_effect.in_npu_cry_effect_thread_id, NULL, audio_process_npu_cry_effect_thread_function, in);
    }
    if( (effect_change & AUDIO_EFFECT_CPU_CRY) && (effect & AUDIO_EFFECT_CPU_CRY) &&
        (audio_effect.audio_effect_type_flag & AUDIO_EFFECT_CPU_CRY))
    {
        pthread_create(&audio_effect.in_cpu_cry_effect_thread_id, NULL, audio_process_cpu_cry_effect_thread_function, in);
    }
    if( (effect_change & AUDIO_EFFECT_LSSL) && (effect & AUDIO_EFFECT_LSSL) &&
        (audio_effect.audio_effect_type_flag & AUDIO_EFFECT_LSSL))
    {
        pthread_create(&audio_effect.in_lssl_effect_thread_id, NULL, audio_process_lssl_effect_thread_function, in);
    }

    /* Handle resample effect enable/disable based on effect_change and resample_for_audio_detect_flag */
    if(effect_change & AUDIO_EFFECT_RESAMPLE)
    {
        if(effect & AUDIO_EFFECT_RESAMPLE)
        {
            if(audio_effect.resample_for_audio_detect_inited == false)
            {
                audio_effect_resample_init(in, 16000);
                audio_effect.resample_for_audio_detect_inited = true;
            }
            audio_effect.audio_effect_type_flag |= AUDIO_EFFECT_RESAMPLE;
        }
        else if(audio_effect.resample_for_audio_detect_flag == false)
        {
            audio_effect_resample_deinit(in);
            audio_effect.audio_effect_type_flag &= ~AUDIO_EFFECT_RESAMPLE;
            audio_effect.resample_for_audio_detect_inited = false;
        }
    }

    /* Initialize resample if required for audio detection.
     * Must set AUDIO_EFFECT_RESAMPLE in type_flag so remove phase-1 queues DeInit. */
    if(audio_effect.resample_for_audio_detect_flag == true)
    {
        if(audio_effect.resample_for_audio_detect_inited == false)
        {
            audio_effect_resample_init(in, 16000);
            audio_effect.resample_for_audio_detect_inited = true;
        }
        audio_effect.audio_effect_type_flag |= AUDIO_EFFECT_RESAMPLE;
    }
    else if((effect & AUDIO_EFFECT_RESAMPLE) == 0)
    {
        if(audio_effect.resample_for_audio_detect_inited == true)
        {
            audio_effect_resample_deinit(in);
            audio_effect.resample_for_audio_detect_inited = false;
            audio_effect.audio_effect_type_flag &= ~AUDIO_EFFECT_RESAMPLE;
        }
    }

    if (in->status == AUDIO_STATUS_ENABLED && in->dev->callback_ops != NULL)
        audio_hw_stream_in_ensure_capture_started(&in->stream);
}

static AK_U64 ShowEffectResult(audio_effect_type_t effect, AUDIO_EFFECT_RESULT_S *pstEffectResult )
{
    AK_S32 s32Index = 0;
    AK_U64 result = 0;

    if(effect & AUDIO_EFFECT_VAD)
    {
        AUDIO_VAD_RESULT_LIST_S *pList = pstEffectResult->unEffectResult.pstDetResult;

        while (pList) 
        {
            if(pList->stResult != NULL)
            {
                s32Index ++;
                result += pList->stResult->s32ProbedThresh;
                pList = (AUDIO_VAD_RESULT_LIST_S *)(pList->next);
            }
        }
    }
    else if(effect & AUDIO_EFFECT_NPU_CRY)
    {
        AUDIO_AICRY_RESULT_LIST_S *pList = pstEffectResult->unEffectResult.pstDetResult;
        while(pList){
            s32Index ++;
            result += s32Index;
            pList = (AUDIO_AICRY_RESULT_LIST_S *)(pList->next);
        }
        result = s32Index;

    }
    else if(effect & AUDIO_EFFECT_CPU_CRY)
    {

        AUDIO_CRY_RESULT_LIST_S *pList = pstEffectResult->unEffectResult.pstDetResult;
        while (pList) {
            s32Index ++;
            result ++;
            pList = (AUDIO_CRY_RESULT_LIST_S *)(pList->next);
        }
    }
    else if(effect & AUDIO_EFFECT_LSSL)
    {
    }

    return result;
}



static void audio_effect_in_resample_process( audio_effect_type_t effect, void* buffer, size_t bytes, struct stream_in *in)
{
    int iRet = 0;
    audio_effect_type_t effect_callback_type = 0;
    struct audio_device *adev = in->dev;
    struct hal_audio_data *my_data = adev->hal_audio;
    const audio_callback_ops_t *callback_ops = in->dev->callback_ops;

    if(effect & AUDIO_EFFECT_RESAMPLE)
    {
          iRet = AK_MPI_AUDIO_EFFECT_Process(AUDIO_EFFECT_RESAMPLE_ID,  buffer, bytes, &audio_effect.stResResult);
           if (iRet == 0)
           {
                audio_capture_result_t result = {};
                uint32_t frame_number;
                uint64_t timestamp;

                result.timestamp = timestamp;
                in->output_pcm_buffer.data = audio_effect.stResResult.unEffectResult.stDataProcessResult.pu8OutBuf;
                in->output_pcm_buffer.data_size = audio_effect.stResResult.unEffectResult.stDataProcessResult.u32OutDataLen;

                result.input_buffer = &in->input_buffer;

                result.output_buffer = &in->output_pcm_buffer;
                callback_ops->process_capture_result(callback_ops, &result);
           }
    }
}

/**
 * @brief CPU-based cry detection processing thread.
 * @param arg Input stream pointer (struct stream_in *).
 * @return NULL on thread exit.
 * @note Continuously processes audio frames for cry detection until effect is disabled.
 */
static void* audio_process_cpu_cry_effect_thread_function(void* arg) {
    struct stream_in *in = (struct stream_in *)arg;
    struct audio_device *adev = in->dev;
    struct hal_audio_data *my_data = adev->hal_audio;
    int iRet = 0;
    const audio_callback_ops_t *callback_ops = in->dev->callback_ops;

    log_info("audio_process_cpu_cry_effect_thread_function enter");
    FRAME_S  ai_frame={0};

    while(audio_effect.audio_effect_type_flag  & AUDIO_EFFECT_CPU_CRY)
    {
        iRet = AK_MPI_AI_GetFrame(my_data->in_device_id, AUDIO_IN_CHANNEL_CPU_CRY_ID,
                                  &ai_frame, effect_getframe_timeout_ms(my_data), 0);

        if(!iRet && ai_frame.u32Len > 0 && ai_frame.pu8Data != NULL)
        {
            iRet = AK_MPI_AUDIO_EFFECT_Process(AUDIO_EFFECT_CRY_ID, ai_frame.pu8Data, ai_frame.u32Len, &audio_effect.stCryResult);
            audio_effect.msg_cpu_cry.detect.value = ShowEffectResult(my_data->in_effect, &audio_effect.stCryResult);

            if (iRet == 0)
            {
                if(audio_effect.msg_cpu_cry.detect.value > 0)
                {
                    if(audio_effect.msg_cpu_cry.detect.value > 0xffff)
                    {
                        audio_effect.msg_cpu_cry.detect.value = 0xffff;
                    }
                    audio_effect.msg_cpu_cry.type = AUDIO_EFFECT_CPU_CRY;
                    if(callback_ops->notify != NULL)
                    {
                        callback_ops->notify(callback_ops, &audio_effect.msg_cpu_cry);
                    }
                    else
                    {
                        log_error("audio notify = NULL");
                    }
                }
            }
        }
        else
        {
            ak_sleep_ms(100);
            log_error("audio_process_cpu_cry_effect_thread_function AK_MPI_AI_GetFrame failed, sleep 100ms");
        }
        AK_MPI_AI_ReleaseFrame(my_data->in_device_id, AUDIO_IN_CHANNEL_CPU_CRY_ID, &ai_frame, 0);
    }
    log_info("audio_process_cpu_cry_effect_thread_function exit");
    return NULL;
}

/**
 * @brief NPU-based cry detection processing thread.
 * @param arg Input stream pointer (struct stream_in *).
 * @return NULL on thread exit.
 * @note Supports 8kHz resample to 16kHz for NPU processing.
 */
static void* audio_process_npu_cry_effect_thread_function(void* arg) {
    struct stream_in *in = (struct stream_in *)arg;
    struct audio_device *adev = in->dev;
    struct hal_audio_data *my_data = adev->hal_audio;
    int iRet = 0;
    const audio_callback_ops_t *callback_ops = in->dev->callback_ops;

    log_info("audio_process_npu_cry_effect_thread_function enter");

    FRAME_S  ai_frame={0};
    while(audio_effect.audio_effect_type_flag  & AUDIO_EFFECT_NPU_CRY)
    {
        iRet = AK_MPI_AI_GetFrame(my_data->in_device_id, AUDIO_IN_CHANNEL_NPU_CRY_ID,
                                  &ai_frame, effect_getframe_timeout_ms(my_data), 0);

        if(!iRet && ai_frame.u32Len > 0 && ai_frame.pu8Data != NULL)
        {
            if(in->sample_rate == 8000)
            {
                /* process Resample */
                iRet = AK_MPI_AUDIO_EFFECT_Process( AUDIO_EFFECT_RESAMPLE_ID, ai_frame.pu8Data, ai_frame.u32Len , &audio_effect.stAiCryResult);
                if(iRet == AK_SUCCESS)
                {
                    iRet = AK_MPI_AUDIO_EFFECT_Process(AUDIO_EFFECT_CRY_ID, audio_effect.stResResult.unEffectResult.pstDetResult, \
                        audio_effect.stResResult.unEffectResult.stDataProcessResult.u32OutDataLen, &audio_effect.stAiCryResult);
                }
            }
            else
            {
                iRet = AK_MPI_AUDIO_EFFECT_Process(AUDIO_EFFECT_CRY_ID, ai_frame.pu8Data, ai_frame.u32Len, &audio_effect.stAiCryResult);
            }

            if (iRet == AK_SUCCESS)
            {
                audio_effect.msg_npu_cry.detect.value = ShowEffectResult(my_data->in_effect, &audio_effect.stAiCryResult);

                if(audio_effect.msg_npu_cry.detect.value > 0)
                {
                    if(audio_effect.msg_npu_cry.detect.value > 0xffff)
                    {
                        audio_effect.msg_npu_cry.detect.value = 0xffff;
                    }
                    audio_effect.msg_npu_cry.type = AUDIO_EFFECT_NPU_CRY;
                    if(callback_ops->notify != NULL)
                    {
                        callback_ops->notify(callback_ops, &audio_effect.msg_npu_cry);
                    }
                    else
                    {
                        log_error("audio notify = NULL");
                    }
                }
            }
        }
        else
        {
            ak_sleep_ms(100);
            log_error("audio_process_npu_cry_effect_thread_function AK_MPI_AI_GetFrame failed, sleep 100ms");
        }
        AK_MPI_AI_ReleaseFrame(my_data->in_device_id, AUDIO_IN_CHANNEL_NPU_CRY_ID, &ai_frame, 0);
    }

    log_info("audio_process_npu_cry_effect_thread_function exit");
    return NULL;
}

/**
 * @brief VAD (Voice Activity Detection) processing thread.
 * @param arg Input stream pointer (struct stream_in *).
 * @return NULL on thread exit.
 */
static void* audio_process_vad_effect_thread_function(void* arg) {
    struct stream_in *in = (struct stream_in *)arg;
    struct audio_device *adev = in->dev;
    struct hal_audio_data *my_data = adev->hal_audio;

    int iRet = 0;
    const audio_callback_ops_t *callback_ops = in->dev->callback_ops;

    log_info("audio_process_vad_effect_thread_function enter");

    FRAME_S  ai_frame={0};
    while(audio_effect.audio_effect_type_flag  & AUDIO_EFFECT_VAD)
    {
        iRet = AK_MPI_AI_GetFrame(my_data->in_device_id, AUDIO_IN_CHANNEL_VAD_ID,
                                  &ai_frame, effect_getframe_timeout_ms(my_data), 0);

        if(!iRet && ai_frame.u32Len > 0 && ai_frame.pu8Data != NULL)
        {
            iRet = AK_MPI_AUDIO_EFFECT_Process(AUDIO_EFFECT_VAD_ID, ai_frame.pu8Data, ai_frame.u32Len, &audio_effect.stVadResult);
            if (iRet == 0)
            {
                audio_effect.msg_vad.detect.value = ShowEffectResult(my_data->in_effect,&audio_effect.stVadResult);
                if(audio_effect.msg_vad.detect.value > 0)
                {
                    if(audio_effect.msg_vad.detect.value > 0xffff)
                    {
                        audio_effect.msg_vad.detect.value = 0xffff;
                    }
                    audio_effect.msg_vad.type = AUDIO_EFFECT_VAD;

                    if(callback_ops->notify != NULL)
                    {
                        callback_ops->notify(callback_ops, &audio_effect.msg_vad);
                    }
                    else
                        log_error("audio notify = NULL");
                }
            }
        }
        else
        {
            ak_sleep_ms(100);
            log_error("audio_process_vad_effect_thread_function AK_MPI_AI_GetFrame failed, sleep 100ms");
        }
        AK_MPI_AI_ReleaseFrame(my_data->in_device_id, AUDIO_IN_CHANNEL_VAD_ID, &ai_frame, 0);
    }
    log_info("audio_process_vad_effect_thread_function exit");
    return NULL;
}

/**
 * @brief LSSL (Linear Sound Source Localization) processing thread.
 * @param arg Input stream pointer (struct stream_in *).
 * @return NULL on thread exit.
 * @note Supports 8kHz resample to 16kHz. Reports sound source direction (azimuth, elevation, radius).
 */
static void* audio_process_lssl_effect_thread_function(void* arg) {
    struct stream_in *in = (struct stream_in *)arg;
    struct audio_device *adev = in->dev;
    struct hal_audio_data *my_data = adev->hal_audio;
    int iRet = 0;
    const audio_callback_ops_t *callback_ops = in->dev->callback_ops;

    log_info("audio_process_lssl_effect_thread_function enter");

    FRAME_S  ai_frame={0};
    //pthread_setname_np(pthread_self(), "audio_lssl");
    while(audio_effect.audio_effect_type_flag  & AUDIO_EFFECT_LSSL)
    {
        iRet = AK_MPI_AI_GetFrame(my_data->in_device_id, AUDIO_IN_CHANNEL_LSSL_ID,
                                  &ai_frame, effect_getframe_timeout_ms(my_data), 0);

        if(!iRet && ai_frame.u32Len > 0 && ai_frame.pu8Data != NULL)
        {
            if(in->sample_rate == 8000)
            {
                /* process Resample */
                iRet = AK_MPI_AUDIO_EFFECT_Process( AUDIO_EFFECT_RESAMPLE_ID, ai_frame.pu8Data, ai_frame.u32Len, &audio_effect.stLsslResult);
                if(iRet == AK_SUCCESS)
                {
                    iRet = AK_MPI_AUDIO_EFFECT_Process(AUDIO_EFFECT_LSSL_ID, audio_effect.stResResult.unEffectResult.pstDetResult, \
                        audio_effect.stResResult.unEffectResult.stDataProcessResult.u32OutDataLen, &audio_effect.stLsslResult);
                }
            }
            else
            {
                iRet = AK_MPI_AUDIO_EFFECT_Process(AUDIO_EFFECT_LSSL_ID, ai_frame.pu8Data, ai_frame.u32Len, &audio_effect.stLsslResult);
            }
            if (iRet == AK_SUCCESS)
            {
                AUDIO_SSL_RESULT_LIST_S *pResult = audio_effect.stLsslResult.unEffectResult.pstDetResult;

                if(pResult)
                {
                    audio_effect.msg_lssl.type = AUDIO_EFFECT_LSSL;

                    audio_effect.msg_lssl.ssl.azimuth = pResult->stResult->pastSndSrcDir->azimuth;
                    audio_effect.msg_lssl.ssl.elevation = pResult->stResult->pastSndSrcDir->elevation;
                    audio_effect.msg_lssl.ssl.radius = pResult->stResult->pastSndSrcDir->radius;
                    if(callback_ops->notify != NULL)
                    {
                        callback_ops->notify(callback_ops, &audio_effect.msg_lssl);
                    }
                }
            }
        }
        else
        {
            ak_sleep_ms(100);
            log_error("audio_process_lssl_effect_thread_function AK_MPI_AI_GetFrame failed, sleep 100ms");
        }
 
        AK_MPI_AI_ReleaseFrame(my_data->in_device_id, AUDIO_IN_CHANNEL_LSSL_ID, &ai_frame, 0);
    }

    log_info("audio_process_lssl_effect_thread_function exit");
    return NULL;
}




void audio_effect_out_process( audio_effect_type_t effect, void* buffer, size_t bytes, uint32_t sample_rate)
{

}


int hal_audio_effect_out_add( void *hal_audio, audio_effect_type_t effect)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;

    my_data->out_effect |= effect;
    return AK_SUCCESS;
}

int hal_audio_effect_out_remove( void *hal_audio, audio_effect_type_t effect)
{
   struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
   my_data->out_effect &= ~effect;
   return AK_SUCCESS;
}

int hal_audio_effect_in_add( void *hal_audio, audio_effect_type_t effect)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    struct audio_device *adev = my_data->adev;
    struct stream_in *in = adev->in;

    my_data->in_effect |= effect;

    check_effect_init(my_data->in_effect, in);

    return AK_SUCCESS;
}

int hal_audio_effect_in_remove( void *hal_audio, audio_effect_type_t effect)
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    struct audio_device *adev = my_data->adev;
    struct stream_in *in = adev->in;
    audio_effect_type_t active_effects = (audio_effect.audio_effect_type_flag & effect);

    if (my_data->recording_flag) {
        /* Phase-1: stop effect threads quickly by clearing run flags. */
        audio_effect.pending_deinit_effect_flag |= active_effects;
        if (audio_effect.resample_for_audio_detect_inited) {
            audio_effect.pending_deinit_effect_flag |= AUDIO_EFFECT_RESAMPLE;
        }
        my_data->in_effect &= ~effect;
        audio_effect.audio_effect_type_flag &= ~effect;
        log_info("effect remove phase-1: active=0x%x pending=0x%x",
                 active_effects, audio_effect.pending_deinit_effect_flag);

        if ((active_effects & AUDIO_EFFECT_VAD) && audio_effect.in_vad_effect_thread_id != 0) {
            pthread_join(audio_effect.in_vad_effect_thread_id, NULL);
            audio_effect.in_vad_effect_thread_id = 0;
        }
        if ((active_effects & AUDIO_EFFECT_NPU_CRY) && audio_effect.in_npu_cry_effect_thread_id != 0) {
            pthread_join(audio_effect.in_npu_cry_effect_thread_id, NULL);
            audio_effect.in_npu_cry_effect_thread_id = 0;
        }
        if ((active_effects & AUDIO_EFFECT_CPU_CRY) && audio_effect.in_cpu_cry_effect_thread_id != 0) {
            pthread_join(audio_effect.in_cpu_cry_effect_thread_id, NULL);
            audio_effect.in_cpu_cry_effect_thread_id = 0;
        }
        if ((active_effects & AUDIO_EFFECT_LSSL) && audio_effect.in_lssl_effect_thread_id != 0) {
            pthread_join(audio_effect.in_lssl_effect_thread_id, NULL);
            audio_effect.in_lssl_effect_thread_id = 0;
        }

        /* Still in input-close flow: disable effect AI sub-channels now so
         * hal_audio_input_close()->AK_MPI_AI_Disable does not fail with
         * "Has channels not be disable". */
        if (my_data->in_device_id >= 0) {
            if (active_effects & AUDIO_EFFECT_VAD) {
                int ret = AK_MPI_AI_DisableChn(my_data->in_device_id, AUDIO_IN_CHANNEL_VAD_ID);
                log_info("phase-1 disable ch%d(VAD) ret=%d", AUDIO_IN_CHANNEL_VAD_ID, ret);
                s_ai_ch_vad_ready = false;
            }
            if (active_effects & AUDIO_EFFECT_NPU_CRY) {
                int ret = AK_MPI_AI_DisableChn(my_data->in_device_id, AUDIO_IN_CHANNEL_NPU_CRY_ID);
                log_info("phase-1 disable ch%d(NPU_CRY) ret=%d", AUDIO_IN_CHANNEL_NPU_CRY_ID, ret);
                s_ai_ch_npu_cry_ready = false;
            }
            if (active_effects & AUDIO_EFFECT_CPU_CRY) {
                int ret = AK_MPI_AI_DisableChn(my_data->in_device_id, AUDIO_IN_CHANNEL_CPU_CRY_ID);
                log_info("phase-1 disable ch%d(CPU_CRY) ret=%d", AUDIO_IN_CHANNEL_CPU_CRY_ID, ret);
                s_ai_ch_cpu_cry_ready = false;
            }
            if (active_effects & AUDIO_EFFECT_LSSL) {
                int ret = AK_MPI_AI_DisableChn(my_data->in_device_id, AUDIO_IN_CHANNEL_LSSL_ID);
                log_info("phase-1 disable ch%d(LSSL) ret=%d", AUDIO_IN_CHANNEL_LSSL_ID, ret);
                s_ai_ch_lssl_ready = false;
            }
        }
        return AK_SUCCESS;
    }

    /* Phase-2: deinit effect engines after input has been closed. */
    if (audio_effect.pending_deinit_effect_flag != AUDIO_EFFECT_NONE) {
        audio_effect_type_t pending = audio_effect.pending_deinit_effect_flag;
        log_info("effect remove phase-2: pending=0x%x", pending);
        if (pending & AUDIO_EFFECT_VAD) {
            audio_effect_vad_deinit(in);
        }
        if (pending & AUDIO_EFFECT_NPU_CRY) {
            audio_effect_npu_cry_deinit(in);
        }
        if (pending & AUDIO_EFFECT_CPU_CRY) {
            audio_effect_cpu_cry_deinit(in);
        }
        if (pending & AUDIO_EFFECT_LSSL) {
            audio_effect_lssl_deinit(in);
        }
        if ((pending & AUDIO_EFFECT_RESAMPLE) || audio_effect.resample_for_audio_detect_inited) {
            audio_effect_resample_deinit(in);
            log_info("AUDIO_EFFECT_RESAMPLE deinit");
        }
        audio_effect.pending_deinit_effect_flag = AUDIO_EFFECT_NONE;
        audio_effect.resample_for_audio_detect_flag = false;
        audio_effect.resample_for_audio_detect_inited = false;
        audio_effect.audio_effect_type_flag &= ~AUDIO_EFFECT_RESAMPLE;
    } else {
        my_data->in_effect &= ~effect;
        check_effect_init(my_data->in_effect, in);
    }

    return AK_SUCCESS;
}

int hal_audio_effect_in_process( void *hal_audio, void* buffer, size_t bytes )
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    struct audio_device *adev = my_data->adev;
    struct stream_in *in = adev->in;

    /* Live PCM must not enter Filter_Control / Echo. Talk (CALL) only. */
    if (!adev || adev->mode != AUDIO_MODE_CALL)
        return AK_SUCCESS;

    audio_effect_in_resample_process(my_data->in_effect, buffer, bytes, in);

    return AK_SUCCESS;

}

int hal_audio_effect_out_process( void *hal_audio, void* buffer, size_t bytes )
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    struct audio_device *adev = my_data->adev;
    struct stream_out *out = adev->out;

    audio_effect_out_process(my_data->out_effect, buffer, bytes, out->sample_rate);
    return AK_SUCCESS;
}



int hal_audio_effect_in_loadconf( void *hal_audio )
{
    struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    struct audio_device *adev = my_data->adev;
    struct stream_in *in = adev->in;
    int ret;

    if(adev->mode == my_data->in_mode)
        return 0;

    if (my_data->recording_flag) {
        log_info("AI: skip LoadConf (capture running, mode=%d)", (int)adev->mode);
        return 0;
    }
    if (my_data->playing_flag) {
        log_info("AI: skip LoadConf (AO Echo running, mode=%d)",
                 (int)adev->mode);
        return 0;
    }

    if(in->devices == AUDIO_DEVICE_IN_AMIC )
    {
        const char *section_name = NULL;
        if(adev->mode != AUDIO_MODE_CALL)
        {
            log_info("AI: skip LoadConf (non-CALL, no AEC)");
            my_data->in_mode = adev->mode;
            return 0;
        }
        section_name = my_data->cfg->loadconf_echo_section;
        log_info("load audio input conf: device_id=%d, channel_id=%d, path=%s, section=%s (CALL mode)",
                 my_data->in_device_id, my_data->in_channel_main_id,
                 my_data->cfg->loadconf_file_path, section_name);
        ret = AK_MPI_AI_LoadConf(my_data->in_device_id, my_data->in_channel_main_id,
                                 my_data->cfg->loadconf_file_path, my_data->cfg->loadconf_echo_section);
        if(ret)
        {
            log_warn("AK_MPI_AI_LoadConf failed: device_id=%d, channel_id=%d, path=%s, section=%s, ret=%d",
                     my_data->in_device_id, my_data->in_channel_main_id,
                     my_data->cfg->loadconf_file_path, section_name, ret);
            log_warn("Audio effect config file may not exist or section may be missing, continuing with defaults");
            /* Continue anyway - audio effects config is optional, defaults will be used */
        } else {
            log_info("Audio input config loaded successfully: path=%s, section=%s",
                     my_data->cfg->loadconf_file_path, section_name);
        }
        
        /* Get gain values even if LoadConf failed (they may have defaults) */
        AK_MPI_AI_GetGain(my_data->in_device_id, my_data->in_channel_main_id, &my_data->in_gain);
        AK_MPI_AI_GetDigitalGain(my_data->in_device_id, my_data->in_channel_main_id, &my_data->in_volume);

    }
    else if( in->devices == AUDIO_DEVICE_IN_DMIC )
    {
        log_debug("load audio input conf (DMIC): device_id=%d, channel_id=%d, path=%s, section=%s",
                 my_data->in_device_id, my_data->in_channel_main_id,
                 my_data->cfg->loadconf_file_path, my_data->cfg->loadconf_in_dmic_section);
        ret = AK_MPI_AI_LoadConf(my_data->in_device_id, my_data->in_channel_main_id,
                                 my_data->cfg->loadconf_file_path, my_data->cfg->loadconf_in_dmic_section);
        if(ret)
        {
            log_warn("AK_MPI_AI_LoadConf failed (DMIC): device_id=%d, channel_id=%d, path=%s, section=%s, ret=%d",
                     my_data->in_device_id, my_data->in_channel_main_id,
                     my_data->cfg->loadconf_file_path, my_data->cfg->loadconf_in_dmic_section, ret);
            log_warn("Audio effect config file may not exist or section may be missing, continuing with defaults");
            /* Continue anyway - audio effects config is optional, defaults will be used */
        } else {
            log_info("Audio input config loaded successfully (DMIC): path=%s, section=%s",
                     my_data->cfg->loadconf_file_path, my_data->cfg->loadconf_in_dmic_section);
        }
    }
    else
    {
        log_debug("hal_audio_effect_in_loadconf: unsupported device type: 0x%x", in->devices);
        /* Continue anyway - may not need audio effects config for this device type */
    }

    my_data->in_mode = adev->mode;
    /* Always return success - audio effects config is optional */
    /* If LoadConf failed, defaults will be used, which is acceptable */
    return 0;
}

int hal_audio_effect_out_loadconf( void *hal_audio )
{
   struct hal_audio_data *my_data = (struct hal_audio_data *)hal_audio;
    struct audio_device *adev = my_data->adev;
   struct stream_out *out = adev->out;

    if (out == NULL)
        return 0;

    if(adev->mode == my_data->out_mode)
        return 0;

    if (my_data->playing_flag) {
        log_info("AO: skip LoadConf (playback running, mode=%d)", (int)adev->mode);
        return 0;
    }

    if(out->devices == AUDIO_DEVICE_OUT_SPEAKER )
    {
        log_debug("load audio output conf:path:%s, section:%s", my_data->cfg->loadconf_file_path, my_data->cfg->loadconf_out_normal_section);
        if(AK_MPI_AO_LoadConf(my_data->out_device_id, my_data->out_channel_id,
                              my_data->cfg->loadconf_file_path, my_data->cfg->loadconf_out_normal_section))
        {
            /* Playback should continue even if optional AO effect config fails. */
            log_warn("AK_MPI_AO_LoadConf failed (dev=%d, ch=%d), continue without AO effect config",
                     my_data->out_device_id, my_data->out_channel_id);
            return 0;
        }
    }

    my_data->out_mode = adev->mode;

    return 0;
}
#endif
