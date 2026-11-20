// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * IPU OSD: store display-space layers, composite after IPU rotation via AK_MPI_IPU_Blend.
 */

#include "hal_osd_ipu.h"
#include "hal_osd_glyph.h"
#include "hal_osd_api.h"

#include "hal_ipu_api.h"
#include "ak_ipu.h"
#include "ak_mem.h"
#include "ak_common.h"
#include "log.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define LOG_TAG "HAL_OSD_IPU"
#define HAL_OSD_IPU_MAX_CHN 3
#define HAL_OSD_IPU_OBJ_COUNT 8
#define HAL_OSD_IPU_BOX_MAX_NUM 50

/* Keep in sync with hal_osd.c hal_osd_ai_box_group_t (AI manager neutral layout). */
typedef struct hal_osd_ipu_ai_rect_s {
    uint16_t u16X;
    uint16_t u16Y;
    uint16_t u16Width;
    uint16_t u16Height;
} hal_osd_ipu_ai_rect_t;

typedef struct hal_osd_ipu_ai_triangle_s {
    uint16_t u16X;
    uint16_t u16Y;
    int8_t u8HalfWidth;
    int8_t u8Bitmap;
} hal_osd_ipu_ai_triangle_t;

typedef struct hal_osd_ipu_ai_box_s {
    uint8_t u8Enable;
    int32_t enType;
    uint16_t u16ColorId;
    uint16_t u16LineWidth;
    int32_t s32DrawFrameNum;
    union {
        hal_osd_ipu_ai_rect_t stRectangleArea;
        hal_osd_ipu_ai_triangle_t stTriangleArea;
    };
} hal_osd_ipu_ai_box_t;

typedef struct hal_osd_ipu_ai_box_group_s {
    hal_osd_ipu_ai_box_t stBox[HAL_OSD_IPU_BOX_MAX_NUM];
} hal_osd_ipu_ai_box_group_t;

typedef struct hal_osd_ipu_layer_s {
    int enable;
    int x;
    int y;
    int w;
    int h;
    int bpp;
    unsigned char *bmp;
    int bmp_len;
} hal_osd_ipu_layer_t;

static hal_osd_ipu_layer_t s_layers[HAL_OSD_IPU_MAX_CHN][HAL_OSD_IPU_OBJ_COUNT];
static hal_osd_ipu_ai_box_group_t s_box_groups[HAL_OSD_IPU_MAX_CHN];
static int s_ipu_mode[HAL_OSD_IPU_MAX_CHN];
static int s_disp_w[HAL_OSD_IPU_MAX_CHN];
static int s_disp_h[HAL_OSD_IPU_MAX_CHN];
static pthread_mutex_t s_ipu_mutex = PTHREAD_MUTEX_INITIALIZER;

static int hal_osd_ipu_norm_rotation(int deg)
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

static void hal_osd_ipu_free_layer(hal_osd_ipu_layer_t *layer)
{
    if (!layer) {
        return;
    }
    free(layer->bmp);
    layer->bmp = NULL;
    layer->bmp_len = 0;
    layer->enable = 0;
    layer->w = 0;
    layer->h = 0;
}

static int hal_osd_ipu_store_bitmap(int nChn, int nObj, int x, int y, int w, int h, int bpp,
                                    const unsigned char *src, int src_len)
{
    hal_osd_ipu_layer_t *layer;
    unsigned char *copy;

    if (nChn < 0 || nChn >= HAL_OSD_IPU_MAX_CHN || nObj < 0 || nObj >= HAL_OSD_IPU_OBJ_COUNT) {
        return -1;
    }
    if (!src || w <= 0 || h <= 0 || src_len <= 0) {
        return -1;
    }

    layer = &s_layers[nChn][nObj];
    copy = (unsigned char *)malloc((size_t)src_len);
    if (!copy) {
        return -1;
    }
    memcpy(copy, src, (size_t)src_len);
    hal_osd_ipu_free_layer(layer);
    layer->enable = 1;
    layer->x = x;
    layer->y = y;
    layer->w = w;
    layer->h = h;
    layer->bpp = bpp;
    layer->bmp = copy;
    layer->bmp_len = src_len;
    return 0;
}

