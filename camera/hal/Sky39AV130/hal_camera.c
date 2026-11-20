// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/camera.
 *
 * HAL camera implementation for AnyCloud39AV100_V2 (Anyka PDK 2.x).
 *
 * Implementation reference (PDK samples, not product build scripts):
 *   - VI bring-up / channel map: PDK VI sample (ak_vi_sample.c)
 *     (ak_vi_open -> ak_vi_load_sensor_cfg -> ak_vi_get_sensor_resolution -> ak_vi_set_dev_attr
 *      -> ak_vi_set_chn_attr_ex -> ak_vi_enable_dev / ak_vi_enable_chn -> ak_vi_get_frame).
 *   - Multi-sensor: platform/sample/multi_CISs/ak_multi_CISs_sample.c as needed.
 *   - Flip/mirror: use ak_vpss_switch_flip_mirror() (VPSS), not VI dev attr fields.
 *
 * Public contract: hal_camera_api.h and camera_hw.c call into this HAL the same way as on
 * AnyCloud39AV200 (nova); only PDK calls and chip limits differ here.
 *
 * Note: Some policy still lives in camera_hw.c; migrate chip-independent logic there over time.
 *
 * VI shutdown order (see ak_vi_sample.c stop_vi):
 *   disable all registered VI channels -> ak_vi_disable_dev -> ak_vi_close
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>

#include "ak_log.h"
#include "ak_common.h"
#include "ak_vi.h"
#include "ak_vpss.h"

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

#include "../../camera_hw.h"
#include "../../hal_camera_api.h"

#define LOG_TAG "HAL CAMERA"

static void hal_camera_close_vi_device(hal_camera_device_t *hal_dev, int device_id)
{
    if (!hal_dev->vi_open) {
        return;
    }
    if (ak_vi_close(device_id) != AK_SUCCESS) {
        log_error("ak_vi_close fail dev=%d", device_id);
    } else {
        log_info("ak_vi_close success dev=%d", device_id);
    }
    hal_dev->vi_open = 0;
    hal_dev->vi_dev_enabled = 0;
}

/* Implemented in camera_hw.c (non-static), used by HAL API compatibility path. */
extern int camera_process_capture_request(const camera_hw_device_t *dev,
                                          camera_capture_request_t *request);

VI_CHN hal_camera_vi_chn(int device_id, int hal_channel_id)
{
    static const VI_CHN map[4][3] = {
        { VIDEO_CHN0, VIDEO_CHN1, VIDEO_CHN16 },
        { VIDEO_CHN2, VIDEO_CHN3, VIDEO_CHN17 },
        { VIDEO_CHN4, VIDEO_CHN5, VIDEO_CHN18 },
        { VIDEO_CHN6, VIDEO_CHN7, VIDEO_CHN19 },
    };

    if (device_id < 0 || device_id > 3)
        device_id = 0;
    if (hal_channel_id < 0 || hal_channel_id > 2)
        hal_channel_id = 0;
    return map[device_id][hal_channel_id];
}

VI_DATA_TYPE_E hal_camera_vi_data_type(pixel_format_t fmt)
{
    /*
     * Some platforms define aliases with identical numeric values, which makes
     * switch/case produce "duplicate case" build errors. Use if/else instead.
     */
    if (fmt == SYSTEM_PIXEL_FORMAT_NV12 ||
        fmt == SYSTEM_PIXEL_FORMAT_YCBCR_420_SP ||
        fmt == SYSTEM_PIXEL_FORMAT_NV21 ||
        fmt == SYSTEM_PIXEL_FORMAT_YCRCB_420_SP) {
        return VI_DATA_TYPE_YUV420SP;
    }

    if (fmt == SYSTEM_PIXEL_FORMAT_YV12 ||
        fmt == SYSTEM_PIXEL_FORMAT_YCBCR_420_P) {
        return VI_DATA_TYPE_YUV420P;
    }

    if (fmt == SYSTEM_PIXEL_FORMAT_RGB_888 ||
        fmt == SYSTEM_PIXEL_FORMAT_BGR_888) {
        return VI_DATA_TYPE_RGB;
    }

    if (fmt == SYSTEM_PIXEL_FORMAT_RAW8 ||
        fmt == SYSTEM_PIXEL_FORMAT_RAW10 ||
        fmt == SYSTEM_PIXEL_FORMAT_RAW12 ||
        fmt == SYSTEM_PIXEL_FORMAT_RAW16) {
        return VI_DATA_TYPE_RAW;
    }

    if (fmt == SYSTEM_PIXEL_FORMAT_RGB_LINEINTL) {
        return VI_DATA_TYPE_RGB_LINEINTL;
    }

    /*
     * AV100_V2 VI only exposes RGB_LINEINTL. For upper layers that request RGB_HWC/CHW,
     * map them to RGB_LINEINTL so VI returns RGB instead of silently falling back to NV12.
     */
    if (fmt == SYSTEM_PIXEL_FORMAT_RGB_HWC ||
        fmt == SYSTEM_PIXEL_FORMAT_RGB_CHW) {
        return VI_DATA_TYPE_RGB_LINEINTL;
    }

    return VI_DATA_TYPE_YUV420SP;
}

void hal_camera_export_frame(VPSS_FRAME_INFO_S *dst, const struct video_input_frame *vin)
{
    if (!dst || !vin)
        return;
    memset(dst, 0, sizeof(*dst));
    dst->stFrame.pu8Data = vin->vi_frame.data;
    dst->stFrame.u32Len = vin->vi_frame.len;
    dst->stFrame.u64Ts = vin->vi_frame.ts;
    dst->stFrame.u64SeqNo = vin->vi_frame.seq_no;
    dst->u64PhyAddr = (unsigned long long)vin->phyaddr;
    dst->pvMdInfo = vin->mdinfo;
    dst->enOutputPixFmt = vin->type;
}

/**
 * Derive VI_DEV_ATTR main/sub max from configured streams (ak_vi_sample.c sets sub_max
 * from sub channel size, not sensor full size).
 */
static void hal_camera_dev_attr_res_from_streams(struct camera_device *cdev, int device_id,
        int *main_w, int *main_h, int *sub_w, int *sub_h)
{
    int mw = 0, mh = 0, sw = 0, sh = 0;

    if (main_w) {
        *main_w = 0;
    }
    if (main_h) {
        *main_h = 0;
    }
    if (sub_w) {
        *sub_w = 0;
    }
    if (sub_h) {
        *sub_h = 0;
    }
    if (cdev == NULL) {
        return;
    }

    for (int i = 0; i < cdev->num_device; i++) {
        device_info_t *dev = &cdev->device_info[i];
        if (dev->device_id != device_id) {
            continue;
        }
        for (int ch = 0; ch < dev->num_channel; ch++) {
            int w = dev->channel_info[ch].channel_config.width;
            int h = dev->channel_info[ch].channel_config.height;
            if (w <= 0 || h <= 0) {
                continue;
            }
            if (ch == 0) {
                if (w > mw) {
                    mw = w;
                }
                if (h > mh) {
                    mh = h;
                }
            } else {
                if (w > sw) {
                    sw = w;
                }
                if (h > sh) {
                    sh = h;
                }
            }
        }
        break;
    }

    if (main_w) {
        *main_w = mw;
    }
    if (main_h) {
        *main_h = mh;
    }
    if (sub_w) {
        *sub_w = sw;
    }
    if (sub_h) {
        *sub_h = sh;
    }
}

