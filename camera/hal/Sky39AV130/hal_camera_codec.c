// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/camera.
 *
 * HAL video encoder (VENC) for AnyCloud39AV100_V2.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ak_common.h"
#include "ak_common_video.h"
#include "ak_log.h"
/*
 * ak_venc.h defines `struct rect`, which collides with rect/rect_t in other
 * headers (system_pixel_format.h, etc.). Rename the tag only while including
 * ak_venc.h so both can coexist; HAL code must not use the `rect` tag directly.
 */
#define rect ak_venc_rect
#include "ak_venc.h"
#undef rect
/* After ak_venc.h: ak_video.h re-includes it, and its guard makes that a no-op. */
#include "ak_video.h"
#include "camera.h"
#include "log.h"

#include "hal_camera.h"
#include "hal_camera_chip_config.h"

#include "../../camera_hw.h"
#include "../../hal_camera_api.h"

#define LOG_TAG "HAL CAMERA CODEC"

/* MJPEG uses the same ak_venc_open_ex path as the sample. Retry CREATE_CHN_FAILED once. */

/*
 * Serializes hal_camera_codec_read() (called lock-free from the per-channel capture
 * thread) against hal_camera_codec_disable() (called under cdev->lock from the control
 * path). Without this, disable() can free pstStream / close the venc handle while a
 * concurrent read() is still using them, causing a use-after-free that corrupts the heap
 * and crashes later (e.g. inside libc malloc/free).
 *
 * Implemented as a plain spinlock with the same GCC atomic builtin already used below for
 * codec_id (__sync_lock_test_and_set/__sync_lock_release), kept file-local and independent
 * of any shared struct/header, so it cannot change the layout of hal_camera_codec_t (or
 * anything that embeds it) as seen by other translation units.
 */
static volatile int s_codec_io_busy[CAMERA_MAX_NUM][CAMERA_MAX_CHANNEL][CAMERA_MAX_STREAM_PER_CHANNEL];

static void codec_io_lock(int device_id, int channel_id, int stream_id)
{
    volatile int *busy = &s_codec_io_busy[device_id][channel_id][stream_id];
    while (__sync_lock_test_and_set(busy, 1)) {
        /* Contention only happens around stream teardown/reconfig, not per frame. */
    }
}

static void codec_io_unlock(int device_id, int channel_id, int stream_id)
{
    __sync_lock_release(&s_codec_io_busy[device_id][channel_id][stream_id]);
}

/* Set while the capture thread still uses pstStream->data after codec_read unlocks. */
static volatile int s_codec_buf_held[CAMERA_MAX_NUM][CAMERA_MAX_CHANNEL][CAMERA_MAX_STREAM_PER_CHANNEL];

static void codec_pin_buf(int device_id, int channel_id, int stream_id)
{
    __sync_lock_test_and_set(&s_codec_buf_held[device_id][channel_id][stream_id], 1);
}

/* Caller holds codec_io_lock. Drops it while the capture callback still owns the buffer. */
static void codec_wait_reader_idle(int device_id, int channel_id, int stream_id)
{
    int spins = 0;

    while (s_codec_buf_held[device_id][channel_id][stream_id]) {
        codec_io_unlock(device_id, channel_id, stream_id);
        if (++spins > 2000) {
            log_warn("venc buffer still in capture callback dev=%d ch=%d sid=%d",
                     device_id, channel_id, stream_id);
            codec_io_lock(device_id, channel_id, stream_id);
            return;
        }
        usleep(1000);
        codec_io_lock(device_id, channel_id, stream_id);
    }
}

void hal_camera_codec_read_done(void *hal_camera, int device_id, int channel_id, int stream_id)
{
    (void)hal_camera;
    if (device_id < 0 || channel_id < 0 || stream_id < 0 ||
        device_id >= CAMERA_MAX_NUM || channel_id >= CAMERA_MAX_CHANNEL ||
        stream_id >= CAMERA_MAX_STREAM_PER_CHANNEL)
        return;
    __sync_lock_release(&s_codec_buf_held[device_id][channel_id][stream_id]);
}

