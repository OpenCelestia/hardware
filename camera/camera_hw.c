// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/camera.
 *
 * Camera hardware abstraction layer implementation.
 * - Implements camera_hw_device_t methods for camera operations.
 * - Manages device, channel, and stream lifecycle.
 * - Handles capture requests, metadata, and AI processing.
 */
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <sys/cdefs.h>
#include <sys/types.h>
#include <stdbool.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#include "camera_hw.h"
#include "hal_camera_api.h"
#include "camera.h"
#include "log.h"
#include "system_pixel_format.h"
#include "hal_camera_chip_config.h"

#define LOG_TAG "HAL CAMERA"
#define NO_ERROR 0

/* HAL_CMAERA_BUFFER_COPY_MODE compatibility guard.
 * In copy mode the capture thread writes into result.output_buffers->data via memcpy().
 * After the deep-copy refactor, owned_output_buffers[].data is initialised to NULL
 * (copied from the framework which always passes NULL for non-copy mode).
 * Enabling the macro without also providing per-slot pre-allocated write buffers would
 * cause an immediate NULL-deref crash in the capture thread.
 * If you need to enable this macro, allocate a write buffer per slot inside
 * stream_info_copy_request() and free it in a matching stream_info_release_request(). */
#ifdef HAL_CMAERA_BUFFER_COPY_MODE
#error "HAL_CMAERA_BUFFER_COPY_MODE is defined but owned_output_buffers[].data is NULL after \
deep-copy.  You must allocate per-slot write buffers in stream_info_copy_request() before \
enabling this mode.  See comment above for details."
#endif

/* ============================================================================
 * Internal Helper Functions
 * ============================================================================ */

/* Framework sets stream->reserved[1] for preview-only main VI keepalive (no VENC). */
static int cam_stream_vi_keepalive(const camera_stream_t *stream)
{
    return stream != NULL && (intptr_t)stream->reserved[1] != 0;
}

static int cam_stream_should_enable_codec(const camera_stream_t *stream,
                                          camera_request_template_t tpl)
{
    if (tpl == CAMERA_TEMPLATE_AI || tpl == CAMERA_TEMPLATE_QR_SCAN)
        return 0;
    if (cam_stream_vi_keepalive(stream))
        return 0;
    return 1;
}

/* Main VI warm (keepalive off, VENC not opened): skip encode on this stream. */
static int cam_stream_vi_only_no_codec(const stream_info_t *stream_info,
                                       const camera_stream_t *stream)
{
    if (!stream_info || !stream || stream_info->codec_id >= 0)
        return 0;
    if (cam_stream_vi_keepalive(stream))
        return 0;
    if (stream->codec_config.codec_mode == CAMERA_CODEC_MODE_YUV ||
        stream->codec_config.codec_mode == CAMERA_CODEC_MODE_MJPEG)
        return 0;
    return 1;
}

static const char *cam_request_tpl_str(camera_request_template_t tpl)
{
    switch (tpl) {
    case CAMERA_TEMPLATE_NONE: return "none";
    case CAMERA_TEMPLATE_PREVIEW: return "preview";
    case CAMERA_TEMPLATE_VIDEO_RECORD: return "record";
    case CAMERA_TEMPLATE_VIDEO_CALL: return "call";
    case CAMERA_TEMPLATE_QR_SCAN: return "qr";
    case CAMERA_TEMPLATE_PICTURE: return "picture";
    case CAMERA_TEMPLATE_METADATA: return "meta";
    case CAMERA_TEMPLATE_AI: return "ai";
    case CAMERA_TEMPLATE_CUSTOM: return "custom";
    default: return "?";
    }
}

static void cam_stream_wh(char *wh, size_t wh_sz, const camera_stream_t *stream)
{
    if (!stream || !stream->channel_config) {
        snprintf(wh, wh_sz, "?");
        return;
    }
    snprintf(wh, wh_sz, "%dx%d", stream->channel_config[0].width, stream->channel_config[0].height);
}

/*
 * Close this stream's VENC if open. Needed when a stream leaves the batch while
 * siblings on the same VI channel stay ENABLE — camera_disable_channel() will not
 * run, so without this DMA pools accumulate across dashcam segment cuts.
 */
static void cam_stream_close_codec_if_open(struct camera_device *cdev,
                                          stream_info_t *stream_info)
{
    if (cdev == NULL || cdev->hal_camera == NULL || stream_info == NULL)
        return;
    if (stream_info->codec_id < 0 || stream_info->stream == NULL)
        return;
    (void)hal_camera_codec_disable(cdev->hal_camera, stream_info->stream);
}

/* Open VENC if this stream has no handle. Only a PICTURE failure fails the request. */
static int cam_stream_open_codec(struct camera_device *cdev, stream_info_t *stream_info,
                                 int device_id, int channel_id, int stream_id)
{
    int picture;

    if (stream_info->codec_id >= 0)
        return 0;
    picture = (stream_info->request.request_template == CAMERA_TEMPLATE_PICTURE);
    if (hal_camera_codec_enable(cdev->hal_camera, stream_info->stream) == 0
        && stream_info->codec_id >= 0)
        return 0;
    if (!picture)
        return 0;
    log_error("picture codec enable failed: dev=%d ch=%d str=%d",
              device_id, channel_id, stream_id);
    return HARDWARE_CAMERA_ERROR_OPERATION_FAILED;
}

int camera_disable_channel(const camera_hw_device_t *dev, int device_id, int channel_id);
int camera_enable_channel(const camera_hw_device_t *dev, int device_id, int channel_id);

static pthread_t camera_mark_channel_stopped(struct camera_device *cdev, int device_id,
                                             int channel_id);
static int camera_join_capture_thread(struct camera_device *cdev, int device_id, int channel_id,
                                      pthread_t tid);

static void camera_dump_close_state(struct camera_device *cdev)
{
    if (cdev == NULL) {
        return;
    }

    log_info("CAM-CLOSE-DUMP begin: cdev=%p hal_camera=%p num_device=%d closing=%d",
             (void *)cdev, cdev->hal_camera, cdev->num_device,
             __sync_add_and_fetch(&cdev->closing, 0));

    for (int d = 0; d < cdev->num_device; d++) {
        device_info_t *dev_info = &cdev->device_info[d];
        log_info("CAM-CLOSE-DUMP device[%d]: device_id=%d status=%d num_channel=%d",
                 d, dev_info->device_id, dev_info->status, dev_info->num_channel);

        for (int c = 0; c < dev_info->num_channel; c++) {
            channel_info_t *ch_info = &dev_info->channel_info[c];
            log_info("CAM-CLOSE-DUMP ch[%d]: status=%d thread_id=%lu num_stream=%d",
                     c, ch_info->status, (unsigned long)ch_info->thread_id, ch_info->num_stream);

            for (int s = 0; s < ch_info->num_stream; s++) {
                stream_info_t *st = &ch_info->stream_info[s];
                log_info("CAM-CLOSE-DUMP ch[%d].st[%d]: status=%d req_tpl=%d cur_tpl=%d codec_id=%d ai_type=0x%x ai_status=%d ai_thread_id=%lu",
                         c, s, st->status, st->request.request_template, st->request_template,
                         st->codec_id, st->ai_type, st->ai_status, (unsigned long)st->ai_thread_id);
            }
        }
    }

    log_info("CAM-CLOSE-DUMP end");
}

/**
 * @brief Deep-copy a capture request into a stream_info's owned storage.
 *
 * Copies the camera_capture_request_t fields and, crucially, copies the
 * output_buffers[] array into stream_info->owned_output_buffers[] so that
 * stream_info->request.output_buffers always points at HAL-owned memory.
 *
 * The capture thread dereferences request.output_buffers asynchronously every
 * frame; if that pointer ever refers to framework memory freed between two
 * successive process_capture_request() calls the thread writes into freed
 * memory → SIGSEGV (store page fault, SEGV_ACCERR on RISC-V).
 *
 * @param dst  Destination stream_info whose request/owned_output_buffers will be written.
 * @param src  Source camera_capture_request_t (from framework caller).
 */
static void stream_info_copy_request(stream_info_t *dst, const camera_capture_request_t *src)
{
    /* Shallow copy all scalar fields (frame_number, request_template, settings, input_buffer …) */
    memcpy(&dst->request, src, sizeof(camera_capture_request_t));

    /* Deep-copy output_buffers into owned storage and redirect the pointer.
     *
     * What each slot field means after copy:
     *   .stream    — pointer to streams_cfg[i] (framework static object); used by capture thread
     *                only for identity match (== comparison), never dereferenced for data.
     *                The framework object outlives any process_capture_request call, so this
     *                pointer is always valid.
     *   .data      — NULL at copy time (framework always passes NULL); HAL capture thread
     *                overwrites this each frame with codec_buffer / stream_buffer (non-copy mode)
     *                or uses it as a write target (HAL_CMAERA_BUFFER_COPY_MODE).
     *   .data_size — 0 at copy time; overwritten by capture thread each frame.
     *
     * IMPORTANT — HAL_CMAERA_BUFFER_COPY_MODE:
     *   If this macro is ever defined, capture thread does:
     *       memcpy(result.output_buffers->data, codec_buffer, bytes_read)
     *   which requires .data to point at a pre-allocated write buffer (not NULL).
     *   In that mode a per-stream DMA/kmalloc buffer must be allocated and stored in
     *   owned_output_buffers[i].data before the capture thread runs.
     *   Currently the macro is NOT defined; the non-copy path (direct pointer assignment)
     *   is used and .data = NULL is safe.
     *
     * Concurrency note:
     *   process_capture_request() runs under cdev->lock; the capture thread does NOT hold
     *   that lock.  Both may touch owned_output_buffers[] concurrently.  The fields written
     *   by the capture thread (.data, .data_size) are pointer/uint32 — single-instruction
     *   stores on aligned addresses, effectively atomic on RISC-V/ARM.  The window where
     *   the capture thread reads a partially-updated slot is bounded to one frame interval
     *   and results in at worst a skipped or corrupt frame — not a crash.  This race existed
     *   before deep-copy and is unchanged. */
    uint32_t n = src->num_output_buffers;
    if (n > STREAM_INFO_MAX_OUTPUT_BUFFERS) {
        log_warn("stream_info_copy_request: num_output_buffers=%u > max=%d, clamping",
                 n, STREAM_INFO_MAX_OUTPUT_BUFFERS);
        n = STREAM_INFO_MAX_OUTPUT_BUFFERS;
    }
    if (src->output_buffers != NULL && n > 0) {
        memcpy(dst->owned_output_buffers, src->output_buffers,
               n * sizeof(camera_stream_buffer_t));
    }
    dst->request.output_buffers = dst->owned_output_buffers;
    dst->request.num_output_buffers = n;
}


/**
 * @brief Get device index by device ID.
 * @param dev Camera hardware device pointer.
 * @param device_id Device ID.
 * @return Device index on success, -1 on failure.
 */
static int camera_get_device_index(const camera_hw_device_t *dev, int device_id)
{
    struct camera_device *cdev = (struct camera_device *)dev;

    for (int i = 0; i < cdev->num_device; i++) {
        if (device_id == cdev->device_info[i].device_id)
            return i;
    }
    return -1;
}

/**
 * @brief Close camera device.
 * @param device Hardware device pointer.
 * @return 0 on success, negative error code on failure.
 */
static int cdev_close(hw_device_t *device)
{
    log_info("[IPC-TEARDOWN] cdev_close: begin");
    log_debug("cdev_close");
    struct camera_device *cdev = (struct camera_device *)device;
    if (cdev != NULL) {
        /* Block new process_capture_request() before tearing down HAL resources. */
        __sync_lock_test_and_set(&cdev->closing, 1);
        camera_dump_close_state(cdev);
    }

    if (cdev != NULL && cdev->hal_camera != NULL) {
        /* Gracefully stop all running channels/devices before freeing memory. */
        for (int d = 0; d < cdev->num_device; d++) {
            int device_id = cdev->device_info[d].device_id;
            for (int c = 0; c < cdev->device_info[d].num_channel; c++) {
                channel_info_t *channel = &cdev->device_info[d].channel_info[c];
                for (int s = 0; s < channel->num_stream; s++) {
                    channel->stream_info[s].status = CAMERA_STATUS_DISABLE;
                }
                camera_disable_channel((const camera_hw_device_t *)cdev, device_id, c);
            }
            if (cdev->device_info[d].status != CAMERA_STATUS_UNINITIALIZE) {
                hal_camera_disable_device(cdev->hal_camera, device_id);
                cdev->device_info[d].status = CAMERA_STATUS_DISABLE;
            }
        }
        log_info("[CAM_TRACE] cdev_close: hal_camera_ai_shutdown_all");
        hal_camera_ai_shutdown_all(cdev->hal_camera);
        log_info("[CAM_TRACE] cdev_close: hal_camera_deinit");
        hal_camera_deinit(cdev->hal_camera);
        cdev->hal_camera = NULL;
    }

    free(cdev->ai_output_result.ai_object);
    cdev->ai_output_result.ai_object = NULL;

    pthread_mutex_destroy(&cdev->lock);
    pthread_mutex_destroy(&cdev->channel_lock);

    free(device);
    device = NULL;

    log_info("[IPC-TEARDOWN] cdev_close: done");
    return HARDWARE_CAMERA_OK;
}

