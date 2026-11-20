// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/osd.
 *
 * OSD HAL implementation for AnyCloud39AV200 platform.
 * - Implements HAL API functions using Anyka SDK.
 * - Handles text rendering, bitmap drawing and box overlay operations.
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <iconv.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hal_osd.h"
#include "hal_osd_glyph.h"
#include "hal_osd_ipu.h"
#include "system_config.h"
#include "ak_common.h"
#include "ak_common_video.h"
#include "ak_log.h"
#include "ak_vpss.h"
#include "ak_osd.h"
#include "ak_mem.h"
#include "log.h"

#define LOG_TAG "HAL_OSD"
#define MAX_CHN_CNT 3

/** Legacy scratch canvas ids (unused after IPU glyph path). */
#define HAL_OSD_SCRATCH_CANVAS_BASE 12
#define HAL_OSD_SCRATCH_REGION_BASE 60

/*===========================================================================
 * Private Variables
 *===========================================================================*/

static AK_CHAR pcFontFile[128];
static AK_S32 s32GColorType = 0;
static AK_S32 s32BitWidth = 1;

static AK_S32 s32GFontColor = 1;
static AK_S32 s32GBgColor = 0;
static AK_S32 s32GEdgeColor = 2;
static AK_S32 s32FontInterval = 0;

static AK_S32 g_s32GrpIndex = 0;
static int g_bCutMode = 1;
static int g_MaxWidth[2] = { 960, 672 };

/** camera0:rotation from hal_camera_config.ini; 90/270 → IPU OSD, skip MPI VPSS OSD. */
static int s_camera_rotation_deg = 0;
static int s_mpi_osd_inited = 0;

/** Owned bitmap buffers for VPSS OSD_REG_BITMAP objects. */
static unsigned char *g_rot_bmp_buf[MAX_CHN_CNT][HAL_OSD_OBJ_COUNT];
static int g_rot_bmp_buf_size[MAX_CHN_CNT][HAL_OSD_OBJ_COUNT];

osd_attr_t g_attr;
struct hal_osd_chn_info g_ChnInfo[MAX_CHN_CNT] = {{16, 10, 16}, {16, 10, 16}};

/*
 * AI manager currently fills box data with this neutral layout.
 * Keep this synchronized with framework/services/multimedia/camera/ai/ai_manager.c.
 */
#define HAL_OSD_AI_BOX_MAX_NUM 50
typedef struct hal_osd_ai_rect_s {
    AK_U16 u16X;
    AK_U16 u16Y;
    AK_U16 u16Width;
    AK_U16 u16Height;
} hal_osd_ai_rect_t;

typedef struct hal_osd_ai_triangle_s {
    AK_U16 u16X;
    AK_U16 u16Y;
    char u8HalfWidth;
    char u8Bitmap;
} hal_osd_ai_triangle_t;

typedef struct hal_osd_ai_box_s {
    AK_U8 u8Enable;
    AK_S32 enType;
    AK_U16 u16ColorId;
    AK_U16 u16LineWidth;
    AK_S32 s32DrawFrameNum;
    union {
        hal_osd_ai_rect_t stRectangleArea;
        hal_osd_ai_triangle_t stTriangleArea;
    };
} hal_osd_ai_box_t;

typedef struct hal_osd_ai_box_group_s {
    hal_osd_ai_box_t stBox[HAL_OSD_AI_BOX_MAX_NUM];
} hal_osd_ai_box_group_t;

/* AI box line: ai_manager uses norm=30 (1/10000). Nova preview 1024w → 3px via PP DrawBox. */
#define HAL_OSD_AI_BOX_LINE_NORM_DEN     10000
#define HAL_OSD_AI_BOX_LINE_MIN_PX       3   /* match nova preview thickness */
#define HAL_OSD_AI_BOX_LINE_MAX_PX       16  /* AK_MPI_PP_DrawBox [1-16] */
#define HAL_OSD_AI_BOX_PREVIEW_W_MAX     1024

/* ARGB8888 color table. */
static AK_U32 u32RgbColorTables[] = {
    0x00000000, 0xffffffff, 0xff000000, 0xff0000ff,
    0xff00ff00, 0xffff0000, 0xffffff00, 0xffff00ff,
    0xff00ffff, 0xff786085, 0xff2c8aa0, 0xff68d535,
    0xff34aa5a, 0xff43e9ab, 0xff4b55a5, 0xff008080
};

/*===========================================================================
 * Private Helper Functions
 *===========================================================================*/

static int char_to_short(unsigned short *dest, const unsigned char *src);

static int hal_osd_norm_rotation(int rotation_deg)
{
    int r = rotation_deg % 360;

    if (r < 0) {
        r += 360;
    }
    return r;
}

static int hal_osd_rotation_uses_ipu(int rotation_deg)
{
    int r = hal_osd_norm_rotation(rotation_deg);

    return (r == 90 || r == 270);
}

/** VPSS MPI OSD (BindVi/Canvas/Region) is only used when rotation is 0 or 180. */
static int hal_osd_mpi_osd_needed(void)
{
    return !hal_osd_rotation_uses_ipu(s_camera_rotation_deg);
}

static void hal_osd_load_camera_rotation(void)
{
    char path[256];
    dictionary *ini;

    if (config_build_path(CONFIG_PATH_TYPE_CONFIG, "hal_camera_config.ini",
                          path, sizeof(path)) != 0) {
        return;
    }
    ini = iniparser_load(path);
    if (!ini) {
        return;
    }
    s_camera_rotation_deg = hal_osd_norm_rotation(iniparser_getint(ini, "camera0:rotation", 0));
    iniparser_freedict(ini);
}

static int hal_osd_mpi_osd_init_once(void)
{
    OSD_ATTR_S stOsdAttrValue = {0};

    if (!hal_osd_mpi_osd_needed()) {
        log_info("skip AK_MPI_OSD_Init (rotation=%d uses IPU OSD)", s_camera_rotation_deg);
        return 0;
    }
    if (s_mpi_osd_inited) {
        return 0;
    }

    stOsdAttrValue.enColorModeIndex = AK_OSD_COLOR_FMT_RGB;
    stOsdAttrValue.enColorType = s32GColorType;
    stOsdAttrValue.pu32ColorTable = u32RgbColorTables;
    if (AK_SUCCESS != AK_MPI_OSD_Init(&stOsdAttrValue)) {
        log_error("AK_MPI_OSD_Init failed!!");
        return -1;
    }
    s_mpi_osd_inited = 1;
    return 0;
}

static void hal_osd_mpi_osd_destroy_once(void)
{
    if (!s_mpi_osd_inited) {
        return;
    }
    AK_MPI_OSD_Destroy();
    s_mpi_osd_inited = 0;
}

/** VPSS 通道仍有效且尺寸非零时才可向 VI 送画布，避免管道正在拆除时 ShowCanvas 长时间阻塞或 DMA 异常。 */
static int hal_osd_vpss_chn_ready(int nChn)
{
    VPSS_CHN_ATTR_S stVpssChnAttr = {0};

    if (AK_MPI_VPSS_GetChnAttr(g_s32GrpIndex, nChn, &stVpssChnAttr) != AK_SUCCESS)
        return 0;
    if (stVpssChnAttr.u32Width == 0 || stVpssChnAttr.u32Height == 0)
        return 0;
    return 1;
}

static void hal_osd_free_rot_buf(int nChn, int nObj)
{
    if (nChn < 0 || nChn >= MAX_CHN_CNT || nObj < 0 || nObj >= HAL_OSD_OBJ_COUNT)
        return;
    if (g_rot_bmp_buf[nChn][nObj]) {
        free(g_rot_bmp_buf[nChn][nObj]);
        g_rot_bmp_buf[nChn][nObj] = NULL;
        g_rot_bmp_buf_size[nChn][nObj] = 0;
    }
}