static int hal_osd_ipu_get_1bpp(const uint8_t *bmp, int bw, int x, int y)
{
    int bit_idx = y * bw + x;
    return (bmp[bit_idx / 8] >> (7 - (bit_idx % 8))) & 1;
}

static int hal_osd_ipu_fill_argb32(const uint8_t *mono, int bw, int bh, int bpp,
                                   uint32_t *argb, uint32_t fg, uint32_t bg)
{
    int x;
    int y;

    if (!mono || !argb || bw <= 0 || bh <= 0) {
        return -1;
    }
    for (y = 0; y < bh; y++) {
        for (x = 0; x < bw; x++) {
            int on = (bpp == 1) ? hal_osd_ipu_get_1bpp(mono, bw, x, y) : 1;
            argb[y * bw + x] = on ? fg : bg;
        }
    }
    return 0;
}

static void hal_osd_ipu_blend_y_cpu(uint8_t *yplane, int stride, int fw, int fh,
                                    const uint8_t *mono, int bw, int bh, int bpp,
                                    int x0, int y0, uint8_t luma)
{
    int x;
    int y;

    for (y = 0; y < bh; y++) {
        int fy = y0 + y;
        if (fy < 0 || fy >= fh) {
            continue;
        }
        for (x = 0; x < bw; x++) {
            int fx = x0 + x;
            if (fx < 0 || fx >= fw) {
                continue;
            }
            if (bpp == 1 && hal_osd_ipu_get_1bpp(mono, bw, x, y)) {
                yplane[fy * stride + fx] = luma;
            }
        }
    }
}

static void hal_osd_ipu_plot_y(uint8_t *yplane, int stride, int fw, int fh,
                               int x, int y, uint8_t luma);

