// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2024 Anyka(GuangZhou) Microelectronics Co., Ltd.
 * @DATE     2024-07
 * @VERSION  1.0
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>


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
#include "hal_ipu_api.h"
#include "hal_camera_chip_config.h"
#include "hal_camera_config.h"
#include "hal_camera_ai_tile_overlap.h"

#define LOG_TAG "HAL CAMERA AI"

/* Camera IPU helper functions */
static int s_camera_ipu_queue_id = -1;

static int camera_ipu_get_queue_id(void)
{
    int q;
    static int s_queue_resolve_logged;

    if (s_camera_ipu_queue_id >= 0) {
        q = s_camera_ipu_queue_id;
    } else {
        q = hal_camera_get_config()->ipu_que_id;
    }
    if (!s_queue_resolve_logged) {
        s_queue_resolve_logged = 1;
        log_info("camera IPU: active queue_id=%d (allocated_by_hal=%s, config ipu_que_id=%d)",
                 q, (s_camera_ipu_queue_id >= 0) ? "yes" : "no", hal_camera_get_config()->ipu_que_id);
    }
    return q;
}

static void camera_rect_to_ipu_rect(camera_rect_t *camera_rect, IPU_RECT_S *ipu_rect)
{
    ipu_rect->s32X = (camera_rect->x / 2) * 2;
    ipu_rect->s32Y = (camera_rect->y / 2) * 2;
    ipu_rect->u32W = (camera_rect->width / 2) * 2;
    ipu_rect->u32H = (camera_rect->height / 2) * 2;
}

#include "../../camera_hw.h"
#include "../../hal_camera_api.h"

static void* camera_process_ai_thread_function(void* arg) ;
/* Track whether camera module owns the current NPU init. */
static int s_camera_npu_owned = 0;
/* SVP CreateChn succeeded for this ai_id — only then is DestroyChn safe. */
static uint8_t s_svp_chn_created[CAMERA_AI_MAX_MODLE];

hal_camera_ai_config_t camera_ai =
{
  .camera_ai_type_flag = CAMERA_AI_NONE,
};

int  hal_camera_ai_init(void *hal_camera)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    int ret = 0;

    log_info("hal_camera_ai_init, NPU init");

    ret = AK_MPI_NPU_Init();

    if(ret != AK_SUCCESS)
    {
        /*
         * Some firmware stacks keep NPU initialized globally (e.g. another module already owns it),
         * and AK_MPI_NPU_Init returns a non-zero "already in work" style code.
         * Treat this as non-fatal and continue so camera AI can attach to the existing NPU runtime.
         */
        s_camera_npu_owned = 0;
        log_warn("AK_MPI_NPU_Init returned %d, continue with shared/existing NPU context", ret);
    } else {
        s_camera_npu_owned = 1;
    }

    my_data->ai_config.num_modle = 0;

    return 0;
}


int  hal_camera_ai_uninit(void *hal_camera)
{
    (void)hal_camera;

    if (s_camera_npu_owned) {
        log_info("AK_MPI_NPU_Deinit");
        (void)AK_MPI_NPU_Deinit();
        s_camera_npu_owned = 0;
    } else {
        log_info("skip AK_MPI_NPU_Deinit (NPU not owned by camera module)");
    }

    return 0;
}

int camera_ai_register_id( void *hal_camera)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;

    for(int i=0; i<CAMERA_AI_MAX_MODLE; i++)
    {
        if(my_data->ai_id_status[i] == 0)
        {
            my_data->ai_id_status[i] = 1;
            log_info("regist ai id:%d",i);
            return i;
        }
    }

    log_info("regist ai id fail!");
    return -1;
}

int camera_ai_unregister_id( void *hal_camera, int ai_id)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;

    my_data->ai_id_status[ai_id] = 0;

    return 0;
}


int  hal_camera_ai_tile_overlap_enable(void *hal_camera)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    int queue_id;
    
    if (hal_ipu_open() != 0) {
        return -1;
    }

    {
        int cfg_que = hal_camera_get_config()->ipu_que_id;
        int from_alloc = 0;
        int from_get_default = 0;

        queue_id = cfg_que;
        if (hal_ipu_alloc_queue(&queue_id) == 0) {
            from_alloc = 1;
        } else if (hal_ipu_get_default_queue("camera", &queue_id) == 0) {
            from_get_default = 1;
        } else {
            log_error("tile_overlap_enable: IPU queue alloc+get_default both failed (cfg_que_id=%d)", cfg_que);
            hal_ipu_close();
            return -1;
        }
        s_camera_ipu_queue_id = queue_id;
        log_info("tile_overlap_enable: IPU queue_id=%d, cfg_que_id=%d, path=%s",
                 queue_id, cfg_que, from_alloc ? "alloc_first_free" : (from_get_default ? "get_default(camera)" : "?"));
    }
    return 0;
}


int  hal_camera_ai_tile_overlap_disable(void *hal_camera)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    
    if (s_camera_ipu_queue_id >= 0) {
        log_info("tile_overlap_disable: hal_ipu_free_queue queue_id=%d", s_camera_ipu_queue_id);
        hal_ipu_free_queue(s_camera_ipu_queue_id);
        s_camera_ipu_queue_id = -1;
    }
    hal_ipu_close();

    return 0;
}

/**
 * @brief Get detect type from sub type (recognition/classification).
 * @param sub_type The sub AI type.
 * @return The corresponding detect type.
 */
camera_ai_type_t hal_camera_ai_get_detect_type(camera_ai_type_t sub_type)
{
    int num_ai_type_detect_sub = 0;
    const hal_camera_ai_type_detect_sub_cfg_t *table =
        hal_camera_get_ai_detect_sub_table(&num_ai_type_detect_sub);

    for (int i = 0; i < num_ai_type_detect_sub; i++) {
        if (table[i].sub_type & sub_type) {
            return table[i].detect_type;
        }
    }
    return CAMERA_AI_NONE;
}

/**
 * @brief Get secondary type from detect type (recognition/classification).
 * @param detect_type The detect AI type.
 * @return The corresponding secondary type.
 */
camera_ai_type_t hal_camera_ai_get_secondary_type(camera_ai_type_t detect_type)
{
    int num_ai_type_detect_sub = 0;
    const hal_camera_ai_type_detect_sub_cfg_t *table =
        hal_camera_get_ai_detect_sub_table(&num_ai_type_detect_sub);

    for (int i = 0; i < num_ai_type_detect_sub; i++) {
        if (table[i].detect_type & detect_type) {
            return table[i].sub_type;
        }
    }
    return CAMERA_AI_NONE;
}

/**
 * @brief Close SVP channel.
 * @param hal_camera Pointer to HAL camera data.
 * @param ai_id AI channel ID to disable.
 * @return 0 on success.
 */
int  hal_camera_ai_disable_channel(void *hal_camera, int ai_id)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    int ret = 0;

    if (ai_id < 0 || ai_id >= CAMERA_AI_MAX_MODLE || !s_svp_chn_created[ai_id]) {
        return 0;
    }

    ret = AK_MPI_SVP_DestroyChn(ai_id);
    if(ret != AK_SUCCESS)
    {
        log_error("Destroy SVP chn failed, ai_id=%d ret=%d", ai_id, ret);
        return ret;
    }
    s_svp_chn_created[ai_id] = 0;

    if(my_data->ai_config.num_modle == 0 && \
        my_data->ai_config.status == CAMERA_STATUS_ENABLE)
    {
        hal_camera_ai_uninit(my_data);
        my_data->ai_config.status = CAMERA_STATUS_UNINITIALIZE;
    }

    return 0;
}

/**
 * @brief Open SVP channel.
 * @param hal_camera Pointer to HAL camera data.
 * @param modle_id Model ID.
 * @param ai_input_id AI input ID.
 * @return 0 on success, negative on failure.
 */
