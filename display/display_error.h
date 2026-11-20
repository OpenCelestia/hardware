// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/display.
 *
 * Display module error code definitions.
 * - Defines error codes for display module operations.
 * - Uses unified error code system from system_error.h.
 */

#ifndef HARDWARE_DISPLAY_ERROR_H
#define HARDWARE_DISPLAY_ERROR_H

#include "system_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================
 * Common Error Codes
 *===========================================================================*/

#define HARDWARE_DISPLAY_OK                        SYSTEM_SUCCESS
#define HARDWARE_DISPLAY_ERROR_INVALID_PARAM       SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_DISPLAY, ERROR_INVALID_PARAM)
#define HARDWARE_DISPLAY_ERROR_NOT_INIT            SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_DISPLAY, ERROR_NOT_INIT)
#define HARDWARE_DISPLAY_ERROR_MEMORY              SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_DISPLAY, ERROR_MEMORY)
#define HARDWARE_DISPLAY_ERROR_IO                  SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_DISPLAY, ERROR_IO)
#define HARDWARE_DISPLAY_ERROR_NOT_FOUND           SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_DISPLAY, ERROR_NOT_FOUND)
#define HARDWARE_DISPLAY_ERROR_INVALID_STATE       SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_DISPLAY, ERROR_INVALID_STATE)
#define HARDWARE_DISPLAY_ERROR_OPERATION_FAILED    SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_DISPLAY, ERROR_OPERATION_FAILED)
#define HARDWARE_DISPLAY_ERROR_NOT_SUPPORTED       SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_DISPLAY, ERROR_NOT_SUPPORTED)

/*===========================================================================
 * Display-Specific Error Codes (0x0017+)
 *===========================================================================*/

#define HARDWARE_DISPLAY_ERROR_DEVICE_BUSY         SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_DISPLAY, 0x0017)
#define HARDWARE_DISPLAY_ERROR_MAP_FAILED          SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_DISPLAY, 0x0018)
#define HARDWARE_DISPLAY_ERROR_FLUSH_FAILED        SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_DISPLAY, 0x0019)

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_DISPLAY_ERROR_H */