static int hal_camera_even_dim(int v)
{
    if (v < HAL_VI_DEV_SUB_MIN_WIDTH) {
        v = HAL_VI_DEV_SUB_MIN_WIDTH;
    }
    return (v + 1) & ~1;
}

static void hal_camera_clamp_sub_dim(int *sub_w, int *sub_h)
{
    int sw;
    int sh;

    if (sub_w == NULL || sub_h == NULL) {
        return;
    }
    sw = *sub_w;
    sh = *sub_h;
    if (sw <= 0 || sh <= 0) {
        return;
    }
    sw = hal_camera_even_dim(sw);
    sh = hal_camera_even_dim(sh);
    if (sw > HAL_VI_DEV_SUB_MAX_WIDTH) {
        sw = HAL_VI_DEV_SUB_MAX_WIDTH;
    }
    if (sh > HAL_VI_DEV_SUB_MAX_HEIGHT) {
        sh = HAL_VI_DEV_SUB_MAX_HEIGHT;
    }
    if (sw < HAL_VI_DEV_SUB_MIN_WIDTH) {
        sw = HAL_VI_DEV_SUB_MIN_WIDTH;
    }
    if (sh < HAL_VI_DEV_SUB_MIN_HEIGHT) {
        sh = HAL_VI_DEV_SUB_MIN_HEIGHT;
    }
    *sub_w = sw;
    *sub_h = sh;
}

/** PDK sub_max for main-only (scaled from main, within main[2880] sub[1280] min[18]). */
static void hal_camera_vi_sub_scale_from_main(int main_w, int main_h, int *sub_w, int *sub_h)
{
    const int cap_w = 1280;
    int sw;
    int sh;

    if (sub_w) {
        *sub_w = 0;
    }
    if (sub_h) {
        *sub_h = 0;
    }
    if (main_w <= 0 || main_h <= 0) {
        if (sub_w) {
            *sub_w = HAL_VI_DEV_SUB_MIN_WIDTH;
        }
        if (sub_h) {
            *sub_h = HAL_VI_DEV_SUB_MIN_HEIGHT;
        }
        return;
    }
    if (main_w <= cap_w) {
        sw = main_w;
        sh = main_h;
    } else {
        sw = cap_w;
        sh = (int)((int64_t)main_h * cap_w / main_w);
    }
    hal_camera_clamp_sub_dim(&sw, &sh);
    if (sub_w) {
        *sub_w = sw;
    }
    if (sub_h) {
        *sub_h = sh;
    }
}

/**
 * Configured crop from streams (test/HAL channel_config.crop).
 */
static void hal_camera_cfg_crop_from_streams(struct camera_device *cdev, int device_id,
        int sensor_w, int sensor_h, int *crop_w, int *crop_h)
{
    int cw = sensor_w;
    int ch = sensor_h;

    if (crop_w) {
        *crop_w = 0;
    }
    if (crop_h) {
        *crop_h = 0;
    }
    if (cdev == NULL) {
        return;
    }

    for (int i = 0; i < cdev->num_device; i++) {
        device_info_t *dev = &cdev->device_info[i];
        if (dev->device_id != device_id) {
            continue;
        }
        for (int chn = 0; chn < dev->num_channel; chn++) {
            camera_channel_config_t *cc = &dev->channel_info[chn].channel_config;
            if (cc->crop.width > 0 && cc->crop.height > 0) {
                cw = cc->crop.width;
                ch = cc->crop.height;
                goto done;
            }
        }
        break;
    }

done:
    if (cw > sensor_w) {
        cw = sensor_w;
    }
    if (ch > sensor_h) {
        ch = sensor_h;
    }
    if (crop_w) {
        *crop_w = cw;
    }
    if (crop_h) {
        *crop_h = ch;
    }
}

/**
 * VI 3DNR memsize (KB) from VICAP crop, applied before set_dev_attr.
 * Size = min(crop, HAL_VI_3DNR_REF_MAX_*) NV12 * NUM/DEN, then clamp to
 * [HAL_VI_3DNR_KB_DRIVER_MIN, HAL_VI_3DNR_KB_DRIVER_MAX] (see hal_camera_chip_config.h).
 */
static int hal_camera_vi_3dnr_memsize_kb(int crop_w, int crop_h)
{
    int eff_w;
    int eff_h;
    uint64_t bytes;
    int kb;

    if (crop_w <= 0 || crop_h <= 0) {
        return 0;
    }

    eff_w = crop_w;
    eff_h = crop_h;
    if (eff_w > HAL_VI_3DNR_REF_MAX_WIDTH) {
        eff_w = HAL_VI_3DNR_REF_MAX_WIDTH;
    }
    if (eff_h > HAL_VI_3DNR_REF_MAX_HEIGHT) {
        eff_h = HAL_VI_3DNR_REF_MAX_HEIGHT;
    }
    eff_w = hal_camera_even_dim(eff_w);
    eff_h = hal_camera_even_dim(eff_h);

    bytes = (uint64_t)eff_w * (uint64_t)eff_h * 3u / 2u;
    bytes = bytes * (uint64_t)HAL_VI_3DNR_NV12_NUM / (uint64_t)HAL_VI_3DNR_NV12_DEN;
    kb = (int)((bytes + 1023u) / 1024u);
    if (kb < (int)HAL_VI_3DNR_KB_DRIVER_MIN) {
        kb = (int)HAL_VI_3DNR_KB_DRIVER_MIN;
    }
    if (kb > (int)HAL_VI_3DNR_KB_DRIVER_MAX) {
        kb = (int)HAL_VI_3DNR_KB_DRIVER_MAX;
    }
    return kb;
}