/*
 * ---------------------------------------------------------------------------
 * VI SLICE_MODE / KERNEL_ENCODE_MODE binding
 * ---------------------------------------------------------------------------
 *
 * When a channel's VI attrs were set with mode=SLICE_MODE (num_slices > 1 in the channel
 * config, see hal_camera_create_channel), the kernel pushes VI slices straight into an
 * encoder bound to that VI channel and userspace never sees a full YUV frame. The encoder
 * is therefore driven by ak_app_video_* rather than by ak_venc_encode_frame():
 *
 *   setup    : ak_app_video_bind_chn(VI:vi_chn -> VENC:handle, KERNEL_ENCODE_MODE)
 *              ak_venc_request_idr(handle)          -- first frame must be an IDR
 *              ak_app_video_set_dst_chn_active(dst, 1)
 *   per frame: ak_app_video_venc_get_stream(dst, &stream) / ak_venc_release_stream()
 *   teardown : ak_app_video_set_dst_chn_active(dst, 0)
 *              ak_venc_release_stream()              -- return held bitstream DMA
 *              ak_app_video_unbind_chn(src, dst)     -- b2e_delete_bridge_channel
 *
 * Release-before-unbind is required: unbind tears down the bridge that owns the bitstream DMA
 * the last get_stream still holds, so releasing afterwards hands the encoder a buffer whose
 * owner is gone. Deactivate-before-unbind and unbind-before-set_chn_attr_ex match the vendor
 * reference (aas_vi_venc_binder_th / aas_venc_set_res_main).
 *
 * Only one encoder may own a VI channel, so at most one stream per channel gets the bind;
 * any other encoded stream on a slice channel cannot be served (there is no frame to feed
 * it) and is rejected at enable time rather than failing silently every frame.
 */
static void slice_chns(const hal_camera_channel_t *chn, APP_CHN_S *src, APP_CHN_S *dst)
{
    src->mid = MODULE_ID_VI;
    src->oid = chn->slice_vi_chn;
    dst->mid = MODULE_ID_VENC;
    dst->oid = chn->slice_venc_handle;
}

/* Bind + IDR + activate. chn->slice_venc_handle / slice_stream_id must already be set. */
static int slice_bind_locked(hal_camera_channel_t *chn, int device_id, int channel_id)
{
    APP_CHN_S src;
    APP_CHN_S dst;
    APP_BIND_PARAM param;
    int ret;

    slice_chns(chn, &src, &dst);

    memset(&param, 0, sizeof(param));
    /*
     * frame_rate 0 = no extra frame-control on top of what VI already applies (the VI chn
     * attr carries the real rate). frame_depth is the bind's stream-list length; the driver
     * ignores it in slice mode (vendor comment: "slice mode the driver unused"), so keep it
     * generous rather than tying it to the VI buffer depth.
     */
    param.frame_rate = 0;
    param.frame_depth = 100;
    param.mode = KERNEL_ENCODE_MODE;

    ret = ak_app_video_bind_chn(&src, &dst, &param);
    if (ret != AK_SUCCESS) {
        log_error("ak_app_video_bind_chn VI:%d->VENC:%d dev=%d ch=%d ret=0x%x",
                  src.oid, dst.oid, device_id, channel_id, ret);
        return -1;
    }
    chn->slice_bound = 1;

    ret = ak_venc_request_idr(chn->slice_venc_handle);
    if (ret != AK_SUCCESS) {
        log_warn("slice: ak_venc_request_idr handle=%d ret=%d (first frame may be a P frame)",
                 chn->slice_venc_handle, ret);
    }

    ret = ak_app_video_set_dst_chn_active(&dst, 1);
    if (ret != AK_SUCCESS) {
        log_error("slice: set_dst_chn_active(1) VENC:%d ret=0x%x", dst.oid, ret);
        (void)ak_app_video_unbind_chn(&src, &dst);
        chn->slice_bound = 0;
        return -1;
    }
    chn->slice_active = 1;
    chn->slice_stream_held = false;

    log_info("slice bind ok dev=%d ch=%d VI:%d -> VENC:%d (KERNEL_ENCODE_MODE, sid=%d)",
             device_id, channel_id, src.oid, dst.oid, chn->slice_stream_id);
    return 0;
}

void hal_camera_codec_slice_deactivate(void *hal_camera, int device_id, int channel_id)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    hal_camera_channel_t *chn;
    APP_CHN_S src;
    APP_CHN_S dst;
    int ret;

    if (my_data == NULL || device_id < 0 || device_id >= CAMERA_MAX_NUM ||
        channel_id < 0 || channel_id >= CAMERA_MAX_CHANNEL) {
        return;
    }
    chn = &my_data->hal_camera_device[device_id].channel[channel_id];
    if (!chn->slice_active) {
        return;
    }

    slice_chns(chn, &src, &dst);
    ret = ak_app_video_set_dst_chn_active(&dst, 0);
    if (ret != AK_SUCCESS) {
        log_warn("slice: set_dst_chn_active(0) VENC:%d ret=0x%x", dst.oid, ret);
    }
    chn->slice_active = 0;
}

