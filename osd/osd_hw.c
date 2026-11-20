// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/osd.
 *
 * OSD module implementation.
 * - Implements osd_hw_device_t methods for OSD operations.
 * - Provides text rendering, bitmap drawing and box overlay operations.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/cdefs.h>
#include <sys/types.h>
#include <stdbool.h>

#include "log.h"
#include "hardware.h"
#include "osd.h"
#include "osd_hw.h"
#include "hal_osd_api.h"

/*===========================================================================
 * Private Constants
 *===========================================================================*/

#define LOG_TAG "HW_OSD"

/*===========================================================================
 * Private Type Definitions
 *===========================================================================*/

/**
 * @brief OSD device internal structure.
 */
struct osd_device {
    osd_hw_device_t device;             /**< OSD hardware device. */
    void *priv;                         /**< Private data. */
};

/*===========================================================================
 * Device Method Implementations
 *===========================================================================*/

/**
 * @brief Close OSD device.
 */
static int osd_dev_close(hw_device_t *device)
{
    log_debug("osd_dev_close");
    if (!device) {
        return HARDWARE_OSD_ERROR_INVALID_PARAM;
    }

    struct osd_device *odev = (struct osd_device *)device;
    free(odev);

    return HARDWARE_OSD_OK;
}

/**
 * @brief Initialize OSD device.
 */
static int osd_dev_init(const struct osd_hw_device *dev, osd_attr_t *attr, const char *font_file)
{
    if (!dev) {
        log_error("device is NULL");
        return HARDWARE_OSD_ERROR_INVALID_PARAM;
    }

    return osd_status_from_hal(hal_osd_init(attr, font_file));
}

/**
 * @brief Deinitialize OSD device.
 */
static int osd_dev_deinit(const struct osd_hw_device *dev)
{
    if (!dev) {
        log_error("device is NULL");
        return HARDWARE_OSD_ERROR_INVALID_PARAM;
    }

    return osd_status_from_hal(hal_osd_deinit());
}

/**
 * @brief Draw bitmap to memory canvas.
 */
static int osd_dev_draw_bmp(const struct osd_hw_device *dev, int nChn, int nObj,
                            osd_rect_info *rc, const unsigned char *pData, int iDataSize)
{
    if (!dev) {
        log_error("device is NULL");
        return HARDWARE_OSD_ERROR_INVALID_PARAM;
    }

    return osd_status_from_hal(hal_osd_draw_bmp(nChn, nObj, rc, pData, iDataSize));
}

/**
 * @brief Clear OSD objects.
 */
static int osd_dev_clean_obj(const struct osd_hw_device *dev, int nChn, int nObj)
{
    if (!dev) {
        log_error("device is NULL");
        return HARDWARE_OSD_ERROR_INVALID_PARAM;
    }

    return osd_status_from_hal(hal_osd_clean_obj(nChn, nObj));
}

/**
 * @brief Copy canvas to VI.
 */
static int osd_dev_copy_cavas_to_vi(const struct osd_hw_device *dev, int nChn)
{
    if (!dev) {
        log_error("device is NULL");
        return HARDWARE_OSD_ERROR_INVALID_PARAM;
    }

    return osd_status_from_hal(hal_osd_copy_cavas_to_vi(nChn));
}

/**
 * @brief Set font for text rendering.
 */
static int osd_dev_set_font(const struct osd_hw_device *dev, int nChn, osd_font_info *pFont)
{
    if (!dev) {
        log_error("device is NULL");
        return HARDWARE_OSD_ERROR_INVALID_PARAM;
    }

    return osd_status_from_hal(hal_osd_set_font(nChn, pFont));
}

/**
 * @brief Get text width and height.
 */
static int osd_dev_get_text_wh(const struct osd_hw_device *dev, int nChn, const char *cText,
                               int cTextLen, int *pWidth, int *pHeight)
{
    if (!dev) {
        log_error("device is NULL");
        return HARDWARE_OSD_ERROR_INVALID_PARAM;
    }

    return osd_status_from_hal(hal_osd_get_text_wh(nChn, cText, cTextLen, pWidth, pHeight));
}

/**
 * @brief Draw text to memory canvas.
 */
static int osd_dev_draw_text(const struct osd_hw_device *dev, int nChn, int nObj, int x, int y,
                             const char *cText, int iTextLen)
{
    if (!dev) {
        log_error("device is NULL");
        return HARDWARE_OSD_ERROR_INVALID_PARAM;
    }

    return osd_status_from_hal(hal_osd_draw_text(nChn, nObj, x, y, cText, iTextLen));
}

/**
 * @brief Set channel attributes.
 */
static int osd_dev_set_attr(const struct osd_hw_device *dev, int nChn, osd_chn_info *pChnInfo)
{
    if (!dev) {
        log_error("device is NULL");
        return HARDWARE_OSD_ERROR_INVALID_PARAM;
    }

    return osd_status_from_hal(hal_osd_set_attr(nChn, pChnInfo));
}

/**
 * @brief Set enable state.
 */
static int osd_dev_set_enable(const struct osd_hw_device *dev, int nChn, int nObj, int bEnable)
{
    if (!dev) {
        log_error("device is NULL");
        return HARDWARE_OSD_ERROR_INVALID_PARAM;
    }

    return osd_status_from_hal(hal_osd_set_enable(nChn, nObj, bEnable));
}

