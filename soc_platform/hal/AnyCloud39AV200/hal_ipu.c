// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/soc_platform.
 *
 * IPU HAL implementation for AnyCloud39AV200 platform.
 * - Provides unified IPU management with reference counting and queue allocation.
 * - Implements IPU operations for image processing.
 */

#define LOG_TAG "HAL IPU"

#include "ak_thread.h"
#include "hal_ipu.h"
#include "hal_ipu_api.h"

#include "ak_common.h"
#include "ak_ipu.h"
#include "log.h"
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*===========================================================================
 * Private Variables
 *===========================================================================*/

static int s_ipu_ref_count = 0;
static bool s_ipu_initialized = false;
static pthread_mutex_t s_ipu_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_ipu_tx_state_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Serialize only EndQue -> WaitForDone stage across all queues. */
static pthread_mutex_t s_ipu_end_wait_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool s_ipu_end_wait_locked = false;
static pthread_t s_ipu_end_wait_owner;

static bool s_queue_allocated[HAL_IPU_MAX_QUEUES] = {false};
static pthread_mutex_t s_queue_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Serialize the whole StartQue->...->WaitForDone sequence per queue, so that
 * multiple producers sharing one queue (e.g. LVGL draw + disp_flush on queue 0,
 * or camera AI + rotate on queue 1) cannot interleave their command-buffer writes.
 * Lock is taken in hal_ipu_start_queue and released in hal_ipu_wait_done. */
static pthread_mutex_t s_queue_op_mutex[HAL_IPU_MAX_QUEUES] = {
    PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER
};
static int s_queue_op_held[HAL_IPU_MAX_QUEUES];
static pthread_t s_queue_op_holder[HAL_IPU_MAX_QUEUES];

/* Soft in-flight flag: set in start_queue, cleared in wait_done. */
static volatile int s_queue_inflight[HAL_IPU_MAX_QUEUES];

static void hal_ipu_end_wait_lock(void)
{
    pthread_mutex_lock(&s_ipu_end_wait_mutex);
    pthread_mutex_lock(&s_ipu_tx_state_mutex);
    s_ipu_end_wait_locked = true;
    s_ipu_end_wait_owner = pthread_self();
    pthread_mutex_unlock(&s_ipu_tx_state_mutex);
}

static void hal_ipu_end_wait_unlock_if_owner(void)
{
    bool need_unlock = false;

    pthread_mutex_lock(&s_ipu_tx_state_mutex);
    if (s_ipu_end_wait_locked && pthread_equal(s_ipu_end_wait_owner, pthread_self())) {
        s_ipu_end_wait_locked = false;
        need_unlock = true;
    }
    pthread_mutex_unlock(&s_ipu_tx_state_mutex);

    if (need_unlock) {
        pthread_mutex_unlock(&s_ipu_end_wait_mutex);
    }
}

static void hal_ipu_queue_op_unlock_if_holder(int queue_id)
{
    if (queue_id < 0 || queue_id >= HAL_IPU_MAX_QUEUES)
        return;
    if (s_queue_op_held[queue_id] &&
        pthread_equal(s_queue_op_holder[queue_id], pthread_self())) {
        s_queue_op_held[queue_id] = 0;
        pthread_mutex_unlock(&s_queue_op_mutex[queue_id]);
    }
}

/*===========================================================================
 * Reference Counting Management
 *===========================================================================*/

/*
 * system("wpa_supplicant -B") inherits ipc_main fds. Without CLOEXEC, wpa keeps
 * /dev/ipu open after ipc_main exits → next AK_MPI_IPU_Open gets Resource busy.
 */
