// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/osd.
 *
 * OSD (On-Screen Display) module public interface.
 * - Android-style HAL interface for OSD hardware devices.
 * - Provides text rendering, bitmap drawing and box overlay operations.
 */

#ifndef HARDWARE_OSD_H
#define HARDWARE_OSD_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/cdefs.h>

#include "hardware.h"
#include "system_error.h"

__BEGIN_DECLS

/*===========================================================================
 * Module Version Definitions
 *===========================================================================*/

#define OSD_MODULE_API_VERSION_0_1      HARDWARE_MODULE_API_VERSION(0, 1)
#define OSD_MODULE_API_VERSION_CURRENT  OSD_MODULE_API_VERSION_0_1
#define OSD_DEVICE_API_VERSION_1_0      HARDWARE_DEVICE_API_VERSION(1, 0)

/*===========================================================================
 * Module ID and Device Definitions
 *===========================================================================*/

/** @brief OSD hardware module ID. */
#define OSD_HARDWARE_MODULE_ID          "osd"

/** @brief OSD hardware interface name. */
#define OSD_HARDWARE_INTERFACE          "osd_hw_if"

/*===========================================================================
 * Constants
 *===========================================================================*/

/** @brief Maximum number of OSD objects per channel. */
#define OSD_OBJ_COUNT                   8

/** @brief Maximum text length for OSD text. */
#define OSD_TEXT_LEN                    100

/** @brief Maximum number of boxes in a box group. */
#define OSD_BOX_MAX_NUM                 50

/*===========================================================================
 * Type Definitions
 *===========================================================================*/

/**
 * @brief OSD color mode enumeration.
 */
typedef enum {
    OSD_COLOR_MODE_RGB1 = 1,            /**< 1-bit RGB color mode. */
    OSD_COLOR_MODE_RGB2 = 2,            /**< 2-bit RGB color mode. */
    OSD_COLOR_MODE_RGB4 = 4,            /**< 4-bit RGB color mode. */
} osd_color_mode_e;

/**
 * @brief OSD attribute structure.
 */
typedef struct osd_attr_s {
    osd_color_mode_e color_mode;        /**< Color mode (YUV or RGB, 1/2/4 bit width). */
    unsigned int *pu32ColorTable;       /**< Color table pointer, can be NULL. */
} osd_attr_t;

/**
 * @brief OSD rectangle information structure.
 * @note Uses x, y, width, height to match system/core rect_t convention
 */
typedef struct osd_rect_info_s {
    int x;                              /**< X position. */
    int y;                              /**< Y position. */
    int width;                          /**< Rectangle width. */
    int height;                         /**< Rectangle height. */
} osd_rect_info;

/**
 * @brief Bitmap information structure.
 */
typedef struct osd_bitmap_info_s {
    unsigned char *pData;               /**< Bitmap data pointer. */
    int iDataSize;                      /**< Bitmap data size in bytes. */
    int iWidth;                         /**< Bitmap width in pixels. */
    int iHeight;                        /**< Bitmap height in pixels. */
    int iBitPerPixel;                   /**< Bits per pixel (bit depth). */
} osd_bitmap_info;

/**
 * @brief Font information structure.
 */
typedef struct osd_font_info_s {
    char cFontName[256];                /**< Font file name. */
    int iFontSize;                      /**< Font size. */
    int iColor;                         /**< Font color (ARGB for 32-bit, index for palette). */
    int iBKClor;                        /**< Background color (ARGB for 32-bit, index for palette). */
} osd_font_info;

/**
 * @brief Channel information structure.
 */
typedef struct osd_chn_info_s {
    int iBitPerPixel;                   /**< Bits per pixel (bit depth). */
    unsigned int *pu32ColorTable;       /**< Color palette pointer. */
} osd_chn_info;

/**
 * @brief PP box group type (forward declaration, actual definition in platform HAL).
 */
typedef void pp_box_group_t;

/*===========================================================================
 * Device Structure Definitions
 *===========================================================================*/

