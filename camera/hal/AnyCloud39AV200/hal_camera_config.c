// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/camera.
 *
 * Runtime configuration loader for camera HAL.
 * - Replaces legacy HAL_CAMERA_* macros with INI-based configuration.
 * - Uses system/core/config.h and iniparser for configuration management.
 */

#include <string.h>

#include "system_config.h"
#include "log.h"
#include "camera.h"
#include "ak_svp.h"

#include "hal_camera_config.h"

#define LOG_TAG "HAL CAMERA CFG"

/* Defaults derived from legacy hal_camera_product_config.h */
#define CAMERA_NUMBER_DEFAULT                 1

/* Default per-camera configuration (camera0..3) */
static const int s_default_interface_mode[CAMERA_MAX_NUM] = {2, 2, 2, 2};
static const int s_default_max_fps[CAMERA_MAX_NUM]        = {30, 30, 30, 30};
static const int s_default_mirror[CAMERA_MAX_NUM]         = {1, 0, 0, 0};
static const int s_default_flip[CAMERA_MAX_NUM]           = {0, 0, 0, 0};
/* Software IPU rotation (degrees). 0 = off; only boards with a rotated sensor mount set this. */
static const int s_default_rotation[CAMERA_MAX_NUM]      = {0, 0, 0, 0};

/* VPSS AK_MPI_VPSS_CreateGrp pu8IspCfgPath: sensor tuning binary *.conf (see SDK ak_vpss_sample -f) */
#define ISP_CONFIG_PATH_DEFAULT "/etc/config/isp_ov8856_mipi_2lane_av200.conf"

/* Default IPU configuration */
#define IPU_QUE_ID_DEFAULT      0
#define IPU_MODULE_ID_DEFAULT   0xCC

/* nova_lite action cam: defer unused VPSS channels until stream_reg_client */
#define LAZY_CHANNEL_CREATE_DEFAULT  1

/* HAL config INI under /etc/config/ with hal_ prefix to avoid conflict with other configs */
#define CAMERA_LOADCONF_FILE_NAME             "hal_camera_config.ini"

/* AI type string to enum mapping table */
typedef struct {
    const char *name;
    camera_ai_type_t value;
} ai_type_map_t;

