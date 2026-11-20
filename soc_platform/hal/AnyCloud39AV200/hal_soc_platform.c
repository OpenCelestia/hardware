// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/soc_platform.
 *
 * SoC platform specific initialization for AnyCloud39AV200.
 * - Implements platform-specific initialization and log level control.
 */


#define LOG_TAG "HAL SOC"

#include <stdlib.h>

#include "ak_common.h"
#include "ak_log.h"

/**
 * @brief Initialize platform HAL for AnyCloud39AV200.
 * @param mode Initialization mode (0: normal, 1: debug).
 * @return AK_SUCCESS on success, AK_FAILED on failure.
 */
int hal_soc_platform_init(int mode)
{
    int m = mode;
    if (m > 1)
        m = 0;

    SDK_RUN_CONFIG_S run_config = {0};

    run_config.s32MemTraceFlag = m;
    run_config.s32DmaMemTraceFlag = m;

    AK_BASE_SdkInit(&run_config);

    return AK_SUCCESS;
}

/**
 * @brief Set platform log level for AnyCloud39AV200.
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
    int l = level;
    if (l > LOG_LEVEL_DEBUG)
        l = LOG_LEVEL_NORMAL;

    /* Set log level */
    AK_BASE_PrintSetLevel(MODULE_ID_ALL, l);
    AK_BASE_PrintSetSyslogLevel(MODULE_ID_ALL, l);

    return AK_SUCCESS;
}
