// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/osd.
 *
 * OSD module internal header.
 * - Internal definitions for OSD module implementation.
 * - Provides HAL return code conversion utilities.
 */

#ifndef HARDWARE_OSD_HW_H
#define HARDWARE_OSD_HW_H

#include <stdint.h>
#include <stdbool.h>

#include "osd.h"

__BEGIN_DECLS

/*===========================================================================
 * Internal Function Declarations
 *===========================================================================*/

/**
 * @brief Convert HAL return code to OSD error code.
 * @param ret  HAL return code to convert.
 * @return OSD error code (HARDWARE_OSD_OK or HARDWARE_OSD_ERROR_OPERATION_FAILED).
 */
static inline int osd_status_from_hal(int ret)
{
    return (ret == 0) ? HARDWARE_OSD_OK : HARDWARE_OSD_ERROR_OPERATION_FAILED;
}

__END_DECLS

#endif /* HARDWARE_OSD_HW_H */

