// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/vision/motion_detect.
 *
 * INI-based configuration implementation for motion detection.
 * - Implements helpers to load and save motion detection configuration from/to INI files.
 * - Shared by motion_detect_hw.c.
 */

#include "motion_detect_config.h"

#include "log.h"
#include "system_config.h"

#include <string.h>

#define LOG_TAG "MOTION_DETECT"

int motion_detect_config_load_from_ini(motion_detect_config_t *config,
                                       double sensitivity_table[5])
{
    if (!config || !sensitivity_table) {
        return HARDWARE_MOTION_DETECT_ERROR_INVALID_PARAM;
    }

    dictionary *ini = NULL;
    char szFilePath[256] = {0};

    if (config_build_file_path(MOTION_DETECT_SETTING_FILE,
                               szFilePath,
                               sizeof(szFilePath)) != 0) {
        log_error("Failed to build config file path");
        return HARDWARE_MOTION_DETECT_ERROR_OPERATION_FAILED;
    }

    ini = iniparser_load(szFilePath);
    if (!ini) {
        /* File doesn't exist, use default values */
        log_info("Config file not found, using defaults: %s", szFilePath);

        config->enable = 0;
        config->sensitivity = 60;
        config->valid_area.x = MOTION_DETECT_AREA_MIN;
        config->valid_area.y = MOTION_DETECT_AREA_MIN;
        config->valid_area.width = MOTION_DETECT_AREA_MAX;
        config->valid_area.height = MOTION_DETECT_AREA_MAX;

        sensitivity_table[0] = 0.15;
        sensitivity_table[1] = 0.10;
        sensitivity_table[2] = 0.06;
        sensitivity_table[3] = 0.04;
        sensitivity_table[4] = 0.02;

        log_info("MD config (default): enable=%d, sensitivity=%d",
                 config->enable, config->sensitivity);
        return HARDWARE_MOTION_DETECT_OK;
    }

    config->enable = iniparser_dict_getint(ini, "motiondetect", "enable", 0);
    config->sensitivity = iniparser_dict_getint(ini, "motiondetect", "sensitity", 60);

    config->valid_area.x =
        iniparser_dict_getint(ini, "valid_rect", "posx", MOTION_DETECT_AREA_MIN);
    config->valid_area.y =
        iniparser_dict_getint(ini, "valid_rect", "posy", MOTION_DETECT_AREA_MIN);
    config->valid_area.width =
        iniparser_dict_getint(ini, "valid_rect", "width", MOTION_DETECT_AREA_MAX);
    config->valid_area.height =
        iniparser_dict_getint(ini, "valid_rect", "height", MOTION_DETECT_AREA_MAX);

    sensitivity_table[0] =
        iniparser_dict_getdouble(ini, "motiondetect", "level0", 0.15);
    sensitivity_table[1] =
        iniparser_dict_getdouble(ini, "motiondetect", "level1", 0.10);
    sensitivity_table[2] =
        iniparser_dict_getdouble(ini, "motiondetect", "level2", 0.06);
    sensitivity_table[3] =
        iniparser_dict_getdouble(ini, "motiondetect", "level3", 0.04);
    sensitivity_table[4] =
        iniparser_dict_getdouble(ini, "motiondetect", "level4", 0.02);

    iniparser_freedict(ini);

    log_info("MD config: enable=%d, sensitivity=%d",
             config->enable,
             config->sensitivity);

    return HARDWARE_MOTION_DETECT_OK;
}

int motion_detect_config_save_to_ini(const motion_detect_config_t *config)
{
    if (!config) {
        return HARDWARE_MOTION_DETECT_ERROR_INVALID_PARAM;
    }

    dictionary *ini = NULL;
    char szFilePath[256] = {0};

    if (config_build_file_path(MOTION_DETECT_SETTING_FILE,
                               szFilePath,
                               sizeof(szFilePath)) != 0) {
        log_error("Failed to build config file path");
        return HARDWARE_MOTION_DETECT_ERROR_OPERATION_FAILED;
    }

    ini = iniparser_load(szFilePath);
    if (!ini) {
        log_error("Cannot load file for write: %s", szFilePath);
        return HARDWARE_MOTION_DETECT_ERROR_OPERATION_FAILED;
    }

    iniparser_seti(ini, "motiondetect:enable", config->enable);
    iniparser_seti(ini, "motiondetect:sensitity", config->sensitivity);
    iniparser_seti(ini, "valid_rect:posx", config->valid_area.x);
    iniparser_seti(ini, "valid_rect:posy", config->valid_area.y);
    iniparser_seti(ini, "valid_rect:width", config->valid_area.width);
    iniparser_seti(ini, "valid_rect:height", config->valid_area.height);

    iniparser_save_file(ini, szFilePath);
    iniparser_freedict(ini);

    return HARDWARE_MOTION_DETECT_OK;
}


