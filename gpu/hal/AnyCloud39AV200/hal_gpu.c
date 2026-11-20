// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/gpu/hal/AnyCloud39AV200.
 *
 * GPU HAL implementation for AnyCloud39AV200 platform.
 * - IPU-based GPU implementation using AK_MPI_IPU APIs.
 * - Provides image copy, scale, transform, blend and fill operations.
 */

#include "gpu.h"
#include "hal_gpu.h"

#include <linux/kernel.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <stdbool.h>
#include <pthread.h>

#include "ak_thread.h"
#include "ak_log.h"
#include "ak_common.h"
#include "ak_mem.h"
#include "ak_ipu.h"

#include "system_pixel_format.h"
#include "hal_ipu_api.h"
#include "log.h"

#define LOG_TAG "HAL_GPU"

/*===========================================================================
 * Private Variables
 *===========================================================================*/

static int s_init_done = 0;
static int s_gpu_ipu_queue_id = -1;

/* Helper function to get GPU queue ID */
static inline int get_gpu_queue_id(void)
{
    return (s_gpu_ipu_queue_id >= 0) ? s_gpu_ipu_queue_id : IPU_QUEUE_INDEX;
}

static int gpu_wait_done(const char *op, int queue_id, int line)
{
    int ret = hal_ipu_wait_done(queue_id, 1000);
    if (AK_SUCCESS != ret) {
        log_error("fun=%s line=%d, hal_ipu_wait_done failed queue_id=%d ret=%d",
                  op, line, queue_id, ret);
    }
    return ret;
}

/*===========================================================================
 * Private Functions - Format Conversion
 *===========================================================================*/

/**
 * @brief Convert pixel format to IPU format
 */
static IPU_COLOR_FMT_E ipu_format_convert(int ColorFormat)
{
    IPU_COLOR_FMT_E format = ColorFormat;
    return format;
}


/**
 * @brief Convert transform to IPU direction
 */
static IPU_DIRECTION_E ipu_transform_convert(transform_t transform)
{
    IPU_DIRECTION_E direction = IPU_DIRECTION_UNCHANGE;

    switch (transform) {
        case TRANSFORM_FLIP_HOR:
            direction = IPU_DIRECTION_FLIP_HOR;
            break;
        case TRANSFORM_FLIP_VER:
            direction = IPU_DIRECTION_FLIP_VER;
            break;
        case TRANSFORM_ROTATE_180:
            direction = IPU_DIRECTION_ROTATE_180;
            break;
        case TRANSFORM_ROTATE_90:
            direction = IPU_DIRECTION_ROTATE_90;
            break;
        case TRANSFORM_ROTATE_270:
            direction = IPU_DIRECTION_ROTATE_270;
            break;
    }

    return direction;
}

static const char *ipu_direction_name(IPU_DIRECTION_E dir)
{
    switch (dir) {
        case IPU_DIRECTION_ROTATE_180: return "ROTATE_180";
        case IPU_DIRECTION_ROTATE_90:  return "ROTATE_90";
        case IPU_DIRECTION_ROTATE_270: return "ROTATE_270";
        case IPU_DIRECTION_FLIP_VER:   return "FLIP_VER";
        case IPU_DIRECTION_FLIP_HOR:   return "FLIP_HOR";
        default:                       return "DIR";
    }
}

/** After Direction, 90/270 swap W/H. Returns 1 if dst size does not match. */
static int ipu_direction_size_mismatch(IPU_DIRECTION_E dir,
                                       unsigned src_w, unsigned src_h,
                                       unsigned dst_w, unsigned dst_h,
                                       unsigned *out_eff_w, unsigned *out_eff_h)
{
    unsigned eff_w = src_w;
    unsigned eff_h = src_h;

    if (dir == IPU_DIRECTION_ROTATE_90 || dir == IPU_DIRECTION_ROTATE_270) {
        eff_w = src_h;
        eff_h = src_w;
    }
    if (out_eff_w)
        *out_eff_w = eff_w;
    if (out_eff_h)
        *out_eff_h = eff_h;
    return (eff_w != dst_w || eff_h != dst_h) ? 1 : 0;
}

/**
 * @brief Get bytes per pixel for format
 */
static float ipu_bytes_per_pixel(IPU_COLOR_FMT_E fmt)
{
    float bpp = 0;

    switch (fmt) {
        case IPU_COLOR_FMT_PACKED_ARGB32:
        case IPU_COLOR_FMT_PACKED_ABGR32:
        case IPU_COLOR_FMT_PACKED_BGRA32:
        case IPU_COLOR_FMT_PACKED_RGBA32:
            bpp = 4.0;
            break;
        case IPU_COLOR_FMT_PACKED_RGB24:
        case IPU_COLOR_FMT_PACKED_BGR24:
            bpp = 3.0;
            break;
        case IPU_COLOR_FMT_PACKED_RGB565:
        case IPU_COLOR_FMT_PACKED_BGR565:
            bpp = 2.0;
            break;
        case IPU_COLOR_FMT_FULLPLANE_YV12:
        case IPU_COLOR_FMT_SEMIPLANE_NV12:
        case IPU_COLOR_FMT_SEMIPLANE_NV21:
            bpp = 1.5;
            break;
        default:
            break;
    }

    return bpp;
}

