// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/camera.
 *
 * HAL camera implementation for AnyCloud39AV200 platform.
 * - Implements platform-specific camera HAL operations.
 * - Provides device, channel, stream, codec, and AI management.
 * Note: Currently hal_camera contains too much chip-independent code,
 *       which should be migrated to camera_hw.c.
 */

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <getopt.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>


#include "ak_log.h"
#include "ak_common.h"
#include "ak_vi.h"
#include "ak_vpss.h"
#include <string.h>
#include "ak_venc.h"

#include "ak_svp.h"
#include "ak_mem.h"
#include "ak_npu.h"
#include "ak_osd.h"

#include "log.h"
#include "camera.h"
#include "system_pixel_format.h"
/* Include ak_common_video.h AFTER system_pixel_format.h to avoid enum value redefinition conflict.
 * ak_common_video.h defines PIXEL_FORMAT_RGB_LINEINTL, PIXEL_FORMAT_RGB_HWC, PIXEL_FORMAT_RGB_CHW
 * which conflict with system_pixel_format.h's system_pixel_format_t enum. */
#include "ak_common_video.h"

#include "hal_camera.h"
#include "hal_camera_chip_config.h"
#include "hal_camera_config.h"
#include "system_thread.h"
#include "hal_ipu_api.h"
#include "hal_osd_ipu.h"

#include "../../camera_hw.h"
#include "../../hal_camera_api.h"

#define LOG_TAG "HAL CAMERA"

extern int camera_process_capture_request(const camera_hw_device_t *dev,
                                          camera_capture_request_t *request);

/* Forward declarations for rotation helpers used before their definitions. */
static void hal_camera_restore_frame_for_release(hal_camera_channel_t *ch);
static int hal_camera_apply_rotation(struct hal_camera_data *my_data,
                                     int device_id, int channel_id,
                                     VPSS_FRAME_INFO_S *pstFrame);

static int s_rotate_ipu_queue_id = -1;

static size_t hal_camera_frame_bytes(unsigned int w, unsigned int h, int ipu_fmt)
{
    switch ((IPU_COLOR_FMT_E)ipu_fmt) {
    case IPU_COLOR_FMT_SEMIPLANE_NV12:
    case IPU_COLOR_FMT_SEMIPLANE_NV21:
    case IPU_COLOR_FMT_FULLPLANE_YV12:
        return (size_t)w * h * 3u / 2u;
    case IPU_COLOR_FMT_PACKED_RGB24:
    case IPU_COLOR_FMT_PACKED_BGR24:
        return (size_t)w * h * 3u;
    default:
        return 0;
    }
}

static int hal_camera_get_rotate_ipu_queue(void)
{
    if (s_rotate_ipu_queue_id >= 0) {
        return s_rotate_ipu_queue_id;
    }

    int queue_id = hal_camera_get_config()->ipu_que_id;
    if (hal_ipu_alloc_queue(&queue_id) != 0 &&
        hal_ipu_get_default_queue("camera", &queue_id) != 0) {
        log_error("rotate: IPU queue alloc/get_default failed (cfg_que_id=%d)",
                  hal_camera_get_config()->ipu_que_id);
        return -1;
    }

    s_rotate_ipu_queue_id = queue_id;
    log_info("rotate: using IPU queue_id=%d", queue_id);
    return queue_id;
}

/*
 * Map a VPSS output pixel format to the matching IPU color format for rotation.
 * NV12/YV12 and packed RGB (incl. RGB_HWC, which is byte-identical to packed
 * RGB24) are supported. Planar RGB_CHW, line-interleaved RGB and RAW have no
 * usable IPU layout here and return -1 so the caller skips rotation.
 */
static int hal_camera_pixfmt_to_ipu(PIXEL_FORMAT_E fmt)
{
    switch (fmt) {
        case PIXEL_FORMAT_YUV420SP: return IPU_COLOR_FMT_SEMIPLANE_NV12;
        case PIXEL_FORMAT_YUV420P:  return IPU_COLOR_FMT_FULLPLANE_YV12;
        case PIXEL_FORMAT_RGB:      return IPU_COLOR_FMT_PACKED_RGB24;
        case PIXEL_FORMAT_RGB_HWC:  return IPU_COLOR_FMT_PACKED_RGB24;
        default:                    return -1;
    }
}

/*
 * Whether 90/270 rotation should swap the VPSS output dimensions.
 *  - YUV image streams (main/preview/record): swap, so the IPU does a clean
 *    rotation and the encoder/RTSP see the post-rotation (portrait) resolution.
 *  - RGB NPU inputs (AI): keep the fixed model dimensions; the IPU rotates AND
 *    resizes back into the same WxH (content is uprighted, only stretched).
 *    The model contract (e.g. 640x384) stays intact and AI overlay boxes still
 *    map correctly via independent per-axis scaling.
 */
static int hal_camera_rotation_swaps_dims(PIXEL_FORMAT_E fmt)
{
    return (fmt == PIXEL_FORMAT_YUV420SP || fmt == PIXEL_FORMAT_YUV420P);
}

/* Normalize an arbitrary degree value to one of {0, 90, 180, 270}. */
static int hal_camera_normalize_rotation(int deg)
{
    int r = deg % 360;
    if (r < 0) {
        r += 360;
    }
    if (r == 90 || r == 180 || r == 270) {
        return r;
    }
    return 0;
}

/* VPSS main channel zoom limits (see VPSS_CB_SetChnAttr log on AV200). */
#define HAL_CAMERA_VPSS_ZOOM_MIN 0.5
#define HAL_CAMERA_VPSS_ZOOM_MAX 2.0

/*
 * Center-crop on the sensor with the same aspect as VPSS output (out_w x out_h),
 * keeping scale within [VPSS_ZOOM_MIN, VPSS_ZOOM_MAX] on both axes.
 * Used when 90/270 rotation swaps VPSS output dims: json crop is usually
 * landscape (e.g. 3264x1836) and cannot scale to portrait VPSS out (e.g. 1440x2560).
 */