int hal_camera_ai_enable_channel( void *hal_camera, int modle_id, int ai_input_id)
{
    int ret = -1;
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    struct camera_device *cdev = my_data->cdev;

    log_info("[CAM_TRACE] hal_camera_ai_enable_channel: enter modle=%d ai_input=%d (tid=%lu)",
             modle_id, ai_input_id, (unsigned long)pthread_self());

    hal_ai_modle_t *modle = &my_data->ai_config.modle[modle_id];
    const hal_camera_ai_modle_config_support_t *support_modle = modle->support_modle;
    int ai_id = modle->ai_id;
    camera_ai_input_t *ai_input = &cdev->ai_input[ai_input_id];

    /* First model on this lifecycle: force a fresh/safe NPU init path.
     * This avoids stale status causing skipped init after app switches. */
    if (my_data->ai_config.num_modle == 0)
    {
        int init_ret = hal_camera_ai_init(my_data);
        if (init_ret == 0) {
            my_data->ai_config.status = CAMERA_STATUS_ENABLE;
        } else {
            my_data->ai_config.status = CAMERA_STATUS_DISABLE;
            log_warn("Camera AI initialization failed on first model, AI features will be disabled");
            return init_ret;
        }
    }
    else if(my_data->ai_config.status == CAMERA_STATUS_UNINITIALIZE)
    {
        int ret = hal_camera_ai_init(my_data);
        if (ret == 0) {
            my_data->ai_config.status = CAMERA_STATUS_ENABLE;
        } else {
            /* NPU initialization failed, mark as disabled */
            my_data->ai_config.status = CAMERA_STATUS_DISABLE;
            log_warn("Camera AI initialization failed, AI features will be disabled");
        }
    }

    /* Number of models - currently fixed at 1 */
    int num_model = 1;
    log_info("ai open, num_model:%d", num_model);

    /* init the svp attr */
    AK_S32 svp_len = sizeof(SVP_CHN_ATTR_S) + num_model*sizeof(SVP_MODEL_ATTR_S);
    modle->attr = (SVP_CHN_ATTR_S *)AK_BASE_MemAlloc(MODULE_ID_APP, svp_len);
    if (modle->attr == NULL) {
        log_error("alloc svp attr failed, len=%d", svp_len);
        return AK_FAILED;
    }
    memset(modle->attr, 0, svp_len);
    modle->attr->s32ModelNum = num_model;
    modle->attr->astModelAttr[0].enModelType = support_modle->enModelType;

    if(support_modle->ai_type[0] & (CAMERA_AI_DETECT_MASK & ~CAMERA_AI_DETECT_CLASSIFY_MASK))
    {
        modle->attr->astModelAttr[0].stModelParam.param.stHdParam.s32ClassifyThreshold = ai_input->classify_threshold;
        /* set the IoU threshold, range [3-10], default is 3*/
        modle->attr->astModelAttr[0].stModelParam.param.stHdParam.s32IouThreshold = ai_input->iou_threshold;
    }
    else
    {
        modle->attr->astModelAttr[0].stModelParam.param.stFasParam.s32ConfidenceThreshold = ai_input->classify_threshold;
    }

    memset(&modle->conf_info, 0, sizeof(SVP_CONF_INFO_S));

    modle->conf_info.conf_type.pcConf = (AK_CHAR *)&support_modle->modle_name;
    modle->conf_info.s32ConfMode = 0;  //0 for modle

    modle->stInput.enImgType             =  support_modle->enImgType;
    modle->stInput.s32Width             =  support_modle->width;
    modle->stInput.s32Height            =  support_modle->height;
    modle->stInput.stPosInfo.s32Left    = 0;
    modle->stInput.stPosInfo.s32Top     = 0;
    modle->stInput.stPosInfo.s32Width   = support_modle->width;
    modle->stInput.stPosInfo.s32Height  = support_modle->height;


    log_info("enEnvMode:%d, enModelType:%d", \
        modle->attr->astModelAttr[0].enEnvMode, \
        modle->attr->astModelAttr[0].enModelType);

    log_info("s32ClassifyThreshold:%d, s32IouThreshold:%d", \
        modle->attr->astModelAttr[0].stModelParam.param.stHdParam.s32ClassifyThreshold, \
        modle->attr->astModelAttr[0].stModelParam.param.stHdParam.s32IouThreshold
    );

    log_info("pcConf:%s", \
        modle->conf_info.conf_type.pcConf
    );

    log_info("ai_id[%d] enabled (svp channel create)",ai_id);

    /* Only Destroy channels we previously Created. Cold-start DestroyChn logs
     * "This module should be init first" from libmpi_svp. */
    if (ai_id >= 0 && ai_id < CAMERA_AI_MAX_MODLE && s_svp_chn_created[ai_id]) {
        AK_S32 destroy_ret = AK_MPI_SVP_DestroyChn(ai_id);
        if (destroy_ret == AK_SUCCESS) {
            log_info("[CAM_TRACE] destroyed stale SVP chn ai_id=%d before create", ai_id);
            s_svp_chn_created[ai_id] = 0;
        }
    }

    log_info("[CAM_TRACE] hal_camera_ai_enable_channel: before AK_MPI_SVP_CreateChn ai_id=%d", ai_id);
    ret = AK_MPI_SVP_CreateChn(ai_id, modle->attr, &modle->conf_info);
    log_info("[CAM_TRACE] hal_camera_ai_enable_channel: after AK_MPI_SVP_CreateChn ret=%d", ret);
    if(ret != AK_SUCCESS)
    {
        log_error("Create SVP chn failed, ai_id=%d ret=%d", ai_id, ret);
        if(modle->attr != NULL) {
            AK_BASE_MemFree(modle->attr);
            modle->attr = NULL;
        }
        return ret;
    }
    if (ai_id >= 0 && ai_id < CAMERA_AI_MAX_MODLE)
        s_svp_chn_created[ai_id] = 1;

    if(modle->attr != NULL) {
        AK_BASE_MemFree(modle->attr);
        modle->attr = NULL;
    }

    log_info("create chn pass");

#if 0
        MD: MD temporarily not processed
    SVP_MD_PARAM_S stParam = {0};
    /*init the md param, please note that, this param is only used for SVP correction*/
    stParam.s32DevId = s32DevId;

    /* set the valid area range of md detect, range is [1-768]*/
    stParam.s32MoveSizeMin = 2;
    stParam.s32MoveSizeMax = 300;
    /* set the sensitivity param of md detect, range [1-65535], the less the more sensitive, but the less accurate */
    stParam.s32FltBig = 1500;
    stParam.s32FltSmall = 1500;

    /* init svp md function */
    ret = AK_SVP_MdInit(&stParam);
    if(ret != AK_SUCCESS)
    {
        log_error( "SVP init md failed!");
        return ret;
    }

    /* enable the svp md function */
    ret = AK_SVP_MdEnable(s32DevId, 1);
    if(ret != AK_SUCCESS)
    {
        log_error( "MD enable failed!");
        AK_SVP_MdUninit(s32DevId);
        return ret;
    }
#endif

    return ret;
}

/**
 * @brief Get corresponding AI input ID based on algorithm type.
 * @param my_data Pointer to HAL camera data.
 * @param ai_type AI type to search for.
 * @return AI input ID on success, -1 on failure.
 */
int get_ai_input_id(struct hal_camera_data *my_data, camera_ai_type_t ai_type )
{
    struct camera_device *cdev = my_data->cdev;

    for(int i = 0; i < cdev->num_camera_ai; i ++)
    {
        if(cdev->ai_input[i].ai_type == ai_type)
        {
            return i;
        }
    }
    return -1;
}



int camera_ai_sign_modle(void *hal_camera,  const hal_camera_ai_modle_config_support_t *supported_modle, \
    int ai_input_id, camera_ai_type_t  ai_type)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    struct camera_device *cdev = my_data->cdev;
    int modle_id;

    /* Get model ID from my_data->ai_config.modle */
    int module_regested_flag = 0;
    for(modle_id = 0; modle_id < my_data->ai_config.num_modle; modle_id ++)
    {
        if( my_data->ai_config.modle[modle_id].support_modle == supported_modle)
        {
            module_regested_flag = 1;
            break;
        }
    }

    /* Register model */
    if(module_regested_flag == 0)
    {
        modle_id = my_data->ai_config.num_modle;
        if(my_data->ai_config.num_modle >= CAMERA_AI_MAX_MODLE)
        {
            log_error("enable modle:%d, the max modle number:%d!!", my_data->ai_config.num_modle+1, CAMERA_AI_MAX_MODLE);
            return -1;
        }

        my_data->ai_config.modle[modle_id].support_modle = supported_modle;
        my_data->ai_config.modle[modle_id].ai_type = cdev->ai_input[ai_input_id].ai_type;
        my_data->ai_config.modle[modle_id].ai_id = camera_ai_register_id(hal_camera);


        if(ai_type & CAMERA_AI_DETECT_CLASSIFY_MASK || 
            ai_type & CAMERA_AI_RECOGNIZE_MASK )
        {
            int data_len = my_data->ai_config.modle[modle_id].support_modle->width * my_data->ai_config.modle[modle_id].support_modle->height * 3;

            const hal_camera_config_t *cfg = hal_camera_get_config();
            my_data->ai_config.modle[modle_id].secondary.output_buffer = \
            AK_BASE_MemDmaAlloc(cfg->ipu_module_id,  data_len);
            AK_BASE_MemDmaVaddr2Paddr(my_data->ai_config.modle[modle_id].secondary.output_buffer, \
                       &my_data->ai_config.modle[modle_id].secondary.output_buffer_phy_addr);

            log_info("modle_id:%d secondary registed",modle_id);
        }
        else
        {
            my_data->ai_config.modle[modle_id].detect.streams = cdev->ai_input[ai_input_id].stream;

            log_info("modle_id:%d detect registed",modle_id);
        }

        int en_ret = hal_camera_ai_enable_channel(hal_camera, modle_id , ai_input_id);
        log_info("%s",AK_MPI_SVP_GetVersion());
        if (en_ret != 0) {
            log_error("enable SVP channel failed modle_id=%d ai_id=%d ret=%d",
                      modle_id, my_data->ai_config.modle[modle_id].ai_id, en_ret);
            camera_ai_unregister_id(hal_camera, my_data->ai_config.modle[modle_id].ai_id);
            memset(&my_data->ai_config.modle[modle_id], 0, sizeof(hal_ai_modle_t));
            return en_ret;
        }

        my_data->ai_config.num_modle++;
    }
    /* Register ai_type to model again */
    my_data->ai_config.modle[modle_id].ai_type |= (ai_type & CAMERA_AI_MAIN_MASK);

    return 0;
}


