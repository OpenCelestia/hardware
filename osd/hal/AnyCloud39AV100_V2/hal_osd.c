// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/osd.
 *
 * OSD HAL implementation for AnyCloud39AV100_V2 platform.
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
#include "ak_osd.h"
#include "ak_vi.h"
#include "log.h"

/*
 * Text rendering uses the V2.04 PDK extension API (ak_osd_ex_*) when the
 * header is shipped.  Guard the optional include so the OSD HAL keeps
 * compiling on older PDKs that do not provide it.
 */
#if defined(__has_include)
#  if __has_include("ak_osd_ex.h")
#    include "ak_osd_ex.h"
#    define HAL_OSD_HAS_AK_OSD_EX 1
#  else
#    define HAL_OSD_HAS_AK_OSD_EX 0
#  endif
#else
/* Best-effort: assume the header exists when __has_include is unavailable. */
#  include "ak_osd_ex.h"
#  define HAL_OSD_HAS_AK_OSD_EX 1
#endif

#define LOG_TAG "HAL_OSD"
#define MAX_CHN_CNT 3

#if 1
/*===========================================================================
 * AnyCloud39AV100_V2 implementation notes
 *===========================================================================*/
/*
 * AV100_V2 SDK exposes OSD APIs via `ak_osd.h` (ak_osd_*), not AK_MPI_OSD_*.
 * Likewise, VPSS channel attributes are not available via AK_MPI_VPSS_* headers
 * in this SDK. Keep this HAL implementation minimal and portable:
 * - Bitmap OSD: supported via ak_osd_create_canvas + ak_osd_draw_bitmap_to_canvas
 *               + ak_osd_set_region_to_vi.
 * - Text OSD:   uses ak_osd_ex_draw_str; input is UTF-8, converted to GBK
 *               code units for the PDK.  Requires a font file from
 *               hal_osd_init(..., font_file) or hal_osd_set_font.
 * - Box OSD:    not implemented in this HAL variant; AV100_V2 has no
 *               equivalent of AV200's AK_MPI_PP_DrawBox API.  The framework
 *               can still render rectangles by drawing them into bitmap
 *               regions using the bitmap path above.
 */

/* ARGB8888 color table. */
static AK_U32 u32RgbColorTables[] = {
    0x00000000, 0xffffffff, 0xff000000, 0xff0000ff,
    0xff00ff00, 0xffff0000, 0xffffff00, 0xffff00ff,
    0xff00ffff, 0xff786085, 0xff2c8aa0, 0xff68d535,
    0xff34aa5a, 0xff43e9ab, 0xff4b55a5, 0xff008080
};

typedef struct {
    int enabled;
    int created;
    int updated;
    osd_rect_info rc;
    const unsigned char *data;
    int data_size;
} osd_obj_state_t;

static struct {
    int inited;
    osd_obj_state_t obj[MAX_CHN_CNT][HAL_OSD_OBJ_COUNT];
} g_osd = {0};

#if HAL_OSD_HAS_AK_OSD_EX
static int g_osd_ex_ready;
static struct osd_attr g_osd_ex_attr;
static char g_osd_ex_font_path[256];
static int g_osd_ex_font_px;
static int g_ch_font_px[MAX_CHN_CNT];

typedef struct {
    int valid;
    osd_rect_info rc;
} osd_text_ex_state_t;

static osd_text_ex_state_t g_text_ex[MAX_CHN_CNT][HAL_OSD_OBJ_COUNT];

static int pack_gbk_to_ushort(const unsigned char *gb, size_t len,
                              unsigned short *out, int cap)
{
    size_t i = 0;
    int n = 0;

    while (i < len && n < cap) {
        if (gb[i] < 0x80u) {
            out[n++] = gb[i++];
        } else if (i + 1 < len) {
            out[n++] = ((unsigned short)gb[i] << 8) | gb[i + 1];
            i += 2;
        } else {
            break;
        }
    }
    return n;
}