static void hal_camera_compute_vpss_crop(unsigned int sensor_w, unsigned int sensor_h,
    unsigned int out_w, unsigned int out_h,
    unsigned int *crop_x, unsigned int *crop_y,
    unsigned int *crop_w, unsigned int *crop_h)
{
    unsigned int cw, ch, cx, cy;
    double out_aspect;
    double scale;

    if (!crop_x || !crop_y || !crop_w || !crop_h || sensor_w == 0 || sensor_h == 0
        || out_w == 0 || out_h == 0) {
        return;
    }

    out_aspect = (double)out_w / (double)out_h;

    ch = sensor_h;
    cw = (unsigned int)((double)ch * out_aspect + 0.5);
    if (cw > sensor_w) {
        cw = sensor_w;
        ch = (unsigned int)((double)cw / out_aspect + 0.5);
    }

    scale = (double)out_w / (double)cw;
    if (scale < HAL_CAMERA_VPSS_ZOOM_MIN) {
        cw = (unsigned int)((double)out_w / HAL_CAMERA_VPSS_ZOOM_MIN + 0.5);
        ch = (unsigned int)((double)cw / out_aspect + 0.5);
    } else if (scale > HAL_CAMERA_VPSS_ZOOM_MAX) {
        cw = (unsigned int)((double)out_w / HAL_CAMERA_VPSS_ZOOM_MAX + 0.5);
        ch = (unsigned int)((double)cw / out_aspect + 0.5);
    }

    if (cw > sensor_w) {
        cw = sensor_w;
    }
    if (ch > sensor_h) {
        ch = sensor_h;
    }

    /* YUV420 requires even crop geometry on most VPSS builds. */
    cw &= ~1u;
    ch &= ~1u;
    if (cw < 2 || ch < 2) {
        return;
    }

    cx = (sensor_w > cw) ? (sensor_w - cw) / 2u : 0u;
    cy = (sensor_h > ch) ? (sensor_h - ch) / 2u : 0u;
    cx &= ~1u;
    cy &= ~1u;

    *crop_x = cx;
    *crop_y = cy;
    *crop_w = cw;
    *crop_h = ch;
}

static void hal_camera_dump_runtime_threads(const char *stage)
{
    char thread_info[4096] = {0};
    if (thread_get_info(thread_info, (int)sizeof(thread_info)) == 0) {
        log_info("HAL-CLOSE stage=%s active_threads:\n%s", stage ? stage : "unknown", thread_info);
    } else {
        log_warn("HAL-CLOSE stage=%s thread_get_info failed", stage ? stage : "unknown");
    }
}

static void hal_camera_dump_device_runtime_state(struct camera_device *cdev, int device_id, const char *stage)
{
    if (cdev == NULL) {
        return;
    }

    int dev_idx = -1;
    for (int i = 0; i < cdev->num_device; i++) {
        if (cdev->device_info[i].device_id == device_id) {
            dev_idx = i;
            break;
        }
    }
    if (dev_idx < 0) {
        log_warn("HAL-CLOSE stage=%s dev=%d not found in cdev", stage ? stage : "unknown", device_id);
        return;
    }

    device_info_t *dev_info = &cdev->device_info[dev_idx];
    log_info("HAL-CLOSE stage=%s dev=%d status=%d num_channel=%d closing=%d",
             stage ? stage : "unknown",
             dev_info->device_id,
             dev_info->status,
             dev_info->num_channel,
             cdev->closing);

    for (int ch = 0; ch < dev_info->num_channel; ch++) {
        channel_info_t *ch_info = &dev_info->channel_info[ch];
        log_info("HAL-CLOSE stage=%s dev=%d ch=%d ch_status=%d cap_tid=%lu num_stream=%d",
                 stage ? stage : "unknown",
                 dev_info->device_id, ch, ch_info->status,
                 (unsigned long)ch_info->thread_id, ch_info->num_stream);
        for (int s = 0; s < ch_info->num_stream; s++) {
            stream_info_t *st = &ch_info->stream_info[s];
            log_info("HAL-CLOSE stage=%s dev=%d ch=%d sid=%d st_status=%d req_tpl=%d cur_tpl=%d codec_id=%d ai_type=0x%x ai_status=%d ai_tid=%lu",
                     stage ? stage : "unknown",
                     dev_info->device_id, ch, s,
                     st->status, st->request.request_template, st->request_template,
                     st->codec_id, st->ai_type, st->ai_status, (unsigned long)st->ai_thread_id);
        }
    }
}


/**
 * Initializes the camera device and allocates memory for camera data
 * @param cdev The camera device structure
 * @return Pointer to the allocated camera data or NULL if allocation fails
 */
void *hal_camera_init(struct camera_device *cdev)
{
    struct hal_camera_data *my_data;


    log_info("AK_MPI_VI_GetVersion: %s   ", AK_MPI_VI_GetVersion());
    log_info("AK_MPI_VPSS_GetVersion: %s", AK_MPI_VPSS_GetVersion());
    log_info("AK_MPI_ISP_GetIspSdkVersion:  %s", AK_MPI_ISP_GetIspSdkVersion());
//    log_info("AK_MPI_MUX_Get_Version:  %s", AK_MPI_MUX_Get_Version());
    log_info("MAK_MPI_OSD_GetVersion: %s", AK_MPI_OSD_GetVersion());
//    log_info("AK_MPI_DEMUX_Get_Version: %s   ", AK_MPI_DEMUX_Get_Version());

    my_data = calloc(1, sizeof(struct hal_camera_data));
    if (!my_data)
    {
        log_error("hal_camera_init error");
        return 0;
    }


    my_data->cdev = cdev;

    hal_camera_codec_init(my_data);

    return  (void *)my_data;
}

/**
 * Deinitializes the camera and frees the allocated memory
 * @param my_data Pointer to the camera data to be freed
 */
void hal_camera_deinit(void *hal_camera)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;

    hal_camera_af_release();
    free(my_data);
}

/**
 * Gets the maximum number of streams
 * @return The maximum number of streams
 */
int hal_camera_get_max_stream( void )
{
    return CAMERA_MAX_CHANNEL;
}

int hal_camera_set_callback_ops(void *hal_camera, const camera_callback_ops_t *callback_ops)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    if (my_data == NULL || my_data->cdev == NULL || callback_ops == NULL) {
        return -1;
    }

    pthread_mutex_lock(&my_data->cdev->lock);
    my_data->cdev->callback_ops = callback_ops;
    pthread_mutex_unlock(&my_data->cdev->lock);
    return 0;
}

int hal_camera_process_capture_request(void *hal_camera, camera_capture_request_t *request)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    struct camera_device *cdev;
    int ret;

    if (my_data == NULL || request == NULL || my_data->cdev == NULL) {
        return -1;
    }

    cdev = my_data->cdev;
    pthread_mutex_lock(&cdev->lock);
    ret = camera_process_capture_request((const camera_hw_device_t *)cdev, request);
    pthread_mutex_unlock(&cdev->lock);

    return ret;
}

int hal_camera_get_group_number(void)
{
    const hal_camera_config_t *cfg = hal_camera_get_config();
    return cfg->camera_number;
}

/*
 * AV200 uses VI chn0 + VPSS multi-chn (see ak_vi_sample / ak_video_sample on V1.03).
 * No V2.04-style sub_max / placeholder VI ch1; keep API for camera_hw parity.
 */