int hal_camera_ai_add(void *hal_camera, camera_ai_type_t ai_type)
{
    log_info("[CAM_TRACE] hal_camera_ai_add: type=0x%x (tid=%lu)",
             ai_type, (unsigned long)pthread_self());
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    struct camera_device *cdev = my_data->cdev;
    camera_ai_type_t detect_type = 0;
    camera_ai_type_t secondary_type = 0;

    log_info("add type:%x",ai_type);

    /* Check if it's a recognize algorithm, ensure corresponding detection algorithm is enabled */
    if (ai_type & CAMERA_AI_DETECT_CLASSIFY_MASK)
    {
        detect_type = hal_camera_ai_get_detect_type(ai_type);
        secondary_type = ai_type & CAMERA_AI_DETECT_CLASSIFY_MASK;
    }
    else if(ai_type & CAMERA_AI_RECOGNIZE_MASK)
    {
        detect_type = hal_camera_ai_get_detect_type(ai_type);
        secondary_type = ai_type & CAMERA_AI_RECOGNIZE_MASK;
    }
    else if(ai_type & CAMERA_AI_DETECT_MASK)
    {
        detect_type = ai_type & CAMERA_AI_DETECT_MASK;
    }

    int ai_input_id = get_ai_input_id(my_data, ai_type);
    if(ai_input_id == -1)
    {
        log_error("ai_type:%d don't set config, plz call set_parameters ", ai_type);
        return -1;
    }
    camera_stream_t *stream = cdev->ai_input[ai_input_id].stream;
    stream_info_t *stream_info = stream->priv[stream->priv_id];

    int num_model_support = 0;
    const hal_camera_ai_modle_config_cfg_t *model_table =
        hal_camera_get_ai_model_table(&num_model_support);

    SVP_IMG_TYPE_E format;

    switch (stream->format)
    {
        case SYSTEM_PIXEL_FORMAT_YCBCR_420_SP:
            format = AK_SVP_IMG_YUV420SP;
        break;
        case SYSTEM_PIXEL_FORMAT_RGB_LINEINTL:
            format = AK_SVP_IMG_RGB_LI;
        break;
        case SYSTEM_PIXEL_FORMAT_RGB_HWC:
            format = AK_SVP_IMG_RGB_HWC;
        break;
        case SYSTEM_PIXEL_FORMAT_RGB_CHW:
            format = AK_SVP_IMG_RGB_CHW;
        break;
        default: 
            log_error("unsupport ai format:%d", stream->format);
            return -1;
    }

    log_info("support model number:%d", num_model_support);
    int regester_flag = 0;

    if(detect_type != 0)
    {
        for (int i = 0; i < num_model_support; i++)
        {
            uint32_t stream_width,stream_height;

            stream_width = stream->channel_config[0].width;
            stream_height = stream->channel_config[0].height;

            /* Check if width/height and SVP_IMG_TYPE_E match */
            if ((model_table[i].width == stream_width) &&
                (model_table[i].height == stream_height) &&
                (model_table[i].enImgType == format))
            {
                for (int j = 0; j < model_table[i].num_camera_ai_modle; j++)
                {
                    /* If supported ones match, save model and record supported algorithms */
                    if (detect_type & model_table[i].ai_type[j])
                    {
                        log_info("ai_type[%x], detect_type[%x] to module[%d, %d]", ai_type, detect_type, i, j);

                        camera_ai_sign_modle(hal_camera, &model_table[i], ai_input_id, detect_type);

                        regester_flag = 1;
                        break;
                    }
                }
            }
        }
    }

    if (secondary_type != 0) 
    {
        regester_flag = 0;

        for (int i = 0; i < num_model_support; i++)
        {
            for (int j = 0; j < model_table[i].num_camera_ai_modle; j++)
            {
                // If supported ones match, record: model is the same
                if (secondary_type & model_table[i].ai_type[j])
                {
                    log_info("ai_type[%x],secondary_type[%x] to module [%d, %d]", ai_type, secondary_type, i, j);

                    // Save model and record supported algorithms
                    camera_ai_sign_modle(hal_camera, &model_table[i], ai_input_id, secondary_type);

                    regester_flag = 1;
                    break;
                }
            }
            if(regester_flag == 1) 
            {
                if (s_camera_ipu_queue_id < 0) {
                    int qcfg = hal_camera_get_config()->ipu_que_id;
                    int queue_id = qcfg;
                    if (hal_ipu_open() == 0) {
                        if (hal_ipu_alloc_queue(&queue_id) != 0 &&
                            hal_ipu_get_default_queue("camera", &queue_id) != 0) {
                            log_error("secondary AI: IPU queue alloc+get_default failed (cfg_que_id=%d)", qcfg);
                            hal_ipu_close();
                        } else {
                            s_camera_ipu_queue_id = queue_id;
                            log_info("secondary AI: IPU camera queue_id=%d (cfg_que_id=%d)", queue_id, qcfg);
                        }
                    } else {
                        log_error("secondary AI: hal_ipu_open failed");
                    }
                }
                break;
            }
        }
    }

    if( regester_flag == 0)
    {
        log_error("not support ai modle:%x",ai_type);
        return -1;
    }

    my_data->ai_thread_info.cdev = cdev;
    my_data->ai_thread_info.stream = stream;

    /* Publish AI type first, then start thread.
     * Otherwise thread may race: start early, see ai_type == NONE, and exit immediately. */
    my_data->ai_type |= (ai_type & CAMERA_AI_MAIN_MASK);
    stream_info->ai_type |= (ai_type & CAMERA_AI_MAIN_MASK);

    if ((stream_info->ai_thread_id == 0) &&
        (stream_info->ai_status != CAMERA_STATUS_READY &&
         stream_info->ai_status != CAMERA_STATUS_BUSY) &&
        ((stream_info->ai_type & CAMERA_AI_MAIN_MASK) != CAMERA_AI_NONE))
    {
        stream_info->ai_status = CAMERA_STATUS_ENABLE;
        if (pthread_create(&stream_info->ai_thread_id, NULL,
                           camera_process_ai_thread_function, &my_data->ai_thread_info) != 0) {
            log_error("create ai thread failed");
            stream_info->ai_status = CAMERA_STATUS_DISABLE;
            stream_info->ai_thread_id = 0;
            return -1;
        }
        log_info("[CAM_TRACE] hal_camera_ai_add: ai_thread spawned dev=%d ch=%d str=%d ai_type=0x%x",
                 stream_info->device_id, stream_info->channel_id, stream_info->stream_id,
                 (unsigned)stream_info->ai_type);
    }

    log_info("[CAM_TRACE] hal_camera_ai_add: done type=0x%x", ai_type);
    return AK_SUCCESS;
}

