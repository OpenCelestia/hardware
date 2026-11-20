// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Post-rotation IPU OSD (display-space coordinates).
 */

#ifndef HARDWARE_HAL_OSD_IPU_H
#define HARDWARE_HAL_OSD_IPU_H

#include "osd.h"

int hal_osd_ipu_mode(int nChn);

int hal_osd_ipu_configure(int nChn, int rotation_deg, int display_width, int display_height);

int hal_osd_ipu_draw_text(int nChn, int nObj, int x, int y,
                          const char *cText, int iTextLen);

int hal_osd_ipu_draw_bmp(int nChn, int nObj, const osd_rect_info *rc,
                         const unsigned char *pData, int iDataSize);

void hal_osd_ipu_clear_channel(int nChn);

void hal_osd_ipu_deinit(void);

/**
 * Composite pending OSD layers onto a rotated frame (NV12/YUV via IPU_COLOR_FMT_E).
 */
int hal_osd_ipu_composite_frame(int nChn, int queue_id,
                                unsigned long frame_phy, void *frame_vaddr,
                                unsigned int frame_w, unsigned int frame_h,
                                int ipu_fmt);

/** Store pixel-space boxes for IPU composite (replaces AK_MPI_PP_DrawBox when rotation uses IPU OSD). */
int hal_osd_ipu_set_draw_boxes(int nChn, pp_box_group_t *pstBoxGroup);

#endif
