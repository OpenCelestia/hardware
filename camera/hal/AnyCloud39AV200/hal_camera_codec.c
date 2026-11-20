// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2024 Anyka(GuangZhou) Microelectronics Co., Ltd.
 * @DATE     2024-09
 * @VERSION  1.0
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <sys/stat.h>



#include "ak_common.h"
#include "ak_log.h"
#include "ak_common_video.h"
#include "ak_venc.h"
#include "ak_thread.h"
#include "ak_mem.h"
#include "ak_vi.h"
#include "ak_vpss.h"

#include "ak_svp.h"
#include "ak_npu.h"
#include "ak_osd.h"

#include "camera.h"
#include "log.h"

#include "hal_camera.h"
//#include "hal_camera_chip_config.h"

#include "../../camera_hw.h"
#include "../../hal_camera_api.h"

#define LOG_TAG "HAL CAMERA CODEC"

/* Dedicated VENC slot for MJPEG alarm snapshots — never allocate via register_id. */
#define MJPEG_SNAPSHOT_CODEC_ID  (CAMERA_CODEC_MAX_NUM - 1)

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

/* Set while the capture thread still uses the encoded buffer after codec_read unlocks. */
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

int hal_camera_codec_init( void *hal_camera )
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;

    for(int i=0; i<CAMERA_CODEC_MAX_NUM; i++)
    {
        my_data->codec_id_status[i] = 0;
    }
    for (int d = 0; d < CAMERA_MAX_NUM; d++) {
        for (int c = 0; c < CAMERA_MAX_CHANNEL; c++) {
            for (int s = 0; s < CAMERA_MAX_STREAM_PER_CHANNEL; s++) {
                s_codec_io_busy[d][c][s] = 0;
            }
        }
    }
    return 0;
}

int camera_codec_register_id( void *hal_camera)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;

    for (int i = 0; i < MJPEG_SNAPSHOT_CODEC_ID; i++) {
        if (my_data->codec_id_status[i] == 0) {
            my_data->codec_id_status[i] = 1;
            return i;
        }
    }
    return -1;
}