/** VICAP crop from channel_config.crop (test/app); only even-align and sensor clamp. */
static void hal_camera_vicap_crop_resolve(int sensor_w, int sensor_h, int cfg_crop_w, int cfg_crop_h,
        int *vicap_w, int *vicap_h)
{
    int vw = cfg_crop_w > 0 ? cfg_crop_w : sensor_w;
    int vh = cfg_crop_h > 0 ? cfg_crop_h : sensor_h;

    if (vw > sensor_w) {
        vw = sensor_w;
    }
    if (vh > sensor_h) {
        vh = sensor_h;
    }
    vw = hal_camera_even_dim(vw);
    vh = hal_camera_even_dim(vh);
    if (vicap_w) {
        *vicap_w = vw;
    }
    if (vicap_h) {
        *vicap_h = vh;
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
    const hal_camera_config_t *cfg = hal_camera_get_config();

    log_info("VI version: %s", ak_vi_get_version());
    log_info("SVP2 version: %s", ak_svp2_get_version());
    if (cfg) {
        log_info("camera_number: %d, isp0: %s", cfg->camera_number, cfg->isp_config_path[0]);
    }

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



/**
 * @brief Create camera device.
 *
 * Mirrors ak_vi_sample.c start_vi() core order: open -> load_sensor_cfg -> get_sensor_resolution
 * -> set_dev_attr. Channel resolution / ak_vi_set_chn_attr_ex is done in hal_camera_create_channel
 * from camera_hw stream config (same role as sample main/sub/third chn setup).
 */
int hal_camera_create_device(void *hal_camera, int device_id)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    int ret;
    RECTANGLE_S sensor_res = {0};
    VI_DEV_ATTR dev_attr;
    hal_camera_device_t *hal_camera_device = &my_data->hal_camera_device[device_id];
    const hal_camera_config_t *cfg = hal_camera_get_config();
    int cam_idx = device_id;

    if (cam_idx < 0 || cam_idx >= cfg->camera_number || cam_idx >= CAMERA_MAX_NUM) {
        cam_idx = 0;
    }

    log_debug("hal_camera_create_device, device_id:%d", device_id);

    hal_camera_device->vi_channel_id = 0;
    hal_camera_device->vpss_group_id = device_id;
    hal_camera_device->flip_enabled = cfg->flip[device_id];
    hal_camera_device->mirror_enabled = cfg->mirror[device_id];
    hal_camera_device->vi_hal_chn_registered = 0;
    hal_camera_device->vi_dev_sub_max_w = 0;
    hal_camera_device->vi_dev_sub_max_h = 0;
    hal_camera_device->vi_main_only_extra_vi_ch1 = 0;
    hal_camera_device->vi_open = 0;
    hal_camera_device->vi_dev_enabled = 0;

    ret = ak_vi_open(device_id);
    if (ret != AK_SUCCESS) {
        log_error("ak_vi_open fail dev=%d ret=%d", device_id, ret);
        return -1;
    }
    hal_camera_device->vi_open = 1;

    /* PDK: AE defaults before ak_vi_load_sensor_cfg. */
    {
        struct vpss_isp_ae_init_info ae_init_info;

        memset(&ae_init_info, 0, sizeof(ae_init_info));
        ae_init_info.a_gain = 256;
        ae_init_info.d_gain = 256;
        ae_init_info.exp_time = 2154;
        ae_init_info.isp_d_gain = 256;
        (void)ak_vpss_set_ae_init_info(device_id, &ae_init_info);
    }

    {
        const char *isp_path = cfg->isp_config_path[cam_idx];
        struct stat st;
        int sensor_id = 0;

        if (isp_path && stat(isp_path, &st) == 0)
            log_info("isp cfg %s size=%ld bytes", isp_path, (long)st.st_size);
        else
            log_error("isp cfg missing or unreadable: %s", isp_path ? isp_path : "(null)");

        if (ak_vi_get_sensor_id(device_id, &sensor_id) == 0)
            log_info("vi sensor_id before load_cfg: 0x%x", sensor_id);
        else
            log_warn("ak_vi_get_sensor_id failed before load_sensor_cfg (dev=%d)", device_id);
    }

    ret = ak_vi_load_sensor_cfg(device_id, cfg->isp_config_path[cam_idx]);
    if (ret != AK_SUCCESS) {
        log_error("ak_vi_load_sensor_cfg fail dev=%d ret=%d path=%s (check hal_camera_config.ini / ISP file)",
                  device_id, ret, cfg->isp_config_path[cam_idx]);
        hal_camera_close_vi_device(hal_camera_device, device_id);
        return -1;
    }

    ret = ak_vi_get_sensor_resolution(device_id, &sensor_res);
    if (ret != AK_SUCCESS) {
        log_error("ak_vi_get_sensor_resolution fail dev=%d ret=%d", device_id, ret);
        hal_camera_close_vi_device(hal_camera_device, device_id);
        return -1;
    }

    {
        int main_w = 0, main_h = 0, sub_w = 0, sub_h = 0;

        hal_camera_dev_attr_res_from_streams(my_data->cdev, device_id,
                &main_w, &main_h, &sub_w, &sub_h);

        int cfg_crop_w = sensor_res.width;
        int cfg_crop_h = sensor_res.height;
        int vicap_w = 0;
        int vicap_h = 0;

        memset(&dev_attr, 0, sizeof(dev_attr));
        dev_attr.dev_id = device_id;
        dev_attr.interf_mode = (VI_INTF_MODE_E)cfg->interface_mode[device_id];
        dev_attr.data_path = VI_PATH_ISP;
        dev_attr.data_type = VI_DATA_TYPE_YUV420SP;
        hal_camera_cfg_crop_from_streams(my_data->cdev, device_id,
                sensor_res.width, sensor_res.height, &cfg_crop_w, &cfg_crop_h);
        hal_camera_vicap_crop_resolve(sensor_res.width, sensor_res.height,
                cfg_crop_w, cfg_crop_h, &vicap_w, &vicap_h);
        hal_camera_device->vicap_crop_w = vicap_w;
        hal_camera_device->vicap_crop_h = vicap_h;
        dev_attr.crop.left = 0;
        dev_attr.crop.top = 0;
        dev_attr.crop.width = vicap_w;
        dev_attr.crop.height = vicap_h;
        if (main_w > 0 && main_h > 0) {
            dev_attr.max_width = main_w;
            dev_attr.max_height = main_h;
        } else {
            dev_attr.max_width = sensor_res.width;
            dev_attr.max_height = sensor_res.height;
        }
        if (dev_attr.max_width > sensor_res.width) {
            dev_attr.max_width = sensor_res.width;
        }
        if (dev_attr.max_height > sensor_res.height) {
            dev_attr.max_height = sensor_res.height;
        }
        if (dev_attr.max_width > HAL_VI_DEV_MAIN_MAX_WIDTH) {
            dev_attr.max_width = HAL_VI_DEV_MAIN_MAX_WIDTH;
        }
        if (dev_attr.max_height > HAL_VI_DEV_MAIN_MAX_HEIGHT) {
            dev_attr.max_height = HAL_VI_DEV_MAIN_MAX_HEIGHT;
        }

        if (sub_w > 0 && sub_h > 0) {
            dev_attr.sub_max_width = sub_w;
            dev_attr.sub_max_height = sub_h;
            if (dev_attr.sub_max_width > HAL_VI_DEV_SUB_MAX_WIDTH) {
                dev_attr.sub_max_width = HAL_VI_DEV_SUB_MAX_WIDTH;
            }
            if (dev_attr.sub_max_height > HAL_VI_DEV_SUB_MAX_HEIGHT) {
                dev_attr.sub_max_height = HAL_VI_DEV_SUB_MAX_HEIGHT;
            }
            if (dev_attr.sub_max_width < HAL_VI_DEV_SUB_MIN_WIDTH ||
                dev_attr.sub_max_height < HAL_VI_DEV_SUB_MIN_HEIGHT) {
                dev_attr.sub_max_width = HAL_VI_DEV_SUB_MIN_WIDTH;
                dev_attr.sub_max_height = HAL_VI_DEV_SUB_MIN_HEIGHT;
                log_warn("vi dev_attr dev=%d: sub_max invalid, clamped to %dx%d",
                         device_id, dev_attr.sub_max_width, dev_attr.sub_max_height);
            }
        } else {
            int ph_w = 0;
            int ph_h = 0;

            hal_camera_vi_sub_scale_from_main(main_w, main_h, &ph_w, &ph_h);
            dev_attr.sub_max_width = ph_w;
            dev_attr.sub_max_height = ph_h;
            hal_camera_device->vi_main_only_extra_vi_ch1 = 1;
            log_info("vi dev_attr dev=%d: main-only sub_max %dx%d",
                     device_id, ph_w, ph_h);
        }
        hal_camera_device->vi_dev_sub_max_w = dev_attr.sub_max_width;
        hal_camera_device->vi_dev_sub_max_h = dev_attr.sub_max_height;
        dev_attr.frame_rate = cfg->max_fps[device_id];

        log_info("vi dev_attr dev=%d sensor=%dx%d main_max=%dx%d sub_max=%dx%d vi_only=%d vicap_crop=%dx%d cfg_crop=%dx%d",
                 device_id, sensor_res.width, sensor_res.height,
                 dev_attr.max_width, dev_attr.max_height,
                 dev_attr.sub_max_width, dev_attr.sub_max_height,
                 (sub_w > 0 && sub_h > 0) ? 0 : 1,
                 vicap_w, vicap_h, cfg_crop_w, cfg_crop_h);

        {
            const char *e3 = getenv("HAL_CAMERA_VI_3DNR_KB");
            int skip_3dnr = (e3 != NULL && e3[0] == '0' && e3[1] == '\0');
            int mem_kb = 0;

            if (skip_3dnr) {
                log_info("vi 3dnr dev=%d: HAL_CAMERA_VI_3DNR_KB=0, skip ak_vi_set_dev_3DNR_memsize "
                         "(PDK default, ak_vi_sample)",
                         device_id);
            } else if (e3 != NULL && e3[0] != '\0') {
                mem_kb = atoi(e3);
                if (mem_kb <= 0) {
                    mem_kb = hal_camera_vi_3dnr_memsize_kb(cfg_crop_w, cfg_crop_h);
                } else {
                    if (mem_kb < (int)HAL_VI_3DNR_KB_DRIVER_MIN) {
                        mem_kb = (int)HAL_VI_3DNR_KB_DRIVER_MIN;
                    }
                    if (mem_kb > (int)HAL_VI_3DNR_KB_DRIVER_MAX) {
                        mem_kb = (int)HAL_VI_3DNR_KB_DRIVER_MAX;
                    }
                }
            } else {
                mem_kb = hal_camera_vi_3dnr_memsize_kb(cfg_crop_w, cfg_crop_h);
            }
            if (!skip_3dnr && mem_kb > 0) {
                int rc = ak_vi_set_dev_3DNR_memsize((VI_DEV)device_id, mem_kb);
                if (rc != AK_SUCCESS) {
                    log_warn("ak_vi_set_dev_3DNR_memsize dev=%d kb=%d ret=%d (continue with PDK default)",
                             device_id, mem_kb, rc);
                } else {
                    log_info("vi 3dnr memsize dev=%d set to %d KB (crop %dx%d eff<=%dx%d NV12*%u/%u, "
                             "driver range [%u,%u] KB)",
                             device_id, mem_kb, cfg_crop_w, cfg_crop_h,
                             HAL_VI_3DNR_REF_MAX_WIDTH, HAL_VI_3DNR_REF_MAX_HEIGHT,
                             (unsigned)HAL_VI_3DNR_NV12_NUM, (unsigned)HAL_VI_3DNR_NV12_DEN,
                             (unsigned)HAL_VI_3DNR_KB_DRIVER_MIN,
                             (unsigned)HAL_VI_3DNR_KB_DRIVER_MAX);
                }
            } else if (!skip_3dnr) {
                log_debug("vi 3dnr memsize dev=%d: crop invalid, using PDK default", device_id);
            }
        }
    }

    ret = ak_vi_set_dev_attr(device_id, &dev_attr);
    if (ret != AK_SUCCESS) {
        log_error("ak_vi_set_dev_attr fail dev=%d ret=%d", device_id, ret);
        hal_camera_close_vi_device(hal_camera_device, device_id);
        return -1;
    }

    /*
     * Flip/mirror: use ak_vpss_switch_flip_mirror() (see ak_vpss.h), not VI_DEV_ATTR
     * fields used on some other chips/PDK variants.
     *
     * Always call this once after device bring-up so the state matches the
     * current config (a previous reload may have left a different state set
     * on the sensor / VPSS).  Failure is non-fatal so VI bring-up still works
     * on sensors that do not support the operation.
     */
    {
        int flip_en   = hal_camera_device->flip_enabled ? 1 : 0;
        int mirror_en = hal_camera_device->mirror_enabled ? 1 : 0;
        int rc = ak_vpss_switch_flip_mirror(device_id, flip_en, mirror_en);
        if (rc != AK_SUCCESS) {
            log_warn("ak_vpss_switch_flip_mirror fail dev=%d flip=%d mirror=%d ret=%d (continue)",
                     device_id, flip_en, mirror_en, rc);
        } else {
            log_info("camera device[%d] flip=%d mirror=%d applied",
                     device_id, flip_en, mirror_en);
        }
    }

    log_info("camera device[%d] created (vi open + dev attr)", device_id);
    return 0;
}


int hal_camera_create_channel(void *hal_camera, int device_id, int channel_id)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    int ret;
    /* VI_CHN mapping matches ak_vi_sample.c (VIDEO_CHN0/1/16 per dev, etc.). */
    VI_CHN vi_chn = hal_camera_vi_chn(device_id, channel_id);
    VI_CHN_ATTR_EX chn_ex;
    hal_camera_device_t *hal_camera_device = &my_data->hal_camera_device[device_id];
    device_info_t *device_info = &my_data->cdev->device_info[device_id];
    camera_stream_t *stream = device_info->channel_info[channel_id].stream_info[0].stream;
    camera_channel_config_t *channel_config = &device_info->channel_info[channel_id].channel_config;
    const int ns = stream->num_slices;
    int vi_frame_depth;
    VI_CHN_MODE_E vi_mode;

    memset(&chn_ex, 0, sizeof(chn_ex));
    chn_ex.chn_id = vi_chn;
    chn_ex.res.width = channel_config->width;
    chn_ex.res.height = channel_config->height;
    chn_ex.max_res.width = channel_config->width;
    chn_ex.max_res.height = channel_config->height;
    chn_ex.frame_rate = 0;
    chn_ex.data_type = hal_camera_vi_data_type(stream->format);

    /*
     * num_slices <= 1 -> FRAME_MODE; num_slices > 1 -> SLICE_MODE.
     *
     * frame_depth is the channel's buffer-list depth in BOTH modes (ak_vi.h: "frame buffer
     * number of the channel", stated unconditionally) — VI_CHN_ATTR_EX has no slice-count
     * field at all, and the vendor reference (aas_vi_venc_binder.c) sets mode=SLICE_MODE
     * with frame_depth=1..4 as a plain depth. num_slices is therefore only a request to
     * switch the channel into slice mode; it never carries a count.
     *
     * What slice mode actually changes: the kernel feeds VI slices directly into the
     * encoder bound to this VI channel (KERNEL_ENCODE_MODE), so no full YUV frame is ever
     * mapped into userspace. The capture path for such a channel must pull the bitstream
     * with ak_app_video_venc_get_stream() instead of ak_vi_get_frame() +
     * ak_venc_encode_frame() — see hal_camera_codec.c and hal_camera_stream_read().
     */
    vi_frame_depth = channel_config->max_buffers > 0 ? (int)channel_config->max_buffers : 3;
    vi_mode = (ns > 1) ? SLICE_MODE : FRAME_MODE;
    log_debug("VI dev=%d ch=%d: %s depth=%d (num_slices=%d)",
              device_id, channel_id, vi_mode == SLICE_MODE ? "SLICE_MODE" : "FRAME_MODE",
              vi_frame_depth, ns <= 0 ? 1 : ns);
    chn_ex.mode = vi_mode;
    chn_ex.frame_depth = vi_frame_depth;

    if (hal_camera_device->vicap_crop_w > 0 && hal_camera_device->vicap_crop_h > 0) {
        chn_ex.crop.left = 0;
        chn_ex.crop.top = 0;
        chn_ex.crop.width = hal_camera_device->vicap_crop_w;
        chn_ex.crop.height = hal_camera_device->vicap_crop_h;
    } else if (channel_config->crop.width != 0 && channel_config->crop.height != 0) {
        chn_ex.crop.left = channel_config->crop.x;
        chn_ex.crop.top = channel_config->crop.y;
        chn_ex.crop.width = channel_config->crop.width;
        chn_ex.crop.height = channel_config->crop.height;
    } else {
        chn_ex.crop.left = 0;
        chn_ex.crop.top = 0;
        chn_ex.crop.width = channel_config->width;
        chn_ex.crop.height = channel_config->height;
    }

    ret = ak_vi_set_chn_attr_ex(vi_chn, &chn_ex);
    if (ret != AK_SUCCESS) {
        log_error("ak_vi_set_chn_attr_ex fail dev=%d hal_ch=%d vi_chn=%d ret=%d depth=%d mode=%d",
                  device_id, channel_id, vi_chn, ret, chn_ex.frame_depth, (int)chn_ex.mode);
        return -1;
    }
    log_info("vi chn dev=%d hal_ch=%d vi_chn=%d %dx%d crop=%dx%d mode=%s depth=%d fmt=%d",
             device_id, channel_id, vi_chn,
             chn_ex.res.width, chn_ex.res.height,
             chn_ex.crop.width, chn_ex.crop.height,
             chn_ex.mode == SLICE_MODE ? "SLICE" : "FRAME", chn_ex.frame_depth,
             (int)stream->format);

    if (channel_id >= 0 && channel_id < 3) {
        my_data->hal_camera_device[device_id].vi_hal_chn_registered |= (1u << channel_id);
    }
    /*
     * Latch the mode the channel was actually configured with. Everything downstream
     * (stream_read, codec enable/disable, quiesce) branches on this rather than re-reading
     * num_slices, so the capture path can never disagree with what VI was told.
     */
    hal_camera_device->channel[channel_id].slice_mode = (vi_mode == SLICE_MODE) ? 1 : 0;
    hal_camera_device->channel[channel_id].slice_vi_chn = (int)vi_chn;
    /*
     * A slice channel produces a bitstream only. Any consumer that needs the YUV itself
     * (codec_mode YUV/RAW: the NPU feed, snapshots taken from raw frames) has to live on a
     * non-slice channel — on some boards the AI feed is group 2 and JPEG snapshots are
     * encoded from the sub group. Warn loudly instead of silently starving such a stream,
     * since hal_camera_stream_read() will never hand it a frame.
     */
    if (hal_camera_device->channel[channel_id].slice_mode) {
        channel_info_t *ci = &device_info->channel_info[channel_id];
        for (int s = 0; s < ci->num_stream; s++) {
            camera_stream_t *st = ci->stream_info[s].stream;
            if (st == NULL) {
                continue;
            }
            if (st->codec_config.codec_mode == CAMERA_CODEC_MODE_YUV ||
                st->codec_config.codec_mode == CAMERA_CODEC_MODE_RAW) {
                log_error("dev=%d ch=%d is SLICE_MODE (num_slices=%d) but stream %d wants raw "
                          "%s: no YUV frame reaches userspace in slice mode. Move it to another "
                          "channel_group_id or set num_slices=1 on this group.",
                          device_id, channel_id, ns, s,
                          st->codec_config.codec_mode == CAMERA_CODEC_MODE_YUV ? "YUV" : "RAW");
            }
        }
    }
    if (channel_id == 1) {
        my_data->hal_camera_device[device_id].vi_main_only_extra_vi_ch1 = 0;
    }
    return 0;
}