void hal_camera_ai_shutdown_all(void *hal_camera)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    struct camera_device *cdev;

    if (my_data == NULL) {
        log_info("[CAM_TRACE] hal_camera_ai_shutdown_all: skip (hal_camera null)");
        return;
    }
    cdev = my_data->cdev;
    if (cdev == NULL) {
        log_info("[CAM_TRACE] hal_camera_ai_shutdown_all: skip (cdev null)");
        return;
    }

    log_info("[CAM_TRACE] hal_camera_ai_shutdown_all: begin num_modle=%d",
             my_data->ai_config.num_modle);

    for (int m = 0; m < my_data->ai_config.num_modle; m++) {
        int ai_id = my_data->ai_config.modle[m].ai_id;
        if (ai_id >= 0 && ai_id < CAMERA_AI_MAX_MODLE && s_svp_chn_created[ai_id]) {
            log_info("[CAM_TRACE] hal_camera_ai_shutdown_all: SVP_DestroyChn ai_id=%d", ai_id);
            (void)AK_MPI_SVP_DestroyChn(ai_id);
            s_svp_chn_created[ai_id] = 0;
        }
        my_data->ai_config.modle[m].ai_type = 0;
    }
    my_data->ai_config.num_modle = 0;
    my_data->ai_type = CAMERA_AI_NONE;

    for (int d = 0; d < cdev->num_device; d++) {
        device_info_t *dev_info = &cdev->device_info[d];
        for (int c = 0; c < dev_info->num_channel; c++) {
            channel_info_t *ch_info = &dev_info->channel_info[c];
            for (int s = 0; s < ch_info->num_stream; s++) {
                stream_info_t *st = &ch_info->stream_info[s];
                st->ai_type = CAMERA_AI_NONE;
                st->ai_status = CAMERA_STATUS_DISABLE;
                if (st->ai_thread_id == 0)
                    continue;
                log_info("[CAM_TRACE] hal_camera_ai_shutdown_all: join ai_thread dev=%d ch=%d str=%d tid=%lu",
                         dev_info->device_id, c, s, (unsigned long)st->ai_thread_id);
                pthread_join(st->ai_thread_id, NULL);
                st->ai_thread_id = 0;
                log_info("[CAM_TRACE] hal_camera_ai_shutdown_all: join done dev=%d ch=%d str=%d",
                         dev_info->device_id, c, s);
            }
        }
    }

    hal_camera_ai_uninit(my_data);
    my_data->ai_config.status = CAMERA_STATUS_UNINITIALIZE;
    log_info("[CAM_TRACE] hal_camera_ai_shutdown_all: done");
}

/**
 * @brief Remove AI processing.
 * @param hal_camera Pointer to HAL camera data.
 * @param ai_type AI type to remove.
 * @return AK_SUCCESS on success.
 */
int hal_camera_ai_remove( void *hal_camera, camera_ai_type_t ai_type)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    struct camera_device *cdev = my_data->cdev;

    int modle_id;
    my_data->ai_type &= ~(ai_type & CAMERA_AI_MAIN_MASK);

    int ai_input_id = get_ai_input_id(my_data, ai_type);
    camera_stream_t *stream = cdev->ai_input[ai_input_id].stream;
    stream_info_t *stream_info = stream->priv[stream->priv_id];
    stream_info->ai_type &= ~(ai_type & CAMERA_AI_MAIN_MASK);

    /* Wait for AI thread to exit if no AI types remaining */
    if ((stream_info->ai_type & CAMERA_AI_MAIN_MASK) == CAMERA_AI_NONE) {
        if (stream_info->ai_thread_id != 0) {
            log_info("[CAM_TRACE] hal_camera_ai_remove: join ai_thread type=0x%x tid=%lu",
                     ai_type, (unsigned long)stream_info->ai_thread_id);
            pthread_join(stream_info->ai_thread_id, NULL);
            log_info("[CAM_TRACE] hal_camera_ai_remove: join ai_thread done type=0x%x", ai_type);
            stream_info->ai_thread_id = 0;
        }
        stream_info->ai_status = CAMERA_STATUS_DISABLE;
    }

    int num_model_support = 0;
    const hal_camera_ai_modle_config_cfg_t *model_table =
        hal_camera_get_ai_model_table(&num_model_support);

    log_debug("support model:%d", num_model_support);
 
    for(modle_id = 0; modle_id < my_data->ai_config.num_modle; modle_id ++)
    {
        if( my_data->ai_config.modle[modle_id].ai_type & ai_type & CAMERA_AI_MAIN_MASK)
        {
            my_data->ai_config.modle[modle_id].ai_type = my_data->ai_config.modle[modle_id].ai_type & ~(ai_type & CAMERA_AI_MAIN_MASK); 
            break;
        }
    }

    if(modle_id >= my_data->ai_config.num_modle)
    {
        return 0;
    }
    if(my_data->ai_config.modle[modle_id].ai_type == 0)
    {
        hal_camera_ai_disable_channel(hal_camera, my_data->ai_config.modle[modle_id].ai_id);
		camera_ai_unregister_id(hal_camera, my_data->ai_config.modle[modle_id].ai_id);
        if(ai_type & CAMERA_AI_DETECT_CLASSIFY_MASK || ai_type & CAMERA_AI_RECOGNIZE_MASK )
        {
            if(my_data->ai_config.modle[modle_id].secondary.output_buffer != NULL)
            {
                AK_BASE_MemDmaFree(my_data->ai_config.modle[modle_id].secondary.output_buffer);
                my_data->ai_config.modle[modle_id].secondary.output_buffer = NULL;
            }
        }

        bool has_secondary_type_enabled = false;
        for(int i=modle_id; i<my_data->ai_config.num_modle; i++)
        {
            memcpy(&my_data->ai_config.modle[i],&my_data->ai_config.modle[i+1],sizeof(hal_ai_modle_t));
            if(my_data->ai_config.modle[i].ai_type & CAMERA_AI_DETECT_CLASSIFY_MASK || 
                my_data->ai_config.modle[i].ai_type & CAMERA_AI_RECOGNIZE_MASK )
            {
                has_secondary_type_enabled = true;
            }
        }
        my_data->ai_config.num_modle --;

        if (my_data->ai_config.num_modle == 0 &&
            my_data->ai_config.status == CAMERA_STATUS_ENABLE) {
            hal_camera_ai_uninit(my_data);
            my_data->ai_config.status = CAMERA_STATUS_UNINITIALIZE;
            log_info("all ai models removed, camera ai uninitialized");
        }

    }

   return AK_SUCCESS;
}

/**
 * @brief Perform classification or recognition on detected object.
 * @param hal_camera Pointer to HAL camera data.
 * @param detect_modle_id Detection model ID.
 * @param device_id Device ID.
 * @param channel_id Channel ID.
 * @param secondary_ai_type Secondary AI type (classify/recognize).
 * @param in_rect Input rectangle.
 * @return 0 on success.
 */
