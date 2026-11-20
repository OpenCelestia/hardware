// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/gpu.
 *
 * GPU module internal header.
 * - Internal definitions for GPU module implementation.
 * - Provides HAL return code conversion utilities.
 */

#ifndef HARDWARE_GPU_HW_H
#define HARDWARE_GPU_HW_H

#include <stdint.h>
#include <stdbool.h>

#include "gpu.h"

__BEGIN_DECLS

/*===========================================================================
 * Internal Function Declarations
 *===========================================================================*/

/**
 * @brief Convert HAL return code to GPU error code.
 * @param ret  HAL return code to convert.
 * @return GPU error code (HARDWARE_GPU_OK or HARDWARE_GPU_ERROR_OPERATION_FAILED).
 */
static inline int gpu_status_from_hal(int ret)
{
    return (ret == 0) ? HARDWARE_GPU_OK : HARDWARE_GPU_ERROR_OPERATION_FAILED;
}

__END_DECLS

#endif /* HARDWARE_GPU_HW_H */

