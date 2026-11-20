// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/soc_platform.
 *
 * IPU HAL API for platform abstraction.
 * - Provides unified IPU management for camera and GPU modules.
 * - Only compiled when CONFIG_HW_CAMERA or CONFIG_HW_GPU is enabled.
 */

#ifndef HARDWARE_HAL_IPU_API_H
#define HARDWARE_HAL_IPU_API_H

#include <stdbool.h>
#include <stdint.h>
#include "ak_common.h"
#if defined(__has_include)
#if __has_include("ak_thread.h")
#include "ak_thread.h"  /* AK_RWLOCK_T ??ak_ipu.h uses it without including ak_thread.h */
#endif
#if __has_include("ak_ipu.h")
#include "ak_ipu.h"
#endif
#else
#include "ak_ipu.h"
#endif

#if defined(__has_include)
#if __has_include("ak_ipu.h")
#define HAL_HAS_AK_IPU 1
#else
#define HAL_HAS_AK_IPU 0
#endif
#else
#include "ak_ipu.h"
#define HAL_HAS_AK_IPU 1
#endif

/* Fallback IPU types for SoCs without a real IPU HAL.
 * Sky39AV130 has an ak_ipu.h (kernel-side, no IPU types) on the userspace
 * include path, so __has_include alone cannot decide: gate on the chip macro
 * as well. SoCs whose ak_ipu.h defines these typedefs are unaffected. */
#if !HAL_HAS_AK_IPU || defined(__CHIP_AK3918AV130_SERIES)
typedef int IPU_COLOR_FMT_E;
typedef int IPU_COLOR_TYPE_E;
typedef int IPU_ALPHA_CHN_E;
typedef struct { int s32X; int s32Y; unsigned int u32W; unsigned int u32H; } IPU_RECT_S;
typedef struct { int dummy; } IPU_SURFACE_S;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Open IPU device (reference counted).
 * @return 0 on success, negative on error
 */
int hal_ipu_open(void);

/**
 * @brief Close IPU device (reference counted).
 * @return 0 on success, negative on error
 */
int hal_ipu_close(void);

/**
 * @brief Force-release IPU regardless of reference count.
 * @details Use on process graceful teardown / restart so AK_MPI_IPU_Close runs
 *          even if some caller leaked open refs (avoids Resource busy on reopen).
 * @return 0 on success
 */
int hal_ipu_force_close(void);

/**
 * @brief Check if IPU is initialized
 * @return true if IPU is initialized, false otherwise
 */
bool hal_ipu_is_initialized(void);

/**
 * @brief Allocate an IPU queue ID
 * @details IPU supports multiple queues (typically 2: 0 and 1).
 *          This function allocates an available queue ID.
 * @param queue_id_out Output pointer to receive queue ID (0 or 1)
 * @return 0 on success, negative on error (e.g., no available queue)
 */
int hal_ipu_alloc_queue(int *queue_id_out);

/**
 * @brief Release an IPU queue ID
 * @param queue_id Queue ID to release (must be 0 or 1)
 * @return 0 on success, negative on error
 */
int hal_ipu_free_queue(int queue_id);

/**
 * @brief Get default queue ID for a module
 * @details Provides default queue allocation:
 *          - GPU: queue 0
 *          - Camera: queue 1 (or from config)
 * @param module_name Module name ("gpu" or "camera")
 * @param queue_id_out Output pointer to receive queue ID
 * @return 0 on success, negative on error
 */
int hal_ipu_get_default_queue(const char *module_name, int *queue_id_out);

/**
 * @brief Start IPU queue
 * @param queue_id Queue ID (0 or 1)
 * @return 0 on success, negative on error
 */
int hal_ipu_start_queue(int queue_id);

/**
 * @brief End IPU queue
 * @param queue_id Queue ID (0 or 1)
 * @param sync Whether to sync
 * @param wait Whether to wait
 * @return 0 on success, negative on error
 */
int hal_ipu_end_queue(int queue_id, bool sync, bool wait);

/**
 * @brief Wait for IPU queue done
 * @param queue_id Queue ID (0 or 1)
 * @param timeout_ms Timeout in milliseconds
 * @return 0 on success, negative on error
 */
int hal_ipu_wait_done(int queue_id, int timeout_ms);

/**
 * @brief Get IPU color type from format
 * @param fmt Color format
 * @return IPU color type
 */
IPU_COLOR_TYPE_E hal_ipu_get_color_type(IPU_COLOR_FMT_E fmt);

/**
 * @brief Get IPU alpha channel from format
 * @param fmt Color format
 * @return IPU alpha channel type
 */
IPU_ALPHA_CHN_E hal_ipu_get_alpha_channel(IPU_COLOR_FMT_E fmt);

/**
 * @brief Setup IPU surface for given format
 * @param surface Output surface structure to fill
 * @param format Color format
 * @param width Surface width
 * @param height Surface height
 * @param phy_addr Physical address of the buffer
 */