/*
 * Main-only: PDK needs VI ch1 attr when sub_max is set, but the app only captures ch0.
 */
int hal_camera_sync_platform_vi_after_channels(void *hal_camera, int device_id)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    hal_camera_device_t *hal_dev;
    VI_CHN vi_sub = hal_camera_vi_chn(device_id, 1);
    VI_CHN_ATTR_EX chn_ex;
    int sub_w;
    int sub_h;
    int ret;

    if (my_data == NULL || my_data->cdev == NULL) {
        return -1;
    }

    hal_dev = &my_data->hal_camera_device[device_id];
    if (!hal_dev->vi_main_only_extra_vi_ch1) {
        return 0;
    }
    if ((hal_dev->vi_hal_chn_registered & (1u << 1)) != 0) {
        return 0;
    }

    sub_w = hal_dev->vi_dev_sub_max_w;
    sub_h = hal_dev->vi_dev_sub_max_h;
    if (sub_w <= 0 || sub_h <= 0) {
        log_warn("vi ch1 extra dev=%d: sub_max unknown, skip", device_id);
        return 0;
    }

    memset(&chn_ex, 0, sizeof(chn_ex));
    chn_ex.chn_id = vi_sub;
    chn_ex.res.width = sub_w;
    chn_ex.res.height = sub_h;
    chn_ex.max_res.width = sub_w;
    chn_ex.max_res.height = sub_h;
    chn_ex.frame_depth = 2;
    chn_ex.frame_rate = 0;
    chn_ex.data_type = VI_DATA_TYPE_YUV420SP;
    chn_ex.mode = FRAME_MODE;
    chn_ex.crop.left = 0;
    chn_ex.crop.top = 0;
    if (hal_dev->vicap_crop_w > 0 && hal_dev->vicap_crop_h > 0) {
        chn_ex.crop.width = hal_dev->vicap_crop_w;
        chn_ex.crop.height = hal_dev->vicap_crop_h;
    } else {
        chn_ex.crop.width = sub_w;
        chn_ex.crop.height = sub_h;
    }

    log_debug("vi ch1 extra dev=%d vi_chn=%d %dx%d depth=%d",
              device_id, vi_sub, sub_w, sub_h, chn_ex.frame_depth);

    ret = ak_vi_set_chn_attr_ex(vi_sub, &chn_ex);
    if (ret != AK_SUCCESS) {
        log_error("vi ch1 extra attr fail dev=%d vi_chn=%d ret=%d",
                  device_id, vi_sub, ret);
        return -1;
    }

    hal_dev->vi_hal_chn_registered |= (1u << 1);
    return 0;
}

