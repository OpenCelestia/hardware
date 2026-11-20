// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * OSD rotation helpers (see osd_rotation.h).
 */

#include "osd_rotation.h"

#include <stdlib.h>
#include <string.h>

int osd_rotation_normalize(int deg)
{
    int r = deg % 360;
    if (r < 0) {
        r += 360;
    }
    if (r == 90 || r == 180 || r == 270) {
        return r;
    }
    return 0;
}

int osd_rotation_inverse(int frame_rotation_deg)
{
    switch (osd_rotation_normalize(frame_rotation_deg)) {
    case 90:
        return 270;
    case 270:
        return 90;
    case 180:
        return 180;
    default:
        return 0;
    }
}

void osd_rotation_display_rect_to_vpss(int rotation,
                                       int disp_w, int disp_h,
                                       int vpss_w, int vpss_h,
                                       int dx, int dy, int dw, int dh,
                                       int *vx, int *vy, int *vw, int *vh)
{
    int iw = vpss_w;
    int ih = vpss_h;
    int rx, ry, rw, rh;

    rotation = osd_rotation_normalize(rotation);
    if (rotation == 0 || disp_w <= 0 || disp_h <= 0 || vpss_w <= 0 || vpss_h <= 0) {
        if (vx) {
            *vx = dx;
        }
        if (vy) {
            *vy = dy;
        }
        if (vw) {
            *vw = dw;
        }
        if (vh) {
            *vh = dh;
        }
        return;
    }

    switch (rotation) {
    case 90:
        /* display (dx,dy) <- vpss (dy, ih-1-dx); inverse below */
        rx = dy;
        ry = ih - dx - dw;
        rw = dh;
        rh = dw;
        break;
    case 180:
        rx = iw - dx - dw;
        ry = ih - dy - dh;
        rw = dw;
        rh = dh;
        break;
    case 270:
        /* forward: dx=sy, dy=iw-1-sx */
        rx = iw - dy - dh;
        ry = dx;
        rw = dh;
        rh = dw;
        break;
    default:
        rx = dx;
        ry = dy;
        rw = dw;
        rh = dh;
        break;
    }

    if (rx < 0) {
        rx = 0;
    }
    if (ry < 0) {
        ry = 0;
    }
    if (rx + rw > iw) {
        rw = iw - rx;
    }
    if (ry + rh > ih) {
        rh = ih - ry;
    }
    if (rw < 0) {
        rw = 0;
    }
    if (rh < 0) {
        rh = 0;
    }

    if (vx) {
        *vx = rx;
    }
    if (vy) {
        *vy = ry;
    }
    if (vw) {
        *vw = rw;
    }
    if (vh) {
        *vh = rh;
    }
}

static int osd_rotation_get_pixel(const uint8_t *src, int src_w, int bpp,
                                  int x, int y, int *out_val)
{
    int bit_idx;
    int byte_idx;
    int shift;

    if (x < 0 || y < 0 || x >= src_w || bpp <= 0 || !src || !out_val) {
        return -1;
    }

    bit_idx = y * src_w + x;
    if (bpp == 1) {
        byte_idx = bit_idx / 8;
        shift = 7 - (bit_idx % 8);
        *out_val = (src[byte_idx] >> shift) & 1;
        return 0;
    }

    byte_idx = (bit_idx * bpp) / 8;
    shift = (8 - bpp) - (bit_idx % (8 / bpp)) * bpp;
    *out_val = (src[byte_idx] >> shift) & ((1 << bpp) - 1);
    return 0;
}

