// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/display.
 *
 * Framebuffer device implementation.
 * - Implements display_device_t methods for Linux framebuffer.
 * - Provides buffer management, resolution query and display flush.
 */

#include "display.h"
#include "display_hw.h"
#include "display_error.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <linux/fb.h>

#if CONFIG_OS_LINUX
#include <linux/kd.h>
#include <sys/mman.h>
#endif

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>

#define LOG_TAG "HW_FB"
#include "log.h"

/*===========================================================================
 * Private Constants
 *===========================================================================*/

/** @brief Custom ioctl for checking LCD busy status */
#ifndef FBIOGET_CUS_LCDBUSY
#define FBIOGET_CUS_LCDBUSY     0x4680
#endif

/*===========================================================================
 * Private Macros
 *===========================================================================*/

#define FB_WIDTH(fb)            ((fb)->vinfo.xres)
#define FB_HEIGHT(fb)           ((fb)->vinfo.yres)
#define FB_BPP(fb)              ((fb)->vinfo.bits_per_pixel)
#define FB_LINE_LENGTH(fb)      ((fb)->finfo.line_length)
#define FB_TOTAL_SIZE(fb)       ((fb)->finfo.smem_len)
#define FB_SIZE(fb)             ((fb)->vinfo.yres * (fb)->finfo.line_length)
#define FB_NUMBER(fb)           (FB_TOTAL_SIZE(fb) / FB_SIZE(fb))
#define OFFLINE_INDEX(fb)       ((fb)->vinfo.reserved[0])
#define OFFLINE_VADDR(fb)       ((fb)->vaddr[OFFLINE_INDEX(fb)])
#define OFFLINE_PADDR(fb)       ((fb)->paddr[OFFLINE_INDEX(fb)])

/*===========================================================================
 * External Declarations (Platform-specific)
 *===========================================================================*/

#if CONFIG_OS_LINUX
#else
extern void *osal_fb_mmap_viraddr(int fb_len, int fb_fd);
extern int osal_fb_munmap_viraddr(void *addr, int fb_len);
#endif

/*===========================================================================
 * Private Functions
 *===========================================================================*/

#ifndef FB_LCDC_BUSY_TIMEOUT_MS
#define FB_LCDC_BUSY_TIMEOUT_MS 100
#endif

/**
 * @brief Wait for framebuffer to be available.
 * @param fb  Framebuffer info structure.
 * @return 0 if LCDC idle, -1 on timeout.
 *
 * LCDC can stick busy after a missed completion IRQ. Infinite poll froze
 * disp_flush (and then IPU/UI). Time out and skip this flip.
 */
static int fb_wait_available(fb_info_t *fb)
{
#ifdef CONFIG_OS_LINUX
    unsigned long lcdc_is_busy = 0;
    unsigned long long wait_ms = 0;
    static unsigned long long s_timeout_count;

    while (1) {
        int ioctl_ret = ioctl(fb->fd, FBIOGET_CUS_LCDBUSY, &lcdc_is_busy);
        if ((ioctl_ret == 0) && (lcdc_is_busy == 0UL))
            return 0;
        if (wait_ms >= (unsigned long long)FB_LCDC_BUSY_TIMEOUT_MS) {
            s_timeout_count++;
            /* First 10 + every 50th. */
            if (s_timeout_count <= 10ull || (s_timeout_count % 50ull) == 0ull)
                log_error("fb flush: LCDC busy timeout (%llums, busy=%lu), "
                          "skip frame cnt=%llu",
                          wait_ms, lcdc_is_busy, s_timeout_count);
            return -1;
        }
        usleep(2000);
        wait_ms += 2;
    }
#else
    (void)fb;
    return 0;
#endif
}

/**
 * @brief Get pixel format from framebuffer info.
 * @param fb  Framebuffer info structure.
 * @return Pixel format enum value.
 */
