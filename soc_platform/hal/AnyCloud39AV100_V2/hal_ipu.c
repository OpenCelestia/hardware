// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/soc_platform.
 *
 * IPU HAL implementation for AnyCloud39AV100_V2 platform.
 * - Provides unified IPU management with reference counting and queue allocation.
 * - Implements IPU operations for image processing.
 */

#define LOG_TAG "HAL IPU"

#include "hal_ipu.h"
#include "hal_ipu_api.h"

#include "ak_common.h"
#include <pthread.h>
#include <string.h>

#if !HAL_HAS_AK_IPU
int hal_ipu_open(void) { return -1; }
int hal_ipu_close(void) { return 0; }
int hal_ipu_force_close(void) { return 0; }
bool hal_ipu_is_initialized(void) { return false; }
int hal_ipu_alloc_queue(int *queue_id_out) { (void)queue_id_out; return -1; }
int hal_ipu_free_queue(int queue_id) { (void)queue_id; return -1; }
int hal_ipu_get_default_queue(const char *module_name, int *queue_id_out) { (void)module_name; (void)queue_id_out; return -1; }
int hal_ipu_start_queue(int queue_id) { (void)queue_id; return -1; }
int hal_ipu_end_queue(int queue_id, bool sync, bool wait) { (void)queue_id; (void)sync; (void)wait; return -1; }
int hal_ipu_wait_done(int queue_id, int timeout_ms) { (void)queue_id; (void)timeout_ms; return -1; }
IPU_COLOR_TYPE_E hal_ipu_get_color_type(IPU_COLOR_FMT_E fmt) { (void)fmt; return 0; }
IPU_ALPHA_CHN_E hal_ipu_get_alpha_channel(IPU_COLOR_FMT_E fmt) { (void)fmt; return 0; }
void hal_ipu_setup_surface(IPU_SURFACE_S *surface, IPU_COLOR_FMT_E format,
                           unsigned int width, unsigned int height, unsigned long phy_addr)
{ (void)surface; (void)format; (void)width; (void)height; (void)phy_addr; }
int hal_ipu_quick_copy(int queue_id, IPU_SURFACE_S *src, IPU_RECT_S *src_rect, IPU_SURFACE_S *dst, IPU_RECT_S *dst_rect)
{ (void)queue_id; (void)src; (void)src_rect; (void)dst; (void)dst_rect; return -1; }
int hal_ipu_composite_resize(int queue_id, IPU_SURFACE_S *src, IPU_RECT_S *src_rect, IPU_SURFACE_S *dst, IPU_RECT_S *dst_rect)
{ (void)queue_id; (void)src; (void)src_rect; (void)dst; (void)dst_rect; return -1; }
int hal_ipu_scale_crop_with_format(int queue_id, unsigned long in_phy_addr, unsigned int iw, unsigned int ih,
                                    IPU_RECT_S *in_rect, IPU_COLOR_FMT_E in_format, unsigned long out_phy_addr,
                                    unsigned int ow, unsigned int oh, IPU_COLOR_FMT_E out_format)
{ (void)queue_id; (void)in_phy_addr; (void)iw; (void)ih; (void)in_rect; (void)in_format; (void)out_phy_addr; (void)ow; (void)oh; (void)out_format; return -1; }
int hal_ipu_blend(int queue_id, IPU_SURFACE_S *bg, IPU_RECT_S *bg_rect, IPU_SURFACE_S *fg, IPU_RECT_S *fg_rect, IPU_SURFACE_S *dst, IPU_RECT_S *dst_rect)
{ (void)queue_id; (void)bg; (void)bg_rect; (void)fg; (void)fg_rect; (void)dst; (void)dst_rect; return -1; }
int hal_ipu_quick_fill(int queue_id, IPU_SURFACE_S *dst, IPU_RECT_S *dst_rect, uint32_t color)
{ (void)queue_id; (void)dst; (void)dst_rect; (void)color; return -1; }
#else

/*===========================================================================
 * Private Variables
 *===========================================================================*/

static int s_ipu_ref_count = 0;
static bool s_ipu_initialized = false;
static pthread_mutex_t s_ipu_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool s_queue_allocated[HAL_IPU_MAX_QUEUES] = {false};
static pthread_mutex_t s_queue_mutex = PTHREAD_MUTEX_INITIALIZER;

/*===========================================================================
 * Reference Counting Management
 *===========================================================================*/