/*===========================================================================
 * Private Functions - Layer Setup
 *===========================================================================*/

/**
 * @brief Set image plane addresses for special formats
 * @note Most formats are handled by hal_ipu_setup_surface.
 *       This function only handles special cases with separate plane addresses.
 */
static int ipu_set_image_plane(struct image_desc *image, IPU_SURFACE_S *layer)
{
    IPU_COLOR_FMT_E fmt = ipu_format_convert(image->format);

    // Handle formats with separate plane addresses (not supported by hal_ipu_setup_surface)
    switch (fmt) {
        case IPU_COLOR_FMT_FULLPLANE_YV12:
            if (image->paddrs[0] != 0 && image->paddrs[1] != 0 && image->paddrs[2] != 0) {
                layer->stPlant[0].u64PhyAddr = image->paddrs[0];
                layer->stPlant[0].u32PitchLine = image->u32Width;
                layer->stPlant[0].u8PixStride = 1;
                layer->stPlant[1].u64PhyAddr = image->paddrs[1];
                layer->stPlant[1].u32PitchLine = image->u32Width >> 1;
                layer->stPlant[1].u8PixStride = 1;
                layer->stPlant[2].u64PhyAddr = image->paddrs[2];
                layer->stPlant[2].u32PitchLine = image->u32Width >> 1;
                layer->stPlant[2].u8PixStride = 1;
            }
            break;

        case IPU_COLOR_FMT_FULLPLANE_RGB:
        case IPU_COLOR_FMT_FULLPLANE_YV24:
            // Full plane formats with separate planes
            layer->stPlant[0].u64PhyAddr = image->paddr;
            layer->stPlant[0].u8PixStride = 1;
            layer->stPlant[0].u32PitchLine = image->u32Width;
            layer->stPlant[1].u64PhyAddr = image->paddr + image->u32Width * image->u32Height;
            layer->stPlant[1].u8PixStride = 1;
            layer->stPlant[1].u32PitchLine = image->u32Width;
            layer->stPlant[2].u64PhyAddr = image->paddr + image->u32Width * image->u32Height * 2;
            layer->stPlant[2].u8PixStride = 1;
            layer->stPlant[2].u32PitchLine = image->u32Width;
            break;

        default:
            // Other formats are already handled by hal_ipu_setup_surface
            // Only override if image has custom plane addresses
            if (image->paddrs[0] != 0 && image->paddrs[0] != image->paddr) {
                layer->stPlant[0].u64PhyAddr = image->paddrs[0];
                if (image->paddrs[1] != 0) {
                    layer->stPlant[1].u64PhyAddr = image->paddrs[1];
                }
            }
            break;
    }

    return AK_SUCCESS;
}

/**
 * @brief Set IPU layer from image descriptor
 */
static int ipu_layer_set(struct image_desc *image, IPU_SURFACE_S *layer, IPU_RECT_S *IpuRect)
{
    IpuRect->u32W = image->rect.width;
    IpuRect->u32H = image->rect.height;
    IpuRect->s32X = image->rect.x;
    IpuRect->s32Y = image->rect.y;

    IPU_COLOR_FMT_E fmt = ipu_format_convert(image->format);
    
    // Use hal_ipu_setup_surface for basic surface setup
    hal_ipu_setup_surface(layer, fmt, image->u32Width, image->u32Height, image->paddr);
    
    // Set GPU-specific settings
    layer->u8AlphaValue = image->u8GlobalOpacity;
    layer->u8ColorPadding = 0x01;
    layer->enDirection = ipu_transform_convert(image->transform);

    ipu_set_image_plane(image, layer);

    return AK_SUCCESS;
}

/**
 * @brief Check image validity
 */
static int ipu_check_image(struct image_desc *image, const char *function, int line)
{
    const char *func = function ? function : __FUNCTION__;
    rect_t *rect = &(image->rect);

    if ((rect->width <= 0) || (rect->height <= 0) ||
        ((rect->x + rect->width) > image->u32Width) ||
        ((rect->y + rect->height) > image->u32Height)) {
        log_error("[%s][%d] invalid rect of image", func, line);
        return AK_FAILED;
    }

    if ((rect->width < SUPPORTED_MIN_WIDTH) || (rect->height < SUPPORTED_MIN_HEIGHT) ||
        (rect->width > SUPPORTED_MAX_WIDTH) || (rect->height > SUPPORTED_MAX_HEIGHT)) {
        log_error("[%s][%d] image rect out of range, width: %d, height: %d",
               func, line, rect->width, rect->height);
        return AK_FAILED;
    }

    if ((NULL == image->vaddr) || (0 == image->paddr)) {
        log_error("[%s][%d] invalid address, vaddr: %p, paddr: 0x%llx",
               func, line, image->vaddr, (unsigned long long)image->paddr);
        return AK_FAILED;
    }

    return AK_SUCCESS;
}