/*
 * AV200: enable_device only starts VI/VPSS group; each VPSS_EnableChn is in enable_channel.
 * AV100 PDK: enable_dev may also start every registered VI chn. For main-only we register
 * a placeholder ch1 (sub_max) but never call enable_channel(1) — stop only that chn here.
 */
static void hal_camera_vi_quiet_placeholder_ch1(struct hal_camera_data *my_data, int device_id)
{
    hal_camera_device_t *hal_dev = &my_data->hal_camera_device[device_id];
    VI_CHN vi_sub;
    int rc;

    if (!hal_dev->vi_main_only_extra_vi_ch1) {
        return;
    }

    vi_sub = hal_camera_vi_chn(device_id, 1);
    rc = ak_vi_disable_chn(vi_sub);
    if (rc != AK_SUCCESS) {
        log_warn("vi ch1 placeholder disable fail dev=%d vi_chn=%d ret=%d",
                 device_id, vi_sub, rc);
    } else {
        log_debug("vi ch1 placeholder disabled dev=%d vi_chn=%d", device_id, vi_sub);
    }
}

int hal_camera_enable_device(void *hal_camera, int device_id)
{
    int ret;
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    hal_camera_device_t *hal_dev = &my_data->hal_camera_device[device_id];

    log_debug("hal_camera_enable_device dev=%d vi_chn_mask=0x%x",
              device_id, hal_dev->vi_hal_chn_registered);

    ret = hal_camera_sync_platform_vi_after_channels(hal_camera, device_id);
    if (ret != 0) {
        log_error("hal_camera_sync_platform_vi_after_channels fail dev=%d ret=%d",
                  device_id, ret);
        return -1;
    }

    ret = ak_vi_enable_dev(device_id);
    if (ret != AK_SUCCESS) {
        log_error("ak_vi_enable_dev fail dev=%d ret=%d", device_id, ret);
        return -1;
    }
    hal_dev->vi_dev_enabled = 1;

    hal_camera_vi_quiet_placeholder_ch1(my_data, device_id);

    return 0;
}