void hal_camera_codec_slice_unbind(void *hal_camera, int device_id, int channel_id)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    hal_camera_channel_t *chn;
    APP_CHN_S src;
    APP_CHN_S dst;
    int ret;

    if (my_data == NULL || device_id < 0 || device_id >= CAMERA_MAX_NUM ||
        channel_id < 0 || channel_id >= CAMERA_MAX_CHANNEL) {
        return;
    }
    chn = &my_data->hal_camera_device[device_id].channel[channel_id];
    if (!chn->slice_bound) {
        return;
    }

    hal_camera_codec_slice_deactivate(hal_camera, device_id, channel_id);

    slice_chns(chn, &src, &dst);
    ret = ak_app_video_unbind_chn(&src, &dst);
    if (ret != AK_SUCCESS) {
        log_warn("ak_app_video_unbind_chn VI:%d->VENC:%d ret=0x%x", src.oid, dst.oid, ret);
    }
    chn->slice_bound = 0;
    log_info("slice unbind dev=%d ch=%d VI:%d -> VENC:%d", device_id, channel_id,
             src.oid, dst.oid);
}

int hal_camera_codec_slice_rebind(void *hal_camera, int device_id, int channel_id)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    hal_camera_channel_t *chn;

    if (my_data == NULL || device_id < 0 || device_id >= CAMERA_MAX_NUM ||
        channel_id < 0 || channel_id >= CAMERA_MAX_CHANNEL) {
        return -1;
    }
    chn = &my_data->hal_camera_device[device_id].channel[channel_id];
    /* Nothing to restore unless a live encoder is still waiting for this VI channel. */
    if (!chn->slice_mode || chn->slice_bound || chn->slice_venc_handle < 0) {
        return 0;
    }
    /* VI attrs were re-set by create_channel; pick up the (possibly new) VI chn id. */
    return slice_bind_locked(chn, device_id, channel_id);
}

size_t hal_camera_codec_slice_read(void *hal_camera, int device_id, int channel_id,
                                   void **buffer, uint64_t *timestamp)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    hal_camera_channel_t *chn = &my_data->hal_camera_device[device_id].channel[channel_id];
    hal_camera_codec_t *codec_info;
    struct video_stream *pstStream;
    APP_CHN_S src;
    APP_CHN_S dst;
    int stream_id;
    int handle;
    int ret;
    size_t len;

    stream_id = chn->slice_stream_id;
    if (!chn->slice_active || stream_id < 0 ||
        stream_id >= CAMERA_MAX_STREAM_PER_CHANNEL) {
        /* Not bound yet (codec_enable has not run) or already torn down. */
        return 0;
    }

    /* Same lock as codec_read/codec_disable: disable() can free pstStream concurrently. */
    codec_io_lock(device_id, channel_id, stream_id);

    codec_info = &chn->codec[stream_id];
    pstStream = codec_info->pstStream;
    handle = chn->slice_venc_handle;
    if (!chn->slice_active || pstStream == NULL || handle < 0) {
        codec_io_unlock(device_id, channel_id, stream_id);
        return 0;
    }

    /* Release the frame published on the previous iteration before asking for a new one. */
    if (chn->slice_stream_held) {
        (void)ak_venc_release_stream(handle, pstStream);
        chn->slice_stream_held = false;
        codec_info->stream_buffer_release_flag = false;
    }

    slice_chns(chn, &src, &dst);
    memset(pstStream, 0, sizeof(*pstStream));
    ret = ak_app_video_venc_get_stream(&dst, pstStream);
    if (ret != AK_SUCCESS) {
        codec_io_unlock(device_id, channel_id, stream_id);
        if (!chn->channel_stopping) {
            log_debug("slice get_stream dev=%d ch=%d VENC:%d ret=0x%x",
                      device_id, channel_id, dst.oid, ret);
        }
        return 0;
    }
    if (pstStream->data == NULL || pstStream->len == 0) {
        (void)ak_venc_release_stream(handle, pstStream);
        codec_io_unlock(device_id, channel_id, stream_id);
        return 0;
    }

    chn->slice_stream_held = true;
    /*
     * Mark it releasable through the normal codec bookkeeping too, so a teardown that
     * races this read (hal_camera_codec_disable) still returns the buffer exactly once —
     * it holds the same lock and clears the flag after releasing.
     */
    codec_info->stream_buffer_release_flag = true;

    *buffer = pstStream->data;
    *timestamp = pstStream->ts;
    len = pstStream->len;
    codec_info->frame_type = (int)pstStream->frame_type;
    codec_io_unlock(device_id, channel_id, stream_id);
    return len;
}

int hal_camera_codec_init( void *hal_camera )
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;

    log_info("ak_venc_get_version: %s", ak_venc_get_version());

    for (int i = 0; i < CAMERA_CODEC_MAX_NUM; i++) {
        my_data->codec_id_status[i] = 0;
    }
    for (int d = 0; d < CAMERA_MAX_NUM; d++) {
        for (int c = 0; c < CAMERA_MAX_CHANNEL; c++) {
            for (int s = 0; s < CAMERA_MAX_STREAM_PER_CHANNEL; s++) {
                my_data->hal_camera_device[d].channel[c].codec[s].codec_slot = -1;
                s_codec_io_busy[d][c][s] = 0;
            }
            my_data->hal_camera_device[d].channel[c].slice_stream_id = -1;
            my_data->hal_camera_device[d].channel[c].slice_venc_handle = -1;
        }
    }
    return 0;
}

