// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * OSD coordinate / bitmap helpers for VPSS pre-rotation -> display post-rotation
 * mapping. Matches hal_ipu_rotate_with_format clockwise 90/180/270 semantics.
 */

#ifndef HARDWARE_OSD_ROTATION_H
#define HARDWARE_OSD_ROTATION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Normalize degrees to one of {0, 90, 180, 270}. */
int osd_rotation_normalize(int deg);

/**
 * Inverse rotation for pre-compensating glyph bitmaps on the VPSS canvas.
 * Frame rotation R and glyph pre-rotation P satisfy: P = inverse(R).
 */
int osd_rotation_inverse(int frame_rotation_deg);

/**
 * Map a display-space axis-aligned rectangle to VPSS canvas coordinates.
 * @param rotation     Channel IPU rotation (0/90/180/270, clockwise).
 * @param disp_w/h     Post-rotation (display) frame size.
 * @param vpss_w/h     Pre-rotation VPSS channel size.
 * @param dx,dy,dw,dh  Rectangle in display coordinates.
 * @param vx,vy,vw,vh  Output rectangle on VPSS (may swap w/h for 90/270).
 */
void osd_rotation_display_rect_to_vpss(int rotation,
                                       int disp_w, int disp_h,
                                       int vpss_w, int vpss_h,
                                       int dx, int dy, int dw, int dh,
                                       int *vx, int *vy, int *vw, int *vh);

/**
 * Rotate a packed indexed bitmap clockwise by 90/180/270 degrees.
 * @param bpp  Bits per pixel (1, 2, or 4).
 * @return 0 on success; caller must free *dst_out with free().
 */
int osd_rotation_rotate_bitmap_cw(const uint8_t *src, int src_w, int src_h, int bpp,
                                  int deg_cw, uint8_t **dst_out,
                                  int *dst_w, int *dst_h);

/**
 * Convert packed indexed bitmap between 1/2/4 bpp.
 * @return 0 on success; caller frees *dst_out with free().
 */
int osd_rotation_convert_bitmap_bpp(const uint8_t *src, int src_bpp, int w, int h,
                                    int dst_bpp, uint8_t **dst_out, int *dst_len);

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_OSD_ROTATION_H */
