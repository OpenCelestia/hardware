/**
 * @file hal_motion_detect.h
 * @brief Motion Detection HAL implementation header for chip-specific operations
 * @details This header defines types used internally by hal_motion_detect.c
 *          and is not intended for direct inclusion by higher layers.
 */

#ifndef HARDWARE_HAL_MOTION_DETECT_H
#define HARDWARE_HAL_MOTION_DETECT_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

/**
 * @brief Motion detection rectangle (HAL layer specific structure).
 */
typedef struct {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
} hal_motion_detect_box_t;

/**
 * @brief MRD handle type (opaque pointer for chip-specific MRD library).
 */
typedef void* hal_mrd_handle_t;

__END_DECLS

#endif /* HARDWARE_HAL_MOTION_DETECT_H */