int camera_ai_secondary_process(void *hal_camera, int detect_modle_id, \
        int device_id, int channel_id,int secondary_ai_type, camera_rect_t in_rect)
{
    int ret = -1;
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    struct camera_device *cdev = my_data->cdev;
    int modle_id = -1;

    /* Check all models */
    for(int i=0; i< my_data->ai_config.num_modle; i++)
    {
       if(secondary_ai_type & my_data->ai_config.modle[i].ai_type)
        {
            modle_id = i;
        }
    }

    if(modle_id == -1) return -1;

    int camera_ai_index = get_ai_input_id( my_data, secondary_ai_type );
    int ai_id = my_data->ai_config.modle[modle_id].ai_id;

    VPSS_FRAME_INFO_S *pstFrame = my_data->hal_camera_device[device_id].channel[channel_id].pstFrame;;

    unsigned char *input = my_data->ai_config.modle[detect_modle_id].stInput.pvVirAddr;
    unsigned long in_phy_addr = my_data->ai_config.modle[detect_modle_id].stInput.u64PhyAddr;
    unsigned int iw = my_data->ai_config.modle[detect_modle_id].support_modle->width;
    unsigned int ih = my_data->ai_config.modle[detect_modle_id].support_modle->height;

    unsigned char *output;
    unsigned long out_phy_addr;
    unsigned int ow = my_data->ai_config.modle[modle_id].support_modle->width;
    unsigned int oh = my_data->ai_config.modle[modle_id].support_modle->height;

    output = my_data->ai_config.modle[modle_id].secondary.output_buffer;
    out_phy_addr = my_data->ai_config.modle[modle_id].secondary.output_buffer_phy_addr;


    if(output == NULL)
    {
        log_error("Alloc dma memory for gustrure crop data failed!");
        return AK_FAILED;
    }

    //hal_camera_ipu_scale_crop(input, in_phy_addr, iw, ih, in_rect, output, out_phy_addr, ow, oh);
    {
        IPU_RECT_S ipu_rect;
        camera_rect_to_ipu_rect(&in_rect, &ipu_rect);
        hal_ipu_scale_crop_with_format(camera_ipu_get_queue_id(), in_phy_addr, iw, ih, &ipu_rect, 
                                      IPU_COLOR_FMT_PACKED_RGB24, out_phy_addr, ow, oh, IPU_COLOR_FMT_PACKED_RGB24);
    }

    my_data->ai_config.modle[modle_id].stInput.pvVirAddr = output;
    my_data->ai_config.modle[modle_id].stInput.u64PhyAddr = out_phy_addr;

    SVP_OUTPUT_S modle_output   = {0};

    memset(&modle_output, 0, sizeof(SVP_OUTPUT_S));


    ret = AK_MPI_SVP_Process(ai_id, &my_data->ai_config.modle[modle_id].stInput, &modle_output);

    /* Store detection result */
    int num_total = cdev->ai_output_result.num_total;

    const hal_camera_ai_modle_config_support_t *support_modle = my_data->ai_config.modle[modle_id].support_modle;

    /* Supports SVP_FAS_RESULT_S, SVP_GESTURE_RESULT_S, FACE_ANGLE_S */
    camera_ai_object_t *ai_object = &cdev->ai_output_result.ai_object[num_total];
    ai_object->rect = in_rect;

    int object_label;

    if(num_total < CAMERA_AI_MAX_BOX){
        if(secondary_ai_type & CAMERA_AI_DETECT_CLASSIFY_MASK)
        {
            ai_object->detect.score = modle_output.result.stFasResult.u32Confidence;
            object_label = modle_output.result.stFasResult.u32Label;
        }
        else if(secondary_ai_type & CAMERA_AI_RECOGNIZE_MASK)
        {
            ai_object->recognize.data_len = modle_output.result.stFrResult.u32DataLen;
            /* pcData is AK_CHAR* (raw byte buffer); caller interprets as int16_t feature vector */
            ai_object->recognize.data = (int16_t *)modle_output.result.stFrResult.pcData;
            object_label = 0;  /* Recognition has no label, use 0 */
        }
    }
    if(support_modle->ai_type[object_label] != CAMERA_AI_NONE)
    {
        ai_object->ai_type = support_modle->ai_type[object_label];
        cdev->ai_output_result.num_total ++;
    }
    return 0;
}


static int camera_ai_process(struct hal_camera_data *my_data, SVP_OUTPUT_S *modle_output,
        int device_id, int modle_id)
{
    camera_rect_t    rect;
    int camera_ai_index;
    struct camera_device *cdev = my_data->cdev;

    const hal_camera_ai_modle_config_support_t *support_modle = my_data->ai_config.modle[modle_id].support_modle;

    // Clear previous results
    cdev->ai_output_result.num_total = 0;

    for(int i = 0; i < modle_output->result.stHdResult.s32TotalNum; i++)
    {
        rect.x = modle_output->result.stHdResult.pstTargetBoxes[i].u16Left;
        rect.y = modle_output->result.stHdResult.pstTargetBoxes[i].u16Top;
        rect.width = modle_output->result.stHdResult.pstTargetBoxes[i].u16Right - modle_output->result.stHdResult.pstTargetBoxes[i].u16Left;
        rect.height = modle_output->result.stHdResult.pstTargetBoxes[i].u16Bottom - modle_output->result.stHdResult.pstTargetBoxes[i].u16Top;

        camera_ai_type_t out_detect_ai_type = support_modle->ai_type[modle_output->result.stHdResult.pstTargetBoxes[i].enLabel];

        camera_ai_index = get_ai_input_id( my_data, out_detect_ai_type & CAMERA_AI_DETECT_MASK);
        //log_error("out_detect_ai_type:%x, camera_ai_index: %d",out_detect_ai_type, camera_ai_index);

        if(camera_ai_index == -1)
        {
            continue;
        }

//        if( 0 == (my_data->ai_type & out_detect_ai_type) &&)
//        {
//            continue;
//        }

        // Store detection result
        if(out_detect_ai_type & my_data->ai_type)
        {
            int num_total = cdev->ai_output_result.num_total;

            if(num_total < CAMERA_AI_MAX_BOX){
                camera_ai_object_t *ai_object = &cdev->ai_output_result.ai_object[num_total];
                ai_object->rect = rect;
                ai_object->ai_type = out_detect_ai_type ;

                ai_object->detect.score = modle_output->result.stHdResult.pstTargetBoxes[i].u64Score;
                cdev->ai_output_result.num_total ++;
            }
        }

        camera_ai_type_t need_process_secondary_ai_type = hal_camera_ai_get_secondary_type(out_detect_ai_type & CAMERA_AI_DETECT_MASK);
        need_process_secondary_ai_type &= my_data->ai_type;

        //log_error("out_detect_ai_type:%x,need_process_secondary_ai_type:%x",out_detect_ai_type,need_process_secondary_ai_type);

        //一个detect可能有多个识别或分类
        while(need_process_secondary_ai_type)
        {
            camera_ai_type_t secondary_ai_type = need_process_secondary_ai_type & -need_process_secondary_ai_type;//取最低位；
            camera_ai_secondary_process(my_data,modle_id, device_id, 0, secondary_ai_type, rect);
            need_process_secondary_ai_type &= (need_process_secondary_ai_type - 1); //清理最低位
        }
    }

    return 0;
}

static int camera_ai_tile_overlap_process(void *hal_camera, SVP_OUTPUT_S *modle_output,
    int num_rows, int num_clos, int device_id, int modle_id)
{
    camera_rect_t    rect;
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;

    struct camera_device *cdev = my_data->cdev;

    // Clear previous results
    cdev->ai_output_result.num_total = 0;

    int final_modle_output_idx = num_rows * num_clos;
    const hal_camera_ai_modle_config_support_t *support_modle = my_data->ai_config.modle[modle_id].support_modle;
    SVP_HD_RESULT_S *result = &modle_output[final_modle_output_idx].result.stHdResult;

    int ret_count = 0;
    for (int i = 0; i < result->s32TotalNum; i ++)
    {
        camera_ai_type_t out_detect_ai_type = support_modle->ai_type[result->pstTargetBoxes[i].enLabel];
        int camera_ai_index = get_ai_input_id( my_data, out_detect_ai_type & CAMERA_AI_DETECT_MASK);
        if(camera_ai_index == -1)
        {
            continue;
        }

        //log_info("number[%d] out_detect_ai_type: %d, my_data->ai_type: %d, ret_count: %d", i, out_detect_ai_type, my_data->ai_type, ret_count);
        if(out_detect_ai_type & my_data->ai_type)
        {
            if(ret_count < CAMERA_AI_MAX_BOX)
            {
                ret_count ++;
            }
            else
            {
                break;
            }
        }
    }
    cdev->ai_output_result.num_total = ret_count;
    if (ret_count > 0)
    {
        int rect_count = 0;
        for (int i = 0; i < result->s32TotalNum; i ++)
        {
            camera_ai_type_t out_detect_ai_type = support_modle->ai_type[result->pstTargetBoxes[i].enLabel];
            int camera_ai_index = get_ai_input_id( my_data, out_detect_ai_type & CAMERA_AI_DETECT_MASK);
            if(camera_ai_index == -1)
            {
                continue;
            }

            if(out_detect_ai_type & my_data->ai_type)
            {
                if(rect_count < ret_count && rect_count < ret_count)
                {
                    camera_ai_object_t *ai_object = &cdev->ai_output_result.ai_object[rect_count ++];
                    rect.x = result->pstTargetBoxes[i].u16Left;
                    rect.y = result->pstTargetBoxes[i].u16Top;
                    rect.width = result->pstTargetBoxes[i].u16Right - result->pstTargetBoxes[i].u16Left;
                    rect.height = result->pstTargetBoxes[i].u16Bottom - result->pstTargetBoxes[i].u16Top;
                    ai_object->rect = rect;
                    ai_object->ai_type = out_detect_ai_type ;
                    ai_object->detect.score = result->pstTargetBoxes[i].u64Score;
                }
            }
        }
    }

    return 0;
}