int hal_camera_sync_platform_vi_after_channels(void *hal_camera, int device_id)
{
    (void)hal_camera;
    (void)device_id;
    return 0;
}

/**
 * @brief Create camera device.
 *
 * Initialize VI device, VI channel, and VPSS group.
 * In AnyCloudAV200, device_id, vi_channel_id, and vpss_grp_id are bound together.
 * vpss_channel_id is the actual channel_id.
 *
 * @param hal_camera Pointer to HAL camera data.
 * @param device_id Device ID to create.
 * @return 0 on success, -1 on failure.
 */
int hal_camera_create_device(void *hal_camera, int device_id)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    int ret;
    int vi_channel_id = 0; // According to AnyCloudAV200 definition, only support 0 vi channel
    int vpss_group_id = device_id;

    VI_DEV_ATTR_S stViDevAttr = {0};
    VI_RECTANGLE_S stViRectangle = {0};
    VI_CHN_ATTR_S stViChnAttr = {0};
    VPSS_GRP_ATTR_S stGrpAttr = {0};

    hal_camera_device_t *hal_camera_device = &my_data->hal_camera_device[device_id];
    const hal_camera_config_t *cfg = hal_camera_get_config();

    log_debug("hal_camera_create_device, device_id:%d", device_id);

    /* Create VI device */

    hal_camera_device->vi_channel_id = vi_channel_id;
    hal_camera_device->vpss_group_id = vpss_group_id;

    hal_camera_device->flip_enabled = 0;
    hal_camera_device->mirror_enabled = 0;

    log_debug("hal_camera_device->vpss_group_id:%d ",hal_camera_device->vpss_group_id);

    stViDevAttr.u32Width = 320;
    stViDevAttr.u32Height = 184;

    stViDevAttr.enIntfMode = cfg->interface_mode[device_id];
    stViDevAttr.bFlip = cfg->flip[device_id];
    stViDevAttr.bMirror = cfg->mirror[device_id];
    stViDevAttr.u32Fps = cfg->max_fps[device_id];

    stViDevAttr.stCropInfo.u32PointX = 0;
    stViDevAttr.stCropInfo.u32PointY = 0;
    stViDevAttr.stCropInfo.u32Width =  stViDevAttr.u32Width;
    stViDevAttr.stCropInfo.u32Height =  stViDevAttr.u32Height;

    /* Fastsys loads camera kos after LCD; wait briefly so /dev/vicap-* exists. */
    {
        char vicap_path[64];
        int wait_i;

        snprintf(vicap_path, sizeof(vicap_path), "/dev/vicap-%d-0-0", device_id);
        for (wait_i = 0; wait_i < 50; wait_i++) {
            if (access(vicap_path, F_OK) == 0) {
                break;
            }
            if (wait_i == 0) {
                log_info("waiting for %s ...", vicap_path);
            }
            usleep(100 * 1000);
        }
        if (access(vicap_path, F_OK) != 0) {
            log_error("%s not present (camera modules not loaded?)", vicap_path);
            return -1;
        }
    }

    ret = AK_MPI_VI_CreateDev(device_id, &stViDevAttr);
    if(ret) {
        log_error("Create vi device fail");

        return -1;
    }
    ret = AK_MPI_VI_GetSensorRes(device_id, &stViRectangle);
    if(ret) {
        log_error("Get sensor res fail");

        return -1;
    }

     stViDevAttr.u32Width = stViRectangle.u32Width;
     stViDevAttr.u32Height = stViRectangle.u32Height;
     stViDevAttr.stCropInfo.u32Width = stViRectangle.u32Width;
     stViDevAttr.stCropInfo.u32Height = stViRectangle.u32Height;
    ret = AK_MPI_VI_SetDevAttr(device_id, &stViDevAttr);
    if(ret) {
        log_error("set device attr fail");

        return -1;
    }

    /* Create VI channel */
    stViChnAttr.enMode = VI_CHN_MODE_AUTO;  /* Only user mode can get raw data */
    stViChnAttr.enOutputPixFmt = PIXEL_FORMAT_YUV420SP; /* Not related to actual channel output */

    stViChnAttr.u32MaxPerFrameSize = 0;  /* Use default value */

    stViChnAttr.u32Width =  stViRectangle.u32Width;
    stViChnAttr.u32Height = stViRectangle.u32Height;
    stViChnAttr.u32FrameDepth = 3;
    stViChnAttr.u32Fps = cfg->max_fps[device_id];

    log_debug("VI channel attr: width:%d, height:%d, depth:%d, fps:%d",
            stViChnAttr.u32Width, stViChnAttr.u32Height, stViChnAttr.u32FrameDepth, stViChnAttr.u32Fps);

    ret = AK_MPI_VI_SetChnAttr(device_id, vi_channel_id, &stViChnAttr);  //vpss channel from vi channel 0
    if(ret) {
        log_error("Set vi chn attr fail");

        return -1;
    }

    log_info("camera device[%d] is created",device_id);

    /* Create VPSS group, set vpss group id = device id */
    stGrpAttr.u32Width = stViRectangle.u32Width;
    stGrpAttr.u32Height = stViRectangle.u32Height;

    stGrpAttr.bFlip = hal_camera_device->flip_enabled;
    stGrpAttr.bMirror = hal_camera_device->mirror_enabled;
    stGrpAttr.enInputPixFmt = PIXEL_FORMAT_YUV420SP;

    // ISP config file path from camera_config.ini via hal_camera_config.h
    int cam_idx = device_id;
    if (cam_idx < 0 || cam_idx >= cfg->camera_number || cam_idx >= CAMERA_MAX_NUM) {
        cam_idx = 0;
    }
    strncpy((char *)stGrpAttr.pu8IspCfgPath, cfg->isp_config_path[cam_idx],
            sizeof(stGrpAttr.pu8IspCfgPath) - 1);
    stGrpAttr.pu8IspCfgPath[sizeof(stGrpAttr.pu8IspCfgPath) - 1] = '\0';
    log_info("VPSS isp cfg path: %s", (const char *)stGrpAttr.pu8IspCfgPath);
    stGrpAttr.bEnGrpBypass = 0;
    stGrpAttr.enWorkMode = VPSS_WORK_AUTO;
    AK_U32 u32SensorId = 0;
    AK_MPI_VI_GetSensorId(vpss_group_id, &u32SensorId);
    stGrpAttr.s32AttachSensorId = u32SensorId;

     ret = AK_MPI_VPSS_CreateGrp(vpss_group_id, &stGrpAttr);
    if(ret) {
        log_error("Create vpss Grp fail \n");
        return -1;
    }

    /* Bind device_id to vi_channel_id, bind vi_channel_id to vpss_group_id */
    ret = AK_MPI_VI_BindVpssGrp(device_id, vi_channel_id, vpss_group_id);
    if(ret) {
        log_error("bind vpss grp fail ");

        return ret;
    }
    
    ret = AK_MPI_VPSS_BindChn(vpss_group_id, vi_channel_id); /* Only bind corresponding channel 0 */
    if(ret) {
        log_error("bind vi device fail ");

        return ret;
    }

    return 0;
}