/**
 * @brief OSD hardware device structure.
 * @details Defines all operation interfaces for OSD hardware device.
 */
typedef struct osd_hw_device {
    hw_device_t common;                 /**< Common hardware device interface. */

    /**
     * @brief Initialize OSD.
     * @param dev       OSD hardware device.
     * @param attr      OSD attributes (can be NULL for defaults).
     * @param font_file Font file path.
     * @return 0 on success, negative error code on failure.
     */
    int (*init)(const struct osd_hw_device *dev, osd_attr_t *attr, const char *font_file);

    /**
     * @brief Deinitialize OSD.
     * @param dev  OSD hardware device.
     * @return 0 on success, negative error code on failure.
     */
    int (*deinit)(const struct osd_hw_device *dev);

    /**
     * @brief Draw bitmap to memory canvas.
     * @param dev        OSD hardware device.
     * @param nChn       Target VI channel number.
     * @param nObj       Target canvas region number.
     * @param rc         Bitmap coordinates.
     * @param pData      Bitmap data.
     * @param iDataSize  Bitmap data size in bytes.
     * @return 0 on success, negative error code on failure.
     */
    int (*draw_bmp)(const struct osd_hw_device *dev, int nChn, int nObj,
                    osd_rect_info *rc, const unsigned char *pData, int iDataSize);

    /**
     * @brief Clear bitmap or text objects.
     * @param dev   OSD hardware device.
     * @param nChn  Channel number.
     * @param nObj  Object number (-1 to clear all).
     * @return 0 on success, negative error code on failure.
     */
    int (*clean_obj)(const struct osd_hw_device *dev, int nChn, int nObj);

    /**
     * @brief Copy canvas to hardware OSD module.
     * @param dev   OSD hardware device.
     * @param nChn  Channel number.
     * @return 0 on success, negative error code on failure.
     */
    int (*copy_cavas_to_vi)(const struct osd_hw_device *dev, int nChn);

    /**
     * @brief Set font for text rendering.
     * @param dev    OSD hardware device.
     * @param nChn   Channel number.
     * @param pFont  Font information.
     * @return 0 on success, negative error code on failure.
     */
    int (*set_font)(const struct osd_hw_device *dev, int nChn, osd_font_info *pFont);

    /**
     * @brief Get text width and height in pixels.
     * @param dev       OSD hardware device.
     * @param nChn      Channel number.
     * @param cText     Text content.
     * @param cTextLen  Text length.
     * @param pWidth    Output width pointer.
     * @param pHeight   Output height pointer.
     * @return 0 on success, negative error code on failure.
     */
    int (*get_text_wh)(const struct osd_hw_device *dev, int nChn, const char *cText,
                       int cTextLen, int *pWidth, int *pHeight);

    /**
     * @brief Draw text to memory canvas.
     * @param dev       OSD hardware device.
     * @param nChn      Channel number.
     * @param nObj      Object number.
     * @param x         X coordinate.
     * @param y         Y coordinate.
     * @param cText     Text content.
     * @param iTextLen  Text length.
     * @return 0 on success, negative error code on failure.
     */
    int (*draw_text)(const struct osd_hw_device *dev, int nChn, int nObj, int x, int y,
                     const char *cText, int iTextLen);

    /**
     * @brief Set channel attributes.
     * @param dev       OSD hardware device.
     * @param nChn      Channel number.
     * @param pChnInfo  Channel information.
     * @return 0 on success, negative error code on failure.
     */
    int (*set_attr)(const struct osd_hw_device *dev, int nChn, osd_chn_info *pChnInfo);

    /**
     * @brief Set enable state.
     * @param dev      OSD hardware device.
     * @param nChn     Channel number.
     * @param nObj     Object number (-1 for all objects).
     * @param bEnable  Enable flag.
     * @return 0 on success, negative error code on failure.
     */
    int (*set_enable)(const struct osd_hw_device *dev, int nChn, int nObj, int bEnable);

    /**
     * @brief Draw box overlay.
     * @param dev          OSD hardware device.
     * @param nChn         Channel number.
     * @param pstBoxGroup  Box group information.
     * @return 0 on success, negative error code on failure.
     */
    int (*draw_box)(const struct osd_hw_device *dev, int nChn, pp_box_group_t *pstBoxGroup);

    /**
     * @brief Adjust box coordinates based on video resolution.
     * @param nChn         VPSS channel index.
     * @param pstBoxGroup  Box group information.
     * @param video_width  Display (post-rotation) width in pixels.
     * @param video_height Display (post-rotation) height in pixels.
     */
    int (*adjust_box_coordinates)(const struct osd_hw_device *dev, int nChn,
                                  pp_box_group_t *pstBoxGroup, int video_width, int video_height);

    /**
     * @brief Set display rotation for a VPSS channel (OSD/box pre-compensation).
     */
    int (*set_rotation)(const struct osd_hw_device *dev, int nChn, int rotation_deg,
                        int display_width, int display_height);

    void *reserved[3];                  /**< Reserved fields. */
    void *priv;                         /**< Private data. */
} osd_hw_device_t;