static pixel_format_t fb_get_pixel_format(fb_info_t *fb)
{
    pixel_format_t format = 0;
    struct fb_var_screeninfo *vinfo = &(fb->vinfo);

    switch (FB_BPP(fb)) {
        case 16:
            if (vinfo->blue.offset == 0 && vinfo->green.offset == 5 &&
                vinfo->red.offset == 11 && vinfo->blue.length == 5 &&
                vinfo->green.length == 6 && vinfo->red.length == 5) {
                format = SYSTEM_PIXEL_FORMAT_RGB565;
            } else if (vinfo->red.offset == 0 && vinfo->green.offset == 5 &&
                       vinfo->blue.offset == 11 && vinfo->red.length == 5 &&
                       vinfo->green.length == 6 && vinfo->blue.length == 5) {
                format = SYSTEM_PIXEL_FORMAT_BGR565;
            }
            break;

        case 24:
            if (vinfo->blue.offset == 0 && vinfo->green.offset == 8 &&
                vinfo->red.offset == 16 && vinfo->red.length == 8 &&
                vinfo->green.length == 8 && vinfo->blue.length == 8) {
                format = SYSTEM_PIXEL_FORMAT_RGB24;
            } else if (vinfo->red.offset == 0 && vinfo->green.offset == 8 &&
                       vinfo->blue.offset == 16 && vinfo->red.length == 8 &&
                       vinfo->green.length == 8 && vinfo->blue.length == 8) {
                format = SYSTEM_PIXEL_FORMAT_BGR24;
            }
            break;

        case 32:
            if (vinfo->red.offset == 0 && vinfo->green.offset == 8 &&
                vinfo->blue.offset == 16 && vinfo->red.length == 8 &&
                vinfo->green.length == 8 && vinfo->blue.length == 8) {
                format = SYSTEM_PIXEL_FORMAT_BGRA32;
            } else if (vinfo->blue.offset == 0 && vinfo->green.offset == 8 &&
                       vinfo->red.offset == 16 && vinfo->red.length == 8 &&
                       vinfo->green.length == 8 && vinfo->blue.length == 8) {
                format = SYSTEM_PIXEL_FORMAT_RGBA32;
            }
            break;

        default:
            format = SYSTEM_PIXEL_FORMAT_RGB24;
            break;
    }

    return format;
}

/**
 * @brief Get framebuffer device information.
 * @param fb  Framebuffer info structure.
 * @return 0 on success, negative error code on failure.
 */
static int fb_get_info(fb_info_t *fb)
{
    if (!fb) {
        log_error("fb_get_info invalid parameter");
        return HARDWARE_DISPLAY_ERROR_INVALID_PARAM;
    }

    if (ioctl(fb->fd, FBIOGET_FSCREENINFO, &fb->finfo) < 0) {
        log_error("failed to get finfo of fb");
        return HARDWARE_DISPLAY_ERROR_IO;
    }

    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &fb->vinfo) < 0) {
        log_error("failed to get vinfo of fb");
        return HARDWARE_DISPLAY_ERROR_IO;
    }

    fb->vinfo.xoffset = 0;
    fb->vinfo.yoffset = 0;
    fb->vinfo.yres = fb->vinfo.yres_virtual;
    fb->vinfo.xres = fb->vinfo.xres_virtual;

    log_warn("fb information:");
    log_warn("device xres=%d yres=%d", FB_WIDTH(fb), FB_HEIGHT(fb));
    log_warn("xres_virtual=%d yres_virtual=%d",
             fb->vinfo.xres_virtual, fb->vinfo.yres_virtual);
    log_warn("bits_per_pixel=%d line_length=%d",
             fb->vinfo.bits_per_pixel, fb->finfo.line_length);
    log_warn("fb_info_t: red(%d %d) green(%d %d) blue(%d %d)",
             fb->vinfo.red.offset, fb->vinfo.red.length,
             fb->vinfo.green.offset, fb->vinfo.green.length,
             fb->vinfo.blue.offset, fb->vinfo.blue.length);
    log_warn("xpanstep=%u ywrapstep=%u", fb->finfo.xpanstep, fb->finfo.ywrapstep);
    log_warn("size=%u, total size=%u, number=%u",
             FB_SIZE(fb), FB_TOTAL_SIZE(fb), (FB_TOTAL_SIZE(fb) / FB_SIZE(fb)));
    log_warn("smem_len=%u, smem_start=%x", fb->finfo.smem_len, fb->finfo.smem_start);

#if CONFIG_OS_LINUX
    fb->vaddr[0] = mmap(0, FB_TOTAL_SIZE(fb), PROT_READ | PROT_WRITE,
                        MAP_SHARED, fb->fd, 0);
