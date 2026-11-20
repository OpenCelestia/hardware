// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/gpu/hal/Sky37Ev2.
 *
 * TDE (2D Graphics Engine) HAL implementation for Sky37Ev2 platform.
 * - Implements GPU operations using TDE hardware acceleration.
 * - Provides image copy, scale, transform, blend and fill operations.
 */

#include "gpu.h"

#include <linux/kernel.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <sys/ioctl.h>

#include "log.h"
#include "ak_common.h"
#include "ak_common_graphics.h"
#include "ak_tde.h"

#define LOG_TAG "HAL TDE"

/**
 * @brief   Convert color format which defined in pixel_format_t to AK_GP_FORMAT.
 * @param   [IN]PixelFormat: The pixel format defined in pixel_format_t.
 * @return  int
 * @retval  color format which defined in AK_GP_FORMAT.
*/
/**
 * @brief Convert pixel format to TDE format.
 * @param PixelFormat  Pixel format defined in pixel_format_t.
 * @return TDE format defined in AK_GP_FORMAT, or -1 if unsupported.
 */
static int tde_format_convert(int PixelFormat)
{
    AK_GP_FORMAT format = -1;

    switch (PixelFormat)
    {
        case PIXEL_FORMAT_FULLPLANE_YV12:
            format = GP_FORMAT_YUV420P;
            break;
        case PIXEL_FORMAT_SEMIPLANE_NV21:
            format = GP_FORMAT_YUV420SP;
            break;
        case PIXEL_FORMAT_ARGB32:
            format = GP_FORMAT_ARGB8888;
            break;
        case PIXEL_FORMAT_ABGR32:
            format = GP_FORMAT_ABGR8888;
            break;
        case PIXEL_FORMAT_BGRA32:
            format = GP_FORMAT_BGRA8888;
            break;
        case PIXEL_FORMAT_RGBA32:
            format = GP_FORMAT_RGBA8888;
            break;
        case PIXEL_FORMAT_RGB24:
            format = GP_FORMAT_RGB888;
            break;
        case PIXEL_FORMAT_BGR24:
            format = GP_FORMAT_BGR888;
            break;
        case PIXEL_FORMAT_RGB565:
            format = GP_FORMAT_RGB565;
            break;
        case PIXEL_FORMAT_BGR565:
            format = GP_FORMAT_BGR565;
            break;

        default:
            break;
    }

    return format;
}

/**
 * @brief Convert transform value to TDE rotation value.
 * @param   [IN]transform: transform value which defined in transform_t.
 * @return  AK_GP_ROTATE
 * @retval  angle of rotation which defined in AK_GP_ROTATE.
*/
/**
 * @brief Convert transform value to TDE rotation value.
 * @param transform  Transform value defined in transform_t.
 * @return Rotation angle defined in AK_GP_ROTATE.
 */
static AK_GP_ROTATE tde_transform_convert(transform_t transform)
{
    AK_GP_ROTATE rotate = 0;

    switch (transform)
    {
        case TRANSFORM_ROTATE_90:
            rotate = AK_GP_ROTATE_90;
            break;
        case TRANSFORM_ROTATE_180:
            rotate = AK_GP_ROTATE_180;
            break;
        case TRANSFORM_ROTATE_270:
            rotate = AK_GP_ROTATE_270;
            break;

        default:
            break;
    }

    return rotate;
}

/**
 * @brief   set image information for tde by struct image_desc.
 * @param   [IN]image: image information.
 * @param   [IN]layer: struct ak_tde_layer.
 * @return  int
 * @retval  AK_SUCCESS: success; AK_FAILED: failed.
*/
static int tde_layer_set(struct image_desc *image, struct ak_tde_layer *layer)
{
    layer->width = image->u32Width;
    layer->height = image->u32Height;
    layer->pos_width = image->rect.width;
    layer->pos_height = image->rect.height;
    layer->pos_left = image->rect.x;
    layer->pos_top = image->rect.y;
    layer->phyaddr = image->paddr;
    layer->format_param = tde_format_convert(image->format);

    return AK_SUCCESS;
}

/**
 * @brief   check information of input image.
 * @param   [IN]pSrcImage: information of input image.
 * @return  int
 * @retval  AK_SUCCESS: success; AK_FAILED: failed.
*/
static int tde_check_input(struct image_desc *pSrcImage)
{
    switch (tde_format_convert(pSrcImage->format))
    {
        case GP_FORMAT_RGB565:
        case GP_FORMAT_RGB888:
        case GP_FORMAT_BGR565:
        case GP_FORMAT_BGR888:
        case GP_FORMAT_YUV420P:
        case GP_FORMAT_YUV420SP:
        case GP_FORMAT_ARGB8888:
        case GP_FORMAT_RGBA8888:
        case GP_FORMAT_ABGR8888:
        case GP_FORMAT_BGRA8888:
        case GP_FORMAT_TILED32X4:
            break;

        default:
            log_error("Unsupported color format of output image");
            return AK_FAILED;
            break;
    }

    return AK_SUCCESS;
}