/**
 * @brief Set callback operations.
 * @param dev Camera hardware device pointer.
 * @param callback_ops Callback operations pointer.
 * @return 0 on success, negative error code on failure.
 */
static int cdev_set_callback_ops(const camera_hw_device_t *dev,
        const camera_callback_ops_t *callback_ops)
{
    log_debug("camera device set callback ops");

    if (dev == NULL || callback_ops == NULL) {
        return HARDWARE_CAMERA_ERROR_INVALID_PARAM;
    }

    struct camera_device *cdev = (struct camera_device *)dev;

    pthread_mutex_lock(&cdev->lock);
    cdev->callback_ops = callback_ops;
    pthread_mutex_unlock(&cdev->lock);

    return HARDWARE_CAMERA_OK;
}


/**
 * @brief Configure camera streams.
 * @param dev Camera hardware device pointer.
 * @param stream_list Stream configuration pointer.
 * @return 0 on success, negative error code on failure.
 */
int camera_streams_configure(const camera_hw_device_t *dev,
        camera_stream_configuration_t *stream_list)
{
    struct camera_device *cdev = (struct camera_device *)dev;
    int ret = 0;
    int stream_index;
    int device_index = 0; //for cdev->device_info[device_index], thd device info index
    int device_id = 0; //save device id from framework config
    int channel_id = 0;
    int device_id_enabled[CAMERA_MAX_NUM];

    log_debug("camera_streams_configure");

    if(stream_list->streams == NULL)
    {
        log_error("stream_list->streams NULL error");
        return HARDWARE_CAMERA_ERROR_INVALID_PARAM;
    }

/* Check stream count */
    log_info("stream_list->num_streams:%d\n\n", stream_list->num_streams);
    if (stream_list->num_streams <= 0 || stream_list->num_streams > 16) {
        log_error("invalid num_streams: %d", stream_list->num_streams);
        return HARDWARE_CAMERA_ERROR_INVALID_PARAM;
    }
/*
 * Check all stream_list, save the stream to channel:
 * - One stream binding to one buffer
 * - One channel binding to one channel in one camera device
 * - One channel contains multiple streams, same width/height, different encoding
 * - One stream contains one channel from multiple cameras, combined into one stream
 */
    for(int i=0; i<CAMERA_MAX_NUM; i++)
    {
        device_id_enabled[i] = 0;
    }

/* Count all stream configs, determine which devices are opened */
    for(int i = 0; i < stream_list->num_streams; i++)
    {
        camera_stream_t *stream = stream_list->streams[i];
        if (stream == NULL) {
            log_error("stream[%d] is NULL", i);
            return HARDWARE_CAMERA_ERROR_INVALID_PARAM;
        }
        if (stream->channel_config == NULL) {
            log_error("stream[%d] channel_config is NULL, check video_cfg.json channels/channel_group_id", i);
            return HARDWARE_CAMERA_ERROR_INVALID_PARAM;
        }

        stream->priv = malloc(sizeof(camera_stream_t *) * CAMERA_MAX_NUM);
        if (stream->priv == NULL) {
            log_error("stream[%d] priv malloc failed", i);
            return HARDWARE_CAMERA_ERROR_MEMORY;
        }
        memset(stream->priv, 0, sizeof(camera_stream_t *) * CAMERA_MAX_NUM);

        if (stream->stream_format == CAMERA_STREAM_FORMAT_SINGLE) {
            stream->num_channel = 1;
            device_id_enabled[stream->channel_config[0].device_id] = 1;
        }
        else if(stream->stream_format == CAMERA_STREAM_FORMAT_SPLICE_VER || \
                stream->stream_format == CAMERA_STREAM_FORMAT_SPLICE_HOR)
        {
            if (stream->num_channel <= 0 || stream->num_channel > CAMERA_MAX_NUM)
                stream->num_channel = 1;
            for(int j=0; j<stream->num_channel; j++)
            {
                device_id_enabled[stream->channel_config[j].device_id] = 1;
            }
        }
    }

/* Save device_id */
    for(int i = 0; i < CAMERA_MAX_NUM; i++)
    {
        if(device_id_enabled[i] == 1)
        {
            cdev->device_info[device_index].device_id = i;/*save device_id to device_info[index]*/
            cdev->device_info[device_index].num_channel = 0;/*reset num_channel*/

            device_index ++;
        }
    }

    cdev->num_device = device_index;

    log_info("cdev->num_device:%d",cdev->num_device);

/* Initialize cdev, store streams by stream list index */
    for(int i = 0; i < stream_list->num_streams; i++)
    {
        camera_stream_t *stream = stream_list->streams[i];
        if (stream == NULL || stream->channel_config == NULL || stream->priv == NULL) {
            log_error("stream[%d] invalid (null stream/channel_config/priv)", i);
            return HARDWARE_CAMERA_ERROR_INVALID_PARAM;
        }
        if(stream->stream_format == CAMERA_STREAM_FORMAT_SINGLE)
        {
            stream->num_channel = 1;
        }
        if (stream->num_channel <= 0 || stream->num_channel > CAMERA_MAX_NUM) {
            log_warn("stream[%d] num_channel=%d invalid, clamp to 1", i, stream->num_channel);
            stream->num_channel = 1;
        }

        for(int stream_channel_config_id=0; stream_channel_config_id<stream->num_channel; stream_channel_config_id++)
        {
            camera_channel_config_t *channel_config = &stream->channel_config[stream_channel_config_id];
            int channel_id_registered = -1;

            device_id = channel_config->device_id;/*if not defined, device_id = 0*/
            if(device_id >= CAMERA_MAX_NUM || device_id < 0)
            {
                log_error("the device number:%d > the max camera number:%d", \
                device_id, CAMERA_MAX_NUM);
                return HARDWARE_CAMERA_ERROR_INVALID_PARAM;
            }
            device_index = camera_get_device_index(dev, device_id);

            device_info_t *device_info = &cdev->device_info[device_index];

            /*
             * Check if channel registered in this device:
             * - If not registered: num_stream = 0, save to channel_info[num_channel], num_channel++
             * - If registered: num_stream++, save to channel_info[channel_id].stream[num_stream]
             */
            for(int k=0; k<device_info->num_channel; k++)
            {

                camera_channel_config_t *saved_channel_config = &device_info->channel_info[k].channel_config;

                //by stream list index
                if( (channel_config->width == saved_channel_config->width) \
                    && (channel_config->height == saved_channel_config->height) )
                {
                    channel_id_registered = k;
                    break;
                }
            }

            if(channel_id_registered == -1) /*new channel*/
            {
                if(device_info->num_channel >= CAMERA_MAX_CHANNEL)
                {
                    /* Keep extra channel_* in fw_video_config.json for manual remap;
                     * only the first CAMERA_MAX_CHANNEL unique resolutions are created.
                     * Leave stream->priv NULL so framework stream_apply skips this stream. */
                    log_warn("skip stream[%d]: would create channel %d but CAMERA_MAX_CHANNEL=%d "
                             "(%dx%d). Remap channel_group_id / stream order if needed.",
                             i, device_info->num_channel, CAMERA_MAX_CHANNEL,
                             channel_config->width, channel_config->height);
                    continue;
                }

                channel_id = device_info->num_channel; /*save to new channel*/

                /*set stream to new channel -> stream[0]*/
                stream_index = device_info->channel_info[channel_id].num_stream = 0;

                stream_info_t *stream_info = &device_info->channel_info[channel_id].stream_info[stream_index];

                stream_info->stream =  stream;
                stream_info->device_id = device_id;
                stream_info->channel_id = channel_id;
                stream_info->stream_id = stream_index;

                stream_info->status = CAMERA_STATUS_UNINITIALIZE;
                stream_info->codec_id = -1;
                stream_info->format = stream->format;
                stream_info->stream_channel_config_id = stream_channel_config_id;

                /* Save channel config */
                memcpy(&device_info->channel_info[channel_id].channel_config, \
                            &stream->channel_config[stream_channel_config_id], sizeof(camera_channel_config_t));
                stream->priv[stream_channel_config_id] = stream_info;
                stream->priv_id = stream_channel_config_id;
                device_info->channel_info[device_info->num_channel].num_stream ++;

                log_info("!stream[%d] register to device[%d],channel[%d],stream[%d], width:%d,height:%d,priv:%x",i,device_id,channel_id,stream_index,channel_config->width,channel_config->height,stream_info);

                device_info->num_channel++;
            }
            else /*had registered, save to  channel_id_registered */
            {
                stream_index = device_info->channel_info[channel_id_registered].num_stream;
                stream_info_t *stream_info = &device_info->channel_info[channel_id_registered].stream_info[stream_index];

                if(stream_index >= CAMERA_MAX_STREAM_PER_CHANNEL)
                {
                    log_error("the stream per channel :%d > CAMERA_MAX_STREAM_PER_CHANNEL:%d", \
                    stream_index + 1, CAMERA_MAX_NUM);

                    return HARDWARE_CAMERA_ERROR_STREAM_LIMIT;
                }

                stream_info->stream = stream;
                stream_info->device_id = device_id;
                stream_info->channel_id = channel_id_registered;
                stream_info->stream_id = stream_index;

                stream_info->status = CAMERA_STATUS_UNINITIALIZE;
                stream_info->codec_id = -1;
                stream_info->format = stream->format;
                stream_info->stream_channel_config_id = stream_channel_config_id;

                stream->priv[stream_channel_config_id] = stream_info;
                stream->priv_id = stream_channel_config_id;

                log_info("@stream[%d] register to device[%d],channel[%d],stream[%d], width:%d,height:%d,priv:%x",i,device_id,stream_info->channel_id,stream_index,channel_config->width,channel_config->height,stream_info);

                device_info->channel_info[channel_id_registered].num_stream ++;
            }
        }
    }


    log_debug("num_device:%d", cdev->num_device);
    for(int i = 0; i < cdev->num_device; i++)
    {
        log_debug("device[%d] num_channel:%d", i, cdev->device_info[i].num_channel);

        for(int j = 0; j < cdev->device_info[i].num_channel; j++)
        {
            for(int k = 0; k < cdev->device_info[i].channel_info[j].num_stream; k++)
            {
                camera_stream_t *s = cdev->device_info[i].channel_info[j].stream_info[k].stream;
                log_debug("device[%d], channel[%d], stream[%d]: width:%d, height:%d, codec:%d",
                    i, j, k, cdev->device_info[i].channel_info[j].channel_config.width,
                    cdev->device_info[i].channel_info[j].channel_config.height,
                    s ? s->codec_config.codec_mode : -1);
            }
        }
    }

    /* Initialize device, create channels and streams */
    {
        int lazy_ch = hal_camera_lazy_channel_create_enabled();

        for (device_index = 0; device_index < cdev->num_device; device_index++)
        {
            device_id = cdev->device_info[device_index].device_id;
            device_info_t *device_info = &cdev->device_info[device_index];

            log_info("********************* device_index:%d", device_index);

            ret = hal_camera_create_device(cdev->hal_camera, device_id);
            if (ret != 0) {
                log_error("hal_camera_create_device failed: device_id=%d ret=%d", device_id, ret);
                return ret;
            }
            device_info->status = CAMERA_STATUS_CREATE;

            if (lazy_ch) {
                /*
                 * Lazy enable: pre-create SetChnAttr for ALL VPSS channels before
                 * StartGrp; only EnableChn / capture threads are deferred per stream.
                 * SetChnAttr after StartGrp (e.g. RTSP on ch0 then preview on ch1)
                 * hangs inside VPSS_CB_SetChnAttr — SDK order is CreateGrp -> SetChnAttr*
                 * -> StartGrp -> EnableChn.
                 */
                log_info("lazy_channel_create: pre-create VPSS attrs for all channels "
                         "(device_id=%d num_channel=%d)", device_id, device_info->num_channel);
                for (int channel_id = 0; channel_id < device_info->num_channel; channel_id++) {
                    device_info->channel_info[channel_id].status = CAMERA_STATUS_CREATE;
                    log_info("[CAM_TRACE] lazy_create: before hal_camera_create_channel dev=%d ch=%d",
                             device_id, channel_id);
                    ret = hal_camera_create_channel(cdev->hal_camera, device_id, channel_id);
                    log_info("[CAM_TRACE] lazy_create: after hal_camera_create_channel dev=%d ch=%d ret=%d",
                             device_id, channel_id, ret);
                    if (ret != 0) {
                        log_error("hal_camera_create_channel failed: device_id=%d channel_id=%d ret=%d",
                                  device_id, channel_id, ret);
                        return ret;
                    }
                }
                continue;
            }

            for (int channel_id = 0; channel_id < device_info->num_channel; channel_id++)
            {
                device_info->channel_info[channel_id].status = CAMERA_STATUS_CREATE;

                log_info("hal_camera_create_channel :[%d],channel[%d]", device_id, channel_id);

                ret = hal_camera_create_channel(cdev->hal_camera, device_id, channel_id);
                if (ret != 0) {
                    log_error("hal_camera_create_channel failed: device_id=%d channel_id=%d ret=%d",
                              device_id, channel_id, ret);
                    return ret;
                }
            }
        }
    }
    log_info("[CAM_TRACE] camera_streams_configure: done ret=0");
    return HARDWARE_CAMERA_OK;
}