static int hal_osd_get_vpss_size(int nChn, int *vpss_w, int *vpss_h)
{
    VPSS_CHN_ATTR_S stVpssChnAttr = {0};

    if (!vpss_w || !vpss_h)
        return -1;
    if (AK_MPI_VPSS_GetChnAttr(g_s32GrpIndex, nChn, &stVpssChnAttr) != AK_SUCCESS)
        return -1;
    *vpss_w = (int)stVpssChnAttr.u32Width;
    *vpss_h = (int)stVpssChnAttr.u32Height;
    return 0;
}

int hal_osd_get_bpp(void)
{
    return s32BitWidth;
}

int hal_osd_set_rotation(int nChn, int rotation_deg, int display_width, int display_height)
{
    if (nChn < 0 || nChn >= MAX_CHN_CNT) {
        return -1;
    }
    s_camera_rotation_deg = hal_osd_norm_rotation(rotation_deg);
    return hal_osd_ipu_configure(nChn, rotation_deg, display_width, display_height);
}

int hal_osd_preset_rotation(int rotation_deg)
{
    s_camera_rotation_deg = hal_osd_norm_rotation(rotation_deg);
    log_info("hal_osd preset rotation=%d mpi_osd=%d",
             s_camera_rotation_deg, hal_osd_mpi_osd_needed());
    return 0;
}

static int hal_osd_prepare_font_attr(int nChn, int nObj, const char *cText, int iTextLen,
                                     OSD_FONT_ATTR_S *pstFontAttr, int *pusTextLen)
{
    int usTextLen;

    if (!pstFontAttr || !cText || iTextLen <= 0)
        return -1;

    usTextLen = char_to_short(g_ChnInfo[nChn].ObjInfo[nObj].info.Text.u16UsOsdString,
                              (unsigned char *)cText);
    memset(pstFontAttr, 0, sizeof(*pstFontAttr));
    pstFontAttr->s32FontFileSize = 16;
    pstFontAttr->s32FontSize = g_ChnInfo[nChn].iCurFontSize;
    pstFontAttr->pcfile = pcFontFile;
    pstFontAttr->s32FontInterval = s32FontInterval;
    pstFontAttr->pu16Str = g_ChnInfo[nChn].ObjInfo[nObj].info.Text.u16UsOsdString;
    pstFontAttr->s32StrLen = usTextLen;
    if (pusTextLen)
        *pusTextLen = usTextLen;
    return 0;
}

static int hal_osd_bitmap_bpp_from_size(int w, int h, int data_size)
{
    int size_1bpp;
    int size_2bpp;
    int size_4bpp;

    if (w <= 0 || h <= 0 || data_size <= 0) {
        return s32BitWidth;
    }

    size_1bpp = (w * h + 7) / 8;
    size_2bpp = (w * h * 2 + 7) / 8;
    size_4bpp = (w * h * 4 + 7) / 8;
    if (data_size == size_1bpp) {
        return 1;
    }
    if (data_size == size_2bpp) {
        return 2;
    }
    if (data_size == size_4bpp) {
        return 4;
    }
    return s32BitWidth;
}

static void hal_osd_set_1bpp_pixel(unsigned char *bmp, int bw, int x, int y, int on)
{
    int bit_idx;
    int byte_idx;
    int shift;

    if (!bmp || x < 0 || y < 0 || bw <= 0) {
        return;
    }
    bit_idx = y * bw + x;
    byte_idx = bit_idx / 8;
    shift = 7 - (bit_idx % 8);
    if (on) {
        bmp[byte_idx] |= (unsigned char)(1U << shift);
    } else {
        bmp[byte_idx] &= (unsigned char)~(1U << shift);
    }
}