static void hal_ipu_mark_dev_fds_cloexec(void)
{
    DIR *dir;
    struct dirent *ent;
    char path[64];
    char link[128];
    ssize_t n;
    int fd;
    int flags;

    dir = opendir("/proc/self/fd");
    if (!dir)
        return;

    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9')
            continue;
        fd = atoi(ent->d_name);
        if (fd < 3)
            continue;
        snprintf(path, sizeof(path), "/proc/self/fd/%s", ent->d_name);
        n = readlink(path, link, sizeof(link) - 1);
        if (n <= 0)
            continue;
        link[n] = '\0';
        if (strcmp(link, "/dev/ipu") != 0)
            continue;
        flags = fcntl(fd, F_GETFD);
        if (flags < 0)
            continue;
        (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
    closedir(dir);
}

int hal_ipu_open(void)
{
    int attempt;
    const int max_attempts = 20; /* ~2s: covers local_restart race after previous ipc exit */

    pthread_mutex_lock(&s_ipu_mutex);

    if (s_ipu_ref_count == 0) {
        IPU_ATTR_S ipu_attr = {0};
        ipu_attr.au32QueSize[0] = HAL_IPU_QUEUE_SIZE;
        ipu_attr.au32QueSize[1] = HAL_IPU_QUEUE_SIZE;
        ipu_attr.u32RespSize = HAL_IPU_RESP_SIZE;

        for (attempt = 0; attempt < max_attempts; attempt++) {
            if (AK_MPI_IPU_Open(&ipu_attr) == AK_SUCCESS) {
                s_ipu_initialized = true;
                pthread_mutex_unlock(&s_ipu_mutex);
                hal_ipu_mark_dev_fds_cloexec();
                pthread_mutex_lock(&s_ipu_mutex);
                break;
            }
            pthread_mutex_unlock(&s_ipu_mutex);
            if (attempt + 1 >= max_attempts) {
                log_error("hal_ipu_open: AK_MPI_IPU_Open failed after %d attempts",
                          max_attempts);
                return -1;
            }
            usleep(100 * 1000);
            pthread_mutex_lock(&s_ipu_mutex);
            if (s_ipu_ref_count > 0 && s_ipu_initialized)
                break;
        }
        if (!s_ipu_initialized) {
            pthread_mutex_unlock(&s_ipu_mutex);
            return -1;
        }
    }

    s_ipu_ref_count++;
    pthread_mutex_unlock(&s_ipu_mutex);
    return 0;
}

int hal_ipu_close(void)
{
    pthread_mutex_lock(&s_ipu_mutex);

    if (s_ipu_ref_count <= 0) {
        pthread_mutex_unlock(&s_ipu_mutex);
        return 0;
    }

    s_ipu_ref_count--;
    if (s_ipu_ref_count == 0 && s_ipu_initialized) {
        AK_MPI_IPU_Close();
        s_ipu_initialized = false;
    }

    pthread_mutex_unlock(&s_ipu_mutex);
    return 0;
}

int hal_ipu_force_close(void)
{
    int q;

    pthread_mutex_lock(&s_ipu_mutex);
    if (s_ipu_initialized) {
        AK_MPI_IPU_Close();
        s_ipu_initialized = false;
        log_info("hal_ipu_force_close: AK_MPI_IPU_Close (was ref=%d)", s_ipu_ref_count);
    }
    s_ipu_ref_count = 0;
    pthread_mutex_unlock(&s_ipu_mutex);

    pthread_mutex_lock(&s_queue_mutex);
    for (q = 0; q < HAL_IPU_MAX_QUEUES; q++) {
        s_queue_allocated[q] = false;
    }
    pthread_mutex_unlock(&s_queue_mutex);
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
            log_info("hal_ipu_alloc_queue: got queue_id=%d", *queue_id_out);
            return 0;
        }
    }
    
    pthread_mutex_unlock(&s_queue_mutex);
    log_error("hal_ipu_alloc_queue: no free queue (all in use)");
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
    log_info("hal_ipu_free_queue: queue_id=%d", queue_id);
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
        log_info("hal_ipu_get_default_queue: module=%s -> queue_id=%d (preferred slot)",
                 module_name, *queue_id_out);
        return 0;
    }
    pthread_mutex_unlock(&s_queue_mutex);
    
    // Preferred queue not available, try to allocate any available queue
    log_info("hal_ipu_get_default_queue: module=%s preferred=%d busy, try alloc first free",
             module_name, preferred_queue);
    return hal_ipu_alloc_queue(queue_id_out);
}

/*===========================================================================
 * IPU Operations
 *===========================================================================*/