/**
 * @brief Check information of output image.
 * @param   [IN]pDstImage: information of output image.
 * @return  int
 * @retval  AK_SUCCESS: success; AK_FAILED: failed.
*/
static int tde_check_output(struct image_desc *pDstImage)
{
    switch (pDstImage->format)
    {
        case SYSTEM_PIXEL_FORMAT_FULLPLANE_YV12:
        case SYSTEM_PIXEL_FORMAT_SEMIPLANE_NV21:
        case SYSTEM_PIXEL_FORMAT_RGB24:
        case SYSTEM_PIXEL_FORMAT_BGR24:
        case SYSTEM_PIXEL_FORMAT_RGB565:
        case SYSTEM_PIXEL_FORMAT_BGR565:
            break;

        default:
            log_error("Unsupported color format of output image");
            return AK_FAILED;
            break;
    }

    return AK_SUCCESS;
}

/**
 * @brief   copy image.
 * @param   [IN]device: the device context.
 * @param   [IN]pSrcImage: descriptor of source image.
 * @param   [IN]pDstImage: descriptor of destination image
 * @return  int
 * @retval  AK_SUCCESS: success; AK_FAILED: failed.
*/
static int hw_tde_image_copy(struct gpu_device *device, struct image_desc *pSrcImage, struct image_desc *pDstImage)
{
    if ((NULL == device) || (NULL == pSrcImage) || (NULL == pDstImage))
    {
        log_error("Invalid parameter");
        return AK_FAILED;
    }

    if (AK_SUCCESS != tde_check_input(pDstImage))
        return AK_FAILED;

    if (AK_SUCCESS != tde_check_output(pDstImage))
        return AK_FAILED;

    int ret = AK_FAILED;

    struct ak_tde_layer SrcLayer = {0};
    struct ak_tde_layer DstLayer = {0};

    tde_layer_set(pSrcImage, &SrcLayer);
    tde_layer_set(pDstImage, &DstLayer);

    ret = ak_tde_opt_blit(&SrcLayer, &DstLayer);

    return ret;
}

/**
 * @brief   scale image.
 * @param   [IN]device: the device context.
 * @param   [IN]pSrcImage: descriptor of source image.
 * @param   [IN]pDstImage: descriptor of destination image
 * @return  int
 * @retval  AK_SUCCESS: success; AK_FAILED: failed.
*/
static int hw_tde_image_scale(struct gpu_device *device, struct image_desc *pSrcImage, struct image_desc *pDstImage)
{
    if ((NULL == device) || (NULL == pSrcImage) || (NULL == pDstImage))
    {
        log_error("Invalid parameter");
        return AK_FAILED;
    }

    if (AK_SUCCESS != tde_check_input(pDstImage))
        return AK_FAILED;

    if (AK_SUCCESS != tde_check_output(pDstImage))
        return AK_FAILED;

    int ret = AK_FAILED;

    struct ak_tde_layer SrcLayer = {0};
    struct ak_tde_layer DstLayer = {0};

    tde_layer_set(pSrcImage, &SrcLayer);
    tde_layer_set(pDstImage, &DstLayer);

    struct ak_tde_cmd tde_cmd_param;
    tde_cmd_param.rotate_param = AK_GP_ROTATE_NONE;
    tde_cmd_param.tde_layer_src = SrcLayer;
    tde_cmd_param.tde_layer_dst = DstLayer;
    tde_cmd_param.opt = GP_OPT_SCALE;

    if ((pSrcImage->u8GlobalOpacity > IMAGE_OPACITY_MIN) && (pSrcImage->u8GlobalOpacity < IMAGE_OPACITY_MAX))
    {
        int alpha = (pSrcImage->u8GlobalOpacity) / 17;
        tde_cmd_param.alpha = alpha;
        tde_cmd_param.opt = GP_OPT_SCALE_TRANSPARENT;
    }

    ret = ak_tde_opt(&tde_cmd_param);

    return ret;
}