/* UTF-8 -> GBK bytes via iconv, then pack to ushort codes for ak_osd_ex_*. */
static int osd_utf8_to_display_ushort(const char *utf8, int utf8_len,
                                      unsigned short *out, int out_cap)
{
    char ibuf[512];
    char obuf[512];

    if (!utf8 || utf8_len <= 0 || !out || out_cap <= 0)
        return -1;

    int all_ascii = 1;
    for (int i = 0; i < utf8_len; i++) {
        if ((unsigned char)utf8[i] > 127u) {
            all_ascii = 0;
            break;
        }
    }
    if (all_ascii) {
        int n = utf8_len < out_cap ? utf8_len : out_cap;
        for (int i = 0; i < n; i++)
            out[i] = (unsigned char)utf8[i];
        return n;
    }

    if (utf8_len >= (int)sizeof(ibuf))
        utf8_len = (int)sizeof(ibuf) - 1;
    memcpy(ibuf, utf8, (size_t)utf8_len);

    iconv_t cd = iconv_open("GBK", "UTF-8");
    if (cd == (iconv_t)-1) {
        int n = utf8_len < out_cap ? utf8_len : out_cap;
        for (int i = 0; i < n; i++)
            out[i] = (unsigned char)utf8[i];
        return n;
    }

    char *pin = ibuf;
    char *pout = obuf;
    size_t inleft = (size_t)utf8_len;
    size_t outleft = sizeof(obuf);

    if (iconv(cd, &pin, &inleft, &pout, &outleft) == (size_t)-1) {
        iconv_close(cd);
        int n = utf8_len < out_cap ? utf8_len : out_cap;
        for (int i = 0; i < n; i++)
            out[i] = (unsigned char)utf8[i];
        return n;
    }
    iconv_close(cd);

    size_t gb_len = sizeof(obuf) - outleft;
    return pack_gbk_to_ushort((unsigned char *)obuf, gb_len, out, out_cap);
}

static void osd_ex_teardown(void)
{
    if (!g_osd_ex_ready)
        return;
    ak_osd_ex_destroy();
    g_osd_ex_ready = 0;
}

static int osd_ex_apply_style(void)
{
    int fs0 = g_ch_font_px[0] > 0 ? g_ch_font_px[0] : g_osd_ex_font_px;

    if (fs0 <= 0)
        fs0 = 16;
    if (ak_osd_ex_set_color(1, 0) != 0)
        log_warn("ak_osd_ex_set_color failed");
    if (ak_osd_ex_set_edge_color(2) != 0)
        log_warn("ak_osd_ex_set_edge_color failed");
    (void)ak_osd_ex_set_alpha(100);
    for (int ch = 0; ch < MAX_CHN_CNT; ch++) {
        int fs = g_ch_font_px[ch] > 0 ? g_ch_font_px[ch] : g_osd_ex_font_px;
        if (fs <= 0)
            fs = 16;
        (void)ak_osd_ex_set_font_size(ch, fs);
    }
    return 0;
}

static int osd_ex_try_start(void)
{
    if (g_osd_ex_ready)
        return 0;
    if (g_osd_ex_font_path[0] == '\0')
        return -1;

    int fs = g_osd_ex_font_px > 0 ? g_osd_ex_font_px : 16;
    if (ak_osd_ex_set_font_file(fs, g_osd_ex_font_path) != 0) {
        log_error("ak_osd_ex_set_font_file failed path=%s", g_osd_ex_font_path);
        return -1;
    }
    if (ak_osd_ex_init2(&g_osd_ex_attr) != 0) {
        log_error("ak_osd_ex_init2 failed");
        return -1;
    }
    g_osd_ex_ready = 1;
    osd_ex_apply_style();
    log_info("ak_osd_ex ready font=%s px=%d", g_osd_ex_font_path, fs);
    return 0;
}
#endif /* HAL_OSD_HAS_AK_OSD_EX */

static int hal_osd_valid_chn(int nChn)
{
    return (nChn >= 0 && nChn < MAX_CHN_CNT);
}

static int hal_osd_valid_obj(int nObj)
{
    return (nObj >= 0 && nObj < HAL_OSD_OBJ_COUNT);
}

static int hal_osd_get_vi_wh(int chn, int *w, int *h)
{
    if (!w || !h) return -1;
    VI_CHN_ATTR attr;
    memset(&attr, 0, sizeof(attr));
    if (ak_vi_get_chn_attr((VI_CHN)chn, &attr) != AK_SUCCESS)
        return -1;
    *w = attr.res.width;
    *h = attr.res.height;
    return (*w > 0 && *h > 0) ? 0 : -1;
}

int hal_osd_preset_rotation(int rotation_deg)
{
    (void)rotation_deg;
    return 0;
}

int hal_osd_set_rotation(int nChn, int rotation_deg, int display_width, int display_height)
{
    /* AV100_V2 uses VI-path ak_osd_* only; no IPU/VPSS backend switch. */
    (void)nChn;
    (void)rotation_deg;
    (void)display_width;
    (void)display_height;
    return 0;
}