/*===========================================================================
 * HAL API Implementation
 *===========================================================================*/

int hal_gpu_image_copy(struct gpu_device *device,
                       struct image_desc *pInputImage,
                       struct image_desc *pOutputImage)
{
    if ((NULL == device) || (NULL == pInputImage) || (NULL == pOutputImage)) {
        log_error("Invalid parameter");
        return AK_FAILED;
    }

    if (AK_SUCCESS != ipu_check_image(pInputImage, __FUNCTION__, __LINE__))
        return AK_FAILED;

    if (AK_SUCCESS != ipu_check_image(pOutputImage, __FUNCTION__, __LINE__))
        return AK_FAILED;

    int ret = AK_FAILED;
    IPU_RECT_S SrcRect = {0};
    IPU_SURFACE_S SrcLayer = {0};
    IPU_RECT_S TargetRect = {0};
    IPU_SURFACE_S TargetLayer = {0};

    ipu_layer_set(pInputImage, &SrcLayer, &SrcRect);
    ipu_layer_set(pOutputImage, &TargetLayer, &TargetRect);

    SrcLayer.u8AlphaValue = 0xFF;
    TargetLayer.u8AlphaValue = 0xFF;

    int queue_id = (s_gpu_ipu_queue_id >= 0) ? s_gpu_ipu_queue_id : IPU_QUEUE_INDEX;
    
    if (hal_ipu_start_queue(queue_id) != 0) {
        log_error("fun=%s line=%d, hal_ipu_start_queue failed", __FUNCTION__, __LINE__);
        return AK_FAILED;
    }
    
    if (hal_ipu_quick_copy(queue_id, &SrcLayer, &SrcRect, &TargetLayer, &TargetRect) != 0) {
        log_error("fun=%s line=%d, hal_ipu_quick_copy failed", __FUNCTION__, __LINE__);
        ret = AK_FAILED;
    }

    if (hal_ipu_end_queue(queue_id, AK_TRUE, AK_TRUE) != 0) {
        log_error("fun=%s line=%d, hal_ipu_end_queue failed", __FUNCTION__, __LINE__);
        ret = AK_FAILED;
    }
    ret = gpu_wait_done(__FUNCTION__, queue_id, __LINE__);

    return ret;
}

int hal_gpu_image_scale(struct gpu_device *device,
                        struct image_desc *pInputImage,
                        struct image_desc *pOutputImage)
{
    if ((NULL == device) || (NULL == pInputImage) || (NULL == pOutputImage)) {
        log_error("Invalid parameter");
        return AK_FAILED;
    }

    if (AK_SUCCESS != ipu_check_image(pInputImage, __FUNCTION__, __LINE__))
        return AK_FAILED;

    if (AK_SUCCESS != ipu_check_image(pOutputImage, __FUNCTION__, __LINE__))
        return AK_FAILED;

    int ret = AK_FAILED;
    IPU_RECT_S SrcRect = {0};
    IPU_SURFACE_S SrcLayer = {0};
    IPU_RECT_S TargetRect = {0};
    IPU_SURFACE_S TargetLayer = {0};

    ipu_layer_set(pInputImage, &SrcLayer, &SrcRect);
    ipu_layer_set(pOutputImage, &TargetLayer, &TargetRect);

    bool alpha_channel = (IPU_ALPHA_CHN_NONE != hal_ipu_get_alpha_channel(ipu_format_convert(pInputImage->format)));
    bool transform = ((SrcRect.u32W != TargetRect.u32W) && (SrcRect.u32H != TargetRect.u32H));
    bool opa = (pInputImage->u8GlobalOpacity < IPU_OPA_MAX) ? true : false;

    IPU_RECT_S TempRect = {0};
    IPU_SURFACE_S TempLayer = {0};
    struct image_desc TempImage = {0};
    uint32_t size = 0;
    TempImage.vaddr = NULL;

