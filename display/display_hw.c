// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/display.
 *
 * Display module implementation.
 * - Provides display module initialization and management.
 * - Coordinates sub-modules like framebuffer device.
 */

#include "display.h"
#include "display_hw.h"
#include "display_error.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>

#include "log.h"
#include "config.h"

/*===========================================================================
 * Private Constants
 *===========================================================================*/

#define LOG_TAG "HW_DISPLAY"

/*===========================================================================
 * Public Functions
 *===========================================================================*/

/**
 * @brief Initialize display module.
 * @details Initializes all display sub-modules including framebuffer.
 * @return 0 on success, negative error code on failure.
 */
int hw_display_module_init(void)
{
    int ret = hw_fb_module_init();
    if (ret != HARDWARE_DISPLAY_OK) {
        return ret;
    }

    return HARDWARE_DISPLAY_OK;
}

/**
 * @brief Get display module by instance name.
 * @details Retrieves the display hardware module by its instance identifier.
 * @param module  Output pointer to the display module handle.
 * @param inst    Instance name identifier for the display module.
 * @return 0 on success, negative error code on failure.
 */
int hw_display_get_module(const struct hw_module_t **module, const char *inst)
{
    if (!module || !inst) {
        return HARDWARE_DISPLAY_ERROR_INVALID_PARAM;
    }

    int ret = hw_get_module_by_class(DISPLAY_HARDWARE_MODULE_ID, inst, module);
    if (ret != 0) {
        return HARDWARE_DISPLAY_ERROR_NOT_FOUND;
    }

    return HARDWARE_DISPLAY_OK;
}

display_color_swizzle_t display_get_color_swizzle(void)
{
#ifndef CONFIG_HW_DISPLAY_COLOR_SWIZZLE
    return DISPLAY_COLOR_SWIZZLE_NONE;
#else
    switch (CONFIG_HW_DISPLAY_COLOR_SWIZZLE) {
    case 1:
        return DISPLAY_COLOR_SWIZZLE_BGR;
    case 2:
        return DISPLAY_COLOR_SWIZZLE_GBR;
    case 3:
        return DISPLAY_COLOR_SWIZZLE_BRG;
    default:
        return DISPLAY_COLOR_SWIZZLE_NONE;
    }
#endif
}