int hal_camera_create_channel(void *hal_camera, int device_id, int channel_id)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    int ret;
    PIXEL_FORMAT_E format;

    hal_camera_device_t *hal_camera_device = &my_data->hal_camera_device[device_id];
    device_info_t *device_info = &my_data->cdev->device_info[device_id];
    VPSS_CHN_ATTR_S stVpssChnAttr = {0};
    camera_stream_t *stream = device_info->channel_info[channel_id].stream_info[0].stream;

    int vi_channel_id = hal_camera_device->vi_channel_id;
    int vpss_group_id = hal_camera_device->vpss_group_id;
    int vpss_channel_id = channel_id;
    camera_channel_config_t *channel_config = &device_info->channel_info[channel_id].channel_config;

    log_info("[CAM_TRACE] hal_create_channel: enter dev=%d ch=%d %ux%u (tid=%lu)",
             device_id, channel_id, channel_config->width, channel_config->height,
             (unsigned long)pthread_self());

    log_info("create channel: device_id:%d, vi_channel_id:%d, vpss_group_id:%d", device_id, vi_channel_id, vpss_group_id);

    /* Set VPSS channel attributes */
    switch (stream->format)
    {
        case SYSTEM_PIXEL_FORMAT_RGB_888:
            format = PIXEL_FORMAT_RGB;
        break;

        case SYSTEM_PIXEL_FORMAT_YCBCR_420_SP:
            format = PIXEL_FORMAT_YUV420SP;
        break;

        case SYSTEM_PIXEL_FORMAT_YCRCB_420_SP:
            format = PIXEL_FORMAT_YUV420P;
        break;

        case SYSTEM_PIXEL_FORMAT_RAW10:
        case SYSTEM_PIXEL_FORMAT_RAW12:
            format = PIXEL_FORMAT_RAW;
        break;

        case SYSTEM_PIXEL_FORMAT_RGB_HWC:
            format = PIXEL_FORMAT_RGB_HWC;
        break;

        case SYSTEM_PIXEL_FORMAT_RGB_CHW:
            format = PIXEL_FORMAT_RGB_CHW;
        break;

        default: 
            log_error("unsupport format:%d", stream->format);

        return -1;

        break;
    }

    /*
     * Software rotation setup. The configured width/height is the FINAL
     * (post-rotation) orientation used by VENC/RTSP/broadcast. For 90/270 the
     * VPSS channel must output the swapped (pre-rotation) dimensions so the IPU
     * can rotate it back into the configured resolution in hal_camera_stream_read().
     */
    const hal_camera_config_t *cfg = hal_camera_get_config();
    int rotation = hal_camera_normalize_rotation(cfg->rotation[device_id]);
    int ipu_fmt = hal_camera_pixfmt_to_ipu(format);
    int rotate_enabled = (rotation != 0 && ipu_fmt >= 0);
    int swap_dims = hal_camera_rotation_swaps_dims(format);

    unsigned int vpss_w = channel_config->width;
    unsigned int vpss_h = channel_config->height;
    if (rotate_enabled && (rotation == 90 || rotation == 270) && swap_dims) {
        vpss_w = channel_config->height;
        vpss_h = channel_config->width;
    }

    hal_camera_channel_t *hal_channel = &hal_camera_device->channel[channel_id];
    if (rotation != 0 && ipu_fmt < 0) {
        log_warn("channel %d rotation %d requested but format %d not rotatable, skip rotation",
                 channel_id, rotation, format);
    }
    hal_channel->rotation = rotate_enabled ? rotation : 0;
    hal_channel->rot_ipu_color_fmt = ipu_fmt;
    hal_channel->rot_src_w = vpss_w;
    hal_channel->rot_src_h = vpss_h;
    hal_channel->rot_dst_w = channel_config->width;
    hal_channel->rot_dst_h = channel_config->height;
    if (rotate_enabled) {
        log_info("channel %d rotation=%d mode=%s: VPSS out %ux%u -> IPU out %ux%u (ipu_fmt=0x%x)",
                 channel_id, rotation, swap_dims ? "swap" : "resize",
                 vpss_w, vpss_h, channel_config->width, channel_config->height, ipu_fmt);
    }

    if (channel_config->crop.width != 0 && channel_config->crop.height != 0) {
        unsigned int crop_x = channel_config->crop.x;
        unsigned int crop_y = channel_config->crop.y;
        unsigned int crop_w = channel_config->crop.width;
        unsigned int crop_h = channel_config->crop.height;

        if (rotate_enabled && (rotation == 90 || rotation == 270) && swap_dims) {
            VI_RECTANGLE_S sensor_res = {0};
            if (AK_MPI_VI_GetSensorRes(device_id, &sensor_res) == 0
                && sensor_res.u32Width > 0 && sensor_res.u32Height > 0) {
                hal_camera_compute_vpss_crop(sensor_res.u32Width, sensor_res.u32Height,
                    vpss_w, vpss_h, &crop_x, &crop_y, &crop_w, &crop_h);
                log_info("channel %d rotation crop: sensor %ux%u json [%u,%u,%u,%u] -> vpss [%u,%u,%u,%u] for out %ux%u",
                         channel_id, sensor_res.u32Width, sensor_res.u32Height,
                         channel_config->crop.x, channel_config->crop.y,
                         channel_config->crop.width, channel_config->crop.height,
                         crop_x, crop_y, crop_w, crop_h, vpss_w, vpss_h);
            } else {
                log_warn("channel %d rotation crop: GetSensorRes failed, keep json crop", channel_id);
            }
        } else if (rotate_enabled && (rotation == 90 || rotation == 270) && !swap_dims) {
            /* RGB/NPU path (resize mode): VPSS buffer is landscape WxH but IPU rotates to
             * upright portrait content. Use swapped VPSS dims for sensor crop so AI sees the
             * same center FOV as the YUV preview channel (e.g. 480x800 preview vs 640x384 AI). */
            VI_RECTANGLE_S sensor_res = {0};
            if (AK_MPI_VI_GetSensorRes(device_id, &sensor_res) == 0
                && sensor_res.u32Width > 0 && sensor_res.u32Height > 0) {
                unsigned int crop_aspect_w = vpss_h;
                unsigned int crop_aspect_h = vpss_w;
                hal_camera_compute_vpss_crop(sensor_res.u32Width, sensor_res.u32Height,
                    crop_aspect_w, crop_aspect_h, &crop_x, &crop_y, &crop_w, &crop_h);
                log_info("channel %d rotation crop (ai_resize): sensor %ux%u json [%u,%u,%u,%u] -> vpss [%u,%u,%u,%u] aspect %ux%u for out %ux%u",
                         channel_id, sensor_res.u32Width, sensor_res.u32Height,
                         channel_config->crop.x, channel_config->crop.y,
                         channel_config->crop.width, channel_config->crop.height,
                         crop_x, crop_y, crop_w, crop_h, crop_aspect_w, crop_aspect_h,
                         vpss_w, vpss_h);
            } else {
                log_warn("channel %d rotation crop (ai_resize): GetSensorRes failed, keep json crop", channel_id);
            }
        }

        stVpssChnAttr.stCropInfo.u32Width = crop_w;
        stVpssChnAttr.stCropInfo.u32Height = crop_h;
        stVpssChnAttr.stCropInfo.u32PointX = crop_x;
        stVpssChnAttr.stCropInfo.u32PointY = crop_y;
    }
    else
    {
        stVpssChnAttr.stCropInfo.u32Width = channel_config->crop.width;
        stVpssChnAttr.stCropInfo.u32Height = channel_config->crop.height;
        stVpssChnAttr.stCropInfo.u32PointX = 0;
        stVpssChnAttr.stCropInfo.u32PointY = 0;
    }

    if(stream->num_slices > 1)
    {
        stVpssChnAttr.enChnMode = VPSS_CHN_SLICE_MODE;
    }
    else
    {
        stVpssChnAttr.enChnMode = VPSS_CHN_FRAME_MODE;
    }

    stVpssChnAttr.enOutputPixFmt = format;

    stVpssChnAttr.s32Fps = stream->fps;
    stVpssChnAttr.u32FrameDepth = channel_config->max_buffers;

    log_debug("channel config: [%d x %d]", channel_config->width, channel_config->height);
    if(channel_config->width == 640 && channel_config->height == 384)
    {
        stVpssChnAttr.u32MaxBufSize = 0;
        stVpssChnAttr.u32Width = vpss_w;
        stVpssChnAttr.u32Height = vpss_h;

        log_info("[CAM_TRACE] hal_create_channel: before VPSS_SetChnAttr grp=%d ch=%d (ai_path)",
                 vpss_group_id, vpss_channel_id);
        ret = AK_MPI_VPSS_SetChnAttr(vpss_group_id, vpss_channel_id, &stVpssChnAttr);
        log_info("[CAM_TRACE] hal_create_channel: after VPSS_SetChnAttr grp=%d ch=%d ret=%d",
                 vpss_group_id, vpss_channel_id, ret);
        if(ret) {
            log_error("set vpss main chn attr fail ");
            return -1;
        }
    }
    else {
        stVpssChnAttr.u32Width = vpss_w;
        stVpssChnAttr.u32Height = vpss_h;
        stVpssChnAttr.u32MaxBufSize = 0;

    log_info("create channel:%d, format:%d, fps:%d, width:%d, height:%d",
        channel_id, stream->format, stVpssChnAttr.s32Fps,
        stVpssChnAttr.u32Width, stVpssChnAttr.u32Height);

    log_info("[CAM_TRACE] hal_create_channel: before VPSS_SetChnAttr grp=%d ch=%d",
             vpss_group_id, vpss_channel_id);
    ret = AK_MPI_VPSS_SetChnAttr(vpss_group_id, vpss_channel_id, &stVpssChnAttr);
    log_info("[CAM_TRACE] hal_create_channel: after VPSS_SetChnAttr grp=%d ch=%d ret=%d",
             vpss_group_id, vpss_channel_id, ret);
    if (ret) {
        log_error("set vpss main chn attr fail[group id: %d, channel id: %d, format:%d,fps:%d,width:%d,height:%d]",
                  vpss_group_id, vpss_channel_id, stream->format, stream->fps,
                  stVpssChnAttr.u32Width, stVpssChnAttr.u32Height);
        return -1;
    }
    }

    log_info("[CAM_TRACE] hal_create_channel: done dev=%d ch=%d ret=0", device_id, channel_id);
    return 0;
}

