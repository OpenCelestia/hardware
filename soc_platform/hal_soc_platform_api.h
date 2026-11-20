// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/soc_platform.
 *
 * HAL API for SoC platform abstraction.
 * - Provides chip-specific initialization and log level control.
 */

#ifndef HARDWARE_HAL_SOC_PLATFORM_API_H
#define HARDWARE_HAL_SOC_PLATFORM_API_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize SoC platform HAL.
 * @param mode Initialization mode (0: normal, 1: debug).
 * @return 0 on success, non-zero on failure.
 */
int hal_soc_platform_init(int mode);

/**
 * @brief Set SoC platform HAL log level.
 * @param level Log level value (LOG_LEVEL_* defined by underlying SDK).\n
 * @return 0 on success, non-zero on failure.
 */
int hal_soc_platform_set_log_level(int level);

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_HAL_SOC_PLATFORM_API_H */


