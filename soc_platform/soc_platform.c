// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/soc_platform.
 *
 * SoC platform high-level wrapper APIs.
 * - Provides high-level interface for SoC platform operations.
 */

#include "hal_soc_platform_api.h"
#include "soc_platform.h"

/**
 * @brief Initialize SoC platform.
 * @param mode Initialization mode (0: normal, 1: debug).
 * @return SOC_PLATFORM_OK on success, SOC_PLATFORM_ERROR_OPERATION_FAILED on failure.
 */
int soc_platform_init(int mode)
{
    int ret = hal_soc_platform_init(mode);
    return (ret == 0) ? SOC_PLATFORM_OK : SOC_PLATFORM_ERROR_OPERATION_FAILED;
}

/**
 * @brief Set SoC platform log level.
 * @param level Log level value from HAL layer (LOG_LEVEL_*).
 * @return SOC_PLATFORM_OK on success, SOC_PLATFORM_ERROR_INVALID_PARAM on invalid parameter or failure.
 */
int soc_platform_set_log_level(int level)
{
   int ret = hal_soc_platform_set_log_level(level);
   return (ret == 0) ? SOC_PLATFORM_OK : SOC_PLATFORM_ERROR_INVALID_PARAM;
}

