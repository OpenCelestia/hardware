// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/audio.
 *
 * Runtime configuration loader for audio HAL.
 * - Replaces legacy hal_audio_config.h macros with INI-based configuration.
 * - Uses system/core/config.h and iniparser for configuration management.
 */

#include <string.h>

#include "system_config.h"
#include "log.h"

#include "hal_audio_config.h"

#define LOG_TAG "HAL AUDIO CFG"

/* Default values matching legacy hal_audio_config.h */
/* AUDIO_PRINT_LEVEL_DEFAULT from ak_common_audio.h = 0x10000 */
#define AUDIO_AI_PRINT_LEVEL_DEFAULT         0x10000
#define AUDIO_AI_GAIN_DEFAULT                30
#define AUDIO_AI_DRV_BUF_PERIODS_DEFAULT     16
#define AUDIO_AI_THREAD_PRIORITY_DEFAULT     50

/* AV200 DAC: AK_MPI_AO_SetGain range [-24, 0]; 0 = loudest. */
#define AUDIO_AO_GAIN_DEFAULT                0
#define AUDIO_AO_DRV_BUF_PERIODS_DEFAULT     8
#define AUDIO_AO_THREAD_PRIORITY_DEFAULT     50

#define AUDIO_LOOPBACK_DRV_BUF_PERIODS_DEFAULT   16
#define AUDIO_LOOPBACK_THREAD_PRIORITY_DEFAULT   80

#define AUDIO_SEND_FRAME_TIMEOUT_DEFAULT     1000
#define AUDIO_RECEIVE_FRAME_TIMEOUT_DEFAULT  1000

/* HAL config INI under /etc/config/ with hal_ prefix to avoid conflict with system audio_config.ini */
#define AUDIO_HAL_CONFIG_FILE_NAME           "hal_audio_config.ini"

/* AK MPI LoadConf file path (used by AK_MPI_AI/AO_LoadConf) */
#define AUDIO_LOADCONF_FILE_PATH_DEFAULT     "/etc/config/audio_config.ini"

#define AUDIO_LOADCONF_IN_NORMAL_SECTION_DEFAULT   "def_near"
#define AUDIO_LOADCONF_ECHO_SECTION_DEFAULT        "def_near"
#define AUDIO_LOADCONF_OUT_NORMAL_SECTION_DEFAULT  "def_far"

#define AUDIO_LOADCONF_IN_DMIC_SECTION_DEFAULT     "two_array_near"
#define AUDIO_LOADCONF_IN_VAD_SECTION_DEFAULT      "config_effect_default_near"
#define AUDIO_LOADCONF_IN_CPU_CRY_SECTION_DEFAULT  "config_effect_default_near"
#define AUDIO_LOADCONF_IN_NPU_CRY_SECTION_DEFAULT  "config_effect_default_near"
#define AUDIO_LOADCONF_IN_LSSL_SECTION_DEFAULT     "config_effect_lssl_near"

/* Default AUDIO_EFFECT_* section names */
#define AUDIO_EFFECT_NPU_CRY_SECTION_DEFAULT   "ai_cry_1"
#define AUDIO_EFFECT_CPU_CRY_SECTION_DEFAULT   "cry_1"
#define AUDIO_EFFECT_VAD_SECTION_DEFAULT       "vad_1"
#define AUDIO_EFFECT_LSSL_SECTION_DEFAULT      "def_lssl"
#define AUDIO_EFFECT_RESAMPLE_SECTION_DEFAULT  "res_1"

/* Legacy save/dump defaults */
#define AUDIO_SAVE_PCM_FILE_PATH_DEFAULT      "/tmp/"
#define AUDIO_INPUT_SAVE_PCM_FILE_NAME_DEFAULT  "dump_in"
#define AUDIO_OUTPUT_SAVE_PCM_FILE_NAME_DEFAULT "dump_out"
#define AUDIO_INPUT_SAVE_PCM_FILE_SECOND_DEFAULT  180
#define AUDIO_OUTPUT_SAVE_PCM_FILE_SECOND_DEFAULT 180
#define AUDIO_SEND_FRAME_LEN_DEFAULT          1280