int hal_camera_enable_device(void *hal_camera, int device_id)
{
    int ret;
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;

    int vi_channel_id = my_data->hal_camera_device[device_id].vi_channel_id;

    log_debug("hal_camera_enable_device: device_id:%d", device_id);

    ret = AK_MPI_VI_EnableDev(device_id);
    if(ret) {
        log_error("enable vi device fail");
        return -1;
    }

    /*
    * step 2: enable vi chn
    */
    ret = AK_MPI_VI_EnableChn(device_id,vi_channel_id);
    if(ret) {
        log_error("enable vi channel fail");
        return -1;
    }

    /* Start VPSS */
    ret = AK_MPI_VPSS_StartGrp(device_id);
    if(ret) {
        log_error("start vpss grp fail ");
        return -1;
    }

    return 0;
}

int hal_camera_disable_device(void *hal_camera, int device_id)
{
    int ret;
    int final_ret = 0;
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;

    int vi_channel_id = my_data->hal_camera_device[device_id].vi_channel_id;
    int vpss_group_id = my_data->hal_camera_device[device_id].vpss_group_id;

    log_info("disable device:%d", device_id);
    hal_camera_dump_device_runtime_state(my_data->cdev, device_id, "before-vpss-stop");
    hal_camera_dump_runtime_threads("before-vpss-stop");

    /* Channel disable is already done in camera_disable_channel().
     * Avoid duplicate VPSS_DisableChn() here, then stop/destroy VPSS group and VI. */

    ret = AK_MPI_VPSS_StopGrp(vpss_group_id);
    if(ret) {
        log_error("AK_MPI_VPSS_StopGrp fail ");
        if (final_ret == 0) {
            final_ret = ret;
        }
    } else {
        log_info("AK_MPI_VPSS_StopGrp success: grp=%d", vpss_group_id);
    }

    hal_camera_dump_device_runtime_state(my_data->cdev, device_id, "after-vpss-stop");
    hal_camera_dump_runtime_threads("after-vpss-stop");

    log_info("HAL-CLOSE stage=before-vpss-destroy grp=%d", vpss_group_id);
    ret = AK_MPI_VPSS_DestroyGrp(vpss_group_id);
    if(ret) {
        log_error("AK_MPI_VPSS_DestroyGrp fail ");
        if (final_ret == 0) {
            final_ret = ret;
        }
    } else {
        log_info("AK_MPI_VPSS_DestroyGrp success: grp=%d", vpss_group_id);
    }

    ret = AK_MPI_VI_DisableChn(device_id,vi_channel_id);
    if(ret) {
        log_error("Disable vi chn fail .");
        if (final_ret == 0) {
            final_ret = -1;
        }
    } else {
        log_info("AK_MPI_VI_DisableChn success: dev=%d chn=%d", device_id, vi_channel_id);
    }

    ret = AK_MPI_VI_DisableDev(device_id);
    if(ret) {
        log_error("Disable vi device fail .");
        if (final_ret == 0) {
            final_ret = -1;
        }
    } else {
        log_info("AK_MPI_VI_DisableDev success: dev=%d", device_id);
    }

    /* Release device node to avoid Resource busy on next start. */
    ret = AK_MPI_VI_DestroyDev(device_id);
    if (ret) {
        log_error("Destroy vi device fail .");
        if (final_ret == 0) {
            final_ret = -1;
        }
    } else {
        log_info("AK_MPI_VI_DestroyDev success: dev=%d", device_id);
    }

    return final_ret;
}