static void osd_rotation_set_pixel(uint8_t *dst, int dst_w, int bpp,
                                   int x, int y, int val)
{
    int bit_idx;
    int byte_idx;
    int shift;
    int mask;

    bit_idx = y * dst_w + x;
    if (bpp == 1) {
        byte_idx = bit_idx / 8;
        shift = 7 - (bit_idx % 8);
        if (val) {
            dst[byte_idx] |= (1 << shift);
        } else {
            dst[byte_idx] &= ~(1 << shift);
        }
        return;
    }

    byte_idx = (bit_idx * bpp) / 8;
    shift = (8 - bpp) - (bit_idx % (8 / bpp)) * bpp;
    mask = (1 << bpp) - 1;
    dst[byte_idx] &= ~(mask << shift);
    dst[byte_idx] |= (val & mask) << shift;
}

int osd_rotation_rotate_bitmap_cw(const uint8_t *src, int src_w, int src_h, int bpp,
                                  int deg_cw, uint8_t **dst_out,
                                  int *dst_w, int *dst_h)
{
    int rot;
    int out_w;
    int out_h;
    uint8_t *dst;
    int x;
    int y;
    int sx;
    int sy;
    int pix;

    if (!src || !dst_out || !dst_w || !dst_h || src_w <= 0 || src_h <= 0) {
        return -1;
    }
    if (bpp != 1 && bpp != 2 && bpp != 4) {
        return -1;
    }

    rot = osd_rotation_normalize(deg_cw);
    if (rot == 0) {
        size_t len = (size_t)((src_w * src_h * bpp + 7) / 8);
        dst = (uint8_t *)malloc(len);
        if (!dst) {
            return -1;
        }
        memcpy(dst, src, len);
        *dst_out = dst;
        *dst_w = src_w;
        *dst_h = src_h;
        return 0;
    }

    if (rot == 90 || rot == 270) {
        out_w = src_h;
        out_h = src_w;
    } else {
        out_w = src_w;
        out_h = src_h;
    }

    dst = (uint8_t *)calloc(1, (size_t)((out_w * out_h * bpp + 7) / 8));
    if (!dst) {
        return -1;
    }

    for (y = 0; y < out_h; y++) {
        for (x = 0; x < out_w; x++) {
            switch (rot) {
            case 90:
                sx = src_w - 1 - y;
                sy = x;
                break;
            case 180:
                sx = src_w - 1 - x;
                sy = src_h - 1 - y;
                break;
            case 270:
                sx = y;
                sy = src_h - 1 - x;
                break;
            default:
                sx = x;
                sy = y;
                break;
            }
            if (osd_rotation_get_pixel(src, src_w, bpp, sx, sy, &pix) == 0) {
                osd_rotation_set_pixel(dst, out_w, bpp, x, y, pix);
            }
        }
    }

    *dst_out = dst;
    *dst_w = out_w;
    *dst_h = out_h;
    return 0;
}

int osd_rotation_convert_bitmap_bpp(const uint8_t *src, int src_bpp, int w, int h,
                                    int dst_bpp, uint8_t **dst_out, int *dst_len)
{
    uint8_t *dst;
    int out_len;
    int x;
    int y;
    int pix;

    if (!src || !dst_out || !dst_len || w <= 0 || h <= 0) {
        return -1;
    }
    if (src_bpp != 1 && src_bpp != 2 && src_bpp != 4) {
        return -1;
    }
    if (dst_bpp != 1 && dst_bpp != 2 && dst_bpp != 4) {
        return -1;
    }

    out_len = (w * h * dst_bpp + 7) / 8;
    if (src_bpp == dst_bpp) {
        dst = (uint8_t *)malloc((size_t)out_len);
        if (!dst) {
            return -1;
        }
        memcpy(dst, src, (size_t)out_len);
        *dst_out = dst;
        *dst_len = out_len;
        return 0;
    }

    dst = (uint8_t *)calloc(1, (size_t)out_len);
    if (!dst) {
        return -1;
    }

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            if (osd_rotation_get_pixel(src, w, src_bpp, x, y, &pix) != 0) {
                continue;
            }
            osd_rotation_set_pixel(dst, w, dst_bpp, x, y, pix);
        }
    }

    *dst_out = dst;
    *dst_len = out_len;
    return 0;
}