/* Mic defaults */
#define AUDIO_MIC_TYPE_DEFAULT                0   /* 0=auto */
#define AUDIO_MIC_NUM_DEFAULT                 0   /* 0=auto */

static hal_audio_config_t s_audio_cfg;
static int s_audio_cfg_loaded = 0;

static void hal_audio_config_set_defaults(hal_audio_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    cfg->ai_print_level          = AUDIO_AI_PRINT_LEVEL_DEFAULT;
    cfg->ai_gain                 = AUDIO_AI_GAIN_DEFAULT;
    cfg->ai_drv_buf_periods      = AUDIO_AI_DRV_BUF_PERIODS_DEFAULT;
    cfg->ai_thread_priority      = AUDIO_AI_THREAD_PRIORITY_DEFAULT;

    cfg->ao_gain                 = AUDIO_AO_GAIN_DEFAULT;
    cfg->ao_drv_buf_periods      = AUDIO_AO_DRV_BUF_PERIODS_DEFAULT;
    cfg->ao_thread_priority      = AUDIO_AO_THREAD_PRIORITY_DEFAULT;

    cfg->loopback_drv_buf_periods   = AUDIO_LOOPBACK_DRV_BUF_PERIODS_DEFAULT;
    cfg->loopback_thread_priority   = AUDIO_LOOPBACK_THREAD_PRIORITY_DEFAULT;

    cfg->send_frame_timeout_ms      = AUDIO_SEND_FRAME_TIMEOUT_DEFAULT;
    cfg->receive_frame_timeout_ms   = AUDIO_RECEIVE_FRAME_TIMEOUT_DEFAULT;

    /* default ini path for AK MPI loadconf (AK_MPI_AI/AO_LoadConf) */
    strncpy(cfg->loadconf_file_path,
            AUDIO_LOADCONF_FILE_PATH_DEFAULT,
            sizeof(cfg->loadconf_file_path) - 1);

    strncpy(cfg->loadconf_in_normal_section,
            AUDIO_LOADCONF_IN_NORMAL_SECTION_DEFAULT,
            sizeof(cfg->loadconf_in_normal_section) - 1);
    strncpy(cfg->loadconf_echo_section,
            AUDIO_LOADCONF_ECHO_SECTION_DEFAULT,
            sizeof(cfg->loadconf_echo_section) - 1);
    strncpy(cfg->loadconf_out_normal_section,
            AUDIO_LOADCONF_OUT_NORMAL_SECTION_DEFAULT,
            sizeof(cfg->loadconf_out_normal_section) - 1);

    strncpy(cfg->loadconf_in_dmic_section,
            AUDIO_LOADCONF_IN_DMIC_SECTION_DEFAULT,
            sizeof(cfg->loadconf_in_dmic_section) - 1);
    strncpy(cfg->loadconf_in_vad_section,
            AUDIO_LOADCONF_IN_VAD_SECTION_DEFAULT,
            sizeof(cfg->loadconf_in_vad_section) - 1);
    strncpy(cfg->loadconf_in_cpu_cry_section,
            AUDIO_LOADCONF_IN_CPU_CRY_SECTION_DEFAULT,
            sizeof(cfg->loadconf_in_cpu_cry_section) - 1);
    strncpy(cfg->loadconf_in_npu_cry_section,
            AUDIO_LOADCONF_IN_NPU_CRY_SECTION_DEFAULT,
            sizeof(cfg->loadconf_in_npu_cry_section) - 1);
    strncpy(cfg->loadconf_in_lssl_section,
            AUDIO_LOADCONF_IN_LSSL_SECTION_DEFAULT,
            sizeof(cfg->loadconf_in_lssl_section) - 1);

    /* default AUDIO_EFFECT_* section names */
    strncpy(cfg->effect_npu_cry_section,
            AUDIO_EFFECT_NPU_CRY_SECTION_DEFAULT,
            sizeof(cfg->effect_npu_cry_section) - 1);
    strncpy(cfg->effect_cpu_cry_section,
            AUDIO_EFFECT_CPU_CRY_SECTION_DEFAULT,
            sizeof(cfg->effect_cpu_cry_section) - 1);
    strncpy(cfg->effect_vad_section,
            AUDIO_EFFECT_VAD_SECTION_DEFAULT,
            sizeof(cfg->effect_vad_section) - 1);
    strncpy(cfg->effect_lssl_section,
            AUDIO_EFFECT_LSSL_SECTION_DEFAULT,
            sizeof(cfg->effect_lssl_section) - 1);
    strncpy(cfg->effect_resample_section,
            AUDIO_EFFECT_RESAMPLE_SECTION_DEFAULT,
            sizeof(cfg->effect_resample_section) - 1);

    /* Save/dump defaults */
    strncpy(cfg->save_pcm_file_path,
            AUDIO_SAVE_PCM_FILE_PATH_DEFAULT,
            sizeof(cfg->save_pcm_file_path) - 1);
    strncpy(cfg->input_save_pcm_file_name,
            AUDIO_INPUT_SAVE_PCM_FILE_NAME_DEFAULT,
            sizeof(cfg->input_save_pcm_file_name) - 1);
    strncpy(cfg->output_save_pcm_file_name,
            AUDIO_OUTPUT_SAVE_PCM_FILE_NAME_DEFAULT,
            sizeof(cfg->output_save_pcm_file_name) - 1);
    cfg->input_save_pcm_file_second  = AUDIO_INPUT_SAVE_PCM_FILE_SECOND_DEFAULT;
    cfg->output_save_pcm_file_second = AUDIO_OUTPUT_SAVE_PCM_FILE_SECOND_DEFAULT;
    cfg->send_frame_len              = AUDIO_SEND_FRAME_LEN_DEFAULT;

    cfg->mic_type                    = AUDIO_MIC_TYPE_DEFAULT;
    cfg->mic_num                     = AUDIO_MIC_NUM_DEFAULT;
}

