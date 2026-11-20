// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/driver/ir/hal/AnyCloud39AV200.
 *
 * IR HAL API interface for AnyCloud39AV200 platform.
 * - Defines HAL functions for IRCUT and IRLED operations.
 */
#ifndef HARDWARE_IR_HAL_HAL_ANYCLOUD39AV200_IR_HAL_H
#define HARDWARE_IR_HAL_HAL_ANYCLOUD39AV200_IR_HAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================
 * IRCUT HAL Interface
 *===========================================================================*/

/**
 * @brief Initialize IRCUT (HAL layer).
 * @param cfg_path  Configuration file path.
 * @return 0 on success, -1 on failure.
 */
int hal_ir_init(const char* cfg_path);

/**
 * @brief Set IRCUT status (HAL layer).
 * @param status  Status: 0=day mode, 1=night mode.
 * @return 0 on success, -1 on failure.
 */
int hal_ir_set_ircut(int status);

/**
 * @brief Set IRCUT switch time (HAL layer).
 * @param time_ms  Switch time in milliseconds.
 */
void hal_ir_set_ircut_time(int time_ms);

/**
 * @brief Get IRCUT switch time (HAL layer).
 * @param time_ms  Output parameter, switch time in milliseconds.
 * @return 0 on success, -1 on failure.
 */
int hal_ir_get_ircut_time(int* time_ms);

/*===========================================================================
 * IRLED HAL Interface
 *===========================================================================*/

/**
 * @brief IRLED initialization parameters (HAL layer).
 */
typedef struct {
    int working_level;  /**< Working level: 0=low active, 1=high active */
} hal_irled_param_t;

/**
 * @brief Initialize IRLED (HAL layer).
 * @param param  Initialization parameters.
 * @return 0 on success, -1 on failure.
 */
int hal_irled_init(const hal_irled_param_t* param);

/**
 * @brief Get IRLED status (HAL layer).
 * @param status  Output parameter, status: 0=off, 1=on.
 * @return 0 on success, -1 on failure.
 */
int hal_irled_get_status(int* status);

/**
 * @brief Set IRLED status (HAL layer).
 * @param status  Status: 0=off, 1=on.
 * @return 0 on success, -1 on failure.
 */
int hal_irled_set_status(int status);

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_IR_HAL_HAL_ANYCLOUD39AV200_IR_HAL_H */
