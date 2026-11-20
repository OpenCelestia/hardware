// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/soc_platform.
 *
 * SoC platform abstraction public interface.
 * - Provides high-level init and log-level control APIs.
 * - Hides chip-specific details behind hal_soc_platform_* in HAL layer.
 */
#ifndef HARDWARE_SOC_PLATFORM_H
#define HARDWARE_SOC_PLATFORM_H

#include <stdint.h>
#include "system_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================
 * Error Code Definitions
 *===========================================================================*/

/**
 * @brief SoC platform module error codes.
 * @details Error codes for SoC platform module operations.
 */
#define SOC_PLATFORM_OK                           SYSTEM_SUCCESS
#define SOC_PLATFORM_ERROR_INVALID_PARAM          SYSTEM_ERROR_CODE(SYSTEM_DOMAIN, SYSTEM_MODULE_SOC_PLATFORM, ERROR_INVALID_PARAM)
#define SOC_PLATFORM_ERROR_NOT_SUPPORTED          SYSTEM_ERROR_CODE(SYSTEM_DOMAIN, SYSTEM_MODULE_SOC_PLATFORM, ERROR_NOT_SUPPORTED)
#define SOC_PLATFORM_ERROR_OPERATION_FAILED       SYSTEM_ERROR_CODE(SYSTEM_DOMAIN, SYSTEM_MODULE_SOC_PLATFORM, ERROR_OPERATION_FAILED)

/* SoC Platform Module Specific Error Codes */
#define SOC_PLATFORM_ERROR_PLATFORM_NOT_SUPPORTED SYSTEM_ERROR_CODE(SYSTEM_DOMAIN, SYSTEM_MODULE_SOC_PLATFORM, 0x0017)
#define SOC_PLATFORM_ERROR_HAL_INIT_FAILED        SYSTEM_ERROR_CODE(SYSTEM_DOMAIN, SYSTEM_MODULE_SOC_PLATFORM, 0x0018)
#define SOC_PLATFORM_ERROR_HAL_DEINIT_FAILED      SYSTEM_ERROR_CODE(SYSTEM_DOMAIN, SYSTEM_MODULE_SOC_PLATFORM, 0x0019)

/**
 * @brief SoC platform error type.
 */
typedef system_error_t soc_platform_error_t;

/*===========================================================================
 * API Functions
 *===========================================================================*/

/**
 * @brief Initialize SoC platform.
 *
 * @param mode Initialization mode (0: normal, 1: debug).
 * @return SOC_PLATFORM_OK on success, SOC_PLATFORM_ERROR_OPERATION_FAILED on failure.
 */
int soc_platform_init(int mode);

/**
 * @brief Set SoC platform log level.
 *
 * @param level Log level of platform:
 *              1: LOG_LEVEL_ERROR
 *              2: LOG_LEVEL_WARNING
 *              3: LOG_LEVEL_NOTICE
 *              4: LOG_LEVEL_NORMAL
 *              5: LOG_LEVEL_INFO
 *              6: LOG_LEVEL_DEBUG
 * @return SOC_PLATFORM_OK on success, SOC_PLATFORM_ERROR_INVALID_PARAM on invalid parameter or failure.
 */
int soc_platform_set_log_level(int level);

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_SOC_PLATFORM_H */