int camera_codec_register_id( void *hal_camera)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;

    for (int i = 0; i < CAMERA_CODEC_MAX_NUM; i++) {
        if (my_data->codec_id_status[i] == 0) {
            my_data->codec_id_status[i] = 1;
            return i;
        }
    }
    return -1;
}

int camera_codec_unregister_id(void *hal_camera, int codec_id)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;

    my_data->codec_id_status[codec_id] = 0;

    return 0;
}

static const char *venc_open_err_name(int ret)
{
    switch (ret) {
    case ERROR_VENC_OPEN_TOO_MANY:
        return "OPEN_TOO_MANY";
    case ERROR_VENC_INVALID_PARAM:
        return "INVALID_PARAM";
    case ERROR_VENC_CREATE_CHN_FAILED:
        return "CREATE_CHN_FAILED";
    case ERROR_VENC_DMA_NO_ENOUGH:
        return "DMA_NO_ENOUGH";
    case ERROR_VENC_OPEN_LIB_ERROR:
        return "OPEN_LIB_ERROR";
    default:
        return "other";
    }
}

/* Close one ak_venc_open_ex handle and release its HAL slot. */
static int codec_close_one_venc(struct hal_camera_data *my_data, stream_info_t *stream_info)
{
    int device_id;
    int channel_id;
    int stream_id;
    int codec_id;
    hal_camera_codec_t *codec_info;
    hal_camera_channel_t *hchn;
    int slot;
    int close_ret = 0;

    if (!my_data || !stream_info)
        return 0;

    device_id = stream_info->device_id;
    channel_id = stream_info->channel_id;
    stream_id = stream_info->stream_id;
    codec_id = __sync_lock_test_and_set(&stream_info->codec_id, -1);
    if (codec_id < 0)
        return 0;

    codec_info = &my_data->hal_camera_device[device_id].channel[channel_id].codec[stream_id];
    hchn = &my_data->hal_camera_device[device_id].channel[channel_id];

    if (hchn->slice_mode && hchn->slice_stream_id == stream_id)
        hal_camera_codec_slice_deactivate(my_data, device_id, channel_id);

    codec_io_lock(device_id, channel_id, stream_id);
    codec_wait_reader_idle(device_id, channel_id, stream_id);

    slot = codec_info->codec_slot;

    if (codec_info->pstStream != NULL && codec_info->stream_buffer_release_flag)
        (void)ak_venc_release_stream(codec_id, codec_info->pstStream);
    codec_info->stream_buffer_release_flag = false;
    if (hchn->slice_stream_id == stream_id)
        hchn->slice_stream_held = false;

    if (hchn->slice_mode && hchn->slice_stream_id == stream_id)
        hal_camera_codec_slice_unbind(my_data, device_id, channel_id);

    if (hchn->slice_stream_id == stream_id) {
        hchn->slice_stream_id = -1;
        hchn->slice_venc_handle = -1;
    }

    if (slot >= 0) {
        camera_codec_unregister_id(my_data, slot);
        codec_info->codec_slot = -1;
    }

    if (codec_info->pstStream != NULL) {
        free(codec_info->pstStream);
        codec_info->pstStream = NULL;
    }

    close_ret = ak_venc_close(codec_id);
    log_info("close camera venc handle:%d", codec_id);
    if (close_ret != 0)
        log_warn("ak_venc_close failed handle=%d ret=%d", codec_id, close_ret);

    codec_io_unlock(device_id, channel_id, stream_id);
    return close_ret;
}

/* camera_rc_mjpeg_t.quality -> jpeg_qlevel. */
static enum jpeg_quant_table_level jpeg_qlevel_from_quality(int quality)
{
    if (quality >= 85)
        return JPEG_QLEVEL_HIGHEST;
    if (quality >= 65)
        return JPEG_QLEVEL_HIGH;
    if (quality >= 40 || quality <= 0)
        return JPEG_QLEVEL_DEFAULT;
    return JPEG_QLEVEL_LOW;
}