int hal_ipu_open(void)
{
    pthread_mutex_lock(&s_ipu_mutex);
    
    if (s_ipu_ref_count == 0) {
        // First reference, initialize IPU
        IPU_ATTR_S ipu_attr = {0};
        ipu_attr.au32QueSize[0] = HAL_IPU_QUEUE_SIZE;
        ipu_attr.au32QueSize[1] = HAL_IPU_QUEUE_SIZE;
        ipu_attr.u32RespSize = HAL_IPU_RESP_SIZE;
        
        if (AK_MPI_IPU_Open(&ipu_attr) != AK_SUCCESS) {
            pthread_mutex_unlock(&s_ipu_mutex);
            return -1;
        }
        
        s_ipu_initialized = true;
    }
    
    s_ipu_ref_count++;
    pthread_mutex_unlock(&s_ipu_mutex);
    return 0;
}

int hal_ipu_close(void)
{
    pthread_mutex_lock(&s_ipu_mutex);
    
    if (s_ipu_ref_count > 0) {
        s_ipu_ref_count--;
        
        if (s_ipu_ref_count == 0) {
            // Last reference, close IPU
            if (s_ipu_initialized) {
                AK_MPI_IPU_Close();
                s_ipu_initialized = false;
            }
        }
    }
    
    pthread_mutex_unlock(&s_ipu_mutex);
    return 0;
}

int hal_ipu_force_close(void)
{
    pthread_mutex_lock(&s_ipu_mutex);
    if (s_ipu_initialized) {
        AK_MPI_IPU_Close();
        s_ipu_initialized = false;
    }
    s_ipu_ref_count = 0;
    pthread_mutex_unlock(&s_ipu_mutex);
    return 0;
}

bool hal_ipu_is_initialized(void)
{
    bool initialized;
    pthread_mutex_lock(&s_ipu_mutex);
    initialized = s_ipu_initialized;
    pthread_mutex_unlock(&s_ipu_mutex);
    return initialized;
}

/*===========================================================================
 * Queue Management
 *===========================================================================*/

int hal_ipu_alloc_queue(int *queue_id_out)
{
    if (queue_id_out == NULL) {
        return -1;
    }
    
    pthread_mutex_lock(&s_queue_mutex);
    
    for (int i = 0; i < HAL_IPU_MAX_QUEUES; i++) {
        if (!s_queue_allocated[i]) {
            s_queue_allocated[i] = true;
            *queue_id_out = i;
            pthread_mutex_unlock(&s_queue_mutex);
            return 0;
        }
    }
    
    pthread_mutex_unlock(&s_queue_mutex);
    return -1; // No available queue
}

int hal_ipu_free_queue(int queue_id)
{
    if (queue_id < 0 || queue_id >= HAL_IPU_MAX_QUEUES) {
        return -1;
    }
    
    pthread_mutex_lock(&s_queue_mutex);
    s_queue_allocated[queue_id] = false;
    pthread_mutex_unlock(&s_queue_mutex);
    return 0;
}

int hal_ipu_get_default_queue(const char *module_name, int *queue_id_out)
{
    if (module_name == NULL || queue_id_out == NULL) {
        return -1;
    }
    
    // Default allocation: GPU -> queue 0, Camera -> queue 1
    int preferred_queue = -1;
    if (strcmp(module_name, "gpu") == 0) {
        preferred_queue = 0;
    } else if (strcmp(module_name, "camera") == 0) {
        preferred_queue = 1;
    } else {
        return -1; // Unknown module
    }
    
    // Try to allocate preferred queue first
    pthread_mutex_lock(&s_queue_mutex);
    if (!s_queue_allocated[preferred_queue]) {
        s_queue_allocated[preferred_queue] = true;
        *queue_id_out = preferred_queue;
        pthread_mutex_unlock(&s_queue_mutex);
        return 0;
    }
    pthread_mutex_unlock(&s_queue_mutex);
    
    // Preferred queue not available, try to allocate any available queue
    return hal_ipu_alloc_queue(queue_id_out);
}

/*===========================================================================
 * IPU Operations
 *===========================================================================*/

int hal_ipu_start_queue(int queue_id)
{
    if (!hal_ipu_is_initialized()) {
        return -1;
    }
    
    if (queue_id < 0 || queue_id >= HAL_IPU_MAX_QUEUES) {
        return -1;
    }
    
    AK_MPI_IPU_StartQue(queue_id);
    return 0;
}