int SAMPLE_VPSS_SaveYuvToFile(const char *path, int index, SVP_IMG_INFO_S *svp_img)
{
    FILE *fd = NULL;
    char *buf = NULL;
    int len = 0;
    char acFilePath[255] = {0};
    snprintf(acFilePath, sizeof(acFilePath), "%s/yuv_frame_%d.yuv", path, index);
    fd = fopen(acFilePath, "w+b");
    int total_len = svp_img->s32Width * svp_img->s32Height * 3;
    if (fd) {
        buf = (char *)svp_img->pvVirAddr;
        len = total_len;
        do {
            len -= fwrite(buf + total_len-len, 1, len, fd);
        } while (len != 0);

        fclose(fd);
    } else {
        log_error("open file [%s] failed!!", acFilePath);
        return -1;
    }
    return 0;
}

int SAMPLE_VPSS_SaveYuvToFile2(const char *path, int index, char *data, int length)
{
    FILE *fd = NULL;
    char *buf = NULL;
    int len = 0;
    char acFilePath[255] = {0};
    snprintf(acFilePath, sizeof(acFilePath), "%s/yuv_frame_%d.yuv", path, index);
    fd = fopen(acFilePath, "w+b");
    if (fd) {
        buf = data;
        len = length;
        do {
            len -= fwrite(buf + length-len, 1, len, fd);
        } while (len != 0);

        fclose(fd);
    } else {
        log_error("open file [%s] failed!!", acFilePath);
        return -1;
    }
    return 0;
}