int hal_ipu_start_queue(int queue_id)
{
    if (!hal_ipu_is_initialized()) {
        log_error("hal_ipu_start_queue: IPU not initialized (prior _exit without hal_ipu_close?)");
        return -1;
    }
    
    if (queue_id < 0 || queue_id >= HAL_IPU_MAX_QUEUES) {
        return -1;
    }

    /* Whole StartQue..WaitForDone must be exclusive per queue. */
    pthread_mutex_lock(&s_queue_op_mutex[queue_id]);
    s_queue_op_held[queue_id] = 1;
    s_queue_op_holder[queue_id] = pthread_self();
    s_queue_inflight[queue_id] = 1;

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

    hal_ipu_end_wait_lock();
    AK_MPI_IPU_EndQue(queue_id, sync ? AK_TRUE : AK_FALSE, wait ? AK_TRUE : AK_FALSE);
    return 0;
}

int hal_ipu_wait_done(int queue_id, int timeout_ms)
{
    int ret = -1;

    if (!hal_ipu_is_initialized()) {
        if (queue_id >= 0 && queue_id < HAL_IPU_MAX_QUEUES)
            s_queue_inflight[queue_id] = 0;
        hal_ipu_queue_op_unlock_if_holder(queue_id);
        hal_ipu_end_wait_unlock_if_owner();
        return -1;
    }
    
    if (queue_id < 0 || queue_id >= HAL_IPU_MAX_QUEUES) {
        hal_ipu_end_wait_unlock_if_owner();
        return -1;
    }

    ret = AK_MPI_IPU_WaitForDone(queue_id, timeout_ms);
    if (ret != AK_SUCCESS) {
        log_error("WaitForDone failed q=%d ret=%d timeout_ms=%d",
                  queue_id, ret, timeout_ms);
    }
    s_queue_inflight[queue_id] = 0;
    hal_ipu_queue_op_unlock_if_holder(queue_id);
    hal_ipu_end_wait_unlock_if_owner();
    return ret;
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

/*===========================================================================
 * Surface Setup Helper Functions
 *===========================================================================*/

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
        log_warn("hal_ipu_scale_crop_with_format: not ready or bad queue (init=%d, queue_id=%d)",
                 hal_ipu_is_initialized() ? 1 : 0, queue_id);
        return -1;
    }

    if (!in_rect || !iw || !ih || !ow || !oh) {
        log_warn("hal_ipu_scale_crop_with_format: invalid size or in_rect (queue_id=%d)", queue_id);
        return -1;
    }

    out_rect.s32X = out_rect.s32Y = 0;
    out_rect.u32W = ow;
    out_rect.u32H = oh;

    hal_ipu_setup_surface(&src, in_format, iw, ih, in_phy_addr);
    hal_ipu_setup_surface(&dst, out_format, ow, oh, out_phy_addr);

    /* Serialization is in hal_ipu_start_queue / wait_done (s_queue_op_mutex). */
    if (hal_ipu_start_queue(queue_id) != 0) {
        log_warn("hal_ipu_scale_crop_with_format: hal_ipu_start_queue failed (queue_id=%d)", queue_id);
        return -1;
    }

    ret = (in_rect->u32W == out_rect.u32W && in_rect->u32H == out_rect.u32H) ?
          hal_ipu_quick_copy(queue_id, &src, in_rect, &dst, &out_rect) :
          hal_ipu_composite_resize(queue_id, &src, in_rect, &dst, &out_rect);
    if (ret != 0) {
        log_warn("hal_ipu_scale_crop_with_format: copy/resize returned %d (queue_id=%d in %ux%u -> out %ux%u in_fmt=%d out_fmt=%d in_phy=0x%lx out_phy=0x%lx in_rect %u,%u %ux%u)",
                 ret, queue_id, iw, ih, ow, oh, in_format, out_format, in_phy_addr, out_phy_addr,
                 in_rect->s32X, in_rect->s32Y, in_rect->u32W, in_rect->u32H);
    }
    
    if (hal_ipu_end_queue(queue_id, AK_TRUE, AK_TRUE) != 0) {
        log_warn("hal_ipu_scale_crop_with_format: end_queue failed (queue_id=%d, copy_resize=%d)", queue_id, ret);
        (void)hal_ipu_wait_done(queue_id, 1000);
        return -1;
    }
    if ((ret = hal_ipu_wait_done(queue_id, 1000)) != AK_SUCCESS) {
        log_warn("hal_ipu_scale_crop_with_format: wait_done fail queue_id=%d ret=%d in %ux%u->%ux%u in_phy=0x%lx out_phy=0x%lx in_fmt=%d out_fmt=%d in_rect %u,%u %ux%u)",
                 queue_id, ret, iw, ih, ow, oh, in_phy_addr, out_phy_addr, in_format, out_format,
                 in_rect->s32X, in_rect->s32Y, in_rect->u32W, in_rect->u32H);
        return -1;
    }

    return ret;
}