#else
    fb->vaddr[0] = osal_fb_mmap_viraddr(FB_TOTAL_SIZE(fb), fb->fd);
#endif

#if CONFIG_OS_LINUX
    if (fb->vaddr[0] == MAP_FAILED)
#else
    if (fb->vaddr[0] == NULL)
#endif
    {
        log_error("map framebuffer failed.");
        fb->vaddr[0] = NULL;
        return HARDWARE_DISPLAY_ERROR_MAP_FAILED;
    }

    fb->paddr[0] = fb->finfo.smem_start;

    if (FB_NUMBER(fb) > 1) {
        fb->vaddr[1] = (void *)((uint8_t *)(fb->vaddr[0]) + FB_SIZE(fb));
        fb->paddr[1] = fb->paddr[0] + (fb->finfo.smem_len >> 1);
    }

    log_warn("fb vaddr= 0x%x paddr= 0x%x", fb->vaddr[0], fb->paddr[0]);

    if (fb->finfo.reserved[0] == 1) {
        fb->vinfo.reserved[0] = 1;
        if (ioctl(fb->fd, FBIOPUT_VSCREENINFO, &(fb->vinfo)) < 0) {
            log_error("FBIOPUT_VSCREENINFO failed!");
            return HARDWARE_DISPLAY_ERROR_OPERATION_FAILED;
        }
    }

    OFFLINE_INDEX(fb) = 0;

    return HARDWARE_DISPLAY_OK;
}

/*===========================================================================
 * Device Method Implementations
 *===========================================================================*/

/**
 * @brief Get the resolution of display device.
 * @param device  Device object.
 * @param width   Output parameter for width.
 * @param height  Output parameter for height.
 * @return 0 on success, negative error code on failure.
 */
static int hw_fb_get_resolution(struct display_device *device,
                                uint32_t *width,
                                uint32_t *height)
{
    if ((NULL == device) || (NULL == width) || (NULL == height)) {
        log_error("parameter error");
        return HARDWARE_DISPLAY_ERROR_INVALID_PARAM;
    }

    fb_info_t *fb = (fb_info_t *)(device->priv);
    if (!fb) {
        log_error("device not initialized");
        return HARDWARE_DISPLAY_ERROR_NOT_INIT;
    }

    *width = fb->vinfo.xres;
    *height = fb->vinfo.yres;

    return HARDWARE_DISPLAY_OK;
}

/**
 * @brief Get the offline buffer information of device.
 * @param device  Device object.
 * @param buffer  Output parameter for display buffer.
 * @return 0 on success, negative error code on failure.
 */
static int hw_fb_get_offline_buffer(struct display_device *device,
                                    display_buffer_t *buffer)
{
    if ((NULL == device) || (NULL == buffer)) {
        log_error("parameter error");
        return HARDWARE_DISPLAY_ERROR_INVALID_PARAM;
    }

    fb_info_t *fb = (fb_info_t *)(device->priv);
    if (!fb) {
        log_error("device not initialized");
        return HARDWARE_DISPLAY_ERROR_NOT_INIT;
    }

    buffer->size = FB_SIZE(fb);
    buffer->total_size = FB_TOTAL_SIZE(fb);
    buffer->vaddr = OFFLINE_VADDR(fb);
    buffer->paddr = OFFLINE_PADDR(fb);

    log_debug("get_offline_buffer: idx=%d, vaddr=%p, paddr=0x%lx, size=%u",
              OFFLINE_INDEX(fb), buffer->vaddr, (unsigned long)buffer->paddr, buffer->size);

    return HARDWARE_DISPLAY_OK;
}

/**
 * @brief Get pixel format of device.
 * @param device  Device object.
 * @param format  Output parameter for pixel format.
 * @return 0 on success, negative error code on failure.
 */
static int hw_fb_get_pixel_format(struct display_device *device,
                                  pixel_format_t *format)
{
    if ((NULL == device) || (NULL == format)) {
        log_error("parameter error");
        return HARDWARE_DISPLAY_ERROR_INVALID_PARAM;
    }

    fb_info_t *fb = (fb_info_t *)(device->priv);
    if (!fb) {
        log_error("device not initialized");
        return HARDWARE_DISPLAY_ERROR_NOT_INIT;
    }

    *format = fb_get_pixel_format(fb);

    return HARDWARE_DISPLAY_OK;
}