// Configure camera stream, type, size, number of buffers
static int cdev_configure_streams(const camera_hw_device_t *dev,
        camera_stream_configuration_t *stream_list)
{
    struct camera_device *cdev = (struct camera_device *)dev;

    // Sanity check stream_list
    if (stream_list == NULL) {
        log_error("NULL stream configuration");
        return HARDWARE_CAMERA_ERROR_INVALID_PARAM;
    }
    if (stream_list->streams == NULL) {
        log_error("NULL stream list");
        return HARDWARE_CAMERA_ERROR_INVALID_PARAM;
    }

    if (stream_list->num_streams < 1) {
        log_error("Bad number of streams requested: %d",
                stream_list->num_streams);
        return HARDWARE_CAMERA_ERROR_INVALID_PARAM;
    }

    if( !dev )
    {
        log_error("NULL device");
        return HARDWARE_CAMERA_ERROR_INVALID_PARAM;
    }

    pthread_mutex_lock(&cdev->lock);
    log_info("[CAM_TRACE] cdev_configure_streams: lock acquired (tid=%lu)",
             (unsigned long)pthread_self());
    int ret = camera_streams_configure(dev, stream_list);
    log_info("[CAM_TRACE] cdev_configure_streams: camera_streams_configure ret=%d", ret);
    pthread_mutex_unlock(&cdev->lock);
    log_info("[CAM_TRACE] cdev_configure_streams: lock released");

    return ret;
}


/**
 * Configures the camera streams
 * @param hal_camera Pointer to the camera data
 * @param stream_list Pointer to the stream configuration
 * @return 0 on success, -1 on failure
 */
static int camera_streams_reconfigure_for_stream_change(const camera_hw_device_t *dev,
        camera_stream_configuration_t *stream_list)
{
    struct camera_device *cdev = (struct camera_device *)dev;
    int ret = 0;
    int stream_index;
    int device_index = 0; //for cdev->device_info[device_index], thd device info index
    int device_id = 0; //save device id from framework config
    int channel_id = 0;
    int num_device;
    int num_reconfigure_add_channel;

    camera_reconfigure_device_t reconfigure_device[CAMERA_MAX_NUM];
    int reconfigure_remove_channel[CAMERA_MAX_CHANNEL];
    int num_reconfigure_remove_channel;

    log_debug( "%s", __func__);

    if(stream_list->streams == NULL)
    {
        log_error("stream_list->streams NULL error");
        return HARDWARE_CAMERA_ERROR_INVALID_PARAM;
    }

/* Set all reconfigure streams to reset status */
    for(int i = 0; i < stream_list->num_streams; i++)
    {
        camera_stream_t *stream = stream_list->streams[i];
        for(int j=0; j<stream->num_channel; j++)
        {
            stream_info_t *stream_info = stream->priv[j];

            log_info("reconfigure, width:%d,height:%d",stream->channel_config[j].width,stream->channel_config[j].height);

            /* If width and height are the same, just reset codec without reconfigure */
            if(stream->channel_config[j].width ==  cdev->device_info[stream_info->device_id].channel_info[stream_info->channel_id].channel_config.width\
                && stream->channel_config[j].height == cdev->device_info[stream_info->device_id].channel_info[stream_info->channel_id].channel_config.height)
            {
                log_info("stream codec reset:[%d].[%d].[%d]", \
                    stream_info->device_id,stream_info->channel_id,stream_info->stream_id);

                if(stream_info->codec_id >= 0 )
                {
                    hal_camera_codec_disable(cdev->hal_camera, stream_info->stream);
                }

                if((stream_info->request.request_template != CAMERA_TEMPLATE_AI) \
                    &&(stream_info->request.request_template != CAMERA_TEMPLATE_QR_SCAN) )
                {
                    hal_camera_codec_enable(cdev->hal_camera, stream_info->stream);
                }
            }
            else
            {
                stream_info->status |= CAMERA_STATUS_RESET;// reset
                log_info("stream reset:[%d].[%d].[%d]", \
                    stream_info->device_id,stream_info->channel_id,stream_info->stream_id);
            }
        }
    }

//change stream status, reset request template
    for(int device_id=0; device_id<cdev->num_device; device_id++)
    {
        num_reconfigure_remove_channel = 0;  /* Channels to remove, IDs saved to reconfigure_remove_channel */
        num_reconfigure_add_channel = 0;     /* New channels, info saved to channel_reconfigure */

        camera_reconfigure_channel_t channel_recofigure[CAMERA_MAX_CHANNEL];
        device_info_t *device_info = &cdev->device_info[device_id];
        for(int channel_id=0; channel_id<cdev->device_info[device_id].num_channel; channel_id++)
        {
                /* Flag to confirm if all streams in this channel are reset */
                int stream_no_reset_flag = 0;

            for(int stream_id=0; stream_id<device_info->channel_info[channel_id].num_stream; stream_id++)
            {
                channel_info_t *channel_info = &device_info->channel_info[channel_id];
                stream_info_t *stream_info = &device_info->channel_info[channel_id].stream_info[stream_id];
                int reconfigure_channel_flag = 0; // Whether corresponding channel has been newly defined

                log_debug("[%d],[%d], status:%d", channel_id, stream_id, stream_info->status & ~CAMERA_STATUS_RESET);

                if((stream_info->status & CAMERA_STATUS_RESET) == CAMERA_STATUS_RESET)
                {
                    int channel_add_id;
                    for(channel_add_id=0; channel_add_id<num_reconfigure_add_channel; channel_add_id++)
                    {
                        if(channel_recofigure[channel_add_id].width == channel_info->channel_config.width \
                            && channel_recofigure[channel_add_id].height == channel_info->channel_config.height)
                        {
                            reconfigure_channel_flag = 1;

                            int stream_reconfigure_id = channel_recofigure[channel_add_id].num_stream;
                            channel_recofigure[channel_add_id].stream_reconfigure[stream_reconfigure_id].stream = stream_info->stream;
                            channel_recofigure[channel_add_id].stream_reconfigure[stream_reconfigure_id].status = stream_info->status & (~CAMERA_STATUS_RESET);
                            channel_recofigure[channel_add_id].stream_reconfigure[stream_reconfigure_id].stream_channel_config_id = stream_info->stream_channel_config_id;

                            log_debug("reconfigure: channel_id:%d, stream_id:%d", channel_add_id, stream_reconfigure_id);

                            channel_recofigure[channel_add_id].num_stream ++;

                            stream_info->status = CAMERA_STATUS_REMOVE;
                            break;
                        }
                    }

                    if(reconfigure_channel_flag == 0)
                    {
                        channel_recofigure[num_reconfigure_add_channel].width = channel_info->channel_config.width;
                        channel_recofigure[num_reconfigure_add_channel].height = channel_info->channel_config.height;
                        channel_recofigure[num_reconfigure_add_channel].channel_id = -1;
                        channel_recofigure[num_reconfigure_add_channel].stream_reconfigure[0].stream = stream_info->stream;
                        memcpy(&channel_recofigure[num_reconfigure_add_channel].stream_reconfigure[0].request, \
                            &stream_info->request, sizeof(camera_capture_request_t));
                        channel_recofigure[num_reconfigure_add_channel].stream_reconfigure[0].status = stream_info->status & (~CAMERA_STATUS_RESET);
                        channel_recofigure[num_reconfigure_add_channel].stream_reconfigure[0].stream_channel_config_id = stream_info->stream_channel_config_id;
                        stream_info->status = CAMERA_STATUS_REMOVE;

                        channel_recofigure[num_reconfigure_add_channel].num_stream = 1;
                        log_debug("@change: channel_id:%d,stream_id:0, stream:%x, old width:%d,old height:%d,new width:%d,new height:%d", \
                        num_reconfigure_add_channel,  stream_info->stream,  \
                        channel_recofigure[num_reconfigure_add_channel].width, channel_recofigure[num_reconfigure_add_channel].height, \
                        stream_info->stream->channel_config[stream_info->stream_channel_config_id].width,stream_info->stream->channel_config[stream_info->stream_channel_config_id].height);

 //                       log_debug(",status:%d", \
                        channel_recofigure[num_reconfigure_add_channel].stream_reconfigure[0].status);
                        num_reconfigure_add_channel++;
                    }

                }
                else
                {
                    stream_no_reset_flag = 1;
                }
            }

            if(stream_no_reset_flag == 0)
            {
                reconfigure_remove_channel[num_reconfigure_remove_channel] = channel_id;
                num_reconfigure_remove_channel++;
            }
        }

    /*
     * Process recorded newly added channels and deleted channels
 * 1. Confirm if newly added channel count + original channel count - deleted channel count > max channel count, if so return error;
 * 2. Check if deleted channel is one of the newly added ones, if so directly reset and replace, otherwise delete original channel
    * 3. Add newly added channels one by one
    */
        log_info("reconfigure source channel:%d, add channel:%d, remove channel:%d", \
            device_info->num_channel, num_reconfigure_add_channel, num_reconfigure_remove_channel);

        if((device_info->num_channel + num_reconfigure_add_channel -  num_reconfigure_remove_channel) > CAMERA_MAX_CHANNEL)
        {
            log_error("reconfigure fail, too much channel, source channel:%d, add channel:%d, remove channel:%d, max channel:%d", \
            device_info->num_channel, num_reconfigure_add_channel, num_reconfigure_remove_channel, CAMERA_MAX_CHANNEL );
            return HARDWARE_CAMERA_ERROR_CHANNEL_LIMIT;
        }

        pthread_mutex_lock(&cdev->channel_lock);

        /*disable remove channel*/
        for(int remove_id=0; remove_id<num_reconfigure_remove_channel; remove_id++ )
        {
            int remove_channel_flag = 1;
            int channel_id = reconfigure_remove_channel[remove_id];

            for(int add_channel_id=0; add_channel_id<num_reconfigure_add_channel ;add_channel_id++)
            {
                int remove_channel_width = device_info->channel_info[channel_id].channel_config.width;
                int remove_channel_height = device_info->channel_info[channel_id].channel_config.height;

                if((channel_recofigure[add_channel_id].width == remove_channel_width) \
                && (channel_recofigure[add_channel_id].height == remove_channel_height))
                {
                    //save channel id to the new recofigure channel
                    channel_recofigure[add_channel_id].channel_id = channel_id;
                }

                //disable channel info
                if(device_info->channel_info[channel_id].status == CAMERA_STATUS_ENABLE)
                {
                    for(int stream_id=0; stream_id<device_info->channel_info[channel_id].num_stream; stream_id++)
                    {
                        stream_info_t *stream_info = &device_info->channel_info[channel_id].stream_info[stream_id];
                        if(stream_info->status == CAMERA_STATUS_ENABLE)
                        {
                            stream_info->status = CAMERA_STATUS_DISABLE;
                        }
                    }
                    log_info("remove: device_id:%d,channel_id:%d", device_id,  channel_id);
                    device_info->channel_info[channel_id].status = CAMERA_STATUS_BUSY;
                    /* camera_disable_channel joins the capture thread; must not hold channel_lock. */
                    pthread_mutex_unlock(&cdev->channel_lock);
                    camera_disable_channel((const camera_hw_device_t *)cdev, device_id, channel_id);
                    pthread_mutex_lock(&cdev->channel_lock);
                }
            }
        }

        pthread_mutex_unlock(&cdev->channel_lock);

        /*configure add channel and enable*/
        for(int add_id=0; add_id<num_reconfigure_add_channel; add_id++)
        {
            int channel_id;
            if(channel_recofigure[add_id].channel_id == -1)
            {
                int channel_id_registered_flag = 0;
                for(int i=0; i<device_info->num_channel; i++)
                {
                    if(device_info->channel_info[i].status == CAMERA_STATUS_BUSY )
                    {
                        channel_id = i;
                        channel_id_registered_flag = 1;
                        break;
                    }
                }
                if(channel_id_registered_flag == 0)
                {
                    channel_id = device_info->num_channel;
                    device_info->num_channel++;
                }
            }
            else
            {
                channel_id = channel_recofigure[add_id].channel_id;
            }


            int stream_channel_config_id = channel_recofigure[add_id].stream_reconfigure[stream_index].stream_channel_config_id;

            camera_stream_t *stream = channel_recofigure[add_id].stream_reconfigure[0].stream;

            //channel_config save 
            memcpy(&device_info->channel_info[channel_id].channel_config, \
                    &stream->channel_config[stream_channel_config_id], sizeof(camera_channel_config_t));

            int channel_id_enable_flag = 0;

//            log_info("num_stream:%d",channel_recofigure[add_id].num_stream);

            device_info->channel_info[channel_id].num_stream = 0;
            for(int stream_index=0; stream_index<channel_recofigure[add_id].num_stream; stream_index++)
            {
                stream_info_t *stream_info = &device_info->channel_info[channel_id].stream_info[stream_index];

                if(stream_index >= CAMERA_MAX_STREAM_PER_CHANNEL)
                {
                    log_error("the stream per channel :%d > CAMERA_MAX_STREAM_PER_CHANNEL:%d", \
                    stream_index + 1, CAMERA_MAX_NUM);

                    return HARDWARE_CAMERA_ERROR_STREAM_LIMIT;
                }
                log_info("add: channel_id:%d,stream_id:%d, stream:%x", add_id, stream_index, stream_info->stream);

//                log_error("status:%d", \
                        channel_recofigure[add_id].stream_reconfigure[stream_index].status);

                stream_info->stream = channel_recofigure[add_id].stream_reconfigure[stream_index].stream;

                stream_info->device_id = device_id;
                stream_info->channel_id = channel_id;
                stream_info->stream_id = stream_index;

                stream_info->status = channel_recofigure[add_id].stream_reconfigure[stream_index].status;
                stream_info->codec_id = -1;
                stream_info->format = channel_recofigure[add_id].stream_reconfigure[stream_index].stream->format;

                stream_info->stream_channel_config_id = channel_recofigure[add_id].stream_reconfigure[stream_index].stream_channel_config_id;

                channel_recofigure[add_id].stream_reconfigure[stream_index].stream->priv[channel_id] = stream_info;

                channel_recofigure[add_id].stream_reconfigure[stream_index].stream->priv_id = channel_id;

                if(stream_info->status == CAMERA_STATUS_ENABLE)
                {
                    channel_id_enable_flag |= 1; // Status already opened before reconfigure
                }
                else
                {
                    channel_id_enable_flag |= 2;                            
                }

//                log_info("stream register to device[%d],channel[%d],stream[%d], width:%d,height:%d,priv:%x", \
                      device_id, stream_info->channel_id, stream_index, \
                      stream_info->stream->channel_config[stream_info->stream_channel_config_id].width, \
                      stream_info->stream->channel_config[stream_info->stream_channel_config_id].height,stream_info);

                /* Deep-copy: output_buffers in reconfigure.request points at the old stream_info's
                 * owned_output_buffers; stream_info_copy_request() copies the array contents into
                 * this (new) stream_info's own owned_output_buffers and fixes the pointer. */
                stream_info_copy_request(stream_info, &channel_recofigure[add_id].stream_reconfigure[stream_index].request);

                device_info->channel_info[channel_id].num_stream ++;

            }

