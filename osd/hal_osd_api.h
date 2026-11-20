// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/osd.
 *
 * OSD HAL API interface.
 * - Platform-agnostic HAL interface for OSD operations.
 * - Defines HAL function prototypes for OSD hardware abstraction.
 */

#ifndef HARDWARE_HAL_OSD_API_H
#define HARDWARE_HAL_OSD_API_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/cdefs.h>

#include "osd.h"

__BEGIN_DECLS

/*===========================================================================
 * HAL API Functions
 *===========================================================================*/

/**
 * @brief Initialize OSD HAL.
 * @param attr       OSD attributes (can be NULL for defaults).
 * @param font_file  Font file path.
 * @return 0 on success, negative error code on failure.
 */
int hal_osd_init(osd_attr_t *attr, const char *font_file);

/**
 * @brief Deinitialize OSD HAL.
 * @return 0 on success, negative error code on failure.
 */
int hal_osd_deinit(void);

/**
 * @brief Draw bitmap to memory canvas.
 * @param nChn       Target VI channel number.
 * @param nObj       Target canvas region number.
 * @param rc         Bitmap coordinates.
 * @param pData      Bitmap data.
 * @param iDataSize  Bitmap data size in bytes.
 * @return 0 on success, negative error code on failure.
 */
int hal_osd_draw_bmp(int nChn, int nObj, osd_rect_info *rc,
                     const unsigned char *pData, int iDataSize);

/**
 * @brief Clear bitmap or text objects.
 * @param nChn  Channel number.
 * @param nObj  Object number (-1 to clear all).
 * @return 0 on success, negative error code on failure.
 */
int hal_osd_clean_obj(int nChn, int nObj);

/**
 * @brief Copy canvas to hardware OSD module.
 * @param nChn  Channel number.
 * @return 0 on success, negative error code on failure.
 */
int hal_osd_copy_cavas_to_vi(int nChn);

/**
 * @brief Set font for text rendering.
 * @param nChn   Channel number.
 * @param pFont  Font information.
 * @return 0 on success, negative error code on failure.
 */
int hal_osd_set_font(int nChn, osd_font_info *pFont);

/**
 * @brief Get text width and height in pixels.
 * @param nChn      Channel number.
 * @param cText     Text content.
 * @param cTextLen  Text length.
 * @param pWidth    Output width pointer.
 * @param pHeight   Output height pointer.
 * @return 0 on success, negative error code on failure.
 */
int hal_osd_get_text_wh(int nChn, const char *cText, int cTextLen,
                        int *pWidth, int *pHeight);

/**
 * @brief Draw text to memory canvas.
 * @param nChn      Channel number.
 * @param nObj      Object number.
 * @param x         X coordinate.
 * @param y         Y coordinate.
 * @param cText     Text content.
 * @param iTextLen  Text length.
 * @return 0 on success, negative error code on failure.
 */
int hal_osd_draw_text(int nChn, int nObj, int x, int y,
                      const char *cText, int iTextLen);

/**
 * @brief Set channel attributes.
 * @param nChn      Channel number.
 * @param pChnInfo  Channel information.
 * @return 0 on success, negative error code on failure.
 */
int hal_osd_set_attr(int nChn, osd_chn_info *pChnInfo);

/**
 * @brief Set enable state.
 * @param nChn     Channel number.
 * @param nObj     Object number (-1 for all objects).
 * @param bEnable  Enable flag.
 * @return 0 on success, negative error code on failure.
 */
int hal_osd_set_enable(int nChn, int nObj, int bEnable);

/**
 * @brief Draw box overlay.
 * @param nChn         Channel number.
 * @param pstBoxGroup  Box group information (platform-specific type).
 * @return 0 on success, negative error code on failure.
 */
int hal_osd_draw_box(int nChn, pp_box_group_t *pstBoxGroup);

/**
 * @brief Adjust box coordinates based on video resolution.
 * @param nChn             VPSS channel index.
 * @param pstBoxGroup      Box group information (platform-specific type).
 * @param video_width      Display (post-rotation) width in pixels.
 * @param video_height     Display (post-rotation) height in pixels.
 */
int hal_osd_adjust_box_coordinates(int nChn, pp_box_group_t *pstBoxGroup,
                                   int video_width, int video_height);

/**
 * @brief Preset camera rotation before hal_osd_init (optional; init also reads hal_camera_config.ini).
 * @param rotation_deg  Camera rotation in degrees.
 * @return 0 on success.
 */
int hal_osd_preset_rotation(int rotation_deg);

/**
 * @brief Select OSD backend from camera rotation (VPSS vs IPU post-rotate).
 * @param rotation_deg   Camera rotation (90/270 enables IPU OSD; 0 uses VPSS).
 * @param display_width  Post-rotation layout width.
 * @param display_height Post-rotation layout height.
 */
int hal_osd_set_rotation(int nChn, int rotation_deg, int display_width, int display_height);

/**
 * @brief Initialize OSD with color type.
 * @param color_type  Color type (0 for color).
 * @return 0 on success, negative error code on failure.
 */
int hal_osd_init2(int color_type);

/**
 * @brief Initialize draw box functionality.
 * @param grp_idx  Group index.
 * @return 0 on success, negative error code on failure.
 */
int hal_draw_box_init(int grp_idx);

/**
 * @brief Combined initialization for draw box and OSD.
 * @param grp_idx     Group index.
 * @param color_type  Color type.
 */
void hal_osd_draw_init(int grp_idx, int color_type);

__END_DECLS

#endif /* HARDWARE_HAL_OSD_API_H */