    if ((alpha_channel || opa) && transform) {
        TempImage.rect.x = 0;
        TempImage.rect.y = 0;
        TempImage.rect.width = TargetRect.u32W;
        TempImage.rect.height = TargetRect.u32H;
        TempImage.u32Width = TargetRect.u32W;
        TempImage.u32Height = TargetRect.u32H;
        TempImage.format = pInputImage->format;
        TempImage.transform = pInputImage->transform;
        TempImage.u8GlobalOpacity = pInputImage->u8GlobalOpacity;

        size = TargetRect.u32W * TargetRect.u32H * ipu_bytes_per_pixel(pInputImage->format);
        TempImage.vaddr = AK_BASE_MemDmaAlloc(MODULE_ID_APP, size);
        if (NULL == TempImage.vaddr) {
            log_error("fun=%s line=%d, malloc for temp image failed", __FUNCTION__, __LINE__);
            return AK_FAILED;
        }
        ret = AK_BASE_MemDmaVaddr2Paddr(TempImage.vaddr, &(TempImage.paddr));
        if (AK_SUCCESS != ret) {
            AK_BASE_MemDmaFree(TempImage.vaddr);
            TempImage.vaddr = NULL;
            log_error("DMA physical address error: %d", ret);
            return AK_FAILED;
        }
        ipu_layer_set(&TempImage, &TempLayer, &TempRect);
    }

    int queue_id = get_gpu_queue_id();
    if (hal_ipu_start_queue(queue_id) != 0) {
        log_error("fun=%s line=%d, hal_ipu_start_queue failed", __FUNCTION__, __LINE__);
        return AK_FAILED;
    }

    if (alpha_channel && transform && (NULL != TempImage.vaddr)) {
        if (hal_ipu_composite_resize(queue_id, &SrcLayer, &SrcRect, &TempLayer, &TempRect) != 0) {
            log_error("fun=%s line=%d, hal_ipu_composite_resize failed", __FUNCTION__, __LINE__);
            ret = AK_FAILED;
        }

        IPU_BLEND_OPT_S BlendOpt = {0};
        BlendOpt.enOpt = IPU_OPT_SOLID_COLOR_BLENDING;
        BlendOpt.stSolidColorBlendAttr.u8Opacity = pInputImage->u8GlobalOpacity;
        TargetLayer.enAlphaChnChoice = IPU_ALPHA_CHN_NONE;
        TempLayer.enDirection = IPU_DIRECTION_UNCHANGE;
        
        AK_MPI_IPU_Blend(queue_id, &TargetLayer, &TargetRect, &TempLayer, &TempRect,
                         &TargetLayer, &TargetRect, &BlendOpt);
    } else {
        if (hal_ipu_composite_resize(queue_id, &SrcLayer, &SrcRect, &TargetLayer, &TargetRect) != 0) {
            log_error("fun=%s line=%d, hal_ipu_composite_resize failed", __FUNCTION__, __LINE__);
            ret = AK_FAILED;
        }
    }

    if (hal_ipu_end_queue(queue_id, AK_TRUE, AK_TRUE) != 0) {
        log_error("fun=%s line=%d, hal_ipu_end_queue failed", __FUNCTION__, __LINE__);
        ret = AK_FAILED;
    }
    ret = gpu_wait_done(__FUNCTION__, queue_id, __LINE__);

    if (NULL != TempImage.vaddr) {
        AK_BASE_MemDmaFree(TempImage.vaddr);
        TempImage.vaddr = NULL;
    }

    return ret;
}