/* stream_read returns frame N and fetches N+1 here while the caller encodes N.
 * At most two frames are held, within max_buffers (3). */
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int started;
    int off;
    int busy;
    int have;
    int drop;
    int halt;
    int fails;
    int ch;
    VPSS_FRAME_INFO_S frame;
} hal_pf_t;

static hal_pf_t g_pf[CAMERA_MAX_CHANNEL];
static pthread_once_t g_pf_once = PTHREAD_ONCE_INIT;

static void hal_pf_init(void)
{
    for (int i = 0; i < CAMERA_MAX_CHANNEL; i++) {
        pthread_mutex_init(&g_pf[i].mu, NULL);
        pthread_cond_init(&g_pf[i].cv, NULL);
        g_pf[i].ch = i;
    }
}

static void *hal_pf_thread(void *arg)
{
    hal_pf_t *pf = (hal_pf_t *)arg;
    VPSS_FRAME_INFO_S f;
    int ret;

    pthread_mutex_lock(&pf->mu);
    for (;;) {
        while (!pf->busy)
            pthread_cond_wait(&pf->cv, &pf->mu);
        pthread_mutex_unlock(&pf->mu);
        memset(&f, 0x00, sizeof(f));
        ret = AK_MPI_VPSS_GetFrame(0, pf->ch, &f, 2000);
        pthread_mutex_lock(&pf->mu);
        if (ret == 0 && pf->drop) {
            pthread_mutex_unlock(&pf->mu);
            AK_MPI_VPSS_ReleaseFrame(0, pf->ch, &f);
            pthread_mutex_lock(&pf->mu);
            ret = -1;
        }
        pf->drop = 0;
        if (ret == 0) {
            pf->frame = f;
            pf->have = 1;
            pf->fails = 0;
        } else if (++pf->fails >= 3 && !pf->off) {
            pf->off = 1;
            log_warn("vpss prefetch off ch=%d", pf->ch);
        }
        pf->busy = 0;
        pthread_cond_broadcast(&pf->cv);
    }
    return NULL;
}

/* Returns 1 when the prefetch path handled the read; *ret is 0 with *out filled on success. */
static int hal_pf_take(int device_id, int channel_id, VPSS_FRAME_INFO_S *out, int *ret)
{
    hal_pf_t *pf;

    if (device_id != 0 || channel_id < 0 || channel_id >= CAMERA_MAX_CHANNEL)
        return 0;
    pthread_once(&g_pf_once, hal_pf_init);
    pf = &g_pf[channel_id];
    pthread_mutex_lock(&pf->mu);
    if (pf->off && !pf->busy && !pf->have) {
        pthread_mutex_unlock(&pf->mu);
        return 0;
    }
    if (!pf->started) {
        pthread_t th;

        if (pthread_create(&th, NULL, hal_pf_thread, pf) != 0) {
            pf->off = 1;
            pthread_mutex_unlock(&pf->mu);
            return 0;
        }
        pthread_detach(th);
        pf->started = 1;
        log_info("vpss prefetch on ch=%d", channel_id);
    }
    if (!pf->busy && !pf->have) {
        pf->halt = 0;
        pf->busy = 1;
        pthread_cond_broadcast(&pf->cv);
    }
    while (pf->busy)
        pthread_cond_wait(&pf->cv, &pf->mu);
    *ret = -1;
    if (pf->have) {
        *out = pf->frame;
        pf->have = 0;
        *ret = 0;
    }
    if (!pf->off && !pf->halt) {
        pf->busy = 1;
        pthread_cond_broadcast(&pf->cv);
    }
    pthread_mutex_unlock(&pf->mu);
    return 1;
}

static void hal_pf_abort(int device_id, int channel_id)
{
    hal_pf_t *pf;
    VPSS_FRAME_INFO_S f;
    int have;

    if (device_id != 0 || channel_id < 0 || channel_id >= CAMERA_MAX_CHANNEL)
        return;
    pthread_once(&g_pf_once, hal_pf_init);
    pf = &g_pf[channel_id];
    pthread_mutex_lock(&pf->mu);
    have = pf->have;
    if (have)
        f = pf->frame;
    pf->have = 0;
    pf->halt = 1;
    if (pf->busy)
        pf->drop = 1;
    pthread_mutex_unlock(&pf->mu);
    if (have)
        AK_MPI_VPSS_ReleaseFrame(device_id, channel_id, &f);
}

static void hal_camera_release_pending_frame(void *hal_camera, int device_id, int channel_id)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    hal_camera_channel_t *chn;
    VPSS_FRAME_INFO_S *pstFrame;

    if (my_data == NULL) {
        return;
    }
    chn = &my_data->hal_camera_device[device_id].channel[channel_id];
    hal_pf_abort(device_id, channel_id);
    if (chn->channel_buffer_release_flag != 1) {
        return;
    }
    pstFrame = chn->pstFrame;
    chn->channel_buffer_release_flag = 0;
    hal_camera_restore_frame_for_release(chn);
    (void)AK_MPI_VPSS_ReleaseFrame(device_id, channel_id, pstFrame);
}

int hal_camera_quiesce_channel(void *hal_camera, int device_id, int channel_id)
{
    hal_camera_release_pending_frame(hal_camera, device_id, channel_id);
    (void)AK_MPI_VPSS_DisableChn(device_id, channel_id);
    return 0;
}