int hal_osd_init(osd_attr_t *attr, const char *font_file)
{
    struct osd_attr osd_attr;
    memset(&osd_attr, 0, sizeof(osd_attr));

    osd_attr.color_mode_index = AK_OSD_COLOR_FMT_RGB;
    osd_attr.color_table = (attr && attr->pu32ColorTable) ? attr->pu32ColorTable : (unsigned int *)u32RgbColorTables;
    osd_attr.region_num = HAL_OSD_OBJ_COUNT;
    osd_attr.color_type = AK_OSD_TYPE_COLOR;
    osd_attr.bit_width = 4;

#if HAL_OSD_HAS_AK_OSD_EX
    memcpy(&g_osd_ex_attr, &osd_attr, sizeof(osd_attr));
    g_osd_ex_font_path[0] = '\0';
    g_osd_ex_font_px = 16;
    memset(g_ch_font_px, 0, sizeof(g_ch_font_px));
    memset(g_text_ex, 0, sizeof(g_text_ex));
#endif

    if (ak_osd_init(&osd_attr) != 0) {
        log_error("ak_osd_init failed");
        return -1;
    }

    memset(&g_osd, 0, sizeof(g_osd));
    g_osd.inited = 1;

#if HAL_OSD_HAS_AK_OSD_EX
    if (font_file && font_file[0]) {
        strncpy(g_osd_ex_font_path, font_file, sizeof(g_osd_ex_font_path) - 1);
        g_osd_ex_font_path[sizeof(g_osd_ex_font_path) - 1] = '\0';
        g_osd_ex_font_px = 16;
        if (osd_ex_try_start() != 0)
            log_warn("ak_osd_ex deferred: set font via hal_osd_set_font");
    }
#endif
    return 0;
}

int hal_osd_deinit(void)
{
    if (!g_osd.inited)
        return 0;

    /* best-effort destroy all canvases */
    for (int ch = 0; ch < MAX_CHN_CNT; ch++) {
        for (int obj = 0; obj < HAL_OSD_OBJ_COUNT; obj++) {
            if (g_osd.obj[ch][obj].created) {
                ak_osd_destroy_canvas(ch, obj);
            }
        }
    }

#if HAL_OSD_HAS_AK_OSD_EX
    osd_ex_teardown();
    memset(g_text_ex, 0, sizeof(g_text_ex));
#endif

    ak_osd_uninit();
    memset(&g_osd, 0, sizeof(g_osd));
    return 0;
}

int hal_osd_clean_obj(int nChn, int nObj)
{
    if (nChn != -1 && !hal_osd_valid_chn(nChn))
        return -1;
    if (nObj != -1 && !hal_osd_valid_obj(nObj))
        return -1;

    int ch_start = (nChn == -1) ? 0 : nChn;
    int ch_end = (nChn == -1) ? (MAX_CHN_CNT - 1) : nChn;
    int obj_start = (nObj == -1) ? 0 : nObj;
    int obj_end = (nObj == -1) ? (HAL_OSD_OBJ_COUNT - 1) : nObj;

    for (int ch = ch_start; ch <= ch_end; ch++) {
        for (int obj = obj_start; obj <= obj_end; obj++) {
#if HAL_OSD_HAS_AK_OSD_EX
            if (g_text_ex[ch][obj].valid && g_osd_ex_ready) {
                osd_rect_info *r = &g_text_ex[ch][obj].rc;
                (void)ak_osd_ex_clean_str(ch, obj, 0, 0, r->width, r->height);
                g_text_ex[ch][obj].valid = 0;
            }
#endif
            if (g_osd.obj[ch][obj].created) {
                ak_osd_destroy_canvas(ch, obj);
            }
            memset(&g_osd.obj[ch][obj], 0, sizeof(g_osd.obj[ch][obj]));
        }
    }
    return 0;
}

int hal_osd_draw_bmp(int nChn, int nBmpId, osd_rect_info *rc,
                     const unsigned char *pData, int iDataSize)
{
    if (!g_osd.inited) return -1;
    if (!hal_osd_valid_chn(nChn) || !hal_osd_valid_obj(nBmpId) || !rc || !pData || iDataSize <= 0)
        return -1;
    if (rc->width <= 0 || rc->height <= 0)
        return -1;

    osd_obj_state_t *o = &g_osd.obj[nChn][nBmpId];
    o->enabled = 1;
    o->updated = 1;
    o->rc = *rc;
    o->data = pData;
    o->data_size = iDataSize;
    return 0;
}

