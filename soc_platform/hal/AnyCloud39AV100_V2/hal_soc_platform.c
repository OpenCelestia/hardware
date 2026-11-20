// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/soc_platform.
 *
 * SoC platform specific initialization for AnyCloud39AV100_V2.
 * - Implements platform-specific initialization and log level control.
 * - Init sequence follows ActionCam/pdk2.04/platform/sample/vi/ak_vi_sample.c ak_sdk_init.
 */

#define LOG_TAG "HAL SOC"

#include <stdlib.h>
#include <string.h>

#include "ak_common.h"
#include "ak_log.h"

/**
 * @brief Initialize platform HAL for AnyCloud39AV100_V2.
 * @param mode Initialization mode (0: normal, 1: debug).
 * @return AK_SUCCESS on success, AK_FAILED on failure.
 */
int hal_soc_platform_init(int mode)
{
    if (mode != 0 && mode != 1)
        return AK_FAILED;

    sdk_run_config run_config;
    memset(&run_config, 0, sizeof(run_config));
    run_config.mem_trace_flag = (mode != 0) ? SDK_RUN_DEBUG : SDK_RUN_NORMAL;
    run_config.dma_mem_trace_flag = mode;
    run_config.audio_tool_server_flag = 0;
    run_config.isp_tool_server_flag = 0;

    if (ak_sdk_init(&run_config) != AK_SUCCESS)
        return AK_FAILED;

    return AK_SUCCESS;
}

/**
 * @brief Set platform log level for AnyCloud39AV100_V2.
 * @param level Log level of platform:\n
 *              1: LOG_LEVEL_ERROR\n
 *              2: LOG_LEVEL_WARNING\n
 *              3: LOG_LEVEL_NOTICE\n
 *              4: LOG_LEVEL_NORMAL\n
 *              5: LOG_LEVEL_INFO\n
 *              6: LOG_LEVEL_DEBUG\n
 * @return AK_SUCCESS on success, AK_FAILED on failure.
 */
int hal_soc_platform_set_log_level(int level)
{
    if (level < LOG_LEVEL_ERROR || level > LOG_LEVEL_DEBUG)
        return AK_FAILED;

    ak_print_set_level(MODULE_ID_ALL, level);
    ak_print_set_syslog_level(MODULE_ID_ALL, level);

    return AK_SUCCESS;
}
