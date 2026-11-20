// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/camera.
 *
 * HAL camera chip configuration header for AnyCloud39AV100_V2 platform.
 * - Constants match the Anyka PDK 2.x platform/include and samples. Do not depend on
 *   version-specific macros; prefer stable PDK enums/types.
 */

#ifndef HARDWARE_HAL_CAMERA_CHIP_CONFIG_HAL_ANYCLOUD39AV100_V2_HAL_CAMERA_CHIP_CONFIG_H
#define HARDWARE_HAL_CAMERA_CHIP_CONFIG_HAL_ANYCLOUD39AV100_V2_HAL_CAMERA_CHIP_CONFIG_H

#include "ak_common.h" /* provides enum module_id (MODULE_ID_VI) */
#include "ak_vi.h"
/* AnyCloud39AV100_V2: camera HAL path does not support IPU. */

/* multi_CISs sample: MAX_SENSOR 4; VI devices VIDEO_DEV0..VIDEO_DEV3 */
#define CAMERA_MAX_NUM                  4

/* ak_video_sample / THIRD_CHN: main, sub, third channel */
#define CAMERA_MAX_CHANNEL              3

#define CAMERA_MAX_STREAM_PER_CHANNEL   4
#define CAMERA_MAX_SPLICE_NUM           MAX_STITCH_BIND_CHN_NUM

#define CAMERA_MAX_ROI                  10

#define CAMERA_AI_MAX_MODLE             8
#define CAMERA_AI_MAX_LABLE_PER_MODEL   10
#define CAMERA_AI_MAX_BOX               BOX_MAX_NUM
#define CAMERA_AI_MAX_RECOGNIZE_PER_STREAM   2

/* sample/mux/ak_mux_sample.h: VENC_MAX_KBPS */
#define CAMERA_CODEC_MAX_KBPS           1024
/* HAL codec_id_status[] size, same scale as venc user table (VENC_MAX_USER). */
#define CAMERA_CODEC_MAX_NUM            8

/* No IPU queues on this chip/PDK path. */
#define CAMERA_IPU_QUE_MAX_NUM          0

/* Tile-overlap AI needs IPU; not available on AV100_V2 camera HAL. */
#define HAL_CAMERA_HAS_AI_TILE_OVERLAP  0

/* ak_vi_set_dev_attr range (VI log: main[2880*2880] sub[1280*1280]). */
#define HAL_VI_DEV_MAIN_MAX_WIDTH       2880
#define HAL_VI_DEV_MAIN_MAX_HEIGHT      2880
#define HAL_VI_DEV_SUB_MAX_WIDTH        1280
#define HAL_VI_DEV_SUB_MAX_HEIGHT       1280
/* ak_vi_set_dev_attr log: min[18*18] for main/sub. */
#define HAL_VI_DEV_SUB_MIN_WIDTH        18
#define HAL_VI_DEV_SUB_MIN_HEIGHT       18

/*
 * 3DNR ref ION (ak_vi_set_dev_3DNR_memsize, KB): effective crop capped, then NV12 * NUM/DEN,
 * finally clamped to the driver-valid range (VI log: [5160960:14745600] bytes).
 * ak_vi_sample / ak_video_sample: only call ak_vi_set_dev_3DNR_memsize when buf_size_3dnr != 0;
 * HAL uses auto size unless HAL_CAMERA_VI_3DNR_KB=0 to skip (PDK default).
 */
#define HAL_VI_3DNR_REF_MAX_WIDTH       1920
#define HAL_VI_3DNR_REF_MAX_HEIGHT      1080
#define HAL_VI_3DNR_NV12_NUM            1u
#define HAL_VI_3DNR_NV12_DEN            1u
#define HAL_VI_3DNR_KB_DRIVER_MIN       ((5160960 + 1023) / 1024)
#define HAL_VI_3DNR_KB_DRIVER_MAX       ((14745600 + 1023) / 1024)

/*
 * ak_venc_sample.c start_venc(): after ak_venc_open_ex calls
 *   ak_venc_strictlylimit_frameSize(handle_id, 400);
 * maxsize unit: KB (encoded frame cap; may reduce INTERNAL_POOL ION).
 * Set 0 to skip (PDK default unlimited for pool sizing).
 */
#define HAL_VENC_STRICT_FRAME_MAX_KB    400

/*
 * hal_camera_quiesce_channel: after camera_hw marks channel DISABLE, wait this long
 * before ak_vi_disable_chn so the capture thread can exit between get_frame calls and
 * avoid noisy warnings during shutdown.
 * Other chips: leave undefined (HAL treats as 0).
 */
#define HAL_CAMERA_QUIESCE_PRE_DELAY_MS 150

#endif /* HARDWARE_HAL_CAMERA_CHIP_CONFIG_HAL_ANYCLOUD39AV100_V2_HAL_CAMERA_CHIP_CONFIG_H */