int hal_camera_ai_process(void *hal_camera, int device_id, int channel_id,int stream_id)
{
    int ret = -1;
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    struct camera_device *cdev = my_data->cdev;


    VPSS_FRAME_INFO_S *pstFrame = my_data->hal_camera_device[device_id].channel[channel_id].pstFrame;
    hal_camera_channel_t *ai_hal_ch = &my_data->hal_camera_device[device_id].channel[channel_id];

    if (pstFrame == NULL || pstFrame->stFrame.pu8Data == NULL) {
        static unsigned s_ai_noframe_log;
        if (s_ai_noframe_log < 8u) {
            s_ai_noframe_log++;
            log_warn("[CAM_TRACE] ai_infer: skip null frame dev=%d ch=%d str=%d rot=%d",
                     device_id, channel_id, stream_id, ai_hal_ch->rotation);
        }
        return -1;
    }

    //save to channel 0(main channel)
    hal_camera_channel_t *hal_camera_channel = &my_data->hal_camera_device[device_id].channel[0];

    //log_error("hal_camera_ai_process[%d][%d][%d],num_modle:%d",device_id,channel_id,stream_id,my_data->ai_config.num_modle);
    stream_info_t *stream_info = &cdev->device_info[device_id].channel_info[channel_id].stream_info[stream_id];
    camera_stream_t *stream = stream_info->stream;

    VPSS_FRAME_INFO_S *main_pstFrame = hal_camera_channel->pstFrame;
    int stream_ai_type =  stream_info->ai_type;
    unsigned char *input;
    unsigned long in_phy_addr = 0;

    //log_info("my_data->ai_config.num_modle:%d",my_data->ai_config.num_modle);
    //check all modle
    for(int modle_id=0; modle_id< my_data->ai_config.num_modle; modle_id++)
    {
        //log_info("stream_ai_type:%x,ai_config.modle[modle_id].ai_type:%x, mask:%x",stream_ai_type, my_data->ai_config.modle[modle_id].ai_type \
            ,(CAMERA_AI_DETECT_MASK & ~CAMERA_AI_DETECT_CLASSIFY_MASK));
        int ai_id = my_data->ai_config.modle[modle_id].ai_id;

        //my_data->ai_config.modle[modle_id].ai_type表示已经add的ai_type，
        // stream_ai_type为本Stream需要的ai_type
        // CAMERA_AI_DETECT_MASK 只对DETECT的算法检测stream的检测，分类和识别在在camera_ai_process中再根据检测的结果再进行识别算法
        //stream_ai_type & 

        static int u32Index = 0;
        if(my_data->ai_config.modle[modle_id].ai_type & 
            (CAMERA_AI_DETECT_MASK & ~CAMERA_AI_DETECT_CLASSIFY_MASK))
        {
            //log_info("Overlap:%d, num_rows: %d, num_rows: %d",stream_info->ai_tile_overlap_enable, stream_info->ai_tile_overlap.num_rows, stream_info->ai_tile_overlap.num_cols);
            if(stream_info->ai_tile_overlap_enable == true)
            {
                /* Overlap mode depends on main-channel frame.
                 * Retry up to 10 times with 10ms interval to absorb timing jitter. */
                int main_retry = 0;
                for (; main_retry < 10; ++main_retry) {
                    main_pstFrame = hal_camera_channel->pstFrame;
                    if (main_pstFrame != NULL && main_pstFrame->stFrame.pu8Data != NULL) {
                        break;
                    }
                    usleep(10000);
                }
                if (main_retry >= 10) {
                    log_error("main frame null in overlap mode after retry: dev=%d ch=%d stream=%d",
                              device_id, channel_id, stream_id);
                    return AK_FAILED;
                }
                input = main_pstFrame->stFrame.pu8Data;
                in_phy_addr = main_pstFrame->u64PhyAddr;

                int num_clos = stream_info->ai_tile_overlap.num_cols;
                int num_rows = stream_info->ai_tile_overlap.num_rows;

                SVP_OUTPUT_S modle_output[num_clos * num_rows + 1];

                unsigned int iw = 0;
                unsigned int ih = 0;

                unsigned char *output;
                unsigned long out_phy_addr;
                unsigned int ow = 0;
                unsigned int oh = 0;
                camera_rect_t in_rect;

                VPSS_FRAME_INFO_S crop_pstFrame;
                //int crop_from_main_ch_len = stream->channel_config[0].width*stream->channel_config[0].height*3;
                int crop_from_main_ch_len = stream_info->ai_stream_tile_width*stream_info->ai_stream_tile_height*3;

                crop_pstFrame.stFrame.u32Len = crop_from_main_ch_len;
                crop_pstFrame.stFrame.u64Ts = main_pstFrame->stFrame.u64Ts;
                crop_pstFrame.stFrame.u64SeqNo = main_pstFrame->stFrame.u64SeqNo;
                crop_pstFrame.enOutputPixFmt = main_pstFrame->enOutputPixFmt;
                crop_pstFrame.pvMdInfo = main_pstFrame->pvMdInfo;
                const hal_camera_config_t *cfg = hal_camera_get_config();
                crop_pstFrame.stFrame.pu8Data = \
                    AK_BASE_MemDmaAlloc(cfg->ipu_module_id,  crop_from_main_ch_len);
                if (NULL == crop_pstFrame.stFrame.pu8Data)
                {
                    log_error("Alloc dma memory for main channel crop data failed, crop_from_main_ch_len: %d!", crop_from_main_ch_len);
                    return AK_FAILED;
                }
                ret = AK_BASE_MemDmaVaddr2Paddr(crop_pstFrame.stFrame.pu8Data, \
                        &crop_pstFrame.u64PhyAddr);
                if (AK_SUCCESS != ret)
                {
                    AK_BASE_MemDmaFree(crop_pstFrame.stFrame.pu8Data);
                    crop_pstFrame.stFrame.pu8Data = NULL;
                    log_error("Dma phy main channel crop addr error: %d!", ret);
                    return AK_FAILED;
                }
                pstFrame = &crop_pstFrame;
                in_rect.y = 0;
                in_rect.x = 0;
                in_rect.width = cdev->device_info[device_id].channel_info[0].stream_info[0].stream->channel_config[0].width;
                in_rect.height = cdev->device_info[device_id].channel_info[0].stream_info[0].stream->channel_config[0].height;


                iw = in_rect.width;
                ih = in_rect.height;

                //ow = stream->channel_config[0].width;
                //oh = stream->channel_config[0].height;
                ow = stream_info->ai_stream_tile_width;
                oh = stream_info->ai_stream_tile_height;

                output = crop_pstFrame.stFrame.pu8Data;
                out_phy_addr = crop_pstFrame.u64PhyAddr;

                //SAMPLE_VPSS_SaveYuvToFile2("/mnt/av200/pics", u32Index++, input, iw*ih*1.5);
                {
                    IPU_RECT_S ipu_rect;
                    camera_rect_to_ipu_rect(&in_rect, &ipu_rect);
                    if (AK_SUCCESS != hal_ipu_scale_crop_with_format(camera_ipu_get_queue_id(), in_phy_addr, iw, ih, &ipu_rect, 
                                                                     IPU_COLOR_FMT_SEMIPLANE_NV12, out_phy_addr, ow, oh, IPU_COLOR_FMT_PACKED_RGB24))
                    {
                        AK_BASE_MemDmaFree(crop_pstFrame.stFrame.pu8Data);
                        crop_pstFrame.stFrame.pu8Data = NULL;
                        log_warn("ipu scale crop failed!");
                        return AK_FAILED;
                    }
                }
                //SAMPLE_VPSS_SaveYuvToFile2("/mnt/av200/pics", u32Index++, pstFrame->stFrame.pu8Data, stream_info->ai_stream_tile_width*stream_info->ai_stream_tile_height*3);

                int data_len = my_data->ai_config.modle[modle_id].support_modle->width * \
                    my_data->ai_config.modle[modle_id].support_modle->height * 3;

                const hal_camera_config_t *cfg2 = hal_camera_get_config();
                my_data->ai_config.modle[modle_id].stInput.pvVirAddr = \
                    AK_BASE_MemDmaAlloc(cfg2->ipu_module_id, data_len);
                if (NULL == my_data->ai_config.modle[modle_id].stInput.pvVirAddr)
                {
                    AK_BASE_MemDmaFree(crop_pstFrame.stFrame.pu8Data);
                    crop_pstFrame.stFrame.pu8Data = NULL;
                    log_error("Alloc dma memory for full crop data failed, data_len: %d!", data_len);
                    return AK_FAILED;
                }

                ret = AK_BASE_MemDmaVaddr2Paddr(my_data->ai_config.modle[modle_id].stInput.pvVirAddr, \
                        &my_data->ai_config.modle[modle_id].stInput.u64PhyAddr);
                if (AK_SUCCESS != ret)
                {
                    AK_BASE_MemDmaFree(crop_pstFrame.stFrame.pu8Data);
                    crop_pstFrame.stFrame.pu8Data = NULL;

                    AK_BASE_MemDmaFree(my_data->ai_config.modle[modle_id].stInput.pvVirAddr);
                    my_data->ai_config.modle[modle_id].stInput.pvVirAddr = NULL;
                    log_error("Dma full crop phy addr error: %d!", ret);
                    return AK_FAILED;
                }

                for(int i=0; i < stream_info->ai_tile_overlap.num_rows; i++)
                {
                    for(int j=0; j < stream_info->ai_tile_overlap.num_cols; j++)
                    {
                        //get left and top coords
                        hal_camera_ai_calculate_tile_start_coords(i, j,
                                             stream->channel_config[0].width, stream->channel_config[0].height,
                                             stream_info->ai_tile_overlap.overlap_ratio,
                                             &in_rect.x, &in_rect.y);

                        //log_info("start_coords:[%d,%d]",in_rect.x, in_rect.y);

                        in_rect.width = stream->channel_config[0].width;
                        in_rect.height = stream->channel_config[0].height;

                        input = pstFrame->stFrame.pu8Data;
                        in_phy_addr = pstFrame->u64PhyAddr;

                        iw = stream_info->ai_stream_tile_width;
                        ih = stream_info->ai_stream_tile_height;

                        ow = stream->channel_config[0].width;
                        oh = stream->channel_config[0].height;

                        output = my_data->ai_config.modle[modle_id].stInput.pvVirAddr;
                        out_phy_addr = my_data->ai_config.modle[modle_id].stInput.u64PhyAddr;

                        //log_info("input:%x, output:%x",input,output);

                        {
                            IPU_RECT_S ipu_rect;
                            camera_rect_to_ipu_rect(&in_rect, &ipu_rect);
                            if (AK_SUCCESS != hal_ipu_scale_crop_with_format(camera_ipu_get_queue_id(), in_phy_addr, iw, ih, &ipu_rect, 
                                                                             IPU_COLOR_FMT_PACKED_RGB24, out_phy_addr, ow, oh, IPU_COLOR_FMT_PACKED_RGB24))
                            {
                                AK_BASE_MemDmaFree(crop_pstFrame.stFrame.pu8Data);
                                crop_pstFrame.stFrame.pu8Data = NULL;

                                AK_BASE_MemDmaFree(my_data->ai_config.modle[modle_id].stInput.pvVirAddr);
                                my_data->ai_config.modle[modle_id].stInput.pvVirAddr = NULL;
                                log_warn("ipu scale crop failed!");
                                return AK_FAILED;
                            }
                        }

                        my_data->ai_config.modle[modle_id].stInput.pvVirAddr   = output;
                        my_data->ai_config.modle[modle_id].stInput.u64PhyAddr  = out_phy_addr;
                        memset(&modle_output[i*num_clos + j], 0, sizeof(SVP_OUTPUT_S));
                        
                        //SAMPLE_VPSS_SaveYuvToFile("/mnt/av200/pics", u32Index++, &my_data->ai_config.modle[modle_id].stInput);
                        AK_MPI_SVP_Process(ai_id, &my_data->ai_config.modle[modle_id].stInput, &modle_output[i*num_clos + j]);
                    }
                }

                //把stream通道压成模型大小再出一个数据
                {
                    in_rect.y = 0;
                    in_rect.x = 0;
                    in_rect.width = stream_info->ai_stream_tile_width;
                    in_rect.height = stream_info->ai_stream_tile_height;

                    input = pstFrame->stFrame.pu8Data;
                    in_phy_addr = pstFrame->u64PhyAddr;

                    iw = stream_info->ai_stream_tile_width;
                    ih = stream_info->ai_stream_tile_height;

                    ow = stream->channel_config[0].width;
                    oh = stream->channel_config[0].height;

                    output = my_data->ai_config.modle[modle_id].stInput.pvVirAddr;
                    out_phy_addr = my_data->ai_config.modle[modle_id].stInput.u64PhyAddr;

                    my_data->ai_config.modle[modle_id].stInput.pvVirAddr   = output;
                    my_data->ai_config.modle[modle_id].stInput.u64PhyAddr  = out_phy_addr;

                    {
                        IPU_RECT_S ipu_rect;
                        camera_rect_to_ipu_rect(&in_rect, &ipu_rect);
                        if (AK_SUCCESS != hal_ipu_scale_crop_with_format(camera_ipu_get_queue_id(), in_phy_addr, iw, ih, &ipu_rect, 
                                                                         IPU_COLOR_FMT_PACKED_RGB24, out_phy_addr, ow, oh, IPU_COLOR_FMT_PACKED_RGB24))
                        {
                            AK_BASE_MemDmaFree(crop_pstFrame.stFrame.pu8Data);
                            crop_pstFrame.stFrame.pu8Data = NULL;

                            AK_BASE_MemDmaFree(my_data->ai_config.modle[modle_id].stInput.pvVirAddr);
                            my_data->ai_config.modle[modle_id].stInput.pvVirAddr = NULL;
                            log_warn("ipu scale crop failed!");
                            return AK_FAILED;
                        }
                    }

                    //SAMPLE_VPSS_SaveYuvToFile("/mnt/av200/pics", u32Index++, &my_data->ai_config.modle[modle_id].stInput);
                    AK_MPI_SVP_Process(ai_id, &my_data->ai_config.modle[modle_id].stInput, &modle_output[num_clos * num_rows]);
                }

//                log_error("[%d,%d], [%d,%d], [%d,%d],%f, [%x,%x]", \
                    stream->channel_config[0].width, stream->channel_config[0].height, \
                    stream_info->ai_stream_tile_width, stream_info->ai_stream_tile_height, \
                    num_rows, num_clos, stream_info->ai_tile_overlap.overlap_ratio, \
                    &modle_output[0]);
#ifdef DEBUG_HAL_AI_CAMERA_OVERLAP
                for (int i = 0; i < num_clos * num_rows + 1; i ++)
                {
                    for (int j = 0; j < modle_output[i].result.stHdResult.s32TotalNum; j ++)
                    {
                        log_info("model_output[%d][%d] rect[(%d,%d), (%d,%d)], score: %d, type: %d", \
                        i, j, modle_output[i].result.stHdResult.pstTargetBoxes[j].u16Left, \
                        modle_output[i].result.stHdResult.pstTargetBoxes[j].u16Top, \
                        modle_output[i].result.stHdResult.pstTargetBoxes[j].u16Right, \
                        modle_output[i].result.stHdResult.pstTargetBoxes[j].u16Bottom, \
                        modle_output[i].result.stHdResult.pstTargetBoxes[j].u64Score, \
                        modle_output[i].result.stHdResult.pstTargetBoxes[j].enLabel);
                    }
                }
#endif
                //去重，合并到full idx
                hal_camera_ai_tile_overlap_detection(
                                        stream_info->ai_stream_tile_width, stream_info->ai_stream_tile_height,
                                        stream->channel_config[0].width, stream->channel_config[0].height,
                                        num_rows, num_clos, stream_info->ai_tile_overlap.overlap_ratio,
                                        &modle_output[0]);
#ifdef DEBUG_HAL_AI_CAMERA_OVERLAP
                for (int j = 0; j < modle_output[num_clos * num_rows].result.stHdResult.s32TotalNum; j ++)
                {
                    log_info("after hal_camera_ai_tile_overlap_detection model_output[%d][%d] rect[(%d,%d), (%d,%d)], score: %d, type: %d", \
                    num_clos * num_rows, j, modle_output[num_clos * num_rows].result.stHdResult.pstTargetBoxes[j].u16Left, \
                    modle_output[num_clos * num_rows].result.stHdResult.pstTargetBoxes[j].u16Top, \
                    modle_output[num_clos * num_rows].result.stHdResult.pstTargetBoxes[j].u16Right, \
                    modle_output[num_clos * num_rows].result.stHdResult.pstTargetBoxes[j].u16Bottom, \
                    modle_output[num_clos * num_rows].result.stHdResult.pstTargetBoxes[j].u64Score, \
                    modle_output[num_clos * num_rows].result.stHdResult.pstTargetBoxes[j].enLabel);
                }
#endif
                camera_ai_tile_overlap_process(my_data, &modle_output[0], \
                    num_rows, num_clos,device_id, modle_id);

#ifdef DEBUG_HAL_AI_CAMERA_OVERLAP
                for (int i = 0; i < cdev->ai_output_result.num_total; i ++)
                {
                    log_info("far distance det output[%d/%d] type: %d, score: %d, rect[(%d,%d), (%d,%d)], ", \
                    i, cdev->ai_output_result.num_total,
                    cdev->ai_output_result.ai_object[i].ai_type,
                    cdev->ai_output_result.ai_object[i].detect.score,
                    cdev->ai_output_result.ai_object[i].rect.x,
                    cdev->ai_output_result.ai_object[i].rect.y,
                    cdev->ai_output_result.ai_object[i].rect.width,
                    cdev->ai_output_result.ai_object[i].rect.height);
                }
#endif
                for(int i=0; i < stream_info->ai_tile_overlap.num_rows; i++)
                {
                    for(int j=0; j < stream_info->ai_tile_overlap.num_cols; j++)
                    {
                        AK_MPI_SVP_Release(&modle_output[i*num_clos + j]);
                    }
                }
                AK_MPI_SVP_Release(&modle_output[num_rows * num_clos]);

                if (NULL != crop_pstFrame.stFrame.pu8Data)
                {
                    AK_BASE_MemDmaFree(crop_pstFrame.stFrame.pu8Data);
                    crop_pstFrame.stFrame.pu8Data = NULL;
                }

                if (NULL != my_data->ai_config.modle[modle_id].stInput.pvVirAddr)
                {
                    AK_BASE_MemDmaFree(my_data->ai_config.modle[modle_id].stInput.pvVirAddr);
                    my_data->ai_config.modle[modle_id].stInput.pvVirAddr = NULL;
                }
            }
            else
            {
                SVP_OUTPUT_S modle_output   = {0};

                my_data->ai_config.modle[modle_id].stInput.pvVirAddr   = pstFrame->stFrame.pu8Data;
                my_data->ai_config.modle[modle_id].stInput.u64PhyAddr  = pstFrame->u64PhyAddr;

                memset(&modle_output, 0x00, sizeof(SVP_OUTPUT_S));

                AK_MPI_SVP_Process(ai_id, &my_data->ai_config.modle[modle_id].stInput, &modle_output);

                if( modle_output.result.stHdResult.s32TotalNum > 0 )
                {
                    camera_ai_process(my_data, &modle_output, device_id, modle_id);
                }
                else
                {
                    cdev->ai_output_result.num_total = 0;
                }
                {
                    static unsigned s_ai_infer_log;
                    if (s_ai_infer_log < 20u) {
                        s_ai_infer_log++;
                        if (modle_output.result.stHdResult.s32TotalNum > 0 &&
                            cdev->ai_output_result.num_total > 0) {
                            log_info("[CAM_TRACE] ai_infer: dev=%d ch=%d rot=%d frame_rot=%d %ux%u svp_det=%d "
                                     "raw[0]=(%d,%d,%d,%d)",
                                     device_id, channel_id, ai_hal_ch->rotation,
                                     ai_hal_ch->frame_rotated ? 1 : 0,
                                     stream->channel_config[0].width, stream->channel_config[0].height,
                                     modle_output.result.stHdResult.s32TotalNum,
                                     cdev->ai_output_result.ai_object[0].rect.x,
                                     cdev->ai_output_result.ai_object[0].rect.y,
                                     cdev->ai_output_result.ai_object[0].rect.width,
                                     cdev->ai_output_result.ai_object[0].rect.height);
                        } else {
                            log_info("[CAM_TRACE] ai_infer: dev=%d ch=%d modle=%d rot=%d frame_rot=%d %ux%u svp_det=%d notify=%d",
                                     device_id, channel_id, modle_id, ai_hal_ch->rotation,
                                     ai_hal_ch->frame_rotated ? 1 : 0,
                                     stream->channel_config[0].width, stream->channel_config[0].height,
                                     modle_output.result.stHdResult.s32TotalNum,
                                     cdev->ai_output_result.num_total);
                        }
                    }
                }
                AK_MPI_SVP_Release(&modle_output);
#ifdef DEBUG_HAL_AI_CAMERA_OVERLAP
                for (int i = 0; i < cdev->ai_output_result.num_total; i ++)
                {
                    log_info("normal det output[%d/%d] type: %d, score: %d, rect[(%d,%d), (%d,%d)], ", \
                    i, cdev->ai_output_result.num_total,
                    cdev->ai_output_result.ai_object[i].ai_type,
                    cdev->ai_output_result.ai_object[i].detect.score,
                    cdev->ai_output_result.ai_object[i].rect.x,
                    cdev->ai_output_result.ai_object[i].rect.y,
                    cdev->ai_output_result.ai_object[i].rect.width,
                    cdev->ai_output_result.ai_object[i].rect.height);
                }
#endif
            }

            if (cdev->callback_ops != NULL && cdev->callback_ops->notify != NULL)
            {
                cdev->ai_output_result.u64Ts = pstFrame->stFrame.u64Ts;
                cdev->callback_ops->notify(cdev->callback_ops, &cdev->ai_output_result);
            }

        }
    }

    //log_info("hal_camera_ai_process[%d][%d][%d],num_modle:%d",device_id,channel_id,stream_id,my_data->ai_config.num_modle);

    return 0;
}