/* rc_config.hevc / .avc: bitrate and qp from that member only. */
static int venc_apply_video_rc(struct venc_param *ve, struct venc_rc_param *rc,
                               int rc_mode, unsigned short cbr_kbps,
                               unsigned short avbr_kbps,
                               unsigned short vbr_kbps, unsigned short vbr_min_qp,
                               unsigned short vbr_max_qp)
{
    if (rc_mode == CAMERA_CODEC_RC_MODE_CBR) {
        ve->br_mode = BR_MODE_CBR;
        ve->target_kbps = cbr_kbps;
        ve->max_kbps = cbr_kbps;
    } else if (rc_mode == CAMERA_CODEC_RC_MODE_AVBR) {
        ve->br_mode = BR_MODE_AVBR;
        ve->target_kbps = avbr_kbps;
        ve->max_kbps = avbr_kbps;
    } else {
        ve->br_mode = BR_MODE_VBR;
        ve->target_kbps = vbr_kbps;
        ve->max_kbps = vbr_kbps;
        ve->minqp = vbr_min_qp;
        ve->maxqp = vbr_max_qp;
        rc->minqp = ve->minqp;
        rc->maxqp = ve->maxqp;
    }
    return ve->max_kbps == 0 ? -1 : 0;
}

/**
 * @brief Enable camera codec for a stream.
 *
 * Codec ID is internally managed and saved in stream info.
 * When disabling, the codec_id from stream info is used directly.
 *
 * @param hal_camera Pointer to HAL camera data.
 * @param stream Stream to enable codec for.
 * @return 0 on success, negative on failure.
 */