int hal_osd_copy_cavas_to_vi(int nChn)
{
    if (!g_osd.inited) return -1;
    if (!hal_osd_valid_chn(nChn)) return -1;

    int vi_w = 0, vi_h = 0;
    if (hal_osd_get_vi_wh(nChn, &vi_w, &vi_h) != 0) {
        log_warn("skip copy: invalid vi attr ch=%d", nChn);
        return -1;
    }

    int rc_total = 0;     /* total error count across regions */
    int rc_loop = 0;      /* most recent failure code */

    for (int obj = 0; obj < HAL_OSD_OBJ_COUNT; obj++) {
        osd_obj_state_t *o = &g_osd.obj[nChn][obj];
        if (!o->enabled || !o->updated)
            continue;

        /* clamp to VI bounds */
        osd_rect_info rc = o->rc;
        if (rc.x < 0) rc.x = 0;
        if (rc.y < 0) rc.y = 0;
        if (rc.x + rc.width > vi_w) rc.width = vi_w - rc.x;
        if (rc.y + rc.height > vi_h) rc.height = vi_h - rc.y;
        if (rc.width <= 0 || rc.height <= 0) {
            o->updated = 0;
            continue;
        }

        if (!o->created) {
            struct osd_canvas_info ci;
            memset(&ci, 0, sizeof(ci));
            ci.channel_id = nChn;
            ci.region_id = obj;
            ci.pos_in_yuv.x = rc.x;
            ci.pos_in_yuv.y = rc.y;
            ci.region_size_width = rc.width;
            ci.region_size_height = rc.height;
            ci.canvas_color_index = 0;
            ci.alpha = 100;
            int rc_create = ak_osd_create_canvas(&ci);
            if (rc_create != 0) {
                log_error("ak_osd_create_canvas failed ch=%d obj=%d ret=%d", nChn, obj, rc_create);
                rc_loop = rc_create;
                rc_total++;
                /* leave updated=1 so the next call retries; create ok will arm us */
                continue;
            }
            o->created = 1;
        } else {
            ak_osd_set_canvas_position(nChn, obj, rc.x, rc.y);
            /* SDK signature: ak_osd_set_canvas_size(chn, obj, height, width). */
            ak_osd_set_canvas_size(nChn, obj, rc.height, rc.width);
            ak_osd_clean_canvas(nChn, obj);
        }

        struct osd_drawing_info di;
        memset(&di, 0, sizeof(di));
        di.channel_id = nChn;
        di.region_id = obj;
        di.buffer = o->data;
        di.buffer_size = o->data_size;
        di.bitmap_size_width = rc.width;
        di.bitmap_size_height = rc.height;
        di.pos_in_region.x = 0;
        di.pos_in_region.y = 0;
        int rc_draw = ak_osd_draw_bitmap_to_canvas(&di);
        if (rc_draw != 0) {
            log_error("ak_osd_draw_bitmap_to_canvas failed ch=%d obj=%d ret=%d", nChn, obj, rc_draw);
            rc_loop = rc_draw;
            rc_total++;
            /* keep updated=1 so the bitmap will be redrawn on next pass instead
             * of being silently dropped.
             */
            continue;
        }

        int rc_set = ak_osd_set_region_to_vi(nChn, obj);
        if (rc_set != 0) {
            log_warn("ak_osd_set_region_to_vi failed ch=%d obj=%d ret=%d", nChn, obj, rc_set);
            rc_loop = rc_set;
            rc_total++;
            continue;
        }

        o->updated = 0;
    }

    int rc_commit = ak_osd_set_to_vi();
    if (rc_commit != 0) {
        log_warn("ak_osd_set_to_vi failed ch=%d ret=%d", nChn, rc_commit);
        return rc_commit;
    }

    return rc_total > 0 ? rc_loop : 0;
}

int hal_osd_set_font(int nChn, osd_font_info *pFont)
{
#if HAL_OSD_HAS_AK_OSD_EX
    if (!hal_osd_valid_chn(nChn) || !pFont)
        return -1;
    if (pFont->cFontName[0] != '\0') {
        strncpy(g_osd_ex_font_path, pFont->cFontName, sizeof(g_osd_ex_font_path) - 1);
        g_osd_ex_font_path[sizeof(g_osd_ex_font_path) - 1] = '\0';
    }
    g_ch_font_px[nChn] = pFont->iFontSize > 0 ? pFont->iFontSize : 16;
    g_osd_ex_font_px = g_ch_font_px[nChn];

    if (!g_osd.inited)
        return 0;

    if (g_osd_ex_font_path[0] == '\0') {
        log_warn("hal_osd_set_font: empty font path");
        return -1;
    }

    if (g_osd_ex_ready) {
        if (ak_osd_ex_set_font_file(g_osd_ex_font_px, g_osd_ex_font_path) != 0)
            log_warn("ak_osd_ex_set_font_file failed");
        (void)ak_osd_ex_set_font_size(nChn, g_ch_font_px[nChn]);
        {
            int fc = pFont->iColor & 0xF;
            int bc = pFont->iBKClor & 0xF;
            (void)ak_osd_ex_set_color(fc, bc);
        }
        return 0;
    }

    if (osd_ex_try_start() != 0)
        return -1;
    {
        int fc = pFont->iColor & 0xF;
        int bc = pFont->iBKClor & 0xF;
        (void)ak_osd_ex_set_color(fc, bc);
    }
    (void)ak_osd_ex_set_font_size(nChn, g_ch_font_px[nChn]);
    return 0;
#else
    (void)nChn;
    (void)pFont;
    return 0;
#endif
}

