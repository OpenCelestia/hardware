// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Internal VCM actuator helpers for AnyCloud39AV200 camera HAL.
 * Not part of the public hal_camera_api.h contract.
 */

#ifndef HARDWARE_HAL_CAMERA_VCM_ANYCLOUD39AV200_H
#define HARDWARE_HAL_CAMERA_VCM_ANYCLOUD39AV200_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hal_camera_vcm_ctx hal_camera_vcm_ctx_t;

/**
 * @brief Probe VCM subdev and open userspace control node.
 * @return Context on success, NULL on failure (AF treated as unsupported).
 */
hal_camera_vcm_ctx_t *hal_camera_vcm_open(const char *vcm_type,
                                          const char *subdev_path_override,
                                          int pos_min, int pos_max);

void hal_camera_vcm_close(hal_camera_vcm_ctx_t *ctx);

int hal_camera_vcm_set_pos(hal_camera_vcm_ctx_t *ctx, int pos);

int hal_camera_vcm_get_pos(hal_camera_vcm_ctx_t *ctx, int *pos_out);

int hal_camera_vcm_get_range(hal_camera_vcm_ctx_t *ctx, int *min_out, int *max_out);

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_HAL_CAMERA_VCM_ANYCLOUD39AV200_H */