static void hal_camera_disable_vi_chns_registered(hal_camera_device_t *hal_dev, int device_id)
{
    for (int hal_ch = 0; hal_ch < 3; hal_ch++) {
        if ((hal_dev->vi_hal_chn_registered & (1u << hal_ch)) == 0) {
            continue;
        }
        VI_CHN vi_chn = hal_camera_vi_chn(device_id, hal_ch);
        int ret = ak_vi_disable_chn(vi_chn);
        if (ret != AK_SUCCESS) {
            log_warn("ak_vi_disable_chn vi_chn=%d ret=%d (continue)", vi_chn, ret);
        }
        hal_dev->vi_hal_chn_registered &= ~(1u << hal_ch);
    }
}

int hal_camera_disable_device(void *hal_camera, int device_id)
{
    int ret;
    int final_ret = 0;
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    hal_camera_device_t *hal_dev = &my_data->hal_camera_device[device_id];

    log_info("disable device:%d vi_chn_mask=0x%x vi_open=%d vi_dev_enabled=%d",
             device_id, hal_dev->vi_hal_chn_registered,
             hal_dev->vi_open, hal_dev->vi_dev_enabled);

    if (hal_dev->vi_dev_enabled) {
        hal_camera_disable_vi_chns_registered(hal_dev, device_id);
        ret = ak_vi_disable_dev(device_id);
        if (ret != AK_SUCCESS) {
            log_error("ak_vi_disable_dev fail dev=%d ret=%d", device_id, ret);
            if (final_ret == 0) {
                final_ret = -1;
            }
        } else {
            hal_dev->vi_dev_enabled = 0;
        }
    } else if (hal_dev->vi_hal_chn_registered != 0) {
        hal_camera_disable_vi_chns_registered(hal_dev, device_id);
    }

    if (hal_dev->vi_open) {
        hal_camera_close_vi_device(hal_dev, device_id);
    }

    hal_dev->vi_hal_chn_registered = 0;
    hal_dev->vi_dev_sub_max_w = 0;
    hal_dev->vi_dev_sub_max_h = 0;
    hal_dev->vi_main_only_extra_vi_ch1 = 0;
    hal_dev->vicap_crop_w = 0;
    hal_dev->vicap_crop_h = 0;

    for (int hal_ch = 0; hal_ch < CAMERA_MAX_CHANNEL; hal_ch++) {
        hal_camera_channel_t *chn = &hal_dev->channel[hal_ch];
        if (chn->pstFrame != NULL) {
            free(chn->pstFrame);
            chn->pstFrame = NULL;
        }
        chn->channel_buffer_release_flag = 0;
        memset(&chn->pending_vin_frame, 0, sizeof(chn->pending_vin_frame));
    }

    return final_ret;
}

static void hal_camera_release_pending_frame(void *hal_camera, int device_id, int channel_id)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    hal_camera_device_t *hal_dev;
    hal_camera_channel_t *chn;
    VI_CHN vi_chn;

    if (my_data == NULL) {
        return;
    }
    hal_dev = &my_data->hal_camera_device[device_id];
    chn = &my_data->hal_camera_device[device_id].channel[channel_id];
    if (chn->channel_buffer_release_flag != 1) {
        return;
    }

    vi_chn = hal_camera_vi_chn(device_id, channel_id);
    chn->channel_buffer_release_flag = 0;
    /*
     * ak_vi_release_frame() requires the VI channel to be enabled.
     * During shutdown there is a race where capture thread returns a frame
     * right as quiesce disables the channel. In that case, drop the pending
     * frame metadata without calling into VI, to avoid noisy warnings.
     *
     * The shutdown path prevents new frames from being latched when
     * channel_stopping is set (see hal_camera_stream_read()).
     */
    if ((hal_dev->vi_hal_chn_registered & (1u << channel_id)) != 0) {
        (void)ak_vi_release_frame(vi_chn, &chn->pending_vin_frame);
    }
    memset(&chn->pending_vin_frame, 0, sizeof(chn->pending_vin_frame));
    log_debug("vi release pending dev=%d hal_ch=%d vi_chn=%d",
              device_id, channel_id, (int)vi_chn);
}