int hal_ipu_end_queue(int queue_id, bool sync, bool wait)
{
    if (!hal_ipu_is_initialized()) {
        return -1;
    }
    
    if (queue_id < 0 || queue_id >= HAL_IPU_MAX_QUEUES) {
        return -1;
    }
    
    AK_MPI_IPU_EndQue(queue_id, sync ? AK_TRUE : AK_FALSE, wait ? AK_TRUE : AK_FALSE);
    return 0;
}

int hal_ipu_wait_done(int queue_id, int timeout_ms)
{
    if (!hal_ipu_is_initialized()) {
        return -1;
    }
    
    if (queue_id < 0 || queue_id >= HAL_IPU_MAX_QUEUES) {
        return -1;
    }
    
    return AK_MPI_IPU_WaitForDone(queue_id, timeout_ms);
}

int hal_ipu_quick_copy(int queue_id,
                       IPU_SURFACE_S *src, IPU_RECT_S *src_rect,
                       IPU_SURFACE_S *dst, IPU_RECT_S *dst_rect)
{
    if (!hal_ipu_is_initialized()) {
        return -1;
    }
    
    if (src == NULL || src_rect == NULL || dst == NULL || dst_rect == NULL) {
        return -1;
    }
    
    if (queue_id < 0 || queue_id >= HAL_IPU_MAX_QUEUES) {
        return -1;
    }
    
    AK_MPI_IPU_QuickCopy(queue_id, src, src_rect, dst, dst_rect);
    return 0;
}

int hal_ipu_composite_resize(int queue_id,
                             IPU_SURFACE_S *src, IPU_RECT_S *src_rect,
                             IPU_SURFACE_S *dst, IPU_RECT_S *dst_rect)
{
    if (!hal_ipu_is_initialized()) {
        return -1;
    }
    
    if (src == NULL || src_rect == NULL || dst == NULL || dst_rect == NULL) {
        return -1;
    }
    
    if (queue_id < 0 || queue_id >= HAL_IPU_MAX_QUEUES) {
        return -1;
    }
    
    AK_MPI_IPU_CompositeResize(queue_id, src, src_rect, dst, dst_rect);
    return 0;
}

int hal_ipu_blend(int queue_id,
                  IPU_SURFACE_S *bg, IPU_RECT_S *bg_rect,
                  IPU_SURFACE_S *fg, IPU_RECT_S *fg_rect,
                  IPU_SURFACE_S *dst, IPU_RECT_S *dst_rect)
{
    // Note: This function is deprecated. 
    // AK_MPI_IPU_Blend requires IPU_BLEND_OPT_S parameter which is complex.
    // Modules should call AK_MPI_IPU_Blend directly but use hal_ipu_* for queue management.
    // This function is kept for API compatibility but may not work correctly.
    // TODO: Remove this function or add BlendOpt parameter.
    return -1; // Not implemented - use AK_MPI_IPU_Blend directly
}

int hal_ipu_quick_fill(int queue_id,
                       IPU_SURFACE_S *dst, IPU_RECT_S *dst_rect,
                       uint32_t color)
{
    if (!hal_ipu_is_initialized()) {
        return -1;
    }
    
    if (dst == NULL || dst_rect == NULL) {
        return -1;
    }
    
    if (queue_id < 0 || queue_id >= HAL_IPU_MAX_QUEUES) {
        return -1;
    }
    
    AK_MPI_IPU_QuickFill(queue_id, dst, dst_rect, color);
    return 0;
}
#endif

/*===========================================================================
 * Surface Setup Helper Functions
 *===========================================================================*/

#if HAL_HAS_AK_IPU
IPU_COLOR_TYPE_E hal_ipu_get_color_type(IPU_COLOR_FMT_E fmt)
{
    switch (fmt) {
        case IPU_COLOR_FMT_PACKED_ARGB32:
        case IPU_COLOR_FMT_PACKED_ABGR32:
        case IPU_COLOR_FMT_PACKED_BGRA32:
        case IPU_COLOR_FMT_PACKED_RGBA32:
        case IPU_COLOR_FMT_PACKED_RGB24:
        case IPU_COLOR_FMT_PACKED_BGR24:
        case IPU_COLOR_FMT_PACKED_RGB565:
        case IPU_COLOR_FMT_PACKED_BGR565:
        case IPU_COLOR_FMT_PACKED_RGB555:
        case IPU_COLOR_FMT_PACKED_BGR555:
            return IPU_COLOR_TYPE_RGB;
        default:
            return IPU_COLOR_TYPE_UNKNOWN;
    }
}