static void hal_audio_config_load_from_ini(hal_audio_config_t *cfg)
{
    char path[256] = {0};

    /* Build path /etc/config/hal_audio_config.ini for HAL config */
    if (config_build_path(CONFIG_PATH_TYPE_CONFIG, AUDIO_HAL_CONFIG_FILE_NAME,
                          path, sizeof(path)) != 0) {
        log_warn(LOG_TAG ": cannot build path for %s, using defaults", AUDIO_HAL_CONFIG_FILE_NAME);
        return;
    }

    /* Note: loadconf_file_path remains as AUDIO_LOADCONF_FILE_PATH_DEFAULT
     * (/etc/config/audio_config.ini) for AK_MPI_AI/AO_LoadConf usage.
     * This function only loads HAL parameters from hal_audio_config.ini */

    dictionary *ini = iniparser_load(path);
    if (!ini) {
        log_warn(LOG_TAG ": cannot load HAL config ini %s, using defaults", path);
        return;
    }


    log_warn("hal_audio_config_load_from_ini 0");
    

    /* ai section */
    cfg->ai_print_level = iniparser_dict_getint(ini, "ai", "print_level", cfg->ai_print_level);
    cfg->ai_gain = iniparser_dict_getint(ini, "ai", "gain", cfg->ai_gain);
    cfg->ai_drv_buf_periods = iniparser_dict_getint(ini, "ai", "drv_buf_periods", cfg->ai_drv_buf_periods);
    cfg->ai_thread_priority = iniparser_dict_getint(ini, "ai", "thread_priority", cfg->ai_thread_priority);

    /* ao section */
    cfg->ao_gain = iniparser_dict_getint(ini, "ao", "gain", cfg->ao_gain);
    cfg->ao_drv_buf_periods = iniparser_dict_getint(ini, "ao", "drv_buf_periods", cfg->ao_drv_buf_periods);
    cfg->ao_thread_priority = iniparser_dict_getint(ini, "ao", "thread_priority", cfg->ao_thread_priority);

    /* loopback section */
    cfg->loopback_drv_buf_periods = iniparser_dict_getint(ini, "loopback", "drv_buf_periods",
                                                     cfg->loopback_drv_buf_periods);
    cfg->loopback_thread_priority = iniparser_dict_getint(ini, "loopback", "thread_priority",
                                                     cfg->loopback_thread_priority);

    /* timeout section */
    cfg->send_frame_timeout_ms = iniparser_dict_getint(ini, "timeout", "send_frame_ms",
                                                  cfg->send_frame_timeout_ms);
    cfg->receive_frame_timeout_ms = iniparser_dict_getint(ini, "timeout", "receive_frame_ms",
                                                     cfg->receive_frame_timeout_ms);

    /* sections for AK_MPI_*_LoadConf and AUDIO_EFFECT_* */
    const char *str;

    str = iniparser_dict_getstring(ini, "effect", "in_normal_section",
                              cfg->loadconf_in_normal_section);
    if (str) {
        strncpy(cfg->loadconf_in_normal_section, str,
                sizeof(cfg->loadconf_in_normal_section) - 1);
        cfg->loadconf_in_normal_section[sizeof(cfg->loadconf_in_normal_section) - 1] = '\0';
    }

    str = iniparser_dict_getstring(ini, "effect", "echo_section",
                              cfg->loadconf_echo_section);
    if (str) {
        strncpy(cfg->loadconf_echo_section, str,
                sizeof(cfg->loadconf_echo_section) - 1);
        cfg->loadconf_echo_section[sizeof(cfg->loadconf_echo_section) - 1] = '\0';
    }
    log_warn("hal_audio_config_load_from_ini 1");

    str = iniparser_dict_getstring(ini, "effect", "out_normal_section",
                              cfg->loadconf_out_normal_section);
    if (str) {
        strncpy(cfg->loadconf_out_normal_section, str,
                sizeof(cfg->loadconf_out_normal_section) - 1);
        cfg->loadconf_out_normal_section[sizeof(cfg->loadconf_out_normal_section) - 1] = '\0';
    }

    str = iniparser_dict_getstring(ini, "effect", "in_dmic_section",
                              cfg->loadconf_in_dmic_section);
    if (str) {
        strncpy(cfg->loadconf_in_dmic_section, str,
                sizeof(cfg->loadconf_in_dmic_section) - 1);
        cfg->loadconf_in_dmic_section[sizeof(cfg->loadconf_in_dmic_section) - 1] = '\0';
    }

    str = iniparser_dict_getstring(ini, "effect", "in_vad_section",
                              cfg->loadconf_in_vad_section);
    if (str) {
        strncpy(cfg->loadconf_in_vad_section, str,
                sizeof(cfg->loadconf_in_vad_section) - 1);
        cfg->loadconf_in_vad_section[sizeof(cfg->loadconf_in_vad_section) - 1] = '\0';
    }

    str = iniparser_dict_getstring(ini, "effect", "in_cpu_cry_section",
                              cfg->loadconf_in_cpu_cry_section);
    if (str) {
        strncpy(cfg->loadconf_in_cpu_cry_section, str,
                sizeof(cfg->loadconf_in_cpu_cry_section) - 1);
        cfg->loadconf_in_cpu_cry_section[sizeof(cfg->loadconf_in_cpu_cry_section) - 1] = '\0';
    }

    str = iniparser_dict_getstring(ini, "effect", "in_npu_cry_section",
                              cfg->loadconf_in_npu_cry_section);
    if (str) {
        strncpy(cfg->loadconf_in_npu_cry_section, str,
                sizeof(cfg->loadconf_in_npu_cry_section) - 1);
        cfg->loadconf_in_npu_cry_section[sizeof(cfg->loadconf_in_npu_cry_section) - 1] = '\0';
    }

    str = iniparser_dict_getstring(ini, "effect", "in_lssl_section",
                              cfg->loadconf_in_lssl_section);
    if (str) {
        strncpy(cfg->loadconf_in_lssl_section, str,
                sizeof(cfg->loadconf_in_lssl_section) - 1);
        cfg->loadconf_in_lssl_section[sizeof(cfg->loadconf_in_lssl_section) - 1] = '\0';
    }

    /* AUDIO_EFFECT_* section names (optional overrides) */
    str = iniparser_dict_getstring(ini, "effect", "npu_cry_section",
                              cfg->effect_npu_cry_section);
    if (str) {
        strncpy(cfg->effect_npu_cry_section, str,
                sizeof(cfg->effect_npu_cry_section) - 1);
        cfg->effect_npu_cry_section[sizeof(cfg->effect_npu_cry_section) - 1] = '\0';
    }

    str = iniparser_dict_getstring(ini, "effect", "cpu_cry_section",
                              cfg->effect_cpu_cry_section);
    if (str) {
        strncpy(cfg->effect_cpu_cry_section, str,
                sizeof(cfg->effect_cpu_cry_section) - 1);
        cfg->effect_cpu_cry_section[sizeof(cfg->effect_cpu_cry_section) - 1] = '\0';
    }

    str = iniparser_dict_getstring(ini, "effect", "vad_section",
                              cfg->effect_vad_section);
    if (str) {
        strncpy(cfg->effect_vad_section, str,
                sizeof(cfg->effect_vad_section) - 1);
        cfg->effect_vad_section[sizeof(cfg->effect_vad_section) - 1] = '\0';
    }

    str = iniparser_dict_getstring(ini, "effect", "lssl_section",
                              cfg->effect_lssl_section);
    if (str) {
        strncpy(cfg->effect_lssl_section, str,
                sizeof(cfg->effect_lssl_section) - 1);
        cfg->effect_lssl_section[sizeof(cfg->effect_lssl_section) - 1] = '\0';
    }

    str = iniparser_dict_getstring(ini, "effect", "resample_section",
                              cfg->effect_resample_section);
    if (str) {
        strncpy(cfg->effect_resample_section, str,
                sizeof(cfg->effect_resample_section) - 1);
        cfg->effect_resample_section[sizeof(cfg->effect_resample_section) - 1] = '\0';
    }

    /* Save/dump and frame length (optional) */
    str = iniparser_dict_getstring(ini, "save", "pcm_path",
                              cfg->save_pcm_file_path);
    if (str) {
        strncpy(cfg->save_pcm_file_path, str,
                sizeof(cfg->save_pcm_file_path) - 1);
        cfg->save_pcm_file_path[sizeof(cfg->save_pcm_file_path) - 1] = '\0';
    }

    str = iniparser_dict_getstring(ini, "save", "input_name",
                              cfg->input_save_pcm_file_name);
    if (str) {
        strncpy(cfg->input_save_pcm_file_name, str,
                sizeof(cfg->input_save_pcm_file_name) - 1);
        cfg->input_save_pcm_file_name[sizeof(cfg->input_save_pcm_file_name) - 1] = '\0';
    }

    str = iniparser_dict_getstring(ini, "save", "output_name",
                              cfg->output_save_pcm_file_name);
    if (str) {
        strncpy(cfg->output_save_pcm_file_name, str,
                sizeof(cfg->output_save_pcm_file_name) - 1);
        cfg->output_save_pcm_file_name[sizeof(cfg->output_save_pcm_file_name) - 1] = '\0';
    }

    cfg->input_save_pcm_file_second = iniparser_dict_getint(
        ini, "save", "input_second", cfg->input_save_pcm_file_second);
    cfg->output_save_pcm_file_second = iniparser_dict_getint(
        ini, "save", "output_second", cfg->output_save_pcm_file_second);
    cfg->send_frame_len = iniparser_dict_getint(
        ini, "save", "send_frame_len", cfg->send_frame_len);

    /* mic section (optional) */
    /* type: 0=auto, 1=amic(adc), 2=dmic(pdm) */
    cfg->mic_type = iniparser_dict_getint(ini, "mic", "type", cfg->mic_type);
    /* num: 0=auto, otherwise force mic count */
    cfg->mic_num = iniparser_dict_getint(ini, "mic", "num", cfg->mic_num);

    log_warn("hal_audio_config_load_from_ini 3");

    iniparser_freedict(ini);
}

hal_audio_config_t *hal_audio_get_config(void)
{
    if (!s_audio_cfg_loaded) {
        hal_audio_config_set_defaults(&s_audio_cfg);
        hal_audio_config_load_from_ini(&s_audio_cfg);
        s_audio_cfg_loaded = 1;
    }
    return &s_audio_cfg;
}