/**
 * @brief   transform image.
 * @param   [IN]device: the device context.
 * @param   [IN]pSrcImage: descriptor of source image.
 * @param   [IN]pDstImage: descriptor of destination image
 * @param   [IN]transform: transform value which defined in transform_t.
 * @return  int
 * @retval  AK_SUCCESS: success; AK_FAILED: failed.
*/
static int hw_tde_image_transform(struct gpu_device *device, struct image_desc *pSrcImage, struct image_desc *pDstImage, transform_t transform)
{
    if ((NULL == device) || (NULL == pSrcImage) || (NULL == pDstImage))
    {
        log_error("Invalid parameter");
        return AK_FAILED;
    }

    if (AK_SUCCESS != tde_check_input(pDstImage))
        return AK_FAILED;

    if (AK_SUCCESS != tde_check_output(pDstImage))
        return AK_FAILED;

    int ret = AK_FAILED;

    struct ak_tde_layer SrcLayer = {0};
    struct ak_tde_layer DstLayer = {0};

    tde_layer_set(pSrcImage, &SrcLayer);
    tde_layer_set(pDstImage, &DstLayer);

    struct ak_tde_cmd tde_cmd_param;
    tde_cmd_param.rotate_param = AK_GP_ROTATE_NONE;
    tde_cmd_param.tde_layer_src = SrcLayer;
    tde_cmd_param.tde_layer_dst = DstLayer;
    tde_cmd_param.opt = GP_OPT_ROTATE;
    tde_cmd_param.rotate_param = tde_transform_convert(transform);

    if ((pSrcImage->u8GlobalOpacity > IMAGE_OPACITY_MIN) && (pSrcImage->u8GlobalOpacity < IMAGE_OPACITY_MAX))
    {
        int alpha = (pSrcImage->u8GlobalOpacity) / 17;
        tde_cmd_param.alpha = alpha;
        tde_cmd_param.opt = GP_OPT_ROTATE_TRANSPARENT;
    }

    ret = ak_tde_opt(&tde_cmd_param);

    return ret;
}

/**
 * @brief   blend image.
 * @param   [IN]device: the device context.
 * @param   [IN]pForeGround: descriptor of foreground image.
 * @param   [IN]pBackGround: descriptor of background image.
 * @param   [IN]pOutputImage: descriptor of output image,
 *                           if null, will just blends foreground image to background image.
 * @param   [IN]config: blending configuration
 * @return  int
 * @retval  AK_SUCCESS: success; AK_FAILED: failed.
*/
static int hw_tde_image_blend(struct gpu_device *device, struct image_desc *pForeGround, struct image_desc *pBackGround, struct image_desc *pOutputImage, struct blend_config *config)
{
    if ((NULL == device) || (NULL == pForeGround) || (NULL == pBackGround))
    {
        log_error("Invalid parameter");
        return AK_FAILED;
    }

    if (AK_SUCCESS != tde_check_input(pForeGround))
        return AK_FAILED;

    if (AK_SUCCESS != tde_check_output(pBackGround))
        return AK_FAILED;

    int ret = AK_FAILED;
    struct ak_tde_layer SrcLayer = {0};
    struct ak_tde_layer DstLayer = {0};

    tde_layer_set(pForeGround, &SrcLayer);
    tde_layer_set(pBackGround, &DstLayer);

    struct ak_tde_cmd tde_cmd_param;
    tde_cmd_param.rotate_param = AK_GP_ROTATE_NONE;
    tde_cmd_param.tde_layer_src = SrcLayer;
    tde_cmd_param.tde_layer_dst = DstLayer;
    tde_cmd_param.opt = GP_OPT_BLIT;

    if ((pForeGround->u8GlobalOpacity > IMAGE_OPACITY_MIN) && (pForeGround->u8GlobalOpacity < IMAGE_OPACITY_MAX))
    {
        int alpha = (pForeGround->u8GlobalOpacity) / 17;
        tde_cmd_param.alpha = alpha;
        tde_cmd_param.opt = GP_OPT_BLIT_TRANSPARENT;
    }

    ret = ak_tde_opt(&tde_cmd_param);

    return ret;
}