int hal_osd_get_text_wh(int nChn, const char *cText, int cTextLen, int *pWidth, int *pHeight)
{
    if (!cText || cTextLen <= 0 || !pWidth || !pHeight)
        return -1;
#if HAL_OSD_HAS_AK_OSD_EX
    if (g_osd.inited && g_osd_ex_ready && hal_osd_valid_chn(nChn)) {
        unsigned short buf[HAL_OSD_TEXT_LEN];
        int n = osd_utf8_to_display_ushort(cText, cTextLen, buf, HAL_OSD_TEXT_LEN);
        if (n > 0) {
            struct osd_bitmap_info bi;
            memset(&bi, 0, sizeof(bi));
            if (ak_osd_ex_get_str_bmp(nChn, buf, n, &bi) == 0) {
                *pWidth = bi.osd_bmp_attr.bitmap_width;
                *pHeight = bi.osd_bmp_attr.bitmap_height;
                (void)ak_osd_ex_release_bmp_buf(&bi);
                if (*pWidth > 0 && *pHeight > 0)
                    return 0;
            }
        }
    }
#endif
    *pHeight = 16;
    *pWidth = cTextLen * 8;
    return 0;
}

int hal_osd_draw_text(int nChn, int nTextId, int x, int y, const char *cText, int iTextLen)
{
#if HAL_OSD_HAS_AK_OSD_EX
    if (!g_osd.inited)
        return -1;
    if (!hal_osd_valid_chn(nChn) || !hal_osd_valid_obj(nTextId) || !cText || iTextLen <= 0)
        return -1;

    if (!g_osd_ex_ready && g_osd_ex_font_path[0] && osd_ex_try_start() != 0) {
        log_warn("hal_osd_draw_text: ak_osd_ex not ready (set font path first)");
        return -1;
    }
    if (!g_osd_ex_ready) {
        log_warn("hal_osd_draw_text: no font configured");
        return -1;
    }

    unsigned short buf[HAL_OSD_TEXT_LEN];
    int nuch = osd_utf8_to_display_ushort(cText, iTextLen, buf, HAL_OSD_TEXT_LEN);
    if (nuch <= 0)
        return -1;

    int vi_w = 0, vi_h = 0;
    if (hal_osd_get_vi_wh(nChn, &vi_w, &vi_h) != 0) {
        log_warn("draw_text: invalid vi ch=%d", nChn);
        return -1;
    }

    int tw = 0, th = 0;
    struct osd_bitmap_info bi;
    memset(&bi, 0, sizeof(bi));
    if (ak_osd_ex_get_str_bmp(nChn, buf, nuch, &bi) == 0) {
        tw = bi.osd_bmp_attr.bitmap_width + 4;
        th = bi.osd_bmp_attr.bitmap_height + 4;
        (void)ak_osd_ex_release_bmp_buf(&bi);
    }
    if (tw <= 0 || th <= 0) {
        int fs = g_ch_font_px[nChn] > 0 ? g_ch_font_px[nChn] : g_osd_ex_font_px;
        if (fs <= 0)
            fs = 16;
        tw = nuch * (fs + 2) + 4;
        th = fs * 2 + 4;
    }

    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x + tw > vi_w)
        tw = vi_w - x;
    if (y + th > vi_h)
        th = vi_h - y;
    if (tw <= 0 || th <= 0)
        return -1;

    (void)ak_osd_ex_set_font_interval(nChn, nTextId, 0);

    if (ak_osd_ex_set_rect(nChn, nTextId, x, y, tw, th) != 0) {
        log_error("ak_osd_ex_set_rect fail ch=%d obj=%d", nChn, nTextId);
        return -1;
    }
    if (ak_osd_ex_draw_str(nChn, nTextId, 0, 0, buf, nuch) != 0) {
        log_error("ak_osd_ex_draw_str fail ch=%d obj=%d", nChn, nTextId);
        return -1;
    }

    g_text_ex[nChn][nTextId].valid = 1;
    g_text_ex[nChn][nTextId].rc.x = x;
    g_text_ex[nChn][nTextId].rc.y = y;
    g_text_ex[nChn][nTextId].rc.width = tw;
    g_text_ex[nChn][nTextId].rc.height = th;
    return 0;