int camera_codec_unregister_id( void *hal_camera, int codec_id)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;

    my_data->codec_id_status[codec_id] = 0;

    return 0;
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
    int ret;
    log_debug("hal_camera_codec_enable");

    if(stream->codec_config.codec_mode == CAMERA_CODEC_MODE_YUV)
    {
        log_info("CAMERA_CODEC_MODE_YUV don't need enable codec");
        return -1;
    }

    /* Save codec ID to stream info, internally managed */
    for(int i=0 ;i<stream->num_channel; i++)
    {
        stream_info_t *stream_info = stream->priv[i];

        VENC_CHN_ATTRS_S stChnAttrs = {0};

        VENC_ATTR_S *pstVencAttrs       = &stChnAttrs.stVencAttrs;   //venc attr
        VENC_RC_ATTRS_S *pstRcAttrs     = &stChnAttrs.stRcAttrs;     //rc attr
        VENC_EXT_ATTRS_S *pstExtAttrs   = &stChnAttrs.stExtAttrs;    //ext attr

        int device_id = stream_info->device_id;
        int channel_id = stream_info->channel_id;
        int stream_id = stream_info->stream_id;
        int is_mjpeg = (stream->codec_config.codec_mode == CAMERA_CODEC_MODE_MJPEG);

        if (is_mjpeg) {
            /* Reuse one MJPEG encoder — avoid VENC_Open id leak (kernel panic at high open count). */
            stream_info->codec_id = MJPEG_SNAPSHOT_CODEC_ID;
            if (my_data->codec_id_status[MJPEG_SNAPSHOT_CODEC_ID] != 0) {
                hal_camera_codec_t *codec_info =
                    &my_data->hal_camera_device[device_id].channel[channel_id].codec[stream_id];
                int old_id = MJPEG_SNAPSHOT_CODEC_ID;
                if (codec_info->pstStream != NULL && codec_info->stream_buffer_release_flag) {
                    (void)AK_MPI_VENC_ReleaseStream(old_id, codec_info->pstStream);
                    codec_info->stream_buffer_release_flag = false;
                }
                (void)AK_MPI_VENC_Close(old_id);
                my_data->codec_id_status[MJPEG_SNAPSHOT_CODEC_ID] = 0;
                log_info("mjpeg snapshot: recycle codec id:%d stream:[%d][%d][%d]",
                         old_id, device_id, channel_id, stream_id);
            }
            my_data->codec_id_status[MJPEG_SNAPSHOT_CODEC_ID] = 1;
        } else {
            stream_info->codec_id = camera_codec_register_id(my_data);
            if (stream_info->codec_id < 0) {
                log_error("no free codec id for stream:[%d][%d][%d]",
                          device_id, channel_id, stream_id);
                return -1;
            }
        }

        log_info("register codec id:%d for stream:[%d][%d][%d]",
                 stream_info->codec_id, device_id, channel_id, stream_id);


        my_data->hal_camera_device[device_id].channel[channel_id].codec[stream_id].pstStream =
            malloc(sizeof(VIDEO_STREAM_S));

        pstVencAttrs->u16Width = get_splice_stream_width(stream);
        pstVencAttrs->u16Height = get_splice_stream_height(stream);

        camera_codec_config_t codec = stream->codec_config;

        camera_rc_config_t rc = stream->rc_config;
        camera_gop_config_t gop = stream->gop_config;

        /* decode type is h264*/
        if(codec.codec_mode == CAMERA_CODEC_MODE_AVC)
        {
            log_debug("CAMERA_CODEC_MODE_AVC");

            pstVencAttrs->enOutputType = H264_ENC_TYPE;
            pstVencAttrs->u32MaxBufSize = 0;         /* Ext buf size (KBytes) */
            pstVencAttrs->u32MaxDMASize = 0;         /* Inner buf size (KBytes) */
            pstVencAttrs->enChromaMode = VENC_CHROMA_YUV420;
            pstVencAttrs->u32Priority = 0;

            pstVencAttrs->stH264Attr.enGdrMode = VENC_GDR_OFF;

            switch(codec.profile)
            {
                case AVC_PROFILE_BASE:
                    pstVencAttrs->stH264Attr.enProfileMode = VENC_PROFILE_BASE;
                break;
                case AVC_PROFILE_MAIN:
                    pstVencAttrs->stH264Attr.enProfileMode = VENC_PROFILE_MAIN;
                break;
                case AVC_PROFILE_HIGH:
                    pstVencAttrs->stH264Attr.enProfileMode = VENC_PROFILE_HIGH;
                break;
                default:
            }

            pstVencAttrs->stH264Attr.enEncLevel = codec.level;

            pstVencAttrs->stH264Attr.u16Framerate = stream->fps;
            pstVencAttrs->stH264Attr.u16GopLen = gop.gop_len;
            if(rc.avc.rc_mode == CAMERA_CODEC_RC_MODE_VBR)
            {
                pstRcAttrs->enRcMode = VENC_RC_MODE_H264_AVBR;
                pstRcAttrs->stH264Avbr.u16MinQP = rc.avc.vbr.min_qp;
                pstRcAttrs->stH264Avbr.u16MaxQP = rc.avc.vbr.max_qp;
                pstRcAttrs->stH264Avbr.u16MaxBitrate = rc.avc.vbr.max_bitrate;
                pstRcAttrs->stH264Avbr.u16ChangePos = rc.avc.vbr.change_pos;
            }
            else if(rc.avc.rc_mode == CAMERA_CODEC_RC_MODE_CBR)
            {
                pstRcAttrs->enRcMode = VENC_RC_MODE_H264_CBR;
                pstRcAttrs->stH264Cbr.u16TargetBitrate = rc.avc.cbr.target_bitrate;
            }

        }
        else if(codec.codec_mode == CAMERA_CODEC_MODE_HEVC)
        {
            log_debug("CAMERA_CODEC_MODE_HEVC");

            pstVencAttrs->enOutputType = HEVC_ENC_TYPE;
            pstVencAttrs->u32MaxBufSize = 0;
            pstVencAttrs->u32MaxDMASize = 0;
            pstVencAttrs->enChromaMode = VENC_CHROMA_YUV420;
            pstVencAttrs->u32Priority = 0;

            pstVencAttrs->stH265Attr.enGdrMode = VENC_GDR_OFF;

            switch(codec.profile)
            {
                case HEVC_PROFILE_MAIN_INTRA:
                    pstVencAttrs->stH265Attr.enProfileMode = VENC_PROFILE_HEVC_MAIN_INTRA;
                break;
                case HEVC_PROFILE_MAIN:
                    pstVencAttrs->stH265Attr.enProfileMode = VENC_PROFILE_HEVC_MAIN;
                break;
                case HEVC_PROFILE_MAIN_STILL:
                    pstVencAttrs->stH265Attr.enProfileMode = VENC_PROFILE_HEVC_MAIN_STILL;
                break;
                default:
            }

            pstVencAttrs->stH265Attr.enEncLevel = codec.level;

            pstVencAttrs->stH265Attr.u16Framerate = stream->fps;
            pstVencAttrs->stH265Attr.u16GopLen = gop.gop_len;

            if(rc.hevc.rc_mode == CAMERA_CODEC_RC_MODE_VBR)
            {
                pstRcAttrs->enRcMode = VENC_RC_MODE_H265_AVBR;
                pstRcAttrs->stH265Avbr.u16MinQP = rc.hevc.vbr.min_qp;
                pstRcAttrs->stH265Avbr.u16MaxQP = rc.hevc.vbr.max_qp;
                pstRcAttrs->stH265Avbr.u16MaxBitrate = rc.hevc.vbr.max_bitrate;
                pstRcAttrs->stH265Avbr.u16ChangePos = rc.hevc.vbr.change_pos;
            }
            else if(rc.hevc.rc_mode == CAMERA_CODEC_RC_MODE_CBR)
            {
                pstRcAttrs->enRcMode = CAMERA_CODEC_RC_MODE_CBR;
                pstRcAttrs->stH265Cbr.u16TargetBitrate = rc.hevc.cbr.target_bitrate;
            }

        }
        /* MJPEG codec */
        else if(codec.codec_mode == CAMERA_CODEC_MODE_MJPEG)
        {
            pstVencAttrs->enOutputType = MJPEG_ENC_TYPE;
            pstVencAttrs->u32MaxBufSize = 0;
            pstVencAttrs->u32MaxDMASize = 0;
            pstVencAttrs->enChromaMode = VENC_CHROMA_YUV420;
            pstVencAttrs->u32Priority = 0;
            pstVencAttrs->stH264Attr.u16Framerate = 1; /* snapshot: 1fps avoids VENC budget limit for 8MP MJPEG */

            pstRcAttrs->enRcMode = VENC_RC_MODE_MJPEG;
            pstRcAttrs->stMjpegRc.u16Quality = rc.mjpeg.quality;
        }


        pstExtAttrs->stAdvance.enCuBlock = VENC_CUSize32x32;
        for(int j = 0; j < 3; j++)
        {
            pstExtAttrs->stAdvance.u16MinQP[j] = rc.avc.vbr.min_qp;
            pstExtAttrs->stAdvance.u16MaxQP[j] = rc.avc.vbr.max_qp;
        }
        pstExtAttrs->stAdvance.s16IPDelta = 0;
        pstExtAttrs->stAdvance.u32MaxPictureISize = 0;
        pstExtAttrs->stAdvance.u32MaxPicturePSize = 0;
        pstExtAttrs->stAdvance.u32MaxPictureBSize = 0;
        pstExtAttrs->stAdvance.bEnableSRD = AK_FALSE;
        pstExtAttrs->stAdvance.u8SRDThreshold = 0;

    #ifdef ENABLE_VENC_MMA
        pstExtAttrs->stAdvance.bEnableMMA = AK_TRUE;
    #endif

        log_debug("AK_MPI_VENC_Open id:%d", stream_info->codec_id);

        ret = AK_MPI_VENC_Open(stream_info->codec_id, &stChnAttrs);
        if (ret)
        {
            hal_camera_codec_t *slot =
                &my_data->hal_camera_device[device_id].channel[channel_id].codec[stream_id];
            int id = stream_info->codec_id;

            log_error("dev%d open venc failed", id);
            if (id >= 0 && id != MJPEG_SNAPSHOT_CODEC_ID)
                camera_codec_unregister_id(my_data, id);
            else if (id == MJPEG_SNAPSHOT_CODEC_ID)
                my_data->codec_id_status[MJPEG_SNAPSHOT_CODEC_ID] = 0;
            free(slot->pstStream);
            slot->pstStream = NULL;
            stream_info->codec_id = -1;
            return ret;
        }
    }
    return ret;
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

    for(int i=0 ;i<stream->num_channel; i++)
    {
        stream_info_t *stream_info = stream->priv[i];
        if (stream_info == NULL) {
            continue;
        }

        int device_id = stream_info->device_id;
        int channel_id = stream_info->channel_id;
        int stream_id = stream_info->stream_id;
        /* Consume codec_id once so concurrent disable paths cannot close same encoder twice. */
        int codec_id = __sync_lock_test_and_set(&stream_info->codec_id, -1);

        if (codec_id < 0) {
            continue;
        }

        hal_camera_codec_t *codec_info =
            &my_data->hal_camera_device[device_id].channel[channel_id].codec[stream_id];

        if (codec_id != MJPEG_SNAPSHOT_CODEC_ID) {
            camera_codec_unregister_id(my_data, codec_id);
        } else {
            my_data->codec_id_status[MJPEG_SNAPSHOT_CODEC_ID] = 0;
        }

        /* Wait for any hal_camera_codec_read() already in flight on the capture thread
         * (it runs without cdev->lock) to finish before freeing pstStream / closing the
         * venc handle below. Without this, the capture thread can still be inside
         * AK_MPI_VENC_ReleaseStream()/AK_MPI_VENC_EncodeFrame() on the freed pstStream or
         * the closed handle, corrupting the heap. */
        codec_io_lock(device_id, channel_id, stream_id);
        codec_wait_reader_idle(device_id, channel_id, stream_id);

        /* Release only when a stream buffer was really held. */
        if (codec_info->pstStream != NULL && codec_info->stream_buffer_release_flag) {
            int rel = AK_MPI_VENC_ReleaseStream(codec_id, codec_info->pstStream);
            if (rel != 0) {
                log_warn("VENC_ReleaseStream codec_id=%d ret=%d (picture/snapshot)", codec_id, rel);
            }
            codec_info->stream_buffer_release_flag = false;
        }

        if (codec_info->pstStream != NULL) {
            free(codec_info->pstStream);
            codec_info->pstStream = NULL;
        }
        log_debug("close camera codec id:%d", codec_id);

        ret = AK_MPI_VENC_Close(codec_id);
        if (ret != 0) {
            log_warn("AK_MPI_VENC_Close failed: codec_id=%d ret=%d", codec_id, ret);
        }

        codec_io_unlock(device_id, channel_id, stream_id);
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

    ret = AK_MPI_VENC_RequestIdr(codec_id);
    if (ret != 0) {
        log_warn("AK_MPI_VENC_RequestIdr handle=%d ret=%d", codec_id, ret);
        return -1;
    }
    log_info("AK_MPI_VENC_RequestIdr handle=%d ok (dev=%d ch=%d sid=%d)",
             codec_id, device_id, channel_id, stream_id);
    return 0;
}

