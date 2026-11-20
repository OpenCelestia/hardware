// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Shared glyph bitmap helpers (VPSS scratch / IPU OSD).
 */

#ifndef HARDWARE_HAL_OSD_GLYPH_H
#define HARDWARE_HAL_OSD_GLYPH_H

int hal_osd_get_bpp(void);

/**
 * Render horizontal text as 1bpp bitmap (GetStrBmp, then built-in ASCII glyphs).
 */
int hal_osd_render_text_bitmap(int nChn, int nObj, const char *cText, int iTextLen,
                               int src_w, int src_h,
                               unsigned char **out_buf, int *out_w, int *out_h, int *out_len);

#endif