#else
    (void)nChn;
    (void)nTextId;
    (void)x;
    (void)y;
    (void)cText;
    (void)iTextLen;
    log_warn("hal_osd_draw_text not supported (no ak_osd_ex.h)");
    return -1;
#endif
}

int hal_osd_set_attr(int nChn, osd_chn_info *pChnInfo)
{
    (void)nChn;
    (void)pChnInfo;
    return 0;
}

int hal_osd_set_enable(int nChn, int nObj, int bEnable)
{
    if (!g_osd.inited) return -1;
    if (!hal_osd_valid_chn(nChn)) return -1;

    if (nObj == -1) {
        for (int i = 0; i < HAL_OSD_OBJ_COUNT; i++) {
            g_osd.obj[nChn][i].enabled = bEnable ? 1 : 0;
            if (!bEnable) {
                hal_osd_clean_obj(nChn, i);
            }
        }
        return 0;
    }

    if (!hal_osd_valid_obj(nObj)) return -1;
    g_osd.obj[nChn][nObj].enabled = bEnable ? 1 : 0;
    if (!bEnable) {
        hal_osd_clean_obj(nChn, nObj);
    }
    return 0;
}

int hal_osd_draw_box(int nChn, pp_box_group_t *pstBoxGroup)
{
    (void)nChn;
    (void)pstBoxGroup;
    return -1;
}

int hal_osd_adjust_box_coordinates(int nChn, pp_box_group_t *pstBoxGroup, int video_width, int video_height)
{
    /* Box overlay not available on AV100_V2 ak_osd path; keep API compatible. */
    (void)nChn;
    (void)pstBoxGroup;
    (void)video_width;
    (void)video_height;
    return 0;
}

int hal_osd_init2(int color_type)
{
#if HAL_OSD_HAS_AK_OSD_EX
    g_osd_ex_attr.color_type = color_type ? AK_OSD_TYPE_MONO : AK_OSD_TYPE_COLOR;
#else
    (void)color_type;
#endif
    return 0;
}
int hal_draw_box_init(int grp_idx) { (void)grp_idx; return 0; }
void hal_osd_draw_init(int grp_idx, int color_type) { (void)grp_idx; (void)color_type; }

#else
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

/* ARGB8888 color table. */
static AK_U32 u32RgbColorTables[] = {
    0x00000000, 0xffffffff, 0xff000000, 0xff0000ff,
    0xff00ff00, 0xffff0000, 0xffffff00, 0xffff00ff,
    0xff00ffff, 0xff786085, 0xff2c8aa0, 0xff68d535,
    0xff34aa5a, 0xff43e9ab, 0xff4b55a5, 0xff008080
};

static int hal_osd_valid_chn(int nChn)
{
    return (nChn >= 0 && nChn < MAX_CHN_CNT);
}

static int hal_osd_valid_obj(int nObj)
{
    return (nObj >= 0 && nObj < HAL_OSD_OBJ_COUNT);
}

/*===========================================================================
 * Private Helper Functions
 *===========================================================================*/

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
 * @brief Preset camera rotation (no-op on AV100; VPSS OSD always used).
 */
int hal_osd_preset_rotation(int rotation_deg)
{
    (void)rotation_deg;
    return 0;
}

/**
 * @brief Select OSD backend from camera rotation (no-op on AV100 legacy MPI path).
 */
int hal_osd_set_rotation(int nChn, int rotation_deg, int display_width, int display_height)
{
    (void)nChn;
    (void)rotation_deg;
    (void)display_width;
    (void)display_height;
    return 0;
}

/**
 * @brief Initialize OSD HAL.
 */
int hal_osd_init(osd_attr_t *attr, const char *font_file)
{
    if (font_file != NULL) {
        strncpy(pcFontFile, font_file, sizeof(pcFontFile) - 2);
    } else {
        pcFontFile[0] = '\0';
    }
    if (attr != NULL) {
        memcpy(&g_attr, attr, sizeof(osd_attr_t));
    }

    for (int i = 0; i < MAX_CHN_CNT; i++) {
        g_ChnInfo[i].s32BigCanvasId = -1;
        for (int j = 0; j < HAL_OSD_OBJ_COUNT; j++) {
            memset(&g_ChnInfo[i].ObjInfo[j], 0, sizeof(g_ChnInfo[i].ObjInfo[j]));
            g_ChnInfo[i].ObjInfo[j].s32CanvasId = -1;
            g_ChnInfo[i].ObjInfo[j].s32RegionId = -1;
        }
    }

    log_debug("hal_osd_init font=%s", pcFontFile);

    OSD_ATTR_S stOsdAttrValue = {0};
    stOsdAttrValue.enColorModeIndex = AK_OSD_COLOR_FMT_RGB;
    stOsdAttrValue.enColorType = s32GColorType;
    stOsdAttrValue.pu32ColorTable = u32RgbColorTables;

    if (AK_SUCCESS != AK_MPI_OSD_Init(&stOsdAttrValue)) {
        log_error("AK_MPI_OSD_Init failed!!");
    }

    return 0;
}