int hal_camera_codec_enable(void *hal_camera, camera_stream_t *stream)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    int ret = 0;
    int success_count = 0;
    log_debug("hal_camera_codec_enable");

    if (stream->codec_config.codec_mode == CAMERA_CODEC_MODE_YUV) {
        log_info("CAMERA_CODEC_MODE_YUV don't need enable codec");
        return -1;
    }

    for (int i = 0; i < stream->num_channel; i++) {
        stream_info_t *stream_info = stream->priv[i];
        int device_id = stream_info->device_id;
        int channel_id = stream_info->channel_id;
        int stream_id = stream_info->stream_id;
        int is_mjpeg = (stream->codec_config.codec_mode == CAMERA_CODEC_MODE_MJPEG);
        int slot;
        hal_camera_codec_t *ci =
            &my_data->hal_camera_device[device_id].channel[channel_id].codec[stream_id];
        hal_camera_channel_t *hchn =
            &my_data->hal_camera_device[device_id].channel[channel_id];
        struct venc_param ve = {0};
        struct venc_rc_param rc = {0};
        camera_codec_config_t codec = stream->codec_config;
        camera_rc_config_t rcc = stream->rc_config;
        camera_gop_config_t gop = stream->gop_config;
        int handle = -1;

        if (is_mjpeg && stream_info->codec_id >= 0)
            (void)codec_close_one_venc(my_data, stream_info);
        slot = camera_codec_register_id(my_data);
        if (slot < 0) {
            log_error("camera_codec_register_id failed");
            return -1;
        }

        ci->codec_slot = slot;
        ci->stream_buffer_release_flag = false;
        if (ci->pstStream == NULL) {
            ci->pstStream = (struct video_stream *)calloc(1, sizeof(struct video_stream));
            if (ci->pstStream == NULL) {
                camera_codec_unregister_id(my_data, slot);
                ci->codec_slot = -1;
                log_error("malloc video_stream failed");
                return -1;
            }
        }

        ve.width = (unsigned short)get_splice_stream_width(stream);
        ve.height = (unsigned short)get_splice_stream_height(stream);
        ve.fps = (unsigned short)stream->fps;
        ve.goplen = gop.gop_len ? gop.gop_len : (unsigned short)(2 * stream->fps);
        ve.jpeg_qlevel = JPEG_QLEVEL_DEFAULT;
        ve.chroma_mode = CHROMA_4_2_0;
        ve.max_picture_size = 0;
        /* ak_venc_sample.c: ve_param.enc_level = 30 (HEVC_AVC_LEVEL_3) */
        ve.enc_level = (enum EncLevel)codec.level;
        if ((int)ve.enc_level <= 0) {
            ve.enc_level = HEVC_AVC_LEVEL_3;
        }
        ve.smart_mode = SMART_DISABLE;
        ve.smart_goplen = 100;
        ve.smart_quality = 50;
        ve.smart_static_value = 0;
        ve.gdr_mode = GDR_OFF;
        ve.initqp = 50;
        ve.minqp = 20;
        ve.maxqp = 48;

        /* ak_venc_sample.c start_venc(): rc_param.enable_MMA = 1 (rest zeroed) */
        rc.CUBlockSize = CUBSize32x32;
        rc.minqp = ve.minqp;
        rc.maxqp = ve.maxqp;
        rc.IPDelta = 0;
        rc.maxPictureISize = 0;
        rc.maxPicturePSize = 0;
        rc.maxPictureBSize = 0;
        rc.enable_SRD = false;
        rc.SRD_threshold = 0;
        rc.enable_MMA = true;

        if (codec.codec_mode == CAMERA_CODEC_MODE_AVC) {
            ve.enc_out_type = H264_ENC_TYPE;
            switch (codec.profile) {
            case AVC_PROFILE_BASE: ve.profile = PROFILE_BASE; break;
            case AVC_PROFILE_HIGH: ve.profile = PROFILE_HIGH; break;
            case AVC_PROFILE_C_BASE: ve.profile = PROFILE_C_BASE; break;
            default: ve.profile = PROFILE_MAIN; break;
            }
            if (venc_apply_video_rc(&ve, &rc, rcc.avc.rc_mode,
                                    rcc.avc.cbr.target_bitrate,
                                    rcc.avc.avbr.max_bitrate,
                                    rcc.avc.vbr.max_bitrate,
                                    rcc.avc.vbr.min_qp, rcc.avc.vbr.max_qp) != 0) {
                log_error("AVC max_kbps=0 (rc_config.avc); slot=%d", slot);
                camera_codec_unregister_id(my_data, slot);
                ci->codec_slot = -1;
                free(ci->pstStream);
                ci->pstStream = NULL;
                return -1;
            }
        } else if (codec.codec_mode == CAMERA_CODEC_MODE_HEVC) {
            /* main + sub: each stream's own rc_config.hevc */
            ve.enc_out_type = HEVC_ENC_TYPE;
            switch (codec.profile) {
            case HEVC_PROFILE_MAIN_INTRA: ve.profile = PROFILE_HEVC_MAIN_INTRA; break;
            case HEVC_PROFILE_MAIN_STILL: ve.profile = PROFILE_HEVC_MAIN_STILL; break;
            default: ve.profile = PROFILE_HEVC_MAIN; break;
            }
            if (venc_apply_video_rc(&ve, &rc, rcc.hevc.rc_mode,
                                    rcc.hevc.cbr.target_bitrate,
                                    rcc.hevc.avbr.max_bitrate,
                                    rcc.hevc.vbr.max_bitrate,
                                    rcc.hevc.vbr.min_qp, rcc.hevc.vbr.max_qp) != 0) {
                log_error("HEVC max_kbps=0 (rc_config.hevc); slot=%d", slot);
                camera_codec_unregister_id(my_data, slot);
                ci->codec_slot = -1;
                free(ci->pstStream);
                ci->pstStream = NULL;
                return -1;
            }
        } else if (codec.codec_mode == CAMERA_CODEC_MODE_MJPEG) {
            /* picture: rc_config.mjpeg (quality + max_bitrate). venc rejects max_kbps 0. */
            ve.enc_out_type = MJPEG_ENC_TYPE;
            ve.profile = PROFILE_JPEG;
            ve.br_mode = BR_MODE_CBR;
            ve.jpeg_qlevel = jpeg_qlevel_from_quality(rcc.mjpeg.quality);
            ve.target_kbps = (unsigned short)rcc.mjpeg.max_bitrate;
            ve.max_kbps = ve.target_kbps;
            if (ve.max_kbps == 0) {
                log_error("MJPEG max_kbps=0 (rc_config.mjpeg.max_bitrate); slot=%d", slot);
                camera_codec_unregister_id(my_data, slot);
                ci->codec_slot = -1;
                free(ci->pstStream);
                ci->pstStream = NULL;
                return -1;
            }
            log_info("mjpeg rc.quality=%d qlevel=%d kbps=%u %ux%u fps=%u",
                     rcc.mjpeg.quality, (int)ve.jpeg_qlevel, ve.max_kbps,
                     ve.width, ve.height, ve.fps);
        } else {
            log_error("unsupported codec_mode %d", codec.codec_mode);
            camera_codec_unregister_id(my_data, slot);
            ci->codec_slot = -1;
            return -1;
        }

        ret = ak_venc_open_ex(&ve, &rc, &handle);
        if (is_mjpeg && ret == ERROR_VENC_CREATE_CHN_FAILED) {
            log_warn("ak_venc_open_ex CREATE_CHN_FAILED, retry once hal_slot=%d", slot);
            usleep(100 * 1000);
            handle = -1;
            ret = ak_venc_open_ex(&ve, &rc, &handle);
        }
        if (ret != AK_SUCCESS || handle < 0) {
            log_error("ak_venc_open_ex %s ret=%d handle=%d hal_slot=%d "
                      "%ux%u fps=%u type=%d profile=%d level=%d max_pic=%u kbps=%u",
                      venc_open_err_name(ret), ret, handle, slot,
                      ve.width, ve.height, ve.fps, (int)ve.enc_out_type,
                      (int)ve.profile, (int)ve.enc_level, ve.max_picture_size,
                      ve.max_kbps);
            camera_codec_unregister_id(my_data, slot);
            ci->codec_slot = -1;
            if (ci->pstStream) {
                free(ci->pstStream);
                ci->pstStream = NULL;
            }
            ret = -1;
            continue;
        }

#if HAL_VENC_STRICT_FRAME_MAX_KB > 0
        if (ak_venc_strictlylimit_frameSize(handle, HAL_VENC_STRICT_FRAME_MAX_KB) != AK_SUCCESS)
            log_warn("ak_venc_strictlylimit_frameSize handle=%d max_kb=%d failed",
                     handle, HAL_VENC_STRICT_FRAME_MAX_KB);
#endif

        stream_info->codec_id = handle;
        log_info("ak_venc_open_ex handle=%d slot=%d stream:[%d][%d][%d]",
                 handle, slot, device_id, channel_id, stream_id);

        /*
         * Slice channel: this encoder is fed by the kernel, not by the capture thread, so
         * it has to be bound to the VI channel before it will produce anything. Only the
         * first encoded stream on the channel can be bound (one encoder per VI channel);
         * a second one has no YUV source at all, so fail it here instead of letting the
         * capture thread log an encode error on every frame.
         */
        if (hchn->slice_mode) {
            if (hchn->slice_bound) {
                log_error("slice ch=%d already bound to VENC:%d; stream %d cannot be encoded "
                          "(a slice VI channel feeds exactly one encoder). Give this stream "
                          "its own channel_group_id, or set num_slices=1 on group %d.",
                          channel_id, hchn->slice_venc_handle, stream_id, channel_id);
                stream_info->codec_id = -1;
                camera_codec_unregister_id(my_data, slot);
                ci->codec_slot = -1;
                (void)ak_venc_close(handle);
                ret = -1;
                continue;
            }
            hchn->slice_stream_id = stream_id;
            hchn->slice_venc_handle = handle;
            if (slice_bind_locked(hchn, device_id, channel_id) != 0) {
                hchn->slice_stream_id = -1;
                hchn->slice_venc_handle = -1;
                stream_info->codec_id = -1;
                camera_codec_unregister_id(my_data, slot);
                ci->codec_slot = -1;
                (void)ak_venc_close(handle);
                ret = -1;
                continue;
            }
        }
        success_count++;
    }

    /*
     * Treat the call as success only when every splice channel opened.
     * Previously the loop would clobber `ret = 0` on the next successful
     * channel even if an earlier channel had failed — silently dropping
     * the error to the caller.
     */
    if (stream->num_channel > 0 && success_count == stream->num_channel) {
        return 0;
    }
    return (success_count > 0) ? -1 : (ret ? ret : -1);
}