/**
 * @brief OSD module structure.
 * @details OSD module interface for device management operations.
 */
typedef struct osd_module {
    hw_module_t common;                 /**< Common module interface. */
    void *reserved[1];                  /**< Reserved fields. */
} osd_module_t;

/*===========================================================================
 * Inline Helper Functions
 *===========================================================================*/

/**
 * @brief Open OSD hardware device.
 * @param module  Hardware module pointer.
 * @param device  Output pointer to OSD device.
 * @return 0 on success, negative error code on failure.
 */
static inline int osd_hw_device_open(const struct hw_module_t *module,
                                     struct osd_hw_device **device)
{
    return module->methods->open(module, OSD_HARDWARE_INTERFACE,
                                 TO_HW_DEVICE_T_OPEN(device));
}

/**
 * @brief Close OSD hardware device.
 * @param device  OSD device to close.
 * @return 0 on success, negative error code on failure.
 */
static inline int osd_hw_device_close(struct osd_hw_device *device)
{
    return device->common.close(&device->common);
}

/*===========================================================================
 * Error Code Definitions
 *===========================================================================*/

/**
 * @brief OSD module error codes.
 * @details Common error codes for OSD module operations.
 */
#define HARDWARE_OSD_OK                       SYSTEM_SUCCESS
#define HARDWARE_OSD_ERROR_INVALID_PARAM      SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_OSD, ERROR_INVALID_PARAM)
#define HARDWARE_OSD_ERROR_NOT_INIT           SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_OSD, ERROR_NOT_INIT)
#define HARDWARE_OSD_ERROR_MEMORY             SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_OSD, ERROR_MEMORY)
#define HARDWARE_OSD_ERROR_NOT_FOUND          SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_OSD, ERROR_NOT_FOUND)
#define HARDWARE_OSD_ERROR_NOT_SUPPORTED      SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_OSD, ERROR_NOT_SUPPORTED)
#define HARDWARE_OSD_ERROR_OPERATION_FAILED   SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_OSD, ERROR_OPERATION_FAILED)

/*===========================================================================
 * Module API Functions
 *===========================================================================*/

/**
 * @brief OSD platform-specific attributes (SoC-dependent).
 */
typedef struct {
    int char_interval;
} osd_platform_attrs_t;

/**
 * @brief Get OSD platform attributes for current SoC build.
 * @param attrs Output structure.
 * @return 0 on success, negative on failure.
 */
int osd_get_platform_attrs(osd_platform_attrs_t *attrs);

/**
 * @brief Initialize OSD hardware module.
 * @details Registers the OSD module with the hardware module system.
 * @return 0 on success, negative error code on failure.
 */
int hw_osd_module_init(void);

__END_DECLS

#endif /* HARDWARE_OSD_H */

