// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Anyka(GuangZhou) Microelectronics Co., Ltd.
 * @VERSION  1.0
 *
 * AnyCloud39AV100_V2: AI path uses ak_svp2 natively (no SVP compat layer).
 * Reference PDK platform/sample/svp2/ak_svp2_sample.c (create_chn, process, conf path).
 * hal_camera_api / camera_hw call surface matches AnyCloud39AV200.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>


#include "ak_common.h"
#include "ak_log.h"
#include "ak_common_video.h"
#include "ak_thread.h"
#include "ak_mem.h"
#include "ak_vi.h"
#include "ak_vpss.h"

#include "ak_svp2.h"
#include "ak_npu.h"
#include "ak_osd.h"

#include "camera.h"
#include "log.h"

#include "hal_camera.h"
#include "hal_camera_chip_config.h"
#include "hal_camera_config.h"

/* AnyCloud39AV100_V2: camera HAL does not support IPU-dependent paths. */
#define HAL_CAMERA_HAS_IPU 0

#if HAL_CAMERA_HAS_IPU
static int s_camera_ipu_queue_id = -1;
#endif

#include "../../camera_hw.h"
#include "../../hal_camera_api.h"

#define LOG_TAG "HAL CAMERA AI"

static void* camera_process_ai_thread_function(void* arg) ;
/* Track whether camera module owns the current NPU init. */
static int s_camera_npu_owned = 0;


hal_camera_ai_config_t camera_ai =
{
  .camera_ai_type_flag = CAMERA_AI_NONE,
};

int  hal_camera_ai_init(void *hal_camera)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    int ret = 0;

    log_info("hal_camera_ai_init, NPU init");

    ret = ak_npu_init();

    if (ret != AK_SUCCESS) {
        /*
         * 其它模块可能已初始化 NPU；非 0 时仍尝试使用已有 NPU 上下文。
         */
        s_camera_npu_owned = 0;
        log_warn("ak_npu_init returned %d, continue with shared/existing NPU context", ret);
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
        log_info("ak_npu_deinit");
        (void)ak_npu_deinit();
        s_camera_npu_owned = 0;
    } else {
        log_info("skip ak_npu_deinit (NPU not owned by camera module)");
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
            log_info("register ai id=%d", i);
            return i;
        }
    }

    log_info("register ai id failed");
    return -1;
}

int camera_ai_unregister_id( void *hal_camera, int ai_id)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;

    my_data->ai_id_status[ai_id] = 0;

    return 0;
}


/*
 * AI tile-overlap is not supported on AnyCloud39AV100_V2 (needs IPU crop/scale,
 * HAL_CAMERA_HAS_AI_TILE_OVERLAP=0). Keep these hooks as no-ops so the shared
 * camera_hw / hal_camera_api contract still links.
 */
int hal_camera_ai_tile_overlap_enable(void *hal_camera)
{
    (void)hal_camera;
    log_warn("ai_tile_overlap not supported on AnyCloud39AV100_V2");
    return -1;
}

int hal_camera_ai_tile_overlap_disable(void *hal_camera)
{
    (void)hal_camera;
    return 0;
}