int hal_camera_quiesce_channel(void *hal_camera, int device_id, int channel_id)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    hal_camera_device_t *hal_dev;
    VI_CHN vi_chn;
    int ret = 0;

    if (my_data == NULL) {
        return -1;
    }
    hal_dev = &my_data->hal_camera_device[device_id];
    vi_chn = hal_camera_vi_chn(device_id, channel_id);

    log_debug("quiesce channel dev=%d hal_ch=%d vi_chn=%d", device_id, channel_id, (int)vi_chn);

    hal_dev->channel[channel_id].channel_stopping = 1;
    /*
     * Slice mode: the capture thread blocks in ak_app_video_venc_get_stream(), which
     * ak_vi_disable_chn() does not unblock (the wait is on the encoder's stream list, not
     * on VI). Deactivating the dst chn first makes get_stream return an error so the
     * thread can notice channel_stopping and leave.
     */
    hal_camera_codec_slice_deactivate(hal_camera, device_id, channel_id);
    hal_camera_release_pending_frame(hal_camera, device_id, channel_id);

#if defined(HAL_CAMERA_QUIESCE_PRE_DELAY_MS) && (HAL_CAMERA_QUIESCE_PRE_DELAY_MS > 0)
    /*
     * camera_hw sets channel DISABLE before quiesce; give capture thread time to
     * exit between get_frame calls and reduce shutdown noise around disable_chn.
     */
    usleep((useconds_t)HAL_CAMERA_QUIESCE_PRE_DELAY_MS * 1000U);
#endif

    /*
     * Slice / KERNEL_ENCODE: ak_vi_disable_chn while the VI->VENC bind is still up runs
     * b2e_delete_bridge_channel, which tears down the bridge owning bitstream DMA the
     * capture thread may still hold. The deactivate above unblocks get_stream; camera_hw
     * joins the capture thread next, then disable_channel releases, unbinds and calls
     * disable_chn in that order. So skip disable_chn here and let that path do it.
     */
    if (hal_dev->channel[channel_id].slice_mode)
        return 0;

    /* Unblock ak_vi_get_frame if thread is still inside get_frame. */
    if ((hal_dev->vi_hal_chn_registered & (1u << channel_id)) != 0) {
        ret = ak_vi_disable_chn(vi_chn);
        if (ret != AK_SUCCESS) {
            log_warn("quiesce ak_vi_disable_chn vi_chn=%d ret=%d", vi_chn, ret);
        } else {
            hal_dev->vi_hal_chn_registered &= ~(1u << channel_id);
        }
    }

    if (channel_id == 0 && hal_dev->vi_main_only_extra_vi_ch1) {
        VI_CHN vi_sub = hal_camera_vi_chn(device_id, 1);
        if ((hal_dev->vi_hal_chn_registered & (1u << 1)) != 0) {
            ret = ak_vi_disable_chn(vi_sub);
            if (ret != AK_SUCCESS) {
                log_warn("quiesce ak_vi_disable_chn ch1 vi_chn=%d ret=%d", vi_sub, ret);
            } else {
                hal_dev->vi_hal_chn_registered &= ~(1u << 1);
            }
        }
    }

    return 0;
}

int hal_camera_disable_channel(void *hal_camera, int device_id, int channel_id)
{
    int ret = 0;
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    hal_camera_device_t *hal_dev = &my_data->hal_camera_device[device_id];
    hal_camera_channel_t *chn = &hal_dev->channel[channel_id];
    VI_CHN vi_chn = hal_camera_vi_chn(device_id, channel_id);

    log_debug("hal_camera_disable_channel dev=%d hal_ch=%d vi_chn=%d",
              device_id, channel_id, (int)vi_chn);

    hal_camera_release_pending_frame(hal_camera, device_id, channel_id);
    hal_camera_codec_disable_channel(hal_camera, device_id, channel_id);

    if ((hal_dev->vi_hal_chn_registered & (1u << channel_id)) != 0) {
        ret = ak_vi_disable_chn(vi_chn);
        if (ret != AK_SUCCESS) {
            log_error("ak_vi_disable_chn fail vi_chn=%d ret=%d", vi_chn, ret);
            return -1;
        }
        if (channel_id >= 0 && channel_id < 3) {
            hal_dev->vi_hal_chn_registered &= ~(1u << channel_id);
        }
    }

    if (channel_id == 0 && hal_dev->vi_main_only_extra_vi_ch1) {
        (void)ak_vi_disable_chn(hal_camera_vi_chn(device_id, 1));
        hal_dev->vi_hal_chn_registered &= ~(1u << 1);
    }

    chn->channel_stopping = 0;
    return 0;
}

int hal_camera_vpss_chn_is_configured(void *hal_camera, int device_id, int channel_id)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    hal_camera_device_t *hal_dev;

    if (!my_data || device_id < 0 || channel_id < 0 || channel_id >= CAMERA_MAX_CHANNEL)
        return 0;
    hal_dev = &my_data->hal_camera_device[device_id];
    return (hal_dev->vi_hal_chn_registered & (1u << channel_id)) ? 1 : 0;
}

/*
 * AV100_V2 is VI-based: ak_vi_set_chn_attr_ex re-registers a channel's attrs even
 * while the device is enabled (see hal_camera_enable_channel re-create path). So a
 * channel disabled on camera-app exit can be re-created on re-entry while the device
 * is kept open. Report the capability so the generic layer permits live reconfigure.
 */
int hal_camera_supports_live_chn_reconfigure(void *hal_camera, int device_id)
{
    (void)hal_camera;
    (void)device_id;
    return 1;
}