int hal_camera_disable_channel(void *hal_camera, int device_id, int channel_id)
{
    int ret = 0;
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;

    int vi_channel_id = 0;
    int vpss_group_id = device_id;
    int vpss_channel_id = channel_id;

    log_debug("hal_camera_disable_channel:vi_channel_id:%d,vpss_group_id:%d,vpss_channel_id:%d",vi_channel_id,vpss_group_id,vpss_channel_id);

    hal_camera_channel_t *hal_channel = &my_data->hal_camera_device[device_id].channel[channel_id];

    hal_camera_release_pending_frame(hal_camera, device_id, channel_id);
    hal_camera_codec_disable_channel(hal_camera, device_id, channel_id);

    /* Tear down rotation resources held by this channel. */
    for (int i = 0; i < 2; i++) {
        if (hal_channel->rot_buf[i]) {
            AK_BASE_MemDmaFree(hal_channel->rot_buf[i]);
            hal_channel->rot_buf[i] = NULL;
            hal_channel->rot_buf_phy[i] = 0;
        }
    }
    hal_channel->rot_buf_len = 0;
    hal_channel->rot_buf_idx = 0;
    hal_channel->frame_rotated = 0;
    if (hal_channel->rot_ipu_opened) {
        hal_ipu_close();
        hal_channel->rot_ipu_opened = 0;
    }

    ret = AK_MPI_VPSS_DisableChn(vpss_group_id, vpss_channel_id);

    if(ret) {
        log_error("Disable vpss chn%d fail .", vpss_channel_id);
        return AK_FAILED;
    }


    return 0;
}


int hal_camera_vpss_chn_is_configured(void *hal_camera, int device_id, int channel_id)
{
    VPSS_CHN_ATTR_S attr;

    (void)hal_camera;
    memset(&attr, 0, sizeof(attr));
    if (AK_MPI_VPSS_GetChnAttr(device_id, channel_id, &attr) != AK_SUCCESS)
        return 0;
    return (attr.u32Width > 0 && attr.u32Height > 0) ? 1 : 0;
}

/*
 * AV200 is VPSS-based: AK_MPI_VPSS_SetChnAttr after AK_MPI_VPSS_StartGrp hangs
 * (SDK order is CreateGrp -> SetChnAttr* -> StartGrp -> EnableChn). A channel cannot
 * be re-created while the group is running, so live per-channel reconfigure is unsafe.
 */
int hal_camera_supports_live_chn_reconfigure(void *hal_camera, int device_id)
{
    (void)hal_camera;
    (void)device_id;
    return 0;
}

int hal_camera_enable_channel(void *hal_camera, int device_id, int channel_id)
{
    int ret = 0;
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    my_data->hal_camera_device[device_id].vi_channel_id = 0;
    my_data->hal_camera_device[device_id].vpss_group_id = device_id;
    int vpss_channel_id = channel_id;

    log_info("vi_channel_id:%d,vpss_group_id:%d,vpss_channel_id:%d",0,device_id,vpss_channel_id);

    ret = AK_MPI_VPSS_EnableChn(my_data->hal_camera_device[device_id].vpss_group_id, vpss_channel_id);
    if(ret) {
        log_error("enable vpss chn%d fail ",vpss_channel_id);
        return -1;
    }

    /* Allocate channel frame buffer if needed */
    if(my_data->hal_camera_device[device_id].channel[channel_id].pstFrame == NULL)
        my_data->hal_camera_device[device_id].channel[channel_id].pstFrame =
            (VPSS_FRAME_INFO_S *)malloc(sizeof(VPSS_FRAME_INFO_S));

    my_data->hal_camera_device[device_id].channel[channel_id].channel_buffer_release_flag = 0;
    my_data->hal_camera_device[device_id].channel[channel_id].num_ai_recognize = 0;

    return 0;
}




/**
 * Reads data from the camera stream
 * @param hal_camera Pointer to the camera data
 * @param channel_id The channel ID
 * @param buffer The buffer to read data into
 * @param bytes The number of bytes to read
 * @return The number of bytes read
 */

/*
 * Restore the original VPSS frame info that was overwritten when the frame was
 * rotated, so AK_MPI_VPSS_ReleaseFrame() gets the buffer it actually handed out.
 */
static void hal_camera_restore_frame_for_release(hal_camera_channel_t *ch)
{
    if (ch->frame_rotated && ch->pstFrame) {
        *ch->pstFrame = ch->vpss_frame_saved;
        ch->frame_rotated = 0;
    }
}

/*
 * Rotate the just-acquired VPSS frame with the IPU and repoint pstFrame at the
 * rotated DMA buffer, so both the encoder (reads pstFrame directly) and the YUV
 * broadcast path see the corrected orientation.
 * Returns 0 if pstFrame now points at a rotated buffer, negative otherwise
 * (caller then falls back to the original frame).
 */