int hal_ipu_rotate_with_format(int queue_id,
                               unsigned long in_phy_addr, unsigned int iw, unsigned int ih,
                               IPU_COLOR_FMT_E in_format,
                               unsigned long out_phy_addr, unsigned int ow, unsigned int oh,
                               IPU_COLOR_FMT_E out_format,
                               int rotation_degrees)
{
    IPU_SURFACE_S src = {0}, dst = {0};
    IPU_RECT_S in_rect, out_rect;
    IPU_DIRECTION_E dir;
    int ret;

    if (!hal_ipu_is_initialized() || queue_id < 0 || queue_id >= HAL_IPU_MAX_QUEUES) {
        log_warn("hal_ipu_rotate_with_format: not ready or bad queue (init=%d, queue_id=%d)",
                 hal_ipu_is_initialized() ? 1 : 0, queue_id);
        return -1;
    }

    if (!iw || !ih || !ow || !oh) {
        log_warn("hal_ipu_rotate_with_format: invalid size (queue_id=%d)", queue_id);
        return -1;
    }

    switch (rotation_degrees) {
        case 90:  dir = IPU_DIRECTION_ROTATE_90;  break;
        case 180: dir = IPU_DIRECTION_ROTATE_180; break;
        case 270: dir = IPU_DIRECTION_ROTATE_270; break;
        default:
            log_warn("hal_ipu_rotate_with_format: unsupported rotation %d", rotation_degrees);
            return -1;
    }

    in_rect.s32X = in_rect.s32Y = 0;
    in_rect.u32W = iw;
    in_rect.u32H = ih;
    out_rect.s32X = out_rect.s32Y = 0;
    out_rect.u32W = ow;
    out_rect.u32H = oh;

    hal_ipu_setup_surface(&src, in_format, iw, ih, in_phy_addr);
    hal_ipu_setup_surface(&dst, out_format, ow, oh, out_phy_addr);
    /* Rotation is applied as a preprocessing direction on the source layer. */
    src.enDirection = dir;

    /* Serialization is in hal_ipu_start_queue / wait_done (s_queue_op_mutex). */
    if (hal_ipu_start_queue(queue_id) != 0) {
        log_warn("hal_ipu_rotate_with_format: start_queue failed (queue_id=%d)", queue_id);
        return -1;
    }

    ret = hal_ipu_composite_resize(queue_id, &src, &in_rect, &dst, &out_rect);
    if (ret != 0) {
        log_warn("hal_ipu_rotate_with_format: composite_resize=%d (queue_id=%d rot=%d in %ux%u->out %ux%u in_fmt=%d out_fmt=%d)",
                 ret, queue_id, rotation_degrees, iw, ih, ow, oh, in_format, out_format);
    }

    if (hal_ipu_end_queue(queue_id, AK_TRUE, AK_TRUE) != 0) {
        log_warn("hal_ipu_rotate_with_format: end_queue failed (queue_id=%d)", queue_id);
        (void)hal_ipu_wait_done(queue_id, 1000);
        return -1;
    }
    if ((ret = hal_ipu_wait_done(queue_id, 1000)) != AK_SUCCESS) {
        log_warn("hal_ipu_rotate_with_format: wait_done fail queue_id=%d ret=%d rot=%d in %ux%u->%ux%u",
                 queue_id, ret, rotation_degrees, iw, ih, ow, oh);
        return -1;
    }

    return ret;
}