int hal_camera_enable_channel(void *hal_camera, int device_id, int channel_id)
{
    int ret = 0;
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    hal_camera_device_t *hal_dev = &my_data->hal_camera_device[device_id];
    hal_camera_channel_t *chn = &hal_dev->channel[channel_id];
    VI_CHN vi_chn = hal_camera_vi_chn(device_id, channel_id);
    int rebind_after_enable = 0;

    log_debug("hal_camera_enable_channel dev=%d hal_ch=%d vi_chn=%d",
              device_id, channel_id, vi_chn);

    /*
     * If the channel is not currently registered, re-register its attrs first.
     * hal_camera_disable_channel() (on camera app exit) calls ak_vi_disable_chn() and
     * clears the vi_hal_chn_registered bit — the vendor VI driver drops the channel's
     * attrs/buffers. On the next camera session the device stays open (ref-counted), so
     * configure_streams/create_channel is NOT re-run, and a bare ak_vi_enable_chn() then
     * fails QBUF ("VIDIOC_QBUF Invalid argument" / "Set CHN QBUF failed") because the
     * channel was never re-registered. Re-create the channel attrs here to recover.
     */
    if (channel_id >= 0 && channel_id < 3 &&
        (hal_dev->vi_hal_chn_registered & (1u << channel_id)) == 0) {
        log_info("hal_camera_enable_channel: dev=%d ch=%d not registered, re-create attrs before enable",
                 device_id, channel_id);
        /*
         * ak_vi_set_chn_attr_ex() must not run while an encoder is still bound to this VI
         * channel: the bind holds the channel's slice/buffer configuration, and the vendor
         * reference (aas_venc_set_res_main) deactivates and unbinds before re-setting attrs.
         * Doing it in the other order leaves the kernel encoder pointed at freed VI buffers.
         * No-op when the channel was never in slice mode.
         */
        hal_camera_codec_slice_unbind(hal_camera, device_id, channel_id);
        ret = hal_camera_create_channel(hal_camera, device_id, channel_id);
        if (ret != 0) {
            log_error("hal_camera_enable_channel: re-create channel failed dev=%d ch=%d ret=%d",
                      device_id, channel_id, ret);
            return -1;
        }
        rebind_after_enable = 1;
    }

    ret = ak_vi_enable_chn(vi_chn);
    if (ret != AK_SUCCESS) {
        log_error("ak_vi_enable_chn fail vi_chn=%d ret=%d", vi_chn, ret);
        return -1;
    }

    /* VI is live again: bind + IDR + activate, mirroring aas_venc_set_res_main(). */
    if (rebind_after_enable) {
        (void)hal_camera_codec_slice_rebind(hal_camera, device_id, channel_id);
    }

    if (chn->pstFrame == NULL) {
        chn->pstFrame = (VPSS_FRAME_INFO_S *)calloc(1, sizeof(VPSS_FRAME_INFO_S));
        if (chn->pstFrame == NULL) {
            log_error("malloc pstFrame failed");
            (void)ak_vi_disable_chn(vi_chn);
            return -1;
        }
    }

    chn->channel_buffer_release_flag = 0;
    memset(&chn->pending_vin_frame, 0, sizeof(chn->pending_vin_frame));
    chn->num_ai_recognize = 0;

    return 0;
}

size_t hal_camera_stream_read(void *hal_camera, int device_id, int channel_id, void **buffer, uint64_t * timestamp,size_t bytes)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    int ret;
    hal_camera_channel_t *chn = &my_data->hal_camera_device[device_id].channel[channel_id];
    VPSS_FRAME_INFO_S *pstFrame = chn->pstFrame;
    VI_CHN vi_chn = hal_camera_vi_chn(device_id, channel_id);
    size_t frame_len;

    (void)bytes;

    /*
     * Slice / kernel-encode channel: VI hands its slices straight to the bound encoder and
     * no full YUV frame is mapped for userspace, so ak_vi_get_frame() here would either
     * fail or steal buffers from the kernel encoder. Pull the bitstream instead; the
     * matching release is ak_venc_release_stream() (never ak_vi_release_frame()).
     *
     * The returned length/timestamp describe the encoded frame. hal_camera_codec_read()
     * for the bound stream re-publishes the very same buffer without re-encoding, which
     * keeps camera_hw.c's stream_read -> codec_read sequencing (and its use of the
     * channel timestamp) correct without special-casing the driver loop.
     */
    if (chn->slice_mode) {
        if (chn->channel_stopping) {
            return 0;
        }
        return hal_camera_codec_slice_read(hal_camera, device_id, channel_id,
                                           buffer, timestamp);
    }

    if (pstFrame == NULL) {
        log_error("stream_read: pstFrame null dev=%d ch=%d", device_id, channel_id);
        return 0;
    }

    if (chn->channel_buffer_release_flag == 1) {
        chn->channel_buffer_release_flag = 0;
        (void)ak_vi_release_frame(vi_chn, &chn->pending_vin_frame);
        memset(&chn->pending_vin_frame, 0, sizeof(chn->pending_vin_frame));
    }

    memset(pstFrame, 0, sizeof(*pstFrame));
    memset(&chn->pending_vin_frame, 0, sizeof(chn->pending_vin_frame));
    ret = ak_vi_get_frame(vi_chn, &chn->pending_vin_frame);
    if (ret != AK_SUCCESS) {
        if (!chn->channel_stopping) {
            log_debug("get_frame fail dev=%d hal_ch=%d vi_chn=%d ret=%d",
                      device_id, channel_id, (int)vi_chn, ret);
        }
        return 0;
    }

    /* If we're stopping, immediately release and don't latch a pending frame. */
    if (chn->channel_stopping) {
        (void)ak_vi_release_frame(vi_chn, &chn->pending_vin_frame);
        memset(&chn->pending_vin_frame, 0, sizeof(chn->pending_vin_frame));
        return 0;
    }

    hal_camera_export_frame(pstFrame, &chn->pending_vin_frame);
    chn->channel_buffer_release_flag = 1;
    *buffer = pstFrame->stFrame.pu8Data;
    *timestamp = pstFrame->stFrame.u64Ts;
    frame_len = pstFrame->stFrame.u32Len;
    return frame_len;
}

void hal_camera_set_torch_mode(const char* camera_id, bool on)
{
    /* Platform has no integrated torch driver in current HAL. */
    log_warn("torch mode not supported on AnyCloud39AV100_V2: camera_id=%s on=%d",
             camera_id ? camera_id : "null", on ? 1 : 0);
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
    int ret;
    RECTANGLE_S res = {0};
    const hal_camera_config_t *cfg = hal_camera_get_config();
    int cam_idx = (int)device_id;

    info->orientation = 0;

    if (cam_idx < 0 || cam_idx >= cfg->camera_number || cam_idx >= CAMERA_MAX_NUM) {
        cam_idx = 0;
    }

    ret = ak_vi_open((VI_DEV)device_id);
    if (ret != AK_SUCCESS) {
        log_error("hal_camera_get_info ak_vi_open fail dev=%u ret=%d", device_id, ret);
        return -1;
    }

    ret = ak_vi_load_sensor_cfg((VI_DEV)device_id, cfg->isp_config_path[cam_idx]);
    if (ret != AK_SUCCESS) {
        log_error("hal_camera_get_info load_sensor_cfg fail dev=%u ret=%d", device_id, ret);
        ak_vi_close((VI_DEV)device_id);
        return -1;
    }

    ret = ak_vi_get_sensor_resolution((VI_DEV)device_id, &res);
    if (ret != AK_SUCCESS) {
        log_error("hal_camera_get_info get_sensor_resolution fail dev=%u ret=%d", device_id, ret);
        ak_vi_close((VI_DEV)device_id);
        return -1;
    }

    info->max_width = (uint32_t)res.width;
    info->max_height = (uint32_t)res.height;

    ret = ak_vi_close((VI_DEV)device_id);
    if (ret != AK_SUCCESS) {
        log_warn("hal_camera_get_info ak_vi_close dev=%u ret=%d", device_id, ret);
    }

    return 0;
}