/**
 * @brief Deinitialize OSD HAL.
 *
 * celestia_old 侧：hal_osd_deinit() 在 hal_osd.c 中为空；同平台 SDK 上成对的
 * 全局释放需与 MPI/OSD Init 对称调用 Destroy（具体路径以 PDK 与 AI 模块布局为准）。
 * ai_eng_osd_deinit() -> AK_MPI_OSD_Destroy()（与 AK_MPI_OSD_Init 配对）。
 * 分辨率切换时 fw_osd.c 的 res_is_changed() 会按通道调用 hal_osd_clean_obj(ch,-1)，
 * 这里在进程内复用 OSD 时先做全通道 hal_osd_clean_obj(-1,-1)，再 Destroy，避免
 * 二次进入出现 OSD already init / BindVi already bind。
 */
int hal_osd_deinit(void)
{
    hal_osd_clean_obj(-1, -1);

    for (int i = 0; i < MAX_CHN_CNT; i++) {
        g_ChnInfo[i].s32BigCanvasId = -1;
        for (int j = 0; j < HAL_OSD_OBJ_COUNT; j++) {
            memset(&g_ChnInfo[i].ObjInfo[j], 0, sizeof(g_ChnInfo[i].ObjInfo[j]));
            g_ChnInfo[i].ObjInfo[j].s32CanvasId = -1;
            g_ChnInfo[i].ObjInfo[j].s32RegionId = -1;
        }
    }

    /* 与 celestia_old ak_hal_video.c ai_eng_osd_deinit 一致，不依赖返回值类型 */
    AK_MPI_OSD_Destroy();

    return 0;
}

/**
 * @brief Clean OSD objects.
 */