static const ai_type_map_t s_ai_type_map[] = {
    /* Detection types */
    {"CAMERA_AI_DETECT_HUMAN_SHAPE", CAMERA_AI_DETECT_HUMAN_SHAPE},
    {"CAMERA_AI_DETECT_HUMAN_FACE", CAMERA_AI_DETECT_HUMAN_FACE},
    {"CAMERA_AI_DETECT_UPPER_BODY", CAMERA_AI_DETECT_UPPER_BODY},
    {"CAMERA_AI_DETECT_GESTURE", CAMERA_AI_DETECT_GESTURE},
    {"CAMERA_AI_DETECT_PET", CAMERA_AI_DETECT_PET},
    {"CAMERA_AI_DETECT_PACKAGE", CAMERA_AI_DETECT_PACKAGE},
    {"CAMERA_AI_DETECT_FIRE", CAMERA_AI_DETECT_FIRE},
    {"CAMERA_AI_DETECT_VEHICLE", CAMERA_AI_DETECT_VEHICLE},
    {"CAMERA_AI_DETECT_NON_MOTOR_VEHICLE", CAMERA_AI_DETECT_NON_MOTOR_VEHICLE},
    {"CAMERA_AI_DETECT_MOTION", CAMERA_AI_DETECT_MOTION},

    /* Classify after detection types */
    {"CAMERA_AI_DETECT_CLASSIFY_GESTURE", CAMERA_AI_DETECT_CLASSIFY_GESTURE},
    {"CAMERA_AI_DETECT_CLASSIFY_ANTI_SPOOFING_FACE", CAMERA_AI_DETECT_CLASSIFY_ANTI_SPOOFING_FACE},

    /* Recognition types */
    {"CAMERA_AI_RECOGNIZE_HUMAN_FACE", CAMERA_AI_RECOGNIZE_HUMAN_FACE},
    {"CAMERA_AI_RECOGNIZE_UPPER_BODY", CAMERA_AI_RECOGNIZE_UPPER_BODY},

    /* Sub-categories for non-motor vehicle */
    {"CAMERA_AI_DETECT_NON_MOTOR_VEHICLE_MOTORCYCLE", CAMERA_AI_DETECT_NON_MOTOR_VEHICLE_MOTORCYCLE},
    {"CAMERA_AI_DETECT_NON_MOTOR_VEHICLE_BICYCLE", CAMERA_AI_DETECT_NON_MOTOR_VEHICLE_BICYCLE},
    {"CAMERA_AI_DETECT_NON_MOTOR_VEHICLE_TRICYCLE", CAMERA_AI_DETECT_NON_MOTOR_VEHICLE_TRICYCLE},

    /* Sub-categories for vehicle */
    {"CAMERA_AI_DETECT_VEHICLE_BUS", CAMERA_AI_DETECT_VEHICLE_BUS},
    {"CAMERA_AI_DETECT_VEHICLE_CAR", CAMERA_AI_DETECT_VEHICLE_CAR},
    {"CAMERA_AI_DETECT_VEHICLE_TRUCK", CAMERA_AI_DETECT_VEHICLE_TRUCK},

    /* Sub-categories for detect gesture */
    {"CAMERA_AI_DETECT_GESTURE_UNCLASSIFY", CAMERA_AI_DETECT_GESTURE_UNCLASSIFY},
    {"CAMERA_AI_DETECT_GESTURE_OK", CAMERA_AI_DETECT_GESTURE_OK},
    {"CAMERA_AI_DETECT_GESTURE_SIX", CAMERA_AI_DETECT_GESTURE_SIX},
    {"CAMERA_AI_DETECT_GESTURE_STOP", CAMERA_AI_DETECT_GESTURE_STOP},
    {"CAMERA_AI_DETECT_GESTURE_THUMB_UP", CAMERA_AI_DETECT_GESTURE_THUMB_UP},
    {"CAMERA_AI_DETECT_GESTURE_TWO", CAMERA_AI_DETECT_GESTURE_TWO},

    /* Sub-categories for detect anti-spoofing face */
    {"CAMERA_AI_DETECT_ANTI_SPOOFING_FACE_LIVING", CAMERA_AI_DETECT_ANTI_SPOOFING_FACE_LIVING},
    {"CAMERA_AI_DETECT_ANTI_SPOOFING_FACE_NON_LIVING", CAMERA_AI_DETECT_ANTI_SPOOFING_FACE_NON_LIVING},

    /* Sub-categories for detect pet */
    {"CAMERA_AI_DETECT_PET_CAT", CAMERA_AI_DETECT_PET_CAT},
    {"CAMERA_AI_DETECT_PET_DOG", CAMERA_AI_DETECT_PET_DOG},

    /* Special value */
    {"CAMERA_AI_NONE", CAMERA_AI_NONE},
};

#define AI_TYPE_MAP_SIZE (sizeof(s_ai_type_map) / sizeof(s_ai_type_map[0]))

/* SVP model type string to enum mapping */
typedef struct {
    const char *name;
    int value;
} svp_model_type_map_t;

static const svp_model_type_map_t s_svp_model_map[] = {
    {"AK_SVP_MODEL_DETECT", AK_SVP_MODEL_DETECT},
    /* AK_SVP_MODEL_0X0A020003 / 0X0A010004 are AV100_V2-only model IDs; absent in AV200 SDK */
};

#define SVP_MODEL_MAP_SIZE (sizeof(s_svp_model_map) / sizeof(s_svp_model_map[0]))

/* SVP image type string to enum mapping (matches ak_svp.h) */
static const svp_model_type_map_t s_svp_img_map[] = {
    {"AK_SVP_IMG_YUV420SP", AK_SVP_IMG_YUV420SP},
    {"AK_SVP_IMG_RGB_LI", AK_SVP_IMG_RGB_LI},
    {"AK_SVP_IMG_RGB_CHW", AK_SVP_IMG_RGB_CHW},
    {"AK_SVP_IMG_RGB_HWC", AK_SVP_IMG_RGB_HWC},
};

#define SVP_IMG_MAP_SIZE (sizeof(s_svp_img_map) / sizeof(s_svp_img_map[0]))

static hal_camera_config_t s_cam_cfg;
static int s_cam_cfg_loaded = 0;

/* AI detect ↔ sub-type mapping table */
static hal_camera_ai_type_detect_sub_cfg_t s_ai_detect_sub[CAMERA_AI_MAX_MODLE];
static int s_ai_detect_sub_num = 0;

/* AI model configuration table */
static hal_camera_ai_modle_config_cfg_t s_ai_models[CAMERA_AI_MAX_MODLE];
static int s_ai_models_num = 0;