//            log_info("num_stream:%d",channel_recofigure[add_id].num_stream);

            hal_camera_create_channel(cdev->hal_camera, device_id, channel_id);

            //enable codec
            for(int stream_index=0; stream_index<channel_recofigure[add_id].num_stream; stream_index++)
            {
                stream_info_t *stream_info = &device_info->channel_info[channel_id].stream_info[stream_index];

                /*enable codec*/
                if((stream_info->request.request_template != CAMERA_TEMPLATE_AI) \
                    && (stream_info->stream->format != CAMERA_CODEC_MODE_YUV))
                {
                    hal_camera_codec_enable(cdev->hal_camera, stream_info->stream);
                }

                stream_info->request_template = stream_info->request.request_template;
            }

            /*enable channel*/
            if((channel_id_enable_flag & 1) == 1)
            {
                log_info("camera_enable_channel :[%d],channel[%d]", device_id, channel_id);
                    hal_camera_enable_channel(cdev->hal_camera, device_id, channel_id);
                    device_info->channel_info[channel_id].status = CAMERA_STATUS_ENABLE;
                    //camera_enable_channel(dev, device_id, channel_id);
            }
            else if(channel_id_enable_flag == 2)
            {
                log_info("camera_enable_channel :[%d],channel[%d]", device_id, channel_id);
                    camera_enable_channel(dev, device_id, channel_id);
                    device_info->channel_info[channel_id].status = CAMERA_STATUS_ENABLE;
            }
        }

        // Need to detect all CAMERA_STATUS_BUSY and change to REMOVE
        for(int channel_id=0; channel_id<cdev->device_info[device_id].num_channel; channel_id++)
        {
            device_info_t *device_info = &cdev->device_info[device_id];

            if(device_info->channel_info[channel_id].status == CAMERA_STATUS_BUSY)
            {
                device_info->channel_info[channel_id].status = CAMERA_STATUS_REMOVE;
            }
        }

    }

//***********************************
//set stream
// Device initialization and channel, stream related interfaces
//*********************************
    for(device_index=0; device_index<cdev->num_device; device_index++)
    {
        device_id = cdev->device_info[device_index].device_id;
        device_info_t *device_info = &cdev->device_info[device_index];

        for(int channel_id = 0; channel_id < device_info->num_channel; channel_id++)
        {
            channel_info_t *channel_info = &device_info->channel_info[channel_id];

            for(int stream_id=0; stream_id < channel_info->num_stream; stream_id++ )
            {
                stream_info_t *stream_info =  &channel_info->stream_info[stream_id];
        
                log_info(" num_stream:%d, [%d][%d][%d]:%d,",channel_info->num_stream, \
                     device_id, channel_id,stream_id, stream_info->status);
            }
        }
    }

    log_info("reconfigure exit");
    return HARDWARE_CAMERA_OK;
}


// Configure camera stream, type, size, number of buffers
static int cdev_reconfigure_streams(const camera_hw_device_t *dev,
        camera_stream_configuration_t *stream_list)
{
    struct camera_device *cdev = (struct camera_device *)dev;

    // Sanity check stream_list
    if (stream_list == NULL) {
        log_error("NULL stream configuration");
        return HARDWARE_CAMERA_ERROR_INVALID_PARAM;
    }
    if (stream_list->streams == NULL) {
        log_error("NULL stream list");
        return HARDWARE_CAMERA_ERROR_INVALID_PARAM;
    }

    if (stream_list->num_streams < 1) {
        log_error("Bad number of streams requested: %d",
                stream_list->num_streams);
        return HARDWARE_CAMERA_ERROR_INVALID_PARAM;
    }

    if( !dev )
    {
        log_error("NULL device");
        return HARDWARE_CAMERA_ERROR_INVALID_PARAM;
    }

    pthread_mutex_lock(&cdev->lock);
    int ret = camera_streams_reconfigure_for_stream_change(dev, stream_list);
    pthread_mutex_unlock(&cdev->lock);

    return ret;
}



// Build default capture request settings, generate corresponding metadata configuration (such as 3A) according to different types (such as preview, snapshot, video recording).
// Will be generated after defining the setting structure later 
static const camera_metadata_t *cdev_construct_default_request_settings(
        const camera_hw_device_t *dev, int type)
{
    struct camera_device *cdev = (struct camera_device *)dev;

    camera_metadata_t *request_setting = NULL;

}

/**
 * @brief Finish one-shot picture capture from the capture thread (no self-join).
 */
static void camera_picture_finish_from_capture_thread(const camera_hw_device_t *dev,
                                                      channel_info_t *channel_info,
                                                      int device_id, int channel_id,
                                                      stream_info_t *stream_info)
{
    struct camera_device *cdev = (struct camera_device *)dev;

    if (cdev->hal_camera != NULL && stream_info->stream != NULL) {
        (void)hal_camera_codec_disable(cdev->hal_camera, stream_info->stream);
    }
    stream_info->status = CAMERA_STATUS_UNINITIALIZE;
    stream_info->request_template = CAMERA_TEMPLATE_NONE;

    int other_active = 0;
    pthread_mutex_lock(&cdev->channel_lock);
    for (int sid = 0; sid < channel_info->num_stream; sid++) {
        if (channel_info->stream_info[sid].status == CAMERA_STATUS_ENABLE) {
            other_active = 1;
            break;
        }
    }
    if (!other_active) {
        channel_info->status = CAMERA_STATUS_DISABLE;
        pthread_mutex_unlock(&cdev->channel_lock);

        if (cdev->hal_camera != NULL) {
            hal_camera_quiesce_channel(cdev->hal_camera, device_id, channel_id);
            hal_camera_disable_channel(cdev->hal_camera, device_id, channel_id);
        }

        pthread_mutex_lock(&cdev->channel_lock);
        channel_info->thread_id = 0;
        pthread_mutex_unlock(&cdev->channel_lock);
        pthread_exit(NULL);
    }
    pthread_mutex_unlock(&cdev->channel_lock);
}

/* YUV copy beside an encoder. Wait before the next read releases the frame. */
struct yuv_side {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int ready;
    int posted;
    int finished;
    const camera_callback_ops_t *ops;
    camera_capture_result_t result;
    camera_stream_buffer_t buf;
};

static struct yuv_side g_yuv_side[CAMERA_MAX_CHANNEL];
static pthread_once_t g_yuv_once = PTHREAD_ONCE_INIT;

static void yuv_side_init_all(void)
{
    for (int i = 0; i < CAMERA_MAX_CHANNEL; i++) {
        pthread_mutex_init(&g_yuv_side[i].mu, NULL);
        pthread_cond_init(&g_yuv_side[i].cv, NULL);
    }
}

static void *yuv_side_thread(void *arg)
{
    struct yuv_side *y = arg;
    camera_capture_result_t result;
    const camera_callback_ops_t *ops;

    pthread_setname_np(pthread_self(), "cam_yuv");
    for (;;) {
        pthread_mutex_lock(&y->mu);
        while (!y->posted)
            pthread_cond_wait(&y->cv, &y->mu);
        ops = y->ops;
        result = y->result;
        y->posted = 0;
        pthread_mutex_unlock(&y->mu);

        if (ops->process_capture_result != NULL)
            ops->process_capture_result(ops, &result);

        pthread_mutex_lock(&y->mu);
        y->finished = 1;
        pthread_cond_signal(&y->cv);
        pthread_mutex_unlock(&y->mu);
    }
    return NULL;
}

static int yuv_side_post(int channel_id, const camera_callback_ops_t *ops,
                         const camera_capture_result_t *result)
{
    struct yuv_side *y;

    if (channel_id < 0 || channel_id >= CAMERA_MAX_CHANNEL || ops == NULL)
        return -1;
    pthread_once(&g_yuv_once, yuv_side_init_all);
    y = &g_yuv_side[channel_id];
    if (!y->ready) {
        pthread_t th;

        if (pthread_create(&th, NULL, yuv_side_thread, y) != 0)
            return -1;
        pthread_detach(th);
        y->ready = 1;
    }
    pthread_mutex_lock(&y->mu);
    y->buf = *result->output_buffers;
    y->result = *result;
    y->result.output_buffers = &y->buf;
    y->ops = ops;
    y->finished = 0;
    y->posted = 1;
    pthread_cond_signal(&y->cv);
    pthread_mutex_unlock(&y->mu);
    return 0;
}

static void yuv_side_wait(int channel_id)
{
    struct yuv_side *y = &g_yuv_side[channel_id];

    pthread_mutex_lock(&y->mu);
    while (!y->finished)
        pthread_cond_wait(&y->cv, &y->mu);
    pthread_mutex_unlock(&y->mu);
}

static int channel_has_video_encoder(const channel_info_t *channel_info, int skip_stream)
{
    for (int s = 0; s < channel_info->num_stream; s++) {
        const camera_stream_t *sib = channel_info->stream_info[s].stream;

        if (s == skip_stream || channel_info->stream_info[s].codec_id < 0 || sib == NULL)
            continue;
        if (sib->codec_config.codec_mode == CAMERA_CODEC_MODE_AVC ||
            sib->codec_config.codec_mode == CAMERA_CODEC_MODE_HEVC)
            return 1;
    }
    return 0;
}

/**
 * @brief Capture thread function for a channel.
 * @param arg Pointer to channel_info_t.
 * @return NULL.
 */