int hal_gpu_image_transform(struct gpu_device *device,
                            struct image_desc *pInputImage,
                            struct image_desc *pOutputImage,
                            transform_t transform)
{
    if ((NULL == device) || (NULL == pInputImage) || (NULL == pOutputImage)) {
        log_error("Invalid parameter");
        return AK_FAILED;
    }

    if (AK_SUCCESS != ipu_check_image(pInputImage, __FUNCTION__, __LINE__))
        return AK_FAILED;

    if (AK_SUCCESS != ipu_check_image(pOutputImage, __FUNCTION__, __LINE__))
        return AK_FAILED;

    int ret = AK_FAILED;
    IPU_RECT_S SrcRect = {0};
    IPU_SURFACE_S SrcLayer = {0};
    IPU_RECT_S TargetRect = {0};
    IPU_SURFACE_S TargetLayer = {0};

    ipu_layer_set(pInputImage, &SrcLayer, &SrcRect);
    ipu_layer_set(pOutputImage, &TargetLayer, &TargetRect);

    SrcLayer.u8AlphaValue = 0xFF;
    SrcLayer.enDirection = ipu_transform_convert(transform);

    bool alpha_channel = (IPU_ALPHA_CHN_NONE != hal_ipu_get_alpha_channel(ipu_format_convert(pInputImage->format)));
    bool scale = ((SrcRect.u32W != TargetRect.u32W) && (SrcRect.u32H != TargetRect.u32H));

    IPU_RECT_S TempRect = {0};
    IPU_SURFACE_S TempLayer = {0};
    struct image_desc TempImage = {0};
    uint32_t size = 0;
    TempImage.vaddr = NULL;

    if (alpha_channel && scale) {
        TempImage.rect.x = 0;
        TempImage.rect.y = 0;
        TempImage.rect.width = TargetRect.u32W;
        TempImage.rect.height = TargetRect.u32H;
        TempImage.u32Width = TargetRect.u32W;
        TempImage.u32Height = TargetRect.u32H;
        TempImage.format = pInputImage->format;
        TempImage.transform = pInputImage->transform;
        TempImage.u8GlobalOpacity = pInputImage->u8GlobalOpacity;

        size = TargetRect.u32W * TargetRect.u32H * ipu_bytes_per_pixel(pInputImage->format);
        TempImage.vaddr = AK_BASE_MemDmaAlloc(MODULE_ID_APP, size);
        if (NULL == TempImage.vaddr) {
            log_error("fun=%s line=%d, malloc for temp image failed", __FUNCTION__, __LINE__);
            return AK_FAILED;
        }
        ret = AK_BASE_MemDmaVaddr2Paddr(TempImage.vaddr, &(TempImage.paddr));
        if (AK_SUCCESS != ret) {
            AK_BASE_MemDmaFree(TempImage.vaddr);
            TempImage.vaddr = NULL;
            log_error("DMA physical address error: %d", ret);
            return AK_FAILED;
        }
        ipu_layer_set(&TempImage, &TempLayer, &TempRect);
    }

    int queue_id = get_gpu_queue_id();
    if (hal_ipu_start_queue(queue_id) != 0) {
        log_error("fun=%s line=%d, hal_ipu_start_queue failed", __FUNCTION__, __LINE__);
        return AK_FAILED;
    }

    if (alpha_channel) {
        IPU_BLEND_OPT_S BlendOpt = {0};
        BlendOpt.enOpt = IPU_OPT_SOLID_COLOR_BLENDING;
        BlendOpt.stSolidColorBlendAttr.u8Opacity = pInputImage->u8GlobalOpacity;
        TargetLayer.enAlphaChnChoice = IPU_ALPHA_CHN_NONE;

        if (scale && (NULL != TempImage.vaddr)) {
            if (hal_ipu_composite_resize(queue_id, &SrcLayer, &SrcRect, &TempLayer, &TempRect) != 0) {
                log_error("fun=%s line=%d, hal_ipu_composite_resize failed", __FUNCTION__, __LINE__);
                ret = AK_FAILED;
            }
            TempLayer.enDirection = IPU_DIRECTION_UNCHANGE;
            
            AK_MPI_IPU_Blend(queue_id, &TargetLayer, &TargetRect, &TempLayer, &TempRect,
                             &TargetLayer, &TargetRect, &BlendOpt);
        } else {
            AK_MPI_IPU_Blend(queue_id, &TargetLayer, &TargetRect, &SrcLayer, &SrcRect,
                             &TargetLayer, &TargetRect, &BlendOpt);
        }
    } else {
        if (scale) {
            if (hal_ipu_composite_resize(queue_id, &SrcLayer, &SrcRect, &TargetLayer, &TargetRect) != 0) {
                log_error("fun=%s line=%d, hal_ipu_composite_resize failed", __FUNCTION__, __LINE__);
                ret = AK_FAILED;
            }
        } else {
            if (hal_ipu_quick_copy(queue_id, &SrcLayer, &SrcRect, &TargetLayer, &TargetRect) != 0) {
                log_error("fun=%s line=%d, hal_ipu_quick_copy failed", __FUNCTION__, __LINE__);
                ret = AK_FAILED;
            }
        }
    }

    if (hal_ipu_end_queue(queue_id, AK_TRUE, AK_TRUE) != 0) {
        log_error("fun=%s line=%d, hal_ipu_end_queue failed", __FUNCTION__, __LINE__);
        ret = AK_FAILED;
    }
    ret = gpu_wait_done(__FUNCTION__, queue_id, __LINE__);

    if (NULL != TempImage.vaddr) {
        AK_BASE_MemDmaFree(TempImage.vaddr);
        TempImage.vaddr = NULL;
    }

    return ret;
}

int hal_gpu_image_blend(struct gpu_device *device,
                        struct image_desc *pForeGround,
                        struct image_desc *pBackGround,
                        struct image_desc *pOutputImage,
                        struct blend_config *config)
{
    if ((NULL == device) || (NULL == pForeGround) || (NULL == pBackGround)) {
        log_error("Invalid parameter");
        return AK_FAILED;
    }

    if (AK_SUCCESS != ipu_check_image(pForeGround, __FUNCTION__, __LINE__))
        return AK_FAILED;

    if (AK_SUCCESS != ipu_check_image(pBackGround, __FUNCTION__, __LINE__))
        return AK_FAILED;

    if (NULL != pOutputImage)
        if (AK_SUCCESS != ipu_check_image(pOutputImage, __FUNCTION__, __LINE__))
            return AK_FAILED;

