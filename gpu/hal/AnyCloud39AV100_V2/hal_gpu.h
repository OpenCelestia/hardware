// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/gpu/hal/AnyCloud39AV100_V2.
 *
 * GPU HAL internal header for AnyCloud39AV100_V2 platform.
 * - Internal definitions for IPU-based GPU implementation.
 * - Defines constants and internal structures for GPU operations.
 */

#ifndef HARDWARE_HAL_GPU_HAL_ANYCLOUD39AV100_V2_HAL_GPU_H
#define HARDWARE_HAL_GPU_HAL_ANYCLOUD39AV100_V2_HAL_GPU_H

#include <stdint.h>
#include <stdbool.h>

#include "gpu.h"

__BEGIN_DECLS

/*===========================================================================
 * Internal Constants
 *===========================================================================*/

#define IPU_QUEUE_INDEX             0
#define IPU_QUEUE_SIZE              (8192)
#define IPU_RESP_SIZE               (8192)

#define SUPPORTED_MIN_WIDTH         16
#define SUPPORTED_MIN_HEIGHT        16
#define SUPPORTED_MAX_WIDTH         15360
#define SUPPORTED_MAX_HEIGHT        15360

#define IPU_OPA_MAX                 253

__END_DECLS

#endif /* HARDWARE_HAL_GPU_HAL_ANYCLOUD39AV100_V2_HAL_GPU_H */