static int hal_camera_apply_rotation(struct hal_camera_data *my_data,
                                     int device_id, int channel_id,
                                     VPSS_FRAME_INFO_S *pstFrame)
{
    hal_camera_channel_t *ch = &my_data->hal_camera_device[device_id].channel[channel_id];
    const hal_camera_config_t *cfg = hal_camera_get_config();
    int queue_id;
    int idx;
    int ret;
    size_t src_len;
    size_t dst_len;
    size_t need_len;

    if (ch->rotation == 0 || ch->rot_ipu_color_fmt < 0) {
        return -1;
    }

    if (pstFrame == NULL || pstFrame->stFrame.pu8Data == NULL || pstFrame->u64PhyAddr == 0) {
        log_warn("rotate: invalid VPSS frame dev=%d ch=%d data=%p phy=0x%llx",
                 device_id, channel_id, (void *)pstFrame->stFrame.pu8Data,
                 (unsigned long long)pstFrame->u64PhyAddr);
        return -1;
    }

    queue_id = hal_camera_get_rotate_ipu_queue();
    if (queue_id < 0) {
        return -1;
    }

    if (!ch->rot_ipu_opened) {
        if (hal_ipu_open() != 0) {
            log_error("rotate: hal_ipu_open failed dev=%d ch=%d", device_id, channel_id);
            return -1;
        }
        ch->rot_ipu_opened = 1;
    }

    src_len = hal_camera_frame_bytes(ch->rot_src_w, ch->rot_src_h, ch->rot_ipu_color_fmt);
    dst_len = hal_camera_frame_bytes(ch->rot_dst_w, ch->rot_dst_h, ch->rot_ipu_color_fmt);
    if (dst_len == 0) {
        dst_len = pstFrame->stFrame.u32Len;
    }
    need_len = dst_len > src_len ? dst_len : src_len;
    if (need_len == 0) {
        need_len = pstFrame->stFrame.u32Len;
    }

    if (ch->rot_buf[0] == NULL || ch->rot_buf_len < need_len) {
        for (int i = 0; i < 2; i++) {
            if (ch->rot_buf[i]) {
                AK_BASE_MemDmaFree(ch->rot_buf[i]);
                ch->rot_buf[i] = NULL;
                ch->rot_buf_phy[i] = 0;
            }
        }
        ch->rot_buf_len = need_len;
        for (int i = 0; i < 2; i++) {
            ch->rot_buf[i] = AK_BASE_MemDmaAlloc(cfg->ipu_module_id, ch->rot_buf_len);
            if (ch->rot_buf[i] == NULL) {
                log_error("rotate: dma alloc %u failed dev=%d ch=%d",
                          (unsigned)ch->rot_buf_len, device_id, channel_id);
                return -1;
            }
            if (AK_BASE_MemDmaVaddr2Paddr(ch->rot_buf[i], &ch->rot_buf_phy[i]) != AK_SUCCESS ||
                ch->rot_buf_phy[i] == 0) {
                log_error("rotate: vaddr2paddr failed dev=%d ch=%d buf=%p",
                          device_id, channel_id, ch->rot_buf[i]);
                AK_BASE_MemDmaFree(ch->rot_buf[i]);
                ch->rot_buf[i] = NULL;
                ch->rot_buf_phy[i] = 0;
                return -1;
            }
        }
        ch->rot_buf_idx = 0;
    }

    idx = ch->rot_buf_idx ^ 1;

    ret = hal_ipu_rotate_with_format(queue_id,
                                     pstFrame->u64PhyAddr, ch->rot_src_w, ch->rot_src_h, ch->rot_ipu_color_fmt,
                                     ch->rot_buf_phy[idx], ch->rot_dst_w, ch->rot_dst_h, ch->rot_ipu_color_fmt,
                                     ch->rotation);
    if (ret != 0) {
        log_warn("rotate: ipu rotate failed dev=%d ch=%d ret=%d", device_id, channel_id, ret);
        return -1;
    }

    /* Keep the raw VPSS frame for release; expose the rotated buffer to consumers. */
    ch->vpss_frame_saved = *pstFrame;
    ch->rot_buf_idx = idx;
    ch->frame_rotated = 1;
    pstFrame->stFrame.pu8Data = (AK_U8 *)ch->rot_buf[idx];
    pstFrame->u64PhyAddr = ch->rot_buf_phy[idx];
    if (dst_len > 0) {
        pstFrame->stFrame.u32Len = (AK_U32)dst_len;
    }

    hal_osd_ipu_composite_frame(channel_id, queue_id, ch->rot_buf_phy[idx], ch->rot_buf[idx],
                                ch->rot_dst_w, ch->rot_dst_h, ch->rot_ipu_color_fmt);
    return 0;
}

size_t hal_camera_stream_read(void *hal_camera, int device_id, int channel_id, void **buffer, uint64_t * timestamp,size_t bytes)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    hal_camera_channel_t *hal_channel = &my_data->hal_camera_device[device_id].channel[channel_id];
    int ret;

    VPSS_FRAME_INFO_S *pstFrame = hal_channel->pstFrame;;
    VPSS_FRAME_INFO_S next;

    /* Rotation reuses a ping-pong IPU buffer, so only unrotated channels prefetch. */
    if (hal_channel->rotation == 0 &&
        hal_pf_take(device_id, channel_id, &next, &ret)) {
        if (hal_channel->channel_buffer_release_flag == 1) {
            hal_channel->channel_buffer_release_flag = 0;
            AK_MPI_VPSS_ReleaseFrame(device_id, channel_id, pstFrame);
        }
        if (ret != 0)
            return 0;
        *pstFrame = next;
        hal_channel->channel_buffer_release_flag = 1;
        *buffer = pstFrame->stFrame.pu8Data;
        *timestamp = pstFrame->stFrame.u64Ts;
        return pstFrame->stFrame.u32Len;
    }

    if(hal_channel->channel_buffer_release_flag == 1)
    {
         hal_channel->channel_buffer_release_flag = 0;
//        log_info("release...device_id:%d,channel_id:%d,u64SeqNo:%d",device_id,channel_id,pstFrame->stFrame.u64SeqNo);
        hal_camera_restore_frame_for_release(hal_channel);
        AK_MPI_VPSS_ReleaseFrame(device_id, channel_id, pstFrame);
    }
    memset(pstFrame, 0x00, sizeof(VPSS_FRAME_INFO_S));
    ret = AK_MPI_VPSS_GetFrame(device_id, channel_id, pstFrame, 2000);

    hal_channel->channel_buffer_release_flag = 1;
    if (!ret)
    {
        if (hal_channel->rotation != 0) {
            if (hal_camera_apply_rotation(my_data, device_id, channel_id, pstFrame) != 0) {
                log_debug("rotate skipped: use original VPSS frame dev=%d ch=%d", device_id, channel_id);
            }
        }
        *buffer =  pstFrame->stFrame.pu8Data;
        *timestamp = pstFrame->stFrame.u64Ts;
        return pstFrame->stFrame.u32Len;
    }
    else
    {
        /* Channel teardown/transition may hit VPSS timeout intermittently.
         * Let upper loop retry/exit by status, avoid reporting as hard failure. */
        log_debug("get frame timeout/error: dev=%d ch=%d ret=%d", device_id, channel_id, ret);
        return 0;
    }
}

void hal_camera_set_torch_mode(const char* camera_id, bool on)
{


}


int hal_camera_get_number(void)
{
    const hal_camera_config_t *cfg = hal_camera_get_config();
    return cfg->camera_number;
}

/**
 * Gets camera information
 * @param camera_id The camera ID
 * @param info Pointer to the camera info structure to fill
 * @return 0 on success, -1 on failure
 */
int hal_camera_get_info(uint32_t device_id, struct camera_info *info)
{
    int ret = 0;

    info->orientation = 0;
    VI_RECTANGLE_S stViRectangle = {0};
    VI_DEV_ATTR_S stViDevAttr = {0};

 //   for(int device_id=0; device_id<HAL_CMAMERA_NUMBER; device_id++)
    {
    // Current interface has no return value to confirm if opened, so must call before open, otherwise VI device will be closed
        ret = AK_MPI_VI_CreateDev(device_id, &stViDevAttr);
        if(ret) {
            log_error("Create vi device fail");
            return -1;
        }

    // Since current interface requires calling GetSensorRes after CreateDev, use this approach
        ret = AK_MPI_VI_GetSensorRes(device_id, &stViRectangle);


        ret = AK_MPI_VI_DestroyDev(device_id);
        if(ret) {
            log_error("Destroy vi device fail .");
            return -1;
        }

        info->max_width = stViRectangle.u32Width;
        info->max_height = stViRectangle.u32Height;

    }
    return ret;
}