/**
 * @brief Get bits per pixel of device.
 * @param device  Device object.
 * @param bpp     Output parameter for bytes per pixel.
 * @return 0 on success, negative error code on failure.
 */
static int hw_fb_get_bpp(struct display_device *device, uint32_t *bpp)
{
    if ((NULL == device) || (NULL == bpp)) {
        log_error("parameter error");
        return HARDWARE_DISPLAY_ERROR_INVALID_PARAM;
    }

    fb_info_t *fb = (fb_info_t *)(device->priv);
    if (!fb) {
        log_error("device not initialized");
        return HARDWARE_DISPLAY_ERROR_NOT_INIT;
    }

    *bpp = FB_BPP(fb) / 8;

    return HARDWARE_DISPLAY_OK;
}

/**
 * @brief Flush display device.
 * @param device  Device object.
 * @return 0 on success, negative error code on failure.
 * @note Anyka driver uses vinfo.reserved[0] (OFFLINE_INDEX) to select display buffer.
 */
static int hw_fb_flush(struct display_device *device)
{
    if (NULL == device) {
        log_error("parameter error");
        return HARDWARE_DISPLAY_ERROR_INVALID_PARAM;
    }

    fb_info_t *fb = (fb_info_t *)(device->priv);
    if (!fb) {
        log_error("device not initialized");
        return HARDWARE_DISPLAY_ERROR_NOT_INIT;
    }

    struct fb_var_screeninfo *vinfo = &(fb->vinfo);

    /* Do not call FBIOPUT while LCDC reports busy forever — that was the hang. */
    if (fb_wait_available(fb) != 0) {
        return HARDWARE_DISPLAY_ERROR_FLUSH_FAILED;
    }

    /* Anyka driver uses reserved[0] to indicate which buffer to display.
     * OFFLINE_INDEX(fb) is vinfo->reserved[0], set it before ioctl.
     */
    log_debug("flush: OFFLINE_INDEX=%d, FB_NUMBER=%d", OFFLINE_INDEX(fb), FB_NUMBER(fb));

    if (ioctl(fb->fd, FBIOPUT_VSCREENINFO, vinfo) < 0) {
        log_error("flush fb failed");
        return HARDWARE_DISPLAY_ERROR_FLUSH_FAILED;
    }

    /* Swap offline buffer index for next write (double buffering) */
    if (FB_NUMBER(fb) > 1) {
        if (0 == OFFLINE_INDEX(fb)) {
            OFFLINE_INDEX(fb) = 1;
        } else {
            OFFLINE_INDEX(fb) = 0;
        }
    }

    return HARDWARE_DISPLAY_OK;
}

/**
 * @brief Close framebuffer device.
 * @param device  Device object.
 * @return 0 on success, negative error code on failure.
 */
static int hw_fb_close(struct hw_device_t *device)
{
    if (NULL == device) {
        log_error("parameter error");
        return HARDWARE_DISPLAY_ERROR_INVALID_PARAM;
    }

    struct display_device *dev = (struct display_device *)device;
    fb_info_t *fb = (fb_info_t *)(dev->priv);
    if (!fb) {
        log_error("device not initialized");
        return HARDWARE_DISPLAY_ERROR_NOT_INIT;
    }

    if (fb->fd < 0) {
        log_error("device hasn't been opened");
        return HARDWARE_DISPLAY_ERROR_INVALID_STATE;
    }

#if CONFIG_OS_LINUX
    if (fb->vaddr[0]) {
        munmap(fb->vaddr[0], FB_TOTAL_SIZE(fb));
        fb->vaddr[0] = NULL;
    }
#else
    if (fb->vaddr[0]) {
        osal_fb_munmap_viraddr(fb->vaddr[0], FB_TOTAL_SIZE(fb));
        fb->vaddr[0] = NULL;
    }
#endif

    close(fb->fd);
    fb->fd = -1;

    free(fb);
    free(device);

    return HARDWARE_DISPLAY_OK;
}

/**
 * @brief Get framebuffer device path from instance name.
 * @param name  Instance name (e.g., "fb", "fb0", "/dev/fb0").
 * @return Device path string.
 */