int hal_osd_clean_obj(int nChn, int nObj)
{
    if (nChn != -1 && !hal_osd_valid_chn(nChn)) {
        return -1;
    }
    if (nObj != -1 && !hal_osd_valid_obj(nObj)) {
        return -1;
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
    if (!hal_osd_valid_chn(nChn) || !hal_osd_valid_obj(nBmpId) || rc == NULL ||
        pData == NULL || iDataSize <= 0) {
        return -1;
    }
    if (rc->width <= 0 || rc->height <= 0) {
        return -1;
    }

    log_debug("[%s][%d] nChn=%d,nBmpId=%d,rc=(%d,%d,%d,%d), pData=%p,iDataSize=%d",
           __FUNCTION__, __LINE__, nChn, nBmpId, rc->x, rc->y,
           rc->width, rc->height, pData, iDataSize);

    log_debug("[%s][%d] pData(%d,%d,%d,%d,%d)(%d,%d,%d,%d,%d)iDataSize=%d",
           __FUNCTION__, __LINE__, pData[0], pData[1], pData[2], pData[3], pData[4],
           pData[iDataSize - 5], pData[iDataSize - 4], pData[iDataSize - 3],
           pData[iDataSize - 2], pData[iDataSize - 1], iDataSize);

    nBmpId = HAL_OSD_OBJ_COUNT - 1 - nBmpId;
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
    if (!hal_osd_valid_chn(nChn) || !hal_osd_valid_obj(nTextId) || cText == NULL || iTextLen <= 0) {
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

    g_ChnInfo[nChn].ObjInfo[nTextId].bEnable = 1;
    g_ChnInfo[nChn].ObjInfo[nTextId].bUpdate = 1;
    g_ChnInfo[nChn].ObjInfo[nTextId].nObjType = OSD_REG_FONT;

    OSD_FONT_ATTR_S stFontAttr = {0};
    int usTextLen = char_to_short(g_ChnInfo[nChn].ObjInfo[nTextId].info.Text.u16UsOsdString,
                                  (unsigned char *)cText);
    stFontAttr.s32FontFileSize = 16;
    stFontAttr.s32FontSize = g_ChnInfo[nChn].iCurFontSize;
    stFontAttr.pcfile = pcFontFile;
    stFontAttr.s32FontInterval = s32FontInterval;

    AK_S32 s32Width;
    AK_S32 s32Height;
    AK_S32 s32MaxW = stVpssChnAttr.u32Width;
    AK_S32 s32MaxH = stVpssChnAttr.u32Height;

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
 * @brief Copy canvas to VI.
 */
int hal_osd_copy_cavas_to_vi(int nChn)
{
    if (!hal_osd_valid_chn(nChn)) {
        return -1;
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
    if (!hal_osd_valid_chn(nChn) || cText == NULL || pWidth == NULL || pHeight == NULL) {
        return -1;
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
    if (!hal_osd_valid_chn(nChn)) {
        return -1;
    }

    if (nObj == -1) {
        if (bEnable) {
            for (int i = 0; i < HAL_OSD_OBJ_COUNT; i++) {
                if (g_ChnInfo[nChn].ObjInfo[i].nObjType == OSD_REG_BITMAP ||
                    g_ChnInfo[nChn].ObjInfo[i].nObjType == OSD_REG_FONT) {
                    g_ChnInfo[nChn].ObjInfo[i].bEnable = 1;
                    g_ChnInfo[nChn].ObjInfo[i].bUpdate = 1;
                }
            }
            return 0;
        }
        log_debug("[%s][%d] nChn=%d disable all", __FUNCTION__, __LINE__, nChn);
        return hal_osd_clean_obj(nChn, -1);
    }

    if (!hal_osd_valid_obj(nObj)) {
        return -1;
    }

    if (!bEnable) {
        log_debug("[%s][%d] nChn=%d disable obj=%d", __FUNCTION__, __LINE__, nChn, nObj);
        return hal_osd_clean_obj(nChn, nObj);
    }

    if (g_ChnInfo[nChn].ObjInfo[nObj].nObjType != OSD_REG_BITMAP &&
        g_ChnInfo[nChn].ObjInfo[nObj].nObjType != OSD_REG_FONT) {
        return -1;
    }
    g_ChnInfo[nChn].ObjInfo[nObj].bEnable = 1;
    g_ChnInfo[nChn].ObjInfo[nObj].bUpdate = 1;
    return 0;
}

/**
 * @brief Draw box overlay.
 */
int hal_osd_draw_box(int nChn, pp_box_group_t *pstBoxGroup)
{
    if (!hal_osd_valid_chn(nChn)) {
        return -1;
    }

    if (!pstBoxGroup) {
        log_error("pstBoxGroup is NULL");
        return -1;
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
    (void)nChn;

    if (!pstBoxGroup) {
        log_error("pstBoxGroup is NULL");
        return -1;
    }

    /* Adjust neutral-format coordinates in-place before draw_box conversion. */
    hal_osd_ai_box_group_t *pBoxGroup = (hal_osd_ai_box_group_t *)pstBoxGroup;

    for (int i = 0; i < HAL_OSD_AI_BOX_MAX_NUM; i++) {
        if (pBoxGroup->stBox[i].u8Enable == 0) {
            continue;
        }

        /* Adjust coordinates from normalized (1/10000) to pixel coordinates */
        pBoxGroup->stBox[i].stRectangleArea.u16X = pBoxGroup->stBox[i].stRectangleArea.u16X * video_width / 10000;
        pBoxGroup->stBox[i].stRectangleArea.u16Y = pBoxGroup->stBox[i].stRectangleArea.u16Y * video_height / 10000;
        pBoxGroup->stBox[i].stRectangleArea.u16Width = pBoxGroup->stBox[i].stRectangleArea.u16Width * video_width / 10000;
        pBoxGroup->stBox[i].stRectangleArea.u16Height = pBoxGroup->stBox[i].stRectangleArea.u16Height * video_height / 10000;
        pBoxGroup->stBox[i].u16LineWidth = pBoxGroup->stBox[i].u16LineWidth * video_width / 10000;
        if (pBoxGroup->stBox[i].u16LineWidth <= 0) {
            pBoxGroup->stBox[i].u16LineWidth = 1;
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
    int ret = AK_SUCCESS;
    OSD_ATTR_S stOsdAttrValue = { 0 };
    stOsdAttrValue.enColorModeIndex = AK_OSD_COLOR_FMT_RGB;
    stOsdAttrValue.enColorType = color_type;  // 0 彩色
    stOsdAttrValue.pu32ColorTable = u32RgbColorTables; // 颜色表

    if (AK_SUCCESS != AK_MPI_OSD_Init(&stOsdAttrValue)) {
        log_error("OSD init failed!!");
        ret = AK_FAILED;
    }

    return ret;
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
#endif