/**
 * @brief   blend image with color key.
 * @param   [IN]device: the device context.
 * @param   [IN]pSrcImage: descriptor of source image.
 * @param   [IN]pDstImage: descriptor of destination image
 * @param   [IN]colorkey: colorkey struct for operating
 * @return  int
 * @retval  AK_SUCCESS: success; AK_FAILED: failed.
*/
static int hw_tde_image_blend_colorkey(struct gpu_device *device, struct image_desc *pSrcImage, struct image_desc *pDstImage, struct image_colorkey *colorkey)
{
    if ((NULL == device) || (NULL == pSrcImage) || (NULL == pDstImage) || (NULL == colorkey))
    {
        log_error("Invalid parameter");
        return AK_FAILED;
    }

    if (AK_SUCCESS != tde_check_input(pDstImage))
        return AK_FAILED;

    if (AK_SUCCESS != tde_check_output(pDstImage))
        return AK_FAILED;

    int ret = AK_FAILED;
    struct ak_tde_layer SrcLayer = {0};
    struct ak_tde_layer DstLayer = {0};

    tde_layer_set(pSrcImage, &SrcLayer);
    tde_layer_set(pDstImage, &DstLayer);

    struct ak_gp_colorkey param = {0};
    param.color_min = colorkey->u32ColorKey - colorkey->u8Differ;
    param.color_max = colorkey->u32ColorKey + colorkey->u8Differ;

    if ((colorkey->u8OpacityPositive >= IMAGE_OPACITY_MAX) && (colorkey->u8OpacityInverse < IMAGE_OPACITY_MIN))
        param.coloract = COLOR_KEEP;
    else if ((colorkey->u8OpacityInverse >= IMAGE_OPACITY_MAX) && (colorkey->u8OpacityPositive < IMAGE_OPACITY_MIN))
        param.coloract = COLOR_DELETE;
    else
    {
        log_error("Unsupported global opacity setting");
        return AK_FAILED;
    }

    ret = ak_tde_opt_colorkey(&SrcLayer, &DstLayer, param);

    return ret;
}

/**
 * @brief   Fill the image with color.
 * @param   [IN]device: the device context.
 * @param   [IN]pDstImage: descriptor of destination image.
 * @param   [IN]color: the color which will be used for filling
 * @return  int
 * @retval  AK_SUCCESS: success; AK_FAILED: failed.
*/
static int hw_tde_image_fill(struct gpu_device *device, struct image_desc *pDstImage, uint32_t color)
{
    if ((NULL == device) || (NULL == pDstImage))
    {
        log_error("Invalid parameter");
        return AK_FAILED;
    }

    if (AK_SUCCESS != tde_check_input(pDstImage))
        return AK_FAILED;

    if (AK_SUCCESS != tde_check_output(pDstImage))
        return AK_FAILED;

    int ret = AK_FAILED;

    struct ak_tde_layer DstLayer = {0};
    tde_layer_set(pDstImage, &DstLayer);

//    ret = ak_tde_opt_fillrect(&DstLayer, color);
    struct ak_tde_cmd tde_cmd_set= {0};

    tde_cmd_set.opt = GP_OPT_FILLRECT;
    tde_cmd_set.tde_layer_dst = DstLayer;
    tde_cmd_set.color_rect = color;
    tde_cmd_set.alpha = pDstImage->u8GlobalOpacity;

    ret = ak_tde_opt( &tde_cmd_set );
    if (AK_SUCCESS != ret)
    {
        log_error("Operating failed");
        return AK_FAILED;
    }

    return ret;
}

/**
 * @brief   check the pixel format of source bitmap.
 * @param   [IN]format: pixel format of source bitmap.
 * @return  bool
 * @retval  true: supported; false: unsupported.
*/
static bool hw_tde_check_source_format(pixel_format_t format)
{
    bool ret = false;

    switch (format)
    {
        /* GP_FORMAT_YUV420P */
        case SYSTEM_PIXEL_FORMAT_FULLPLANE_YV12:
        /* GP_FORMAT_YUV420SP */
        case SYSTEM_PIXEL_FORMAT_SEMIPLANE_NV21:
        /* GP_FORMAT_ARGB8888 */
        case SYSTEM_PIXEL_FORMAT_ARGB32:
        /* GP_FORMAT_ABGR8888 */
        case SYSTEM_PIXEL_FORMAT_ABGR32:
        /* GP_FORMAT_BGRA8888 */
        case SYSTEM_PIXEL_FORMAT_BGRA32:
        /* GP_FORMAT_RGBA8888 */
        case SYSTEM_PIXEL_FORMAT_RGBA32:
        /* GP_FORMAT_RGB888 */
        case SYSTEM_PIXEL_FORMAT_RGB24:
        /* GP_FORMAT_BGR888 */
        case SYSTEM_PIXEL_FORMAT_BGR24:
        /* GP_FORMAT_RGB565 */
        case SYSTEM_PIXEL_FORMAT_RGB565:
        /* GP_FORMAT_BGR565 */
        case SYSTEM_PIXEL_FORMAT_BGR565:
            ret = true;
            break;

        default:
            break;
    }

    return ret;
}