    int ret = AK_FAILED;
    IPU_RECT_S ForeGroundRect = {0};
    IPU_SURFACE_S ForeGroundLayer = {0};
    IPU_RECT_S BackGroundRect = {0};
    IPU_SURFACE_S BackGroundLayer = {0};
    IPU_RECT_S OutputRect = {0};
    IPU_SURFACE_S OutputLayer = {0};

    ipu_layer_set(pForeGround, &ForeGroundLayer, &ForeGroundRect);
    ipu_layer_set(pBackGround, &BackGroundLayer, &BackGroundRect);

    if (NULL != pOutputImage)
        ipu_layer_set(pOutputImage, &OutputLayer, &OutputRect);

    int option = 0;
    IPU_BLEND_OPT_S BlendOpt = {0};
    if (IPU_ALPHA_CHN_NONE == BackGroundLayer.enAlphaChnChoice)
        option = eBLEND_SOLID_COLOR;
    else
        option = eBLEND_ALPHA_BLEND;

    switch (option) {
        case eBLEND_SOLID_COLOR:
            BlendOpt.enOpt = IPU_OPT_SOLID_COLOR_BLENDING;
            BlendOpt.stSolidColorBlendAttr.u8Opacity = 0xFF;
            BackGroundLayer.enAlphaChnChoice = IPU_ALPHA_CHN_NONE;
            if (NULL != pOutputImage)
                OutputLayer.enAlphaChnChoice = IPU_ALPHA_CHN_NONE;
            break;

        case eBLEND_ALPHA_BLEND:
            BlendOpt.enOpt = IPU_OPT_SPECIFIED_ALPHA_BLENDING;
            BlendOpt.stSpeciAlphaBlendAttr.u8gAlphaFg = 0xFF;
            BlendOpt.stSpeciAlphaBlendAttr.u8gAlphaBg = 0xFF;
            break;

        case eBLEND_PURE_BACKGROUND:
            BackGroundLayer.enAlphaChnChoice = IPU_ALPHA_CHN_NONE;
            BlendOpt.stSolidBgBlendAttr.u32ColorBG = config->u8ColorBackGround;
            BlendOpt.stSolidBgBlendAttr.u8Opacity = 0xFF;
            if (NULL != pOutputImage)
                OutputLayer.enAlphaChnChoice = IPU_ALPHA_CHN_NONE;
            break;

        case eBLEND_PURE_FOREGROUND:
            BackGroundLayer.enAlphaChnChoice = IPU_ALPHA_CHN_NONE;
            BlendOpt.stSolidFgBlendAttr.u32ColorFG = config->u8ColorForeGround;
            BlendOpt.stSolidFgBlendAttr.u8Opacity = 0xFF;
            if (NULL != pOutputImage)
                OutputLayer.enAlphaChnChoice = IPU_ALPHA_CHN_NONE;
            break;

        default:
            log_error("Unsupported blending option");
            break;
    }

    int queue_id = get_gpu_queue_id();

    /* Refuse Blend+Direction when dst size != footprint after Direction (unsafe). */
    if (ForeGroundLayer.enDirection != IPU_DIRECTION_UNCHANGE) {
        IPU_RECT_S *dst_rect = (NULL != pOutputImage) ? &OutputRect : &BackGroundRect;
        unsigned eff_w = 0, eff_h = 0;
        if (ipu_direction_size_mismatch(ForeGroundLayer.enDirection,
                                        (unsigned)ForeGroundRect.u32W,
                                        (unsigned)ForeGroundRect.u32H,
                                        (unsigned)dst_rect->u32W,
                                        (unsigned)dst_rect->u32H,
                                        &eff_w, &eff_h)) {
            log_error("refuse Blend+%s: FG %ux%u -> eff %ux%u but DST %ux%u",
                      ipu_direction_name(ForeGroundLayer.enDirection),
                      (unsigned)ForeGroundRect.u32W, (unsigned)ForeGroundRect.u32H,
                      eff_w, eff_h,
                      (unsigned)dst_rect->u32W, (unsigned)dst_rect->u32H);
            return AK_FAILED;
        }
    }

    if (hal_ipu_start_queue(queue_id) != 0) {
        log_error("fun=%s line=%d, hal_ipu_start_queue failed", __FUNCTION__, __LINE__);
        return AK_FAILED;
    }

    if (NULL != pOutputImage) {
        AK_MPI_IPU_Blend(queue_id, &BackGroundLayer, &BackGroundRect, &ForeGroundLayer, &ForeGroundRect,
                         &OutputLayer, &OutputRect, &BlendOpt);
    } else {
        AK_MPI_IPU_Blend(queue_id, &BackGroundLayer, &BackGroundRect, &ForeGroundLayer, &ForeGroundRect,
                         &BackGroundLayer, &BackGroundRect, &BlendOpt);
    }