void* camera_process_capture_thread_function(void* arg) {
	channel_info_t *channel_info = (channel_info_t*)arg;
    camera_device_t *cdev = (camera_device_t *)channel_info->dev;
    camera_hw_device_t *dev = (camera_hw_device_t *)cdev;

    int device_id = channel_info->stream_info[0].device_id;
    int channel_id = channel_info->stream_info[0].channel_id;
    void * stream_buffer = NULL;
    void * codec_buffer = NULL;
    int fps =  channel_info->stream_info[0].stream->fps;
    int bytes_read;
    int bytes = 0;
    void *output_buffer;
    uint64_t channel_timestamp;
    uint64_t codec_timestamp;

    camera_capture_result_t result = {};
    unsigned preview_div = 0;

    {
        char tname[16];
        snprintf(tname, sizeof(tname), "cam_cap%d", channel_id);
        pthread_setname_np(pthread_self(), tname);
    }
    log_debug("process:device_id %d, channel_id:%d",device_id,channel_id);
    int exit_reason = 0;

    while((channel_info->status & CAMERA_STATUS_WORK_MASK) == CAMERA_STATUS_ENABLE ||
        (channel_info->status & CAMERA_STATUS_WORK_MASK) == CAMERA_STATUS_BUSY)
    {
        if (cdev != NULL && __sync_add_and_fetch(&cdev->closing, 0) != 0) {
            exit_reason = 1; /* closing */
            break;
        }
        /*
         * Defensive exit conditions:
         * - During global shutdown (cdev_close), hal_camera may be deinitialized and set to NULL.
         * - Device may be disabled while some channel status is still ENABLE/BUSY.
         * In either case, this thread must exit to avoid keeping the process alive.
         */
        if (cdev == NULL || cdev->hal_camera == NULL) {
            exit_reason = 2; /* hal null */
            break;
        }
        if (cdev->device_info[device_id].status == CAMERA_STATUS_DISABLE) {
            exit_reason = 3; /* device disabled */
            break;
        }

        /* Only depend on this channel's own status; do not gate channel[1] capture on channel[0] status,
         * otherwise preview-only (video1) will never read frames and capture thread will just sleep. */
        int ai_stream_bypass_flag = 0;
        for(int stream_id=0; stream_id<channel_info->num_stream; stream_id++)
        {
            stream_info_t *stream_info = &channel_info->stream_info[stream_id];

            if(stream_info->request.request_template == CAMERA_TEMPLATE_AI && \
                stream_info->ai_status != CAMERA_STATUS_ENABLE)
            {
                ai_stream_bypass_flag = 1;
            }
        }

        if(ai_stream_bypass_flag == 0)
        {
            int allow_read = 0;
            int ch_work = 0;
            pthread_mutex_lock(&cdev->channel_lock);
            ch_work = (channel_info->status & CAMERA_STATUS_WORK_MASK);
            if (ch_work == CAMERA_STATUS_ENABLE || ch_work == CAMERA_STATUS_BUSY) {
                allow_read = 1;
            }
            pthread_mutex_unlock(&cdev->channel_lock);

            if (allow_read) {
                bytes_read = hal_camera_stream_read(cdev->hal_camera, device_id, channel_id,
                                                    &stream_buffer, &channel_timestamp, bytes);
                if (bytes_read > 0) {
                    log_debug("capture read: device_id:%d channel_id:%d bytes_read:%d",
                              device_id, channel_id, bytes_read);
                }
            } else {
                bytes_read = -1;
            }
        }
        else
        {
            continue;
        }

        {
            int still_active = 0;
            pthread_mutex_lock(&cdev->channel_lock);
            if ((channel_info->status & CAMERA_STATUS_WORK_MASK) == CAMERA_STATUS_ENABLE ||
                (channel_info->status & CAMERA_STATUS_WORK_MASK) == CAMERA_STATUS_BUSY) {
                still_active = 1;
            }
            pthread_mutex_unlock(&cdev->channel_lock);
            if (!still_active) {
                exit_reason = 6; /* stop: channel disabled before processing frame */
                break;
            }
        }

        if(bytes_read > 0)
        {
            stream_info_t *yuv_pending = NULL;

            for(int stream_id=0; stream_id<channel_info->num_stream; stream_id++)
            {
                int codec_buf_pinned = 0;
                stream_info_t *stream_info = &channel_info->stream_info[stream_id];
                /* Reset per-stream to avoid carrying stale pointer into logs. */
                codec_buffer = NULL;

                /* Snapshot the request fields that process_capture_request() may update concurrently.
                 *
                 * process_capture_request() runs under cdev->lock and calls stream_info_copy_request(),
                 * which writes stream_info->request (including owned_output_buffers) while this capture
                 * thread reads the same memory without any lock.  That is a data race.
                 *
                 * Fix: take channel_lock for the minimum window needed to copy the mutable fields into
                 * local variables.  The lock is NOT held across hal_camera_stream_read / codec_read
                 * (those are blocking calls; holding the lock would deadlock against the control path
                 * that calls camera_disable_channel → pthread_join while holding channel_lock).
                 *
                 * Fields snapshotted:
                 *   snap_template   — request_template (enum, 4 bytes, atomic on any sane arch)
                 *   snap_buf_slot   — pointer to the owned_output_buffers slot matching this stream,
                 *                     found by scanning output_buffers[0..n-1] for .stream == stream.
                 *                     After the lock is released, snap_buf_slot points into the HAL-owned
                 *                     owned_output_buffers[] array (never freed), so dereferencing it
                 *                     outside the lock is safe.
                 */
                camera_request_template_t snap_template;
                camera_stream_buffer_t   *snap_buf_slot = NULL;
                int snap_stream_status;

                pthread_mutex_lock(&cdev->channel_lock);
                snap_template     = stream_info->request.request_template;
                snap_stream_status = stream_info->status;
                if (snap_template != CAMERA_TEMPLATE_AI &&
                    snap_template != CAMERA_TEMPLATE_NONE &&
                    snap_stream_status == CAMERA_STATUS_ENABLE &&
                    stream_info->request.output_buffers != NULL)
                {
                    camera_stream_t *stream = stream_info->stream;
                    int num_bufs = (int)stream_info->request.num_output_buffers;
                    for (int b = 0; b < num_bufs; b++) {
                        if (stream_info->request.output_buffers[b].stream == stream) {
                            snap_buf_slot = &stream_info->request.output_buffers[b];
                            break;
                        }
                    }
                }
                pthread_mutex_unlock(&cdev->channel_lock);

                /* AI doesn't fetch data, directly process AI */
                if(snap_template == CAMERA_TEMPLATE_AI)
                {
                    if(stream_info->ai_status == CAMERA_STATUS_ENABLE)
                    {
                        stream_info->ai_status = CAMERA_STATUS_READY;
                        {
                            static unsigned s_ai_frame_ready_log;
                            if (s_ai_frame_ready_log < 10u) {
                                s_ai_frame_ready_log++;
                                log_info("[CAM_TRACE] ai_frame_ready: dev=%d ch=%d str=%d bytes=%d ts=%llu",
                                         device_id, channel_id, stream_id, bytes_read,
                                         (unsigned long long)channel_timestamp);
                            }
                        }
                    }
//                    log_debug("ai processing... channel_id:%d,stream_id:%d",channel_id,stream_id);
//                    hal_camera_ai_process(cdev->hal_camera, device_id, channel_id,stream_id);
                }
                else if(snap_template == CAMERA_TEMPLATE_NONE)
                {
                    if(stream_info->request_template != CAMERA_TEMPLATE_NONE)
                    {
                        log_debug("set request to none,channel_id:%d,stream_id:%d",channel_id,stream_id);

                        stream_info->status = CAMERA_STATUS_DISABLE;
                        stream_info->request_template = CAMERA_TEMPLATE_NONE;
                    }
                }
                else
                {
                    if(snap_stream_status == CAMERA_STATUS_ENABLE)
                    {
                        camera_stream_t *stream = stream_info->stream;
//                        log_debug("stream capture,channel_id:%d,stream_id:%d",channel_id,stream_id);

                        // Extract 3A metadata
                        // result.result = getResultMetadata(stream_info->request_template);
                        // Populate metadata result
                        result.frame_number = stream_info->request.frame_number;

                        /*
                         * Use the output_buffers slot that matches this stream (found under lock above).
                         * With video_cfg.json, channel 1 has two streams (video1 + qr_scan) but the
                         * recorder request may only include video0 and video1. If we deliver qr_scan
                         * into slot 0 we corrupt the main stream and cause segfault. Only deliver
                         * when this stream is actually in the request (has a matching buffer slot).
                         */
                        if (snap_buf_slot == NULL) {
                            /* Either output_buffers was NULL or this stream was not in the active request */
                            if (stream_info->request.output_buffers == NULL)
                                log_warn("channel_id=%d stream_id=%d: request output_buffers NULL", channel_id, stream_id);
                            continue;
                        }
                        result.num_output_buffers = 1;
                        result.output_buffers = snap_buf_slot;

                        if (cam_stream_vi_keepalive(stream)) {
                            stream_info->request.frame_number++;
                            continue;
                        }
                        if (cam_stream_vi_only_no_codec(stream_info, stream)) {
                            continue;
                        }

                        if(stream->codec_config.codec_mode == CAMERA_CODEC_MODE_YUV)
                        {
                            int sibling_venc = channel_has_video_encoder(channel_info, stream_id);

                            result.output_buffers->stream = channel_info->stream_info[stream_id].stream;//
                            result.timestamp = channel_timestamp;
                            /* For YUV mode, stream_buffer is the valid source buffer. */
                            codec_buffer = stream_buffer;

    #ifdef HAL_CMAERA_BUFFER_COPY_MODE
                                if(result.output_buffers->data_size > bytes_read)
                                {
                                    memcpy(result.output_buffers->data, stream_buffer, bytes_read);
                                    result.output_buffers->data_size = bytes_read;
                                }
                                else
                                {
                                    memcpy(result.output_buffers->data, stream_buffer, result.output_buffers->data_size);
                                }
    #else
                                result.output_buffers->data = stream_buffer;
                                result.output_buffers->data_size = bytes_read;
    #endif
                            /* Encoder on this channel: every other frame, beside the encoder. */
                            if (sibling_venc &&
                                stream_info->request.request_template != CAMERA_TEMPLATE_PICTURE) {
                                if ((++preview_div & 1u) != 0u)
                                    continue;
                                if (yuv_side_post(channel_id, cdev->callback_ops, &result) == 0) {
                                    yuv_pending = stream_info;
                                    continue;
                                }
                            }
                        }
                        else
                        {
                            if (cdev->hal_camera == NULL) {
                                bytes_read = -1;
                                break;
                            }
                            int encoded_len = (int)hal_camera_codec_read(cdev->hal_camera, device_id, channel_id, stream_id, &codec_buffer, &codec_timestamp, bytes);
                            if (encoded_len <= 0) {
                                /* Empty read: retry the next frame. Do not finish a picture here. */
                                continue;
                            }
                            codec_buf_pinned = 1;

                            result.output_buffers->stream = channel_info->stream_info[stream_id].stream;//
                            result.timestamp = channel_timestamp;// Temporarily use channel timestamp, codec returns 0

    #ifdef HAL_CMAERA_BUFFER_COPY_MODE
                                if(result.output_buffers->data_size > (size_t)encoded_len)
                                {
                                    memcpy(result.output_buffers->data, codec_buffer, (size_t)encoded_len);
                                    result.output_buffers->data_size = (size_t)encoded_len;
                                }
                                else
                                {
                                    memcpy(result.output_buffers->data, codec_buffer, result.output_buffers->data_size);
                                }
    #else
                                result.output_buffers->data = codec_buffer;
                                result.output_buffers->data_size = (size_t)encoded_len;
    #endif
                                result.output_buffers->frame_type =
                                    hal_camera_codec_get_frame_type(cdev->hal_camera, device_id,
                                                                    channel_id, stream_id);
                            }
                            if (bytes_read <= 0) {
                            if (codec_buf_pinned)
                                hal_camera_codec_read_done(cdev->hal_camera, device_id, channel_id, stream_id);
                            continue;
                        }
                        if(cdev->callback_ops->process_capture_result != NULL)
                        {
                            cdev->callback_ops->process_capture_result(cdev->callback_ops, &result);
                        }

                        if(stream_info->request.request_template == CAMERA_TEMPLATE_PICTURE)
                        {
                            /* The callback already copied the JPEG. Drop the pin
                             * before disable, or disable waits on this same thread. */
                            if (codec_buf_pinned) {
                                hal_camera_codec_read_done(cdev->hal_camera, device_id,
                                                           channel_id, stream_id);
                                codec_buf_pinned = 0;
                            }
                            log_debug("picture capture done");
                            camera_picture_finish_from_capture_thread(dev, channel_info,
                                                                      device_id, channel_id,
                                                                      stream_info);
                        }
                        else
                        {
                            stream_info->request.frame_number++;
                        }
                        if (codec_buf_pinned)
                            hal_camera_codec_read_done(cdev->hal_camera, device_id, channel_id, stream_id);
                    } //if(snap_stream_status == CAMERA_STATUS_ENABLE)

                }//if(stream_info->request.request_template == CAMERA_TEMPLATE_AI)
            }

            if (yuv_pending != NULL) {
                yuv_side_wait(channel_id);
                yuv_pending->request.frame_number++;
            }

            //pthread_mutex_unlock(&cdev->lock);

        }  //if(bytes_read > 0)
        else
        {
            usleep(1000*100/fps);
            log_debug("capture thread sleep, device_id:%d, channel_id:%d", device_id, channel_id);
        }
    }
    log_debug("capture thread exit: dev=%d ch=%d tid=%lu reason=%d",
              device_id, channel_id, (unsigned long)pthread_self(), exit_reason);

    if (cdev != NULL) {
        pthread_mutex_lock(&cdev->channel_lock);
        if (pthread_equal(channel_info->thread_id, pthread_self())) {
            channel_info->thread_id = 0;
        }
        pthread_mutex_unlock(&cdev->channel_lock);
    }

    return NULL;
}

/**
 * @brief Mark channel stopped so capture thread will not re-enter get_frame.
 * @details Must be called before hal_camera_quiesce_channel. Not holding channel_lock on return.
 */
static pthread_t camera_mark_channel_stopped(struct camera_device *cdev, int device_id,
                                           int channel_id)
{
    channel_info_t *channel_info = &cdev->device_info[device_id].channel_info[channel_id];
    pthread_t tid = 0;

    pthread_mutex_lock(&cdev->channel_lock);
    channel_info->status = CAMERA_STATUS_DISABLE;
    tid = channel_info->thread_id;
    pthread_mutex_unlock(&cdev->channel_lock);

    return tid;
}

/**
 * @brief Join capture thread after quiesce unblocked get_frame.
 * @details Do not call from the capture thread (self-join) or while holding channel_lock.
 */