/* 8x16 glyph rows for digits and time punctuation (MSB = left pixel). */
static const AK_U8 g_ascii_glyph8x16[12][16] = {
    {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 0 */
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 1 */
    {0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 2 */
    {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 3 */
    {0x0C,0x1C,0x2C,0x4C,0x7E,0x0C,0x0C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 4 */
    {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 5 */
    {0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 6 */
    {0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 7 */
    {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 8 */
    {0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 9 */
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* - */
    {0x00,0x00,0x00,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x08,0x00,0x00,0x00,0x00,0x00}, /* : */
};

static void hal_osd_fill_rect_1bpp(unsigned char *bmp, int bw, int bh,
                                   int x0, int y0, int w, int h)
{
    int y;
    int x;

    if (!bmp || w <= 0 || h <= 0) {
        return;
    }
    for (y = y0; y < y0 + h; y++) {
        for (x = x0; x < x0 + w; x++) {
            if (x >= 0 && y >= 0 && x < bw && y < bh) {
                hal_osd_set_1bpp_pixel(bmp, bw, x, y, 1);
            }
        }
    }
}

/** Draw colon aligned with 8x16 digit ink band (glyph rows 0-6). */
static void hal_osd_draw_colon(unsigned char *bmp, int bw, int bh,
                               int x, int cell_w, int font_sz)
{
    int vis_h = (font_sz * 7 + 8) / 16;
    int dot = font_sz / 8;
    int gap;
    int cx;
    int y1;
    int y2;

    if (!bmp || cell_w <= 0 || font_sz <= 0 || vis_h <= 0) {
        return;
    }
    if (dot < 3) {
        dot = 3;
    }
    if (dot > 8) {
        dot = 8;
    }

    gap = vis_h / 3;
    if (gap < dot + 2) {
        gap = dot + 2;
    }

    cx = x + (cell_w - dot) / 2;
    y1 = (vis_h - gap - dot) / 2;
    y2 = y1 + gap + dot;
    /* Nudge up to match digit visual center (glyph ink sits high in the cell). */
    y1 -= (font_sz / 12 + 1);
    y2 -= (font_sz / 12 + 1);
    if (y1 < 0) {
        y1 = 0;
    }
    if (y2 + dot > bh) {
        y2 = bh - dot;
    }

    hal_osd_fill_rect_1bpp(bmp, bw, bh, cx, y1, dot, dot);
    hal_osd_fill_rect_1bpp(bmp, bw, bh, cx, y2, dot, dot);
}

static int hal_osd_ascii_char_width(char ch, int font_sz)
{
    int asc_w = font_sz / 2 + s32FontInterval;

    if (ch == ':') {
        /* ~40% digit width, enough gap around the dots */
        return (asc_w * 2 + 2) / 5;
    }
    if (ch == '-') {
        return asc_w;
    }
    if (ch == ' ') {
        return asc_w / 2;
    }
    if (ch >= '0' && ch <= '9') {
        return asc_w;
    }
    if ((unsigned char)ch > 0x7F) {
        return font_sz + s32FontInterval;
    }
    return asc_w;
}

static int hal_osd_calc_ascii_text_wh(const char *cText, int iTextLen, int font_sz, int *pW, int *pH)
{
    int w = 0;
    int i;

    if (!cText || iTextLen <= 0 || !pW || !pH) {
        return -1;
    }
    for (i = 0; i < iTextLen; i++) {
        w += hal_osd_ascii_char_width(cText[i], font_sz);
    }
    *pW = w;
    *pH = font_sz;
    return 0;
}

static const AK_U8 *hal_osd_ascii_glyph_rows(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return g_ascii_glyph8x16[ch - '0'];
    }
    if (ch == '-') {
        return g_ascii_glyph8x16[10];
    }
    if (ch == ':') {
        return g_ascii_glyph8x16[11];
    }
    return NULL;
}

static void hal_osd_blit_glyph8x16_scaled(unsigned char *bmp, int bw, int bh,
                                          int dst_x, int dst_y, int dst_w, int dst_h,
                                          const AK_U8 *rows)
{
    int y;
    int x;

    if (!bmp || !rows || dst_w <= 0 || dst_h <= 0) {
        return;
    }
    for (y = 0; y < dst_h; y++) {
        int sy = y * 16 / dst_h;
        AK_U8 row = rows[sy];
        for (x = 0; x < dst_w; x++) {
            int sx = x * 8 / dst_w;
            int on = (row >> (7 - sx)) & 1;
            int dx = dst_x + x;
            int dy = dst_y + y;
            if (dx >= 0 && dy >= 0 && dx < bw && dy < bh) {
                hal_osd_set_1bpp_pixel(bmp, bw, dx, dy, on);
            }
        }
    }
}

static int hal_osd_render_text_ascii_bitmap(int nChn, const char *cText, int iTextLen,
                                            unsigned char **out_buf, int *out_w, int *out_h,
                                            int *out_len)
{
    int font_sz = g_ChnInfo[nChn].iCurFontSize;
    int bw = 0;
    int bh = 0;
    int buflen;
    unsigned char *bmp = NULL;
    int x = 0;
    int i;

    if (hal_osd_calc_ascii_text_wh(cText, iTextLen, font_sz, &bw, &bh) != 0 || bw <= 0 || bh <= 0) {
        return -1;
    }
    buflen = (bw * bh + 7) / 8;
    bmp = (unsigned char *)calloc(1, (size_t)buflen);
    if (!bmp) {
        return -1;
    }

    for (i = 0; i < iTextLen; i++) {
        const AK_U8 *glyph = hal_osd_ascii_glyph_rows(cText[i]);
        int cell_w = hal_osd_ascii_char_width(cText[i], font_sz);

        if (cText[i] == ':') {
            hal_osd_draw_colon(bmp, bw, bh, x, cell_w, font_sz);
            x += cell_w;
            continue;
        }
        if (!glyph) {
            x += cell_w;
            continue;
        }
        hal_osd_blit_glyph8x16_scaled(bmp, bw, bh, x, 0, cell_w, font_sz, glyph);
        x += cell_w;
    }

    *out_buf = bmp;
    *out_w = bw;
    *out_h = bh;
    *out_len = buflen;
    return 0;
}

static int hal_osd_render_text_getstrbmp(int nChn, int nObj, const char *cText, int iTextLen,
                                        unsigned char **out_buf, int *out_w, int *out_h,
                                        int *out_len)
{
    OSD_FONT_ATTR_S stFontAttr = {0};
    OSD_BITMAP_INFO_S stBmp = {0};
    OSD_CHANGE_FONT_COLOR_S *pstFontColor = NULL;
    int usTextLen;
    int i;
    unsigned char *copy = NULL;

    if (!hal_osd_mpi_osd_needed() || !s_mpi_osd_inited) {
        return -1;
    }
    if (hal_osd_prepare_font_attr(nChn, nObj, cText, iTextLen, &stFontAttr, &usTextLen) != 0) {
        return -1;
    }
    pstFontColor = (OSD_CHANGE_FONT_COLOR_S *)calloc((size_t)usTextLen, sizeof(OSD_CHANGE_FONT_COLOR_S));
    if (!pstFontColor) {
        return -1;
    }
    for (i = 0; i < usTextLen; i++) {
        pstFontColor[i].s32FontColor = s32GFontColor;
        pstFontColor[i].s32BgColor = s32GBgColor;
        pstFontColor[i].s32EdgeColor = s32GEdgeColor;
    }
    stFontAttr.pstFontColor = pstFontColor;

    if (AK_MPI_OSD_GetStrBmp(&stFontAttr, &stBmp) != AK_SUCCESS) {
        free(pstFontColor);
        return -1;
    }
    free(pstFontColor);

    copy = (unsigned char *)malloc((size_t)stBmp.u32BufferLength);
    if (!copy) {
        AK_MPI_OSD_ReleaseBmpBuf(&stBmp, (AK_S32)stBmp.u32BufferLength);
        return -1;
    }
    memcpy(copy, stBmp.pu8Buffer, (size_t)stBmp.u32BufferLength);
    AK_MPI_OSD_ReleaseBmpBuf(&stBmp, (AK_S32)stBmp.u32BufferLength);

    *out_buf = copy;
    *out_w = stBmp.stOsdBmpAttr.s32BitmapWidth;
    *out_h = stBmp.stOsdBmpAttr.s32BitmapHeight;
    *out_len = (int)stBmp.u32BufferLength;
    return 0;
}

int hal_osd_render_text_bitmap(int nChn, int nObj, const char *cText, int iTextLen,
                               int src_w, int src_h,
                               unsigned char **out_buf, int *out_w, int *out_h,
                               int *out_len)
{
    (void)src_w;
    (void)src_h;
    (void)nObj;

    if (!cText || iTextLen <= 0 || !out_buf || !out_w || !out_h || !out_len) {
        return -1;
    }

    /* IPU path uses built-in ASCII glyphs; skip GetStrBmp to avoid SDK pixel_format errors. */
    if (hal_osd_ipu_mode(nChn)) {
        return hal_osd_render_text_ascii_bitmap(nChn, cText, iTextLen, out_buf, out_w, out_h, out_len);
    }

    if (hal_osd_render_text_getstrbmp(nChn, nObj, cText, iTextLen, out_buf, out_w, out_h, out_len) == 0) {
        return 0;
    }

    if (hal_osd_render_text_ascii_bitmap(nChn, cText, iTextLen, out_buf, out_w, out_h, out_len) == 0) {
        return 0;
    }

    log_warn("render_text_bitmap failed ch=%d (GetStrBmp+ascii)", nChn);
    return -1;
}

static int hal_osd_setup_font_obj(int nChn, int nTextId, int x, int y,
                                  const char *cText, int iTextLen,
                                  const VPSS_CHN_ATTR_S *pstVpssChnAttr)
{
    int usTextLen;
    OSD_FONT_ATTR_S stFontAttr = {0};
    AK_S32 s32Width;
    AK_S32 s32Height;
    AK_S32 s32MaxW = pstVpssChnAttr->u32Width;
    AK_S32 s32MaxH = pstVpssChnAttr->u32Height;

    g_ChnInfo[nChn].ObjInfo[nTextId].bEnable = 1;
    g_ChnInfo[nChn].ObjInfo[nTextId].bUpdate = 1;
    g_ChnInfo[nChn].ObjInfo[nTextId].nObjType = OSD_REG_FONT;

    usTextLen = char_to_short(g_ChnInfo[nChn].ObjInfo[nTextId].info.Text.u16UsOsdString,
                              (unsigned char *)cText);
    stFontAttr.s32FontFileSize = 16;
    stFontAttr.s32FontSize = g_ChnInfo[nChn].iCurFontSize;
    stFontAttr.pcfile = pcFontFile;
    stFontAttr.s32FontInterval = s32FontInterval;
    stFontAttr.pu16Str = g_ChnInfo[nChn].ObjInfo[nTextId].info.Text.u16UsOsdString;
    stFontAttr.s32StrLen = usTextLen;

    s32Width = iTextLen * (g_ChnInfo[nChn].iFontWidth + stFontAttr.s32FontInterval);
    s32Height = g_ChnInfo[nChn].iFontHeight;
    s32Width = (s32Width > s32MaxW) ? s32MaxW : s32Width;
    s32Height = (s32Height > s32MaxH) ? s32MaxH : s32Height;

    memcpy(&g_ChnInfo[nChn].ObjInfo[nTextId].info.Text.stFontAttr, &stFontAttr, sizeof(stFontAttr));
    g_ChnInfo[nChn].ObjInfo[nTextId].rc.x = x;
    g_ChnInfo[nChn].ObjInfo[nTextId].rc.y = y;
    g_ChnInfo[nChn].ObjInfo[nTextId].rc.width = s32Width + 2;
    g_ChnInfo[nChn].ObjInfo[nTextId].rc.height = s32Height;
    return 0;
}

/**
 * @brief Convert character to short code.
 */
static int char_to_short(unsigned short *dest, const unsigned char *src)
{
    int count = 0;
    while (*src) {
        if ((*src < 0x80) && (*src > 0x00)) {
            *dest = *src;
            dest++;
            src++;
            count++;
        } else if (*src < 0xA0) {
            src += 2;
        } else if (*(src + 1) < 0xA0) {
            src += 2;
        } else {
            *dest = ((unsigned short)(*(src + 1) << 8)) | *src;
            dest++;
            src += 2;
            count++;
        }
    }
    return count;
}

/**
 * @brief Get ASCII character count.
 */
static int get_asc_sum(unsigned short *src, int total)
{
    unsigned short *p = src;
    int asc_sum = 0;
    int i = 0;
    for (i = 0; i < total; i++) {
        if (*(p + i) < 0x80) {
            asc_sum++;
        }
    }
    return asc_sum;
}

/**
 * @brief Get text width and height from unicode string.
 */
static int hal_osd_get_ustext_wh(int iFontSize, int CharInterval,
                                  unsigned short usOsdStr[], int iOsdLen,
                                  int *pW, int *pH)
{
    int iAscSum = get_asc_sum(usOsdStr, iOsdLen);
    *pW = (iOsdLen - iAscSum) * iFontSize + iAscSum * (iFontSize / 2 + CharInterval);
    *pH = iFontSize;
    log_debug("[%s][%d] iFontSize=%d iOsdLen=%d iAscSum=%d, *pW=%d *pH=%d",
           __FUNCTION__, __LINE__, iFontSize, iOsdLen, iAscSum, *pW, *pH);
    return 0;
}

/**
 * @brief Clean OSD objects (no cut mode).
 */
static int hal_osd_clean_obj_no_cut(int nChn, int nObj, int OnlyMem)
{
    if (!hal_osd_mpi_osd_needed()) {
        return 0;
    }

    for (int ch = 0; ch < MAX_CHN_CNT; ch++) {
        if (nChn != -1 && nChn != ch) {
            continue;
        }

        for (int i = 0; i < HAL_OSD_OBJ_COUNT; i++) {
            if (nObj != -1 && nObj != i) {
                continue;
            }

            if (g_ChnInfo[ch].ObjInfo[i].s32RegionId < 0) {
                continue;
            }

            AK_MPI_OSD_DetachRegion(g_ChnInfo[ch].ObjInfo[i].s32RegionId,
                                    g_ChnInfo[ch].ObjInfo[i].s32CanvasId);
            AK_MPI_OSD_UnbindVi(g_ChnInfo[ch].ObjInfo[i].s32CanvasId, ch, 0);
            AK_MPI_OSD_DestroyRegion(g_ChnInfo[ch].ObjInfo[i].s32RegionId);
            AK_MPI_OSD_DestroyCanvas(g_ChnInfo[ch].ObjInfo[i].s32CanvasId);

            g_ChnInfo[ch].ObjInfo[i].s32RegionId = -1;
            g_ChnInfo[ch].ObjInfo[i].s32CanvasId = -1;

            hal_osd_free_rot_buf(ch, i);

            if (OnlyMem == 0) {
                g_ChnInfo[ch].ObjInfo[i].bEnable = 0;
            }
        }
    }
    return 0;
}

/**
 * @brief Clean OSD objects (cut mode).
 */
static int hal_osd_clean_obj_cut(int nChn, int nObj, int OnlyMem)
{
    if (!hal_osd_mpi_osd_needed()) {
        return 0;
    }

    for (int ch = 0; ch < MAX_CHN_CNT; ch++) {
        if (nChn != -1 && nChn != ch) {
            continue;
        }

        for (int i = 0; i < HAL_OSD_OBJ_COUNT; i++) {
            if (nObj != -1 && nObj != i) {
                continue;
            }

            if (g_ChnInfo[ch].ObjInfo[i].s32RegionId < 0) {
                continue;
            }

            AK_MPI_OSD_DetachRegion(g_ChnInfo[ch].ObjInfo[i].s32RegionId,
                                    g_ChnInfo[ch].ObjInfo[i].s32CanvasId);
            AK_MPI_OSD_DestroyRegion(g_ChnInfo[ch].ObjInfo[i].s32RegionId);

            g_ChnInfo[ch].ObjInfo[i].s32RegionId = -1;
            g_ChnInfo[ch].ObjInfo[i].s32CanvasId = -1;

            hal_osd_free_rot_buf(ch, i);

            if (OnlyMem == 0) {
                g_ChnInfo[ch].ObjInfo[i].bEnable = 0;
            }
        }

        if (g_ChnInfo[ch].s32BigCanvasId >= 0) {
            AK_MPI_OSD_UnbindVi(g_ChnInfo[ch].s32BigCanvasId, ch, 0);
            AK_MPI_OSD_DestroyCanvas(g_ChnInfo[ch].s32BigCanvasId);
            g_ChnInfo[ch].s32BigCanvasId = -1;
        }
    }
    return 0;
}

/**
 * @brief Prepare OSD (no cut mode).
 */
static int hal_osd_prepare_no_cut(int nChn)
{
    if (!hal_osd_mpi_osd_needed() || hal_osd_ipu_mode(nChn)) {
        return 0;
    }

    for (int i = 0; i < HAL_OSD_OBJ_COUNT; i++) {
        if (g_ChnInfo[nChn].ObjInfo[i].bEnable == 0) {
            continue;
        }

        if (g_ChnInfo[nChn].ObjInfo[i].s32RegionId >= 0) {
            continue;
        }

        osd_rect_info *prc = &g_ChnInfo[nChn].ObjInfo[i].rc;
        if (prc->width <= 0 || prc->height <= 0) {
            log_warn("skip prepare_no_cut: invalid region size ch=%d obj=%d w=%d h=%d",
                     nChn, i, prc->width, prc->height);
            continue;
        }

        AK_S32 s32CanvasId = nChn * 4 + i;
        OSD_CANVAS_INFO_S stOsdCanvasInfo = {0};
        stOsdCanvasInfo.s32CanvasSizeWidth = prc->width;
        stOsdCanvasInfo.s32CanvasSizeHeight = prc->height;
        stOsdCanvasInfo.s32CanvasColorIndex = 0;
        stOsdCanvasInfo.s32BitWidth = s32BitWidth;
        stOsdCanvasInfo.stPosInYuv.u32X = prc->x;
        stOsdCanvasInfo.stPosInYuv.u32Y = prc->y;

        if (AK_SUCCESS != AK_MPI_OSD_CreateCanvas(s32CanvasId, &stOsdCanvasInfo)) {
            log_error("AK_MPI_OSD_CreateCanvas failed!!");
            continue;
        }

        AK_S32 s32RegionId = (AK_S32)nChn * 16 + i;
        OSD_REGION_ATTR_S stRegionAttr = {0};
        stRegionAttr.s32Width = prc->width;
        stRegionAttr.s32Height = prc->height;
        stRegionAttr.s32Priority = 1;

        if (AK_SUCCESS != AK_MPI_OSD_CreateRegion(s32RegionId, &stRegionAttr)) {
            log_error("AK_MPI_OSD_CreateRegion failed!!");
            AK_MPI_OSD_DestroyCanvas(s32CanvasId);
            continue;
        }

        AK_S32 s32ChannelId = nChn;
        if (AK_SUCCESS != AK_MPI_OSD_BindVi(s32CanvasId, s32ChannelId, 0)) {
            log_error("AK_MPI_OSD_BindVi failed!!");
            AK_MPI_OSD_DestroyRegion(s32RegionId);
            AK_MPI_OSD_DestroyCanvas(s32CanvasId);
            continue;
        }

        OSD_VECTOR_S stPos = {0};
        stPos.u32X = 0;
        stPos.u32Y = 0;

        if (AK_SUCCESS != AK_MPI_OSD_AttachRegion(stPos, s32RegionId, s32CanvasId)) {
            log_error("AK_MPI_OSD_AttachRegion failed!!");
            AK_MPI_OSD_UnbindVi(s32CanvasId, nChn, 0);
            AK_MPI_OSD_DestroyRegion(s32RegionId);
            AK_MPI_OSD_DestroyCanvas(s32CanvasId);
            continue;
        }

        g_ChnInfo[nChn].ObjInfo[i].s32CanvasId = s32CanvasId;
        g_ChnInfo[nChn].ObjInfo[i].s32RegionId = s32RegionId;
        g_ChnInfo[nChn].ObjInfo[i].bUpdate = 1;
    }
    return 0;
}

/**
 * @brief Draw OSD (no cut mode).
 */
static int hal_osd_draw_no_cut(int nChn)
{
    if (!hal_osd_mpi_osd_needed() || hal_osd_ipu_mode(nChn)) {
        return 0;
    }

    if (!hal_osd_vpss_chn_ready(nChn)) {
        log_warn("skip draw_no_cut: vpss chn not ready ch=%d", nChn);
        return 0;
    }

    for (int i = 0; i < HAL_OSD_OBJ_COUNT; i++) {
        if (g_ChnInfo[nChn].ObjInfo[i].bEnable == 0) {
            continue;
        }

        if (g_ChnInfo[nChn].ObjInfo[i].bUpdate == 0) {
            continue;
        }

        if (g_ChnInfo[nChn].ObjInfo[i].s32RegionId < 0 ||
            g_ChnInfo[nChn].ObjInfo[i].s32CanvasId < 0) {
            log_warn("skip draw_no_cut: invalid region/canvas ch=%d obj=%d region=%d canvas=%d",
                     nChn, i, g_ChnInfo[nChn].ObjInfo[i].s32RegionId,
                     g_ChnInfo[nChn].ObjInfo[i].s32CanvasId);
            g_ChnInfo[nChn].ObjInfo[i].bUpdate = 0;
            continue;
        }

        if (g_ChnInfo[nChn].ObjInfo[i].nObjType == OSD_REG_FONT) {
            AK_S32 s32Status = 1;
            OSD_REGION_DATA_S stRegionData = {0};
            stRegionData.enType = OSD_REG_FONT;
            memcpy(&stRegionData.data.stFontAttr,
                   &g_ChnInfo[nChn].ObjInfo[i].info.Text.stFontAttr,
                   sizeof(OSD_FONT_ATTR_S));
            OSD_CHANGE_FONT_COLOR_S *pstFontColor = (OSD_CHANGE_FONT_COLOR_S *)malloc(
                sizeof(OSD_CHANGE_FONT_COLOR_S) * stRegionData.data.stFontAttr.s32StrLen);
            if (pstFontColor == NULL) {
                log_error("draw_no_cut: malloc pstFontColor failed ch=%d obj=%d", nChn, i);
                continue;
            }
            for (int j = 0; j < stRegionData.data.stFontAttr.s32StrLen; j++) {
                pstFontColor[j].s32FontColor = s32GFontColor;
                pstFontColor[j].s32BgColor = s32GBgColor;
                pstFontColor[j].s32EdgeColor = s32GEdgeColor;
            }
            stRegionData.data.stFontAttr.pstFontColor = pstFontColor;
            if (AK_MPI_OSD_UpdateRegion(g_ChnInfo[nChn].ObjInfo[i].s32RegionId,
                                        s32Status, &stRegionData)) {
                log_error("AK_MPI_OSD_UpdateRegion failed!!");
            }
            free(pstFontColor);
            if (hal_osd_vpss_chn_ready(nChn) && g_ChnInfo[nChn].ObjInfo[i].s32CanvasId >= 0 &&
                AK_MPI_OSD_ShowCanvas(g_ChnInfo[nChn].ObjInfo[i].s32CanvasId)) {
                log_error("AK_MPI_OSD_ShowCanvas failed!!");
            }
            g_ChnInfo[nChn].ObjInfo[i].bUpdate = 0;
        } else if (g_ChnInfo[nChn].ObjInfo[i].nObjType == OSD_REG_BITMAP) {
            AK_U32 u32Status = 1;
            OSD_REGION_DATA_S stRegionData = {0};
            stRegionData.enType = OSD_REG_BITMAP;
            stRegionData.data.stOsdBitmapInfo.pu8Buffer =
                g_ChnInfo[nChn].ObjInfo[i].info.Img.pData;
            stRegionData.data.stOsdBitmapInfo.u32BufferLength =
                g_ChnInfo[nChn].ObjInfo[i].info.Img.iDataSize;
            stRegionData.data.stOsdBitmapInfo.stOsdBmpAttr.s32BitmapWidth =
                g_ChnInfo[nChn].ObjInfo[i].rc.width;
            stRegionData.data.stOsdBitmapInfo.stOsdBmpAttr.s32BitmapHeight =
                g_ChnInfo[nChn].ObjInfo[i].rc.height;
            stRegionData.data.stOsdBitmapInfo.stOsdBmpAttr.s32BitPerPixel = s32BitWidth;
            if (AK_MPI_OSD_UpdateRegion(g_ChnInfo[nChn].ObjInfo[i].s32RegionId,
                                        u32Status, &stRegionData)) {
                log_error("AK_MPI_OSD_UpdateRegion failed!!");
            }
            if (hal_osd_vpss_chn_ready(nChn) && g_ChnInfo[nChn].ObjInfo[i].s32CanvasId >= 0 &&
                AK_MPI_OSD_ShowCanvas(g_ChnInfo[nChn].ObjInfo[i].s32CanvasId)) {
                log_error("AK_MPI_OSD_ShowCanvas failed!!");
            }
            g_ChnInfo[nChn].ObjInfo[i].bUpdate = 0;
        } else {
            log_warn("draw_no_cut: unknown obj type ch=%d obj=%d type=%d",
                     nChn, i, g_ChnInfo[nChn].ObjInfo[i].nObjType);
            g_ChnInfo[nChn].ObjInfo[i].bUpdate = 0;
        }
    }
    return 0;
}

/**
 * @brief Prepare OSD (cut mode).
 */
static int hal_osd_prepare_cut(int nChn)
{
    if (!hal_osd_mpi_osd_needed() || hal_osd_ipu_mode(nChn)) {
        return 0;
    }

    AK_S32 s32CanvasId = nChn * 4;
    if (g_ChnInfo[nChn].s32BigCanvasId < 0) {
        osd_rect_info *prc = &g_ChnInfo[nChn].rcBig;
        if (prc->width <= 0 || prc->height <= 0) {
            log_warn("skip prepare_cut: invalid canvas size ch=%d w=%d h=%d",
                     nChn, prc->width, prc->height);
            return -1;
        }
        OSD_CANVAS_INFO_S stOsdCanvasInfo = {0};
        stOsdCanvasInfo.s32CanvasSizeWidth = prc->width;
        stOsdCanvasInfo.s32CanvasSizeHeight = prc->height;
        stOsdCanvasInfo.s32CanvasColorIndex = 0;
        stOsdCanvasInfo.s32BitWidth = s32BitWidth;
        stOsdCanvasInfo.stPosInYuv.u32X = prc->x;
        stOsdCanvasInfo.stPosInYuv.u32Y = prc->y;

        if (AK_SUCCESS != AK_MPI_OSD_CreateCanvas(s32CanvasId, &stOsdCanvasInfo)) {
            log_error("AK_MPI_OSD_CreateCanvas failed!!");
            return -1;
        }
        g_ChnInfo[nChn].s32BigCanvasId = s32CanvasId;

        AK_S32 s32ChannelId = nChn;
        if (AK_SUCCESS != AK_MPI_OSD_BindVi(s32CanvasId, s32ChannelId, 0)) {
            log_error("AK_MPI_OSD_BindVi failed!!");
            AK_MPI_OSD_DestroyCanvas(s32CanvasId);
            g_ChnInfo[nChn].s32BigCanvasId = -1;
            return -1;
        }
    }

    for (int i = HAL_OSD_OBJ_COUNT - 1; i >= 0; i--) {
        if (g_ChnInfo[nChn].ObjInfo[i].bEnable == 0) {
            continue;
        }

        if (g_ChnInfo[nChn].ObjInfo[i].s32RegionId >= 0) {
            continue;
        }

        osd_rect_info *prc = &g_ChnInfo[nChn].ObjInfo[i].rc;
        AK_S32 s32RegionId = (AK_S32)nChn * 16 + i;
        OSD_REGION_ATTR_S stRegionAttr = {0};
        stRegionAttr.s32Width = prc->width;
        stRegionAttr.s32Height = prc->height;
        stRegionAttr.s32Priority = 0;
        if (stRegionAttr.s32Width <= 0 || stRegionAttr.s32Height <= 0) {
            log_warn("skip create region: invalid size ch=%d obj=%d w=%d h=%d",
                     nChn, i, stRegionAttr.s32Width, stRegionAttr.s32Height);
            continue;
        }

        log_debug("[%s][%d] nChn=%d AK_MPI_OSD_CreateRegion:%d start",
               __FUNCTION__, __LINE__, nChn, s32RegionId);
        log_debug("[%s][%d] nChn=%d,ObjId=%d,xy=(%d,%d)wh=(%d,%d)",
               __FUNCTION__, __LINE__, nChn, i, prc->x, prc->y, prc->width, prc->height);

        if (AK_SUCCESS != AK_MPI_OSD_CreateRegion(s32RegionId, &stRegionAttr)) {
            log_error("AK_MPI_OSD_CreateRegion failed!!");
            continue;
        }
        g_ChnInfo[nChn].ObjInfo[i].bUpdate = 1;

        OSD_VECTOR_S stPos = {0};
        stPos.u32X = prc->x;
        stPos.u32Y = prc->y;

        if (AK_SUCCESS != AK_MPI_OSD_AttachRegion(stPos, s32RegionId, s32CanvasId)) {
            log_error("AK_MPI_OSD_AttachRegion failed!!");
            continue;
        }

        g_ChnInfo[nChn].ObjInfo[i].s32CanvasId = s32CanvasId;
        g_ChnInfo[nChn].ObjInfo[i].s32RegionId = s32RegionId;
    }
    return 0;
}

/**
 * @brief Draw OSD (cut mode).
 */
static int hal_osd_draw_cut(int nChn)
{
    if (!hal_osd_mpi_osd_needed() || hal_osd_ipu_mode(nChn)) {
        return 0;
    }

    if (g_ChnInfo[nChn].s32BigCanvasId < 0) {
        log_warn("skip draw_cut: no big canvas ch=%d", nChn);
        return 0;
    }

    for (int i = HAL_OSD_OBJ_COUNT - 1; i >= 0; i--) {
        if (g_ChnInfo[nChn].ObjInfo[i].bEnable == 0) {
            continue;
        }

        if (g_ChnInfo[nChn].ObjInfo[i].bUpdate == 0) {
            continue;
        }

        if (g_ChnInfo[nChn].ObjInfo[i].s32RegionId < 0) {
            log_warn("skip draw_cut: invalid region ch=%d obj=%d", nChn, i);
            g_ChnInfo[nChn].ObjInfo[i].bUpdate = 0;
            continue;
        }

        if (g_ChnInfo[nChn].ObjInfo[i].nObjType == OSD_REG_FONT) {
            AK_S32 s32Status = 1;
            OSD_REGION_DATA_S stRegionData = {0};
            stRegionData.enType = OSD_REG_FONT;
            memcpy(&stRegionData.data.stFontAttr,
                   &g_ChnInfo[nChn].ObjInfo[i].info.Text.stFontAttr,
                   sizeof(OSD_FONT_ATTR_S));

            OSD_CHANGE_FONT_COLOR_S *pstFontColor = (OSD_CHANGE_FONT_COLOR_S *)malloc(
                sizeof(OSD_CHANGE_FONT_COLOR_S) * stRegionData.data.stFontAttr.s32StrLen);
            if (pstFontColor == NULL) {
                log_error("draw_cut: malloc pstFontColor failed ch=%d obj=%d", nChn, i);
                continue;
            }
            for (int j = 0; j < stRegionData.data.stFontAttr.s32StrLen; j++) {
                pstFontColor[j].s32FontColor = s32GFontColor;
                pstFontColor[j].s32BgColor = s32GBgColor;
                pstFontColor[j].s32EdgeColor = s32GEdgeColor;
            }
            stRegionData.data.stFontAttr.pstFontColor = pstFontColor;

            if (AK_MPI_OSD_UpdateRegion(g_ChnInfo[nChn].ObjInfo[i].s32RegionId,
                                        s32Status, &stRegionData)) {
                log_error("AK_MPI_OSD_UpdateRegion failed!!");
            }
            free(pstFontColor);
            g_ChnInfo[nChn].ObjInfo[i].bUpdate = 0;
        } else if (g_ChnInfo[nChn].ObjInfo[i].nObjType == OSD_REG_BITMAP) {
            AK_U32 u32Status = 1;
            OSD_REGION_DATA_S stRegionData = {0};
            stRegionData.enType = OSD_REG_BITMAP;
            stRegionData.data.stOsdBitmapInfo.pu8Buffer =
                g_ChnInfo[nChn].ObjInfo[i].info.Img.pData;
            stRegionData.data.stOsdBitmapInfo.u32BufferLength =
                g_ChnInfo[nChn].ObjInfo[i].info.Img.iDataSize;
            stRegionData.data.stOsdBitmapInfo.stOsdBmpAttr.s32BitmapWidth =
                g_ChnInfo[nChn].ObjInfo[i].rc.width;
            stRegionData.data.stOsdBitmapInfo.stOsdBmpAttr.s32BitmapHeight =
                g_ChnInfo[nChn].ObjInfo[i].rc.height;
            stRegionData.data.stOsdBitmapInfo.stOsdBmpAttr.s32BitPerPixel = s32BitWidth;

            if (AK_MPI_OSD_UpdateRegion(g_ChnInfo[nChn].ObjInfo[i].s32RegionId,
                                        u32Status, &stRegionData)) {
                log_error("AK_MPI_OSD_UpdateRegion failed!!");
            }
            g_ChnInfo[nChn].ObjInfo[i].bUpdate = 0;
        }
    }

    if (hal_osd_vpss_chn_ready(nChn) && g_ChnInfo[nChn].s32BigCanvasId >= 0 &&
        AK_MPI_OSD_ShowCanvas(g_ChnInfo[nChn].s32BigCanvasId)) {
        log_error("AK_MPI_OSD_ShowCanvas failed!!");
    }
    return 0;
}

/*===========================================================================
 * HAL API Implementation
 *===========================================================================*/

/**
 * @brief Initialize OSD HAL.
 */
int hal_osd_init(osd_attr_t *attr, const char *font_file)
{
    strncpy(pcFontFile, font_file, sizeof(pcFontFile) - 2);
    if (attr != NULL) {
        memcpy(&g_attr, attr, sizeof(osd_attr_t));
        if (attr->color_mode == OSD_COLOR_MODE_RGB1 ||
            attr->color_mode == OSD_COLOR_MODE_RGB2 ||
            attr->color_mode == OSD_COLOR_MODE_RGB4) {
            s32BitWidth = (AK_S32)attr->color_mode;
        }
    }

    for (int i = 0; i < MAX_CHN_CNT; i++) {
        g_ChnInfo[i].s32BigCanvasId = -1;
        for (int j = 0; j < HAL_OSD_OBJ_COUNT; j++) {
            memset(&g_ChnInfo[i].ObjInfo[j], 0, sizeof(g_ChnInfo[i].ObjInfo[j]));
            g_ChnInfo[i].ObjInfo[j].s32CanvasId = -1;
            g_ChnInfo[i].ObjInfo[j].s32RegionId = -1;
        }
    }

    hal_osd_load_camera_rotation();
    log_info("hal_osd_init font=%s bpp=%d rotation=%d mpi_osd=%d",
             pcFontFile, s32BitWidth, s_camera_rotation_deg, hal_osd_mpi_osd_needed());

    if (hal_osd_mpi_osd_init_once() != 0) {
        log_error("hal_osd_mpi_osd_init_once failed");
    }

    return 0;
}

/**
 * @brief Deinitialize OSD HAL.
 *
 * celestia_old 侧：hal_osd_deinit() 在 hal_osd.c 中为空；同平台 SDK 上成对的
 * 全局释放见 hardware/modules/ai/src/AnyCloud39AV200/ak_hal_video.c 中
 * ai_eng_osd_deinit() -> AK_MPI_OSD_Destroy()（与 AK_MPI_OSD_Init 配对）。
 * 分辨率切换时 fw_osd.c 的 res_is_changed() 会按通道调用 hal_osd_clean_obj(ch,-1)，
 * 这里在进程内复用 OSD 时先做全通道 hal_osd_clean_obj(-1,-1)，再 Destroy，避免
 * 二次进入出现 OSD already init / BindVi already bind。
 */
int hal_osd_deinit(void)
{
    hal_osd_clean_obj(-1, -1);
    hal_osd_ipu_deinit();

    for (int i = 0; i < MAX_CHN_CNT; i++) {
        g_ChnInfo[i].s32BigCanvasId = -1;
        for (int j = 0; j < HAL_OSD_OBJ_COUNT; j++) {
            memset(&g_ChnInfo[i].ObjInfo[j], 0, sizeof(g_ChnInfo[i].ObjInfo[j]));
            g_ChnInfo[i].ObjInfo[j].s32CanvasId = -1;
            g_ChnInfo[i].ObjInfo[j].s32RegionId = -1;
        }
    }

    /* 与 celestia_old ak_hal_video.c ai_eng_osd_deinit 一致，不依赖返回值类型 */
    hal_osd_mpi_osd_destroy_once();

    return 0;
}

/**
 * @brief Clean OSD objects.
 */
int hal_osd_clean_obj(int nChn, int nObj)
{
    if (nChn < 0) {
        int ch;
        for (ch = 0; ch < MAX_CHN_CNT; ch++) {
            hal_osd_ipu_clear_channel(ch);
        }
    } else if (hal_osd_ipu_mode(nChn)) {
        hal_osd_ipu_clear_channel(nChn);
    }

    if (!hal_osd_mpi_osd_needed()) {
        return 0;
    }

    if (g_bCutMode) {
        hal_osd_clean_obj_cut(nChn, nObj, 0);
    } else {
        hal_osd_clean_obj_no_cut(nChn, nObj, 0);
    }
    return 0;
}

/**
 * @brief Draw bitmap to memory canvas.
 */
int hal_osd_draw_bmp(int nChn, int nBmpId, osd_rect_info *rc,
                     const unsigned char *pData, int iDataSize)
{
    log_debug("[%s][%d] nChn=%d,nBmpId=%d,rc=(%d,%d,%d,%d), pData=%p,iDataSize=%d",
           __FUNCTION__, __LINE__, nChn, nBmpId, rc->x, rc->y,
           rc->width, rc->height, pData, iDataSize);

    if (nBmpId < 0 || nBmpId >= HAL_OSD_OBJ_COUNT) {
        log_error("[%s][%d] Invalid nBmpId=%d, nChn=%d",
               __FUNCTION__, __LINE__, nBmpId, nChn);
        return -1;
    }

    nBmpId = HAL_OSD_OBJ_COUNT - 1 - nBmpId;

    if (hal_osd_ipu_mode(nChn)) {
        return hal_osd_ipu_draw_bmp(nChn, nBmpId, rc, pData, iDataSize);
    }

    if (g_ChnInfo[nChn].ObjInfo[nBmpId].bEnable != 0) {
        log_error("obj is override !!");
    }

    g_ChnInfo[nChn].ObjInfo[nBmpId].bEnable = 1;
    g_ChnInfo[nChn].ObjInfo[nBmpId].nObjType = OSD_REG_BITMAP;
    memcpy(&g_ChnInfo[nChn].ObjInfo[nBmpId].rc, rc, sizeof(osd_rect_info));
    g_ChnInfo[nChn].ObjInfo[nBmpId].info.Img.pData = (unsigned char *)pData;
    g_ChnInfo[nChn].ObjInfo[nBmpId].info.Img.iDataSize = iDataSize;

    return 0;
}

/**
 * @brief Draw text to memory canvas.
 */
int hal_osd_draw_text(int nChn, int nTextId, int x, int y,
                      const char *cText, int iTextLen)
{
    if (nTextId < 0 || nTextId >= HAL_OSD_OBJ_COUNT) {
        log_error("[%s][%d] Invalid nTextId=%d, nChn=%d,xy=(%d,%d), pData=%s,iDataSize=%d",
               __FUNCTION__, __LINE__, nTextId, nChn, x, y, cText, iTextLen);
        return -1;
    }

    VPSS_CHN_ATTR_S stVpssChnAttr = {0};
    if (AK_MPI_VPSS_GetChnAttr(g_s32GrpIndex, nChn, &stVpssChnAttr) != AK_SUCCESS) {
        log_warn("skip draw_text: get vpss chn attr failed ch=%d", nChn);
        return -1;
    }
    if (stVpssChnAttr.u32Width == 0 || stVpssChnAttr.u32Height == 0) {
        log_warn("skip draw_text: invalid vpss size ch=%d w=%u h=%u",
                 nChn, stVpssChnAttr.u32Width, stVpssChnAttr.u32Height);
        return -1;
    }

    if (hal_osd_ipu_mode(nChn)) {
        return hal_osd_ipu_draw_text(nChn, nTextId, x, y, cText, iTextLen);
    }

    return hal_osd_setup_font_obj(nChn, nTextId, x, y, cText, iTextLen, &stVpssChnAttr);
}

/**
 * @brief Copy canvas to VI.
 */
int hal_osd_copy_cavas_to_vi(int nChn)
{
    if (!hal_osd_mpi_osd_needed() || hal_osd_ipu_mode(nChn)) {
        return 0;
    }

    VPSS_CHN_ATTR_S stVpssChnAttr = {0};
    if (AK_MPI_VPSS_GetChnAttr(g_s32GrpIndex, nChn, &stVpssChnAttr) != AK_SUCCESS) {
        log_warn("skip copy_cavas_to_vi: get vpss chn attr failed ch=%d", nChn);
        return -1;
    }
    if (stVpssChnAttr.u32Width == 0 || stVpssChnAttr.u32Height == 0) {
        log_warn("skip copy_cavas_to_vi: invalid vpss size ch=%d w=%u h=%u",
                 nChn, stVpssChnAttr.u32Width, stVpssChnAttr.u32Height);
        return -1;
    }

    if (g_bCutMode) {
        g_ChnInfo[nChn].rcBig.x = 0;
        g_ChnInfo[nChn].rcBig.y = 0;
        g_ChnInfo[nChn].rcBig.width = stVpssChnAttr.u32Width;
        g_ChnInfo[nChn].rcBig.height = stVpssChnAttr.u32Height;
        if (hal_osd_prepare_cut(nChn) != 0) {
            return -1;
        }
        hal_osd_draw_cut(nChn);
    } else {
        hal_osd_prepare_no_cut(nChn);
        hal_osd_draw_no_cut(nChn);
    }
    return 0;
}

/**
 * @brief Get text width and height.
 */
int hal_osd_get_text_wh(int nChn, const char *cText, int cTextLen, int *pWidth, int *pHeight)
{
    if (cText == NULL || pWidth == NULL || pHeight == NULL) {
        return -1;
    }

    if (hal_osd_ipu_mode(nChn)) {
        return hal_osd_calc_ascii_text_wh(cText, cTextLen, g_ChnInfo[nChn].iCurFontSize,
                                          pWidth, pHeight);
    }

    unsigned short usText[HAL_OSD_TEXT_LEN];
    int usTextLen = char_to_short(usText, (unsigned char *)cText);
    hal_osd_get_ustext_wh(g_ChnInfo[nChn].iCurFontSize, s32FontInterval, usText, usTextLen,
                          pWidth, pHeight);
    return 0;
}

/**
 * @brief Set font for text rendering.
 */
int hal_osd_set_font(int nChn, osd_font_info *pFont)
{
    if (nChn < 0 || nChn >= MAX_CHN_CNT || pFont == NULL) {
        return -1;
    }
    g_ChnInfo[nChn].iCurFontSize = pFont->iFontSize;
    g_ChnInfo[nChn].iFontWidth = g_ChnInfo[nChn].iCurFontSize * 8 / 16;
    g_ChnInfo[nChn].iFontHeight = g_ChnInfo[nChn].iCurFontSize;
    return 0;
}

/**
 * @brief Create text bitmap.
 */
int hal_osd_new_text_bmp(int nChn, const char *cText, int iTextLen, osd_bitmap_info *pBmps)
{
    return 0;
}

/**
 * @brief Delete text bitmap.
 */
int hal_osd_delete_text_bmp(int nChn, osd_bitmap_info *bmp_info, int iTextLen)
{
    return 0;
}

/**
 * @brief Set channel attributes.
 */
int hal_osd_set_attr(int nChn, osd_chn_info *pChnInfo)
{
    if (nChn < 0 || nChn >= MAX_CHN_CNT || pChnInfo == NULL) {
        return -1;
    }

    s32BitWidth = pChnInfo->iBitPerPixel;
    return 0;
}

/**
 * @brief Set enable state.
 */
int hal_osd_set_enable(int nChn, int nObj, int bEnable)
{
    if (bEnable || nObj != -1) {
        return -1;
    }

    log_debug("[%s][%d] nChn=%d disable", __FUNCTION__, __LINE__, nChn);
    return hal_osd_clean_obj(nChn, -1);
}

/**
 * @brief Draw box overlay.
 */
int hal_osd_draw_box(int nChn, pp_box_group_t *pstBoxGroup)
{
    if (!pstBoxGroup) {
        log_error("pstBoxGroup is NULL");
        return -1;
    }

    if (hal_osd_ipu_mode(nChn)) {
        return hal_osd_ipu_set_draw_boxes(nChn, pstBoxGroup);
    }

    AK_S32 color[] = {0xff0000ff, 0xff00ff00, 0xffff0000, 0xff00ffff,
                      0xffff00ff, 0xffffff00, 0xffffffff};
    static int i = 0;
    if (i++ == 0) {
        AK_MPI_PP_SetBoxColorTable(g_s32GrpIndex, color);
    }

    /*
     * Convert neutral box layout to SDK PP_BOX_GROUP_S.
     * Do not cast directly: SDK PP_BOX_S field layout may differ across versions.
     */
    const hal_osd_ai_box_group_t *src = (const hal_osd_ai_box_group_t *)pstBoxGroup;
    PP_BOX_GROUP_S dst;
    memset(&dst, 0, sizeof(dst));

    int copy_cnt = BOX_MAX_NUM;
    if (copy_cnt > HAL_OSD_AI_BOX_MAX_NUM) {
        copy_cnt = HAL_OSD_AI_BOX_MAX_NUM;
    }

    for (int idx = 0; idx < copy_cnt; idx++) {
        const hal_osd_ai_box_t *s = &src->stBox[idx];
        PP_BOX_S *d = &dst.stBox[idx];
        if (s->u8Enable == 0) {
            continue;
        }

        d->u8Enable = (char)s->u8Enable;
        d->u16ColorId = s->u16ColorId;
        d->u16LineWidth = s->u16LineWidth;
        d->s32DrawFrameNum = s->s32DrawFrameNum;
        d->stRectangleArea.u16X = s->stRectangleArea.u16X;
        d->stRectangleArea.u16Y = s->stRectangleArea.u16Y;
        d->stRectangleArea.u16Width = s->stRectangleArea.u16Width;
        d->stRectangleArea.u16Height = s->stRectangleArea.u16Height;

        /*
         * Be tolerant to upstream enum mismatch.
         * Rectangle hollow is the expected default.
         */
        if (s->enType >= 0 && s->enType <= 3) {
            d->enType = s->enType;
        } else {
            d->enType = 0;
        }
    }

    int iRet = AK_MPI_PP_DrawBox(g_s32GrpIndex, nChn, &dst);
    if (iRet != 0) {
        log_error("AK_MPI_PP_DrawBox failed!!");
    }
    return iRet;
}

/**
 * @brief Adjust box coordinates based on video resolution.
 */
int hal_osd_adjust_box_coordinates(int nChn, pp_box_group_t *pstBoxGroup, int video_width, int video_height)
{
    if (!pstBoxGroup) {
        log_error("pstBoxGroup is NULL");
        return -1;
    }

    /* Adjust neutral-format coordinates in-place before draw_box conversion. */
    hal_osd_ai_box_group_t *pBoxGroup = (hal_osd_ai_box_group_t *)pstBoxGroup;
    int disp_w = video_width;
    int disp_h = video_height;
    static unsigned s_box_adj_log;

    for (int i = 0; i < HAL_OSD_AI_BOX_MAX_NUM; i++) {
        if (pBoxGroup->stBox[i].u8Enable == 0) {
            continue;
        }

        /* Normalized (1/10000) -> pixel coordinates */
        int dx = (int)pBoxGroup->stBox[i].stRectangleArea.u16X * disp_w / 10000;
        int dy = (int)pBoxGroup->stBox[i].stRectangleArea.u16Y * disp_h / 10000;
        int dw = (int)pBoxGroup->stBox[i].stRectangleArea.u16Width * disp_w / 10000;
        int dh = (int)pBoxGroup->stBox[i].stRectangleArea.u16Height * disp_h / 10000;
        int lw = ((int)pBoxGroup->stBox[i].u16LineWidth * disp_w + HAL_OSD_AI_BOX_LINE_NORM_DEN / 2)
                 / HAL_OSD_AI_BOX_LINE_NORM_DEN;

        if (dx < 0) {
            dx = 0;
        }
        if (dy < 0) {
            dy = 0;
        }
        if (dw < 1) {
            dw = 1;
        }
        if (dh < 1) {
            dh = 1;
        }
        if (dx + dw > disp_w) {
            dw = disp_w - dx;
        }
        if (dy + dh > disp_h) {
            dh = disp_h - dy;
        }
        if (dw < 1 || dh < 1) {
            pBoxGroup->stBox[i].u8Enable = 0;
            continue;
        }

        pBoxGroup->stBox[i].stRectangleArea.u16X = (AK_U16)dx;
        pBoxGroup->stBox[i].stRectangleArea.u16Y = (AK_U16)dy;
        pBoxGroup->stBox[i].stRectangleArea.u16Width = (AK_U16)dw;
        pBoxGroup->stBox[i].stRectangleArea.u16Height = (AK_U16)dh;
        if (lw < 1) {
            lw = 1;
        }
        /* Preview/sub streams (<=1024w): keep at least 3px like nova PP DrawBox. */
        if (lw < HAL_OSD_AI_BOX_LINE_MIN_PX && disp_w > 0 && disp_w <= HAL_OSD_AI_BOX_PREVIEW_W_MAX) {
            lw = HAL_OSD_AI_BOX_LINE_MIN_PX;
        }
        if (lw > HAL_OSD_AI_BOX_LINE_MAX_PX) {
            lw = HAL_OSD_AI_BOX_LINE_MAX_PX;
        }
        pBoxGroup->stBox[i].u16LineWidth = (AK_U16)lw;

        if (s_box_adj_log < 24u) {
            s_box_adj_log++;
            log_info("[CAM_TRACE] osd_box_px: ch=%d disp=%dx%d box[%d] px=(%d,%d,%d,%d) lw=%d ipu=%d",
                     nChn, disp_w, disp_h, i, dx, dy, dw, dh, lw,
                     hal_osd_ipu_mode(nChn) ? 1 : 0);
        }
    }
    
    return 0;
}

/**
 * @brief Initialize OSD with color type.
 * @param color_type  Color type (0 for color).
 * @return 0 on success, negative error code on failure.
 */
int hal_osd_init2(int color_type)
{
    (void)color_type;
    return hal_osd_mpi_osd_init_once();
}

/**
 * @brief Initialize draw box functionality.
 * @param grp_idx  Group index.
 * @return 0 on success, negative error code on failure.
 */
int hal_draw_box_init(int grp_idx)
{
    int ret = AK_SUCCESS;
    //红、绿、蓝、黄、紫、青、白
    AK_S32 color[] = {0xff0000ff, 0xff00ff00, 0xffff0000, 0xff00ffff,
                      0xffff00ff, 0xffffff00, 0xffffffff};
    if (AK_SUCCESS != AK_MPI_PP_SetBoxColorTable(grp_idx, color)) {
        log_error("Draw box init failed!!");
        ret = AK_FAILED;
    }
    g_s32GrpIndex = grp_idx;

    return ret;
}

/**
 * @brief Combined initialization for draw box and OSD.
 * @param grp_idx     Group index.
 * @param color_type  Color type.
 */
void hal_osd_draw_init(int grp_idx, int color_type)
{
    hal_draw_box_init(grp_idx);
    hal_osd_init2(color_type);
}