    if (hal_ipu_end_queue(queue_id, AK_TRUE, AK_TRUE) != 0) {
        log_error("fun=%s line=%d, hal_ipu_end_queue failed", __FUNCTION__, __LINE__);
        ret = AK_FAILED;
    }
    ret = gpu_wait_done(__FUNCTION__, queue_id, __LINE__);

    return ret;
}

int hal_gpu_image_blend_colorkey(struct gpu_device *device,
                                 struct image_desc *pInputImage,
                                 struct image_desc *pOutputImage,
                                 struct image_colorkey *colorkey)
{
    if ((NULL == device) || (NULL == pInputImage) || (NULL == pOutputImage) || (NULL == colorkey)) {
        log_error("Invalid parameter");
        return AK_FAILED;
    }

    if (AK_SUCCESS != ipu_check_image(pInputImage, __FUNCTION__, __LINE__))
        return AK_FAILED;

    if (AK_SUCCESS != ipu_check_image(pOutputImage, __FUNCTION__, __LINE__))
        return AK_FAILED;

    int ret = AK_FAILED;
    IPU_RECT_S SrcRect = {0};
    IPU_SURFACE_S SrcLayer = {0};
    IPU_RECT_S TargetRect = {0};
    IPU_SURFACE_S TargetLayer = {0};

    ipu_layer_set(pInputImage, &SrcLayer, &SrcRect);
    ipu_layer_set(pOutputImage, &TargetLayer, &TargetRect);

    SrcLayer.enAlphaChnChoice = IPU_ALPHA_CHN_NONE;
    TargetLayer.enAlphaChnChoice = IPU_ALPHA_CHN_NONE;

    IPU_BLEND_OPT_S BlendOpt = {0};
    BlendOpt.enOpt = IPU_OPT_COLOR_KEY_BLENDING;
    BlendOpt.stColorScopeBlendAttr.u8OpacityPositive = colorkey->u8OpacityPositive;
    BlendOpt.stColorScopeBlendAttr.u8OpacityNegative = colorkey->u8OpacityInverse;
    BlendOpt.stColorScopeBlendAttr.u32RefColor = colorkey->u32ColorKey;
    BlendOpt.stColorScopeBlendAttr.u8Differ = colorkey->u8Differ;

    int queue_id = get_gpu_queue_id();
    if (hal_ipu_start_queue(queue_id) != 0) {
        log_error("fun=%s line=%d, hal_ipu_start_queue failed", __FUNCTION__, __LINE__);
        return AK_FAILED;
    }
    
    AK_MPI_IPU_Blend(queue_id, NULL, NULL, &SrcLayer, &SrcRect,
                     &TargetLayer, &TargetRect, &BlendOpt);

    if (hal_ipu_end_queue(queue_id, AK_TRUE, AK_TRUE) != 0) {
        log_error("fun=%s line=%d, hal_ipu_end_queue failed", __FUNCTION__, __LINE__);
        ret = AK_FAILED;
    }
    ret = gpu_wait_done(__FUNCTION__, queue_id, __LINE__);

    return ret;
}

int hal_gpu_image_fill(struct gpu_device *device,
                       struct image_desc *pOutputImage,
                       uint32_t color)
{
    if ((NULL == device) || (NULL == pOutputImage)) {
        log_error("Invalid parameter");
        return AK_FAILED;
    }

    if (AK_SUCCESS != ipu_check_image(pOutputImage, __FUNCTION__, __LINE__))
        return AK_FAILED;

    int ret = AK_FAILED;
    IPU_RECT_S TargetRect = {0};
    IPU_SURFACE_S TargetLayer = {0};

    ipu_layer_set(pOutputImage, &TargetLayer, &TargetRect);

    int queue_id = get_gpu_queue_id();
    if (hal_ipu_start_queue(queue_id) != 0) {
        log_error("fun=%s line=%d, hal_ipu_start_queue failed", __FUNCTION__, __LINE__);
        return AK_FAILED;
    }
    
    if (hal_ipu_quick_fill(queue_id, &TargetLayer, &TargetRect, color) != 0) {
        log_error("fun=%s line=%d, hal_ipu_quick_fill failed", __FUNCTION__, __LINE__);
        ret = AK_FAILED;
    }

    if (hal_ipu_end_queue(queue_id, AK_TRUE, AK_TRUE) != 0) {
        log_error("fun=%s line=%d, hal_ipu_end_queue failed", __FUNCTION__, __LINE__);
        ret = AK_FAILED;
    }
    ret = gpu_wait_done(__FUNCTION__, queue_id, __LINE__);

    return ret;
}