IPU_ALPHA_CHN_E hal_ipu_get_alpha_channel(IPU_COLOR_FMT_E fmt)
{
    switch (fmt) {
        case IPU_COLOR_FMT_PACKED_ARGB32:
        case IPU_COLOR_FMT_PACKED_ABGR32:
        case IPU_COLOR_FMT_PACKED_BGRA32:
        case IPU_COLOR_FMT_PACKED_RGBA32:
            return IPU_ALPHA_CHN_PACKED;
        default:
            return IPU_ALPHA_CHN_NONE;
    }
}

void hal_ipu_setup_surface(IPU_SURFACE_S *s, IPU_COLOR_FMT_E fmt,
                           unsigned int w, unsigned int h, unsigned long addr)
{
    memset(s, 0, sizeof(IPU_SURFACE_S));
    s->u32Width = w;
    s->u32Height = h;
    s->enColorFmt = fmt;
    s->enColorTypeImage = hal_ipu_get_color_type(fmt);
    s->enAlphaChnChoice = hal_ipu_get_alpha_channel(fmt);
    
    if (fmt == IPU_COLOR_FMT_SEMIPLANE_NV12 || fmt == IPU_COLOR_FMT_SEMIPLANE_NV21 ||
        fmt == IPU_COLOR_FMT_FULLPLANE_YV12) {
        s->stPlant[0].u64PhyAddr = addr;
        s->stPlant[0].u8PixStride = 1;
        s->stPlant[0].u32PitchLine = w;
        s->stPlant[1].u64PhyAddr = addr + w * h;
        s->stPlant[1].u8PixStride = 2;
        s->stPlant[1].u32PitchLine = w;
    } else {
        float bpp = (fmt == IPU_COLOR_FMT_PACKED_ARGB32 || fmt == IPU_COLOR_FMT_PACKED_ABGR32 ||
                     fmt == IPU_COLOR_FMT_PACKED_BGRA32 || fmt == IPU_COLOR_FMT_PACKED_RGBA32) ? 4.0 :
                   (fmt == IPU_COLOR_FMT_PACKED_RGB24 || fmt == IPU_COLOR_FMT_PACKED_BGR24) ? 3.0 : 2.0;
        s->stPlant[0].u64PhyAddr = addr;
        s->stPlant[0].u8PixStride = (uint8_t)bpp;
        s->stPlant[0].u32PitchLine = w * (uint32_t)bpp;
    }
}

int hal_ipu_scale_crop_with_format(int queue_id,
                                    unsigned long in_phy_addr, unsigned int iw, unsigned int ih,
                                    IPU_RECT_S *in_rect, IPU_COLOR_FMT_E in_format,
                                    unsigned long out_phy_addr, unsigned int ow, unsigned int oh,
                                    IPU_COLOR_FMT_E out_format)
{
    IPU_SURFACE_S src = {0}, dst = {0};
    IPU_RECT_S out_rect;
    int ret;

    if (!hal_ipu_is_initialized() || queue_id < 0 || queue_id >= HAL_IPU_MAX_QUEUES) {
        return -1;
    }

    if (!in_rect || !iw || !ih || !ow || !oh) {
        return -1;
    }

    out_rect.s32X = out_rect.s32Y = 0;
    out_rect.u32W = ow;
    out_rect.u32H = oh;

    hal_ipu_setup_surface(&src, in_format, iw, ih, in_phy_addr);
    hal_ipu_setup_surface(&dst, out_format, ow, oh, out_phy_addr);

    if (hal_ipu_start_queue(queue_id) != 0) {
        return -1;
    }

    ret = (in_rect->u32W == out_rect.u32W && in_rect->u32H == out_rect.u32H) ?
          hal_ipu_quick_copy(queue_id, &src, in_rect, &dst, &out_rect) :
          hal_ipu_composite_resize(queue_id, &src, in_rect, &dst, &out_rect);
    
    if (hal_ipu_end_queue(queue_id, AK_TRUE, AK_TRUE) != 0 || 
        (ret = hal_ipu_wait_done(queue_id, 1000)) != AK_SUCCESS) {
        ret = -1;
    }

    return ret;
}

#endif