static void* camera_process_ai_thread_function(void* arg) {
    hal_camera_ai_thread_info_t *ai_thread_info = (hal_camera_ai_thread_info_t*)arg;

    camera_device_t *cdev = ai_thread_info->cdev;
    camera_stream_t *stream = ai_thread_info->stream;
    stream_info_t *stream_info = stream->priv[stream->priv_id];

    int device_id = stream_info->device_id;
    int channel_id = stream_info->channel_id;
    int stream_id = stream_info->stream_id;

    camera_ai_type_t ai_type;    //only used for ai request template
    camera_status_type_t ai_status;
    int exit_reason = 0;


    while((stream_info->ai_type & CAMERA_AI_MAIN_MASK) != CAMERA_AI_NONE)
    {
//        log_info("stream_info->ai_status: %d   !!!!!!!!",stream_info->ai_status);
        if (cdev == NULL) {
            exit_reason = 1; /* cdev null */
            break;
        }
        if (__sync_add_and_fetch(&cdev->closing, 0) != 0) {
            exit_reason = 2; /* device closing */
            break;
        }
        if (cdev->hal_camera == NULL) {
            exit_reason = 3; /* hal already released */
            break;
        }

        if(stream_info->ai_status == CAMERA_STATUS_READY)
        {
            stream_info->ai_status = CAMERA_STATUS_BUSY;
            int ai_ret = hal_camera_ai_process(cdev->hal_camera, device_id, channel_id,stream_id);
            (void)ai_ret;

            if( stream_info->ai_status == CAMERA_STATUS_BUSY )
            {
                stream_info->ai_status = CAMERA_STATUS_ENABLE;
            }
        }
       else
       {
        usleep(10000);
       }

    }
    log_debug("ai thread exit: dev=%d ch=%d str=%d", device_id, channel_id, stream_id);
    return NULL;
}

