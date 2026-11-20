// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/display.
 *
 * Display module internal header.
 * - Internal definitions for display module implementation.
 * - Defines framebuffer information structures and internal APIs.
 */

#ifndef HARDWARE_DISPLAY_HW_H
#define HARDWARE_DISPLAY_HW_H

#include <stdint.h>
#include <stdbool.h>
#include <linux/fb.h>

#include "display.h"
#include "display_error.h"

__BEGIN_DECLS

/*===========================================================================
 * Internal Constants
 *===========================================================================*/

/** @brief Maximum number of framebuffers */
#define FB_NUM_MAX              2

/** @brief Framebuffer device name */
#define FB_DEVICE_NAME          "/dev/fb0"

/* must be consistent with the command definition in the device driver */
#ifndef FBIOGET_CUS_LCDBUSY
#define FBIOGET_CUS_LCDBUSY     0x4680
#endif

/*===========================================================================
 * Internal Type Definitions
 *===========================================================================*/

/**
 * @brief Framebuffer information structure.
 * @details Contains framebuffer device state and buffer information.
 */
typedef struct fb_info {
    int fd;                             /**< Framebuffer device file descriptor */
    struct fb_fix_screeninfo finfo;     /**< Fixed screen information */
    struct fb_var_screeninfo vinfo;     /**< Variable screen information */
    void *vaddr[FB_NUM_MAX];            /**< Virtual addresses of framebuffers */
    uint64_t paddr[FB_NUM_MAX];         /**< Physical addresses of framebuffers */
} fb_info_t;

/*===========================================================================
 * Internal Macros
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
 * Internal Function Declarations
 *===========================================================================*/

/**
 * @brief Initialize framebuffer module.
 * @details Registers the framebuffer module with the hardware module system.
 * @return 0 on success, negative error code on failure.
 */
int hw_fb_module_init(void);

__END_DECLS

#endif /* HARDWARE_DISPLAY_HW_H */