/**
 * @brief Draw box overlay.
 */
static int osd_dev_draw_box(const struct osd_hw_device *dev, int nChn, pp_box_group_t *pstBoxGroup)
{
    if (!dev) {
        log_error("device is NULL");
        return HARDWARE_OSD_ERROR_INVALID_PARAM;
    }

    return osd_status_from_hal(hal_osd_draw_box(nChn, pstBoxGroup));
}

/**
 * @brief Adjust box coordinates based on video resolution.
 */
static int osd_dev_adjust_box_coordinates(const struct osd_hw_device *dev, int nChn,
                                          pp_box_group_t *pstBoxGroup, int video_width, int video_height)
{
    if (!dev) {
        log_error("device is NULL");
        return HARDWARE_OSD_ERROR_INVALID_PARAM;
    }

    return osd_status_from_hal(hal_osd_adjust_box_coordinates(nChn, pstBoxGroup, video_width, video_height));
}

static int osd_dev_set_rotation(const struct osd_hw_device *dev, int nChn, int rotation_deg,
                                int display_width, int display_height)
{
    if (!dev) {
        log_error("device is NULL");
        return HARDWARE_OSD_ERROR_INVALID_PARAM;
    }

    return osd_status_from_hal(hal_osd_set_rotation(nChn, rotation_deg, display_width, display_height));
}

/*===========================================================================
 * Module Implementation
 *===========================================================================*/

/**
 * @brief Open OSD device.
 * @details Creates and initializes an osd_hw_device instance.
 * @param module  Pointer to the OSD hardware module.
 * @param name    Device name, must be OSD_HARDWARE_INTERFACE.
 * @param device  Output pointer to the opened device handle.
 * @return 0 on success, negative error code on failure.
 */
static int osd_dev_open(const hw_module_t *module, const char *name, hw_device_t **device)
{
    log_debug("osd_dev_open: %s", name);

    if (!module || !device) {
        log_error("invalid parameters");
        return HARDWARE_OSD_ERROR_INVALID_PARAM;
    }

    if (!name || strcmp(name, OSD_HARDWARE_INTERFACE) != 0) {
        log_error("Invalid interface name: %s", name ? name : "null");
        return HARDWARE_OSD_ERROR_NOT_SUPPORTED;
    }

    struct osd_device *odev = calloc(1, sizeof(struct osd_device));
    if (!odev) {
        log_error("Failed to allocate osd_device");
        return HARDWARE_OSD_ERROR_MEMORY;
    }

    /* Initialize device structure */
    odev->device.common.tag = HARDWARE_DEVICE_TAG;
    odev->device.common.version = OSD_DEVICE_API_VERSION_1_0;
    odev->device.common.module = (struct hw_module_t *)module;
    odev->device.common.close = osd_dev_close;

    /* Assign device methods */
    odev->device.init = osd_dev_init;
    odev->device.deinit = osd_dev_deinit;
    odev->device.draw_bmp = osd_dev_draw_bmp;
    odev->device.clean_obj = osd_dev_clean_obj;
    odev->device.copy_cavas_to_vi = osd_dev_copy_cavas_to_vi;
    odev->device.set_font = osd_dev_set_font;
    odev->device.get_text_wh = osd_dev_get_text_wh;
    odev->device.draw_text = osd_dev_draw_text;
    odev->device.set_attr = osd_dev_set_attr;
    odev->device.set_enable = osd_dev_set_enable;
    odev->device.draw_box = osd_dev_draw_box;
    odev->device.adjust_box_coordinates = osd_dev_adjust_box_coordinates;
    odev->device.set_rotation = osd_dev_set_rotation;

    *device = &odev->device.common;

    log_debug("osd device opened successfully");

    return HARDWARE_OSD_OK;
}

/*===========================================================================
 * Module Definition
 *===========================================================================*/

static struct hw_module_methods_t hw_osd_module_methods = {
    .open = osd_dev_open,
};

static struct osd_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = OSD_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = OSD_HARDWARE_MODULE_ID,
        .name = "OSD Module",
        .author = "Anyka",
        .methods = &hw_osd_module_methods,
        .dso = NULL,
        .reserved = {0}
    },
    .reserved = {0}
};

/*===========================================================================
 * Public Functions
 *===========================================================================*/

/* Resolve SoC-dependent OSD attributes (see osd.h for the public contract). */
int osd_get_platform_attrs(osd_platform_attrs_t *attrs)
{
    if (!attrs) {
        return HARDWARE_OSD_ERROR_INVALID_PARAM;
    }
    attrs->char_interval = 0;
#if defined(AnyCloud39EV330) || defined(PLATFORM_EV33X)
    attrs->char_interval = 2;
#endif
    return HARDWARE_OSD_OK;
}

/**
 * @brief Initialize OSD hardware module.
 * @details Registers the OSD module with the hardware module system.
 * @return 0 on success, negative error code on failure.
 */
int hw_osd_module_init(void)
{
    int ret = hw_register_module(&(HAL_MODULE_INFO_SYM.common));
    return (ret == 0) ? HARDWARE_OSD_OK : HARDWARE_OSD_ERROR_OPERATION_FAILED;
}