/**
 * @brief   check the pixel format of target bitmap.
 * @param   [IN]format: pixel format of source bitmap.
 * @return  bool
 * @retval  true: supported; false: unsupported.
*/
static bool hw_tde_check_target_format(pixel_format_t format)
{
    bool ret = false;

    switch (format)
    {
        /* GP_FORMAT_YUV420P */
        case SYSTEM_PIXEL_FORMAT_FULLPLANE_YV12:
        /* GP_FORMAT_YUV420SP */
        case SYSTEM_PIXEL_FORMAT_SEMIPLANE_NV21:
        /* GP_FORMAT_RGB888 */
        case SYSTEM_PIXEL_FORMAT_RGB24:
        /* GP_FORMAT_BGR888 */
        case SYSTEM_PIXEL_FORMAT_BGR24:
        /* GP_FORMAT_RGB565 */
        case SYSTEM_PIXEL_FORMAT_RGB565:
        /* GP_FORMAT_BGR565 */
        case SYSTEM_PIXEL_FORMAT_BGR565:
            ret = true;
            break;

        default:
            break;
    }

    return ret;
}

/**
 * @brief   check whether the source image is supported.
 * @param   [IN]image: information of source image.
 * @return  bool
 * @retval  true: supported; false: unsupported.
*/
bool hw_tde_check_source(struct image_desc *image)
{
    bool ret = false;

    return ret;
}

/**
 * @brief   check whether the target image is supported.
 * @param   [IN]image: information of source image.
 * @return  bool
 * @retval  true: supported; false: unsupported.
*/
bool hw_tde_check_target(struct image_desc *image)
{
    bool ret = false;

    return ret;
}

/**
 * @brief   close device.
 * @param   [IN]device: the device context.
 * @return  int
 * @retval  AK_SUCCESS: success; AK_FAILED: failed.
*/
static int hw_tde_close(struct hw_device_t* device)
{
    if (NULL == device)
    {
        log_error("Invalid parameter");
        return AK_FAILED;
    }

    int ret = AK_FAILED;
    struct gpu_device *dev = (struct gpu_device *)device;

    ret = ak_tde_close();
    if (AK_SUCCESS != ret)
    {
        log_error("Failed to close the device");
        return AK_FAILED;
    }

    free(device);

    return AK_SUCCESS;
}

/**
 * @brief   open device.
 * @param   [IN]module: the module context.
 * @param   [IN]name: device name.
 * @param   [OUT]device: information struct of device.
 * @return  int
 * @retval  AK_SUCCESS: success; AK_FAILED: failed.
*/
static int hw_tde_open(const hw_module_t* module, const char* name, hw_device_t** device)
{
    *device = NULL;

    if ((NULL == module) || (NULL == name)  || (NULL == device))
    {
        log_error("Invalid parameter");
        return AK_FAILED;
    }

    if (0 != strcmp(name, GRAPHIC_GPU_NAME))
    {
        log_error("Unsupported device.");
        return AK_FAILED;
    }

    struct gpu_device *dev = NULL;
    dev = calloc(1, sizeof(struct gpu_device));
    if (NULL == dev)
    {
        log_error("malloc for information struct of device failed");
        return AK_FAILED;
    }

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = GRAPHIC_GPU_API_VERSION_1_0;
    dev->common.module = (struct hw_module_t *) module;
    dev->common.close = hw_tde_close;

    dev->image_copy = hw_tde_image_copy;
    dev->image_scale = hw_tde_image_scale;
    dev->image_transform = hw_tde_image_transform;
    dev->image_blend = hw_tde_image_blend;
    dev->image_blend_colorkey = hw_tde_image_blend_colorkey;
    dev->image_fill = hw_tde_image_fill;
    dev->check_source_format = hw_tde_check_source_format;
    dev->check_target_format = hw_tde_check_target_format;
    dev->check_source = hw_tde_check_source;
    dev->check_target = hw_tde_check_target;

    *device = &dev->common;

    return AK_SUCCESS;
}


static struct hw_module_methods_t hw_module_methods = {
    .open = hw_tde_open,
};

static struct graphic_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = GPU_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = GPU_HARDWARE_MODULE_ID,
        .name = "GPU Module",
        .author = "Anyka",
        .methods = &hw_module_methods,
    },
};

/**
 * @brief   initialize module.
 * @param   void
 * @return  int
 * @retval  AK_SUCCESS: success; AK_FAILED: failed.
*/
int hw_gpu_module_init(void)
{
    return hw_register_module(&(HAL_MODULE_INFO_SYM.common));
}