size_t hal_camera_codec_read(void *hal_camera, int device_id, int channel_id,int stream_id, void **buffer, uint64_t * timestamp,size_t bytes)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    struct camera_device *cdev = my_data->cdev;
    int ret;

    VPSS_FRAME_INFO_S *pstFrame = my_data->hal_camera_device[device_id].channel[channel_id].pstFrame;
    hal_camera_codec_t *codec_info = &my_data->hal_camera_device[device_id].channel[channel_id].codec[stream_id];

    /* Serialize against hal_camera_codec_disable(), which can free pstStream and close
     * the venc handle concurrently from the control thread (this capture thread does not
     * hold cdev->lock). Re-read codec_id/pstStream under the lock rather than trusting
     * values fetched beforehand, since disable() may have run in between. */
    codec_io_lock(device_id, channel_id, stream_id);

    int codec_id = cdev->device_info[device_id].channel_info[channel_id].stream_info[stream_id].codec_id;
    VIDEO_STREAM_S *pstStream = codec_info->pstStream;

    if (codec_id == -1 || pstStream == NULL)
    {
        codec_io_unlock(device_id, channel_id, stream_id);
        log_error("codec id don't registered!!!,device_id:%d,channel_id:%d,stream_id:%d",device_id, channel_id, stream_id);
        return -1;
    }

    if (codec_info->stream_buffer_release_flag == true) {
        int rel = AK_MPI_VENC_ReleaseStream(codec_id, pstStream);
        if (rel != 0) {
            log_warn("VENC_ReleaseStream before encode codec_id=%d ret=%d", codec_id, rel);
        }
        codec_info->stream_buffer_release_flag = false;
    }


    memset(pstStream, 0x00, sizeof(VIDEO_STREAM_S));

    /* send it to encode */
    ret = AK_MPI_VENC_EncodeFrame(codec_id, pstFrame->stFrame.pu8Data,     \
                                        pstFrame->stFrame.u32Len, pstFrame->pvMdInfo, pstStream);


    if (ret)
    {
        /* send to encode failed */
        codec_io_unlock(device_id, channel_id, stream_id);
        log_error( "send to encode failed");
    }
    else
    {
        *buffer = pstStream->pu8Data;
        *timestamp = pstStream->u64Ts;  /* Note: This value is 0 on current platform */
        codec_info->frame_type = (int)pstStream->enFrameType;

        codec_info->stream_buffer_release_flag = true;

        size_t len = pstStream->u32Len;
        if (len > 0)
            codec_pin_buf(device_id, channel_id, stream_id);
        codec_io_unlock(device_id, channel_id, stream_id);
        return len;
    }

    return -1;
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