static int camera_join_capture_thread(struct camera_device *cdev, int device_id, int channel_id,
                                      pthread_t tid)
{
    channel_info_t *channel_info = &cdev->device_info[device_id].channel_info[channel_id];

    if (tid == 0) {
        return HARDWARE_CAMERA_OK;
    }
    if (pthread_equal(pthread_self(), tid)) {
        log_warn("camera_join_capture_thread: skip self-join dev=%d ch=%d", device_id, channel_id);
        return HARDWARE_CAMERA_OK;
    }

    log_info("capture stop: join thread dev=%d ch=%d tid=%lu",
             device_id, channel_id, (unsigned long)tid);
    pthread_join(tid, NULL);

    pthread_mutex_lock(&cdev->channel_lock);
    if (channel_info->thread_id == tid) {
        channel_info->thread_id = 0;
    }
    pthread_mutex_unlock(&cdev->channel_lock);

    return HARDWARE_CAMERA_OK;
}

/**
 * @brief Disable camera channel.
 * @param dev Camera hardware device pointer.
 * @param device_id Device ID.
 * @param channel_id Channel ID.
 * @return HARDWARE_CAMERA_OK on success.
 */
int camera_disable_channel(const camera_hw_device_t *dev, int device_id, int channel_id)
{
    struct camera_device *cdev = (struct camera_device *)dev;
    int ret = 0;
    channel_info_t *channel_info = &cdev->device_info[device_id].channel_info[channel_id];

    /* Check all streams in one channel, disable channel if all streams are closed */
    int channel_enable_flag = 0;
    pthread_mutex_lock(&cdev->channel_lock);

    for(int stream_id=0; stream_id<channel_info->num_stream; stream_id++)
    {
        if(channel_info->stream_info[stream_id].status == CAMERA_STATUS_ENABLE)
        {
            channel_enable_flag = 1;
            break;
        }
    }
    /*
     * If all streams are closed, close corresponding channel.
     * After closing channel, check if all channels are closed, if so disable device.
     */
    if(channel_enable_flag == 0)
    {
        int had_active_capture = (channel_info->thread_id != 0 ||
            (channel_info->status & CAMERA_STATUS_WORK_MASK) == CAMERA_STATUS_ENABLE ||
            (channel_info->status & CAMERA_STATUS_WORK_MASK) == CAMERA_STATUS_BUSY);

        pthread_mutex_unlock(&cdev->channel_lock);

        pthread_t capture_tid = camera_mark_channel_stopped(cdev, device_id, channel_id);

        if (capture_tid != 0) {
            if (cdev->hal_camera != NULL) {
                hal_camera_quiesce_channel(cdev->hal_camera, device_id, channel_id);
            }
            camera_join_capture_thread(cdev, device_id, channel_id, capture_tid);
        }

        if (had_active_capture && cdev->hal_camera != NULL) {
            hal_camera_disable_channel(cdev->hal_camera, device_id, channel_id);
        }

        pthread_mutex_lock(&cdev->channel_lock);

        if (channel_info->thread_id != 0) {
            log_warn("disable channel: capture thread still running (self-join skipped) dev=%d ch=%d",
                     device_id, channel_id);
        }

        log_debug("disable channel: dev=%d ch=%d", device_id, channel_id);

        int device_enable_flag = 0;
        for(int i=0; i<cdev->device_info[device_id].num_channel; i++)
        {
            if(cdev->device_info[device_id].channel_info[i].status == CAMERA_STATUS_ENABLE)
            {
                device_enable_flag = 1;
                break;
            }
        }

        if(device_enable_flag == 0)
        {
            /*
             * Do NOT disable the whole device here.
             *
             * hal_camera_disable_device() tears down VI/ISP/VPSS; re-enable via
             * hal_camera_enable_device() may fail (e.g. VPSS grp already running)
             * and breaks camera preview on next open (QR scan after camera app).
             */
            log_info("all channels disabled, keep device enabled: device_id=%d", device_id);
        }

    }

    pthread_mutex_unlock(&cdev->channel_lock);

    return HARDWARE_CAMERA_OK;
}


int camera_enable_channel(const camera_hw_device_t *dev, int device_id, int channel_id)
{
    struct camera_device *cdev = (struct camera_device *)dev;
    int ret = 0;
    channel_info_t *channel_info = &cdev->device_info[device_id].channel_info[channel_id];

    if(channel_info->status != CAMERA_STATUS_ENABLE)
    {
        log_debug("device_id:%d, channel_id:%d, device status:%d, channel status:%d",
            device_id, channel_id, cdev->device_info[device_id].status, channel_info->status);

        if(cdev->device_info[device_id].status == CAMERA_STATUS_UNINITIALIZE)
        {
            log_error("device[%d] uninitialized when enable, plz check the status",device_id);
            ret = hal_camera_create_device(cdev->hal_camera, device_id);
            if (ret != 0) {
                log_error("hal_camera_create_device failed: device_id=%d ret=%d", device_id, ret);
                return ret;
            }
            cdev->device_info[device_id].status = CAMERA_STATUS_CREATE;
        }

        /*
         * SetChnAttr must complete before AK_MPI_VPSS_StartGrp. If the device is
         * already running (RTSP/main on ch0), never call hal_camera_create_channel here.
         */
        if ((channel_info->status == CAMERA_STATUS_UNINITIALIZE ||
             !hal_camera_vpss_chn_is_configured(cdev->hal_camera, device_id, channel_id)) &&
            cdev->device_info[device_id].status != CAMERA_STATUS_ENABLE)
        {
            int ci;

            log_info("lazy create_channel: device_id=%d channel_id=%d status=%d vpss_cfg=%d",
                     device_id, channel_id, channel_info->status,
                     hal_camera_vpss_chn_is_configured(cdev->hal_camera, device_id, channel_id));
            for (ci = 0; ci < channel_id; ci++) {
                channel_info_t *lower = &cdev->device_info[device_id].channel_info[ci];

                if (lower->status != CAMERA_STATUS_UNINITIALIZE &&
                    hal_camera_vpss_chn_is_configured(cdev->hal_camera, device_id, ci)) {
                    continue;
                }
                log_info("lazy create prerequisite channel: device_id=%d channel_id=%d",
                         device_id, ci);
                ret = hal_camera_create_channel(cdev->hal_camera, device_id, ci);
                if (ret != 0) {
                    log_error("hal_camera_create_channel prereq failed: device_id=%d channel_id=%d ret=%d",
                              device_id, ci, ret);
                    return ret;
                }
                lower->status = CAMERA_STATUS_CREATE;
            }

            ret = hal_camera_create_channel(cdev->hal_camera, device_id, channel_id);
            if (ret != 0) {
                log_error("hal_camera_create_channel failed on recovery: device_id=%d channel_id=%d ret=%d",
                          device_id, channel_id, ret);
                return ret;
            }
            channel_info->status = CAMERA_STATUS_CREATE;
        }
        else if (!hal_camera_vpss_chn_is_configured(cdev->hal_camera, device_id, channel_id))
        {
            /*
             * Device is already running but this channel's attrs are gone. On camera
             * app re-entry the device is kept ENABLE (see camera_disable_channel) while
             * the channel was disabled on exit, so its attrs were dropped.
             *
             * On VI-based SoCs (AV100_V2) the channel can be re-registered live
             * (ak_vi_set_chn_attr_ex); hal_camera_enable_channel re-creates the attrs
             * when the channel is unregistered, so mark it CREATE and let enable below
             * recover it. On VPSS SoCs (AV200) SetChnAttr after StartGrp hangs, so keep
             * the hard failure.
             */
            if (hal_camera_supports_live_chn_reconfigure(cdev->hal_camera, device_id))
            {
                log_info("live reconfigure ch%d while device enabled (device_id=%d): "
                         "re-register attrs on enable", channel_id, device_id);
                channel_info->status = CAMERA_STATUS_CREATE;
            }
            else
            {
                log_error("VPSS ch%d attr missing after StartGrp (device_id=%d); "
                          "cannot SetChnAttr while group is running",
                          channel_id, device_id);
                return HARDWARE_CAMERA_ERROR_INVALID_STATE;
            }
        }

        if(cdev->device_info[device_id].status == CAMERA_STATUS_DISABLE || \
        cdev->device_info[device_id].status == CAMERA_STATUS_CREATE)
        {
            ret = hal_camera_enable_device(cdev->hal_camera, device_id);
            if (ret != 0) {
                log_error("hal_camera_enable_device failed: device_id=%d ret=%d", device_id, ret);
                return ret;
            }
            cdev->device_info[device_id].status = CAMERA_STATUS_ENABLE;
            log_info("enable device:%d",device_id);
        }

        if(channel_info->status == CAMERA_STATUS_DISABLE || channel_info->status == CAMERA_STATUS_CREATE)
        {
            ret = hal_camera_enable_channel(cdev->hal_camera, device_id, channel_id);
            if (ret != 0) {
                log_error("hal_camera_enable_channel failed: device_id=%d channel_id=%d ret=%d",
                          device_id, channel_id, ret);
                return ret;
            }
        }
        channel_info->status = CAMERA_STATUS_ENABLE;

        channel_info->dev = (camera_hw_device_t *)cdev;

        pthread_mutex_lock(&cdev->channel_lock);
        if (channel_info->thread_id != 0) {
            pthread_t stale = channel_info->thread_id;
            pthread_mutex_unlock(&cdev->channel_lock);
            if (!pthread_equal(pthread_self(), stale)) {
                log_warn("join stale capture thread before enable dev=%d ch=%d", device_id, channel_id);
                pthread_join(stale, NULL);
            }
            pthread_mutex_lock(&cdev->channel_lock);
            channel_info->thread_id = 0;
        }
        pthread_mutex_unlock(&cdev->channel_lock);

        log_info("camera_process_capture_thread_function pthread create, device_id:%d,channel_id:%d",device_id,channel_id);

        {
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            /* Default utask stack is 8KB; encode + frame copy overflows it. */
            pthread_attr_setstacksize(&attr, 64 * 1024);
            pthread_create(&channel_info->thread_id, &attr, camera_process_capture_thread_function, channel_info);
            pthread_attr_destroy(&attr);
        }
    }

    return HARDWARE_CAMERA_OK;
}


/**
 * @brief Process camera capture request.
 * @param dev Camera hardware device pointer.
 * @param request Pointer to the capture request.
 * @return 0 on success.
 */