void hal_camera_codec_disable_channel(void *hal_camera, int device_id, int channel_id)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    struct camera_device *cdev;
    channel_info_t *channel_info;

    if (my_data == NULL || my_data->cdev == NULL) {
        return;
    }
    cdev = my_data->cdev;
    if (device_id < 0 || device_id >= cdev->num_device ||
        channel_id < 0 || channel_id >= CAMERA_MAX_CHANNEL) {
        return;
    }
    channel_info = &cdev->device_info[device_id].channel_info[channel_id];
    for (int stream_id = 0; stream_id < channel_info->num_stream; stream_id++) {
        stream_info_t *stream_info = &channel_info->stream_info[stream_id];

        if (stream_info->codec_id >= 0 && stream_info->stream != NULL) {
            (void)hal_camera_codec_disable(hal_camera, stream_info->stream);
        }
    }
}

int hal_camera_codec_disable(void *hal_camera, camera_stream_t *stream)
{
    int ret = 0;
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    if (stream == NULL) {
        return 0;
    }

    for (int i = 0; i < stream->num_channel; i++) {
        stream_info_t *stream_info = stream->priv[i];
        int cr;

        if (stream_info == NULL)
            continue;
        cr = codec_close_one_venc(my_data, stream_info);
        if (cr != 0)
            ret = cr;
    }
    return ret;
}

/**
 * Reads data from the camera codec
 * @param hal_camera Pointer to the camera data
  * @param device_id The device ID
 * @param channel_id The channel ID
 * @param stream_id The stream ID
 * @param buffer The buffer to read data into
 * @param bytes The number of bytes to read
 * @return The number of bytes read
 */