static const char *hw_fb_get_device_path(const char *name)
{
    /* If already a full path, use as-is */
    if (name[0] == '/') {
        return name;
    }
    
    /* Map instance name to device path */
    if (strcmp(name, DISPLAY_DEVICE_INSTANCE_FB) == 0 ||
        strcmp(name, "fb0") == 0) {
        return "/dev/fb0";
    } else if (strcmp(name, "fb1") == 0) {
        return "/dev/fb1";
    }
    
    /* Default to /dev/fb0 */
    return "/dev/fb0";
}

/**
 * @brief Open framebuffer device.
 * @param module  Module object.
 * @param name    Device instance name (e.g., "fb") or path (e.g., "/dev/fb0").
 * @param device  Output parameter for device pointer.
 * @return 0 on success, negative error code on failure.
 */
static int hw_fb_open(const hw_module_t *module,
                      const char *name,
                      hw_device_t **device)
{
    if ((NULL == module) || (NULL == name) || (NULL == device)) {
        log_error("parameter error");
        return HARDWARE_DISPLAY_ERROR_INVALID_PARAM;
    }

    *device = NULL;

    const char *dev_path = hw_fb_get_device_path(name);
    
    if (access(dev_path, F_OK) != 0) {
        log_error("device %s not exist", dev_path);
        return HARDWARE_DISPLAY_ERROR_NOT_FOUND;
    }

    int fd = open(dev_path, O_RDWR);
    if (fd < 0) {
        log_error("open %s failed", dev_path);
        return HARDWARE_DISPLAY_ERROR_IO;
    }

    struct display_device *dev = calloc(1, sizeof(struct display_device));
    if (!dev) {
        log_error("failed to malloc for device");
        close(fd);
        return HARDWARE_DISPLAY_ERROR_MEMORY;
    }

    fb_info_t *fb = calloc(1, sizeof(fb_info_t));
    if (!fb) {
        log_error("failed to malloc for fb");
        free(dev);
        close(fd);
        return HARDWARE_DISPLAY_ERROR_MEMORY;
    }

    fb->fd = fd;

    int ret = fb_get_info(fb);
    if (ret != HARDWARE_DISPLAY_OK) {
        log_error("failed to get information of fb");
#if CONFIG_OS_LINUX
        if (fb->vaddr[0]) {
            munmap(fb->vaddr[0], FB_TOTAL_SIZE(fb));
            fb->vaddr[0] = NULL;
        }
#else
        if (fb->vaddr[0]) {
            osal_fb_munmap_viraddr(fb->vaddr[0], FB_TOTAL_SIZE(fb));
            fb->vaddr[0] = NULL;
        }
#endif
        free(fb);
        free(dev);
        close(fd);
        return ret;
    }

    /* Initialize device structure */
    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = DISPLAY_DEVICE_FB_API_VERSION_1_0;
    dev->common.module = (struct hw_module_t *)module;
    dev->common.close = hw_fb_close;

    dev->get_resolution = hw_fb_get_resolution;
    dev->get_offline_buffer = hw_fb_get_offline_buffer;
    dev->get_pixel_format = hw_fb_get_pixel_format;
    dev->get_bpp = hw_fb_get_bpp;
    dev->flush = hw_fb_flush;

    dev->priv = (void *)fb;

    *device = &dev->common;

    return HARDWARE_DISPLAY_OK;
}

/*===========================================================================
 * Module Definition
 *===========================================================================*/

static struct hw_module_methods_t hw_fb_module_methods = {
    .open = hw_fb_open,
};

static struct display_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = DISPLAY_DEVICE_FB_API_VERSION_1_0,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = "display.fb",  /* Module ID for hw_get_module_by_class("display", "fb") */
        .name = "FB Module",
        .author = "Anyka",
        .methods = &hw_fb_module_methods,
    },
};

/*===========================================================================
 * Public Functions
 *===========================================================================*/

/**
 * @brief Initialize framebuffer module.
 * @details Registers the framebuffer module with the hardware module system.
 * @return 0 on success, negative error code on failure.
 */
int hw_fb_module_init(void)
{
    int ret = hw_register_module(&(HAL_MODULE_INFO_SYM.common));
    return (ret == 0) ? HARDWARE_DISPLAY_OK : HARDWARE_DISPLAY_ERROR_OPERATION_FAILED;
}