#if 0
static int aas_vi_encode_picture(aas_vi_channel *pChns,int handle, aa_frame_info *pFrame, aa_cap_picture_info2 *pCapInfo2)
{
    //先做截一个图，多图暂时不做;
    //需要打开编码器，此函数不做编码器开关
    if(pChns->iEncJpgHandle    < 0)
        return AA_ERROR_HANDLE_NOEXIST;

    if(pCapInfo2 == NULL)
        return AA_ERROR_NULL_POINTER_PARAM;

    VIDEO_STREAM_S vs;
    memset(&vs, 0, sizeof(vs));
    
    int iRet = -1;
    unsigned char *pFrameData     = pFrame->pData;
    int        uiFrameDataLen    = pFrame->uiDataLen ;
    //unsigned char *pResizeBuf    = NULL;
    if(pCapInfo2->iIsFull == 0 && pCapInfo2->iPicNum > 0)//need to cut
    {
        int    i = 0;
        //认为所有的输出大小不变
        //int iOutEncLen = ow * oh * 3 / 2;
        //pResizeBuf = ak_mem_dma_alloc( iOutEncLen);
        //printf("pCapInfo2->iPicNum=%d ",pCapInfo2->iPicNum);
        for(i = 0;i < pCapInfo2->iPicNum; i++)
        {
            int ow = (pCapInfo2->stCapInfo[i].uiDstW+31) /32*32;
            int oh = (pCapInfo2->stCapInfo[i].uiDstH+7) /8*8;
            if(ow <= 0 || oh <= 0)
            {
                log_error("input ow/oh error, left:%lf top:%lf w:%lf h:%lf ! ",
                                            pCapInfo2->stCapInfo[i].dLeft,pCapInfo2->stCapInfo[i].dTop,pCapInfo2->stCapInfo[i].dWidth, pCapInfo2->stCapInfo[i].dHeight);
                continue;
            }
            
            int x1 = pCapInfo2->stCapInfo[i].dLeft    * pFrame->iWidth;
            int y1 = pCapInfo2->stCapInfo[i].dTop    * pFrame->iHeight;
            int w = pCapInfo2->stCapInfo[i].dWidth    * pFrame->iWidth;
            int h = pCapInfo2->stCapInfo[i].dHeight * pFrame->iHeight;
            y1 = y1 / 2 * 2 ;
            x1 = x1 / 2 * 2 ;
            w +=  2 ;
            h +=  2 ;

            //printf( "===aas_encode_picture2=== (x:%lf y:%lf w:%lf h:%lf )", pCapInfo2->stCapInfo[i].dLeft,pCapInfo2->stCapInfo[i].dTop,pCapInfo2->stCapInfo[i].dWidth,pCapInfo2->stCapInfo[i].dHeight);    
            
            //printf("yuv420sp_roi_resize(w:%d h:%d) roi(%d %d %d %d) ow:%d oh:%d ! ",
            //                pframe->iWidth,pframe->iHeight,x1, y1, w , h,ow,oh);

            //long long llSset = aas_get_sys_run_time64();                
            aas_vi_check_param_and_set(pChns,ow,oh);
            //printf("aas_check_param_and_set:%lld ",aas_get_sys_run_time64()-llSset);
            //long long llS = aas_get_sys_run_time64();
            //printf("yuv420sp_roi_resize(w:%d h:%d) roi(%d %d %d %d) ow:%d oh:%d  ! ",
            //    pframe->iWidth,pframe->iHeight,x1, y1, w , h,ow,oh);
            iRet = aas_yuv420sp_roi_resize(pFrame->pData, pFrame->iWidth,pFrame->iHeight, 
                                x1, y1, w , h, ow , oh , pChns->pResizeBuf);
            //printf("aas_yuv420sp_roi_resize:%lld ",aas_get_sys_run_time64()-llS);
            if(iRet)
            {
                log_error( "yuv420sp_roi_resize(w:%d h:%d) roi(%d %d %d %d) ow:%d oh:%d failed ! ",
                                pFrame->iWidth,pFrame->iHeight,x1, y1, w , h,ow,oh);
                //if(pResizeBuf)
                //    ak_mem_dma_free(pResizeBuf);
                
                return AA_ERROR_NO_DATA;
            }

            pFrameData =  pChns->pResizeBuf;
            uiFrameDataLen = pChns->iResizeBufLen;
            iRet = AK_MPI_VENC_EncodeFrame(handle, pFrameData, uiFrameDataLen, NULL, &vs);
            
            if(iRet == 0)
            {
                memcpy(pCapInfo2->pBuf + pCapInfo2->iDataLen , vs.pu8Data, vs.u32Len);
                pCapInfo2->iDataLen += vs.u32Len;
                pCapInfo2->stCapInfo[i].iDataLen = vs.u32Len;
            }

            AK_MPI_VENC_ReleaseStream(handle, &vs);
        }
        
    }
    else if(pCapInfo2->iIsFull == 1)
    {
        aas_vi_check_param_and_set(pChns,pFrame->iWidth,pFrame->iHeight);
        /* do the video encode */
        log_info( "yuv DataLen=%d w=%d h=%d ", uiFrameDataLen, pFrame->iWidth, pFrame->iHeight);
        iRet = AK_MPI_VENC_EncodeFrame(handle, pFrameData, uiFrameDataLen, NULL, &vs);
        if(iRet == 0)
        {
            if(pCapInfo2->pBuf == NULL) //如果没有分配内存，则内部分配内存
            {
                pCapInfo2->pBuf = aas_malloc(vs.u32Len);
                if(pCapInfo2->pBuf == NULL)
                {
                    pCapInfo2->iBufLen = 0;
                    log_error( "malloc(size:%d) failed ! ", vs.u32Len);
                    iRet = AA_ERROR_OUT_OF_MEM;
                }
                else
                    pCapInfo2->iBufLen = vs.u32Len;
            }
        
            if( pCapInfo2->iBufLen < vs.u32Len)
            {
                iRet = AA_ERROR_BUF_TOO_SMALL;
                log_error( "cap failed, BufLen=%d, DataLen=%d ", pCapInfo2->iBufLen, vs.u32Len);
            }
            else
            {
                memcpy(pCapInfo2->pBuf, vs.pu8Data, vs.u32Len);
                pCapInfo2->iDataLen = vs.u32Len;
                log_debug("pCapInfo2->iDataLen =%d , vs.len=%d", pCapInfo2->iDataLen, vs.u32Len);
                iRet = 0;
                log_info( "out: encode ok, jpg DataLen=%d w=%d h=%d iRet=%d ", vs.u32Len, pFrame->iWidth, pFrame->iHeight, iRet);
            }
            AK_MPI_VENC_ReleaseStream(handle, &vs);
        }
        else
        {
            log_error( "out: encode failed, yuv DataLen=%d w=%d h=%d iRet=%d handle_id=%d ",
                pFrame->uiDataLen, pFrame->iWidth, pFrame->iHeight, iRet, handle);        
        }
    }
    
    return 0;
}

#endif