int camera_process_capture_request(const camera_hw_device_t *dev, camera_capture_request_t *request)
{
    int ret = 0;
    int picture_open_ret = 0;
    struct camera_device *cdev = (struct camera_device *)dev;

/* Set new status */
    if (request->num_output_buffers <= 0 || request->output_buffers == NULL) {
        log_error("invalid request: num_output_buffers=%d", request->num_output_buffers);
        return HARDWARE_CAMERA_ERROR_INVALID_PARAM;
    }
    for(int i=0; i<request->num_output_buffers; i++)
    {
        camera_stream_t *stream = request->output_buffers[i].stream;
        if (stream == NULL) {
            log_warn("request output_buffers[%d].stream is NULL", i);
            continue;
        }
        if (stream->priv == NULL) {
            log_warn("stream priv is NULL (stream not configured?)");
            continue;
        }
        if (stream->num_channel <= 0 || stream->num_channel > CAMERA_MAX_NUM) {
            log_warn("stream num_channel=%d invalid", stream->num_channel);
            continue;
        }
        for(int j=0; j<stream->num_channel; j++)
        {
            stream_info_t *stream_info = (stream_info_t *)stream->priv[j];
            if (stream_info == NULL) {
                log_warn("stream->priv[%d] is NULL", j);
                continue;
            }

            stream_info->status |= CAMERA_STATUS_RESET;

            /* Deep-copy request under channel_lock so the capture thread's snapshot section
             * (which also holds channel_lock) sees a consistent view of request/owned_output_buffers.
             * cdev->lock is already held by the caller (cdev_process_capture_request); taking
             * channel_lock here is safe because the capture thread never holds cdev->lock,
             * so there is no lock-order inversion / deadlock risk. */
            pthread_mutex_lock(&cdev->channel_lock);
            stream_info_copy_request(stream_info, request);

            /* Batch request uses one template for all outputs; per-stream HAL template comes from
             * video_cfg (camera_stream_t.reserved[0] intptr_t = camera_request_template_t).
             *
             * IMPORTANT:
             * - If upper layer explicitly requests CAMERA_TEMPLATE_NONE (stop), we MUST NOT override it.
             *   Otherwise stop-all flows will be rewritten into VIDEO_RECORD and re-trigger codec/channel enable
             *   (VENC_Open noise) during teardown.
             */
            if (stream_info->request.request_template != CAMERA_TEMPLATE_NONE) {
                intptr_t per_tpl = (intptr_t)stream->reserved[0];
                if (per_tpl != 0) {
                    stream_info->request.request_template = (camera_request_template_t)per_tpl;
                }
            }
            pthread_mutex_unlock(&cdev->channel_lock);
        }
    }

    if (request->num_output_buffers > 0) {
        char summary[192];
        int pos = 0;
        summary[0] = '\0';
        for (int oi = 0; oi < (int)request->num_output_buffers && pos < (int)sizeof(summary) - 8; oi++) {
            camera_stream_t *st = request->output_buffers[oi].stream;
            char wh[24];
            cam_stream_wh(wh, sizeof(wh), st);
            pos += snprintf(summary + (size_t)pos, sizeof(summary) - (size_t)pos, "%s%s",
                             oi ? "," : "", wh);
        }
        log_debug("capture_request: tpl=%s outputs=[%s]", cam_request_tpl_str(request->request_template), summary);
    }

//change stream status, reset request template
    for(int device_id=0; device_id<cdev->num_device; device_id++)
    {
        for(int channel_id=0; channel_id<cdev->device_info[device_id].num_channel; channel_id++)
        {
            int channel_disable_flag = 0;
            int channel_enable_flag = 0;
            channel_info_t *channel_info = &cdev->device_info[device_id].channel_info[channel_id];
            for(int stream_id=0; stream_id<channel_info->num_stream; stream_id++)
            {
                stream_info_t *stream_info = &cdev->device_info[device_id].channel_info[channel_id].stream_info[stream_id];

                if((stream_info->status & CAMERA_STATUS_RESET) == CAMERA_STATUS_RESET)
                {
                    char wh[24];
                    cam_stream_wh(wh, sizeof(wh), stream_info->stream);
                    log_debug("request change: dev=%d ch=%d str=%d tpl=%s %s",
                             device_id, channel_id, stream_id,
                             cam_request_tpl_str(stream_info->request.request_template), wh);

                    stream_info->status &= ~CAMERA_STATUS_RESET;//remove reset bit
                    if(stream_info->status == CAMERA_STATUS_UNINITIALIZE)
                    {
                        int stream_open_ok = 1;
                        if (stream_info->request.request_template == CAMERA_TEMPLATE_NONE) {
                            /* Stop request: do not enable codec/channel on an uninitialized stream. */
                            cam_stream_close_codec_if_open(cdev, stream_info);
                            stream_info->status = CAMERA_STATUS_DISABLE;
                            stream_info->request_template = CAMERA_TEMPLATE_NONE;
                        }
                        else if (cam_stream_should_enable_codec(stream_info->stream,
                                                              stream_info->request.request_template))
                        {
                            int eret = cam_stream_open_codec(cdev, stream_info,
                                                             device_id, channel_id, stream_id);
                            if (eret != 0) {
                                picture_open_ret = eret;
                                stream_open_ok = 0;
                            }
                        }
                        if (stream_open_ok &&
                            stream_info->request.request_template != CAMERA_TEMPLATE_NONE) {
                            stream_info->status = CAMERA_STATUS_ENABLE;
                            stream_info->request_template = stream_info->request.request_template;
                        }
                    }
                    else if(stream_info->status == CAMERA_STATUS_DISABLE || \
                           stream_info->status == CAMERA_STATUS_CREATE )
                    {
                        int stream_open_ok = 1;
                        if (stream_info->request.request_template == CAMERA_TEMPLATE_NONE) {
                            /* Stop request: keep disabled. Do not enable codec/channel. */
                            cam_stream_close_codec_if_open(cdev, stream_info);
                            stream_info->status = CAMERA_STATUS_DISABLE;
                            stream_info->request_template = CAMERA_TEMPLATE_NONE;
                        }
                        else if (cam_stream_should_enable_codec(stream_info->stream,
                                                              stream_info->request.request_template))
                        {
                            /* Open only when close-on-disable cleared the handle. */
                            if (stream_info->codec_id < 0) {
                                int eret = cam_stream_open_codec(cdev, stream_info,
                                                                 device_id, channel_id, stream_id);
                                if (eret != 0) {
                                    picture_open_ret = eret;
                                    stream_open_ok = 0;
                                }
                            } else {
                                log_info("request enable: reuse venc handle=%d dev=%d ch=%d str=%d",
                                         stream_info->codec_id, device_id, channel_id, stream_id);
                            }
                        }
                        if (stream_open_ok &&
                            stream_info->request.request_template != CAMERA_TEMPLATE_NONE) {
                            stream_info->status = CAMERA_STATUS_ENABLE;
                            stream_info->request_template = stream_info->request.request_template;
                        }
                    }
                    else if(stream_info->status == CAMERA_STATUS_ENABLE)
                    {
                        if(stream_info->request.request_template == CAMERA_TEMPLATE_NONE)
                        {
                            char wh2[24];
                            cam_stream_wh(wh2, sizeof(wh2), stream_info->stream);
                            log_info("request disable: dev=%d ch=%d str=%d %s",
                                     device_id, channel_id, stream_id, wh2);
                            /* Drop VENC even if siblings keep the VI channel alive. */
                            cam_stream_close_codec_if_open(cdev, stream_info);
                            stream_info->status = CAMERA_STATUS_DISABLE;
                            stream_info->request_template = CAMERA_TEMPLATE_NONE;
                        }
                        else if (!cam_stream_should_enable_codec(stream_info->stream,
                                                                stream_info->request.request_template))
                        {
                            /* VI keepalive: stream stays ENABLE, release VENC DMA. */
                            cam_stream_close_codec_if_open(cdev, stream_info);
                        }
                        else if (stream_info->codec_id < 0)
                        {
                            /* ENABLE with no handle: open VENC. Picture failure drops back to DISABLE. */
                            int eret = cam_stream_open_codec(cdev, stream_info,
                                                             device_id, channel_id, stream_id);
                            if (eret != 0) {
                                picture_open_ret = eret;
                                stream_info->status = CAMERA_STATUS_DISABLE;
                            }
                        }
                    }

                }
                else
                {
                    /* Stream not in this request: leave the batch. */
                    if((stream_info->status == CAMERA_STATUS_ENABLE) && (stream_info->request_template == request->request_template))
                    {
                        log_info("request drop stream: dev=%d ch=%d str=%d (sibling channel may stay up)",
                                 device_id, channel_id, stream_id);
                        cam_stream_close_codec_if_open(cdev, stream_info);
                        stream_info->status = CAMERA_STATUS_DISABLE;
                        stream_info->request_template = CAMERA_TEMPLATE_NONE;
                    }
                }
            }

            /* Enable the channel only when a stream is active and the channel is
             * not already working; otherwise disable it. */
            {
                int any_stream_enable = 0;
                for (int sid = 0; sid < channel_info->num_stream; sid++) {
                    if (channel_info->stream_info[sid].status == CAMERA_STATUS_ENABLE)
                        any_stream_enable = 1;
                }
                if (any_stream_enable) {
                    int ch_work = channel_info->status & CAMERA_STATUS_WORK_MASK;
                    if (ch_work != CAMERA_STATUS_ENABLE && ch_work != CAMERA_STATUS_BUSY) {
                        channel_enable_flag = 1;
                    }
                } else {
                    channel_disable_flag = 1;
                }
            }

            if(channel_enable_flag == 1)
            {
                log_debug("camera channel enable by request");
                ret = camera_enable_channel(dev, cdev->device_info[device_id].device_id,channel_id);
                if (ret != 0) {
                    log_error("camera_enable_channel failed: device_id=%d channel_id=%d ret=%d",
                              cdev->device_info[device_id].device_id, channel_id, ret);
                    return ret;
                }
            }
            else
            {
                if(channel_disable_flag == 1)
                {
                    camera_disable_channel(dev, cdev->device_info[device_id].device_id,
                                           channel_id);
                }
            }
        }
    }
    if (picture_open_ret != 0)
        return picture_open_ret;
    return HARDWARE_CAMERA_OK;
}

static int cdev_process_capture_request(const camera_hw_device_t *dev,
        camera_capture_request_t *request)
{
    struct camera_device *cdev = (struct camera_device *)dev;
    size_t bytes_read;

    log_debug("cdev_process_capture_request tpl=%s num_out=%u",
             cam_request_tpl_str(request->request_template), (unsigned int)request->num_output_buffers);
    log_info("[CAM_TRACE] process_capture_request: tpl=%s num_out=%u (tid=%lu)",
             cam_request_tpl_str(request->request_template),
             (unsigned int)request->num_output_buffers, (unsigned long)pthread_self());

    if(!dev || !request)
    {
        return HARDWARE_CAMERA_ERROR_INVALID_PARAM;
    }
    if (__sync_add_and_fetch(&cdev->closing, 0) != 0) {
        log_warn("cdev_process_capture_request dropped(closing): tid=%lu request=%p tpl=%d",
                 (unsigned long)pthread_self(), (void *)request, request->request_template);
        return HARDWARE_CAMERA_OK;
    }

    pthread_mutex_lock(&cdev->lock);
    if (__sync_add_and_fetch(&cdev->closing, 0) != 0) {
        log_warn("cdev_process_capture_request dropped-after-lock(closing): tid=%lu request=%p tpl=%d",
                 (unsigned long)pthread_self(), (void *)request, request->request_template);
        pthread_mutex_unlock(&cdev->lock);
        return HARDWARE_CAMERA_OK;
    }
    int ret = camera_process_capture_request(dev, request);
    pthread_mutex_unlock(&cdev->lock);

    return ret;
}

/**
 * Configures the camera streams
 * @param hal_camera Pointer to the camera data
 * @return 0 on success, -1 on failure
 */
static int camera_streams_reconfigure_for_ai(const camera_hw_device_t *dev,
        camera_stream_t *stream, bool ai_tile_overlap_enable)
{
    struct camera_device *cdev = (struct camera_device *)dev;
    camera_stream_configuration_t stream_config;
    stream_config.streams = (camera_stream_t **)malloc(sizeof(camera_stream_t));
    camera_stream_t *ai_stream =  stream;
    int priv_id = ai_stream->priv_id;
    int ret = 0;
    stream_info_t *stream_info = ai_stream->priv[priv_id];
    stream_config.num_streams = 1;
    stream_config.streams[0] = ai_stream;

    log_info("ai_tile_overlap->enable: %d", ai_tile_overlap_enable);

    if(ai_tile_overlap_enable == true)
    {
        int tile_width = ai_stream->channel_config[0].width;
        int tile_height = ai_stream->channel_config[0].height;
        int input_width;
        int input_height;

        stream_info->ai_tile_overlap_enable = true;

//        stream_info->ai_tile_overlap_num_cols = ai_tile_overlap->num_cols;
//        stream_info->ai_tile_overlap_num_rows = ai_tile_overlap->num_rows;
//        stream_info->ai_tile_overlap_ratio = ai_tile_overlap->overlap_ratio;

        hal_camera_ai_tile_overlap_calculate_input_size(
            tile_width, tile_height, \
            stream_info->ai_tile_overlap.num_cols, stream_info->ai_tile_overlap.num_rows, \
            stream_info->ai_tile_overlap.overlap_ratio,\
            &input_width, &input_height);

        //ai_stream->channel_config[0].width = input_width;
        //ai_stream->channel_config[0].height = input_height;
        stream_info->ai_stream_tile_width = input_width;
        stream_info->ai_stream_tile_height = input_height;

        log_info("input size: [%d,%d], video_stream.num_channel:%d", \
            input_width, input_height, ai_stream->num_channel);

#if 0 // sunzhiqiang 20250901 should not reconfigure, overlap from main channel
        ret = camera_streams_reconfigure_for_stream_change(dev, &stream_config);

        if (ret != 0) {
            log_error("Failed to configure camera streams: %d", ret);
            return ret;
        }
#endif
    }
    else
    {
        stream_info->ai_tile_overlap_enable = false;
#if 0 // sunzhiqiang 20250901 should not reconfigure, overlap from main channel
        int source_width = stream_info->ai_stream_tile_width;
        int source_height = stream_info->ai_stream_tile_height;

        ai_stream->channel_config[0].width = source_width;
        ai_stream->channel_config[0].height = source_height;
        ret = camera_streams_reconfigure_for_stream_change(dev, &stream_config);
        if (ret != 0) {
            log_error("Failed to configure camera streams: %d", ret);
            return ret;
        }
#endif
    }

    free(stream_config.streams);
    stream_config.streams = NULL;
//    pthread_mutex_unlock(&cdev->lock);

    return ret;
}


