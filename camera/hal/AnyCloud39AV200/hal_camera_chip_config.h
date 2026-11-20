// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/camera.
 *
 * HAL camera chip configuration header for AnyCloud39AV200 platform.
 * - Defines platform-specific camera limits and constants.
 * - Contains maximum device, channel, stream, and AI model counts.
 */

#ifndef HARDWARE_HAL_CAMERA_CHIP_CONFIG_HAL_ANYCLOUD39AV200_HAL_CAMERA_CHIP_CONFIG_H
#define HARDWARE_HAL_CAMERA_CHIP_CONFIG_HAL_ANYCLOUD39AV200_HAL_CAMERA_CHIP_CONFIG_H

#include "ak_vpss.h"
#include "ak_vi.h"

#define CAMERA_MAX_NUM                  VI_DEV_MAX //support max number, for camera_stream_format_t
#define CAMERA_MAX_CHANNEL              VPSS_MAX_CHN_NUM //max channel for on camera
#define CAMERA_MAX_STREAM_PER_CHANNEL   4 //max stream for on channel,H.264,H.265,MJPEG,YUV
#define CAMERA_MAX_SPLICE_NUM           MAX_STITCH_CHN_NUM /**< Max stitch channel number */

#define CAMERA_MAX_ROI                  10

#define CAMERA_AI_MAX_MODLE             8  /**< Max AI models supported */
#define CAMERA_AI_MAX_LABLE_PER_MODEL   10 /**< Max labels per model */
#define CAMERA_AI_MAX_BOX               BOX_MAX_NUM
#define CAMERA_AI_MAX_RECOGNIZE_PER_STREAM   2 /**< Max concurrent recognition per frame */


#define CAMERA_CODEC_MAX_KBPS           VENC_MAX_KBPS
#define CAMERA_CODEC_MAX_NUM            9//defined in venc_adapter.c VENC_MAX_USER

#define CAMERA_IPU_QUE_MAX_NUM          AK_IPU_MAX_QUE

#define HAL_CAMERA_HAS_AI_TILE_OVERLAP  1

#endif /* HARDWARE_HAL_CAMERA_CHIP_CONFIG_HAL_ANYCLOUD39AV200_HAL_CAMERA_CHIP_CONFIG_H */