void hal_ipu_setup_surface(IPU_SURFACE_S *surface, IPU_COLOR_FMT_E format,
                           unsigned int width, unsigned int height, unsigned long phy_addr);

/**
 * @brief IPU QuickCopy operation
 * @param queue_id Queue ID
 * @param src Source surface
 * @param src_rect Source rectangle
 * @param dst Destination surface
 * @param dst_rect Destination rectangle
 * @return 0 on success, negative on error
 */
int hal_ipu_quick_copy(int queue_id,
                       IPU_SURFACE_S *src, IPU_RECT_S *src_rect,
                       IPU_SURFACE_S *dst, IPU_RECT_S *dst_rect);

/**
 * @brief IPU CompositeResize operation
 * @param queue_id Queue ID
 * @param src Source surface
 * @param src_rect Source rectangle
 * @param dst Destination surface
 * @param dst_rect Destination rectangle
 * @return 0 on success, negative on error
 */
int hal_ipu_composite_resize(int queue_id,
                             IPU_SURFACE_S *src, IPU_RECT_S *src_rect,
                             IPU_SURFACE_S *dst, IPU_RECT_S *dst_rect);

/**
 * @brief IPU scale and crop with format conversion (generic interface)
 * @param queue_id Queue ID
 * @param in_phy_addr Input buffer physical address
 * @param iw Input width
 * @param ih Input height
 * @param in_rect Input rectangle (crop region)
 * @param in_format Input color format
 * @param out_phy_addr Output buffer physical address
 * @param ow Output width
 * @param oh Output height
 * @param out_format Output color format
 * @return 0 on success, negative on error
 */
int hal_ipu_scale_crop_with_format(int queue_id,
                                    unsigned long in_phy_addr, unsigned int iw, unsigned int ih,
                                    IPU_RECT_S *in_rect, IPU_COLOR_FMT_E in_format,
                                    unsigned long out_phy_addr, unsigned int ow, unsigned int oh,
                                    IPU_COLOR_FMT_E out_format);

/**
 * @brief IPU rotate (with optional resize and format conversion).
 * @details Rotates the full input image clockwise by 90/180/270 degrees using the
 *          IPU source-layer direction, scaling the rotated result into ow x oh.
 *          - Clean rotation (no distortion): pass ow=ih, oh=iw for 90/270.
 *          - Rotate into a fixed size (e.g. an NPU input that must stay WxH):
 *            pass ow=iw, oh=ih; the rotated content is resized to fit.
 *          The whole StartQue->WaitForDone sequence is serialized per queue.
 * @param queue_id Queue ID
 * @param in_phy_addr Input buffer physical address
 * @param iw Input width
 * @param ih Input height
 * @param in_format Input color format
 * @param out_phy_addr Output buffer physical address
 * @param ow Output width
 * @param oh Output height
 * @param out_format Output color format
 * @param rotation_degrees 90, 180 or 270 (clockwise)
 * @return 0 on success, negative on error
 */
int hal_ipu_rotate_with_format(int queue_id,
                               unsigned long in_phy_addr, unsigned int iw, unsigned int ih,
                               IPU_COLOR_FMT_E in_format,
                               unsigned long out_phy_addr, unsigned int ow, unsigned int oh,
                               IPU_COLOR_FMT_E out_format,
                               int rotation_degrees);

/**
 * @brief IPU Blend operation
 * @deprecated This function is not fully implemented due to complex BlendOpt requirements.
 *             Modules should call AK_MPI_IPU_Blend directly but use hal_ipu_start_queue/end_queue
 *             for queue management.
 * @param queue_id Queue ID
 * @param bg Background surface (can be NULL)
 * @param bg_rect Background rectangle (can be NULL)
 * @param fg Foreground surface
 * @param fg_rect Foreground rectangle
 * @param dst Destination surface
 * @param dst_rect Destination rectangle
 * @return 0 on success, negative on error
 * @note For blend operations, use:
 *       hal_ipu_start_queue(queue_id);
 *       AK_MPI_IPU_Blend(queue_id, bg, bg_rect, fg, fg_rect, dst, dst_rect, &BlendOpt);
 *       hal_ipu_end_queue(queue_id, sync, wait);
 *       hal_ipu_wait_done(queue_id, timeout);
 */
int hal_ipu_blend(int queue_id,
                  IPU_SURFACE_S *bg, IPU_RECT_S *bg_rect,
                  IPU_SURFACE_S *fg, IPU_RECT_S *fg_rect,
                  IPU_SURFACE_S *dst, IPU_RECT_S *dst_rect);

/**
 * @brief IPU QuickFill operation
 * @param queue_id Queue ID
 * @param dst Destination surface
 * @param dst_rect Destination rectangle
 * @param color Fill color
 * @return 0 on success, negative on error
 */
int hal_ipu_quick_fill(int queue_id,
                       IPU_SURFACE_S *dst, IPU_RECT_S *dst_rect,
                       uint32_t color);

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_HAL_IPU_API_H */

