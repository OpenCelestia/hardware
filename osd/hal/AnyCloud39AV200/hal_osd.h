// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/osd.
 *
 * OSD HAL implementation for AnyCloud39AV200 platform.
 * - Internal definitions for platform-specific OSD HAL implementation.
 * - Contains HAL internal structures and helper functions.
 */

#ifndef HARDWARE_HAL_OSD_HAL_ANYCLOUD39AV200_HAL_OSD_H
#define HARDWARE_HAL_OSD_HAL_ANYCLOUD39AV200_HAL_OSD_H

#include "hal_osd_api.h"
#include "osd.h"
#include "ak_vpss_isp.h"
#include "ak_osd.h"

/*===========================================================================
 * Constants
 *===========================================================================*/

#define HAL_OSD_OBJ_COUNT   8
#define HAL_OSD_TEXT_LEN    100

/*===========================================================================
 * HAL Internal Type Definitions
 *===========================================================================*/

/**
 * @brief HAL internal image information.
 */
struct hal_osd_img_info {
    unsigned char *pData;               /**< Image data pointer. */
    int iDataSize;                      /**< Image data size. */
};

/**
 * @brief HAL internal text information.
 */
struct hal_osd_text_info {
    char cText[256];                    /**< Text content. */
    int iTextLen;                       /**< Text length. */
    AK_U16 u16UsOsdString[80];          /**< Unicode string buffer. */
    OSD_FONT_ATTR_S stFontAttr;         /**< Font attributes. */
};

/**
 * @brief HAL internal object information.
 */
struct hal_osd_obj_info {
    int bEnable;                        /**< Enable flag. */
    int bUpdate;                        /**< Update flag. */
    int nObjType;                       /**< Object type. */
    int s32CanvasId;                    /**< Canvas ID. */
    int s32RegionId;                    /**< Region ID. */
    osd_rect_info rc;                   /**< Rectangle info. */
    union {
        struct hal_osd_img_info Img;    /**< Image info. */
        struct hal_osd_text_info Text;  /**< Text info. */
    } info;
};

/**
 * @brief HAL internal channel information.
 */
struct hal_osd_chn_info {
    int iCurFontSize;                   /**< Current font size. */
    int iFontWidth;                     /**< Font width. */
    int iFontHeight;                    /**< Font height. */
    struct hal_osd_obj_info ObjInfo[HAL_OSD_OBJ_COUNT]; /**< Object info array. */
    osd_rect_info rcBig;                /**< Big canvas rectangle. */
    int s32BigCanvasId;                 /**< Big canvas ID. */
};

/**
 * @brief Drawing information structure.
 */
struct osd_drawing_info {
    const unsigned char *pData;         /**< Bitmap data. */
    int iDataSize;                      /**< Bitmap data size in bytes. */
    int x;                              /**< X position on canvas in pixels. */
    int y;                              /**< Y position on canvas in pixels. */
    int height;                         /**< Bitmap height in pixels. */
    int width;                          /**< Bitmap width in pixels. */
};

/*===========================================================================
 * Platform-specific Helper Functions
 *===========================================================================*/

/**
 * @brief Create text bitmap.
 * @param nChn      Channel number.
 * @param cText     Text content.
 * @param iTextLen  Text length.
 * @param pBmps     Output bitmap info.
 * @return 0 on success, negative error code on failure.
 */
int hal_osd_new_text_bmp(int nChn, const char *cText, int iTextLen, osd_bitmap_info *pBmps);

/**
 * @brief Delete text bitmap.
 * @param nChn      Channel number.
 * @param bmp_info  Bitmap info to delete.
 * @param iTextLen  Text length.
 * @return 0 on success, negative error code on failure.
 */
int hal_osd_delete_text_bmp(int nChn, osd_bitmap_info *bmp_info, int iTextLen);

#endif /* HARDWARE_HAL_OSD_HAL_ANYCLOUD39AV200_HAL_OSD_H */
