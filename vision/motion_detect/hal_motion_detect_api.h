/**
 * @file hal_motion_detect_api.h
 * @brief Motion detect HAL unified interface (chip-agnostic, used by motion_detect_hw)
 *
 * This file defines only function interfaces for motion detection HAL layer.
 * All type definitions are in motion_detect_hw.h (for logic layer) or hal_motion_detect.h (for HAL internal).
 * Upper layer (`motion_detect_hw.c`) should only depend on this file, and not
 * on chip-specific HAL headers directly.
 */

#ifndef HARDWARE_HAL_MOTION_DETECT_API_H
#define HARDWARE_HAL_MOTION_DETECT_API_H

#include "motion_detect_hw.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize motion detection HAL environment.
 *
 * @return 0 on success, non-zero on failure.
 */
int hal_motion_detect_init(void);

/**
 * @brief Deinitialize motion detection HAL environment.
 *
 * @return 0 on success, non-zero on failure.
 */
int hal_motion_detect_deinit(void);

/**
 * @brief Get current day/night status.
 *
 * @return 0 for day, 1 for night, negative on error.
 */
int hal_motion_detect_get_day_night_status(void);

/**
 * @brief Set day/night status (called by HW layer).
 *
 * @param status 0 for day, 1 for night, negative for unknown/error.
 * @return 0 on success, non-zero on failure.
 * @note HW layer should call this after getting status from framework layer.
 *       HAL layer will update MRD mode accordingly.
 */
int hal_motion_detect_set_day_night_status(int status);

/**
 * @brief Get ISP MD vertical dimension.
 *
 * @param vsize Output vertical line count, must not be NULL.
 * @return 0 on success, non-zero on failure.
 */
int hal_motion_detect_get_vsize(int *vsize);

/**
 * @brief Run MRD algorithm (one-shot interface).
 *
 * @param md_stat   ISP MD statistics 2D array (unsigned short md_stat[][64]).
 * @param vsize     vertical dimension.
 * @param boxes     output boxes array (void* to avoid type dependency).
 * @param max_boxes capacity of boxes array.
 * @return number of detected boxes (>0), 0 for no motion, <0 on error.
 */
int hal_motion_detect_process_mrd(unsigned short md_stat[][64],
                                  int vsize,
                                  void *boxes,
                                  int max_boxes);

/**
 * @brief Create an MRD handle.
 *
 * @return non-NULL handle on success, NULL on failure.
 */
void *hal_mrd_init(void);

/**
 * @brief Destroy an MRD handle.
 *
 * @param handle MRD handle.
 * @return 0 on success, non-zero on failure.
 */
int hal_mrd_destroy(void *handle);

/**
 * @brief Set MRD filter parameters.
 *
 * @param handle    MRD handle.
 * @param flt_big   filter for large motion regions.
 * @param flt_small filter for small motion regions.
 * @return 0 on success, non-zero on failure.
 */
int hal_mrd_set_filters(void *handle,
                        unsigned short flt_big,
                        unsigned short flt_small);

/**
 * @brief Set MRD motion region size thresholds.
 *
 * @param handle   MRD handle.
 * @param size_min minimum motion region size.
 * @param size_max maximum motion region size.
 * @return 0 on success, non-zero on failure.
 */
int hal_mrd_set_motion_region_size(void *handle,
                                   int size_min,
                                   int size_max);

/**
 * @brief Set MRD working mode.
 *
 * @param handle MRD handle.
 * @param mode   mode value (semantics defined by implementation).
 * @return 0 on success, non-zero on failure.
 */
int hal_mrd_set_mode(void *handle, unsigned char mode);

/**
 * @brief Run MRD algorithm (handle-style interface).
 *
 * @param handle MRD handle.
 * @param md_stat ISP MD statistics 2D array.
 * @param len     vertical line count.
 * @param boxes   output boxes array (void* to avoid type dependency), must not be NULL.
 * @return number of detected boxes (>=0), negative on error.
 */
int hal_mrd(void *handle,
            unsigned short md_stat[][64],
            int len,
            void *boxes);

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_HAL_MOTION_DETECT_API_H */


