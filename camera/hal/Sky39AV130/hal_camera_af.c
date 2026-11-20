// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * AF stub for AnyCloud39AV100_V2 — always unsupported.
 */

#include <errno.h>
#include <stdint.h>

#include "camera.h"

int hal_camera_af_is_supported(int device_id)
{
    (void)device_id;
    return 0;
}

int hal_camera_af_get_sharpness(int device_id, uint32_t *score_out)
{
    (void)device_id;
    (void)score_out;
    return -ENOTSUP;
}

int hal_camera_af_set_focus_pos(int device_id, int pos)
{
    (void)device_id;
    (void)pos;
    return -ENOTSUP;
}

int hal_camera_af_get_focus_pos(int device_id, int *pos_out)
{
    (void)device_id;
    (void)pos_out;
    return -ENOTSUP;
}

int hal_camera_af_get_pos_range(int device_id, int *min_out, int *max_out)
{
    (void)device_id;
    (void)min_out;
    (void)max_out;
    return -ENOTSUP;
}

int hal_camera_af_get_settle_ms(int device_id)
{
    (void)device_id;
    return 20;
}

int hal_camera_af_control(int device_id, int cmd, void *arg)
{
    (void)device_id;
    (void)cmd;
    (void)arg;
    return -ENOTSUP;
}

void hal_camera_af_release(void)
{
}