static void hal_camera_config_set_defaults(hal_camera_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    cfg->camera_number = CAMERA_NUMBER_DEFAULT;

    for (int i = 0; i < CAMERA_MAX_NUM; ++i) {
        cfg->interface_mode[i] = s_default_interface_mode[i];
        cfg->max_fps[i]        = s_default_max_fps[i];
        cfg->mirror[i]         = s_default_mirror[i];
        cfg->flip[i]           = s_default_flip[i];
        cfg->rotation[i]       = s_default_rotation[i];

        /* default ISP config path per camera */
        strncpy(cfg->isp_config_path[i],
                ISP_CONFIG_PATH_DEFAULT,
                sizeof(cfg->isp_config_path[i]) - 1);
        cfg->isp_config_path[i][sizeof(cfg->isp_config_path[i]) - 1] = '\0';
    }

    cfg->ipu_que_id    = IPU_QUE_ID_DEFAULT;
    cfg->ipu_module_id = IPU_MODULE_ID_DEFAULT;
    cfg->lazy_channel_create = LAZY_CHANNEL_CREATE_DEFAULT;

    cfg->af_enable = AF_ENABLE_DEFAULT;
    cfg->af_run_mode = AF_RUN_MODE_DEFAULT;
    cfg->focus_pos_min = FOCUS_POS_MIN_DEFAULT;
    cfg->focus_pos_max = FOCUS_POS_MAX_DEFAULT;
    cfg->af_settle_ms = AF_SETTLE_MS_DEFAULT;
    cfg->vcm_type[0] = '\0';
    cfg->vcm_subdev_path[0] = '\0';
}

static camera_ai_type_t parse_ai_type_token(const char *token)
{
    if (!token || !*token) {
        return CAMERA_AI_NONE;
    }

    for (size_t i = 0; i < AI_TYPE_MAP_SIZE; ++i) {
        if (strcmp(token, s_ai_type_map[i].name) == 0) {
            return s_ai_type_map[i].value;
        }
    }

    log_warn(LOG_TAG ": unknown ai type token '%s'", token);
    return CAMERA_AI_NONE;
}

static SVP_MODEL_E parse_svp_model_type(const char *str)
{
    if (!str || !*str) {
        return AK_SVP_MODEL_DETECT;
    }

    for (size_t i = 0; i < SVP_MODEL_MAP_SIZE; ++i) {
        if (strcmp(str, s_svp_model_map[i].name) == 0) {
            return (SVP_MODEL_E)s_svp_model_map[i].value;
        }
    }

    log_warn(LOG_TAG ": unknown svp model type '%s', using DETECT", str);
    return AK_SVP_MODEL_DETECT;
}

static SVP_IMG_TYPE_E parse_svp_img_type(const char *str)
{
    if (!str || !*str) {
        return AK_SVP_IMG_RGB_HWC;
    }

    for (size_t i = 0; i < SVP_IMG_MAP_SIZE; ++i) {
        if (strcmp(str, s_svp_img_map[i].name) == 0) {
            return (SVP_IMG_TYPE_E)s_svp_img_map[i].value;
        }
    }

    log_warn(LOG_TAG ": unknown svp img type '%s', using RGB_HWC", str);
    return AK_SVP_IMG_RGB_HWC;
}