bool hal_gpu_check_input_format(pixel_format_t format)
{
    bool ret = false;

    switch (format) {
        case SYSTEM_PIXEL_FORMAT_FULLPLANE_YV12:
        case SYSTEM_PIXEL_FORMAT_SEMIPLANE_NV12:
        case SYSTEM_PIXEL_FORMAT_SEMIPLANE_NV21:
        case SYSTEM_PIXEL_FORMAT_ARGB32:
        case SYSTEM_PIXEL_FORMAT_ABGR32:
        case SYSTEM_PIXEL_FORMAT_BGRA32:
        case SYSTEM_PIXEL_FORMAT_RGBA32:
        case SYSTEM_PIXEL_FORMAT_RGB24:
        case SYSTEM_PIXEL_FORMAT_BGR24:
        case SYSTEM_PIXEL_FORMAT_RGB565:
        case SYSTEM_PIXEL_FORMAT_BGR565:
            ret = true;
            break;
        default:
            break;
    }

    return ret;
}

bool hal_gpu_check_output_format(pixel_format_t format)
{
    bool ret = false;

    switch (format) {
        case SYSTEM_PIXEL_FORMAT_FULLPLANE_YV12:
        case SYSTEM_PIXEL_FORMAT_SEMIPLANE_NV12:
        case SYSTEM_PIXEL_FORMAT_SEMIPLANE_NV21:
        case SYSTEM_PIXEL_FORMAT_ARGB32:
        case SYSTEM_PIXEL_FORMAT_ABGR32:
        case SYSTEM_PIXEL_FORMAT_BGRA32:
        case SYSTEM_PIXEL_FORMAT_RGBA32:
        case SYSTEM_PIXEL_FORMAT_RGB24:
        case SYSTEM_PIXEL_FORMAT_BGR24:
        case SYSTEM_PIXEL_FORMAT_RGB565:
        case SYSTEM_PIXEL_FORMAT_BGR565:
            ret = true;
            break;
        default:
            break;
    }

    return ret;
}

int32_t hal_gpu_supported_min_width(void)
{
    return SUPPORTED_MIN_WIDTH;
}

int32_t hal_gpu_supported_min_height(void)
{
    return SUPPORTED_MIN_HEIGHT;
}

int hal_gpu_general_process(struct gpu_device *device,
                            struct image_desc *pInputImage,
                            struct image_desc *pOutputImage)
{
    if ((NULL == pInputImage) || (NULL == pOutputImage)) {
        log_error("Invalid parameter");
        return AK_FAILED;
    }

    if (AK_SUCCESS != ipu_check_image(pInputImage, __FUNCTION__, __LINE__))
        return AK_FAILED;

    if (AK_SUCCESS != ipu_check_image(pOutputImage, __FUNCTION__, __LINE__))
        return AK_FAILED;

    /* Use scale or copy based on dimensions */
    if (pInputImage->u32Width == pOutputImage->u32Width &&
        pInputImage->u32Height == pOutputImage->u32Height) {
        return hal_gpu_image_copy(device, pInputImage, pOutputImage);
    } else {
        return hal_gpu_image_scale(device, pInputImage, pOutputImage);
    }
}

int hal_gpu_close(struct hw_device_t *device)
{
    if (!s_init_done) {
        log_warn("[%s][%d] hal gpu has not opened", __FUNCTION__, __LINE__);
        return AK_SUCCESS;
    }

    int ret = AK_SUCCESS;

    if (NULL != device) {
        free(device);
        device = NULL;
    } else {
        log_warn("[%s][%d] hal gpu close device is null, only close gpu", __FUNCTION__, __LINE__);
    }

    // Free queue
    if (s_gpu_ipu_queue_id >= 0) {
        hal_ipu_free_queue(s_gpu_ipu_queue_id);
        s_gpu_ipu_queue_id = -1;
    }

    // Close IPU (reference counted)
    if (hal_ipu_close() != 0) {
        log_error("Failed to close IPU");
        ret = AK_FAILED;
    }

    s_init_done = 0;

    return ret;
}

int hal_gpu_open(void)
{
    if (s_init_done) {
        log_warn("[%s][%d] hal gpu has opened", __FUNCTION__, __LINE__);
        return AK_SUCCESS;
    }

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);

    // Open IPU (reference counted)
    if (hal_ipu_open() != 0) {
        log_error("[%s][%d] hal_ipu_open failed", __FUNCTION__, __LINE__);
        return AK_FAILED;
    }

    // Allocate queue for GPU module (default: queue 0)
    int queue_id;
    if (hal_ipu_get_default_queue("gpu", &queue_id) != 0) {
        log_error("[%s][%d] Failed to allocate IPU queue for GPU", __FUNCTION__, __LINE__);
        hal_ipu_close();
        return AK_FAILED;
    }
    
    s_gpu_ipu_queue_id = queue_id;
    s_init_done = 1;

    log_info("[%s][%d] GPU IPU opened, alloc queue_id=%d (get_default gpu prefers queue0, fallback static IPU_QUEUE_INDEX=%d)",
             __FUNCTION__, __LINE__, queue_id, (int)IPU_QUEUE_INDEX);

    return AK_SUCCESS;
}