int hal_camera_ai_tile_overlap_calculate_input_size(int tile_width, int tile_height,
                                                    int num_rows, int num_cols,
                                                    float overlap_ratio,
                                                    int *input_width, int *input_height)
{
    (void)tile_width;
    (void)tile_height;
    (void)num_rows;
    (void)num_cols;
    (void)overlap_ratio;
    (void)input_width;
    (void)input_height;
    return -1;
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

/*
 * SVP2 channel helpers.
 *
 * AV100_V2 talks to ak_svp2 natively. These helpers keep shutdown/error handling
 * local to this module and deep-copy any result buffers that are owned by the NPU
 * so callers can safely use results after ak_svp2_release().
 */
static int ai_svp_create_chn(int ai_id, const AK_SVP2_CHN_ATTR_T *attr,
                             AK_SVP2_CONF_INFO *conf_info)
{
    int ret;

    if (!attr || !conf_info) {
        log_error("svp2 create_chn invalid args attr=%p conf=%p", attr, conf_info);
        return AK_FAILED;
    }
    if (attr->model_num <= 0 || attr->model_num > 8) {
        log_error("svp2 create_chn invalid model_num=%d", attr->model_num);
        return AK_FAILED;
    }
    if (conf_info->conf_mode == 0) {
        if (!conf_info->conf_type.conf || conf_info->conf_type.conf[0] == '\0') {
            log_error("svp2 create_chn empty conf path");
            return AK_FAILED;
        }
        if (access(conf_info->conf_type.conf, F_OK) != 0) {
            log_error("svp2 create_chn conf not readable: %s", conf_info->conf_type.conf);
        }
    }

    ret = ak_svp2_create_chn(ai_id, attr, conf_info);
    log_info("ak_svp2_create_chn ai_id=%d model_num=%d model0=%d ret=%d",
             ai_id, attr->model_num, (int)attr->model_attr[0].model_type, ret);
    return ret;
}

/* Deep-copy detect boxes so the caller can read them after ak_svp2_release(). */
static void ai_svp_copy_hd_result(const AK_SVP2_HD_RESULT_T *src, AK_SVP2_HD_RESULT_T *dst)
{
    int n = src->total_num;

    dst->total_num = 0;
    dst->target_boxes = NULL;
    if (n <= 0 || !src->target_boxes) {
        return;
    }
    dst->target_boxes = (AK_SVP2_RECT_T *)ak_mem_alloc(MODULE_ID_APP,
                                                      (unsigned int)(n * sizeof(AK_SVP2_RECT_T)));
    if (!dst->target_boxes) {
        return;
    }
    memcpy(dst->target_boxes, src->target_boxes, (size_t)n * sizeof(AK_SVP2_RECT_T));
    dst->total_num = n;
}

/*
 * Run one inference and copy the result into a caller-owned output. The NPU
 * buffer is released here, so the caller must free the copy via ai_svp_release().
 */
static int ai_svp_process(int ai_id, AK_SVP2_IMG_INFO_T *input, AK_SVP2_OUTPUT_T *output)
{
    AK_SVP2_OUTPUT_T raw;
    int ret;

    if (!input || !output) {
        return AK_FAILED;
    }
    memset(&raw, 0, sizeof(raw));
    memset(output, 0, sizeof(*output));

    ret = ak_svp2_process(ai_id, input, &raw);
    if (ret != AK_SUCCESS) {
        return ret;
    }

    output->model_type = raw.model_type;
    output->model_index = raw.model_index;

    switch ((AK_SVP2_MODEL_E)raw.model_type) {
    case AK_SVP2_MODEL_FACE_RECOGNIZE:
    case AK_SVP2_MODEL_0X0A020002: {
        unsigned int len = raw.result.fr_result.data_len;
        output->result.fr_result.data = NULL;
        output->result.fr_result.data_len = 0;
        if (len > 0 && raw.result.fr_result.data) {
            signed char *buf = (signed char *)ak_mem_alloc(MODULE_ID_APP, len);
            if (buf) {
                memcpy(buf, raw.result.fr_result.data, len);
                output->result.fr_result.data = buf;
                output->result.fr_result.data_len = len;
            } else {
                log_warn("FR result alloc failed (len=%u), drop result", len);
            }
        }
        break;
    }
    case AK_SVP2_MODEL_FAS:
    case AK_SVP2_MODEL_0X0A010005:
    case AK_SVP2_MODEL_0X0A010006:
        output->result.fas_result = raw.result.fas_result;
        break;
    case AK_SVP2_MODEL_0X0A010002:
    case AK_SVP2_MODEL_0X0A010004:
        output->result.gesture_result = raw.result.gesture_result;
        break;
    default:
        ai_svp_copy_hd_result(&raw.result.hd_result, &output->result.hd_result);
        break;
    }

    (void)ak_svp2_release(&raw);
    return AK_SUCCESS;
}

/* Free the copy produced by ai_svp_process(). result is a union: one live pointer. */
static void ai_svp_release(AK_SVP2_OUTPUT_T *output)
{
    if (!output) {
        return;
    }
    if (output->result.hd_result.target_boxes) {
        ak_mem_free(output->result.hd_result.target_boxes);
        output->result.hd_result.target_boxes = NULL;
        output->result.hd_result.total_num = 0;
    }
    if (output->result.fr_result.data && output->result.fr_result.data_len > 0) {
        ak_mem_free(output->result.fr_result.data);
        output->result.fr_result.data = NULL;
        output->result.fr_result.data_len = 0;
    }
    memset(output, 0, sizeof(*output));
}

/**
 * @brief Close SVP2 channel.
 * @param hal_camera Pointer to HAL camera data.
 * @param ai_id AI channel ID to disable.
 * @return 0 on success.
 */
int  hal_camera_ai_disable_channel(void *hal_camera, int ai_id)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    int ret = 0;

    ret = ak_svp2_destroy_chn(ai_id);
    if (ret != AK_SUCCESS) {
        log_error("Destroy SVP chn failed ret=%d", ret);
        return ret;
    }

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

    log_info("hal_camera_ai_enable_channel, modle_id:%d, ai_input_id:%d",modle_id, ai_input_id);

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

    /* init the svp2 attr (header + model_num * model_attr, see ak_svp2_sample.c) */
    AK_S32 svp_len = sizeof(AK_SVP2_CHN_ATTR_T) + num_model*sizeof(AK_SVP2_MODEL_ATTR_T);
    modle->attr = (AK_SVP2_CHN_ATTR_T *)ak_mem_alloc(MODULE_ID_APP, svp_len);
    if (modle->attr == NULL) {
        log_error("alloc svp2 attr failed, len=%d", svp_len);
        return AK_FAILED;
    }
    memset(modle->attr, 0, svp_len);
    modle->attr->model_num = num_model;
    modle->attr->model_attr[0].model_type = support_modle->enModelType;

    if(support_modle->ai_type[0] & (CAMERA_AI_DETECT_MASK & ~CAMERA_AI_DETECT_CLASSIFY_MASK))
    {
        int cls = ai_input->classify_threshold;
        int iou = ai_input->iou_threshold;
        if (support_modle->classify_threshold >= 0) {
            cls = support_modle->classify_threshold;
        }
        if (support_modle->iou_threshold >= 0) {
            iou = support_modle->iou_threshold;
        }
        modle->attr->model_attr[0].model_param.param.hd_param.classify_threshold = cls;
        /* set the IoU threshold, range [3-10], default is 3*/
        modle->attr->model_attr[0].model_param.param.hd_param.IoU_threshold = iou;
        modle->attr->model_attr[0].model_param.param.hd_param.target_type =
            (support_modle->target_type >= 0) ? (AK_SVP2_TARGET_TYPE_E)support_modle->target_type : AK_SVP2_HUMAN_SHAPE;
    }
    else
    {
        modle->attr->model_attr[0].model_param.param.gesture_param.confidence_threshold = ai_input->classify_threshold;
    }

    memset(&modle->conf_info, 0, sizeof(AK_SVP2_CONF_INFO));

    modle->conf_info.conf_type.conf = (char *)support_modle->modle_name;
    modle->conf_info.conf_mode = 0;  //0 for modle

    modle->stInput.img_type             =  support_modle->enImgType;
    modle->stInput.width                =  support_modle->width;
    modle->stInput.height               =  support_modle->height;
    modle->stInput.pos_info.left        = 0;
    modle->stInput.pos_info.top         = 0;
    modle->stInput.pos_info.width       = support_modle->width;
    modle->stInput.pos_info.height      = support_modle->height;


    log_info("env_mode:%d, model_type:%d", \
        modle->attr->model_attr[0].env_mode, \
        modle->attr->model_attr[0].model_type);

    log_info("classify_threshold:%d, IoU_threshold:%d", \
        modle->attr->model_attr[0].model_param.param.hd_param.classify_threshold, \
        modle->attr->model_attr[0].model_param.param.hd_param.IoU_threshold
    );

    log_info("conf:%s", \
        modle->conf_info.conf_type.conf
    );

    log_info("svp2 create: ai_id=%d model_num=%d model_type=%d img_type=%d %dx%d conf=%p(%s) attr=%p",
             ai_id,
             modle->attr ? modle->attr->model_num : -1,
             modle->attr ? (int)modle->attr->model_attr[0].model_type : -1,
             (int)modle->stInput.img_type,
             (int)modle->stInput.width, (int)modle->stInput.height,
             modle->conf_info.conf_type.conf,
             modle->conf_info.conf_type.conf ? modle->conf_info.conf_type.conf : "(null)",
             modle->attr);

    log_info("ai_id[%d] enabled (svp2 create_chn)", ai_id);

    ret = ai_svp_create_chn(ai_id, modle->attr, &modle->conf_info);
    if(ret != AK_SUCCESS)
    {
        log_error("Create SVP chn failed");
        if(modle->attr != NULL) {
            ak_mem_free(modle->attr);
            modle->attr = NULL;
        }
        return ret;
    }

    if(modle->attr != NULL) {
        ak_mem_free(modle->attr);
        modle->attr = NULL;
    }

    log_info("create chn pass");

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
            ak_mem_dma_alloc(cfg->ipu_module_id,  data_len);
            {
                unsigned long pa = 0;
                if (my_data->ai_config.modle[modle_id].secondary.output_buffer &&
                    ak_mem_dma_vaddr2paddr(my_data->ai_config.modle[modle_id].secondary.output_buffer, &pa) == AK_SUCCESS) {
                    my_data->ai_config.modle[modle_id].secondary.output_buffer_phy_addr = (uint64_t)pa;
                } else {
                    my_data->ai_config.modle[modle_id].secondary.output_buffer_phy_addr = 0;
                }
            }

            log_info("model_id=%d secondary registered", modle_id);
        }
        else
        {
            my_data->ai_config.modle[modle_id].detect.streams = cdev->ai_input[ai_input_id].stream;

            log_info("model_id=%d detect registered", modle_id);
        }

        hal_camera_ai_enable_channel(hal_camera, modle_id , ai_input_id);
        log_info("%s", ak_svp2_get_version());

        my_data->ai_config.num_modle++;
    }
    /* Register ai_type to model again */
    my_data->ai_config.modle[modle_id].ai_type |= (ai_type & CAMERA_AI_MAIN_MASK);

    return 0;
}