static camera_ai_type_t parse_ai_type_mask(const char *str)
{
    if (!str || !*str) {
        return CAMERA_AI_NONE;
    }

    camera_ai_type_t mask = 0;
    char buf[256];
    strncpy(buf, str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *saveptr = NULL;
    char *token = strtok_r(buf, "|", &saveptr);
    while (token) {
        /* trim spaces */
        while (*token == ' ' || *token == '\t') {
            ++token;
        }
        char *end = token + strlen(token) - 1;
        while (end >= token && (*end == ' ' || *end == '\t')) {
            *end-- = '\0';
        }
        mask |= parse_ai_type_token(token);
        token = strtok_r(NULL, "|", &saveptr);
    }
    return mask;
}

static void hal_camera_config_load_from_ini(hal_camera_config_t *cfg)
{
    char path[256] = {0};

    /* Build path /etc/config/hal_camera_config.ini */
    if (config_build_path(CONFIG_PATH_TYPE_CONFIG,
                          CAMERA_LOADCONF_FILE_NAME,
                          path, sizeof(path)) != 0) {
        log_warn(LOG_TAG ": cannot build path for %s, using defaults", CAMERA_LOADCONF_FILE_NAME);
        return;
    }

    dictionary *ini = iniparser_load(path);
    if (!ini) {
        log_warn(LOG_TAG ": cannot load ini %s, using defaults", path);
        return;
    }

    /* Global section */
    cfg->camera_number = iniparser_dict_getint(ini, "camera", "number", cfg->camera_number);

    /* Per-camera sections: camera0, camera1, ... */
    for (int i = 0; i < cfg->camera_number && i < CAMERA_MAX_NUM; ++i) {
        char key[64];

        snprintf(key, sizeof(key), "camera%d:interface_mode", i);
        cfg->interface_mode[i] = iniparser_getint(ini, key, cfg->interface_mode[i]);

        snprintf(key, sizeof(key), "camera%d:max_fps", i);
        cfg->max_fps[i] = iniparser_getint(ini, key, cfg->max_fps[i]);

        snprintf(key, sizeof(key), "camera%d:mirror", i);
        cfg->mirror[i] = iniparser_getint(ini, key, cfg->mirror[i]);

        snprintf(key, sizeof(key), "camera%d:flip", i);
        cfg->flip[i] = iniparser_getint(ini, key, cfg->flip[i]);

        /* Optional: software IPU rotation. Missing key keeps default (0 = off),
         * so projects without this key (e.g. nova) are unaffected. */
        snprintf(key, sizeof(key), "camera%d:rotation", i);
        cfg->rotation[i] = iniparser_getint(ini, key, cfg->rotation[i]);

        /* Per-camera ISP config path: cameraN:isp_config_path */
        snprintf(key, sizeof(key), "camera%d:isp_config_path", i);
        const char *str = iniparser_getstring(ini, key, NULL);
        if (str && str[0] != '\0') {
            strncpy(cfg->isp_config_path[i], str, sizeof(cfg->isp_config_path[i]) - 1);
            cfg->isp_config_path[i][sizeof(cfg->isp_config_path[i]) - 1] = '\0';
        }
    }

    /* IPU config section */
    cfg->ipu_que_id = iniparser_dict_getint(ini, "ipu", "que_id", cfg->ipu_que_id);
    cfg->ipu_module_id = iniparser_dict_getint(ini, "ipu", "module_id", cfg->ipu_module_id);

    cfg->lazy_channel_create = iniparser_dict_getint(ini, "camera", "lazy_channel_create",
                                                       cfg->lazy_channel_create);

    /* [af] section — optional; default enable=0 so non-Nova products need no changes */
    cfg->af_enable = iniparser_dict_getint(ini, "af", "enable", cfg->af_enable);
    if (cfg->af_enable) {
        const char *af_mode = iniparser_dict_getstring(ini, "af", "mode", "kernel");
        if (af_mode && strcmp(af_mode, "user") == 0) {
            cfg->af_run_mode = AF_RUN_MODE_USER;
        } else {
            cfg->af_run_mode = AF_RUN_MODE_KERNEL;
        }
        cfg->focus_pos_min = iniparser_dict_getint(ini, "af", "focus_pos_min", cfg->focus_pos_min);
        cfg->focus_pos_max = iniparser_dict_getint(ini, "af", "focus_pos_max", cfg->focus_pos_max);
        cfg->af_settle_ms = iniparser_dict_getint(ini, "af", "af_settle_ms", cfg->af_settle_ms);
        if (cfg->focus_pos_max < cfg->focus_pos_min) {
            cfg->focus_pos_max = cfg->focus_pos_min;
        }
        const char *vcm_type = iniparser_dict_getstring(ini, "af", "vcm_type", NULL);
        if (vcm_type && vcm_type[0] != '\0') {
            strncpy(cfg->vcm_type, vcm_type, sizeof(cfg->vcm_type) - 1);
            cfg->vcm_type[sizeof(cfg->vcm_type) - 1] = '\0';
        }
        const char *subdev = iniparser_dict_getstring(ini, "af", "vcm_subdev_path", NULL);
        if (subdev && subdev[0] != '\0') {
            strncpy(cfg->vcm_subdev_path, subdev, sizeof(cfg->vcm_subdev_path) - 1);
            cfg->vcm_subdev_path[sizeof(cfg->vcm_subdev_path) - 1] = '\0';
        }
    }

    /* AI detect ↔ sub-type sections: ai_detect_sub0..N */
    s_ai_detect_sub_num = 0;
    for (int i = 0; i < CAMERA_AI_MAX_MODLE; ++i) {
        char section[64];
        snprintf(section, sizeof(section), "ai_detect_sub%d", i);

        char key_detect[80];
        char key_sub[80];
        snprintf(key_detect, sizeof(key_detect), "%s:detect_type", section);
        snprintf(key_sub, sizeof(key_sub), "%s:sub_type", section);

        const char *detect_str = iniparser_getstring(ini, key_detect, NULL);
        const char *sub_str    = iniparser_getstring(ini, key_sub, NULL);
        if (!detect_str && !sub_str) {
            continue;
        }

        hal_camera_ai_type_detect_sub_cfg_t *entry = &s_ai_detect_sub[s_ai_detect_sub_num];
        entry->detect_type = parse_ai_type_mask(detect_str);
        entry->sub_type    = parse_ai_type_mask(sub_str);

        if (entry->detect_type == CAMERA_AI_NONE && entry->sub_type == CAMERA_AI_NONE) {
            continue;
        }

        ++s_ai_detect_sub_num;
    }

    /* AI model sections: ai_model0..N */
    s_ai_models_num = 0;
    for (int i = 0; i < CAMERA_AI_MAX_MODLE; ++i) {
        char section[64];
        snprintf(section, sizeof(section), "ai_model%d", i);

        char key_path[80];
        snprintf(key_path, sizeof(key_path), "%s:model_path", section);
        const char *path_str = iniparser_getstring(ini, key_path, NULL);
        if (!path_str) {
            continue;
        }

        hal_camera_ai_modle_config_cfg_t *model = &s_ai_models[s_ai_models_num];
        memset(model, 0, sizeof(*model));

        strncpy(model->modle_name, path_str, sizeof(model->modle_name) - 1);

        char key_width[80];
        char key_height[80];
        char key_model_type[80];
        char key_img_type[80];
        char key_label_count[80];

        snprintf(key_width, sizeof(key_width), "%s:width", section);
        snprintf(key_height, sizeof(key_height), "%s:height", section);
        snprintf(key_model_type, sizeof(key_model_type), "%s:model_type", section);
        snprintf(key_img_type, sizeof(key_img_type), "%s:img_type", section);
        snprintf(key_label_count, sizeof(key_label_count), "%s:label_count", section);

        model->width  = iniparser_getint(ini, key_width, 0);
        model->height = iniparser_getint(ini, key_height, 0);

        const char *model_type_str = iniparser_getstring(ini, key_model_type, NULL);
        const char *img_type_str   = iniparser_getstring(ini, key_img_type, NULL);

        /* Parse model_type and img_type from string to enum */
        model->enModelType = parse_svp_model_type(model_type_str);
        model->enImgType   = parse_svp_img_type(img_type_str);

        int label_count = iniparser_getint(ini, key_label_count, 0);
        if (label_count < 0) {
            label_count = 0;
        }
        if (label_count > CAMERA_AI_MAX_LABLE_PER_MODEL) {
            label_count = CAMERA_AI_MAX_LABLE_PER_MODEL;
        }
        model->num_camera_ai_modle = label_count;

        for (int l = 0; l < label_count; ++l) {
            char key_label[80];
            snprintf(key_label, sizeof(key_label), "%s:label%d", section, l);
            const char *label_str = iniparser_getstring(ini, key_label, NULL);
            model->ai_type[l] = parse_ai_type_mask(label_str);
        }

        ++s_ai_models_num;
    }

    iniparser_freedict(ini);
}

const hal_camera_config_t *hal_camera_get_config(void)
{
    if (!s_cam_cfg_loaded) {
        hal_camera_config_set_defaults(&s_cam_cfg);
        hal_camera_config_load_from_ini(&s_cam_cfg);
        s_cam_cfg_loaded = 1;
    }
    return &s_cam_cfg;
}

const hal_camera_ai_type_detect_sub_cfg_t *
hal_camera_get_ai_detect_sub_table(int *num_entries_out)
{
    if (!s_cam_cfg_loaded) {
        hal_camera_config_set_defaults(&s_cam_cfg);
        hal_camera_config_load_from_ini(&s_cam_cfg);
        s_cam_cfg_loaded = 1;
    }
    if (num_entries_out) {
        *num_entries_out = s_ai_detect_sub_num;
    }
    return s_ai_detect_sub;
}

const hal_camera_ai_modle_config_cfg_t *
hal_camera_get_ai_model_table(int *num_models_out)
{
    if (!s_cam_cfg_loaded) {
        hal_camera_config_set_defaults(&s_cam_cfg);
        hal_camera_config_load_from_ini(&s_cam_cfg);
        s_cam_cfg_loaded = 1;
    }
    if (num_models_out) {
        *num_models_out = s_ai_models_num;
    }
    return s_ai_models;
}

int hal_camera_lazy_channel_create_enabled(void)
{
    return hal_camera_get_config()->lazy_channel_create ? 1 : 0;
}