int hal_camera_codec_request_idr(void *hal_camera, int device_id, int channel_id,
                                 int stream_id)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    struct camera_device *cdev;
    int codec_id;
    int ret;

    if (my_data == NULL || my_data->cdev == NULL)
        return -1;
    cdev = my_data->cdev;
    if (device_id < 0 || device_id >= cdev->num_device ||
        channel_id < 0 || channel_id >= CAMERA_MAX_CHANNEL ||
        stream_id < 0 || stream_id >= CAMERA_MAX_STREAM_PER_CHANNEL)
        return -1;

    codec_id = cdev->device_info[device_id].channel_info[channel_id]
                   .stream_info[stream_id].codec_id;
    if (codec_id < 0) {
        log_warn("request_idr: no codec handle dev=%d ch=%d sid=%d",
                 device_id, channel_id, stream_id);
        return -1;
    }

    ret = ak_venc_request_idr(codec_id);
    if (ret != AK_SUCCESS) {
        log_warn("ak_venc_request_idr handle=%d ret=%d", codec_id, ret);
        return -1;
    }
    log_info("ak_venc_request_idr handle=%d ok (dev=%d ch=%d sid=%d)",
             codec_id, device_id, channel_id, stream_id);
    return 0;
}

size_t hal_camera_codec_read(void *hal_camera, int device_id, int channel_id,int stream_id, void **buffer, uint64_t * timestamp,size_t bytes)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    struct camera_device *cdev = my_data->cdev;
    hal_camera_channel_t *hchn = &my_data->hal_camera_device[device_id].channel[channel_id];
    int ret;

    VPSS_FRAME_INFO_S *pstFrame = my_data->hal_camera_device[device_id].channel[channel_id].pstFrame;
    hal_camera_codec_t *codec_info =
        &my_data->hal_camera_device[device_id].channel[channel_id].codec[stream_id];

    /* Serialize against hal_camera_codec_disable(), which can free pstStream and close
     * the venc handle concurrently from the control thread (this capture thread does not
     * hold cdev->lock). Re-read codec_id/pstStream under the lock rather than trusting
     * values fetched beforehand, since disable() may have run in between. */
    codec_io_lock(device_id, channel_id, stream_id);

    int codec_id = cdev->device_info[device_id].channel_info[channel_id].stream_info[stream_id].codec_id;
    struct video_stream *pstStream = codec_info->pstStream;

    if (codec_id == -1 || pstStream == NULL || pstFrame == NULL) {
        codec_io_unlock(device_id, channel_id, stream_id);
        log_error("codec_read unavailable dev=%d ch=%d sid=%d codec_id=%d stream=%p frame=%p",
                  device_id, channel_id, stream_id, codec_id, (void *)pstStream, (void *)pstFrame);
        return (size_t)-1;
    }

    /*
     * Slice channel: hal_camera_stream_read() already fetched this frame with
     * ak_app_video_venc_get_stream() (there is no YUV frame to encode here, and the
     * encoder is driven by the kernel). Re-publish the held buffer; the release happens on
     * the next slice read, or in codec_disable if we are torn down first.
     */
    if (hchn->slice_mode && hchn->slice_stream_id == stream_id) {
        size_t held_len;

        if (!hchn->slice_stream_held) {
            codec_io_unlock(device_id, channel_id, stream_id);
            return (size_t)-1;
        }
        *buffer = pstStream->data;
        *timestamp = pstStream->ts;
        held_len = pstStream->len;
        if (held_len > 0)
            codec_pin_buf(device_id, channel_id, stream_id);
        codec_io_unlock(device_id, channel_id, stream_id);
        return held_len;
    }

    if (codec_info->stream_buffer_release_flag) {
        (void)ak_venc_release_stream(codec_id, pstStream);
    }

    memset(pstStream, 0, sizeof(*pstStream));

    ret = ak_venc_encode_frame(codec_id,
                               pstFrame->stFrame.pu8Data,
                               pstFrame->stFrame.u32Len,
                               pstFrame->pvMdInfo,
                               pstStream);

    if (ret != AK_SUCCESS) {
        codec_io_unlock(device_id, channel_id, stream_id);
        log_error("ak_venc_encode_frame failed handle=%d ret=%d", codec_id, ret);
        return (size_t)-1;
    }

    *buffer = pstStream->data;
    *timestamp = pstStream->ts;
    codec_info->stream_buffer_release_flag = true;
    codec_info->frame_type = (int)pstStream->frame_type;

    size_t len = pstStream->len;
    if (len > 0)
        codec_pin_buf(device_id, channel_id, stream_id);
    codec_io_unlock(device_id, channel_id, stream_id);
    return len;
}

int hal_camera_codec_get_frame_type(void *hal_camera, int device_id, int channel_id,
                                    int stream_id)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;

    if (my_data == NULL || device_id < 0 || channel_id < 0 || stream_id < 0)
        return -1;
    if (device_id >= CAMERA_MAX_NUM || channel_id >= CAMERA_MAX_CHANNEL ||
        stream_id >= CAMERA_MAX_STREAM_PER_CHANNEL)
        return -1;

    return my_data->hal_camera_device[device_id].channel[channel_id].codec[stream_id].frame_type;
}