int hal_camera_ai_add(void *hal_camera, camera_ai_type_t ai_type)
{
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
    camera_stream_t *stream = cdev->ai_input[ai_input_id].stream;
    stream_info_t *stream_info = stream->priv[stream->priv_id];

    if(ai_input_id == -1)
    {
        log_error("ai_type:%d don't set config, plz call set_parameters ", ai_type);
        return -1;
    }

    int num_model_support = 0;
    const hal_camera_ai_modle_config_cfg_t *model_table =
        hal_camera_get_ai_model_table(&num_model_support);

    AK_SVP2_IMG_TYPE_E format;

    switch (stream->format)
    {
        case SYSTEM_PIXEL_FORMAT_YCBCR_420_SP:
            format = AK_SVP2_IMG_YUV420SP;
        break;
        case SYSTEM_PIXEL_FORMAT_RGB_LINEINTL:
            format = AK_SVP2_IMG_RGB_LI;
        break;
        case SYSTEM_PIXEL_FORMAT_RGB_HWC:
            /*
             * AV100_V2 VI only exposes RGB_LINEINTL. Upper layers may request RGB_HWC/CHW,
             * but VI will still produce RGB_LINEINTL. Treat them as RGB_LI for model match.
             */
            format = AK_SVP2_IMG_RGB_LI;
        break;
        case SYSTEM_PIXEL_FORMAT_RGB_CHW:
            format = AK_SVP2_IMG_RGB_LI;
        break;
        default: 
            log_error("unsupported ai format=%d", stream->format);
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

            /* Check if width/height and AK_SVP2_IMG_TYPE_E match */
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
#if HAL_CAMERA_HAS_IPU
                if (s_camera_ipu_queue_id < 0) {
                    int queue_id = hal_camera_get_config()->ipu_que_id;
                    if (hal_ipu_open() == 0) {
                        if (hal_ipu_alloc_queue(&queue_id) != 0 &&
                            hal_ipu_get_default_queue("camera", &queue_id) != 0) {
                            hal_ipu_close();
                        } else {
                            s_camera_ipu_queue_id = queue_id;
                        }
                    }
                }
#endif
                break;
            }
        }
    }

    if( regester_flag == 0)
    {
        log_error("unsupported ai model type=0x%x", ai_type);
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
    }

    return AK_SUCCESS;
}