/** PP box palette in hal_osd_draw_box (R,G,B,Y,M,C,W) — match nova MPI_PP_DrawBox colors. */
static void hal_osd_ipu_box_yuv(uint16_t color_id, uint8_t *out_y, uint8_t *out_u, uint8_t *out_v)
{
    static const uint32_t pp_argb[] = {
        0xff0000ffU, 0xff00ff00U, 0xffff0000U, 0xff00ffffU,
        0xffff00ffU, 0xffffff00U, 0xffffffffU
    };
    uint32_t c = pp_argb[color_id % (sizeof(pp_argb) / sizeof(pp_argb[0]))];
    int r = (int)((c >> 16) & 0xffU);
    int g = (int)((c >> 8) & 0xffU);
    int b = (int)(c & 0xffU);
    int y = (77 * r + 150 * g + 29 * b) >> 8;
    int u = ((-43 * r - 85 * g + 128 * b) >> 8) + 128;
    int v = ((128 * r - 107 * g - 21 * b) >> 8) + 128;

    if (out_y) {
        *out_y = (uint8_t)(y < 0 ? 0 : (y > 255 ? 255 : y));
    }
    if (out_u) {
        *out_u = (uint8_t)(u < 0 ? 0 : (u > 255 ? 255 : u));
    }
    if (out_v) {
        *out_v = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
}

static void hal_osd_ipu_plot_uv(uint8_t *uv_plane, int uv_stride, int fw, int fh,
                                int x, int y, uint8_t u, uint8_t v)
{
    int ux;
    int uy;

    if (x < 0 || y < 0 || x >= fw || y >= fh) {
        return;
    }
    ux = x / 2;
    uy = y / 2;
    if (ux < 0 || uy < 0 || (ux * 2 + 1) >= fw || (uy * 2 + 1) >= fh) {
        return;
    }
    uv_plane[uy * uv_stride + ux * 2] = u;
    uv_plane[uy * uv_stride + ux * 2 + 1] = v;
}

static void hal_osd_ipu_draw_hollow_rect_yuv(uint8_t *yplane, uint8_t *uv_plane,
                                              int y_stride, int uv_stride,
                                              int fw, int fh,
                                              int x, int y, int w, int h, int lw,
                                              uint8_t y_luma, uint8_t u_chroma, uint8_t v_chroma)
{
    int edge;
    int t = y;
    int b = y + h - 1;
    int l = x;
    int r = x + w - 1;

    if (w <= 0 || h <= 0 || lw <= 0) {
        return;
    }
    if (l >= fw || t >= fh || r < 0 || b < 0) {
        return;
    }

    for (edge = 0; edge < lw; edge++) {
        int yy;
        int xx;
        int yt = t + edge;
        int yb = b - edge;
        int xl = l + edge;
        int xr = r - edge;

        if (yt > yb || xl > xr) {
            break;
        }
        for (yy = yt; yy <= yb; yy++) {
            hal_osd_ipu_plot_y(yplane, y_stride, fw, fh, xl, yy, y_luma);
            hal_osd_ipu_plot_y(yplane, y_stride, fw, fh, xr, yy, y_luma);
            if (uv_plane) {
                hal_osd_ipu_plot_uv(uv_plane, uv_stride, fw, fh, xl, yy, u_chroma, v_chroma);
                hal_osd_ipu_plot_uv(uv_plane, uv_stride, fw, fh, xr, yy, u_chroma, v_chroma);
            }
        }
        for (xx = xl; xx <= xr; xx++) {
            hal_osd_ipu_plot_y(yplane, y_stride, fw, fh, xx, yt, y_luma);
            hal_osd_ipu_plot_y(yplane, y_stride, fw, fh, xx, yb, y_luma);
            if (uv_plane) {
                hal_osd_ipu_plot_uv(uv_plane, uv_stride, fw, fh, xx, yt, u_chroma, v_chroma);
                hal_osd_ipu_plot_uv(uv_plane, uv_stride, fw, fh, xx, yb, u_chroma, v_chroma);
            }
        }
    }
}

static void hal_osd_ipu_plot_y(uint8_t *yplane, int stride, int fw, int fh,
                               int x, int y, uint8_t luma)
{
    if (x >= 0 && x < fw && y >= 0 && y < fh) {
        yplane[y * stride + x] = luma;
    }
}

static int hal_osd_ipu_has_active_boxes(int nChn)
{
    int i;

    if (nChn < 0 || nChn >= HAL_OSD_IPU_MAX_CHN) {
        return 0;
    }
    for (i = 0; i < HAL_OSD_IPU_BOX_MAX_NUM; i++) {
        if (s_box_groups[nChn].stBox[i].u8Enable &&
            s_box_groups[nChn].stBox[i].s32DrawFrameNum != 0) {
            return 1;
        }
    }
    return 0;
}

static void hal_osd_ipu_composite_boxes(int nChn, void *frame_vaddr,
                                        unsigned int frame_w, unsigned int frame_h,
                                        int ipu_fmt)
{
    hal_osd_ipu_ai_box_t boxes[HAL_OSD_IPU_BOX_MAX_NUM];
    int box_count = 0;
    int i;
    uint8_t *yplane;
    size_t y_size;

    if (!frame_vaddr || frame_w == 0 || frame_h == 0) {
        return;
    }
    if ((IPU_COLOR_FMT_E)ipu_fmt != IPU_COLOR_FMT_SEMIPLANE_NV12 &&
        (IPU_COLOR_FMT_E)ipu_fmt != IPU_COLOR_FMT_SEMIPLANE_NV21) {
        return;
    }

    pthread_mutex_lock(&s_ipu_mutex);
    for (i = 0; i < HAL_OSD_IPU_BOX_MAX_NUM; i++) {
        hal_osd_ipu_ai_box_t *src = &s_box_groups[nChn].stBox[i];
        if (!src->u8Enable || src->s32DrawFrameNum == 0) {
            continue;
        }
        boxes[box_count++] = *src;
        if (src->s32DrawFrameNum > 0) {
            src->s32DrawFrameNum--;
            if (src->s32DrawFrameNum == 0) {
                src->u8Enable = 0;
            }
        }
    }
    pthread_mutex_unlock(&s_ipu_mutex);

    if (box_count == 0) {
        return;
    }

    yplane = (uint8_t *)frame_vaddr;
    y_size = (size_t)frame_w * (size_t)frame_h * 3U / 2U;
    AK_BASE_MemSyncBegin(frame_vaddr, (AK_U32)y_size);

    for (i = 0; i < box_count; i++) {
        hal_osd_ipu_ai_box_t *b = &boxes[i];
        int lw = (int)b->u16LineWidth;
        uint8_t y_col;
        uint8_t u_col;
        uint8_t v_col;
        uint8_t *uv_plane = yplane + (size_t)frame_w * (size_t)frame_h;

        if (lw < 1) {
            lw = 1;
        }
        if (lw > 16) {
            lw = 16;
        }
        hal_osd_ipu_box_yuv(b->u16ColorId, &y_col, &u_col, &v_col);
        hal_osd_ipu_draw_hollow_rect_yuv(yplane, uv_plane,
                                         (int)frame_w, (int)frame_w,
                                         (int)frame_w, (int)frame_h,
                                         (int)b->stRectangleArea.u16X,
                                         (int)b->stRectangleArea.u16Y,
                                         (int)b->stRectangleArea.u16Width,
                                         (int)b->stRectangleArea.u16Height,
                                         lw, y_col, u_col, v_col);
    }

    AK_BASE_MemSyncEnd(frame_vaddr, (AK_U32)y_size);
}

int hal_osd_ipu_set_draw_boxes(int nChn, pp_box_group_t *pstBoxGroup)
{
    const hal_osd_ipu_ai_box_group_t *src;
    int i;
    int copied = 0;

    if (!hal_osd_ipu_mode(nChn) || !pstBoxGroup) {
        return -1;
    }
    if (nChn < 0 || nChn >= HAL_OSD_IPU_MAX_CHN) {
        return -1;
    }

    src = (const hal_osd_ipu_ai_box_group_t *)pstBoxGroup;

    pthread_mutex_lock(&s_ipu_mutex);
    memset(&s_box_groups[nChn], 0, sizeof(s_box_groups[nChn]));
    for (i = 0; i < HAL_OSD_IPU_BOX_MAX_NUM; i++) {
        if (!src->stBox[i].u8Enable) {
            continue;
        }
        s_box_groups[nChn].stBox[i] = src->stBox[i];
        copied++;
    }
    pthread_mutex_unlock(&s_ipu_mutex);

    return (copied > 0) ? 0 : 0;
}

static int hal_osd_ipu_blend_ipu(int queue_id, IPU_SURFACE_S *bg,
                                 unsigned long fg_phy, int x, int y, int bw, int bh)
{
    IPU_SURFACE_S fg;
    IPU_SURFACE_S dst;
    IPU_RECT_S bg_rect;
    IPU_RECT_S fg_rect;
    IPU_RECT_S dst_rect;
    IPU_BLEND_OPT_S opt;

    if (!bg || fg_phy == 0 || bw <= 0 || bh <= 0) {
        return -1;
    }

    hal_ipu_setup_surface(&fg, IPU_COLOR_FMT_PACKED_ARGB32, (unsigned int)bw, (unsigned int)bh, fg_phy);
    memcpy(&dst, bg, sizeof(dst));

    bg_rect.s32X = x;
    bg_rect.s32Y = y;
    bg_rect.u32W = (unsigned int)bw;
    bg_rect.u32H = (unsigned int)bh;
    fg_rect.s32X = 0;
    fg_rect.s32Y = 0;
    fg_rect.u32W = (unsigned int)bw;
    fg_rect.u32H = (unsigned int)bh;
    dst_rect = bg_rect;

    memset(&opt, 0, sizeof(opt));
    opt.enOpt = IPU_OPT_COLOR_KEY_BLENDING;
    opt.stColorScopeBlendAttr.u8OpacityPositive = 255;
    opt.stColorScopeBlendAttr.u8OpacityNegative = 0;
    opt.stColorScopeBlendAttr.enRefLayer = IPU_LAYER_TMP;
    opt.stColorScopeBlendAttr.u32RefColor = 0x00000000;
    opt.stColorScopeBlendAttr.u8Differ = 24;

    if (hal_ipu_start_queue(queue_id) != 0) {
        return -1;
    }
    if (AK_MPI_IPU_Blend(queue_id, bg, &bg_rect, &fg, &fg_rect, &dst, &dst_rect, &opt) != AK_SUCCESS) {
        hal_ipu_end_queue(queue_id, true, true);
        hal_ipu_wait_done(queue_id, 1000);
        return -1;
    }
    if (hal_ipu_end_queue(queue_id, true, true) != 0) {
        hal_ipu_wait_done(queue_id, 1000);
        return -1;
    }
    if (hal_ipu_wait_done(queue_id, 1000) != AK_SUCCESS) {
        return -1;
    }
    return 0;
}

int hal_osd_ipu_mode(int nChn)
{
    if (nChn < 0 || nChn >= HAL_OSD_IPU_MAX_CHN) {
        return 0;
    }
    return s_ipu_mode[nChn];
}

int hal_osd_ipu_configure(int nChn, int rotation_deg, int display_width, int display_height)
{
    int rot;
    int active;

    if (nChn < 0 || nChn >= HAL_OSD_IPU_MAX_CHN) {
        return -1;
    }

    rot = hal_osd_ipu_norm_rotation(rotation_deg);
    active = (rot == 90 || rot == 270);

    pthread_mutex_lock(&s_ipu_mutex);
    if (s_ipu_mode[nChn] && !active) {
        int i;
        for (i = 0; i < HAL_OSD_IPU_OBJ_COUNT; i++) {
            hal_osd_ipu_free_layer(&s_layers[nChn][i]);
        }
    }
    s_ipu_mode[nChn] = active;
    s_disp_w[nChn] = display_width;
    s_disp_h[nChn] = display_height;
    pthread_mutex_unlock(&s_ipu_mutex);

    log_debug("ipu osd ch=%d mode=%d rot=%d display=%dx%d",
             nChn, active, rot, display_width, display_height);
    return 0;
}

int hal_osd_ipu_draw_text(int nChn, int nObj, int x, int y,
                          const char *cText, int iTextLen)
{
    unsigned char *bmp = NULL;
    int bmp_w = 0;
    int bmp_h = 0;
    int bmp_len = 0;
    int tw = 0;
    int th = 0;
    int ret;

    if (!hal_osd_ipu_mode(nChn)) {
        return -1;
    }
    if (hal_osd_get_text_wh(nChn, cText, iTextLen, &tw, &th) != 0 || tw <= 0 || th <= 0) {
        return -1;
    }
    if (hal_osd_render_text_bitmap(nChn, nObj, cText, iTextLen, tw, th,
                                   &bmp, &bmp_w, &bmp_h, &bmp_len) != 0) {
        log_warn("ipu osd text bitmap failed ch=%d obj=%d", nChn, nObj);
        return -1;
    }

    pthread_mutex_lock(&s_ipu_mutex);
    ret = hal_osd_ipu_store_bitmap(nChn, nObj, x, y, bmp_w, bmp_h, hal_osd_get_bpp(), bmp, bmp_len);
    pthread_mutex_unlock(&s_ipu_mutex);
    free(bmp);
    return ret;
}

int hal_osd_ipu_draw_bmp(int nChn, int nObj, const osd_rect_info *rc,
                         const unsigned char *pData, int iDataSize)
{
    int ret;

    if (!hal_osd_ipu_mode(nChn) || !rc || !pData || iDataSize <= 0) {
        return -1;
    }

    pthread_mutex_lock(&s_ipu_mutex);
    ret = hal_osd_ipu_store_bitmap(nChn, nObj, rc->x, rc->y, rc->width, rc->height,
                                   hal_osd_get_bpp(), pData, iDataSize);
    pthread_mutex_unlock(&s_ipu_mutex);
    return ret;
}

void hal_osd_ipu_clear_channel(int nChn)
{
    int i;

    if (nChn < 0 || nChn >= HAL_OSD_IPU_MAX_CHN) {
        return;
    }
    for (i = 0; i < HAL_OSD_IPU_OBJ_COUNT; i++) {
        hal_osd_ipu_free_layer(&s_layers[nChn][i]);
    }
    memset(&s_box_groups[nChn], 0, sizeof(s_box_groups[nChn]));
}

void hal_osd_ipu_deinit(void)
{
    int ch;

    pthread_mutex_lock(&s_ipu_mutex);
    for (ch = 0; ch < HAL_OSD_IPU_MAX_CHN; ch++) {
        hal_osd_ipu_clear_channel(ch);
        s_ipu_mode[ch] = 0;
    }
    pthread_mutex_unlock(&s_ipu_mutex);
}

int hal_osd_ipu_composite_frame(int nChn, int queue_id,
                                unsigned long frame_phy, void *frame_vaddr,
                                unsigned int frame_w, unsigned int frame_h,
                                int ipu_fmt)
{
    IPU_SURFACE_S bg;
    hal_osd_ipu_layer_t layers[HAL_OSD_IPU_OBJ_COUNT];
    int layer_count = 0;
    int i;
    int ret = 0;
    int ipu_opened = 0;

    if (!hal_osd_ipu_mode(nChn) || frame_phy == 0 || !frame_vaddr || frame_w == 0 || frame_h == 0) {
        return 0;
    }

    pthread_mutex_lock(&s_ipu_mutex);
    for (i = 0; i < HAL_OSD_IPU_OBJ_COUNT; i++) {
        if (!s_layers[nChn][i].enable || !s_layers[nChn][i].bmp) {
            continue;
        }
        layers[layer_count++] = s_layers[nChn][i];
    }
    pthread_mutex_unlock(&s_ipu_mutex);

    if (layer_count == 0 && !hal_osd_ipu_has_active_boxes(nChn)) {
        return 0;
    }

    if (layer_count > 0 && hal_ipu_open() != 0) {
        return -1;
    }
    if (layer_count > 0) {
        ipu_opened = 1;
    }

    if (layer_count > 0) {
        hal_ipu_setup_surface(&bg, (IPU_COLOR_FMT_E)ipu_fmt, frame_w, frame_h, frame_phy);

        for (i = 0; i < layer_count; i++) {
            hal_osd_ipu_layer_t *layer = &layers[i];
        size_t argb_bytes = (size_t)layer->w * (size_t)layer->h * 4U;
        void *fg_vaddr = NULL;
        unsigned long fg_phy = 0;
        uint32_t *argb = NULL;
        int clipped_w = layer->w;
        int clipped_h = layer->h;
        int draw_x = layer->x;
        int draw_y = layer->y;

        if (draw_x >= (int)frame_w || draw_y >= (int)frame_h) {
            continue;
        }
        if (draw_x + clipped_w > (int)frame_w) {
            clipped_w = (int)frame_w - draw_x;
        }
        if (draw_y + clipped_h > (int)frame_h) {
            clipped_h = (int)frame_h - draw_y;
        }
        if (clipped_w <= 0 || clipped_h <= 0) {
            continue;
        }

        if ((IPU_COLOR_FMT_E)ipu_fmt == IPU_COLOR_FMT_SEMIPLANE_NV12 ||
            (IPU_COLOR_FMT_E)ipu_fmt == IPU_COLOR_FMT_SEMIPLANE_NV21) {
            uint8_t *y = (uint8_t *)frame_vaddr;
            AK_BASE_MemSyncBegin(frame_vaddr, frame_w * frame_h * 3U / 2U);
            hal_osd_ipu_blend_y_cpu(y, (int)frame_w, (int)frame_w, (int)frame_h,
                                    layer->bmp, layer->w, layer->h, layer->bpp,
                                    draw_x, draw_y, 235);
            AK_BASE_MemSyncEnd(frame_vaddr, frame_w * frame_h * 3U / 2U);
            continue;
        }

        fg_vaddr = AK_BASE_MemDmaAlloc(MODULE_ID_OSD, argb_bytes);
        if (!fg_vaddr) {
            ret = -1;
            continue;
        }
        if (AK_BASE_MemDmaVaddr2Paddr(fg_vaddr, &fg_phy) != AK_SUCCESS || fg_phy == 0) {
            AK_BASE_MemDmaFree(fg_vaddr);
            ret = -1;
            continue;
        }

        argb = (uint32_t *)fg_vaddr;
        if (hal_osd_ipu_fill_argb32(layer->bmp, layer->w, layer->h, layer->bpp,
                                    argb, 0xFFFFFFFFU, 0x00000000U) != 0) {
            AK_BASE_MemDmaFree(fg_vaddr);
            ret = -1;
            continue;
        }
        AK_BASE_MemSyncBegin(fg_vaddr, (AK_U32)argb_bytes);

        if (hal_osd_ipu_blend_ipu(queue_id, &bg, fg_phy, draw_x, draw_y, clipped_w, clipped_h) != 0) {
            log_warn("ipu osd blend failed ch=%d layer=%d", nChn, i);
            ret = -1;
        }

        AK_BASE_MemSyncEnd(fg_vaddr, (AK_U32)argb_bytes);
        AK_BASE_MemDmaFree(fg_vaddr);
        }
    }

    hal_osd_ipu_composite_boxes(nChn, frame_vaddr, frame_w, frame_h, ipu_fmt);

    if (ipu_opened) {
        (void)hal_ipu_close();
    }
    return ret;
}