static int cdev_set_parameters(const struct camera_hw_device *dev,  const char* name, void *value, unsigned int len)
{
    if (dev == NULL || name == NULL) {
        return HARDWARE_CAMERA_ERROR_INVALID_PARAM;
    }
    int ret = HARDWARE_CAMERA_OK;
    struct camera_device *cdev = (struct camera_device *)dev;

    pthread_mutex_lock(&cdev->lock);

    if(strcmp("AiConfig",name)== 0)
    {
        camera_ai_input_t *new_ai_input = value;

        // Currently multiple cameras in one stream, each camera registers and returns
        for(int channel_id=0; channel_id<new_ai_input->stream->num_channel; channel_id++)
        {
            stream_info_t *stream_info = (stream_info_t *)new_ai_input->stream->priv[channel_id];

            int camera_ai_registered_flag = 0;
            for(int i=0; i<cdev->num_camera_ai; i++)
            {
                if(cdev->ai_input[i].ai_type == new_ai_input->ai_type)
                {
                    camera_ai_registered_flag = 1;
                    memcpy(&cdev->ai_input[i], new_ai_input, sizeof(camera_ai_input_t));
                }
            }

            if(camera_ai_registered_flag == 0)
            {
                if(cdev->num_camera_ai >= CAMERA_AI_MAX)
                {
                    log_error("ai regesit fail! register too much! the max ai num:%d",CAMERA_AI_MAX);
                    pthread_mutex_unlock(&cdev->lock);
                    return HARDWARE_CAMERA_ERROR_AI_NOT_AVAILABLE;
                }
                memcpy(&cdev->ai_input[cdev->num_camera_ai], new_ai_input, sizeof(camera_ai_input_t));
                log_info("ai config [%d][%d][%d],ai_type:%x",stream_info->device_id,stream_info->channel_id,stream_info->stream_id,new_ai_input->ai_type);
                cdev->num_camera_ai++;
            }
        }
    }
    else if(strcmp("AiTileOverlap",name)== 0)
    {
        camera_ai_tile_overlap_t *ai_tile_overlap = value;
        int priv_id = ai_tile_overlap->stream->priv_id;

//        stream_info_t *stream_info = ai_tile_overlap->stream->priv[priv_id];
        // Currently multiple cameras in one stream, each camera registers and returns
        for(int channel_id=0; channel_id<ai_tile_overlap->stream->num_channel; channel_id++)
        {
            stream_info_t *stream_info = (stream_info_t *)ai_tile_overlap->stream->priv[channel_id];
            memcpy(&stream_info->ai_tile_overlap, ai_tile_overlap, sizeof(camera_ai_tile_overlap_t));
        }
    }
    else if (strcmp("ForceIFrame", name) == 0)
    {
        /* value: camera_stream_t * (framework stream); request IDR on its encoder(s). */
        camera_stream_t *stream = (camera_stream_t *)value;
        int ok = 0;

        (void)len;
        if (stream == NULL || stream->priv == NULL || stream->num_channel <= 0) {
            pthread_mutex_unlock(&cdev->lock);
            return HARDWARE_CAMERA_ERROR_INVALID_PARAM;
        }
        for (int ch = 0; ch < stream->num_channel; ch++) {
            stream_info_t *si = (stream_info_t *)stream->priv[ch];
            if (si == NULL || si->codec_id < 0)
                continue;
            if (cdev->hal_camera != NULL &&
                hal_camera_codec_request_idr(cdev->hal_camera, si->device_id,
                                             si->channel_id, si->stream_id) == 0)
                ok = 1;
        }
        pthread_mutex_unlock(&cdev->lock);
        return ok ? HARDWARE_CAMERA_OK : HARDWARE_CAMERA_ERROR_OPERATION_FAILED;
    }
    else
    {
        log_error( " set (%s) failed , not support !", name);
        pthread_mutex_unlock(&cdev->lock);
        return HARDWARE_CAMERA_ERROR_NOT_SUPPORTED;
    }

    pthread_mutex_unlock(&cdev->lock);

    return ret;
}

/*add ai interface
 * Current solution has a drawback: in multi-purpose scenarios without splicing, opening one algorithm simultaneously will have issues---need to consider improvements later
*/
static int cdev_add_camera_ai(const struct camera_hw_device *dev, camera_ai_type_t type)
{
    struct camera_device *cdev = (struct camera_device *)dev;
    int ret;
    pthread_mutex_lock(&cdev->lock);
    ret = hal_camera_ai_add(cdev->hal_camera, type);
    pthread_mutex_unlock(&cdev->lock);

    return ret;
}

static int cdev_remove_camera_ai(const struct camera_hw_device *dev,  camera_ai_type_t type)
{
    struct camera_device *cdev = (struct camera_device *)dev;

    pthread_mutex_lock(&cdev->lock);
    hal_camera_ai_remove(cdev->hal_camera, type);
    pthread_mutex_unlock(&cdev->lock);

    return HARDWARE_CAMERA_OK;
}


static int cdev_enable_camera_ai_tile_overlap(const struct camera_hw_device *dev, camera_stream_t *stream)
{
    struct camera_device *cdev = (struct camera_device *)dev;

    int priv_id = stream->priv_id;
    stream_info_t *stream_info = stream->priv[priv_id];
    log_info("cdev_enable_camera_ai_tile_overlap: %d", stream_info->ai_tile_overlap_enable?1:0);
    pthread_mutex_lock(&cdev->lock);
    if(!stream_info->ai_tile_overlap_enable)
    {
        camera_streams_reconfigure_for_ai(dev, stream, true);
        hal_camera_ai_tile_overlap_enable(cdev->hal_camera);
    }

    pthread_mutex_unlock(&cdev->lock);

    return HARDWARE_CAMERA_OK;
}

static int cdev_disable_camera_ai_tile_overlap(const struct camera_hw_device *dev,  camera_stream_t *stream)
{
    struct camera_device *cdev = (struct camera_device *)dev;

    int priv_id = stream->priv_id;

    stream_info_t *stream_info = stream->priv[priv_id];
    log_info("cdev_disable_camera_ai_tile_overlap: %d", stream_info->ai_tile_overlap_enable?1:0);
    pthread_mutex_lock(&cdev->lock);
    if(stream_info->ai_tile_overlap_enable)
    {
        camera_streams_reconfigure_for_ai(dev, stream, false);
        hal_camera_ai_tile_overlap_disable(cdev->hal_camera);
    }

    pthread_mutex_unlock(&cdev->lock);

    return HARDWARE_CAMERA_OK;
}

static int cdev_af_control(const struct camera_hw_device *dev, int camera_id,
                           int cmd, void *arg)
{
    (void)dev;
    return hal_camera_af_control(camera_id, cmd, arg);
}

static void cdev_dump(const camera_hw_device_t *dev, int fd)
{
    struct camera_device *cdev = (struct camera_device *)dev;

    pthread_mutex_lock(&cdev->lock);
//    hal_cameara_dump(cdev->hal_camera, int fd);
    pthread_mutex_unlock(&cdev->lock);

}

static int cdev_flush(const camera_hw_device_t *dev)
{

    struct camera_device *cdev = (struct camera_device *)dev;

    pthread_mutex_lock(&cdev->lock);
//    hal_cameara_flush(cdev->hal_camera);
    pthread_mutex_unlock(&cdev->lock);

    return HARDWARE_CAMERA_OK;
}


static void cdev_signal_stream_flush(const camera_hw_device_t *dev,
            uint32_t num_streams,
            const camera_stream_t* const* streams)
{

    struct camera_device *cdev = (struct camera_device *)dev;

    pthread_mutex_lock(&cdev->lock);
//    hal_cameara_signal_stream_flush(cdev->hal_camera);
    pthread_mutex_unlock(&cdev->lock);

}

static int cdev_is_reconfiguration_required(const camera_hw_device_t *dev,
            const camera_metadata_t* old_session_params,
            const camera_metadata_t* new_session_params)
{
    struct camera_device *cdev = (struct camera_device *)dev;

    pthread_mutex_lock(&cdev->lock);

    pthread_mutex_unlock(&cdev->lock);


    return HARDWARE_CAMERA_OK;
}


static int cdev_open(const hw_module_t* module, const char* name,
                     hw_device_t** device)
{
    log_debug("cdev_open: %s", name);

    struct camera_device *cdev;

    if (strcmp(name, CAMERA_HARDWARE_INTERFACE) != 0)
        return HARDWARE_CAMERA_ERROR_INVALID_PARAM;

    cdev = calloc(1, sizeof(struct camera_device));
    if (!cdev)
        return HARDWARE_CAMERA_ERROR_MEMORY;

    pthread_mutex_init(&cdev->lock, (const pthread_mutexattr_t *) NULL);
    pthread_mutex_init(&cdev->channel_lock, (const pthread_mutexattr_t *) NULL);

    log_debug("cdev->hal_camera: %x", &cdev->hal_camera);

    pthread_mutex_lock(&cdev->lock);
    cdev->hal_camera = hal_camera_init(cdev);
    pthread_mutex_unlock(&cdev->lock);

    if(!cdev->hal_camera) {
        free(cdev);
        return HARDWARE_CAMERA_ERROR_MEMORY;
    }

    cdev->device.common.tag = HARDWARE_DEVICE_TAG;
    cdev->device.common.version = CAMERA_DEVICE_API_VERSION_1_0;
    cdev->device.common.module = (struct hw_module_t *) module;
    cdev->device.common.close = cdev_close;

    cdev->device.set_callback_ops = cdev_set_callback_ops;
    cdev->device.configure_streams = cdev_configure_streams;
    cdev->device.reconfigure_streams = cdev_reconfigure_streams;

    cdev->device.construct_default_request_settings = cdev_construct_default_request_settings;
    cdev->device.process_capture_request = cdev_process_capture_request;
    cdev->device.dump = cdev_dump;
    cdev->device.flush = cdev_flush;

    cdev->device.add_camera_ai = cdev_add_camera_ai;
    cdev->device.remove_camera_ai = cdev_remove_camera_ai;

    cdev->device.enable_camera_ai_tile_overlap = cdev_enable_camera_ai_tile_overlap;
    cdev->device.disable_camera_ai_tile_overlap = cdev_disable_camera_ai_tile_overlap;

//    cdev->device.signal_stream_flush = cdev_signal_stream_flush;
//    cdev->device.is_reconfiguration_required = cdev_is_reconfiguration_required;
    cdev->device.set_parameters = cdev_set_parameters;

    cdev->device.af_control = cdev_af_control;

//    cdev->device.reserved = { 0 };

//&cdev->device.common and &cdev->device are same address
    *device = &cdev->device.common;
    cdev->num_device = 0;
    cdev->num_camera_ai = 0;
    cdev->closing = 0;

    cdev->ai_output_result.ai_object =  malloc(CAMERA_AI_MAX_BOX * sizeof(camera_ai_object_t));

    return HARDWARE_CAMERA_OK;
}




/*===========================================================================
 * FUNCTION   : get_number_of_cameras
 *
 * DESCRIPTION: static function to query number of cameras detected
 *
 * PARAMETERS : none
 *
 * RETURN     : number of cameras detected
 *==========================================================================*/
int get_number_of_cameras()
{
    return hal_camera_get_number();
}




/*===========================================================================
 * FUNCTION   : get_camera_info
 *
 * DESCRIPTION: static function to query camera information with its ID
 *
 * PARAMETERS :
 *   @camera_id : camera ID
 *   @info      : ptr to camera info struct
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int get_camera_info(int camera_id, struct camera_info *info)
{
    int rc = NO_ERROR;

    info->device_version = CAMERA_DEVICE_API_VERSION_1_0;

    rc =  hal_camera_get_info(camera_id, info);

    return rc;
}


/*===========================================================================
 * FUNCTION   : set_camera_info
 *
 * DESCRIPTION: static function to query camera information with its ID
 *
 * PARAMETERS :
 *   @camera_id : camera ID
 *   @info      : ptr to camera info struct
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int set_camera_info(int camera_id, struct camera_info *info)
{
    int rc = NO_ERROR;

//    rc =  hal_camera_set_group_info(group_id, group_info);

    return rc;
}

/*===========================================================================
 * FUNCTION   : set_torch_mode
 *
 * DESCRIPTION: Attempt to turn on or off the torch mode of the flash unit.
 *
 * PARAMETERS :
 *   @camera_id : camera ID
 *   @on        : Indicates whether to turn the flash on or off
 *
 * RETURN     : 0  -- success
 *              none-zero failure code
 *==========================================================================*/
int set_torch_mode(const char* camera_id, bool on)
{
    hal_camera_set_torch_mode(camera_id, on);
    return HARDWARE_CAMERA_OK;
}


/*===========================================================================
 * FUNCTION   : set_callbacks
 *
 * DESCRIPTION: static function to set callbacks function to camera module
 *
 * PARAMETERS :
 *   @callbacks : ptr to callback functions
 *
 * RETURN     : NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int set_callbacks(const camera_module_callbacks_t *callbacks)
{
    int rc = NO_ERROR;


    return rc;
}

/*===========================================================================
 * FUNCTION   : isStreamCombinationSupported
 *
 * DESCRIPTION: method to query camera combination support
 *
 * PARAMETERS :
 *   @camera_id : camera ID
 *   @streams   : stream combination
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- in case combination is supported
 *              none-zero failure code
 *==========================================================================*/
#if 0
int is_stream_combination_supported(int camera_id,
        const camera_stream_combination_t *streams)
{
    if (!mNumOfCameras || camera_id >= mNumOfCameras || !streams ||
        (camera_id < 0)) {
        LOGE("Error during stream combination query!! mNumOfCameras = %d,"
                "camera_id = %d, info = %p",
                 mNumOfCameras, camera_id, streams);
        return BAD_VALUE;
    }

    return isStreamCombinationSupported(camera_id, streams);
}

#endif





static struct hw_module_methods_t hal_module_methods = {
    .open = cdev_open,
};

static struct camera_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag                    = HARDWARE_MODULE_TAG,
        .module_api_version     = CAMERA_MODULE_API_VERSION_1_0,
        .hal_api_version        = HARDWARE_HAL_API_VERSION,
        .id                     = CAMERA_HARDWARE_MODULE_ID,
        .name                   = "Celestia camera HW HAL",
        .author                 = "AnyKa Celestia",
        .methods                = &hal_module_methods,
        .dso                    = NULL,
        .reserved               = {0}
    },

    .get_number_of_cameras  = get_number_of_cameras,
    .get_camera_info        = get_camera_info,
    .set_camera_info        = set_camera_info,

    .set_callbacks          = set_callbacks,
//    .get_vendor_tag_ops     = get_vendor_tag_ops,
    .set_torch_mode         = set_torch_mode,
//    .set_parameters,
    .reserved               = {0}
};



int hw_camera_module_init(void)
{
    return hw_register_module(&(HAL_MODULE_INFO_SYM.common));
}