void hal_camera_ai_shutdown_all(void *hal_camera)
{
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    struct camera_device *cdev;

    if (my_data == NULL || my_data->cdev == NULL)
        return;

    cdev = my_data->cdev;
    /*
     * AnyCloud39AV100_V2 has no per-channel SVP destroy (AK_MPI_SVP_DestroyChn is
     * AV200-only); AI/NPU teardown is done wholesale by hal_camera_ai_uninit()
     * (ak_npu_deinit) at the end of this function. Here we only reset per-model state.
     */
    for (int m = 0; m < my_data->ai_config.num_modle; m++) {
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
                if (st->ai_thread_id != 0) {
                    pthread_join(st->ai_thread_id, NULL);
                    st->ai_thread_id = 0;
                }
            }
        }
    }
    hal_camera_ai_uninit(my_data);
    my_data->ai_config.status = CAMERA_STATUS_UNINITIALIZE;
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
            pthread_join(stream_info->ai_thread_id, NULL);
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
                ak_mem_dma_free(my_data->ai_config.modle[modle_id].secondary.output_buffer);
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

    unsigned char *input = my_data->ai_config.modle[detect_modle_id].stInput.vir_addr;
    unsigned long in_phy_addr = my_data->ai_config.modle[detect_modle_id].stInput.phy_addr;
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

#if !HAL_CAMERA_HAS_IPU
    (void)input;
    (void)in_phy_addr;
    (void)iw;
    (void)ih;
    (void)in_rect;
    (void)out_phy_addr;
    (void)ow;
    (void)oh;
    log_warn("camera_ai_secondary_process: IPU is not supported on this chip, skip secondary process");
    return AK_FAILED;
#else
    //hal_camera_ipu_scale_crop(input, in_phy_addr, iw, ih, in_rect, output, out_phy_addr, ow, oh);
    {
        IPU_RECT_S ipu_rect;
        camera_rect_to_ipu_rect(&in_rect, &ipu_rect);
        hal_ipu_scale_crop_with_format(camera_ipu_get_queue_id(), in_phy_addr, iw, ih, &ipu_rect,
                                      IPU_COLOR_FMT_PACKED_RGB24, out_phy_addr, ow, oh, IPU_COLOR_FMT_PACKED_RGB24);
    }
#endif

    my_data->ai_config.modle[modle_id].stInput.vir_addr = output;
    my_data->ai_config.modle[modle_id].stInput.phy_addr = out_phy_addr;

    AK_SVP2_OUTPUT_T modle_output   = {0};

    memset(&modle_output, 0, sizeof(AK_SVP2_OUTPUT_T));


    ret = ai_svp_process(ai_id, &my_data->ai_config.modle[modle_id].stInput, &modle_output);

    /* Store detection result */
    int num_total = cdev->ai_output_result.num_total;

    const hal_camera_ai_modle_config_support_t *support_modle = my_data->ai_config.modle[modle_id].support_modle;

    /* Supports AK_SVP2 FAS/gesture/FR results */
    camera_ai_object_t *ai_object = &cdev->ai_output_result.ai_object[num_total];
    ai_object->rect = in_rect;

    int object_label;

    if(num_total < CAMERA_AI_MAX_BOX){
        if(secondary_ai_type & CAMERA_AI_DETECT_CLASSIFY_MASK)
        {
            ai_object->detect.score = modle_output.result.gesture_result.confidence;
            object_label = modle_output.result.gesture_result.label;
        }
        else if(secondary_ai_type & CAMERA_AI_RECOGNIZE_MASK)
        {
            ai_object->recognize.data_len = modle_output.result.fr_result.data_len;
            ai_object->recognize.data = (int16_t *)modle_output.result.fr_result.data;
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


static int camera_ai_process(struct hal_camera_data *my_data, AK_SVP2_OUTPUT_T *modle_output,
        int device_id, int modle_id)
{
    camera_rect_t    rect;
    int camera_ai_index;
    struct camera_device *cdev = my_data->cdev;

    const hal_camera_ai_modle_config_support_t *support_modle = my_data->ai_config.modle[modle_id].support_modle;

    // Clear previous results
    cdev->ai_output_result.num_total = 0;

    for(int i = 0; i < modle_output->result.hd_result.total_num; i++)
    {
        rect.x = modle_output->result.hd_result.target_boxes[i].left;
        rect.y = modle_output->result.hd_result.target_boxes[i].top;
        rect.width = modle_output->result.hd_result.target_boxes[i].right - modle_output->result.hd_result.target_boxes[i].left;
        rect.height = modle_output->result.hd_result.target_boxes[i].bottom - modle_output->result.hd_result.target_boxes[i].top;

        camera_ai_type_t out_detect_ai_type = support_modle->ai_type[modle_output->result.hd_result.target_boxes[i].label];

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

                ai_object->detect.score = modle_output->result.hd_result.target_boxes[i].score;
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

int hal_camera_ai_process(void *hal_camera, int device_id, int channel_id,int stream_id)
{
    int ret = -1;
    struct hal_camera_data *my_data = (struct hal_camera_data *)hal_camera;
    struct camera_device *cdev = my_data->cdev;


    VPSS_FRAME_INFO_S *pstFrame = my_data->hal_camera_device[device_id].channel[channel_id].pstFrame;

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
            (void)stream_info->ai_tile_overlap_enable;
            {
                AK_SVP2_OUTPUT_T modle_output   = {0};

                my_data->ai_config.modle[modle_id].stInput.vir_addr   = pstFrame->stFrame.pu8Data;
                my_data->ai_config.modle[modle_id].stInput.phy_addr   = pstFrame->u64PhyAddr;
                my_data->ai_config.modle[modle_id].stInput.img_type =
                    hal_camera_svp_img_from_vi_data_type(pstFrame->enOutputPixFmt);

                memset(&modle_output, 0x00, sizeof(AK_SVP2_OUTPUT_T));

                ai_svp_process(ai_id, &my_data->ai_config.modle[modle_id].stInput, &modle_output);

                if( modle_output.result.hd_result.total_num > 0 )
                {
                    camera_ai_process(my_data, &modle_output, device_id, modle_id);
                }
                else
                {
                    cdev->ai_output_result.num_total = 0;
                }
                ai_svp_release(&modle_output);
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
