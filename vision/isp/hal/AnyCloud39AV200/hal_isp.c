/**
 * @file hal_isp.c
 * @brief ISP HAL implementation for AnyCloud39AV200
 * @details Chip-specific implementation bridging hal_isp.h to ak_vpss_isp APIs.
 */

#include "hal_isp.h"
#include "ak_vpss_isp.h"
#include <stdlib.h>
#include <string.h>

struct hal_isp_ctx {
    int dev_id;  /* Device ID used when calling AK_MPI_ISP_* APIs. */
};

/* ========== Basic HAL implementation ========== */

void *hal_isp_init(int dev_id, void* config)
{
    (void)config;  /* config is currently unused, reserved for future extension. */

    struct hal_isp_ctx* ctx = (struct hal_isp_ctx*)malloc(sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->dev_id = dev_id;
    /* Note: on AnyCloud, ISP is usually initialized by the system; we only store dev_id. */

    return (void *)ctx;
}

int hal_isp_deinit(void *h)
{
    if (!h) return 0;

    struct hal_isp_ctx* ctx = (struct hal_isp_ctx*)h;
    /* Note: on AnyCloud, ISP deinit is usually handled by the system. */

    free(ctx);
    return 0;
}

int hal_isp_set_param(void *h, int key, long value)
{
    (void)h;
    (void)key;
    (void)value;
    /* Generic parameter setting entry; key can be mapped to AK_MPI_ISP_* calls. */
    /* Currently unused, kept for future extension. */
    return 0;
}

int hal_isp_get_frame(void *h, void* frame_out)
{
    (void)h;
    (void)frame_out;
    /* Frame acquisition entry; currently unused and kept for future extension. */
    return 0;
}

int hal_isp_switch_mode(void *h, hal_isp_mode_t mode)
{
    if (!h) return -1;

    struct hal_isp_ctx* ctx = (struct hal_isp_ctx*)h;

    /* Map HAL mode to AnyCloud ISP mode. */
    ISP_DAY_NIGHT_MODE_E ak_mode;
    switch (mode) {
        case HAL_ISP_MODE_DAY:
            ak_mode = MODE_DAY;
            break;
        case HAL_ISP_MODE_NIGHT:
            ak_mode = MODE_NIGHT;
            break;
        case HAL_ISP_MODE_USR_DEFINE_1:
            ak_mode = MODE_USR_DEFINE_1;
            break;
        case HAL_ISP_MODE_USR_DEFINE_2:
            ak_mode = MODE_USR_DEFINE_2;
            break;
        case HAL_ISP_MODE_USR_DEFINE_3:
            ak_mode = MODE_USR_DEFINE_3;
            break;
        default:
            return -1;
    }

    /* Call AnyCloud ISP API to switch mode. */
    AK_S32 ret = AK_MPI_ISP_SwitchMode(ctx->dev_id, ak_mode);
    return (ret == 0) ? 0 : -1;
}

int hal_isp_set_soft_ps_param(void *h, const hal_isp_soft_ps_param_t* param)
{
    if (!h || !param) return -1;

    struct hal_isp_ctx* ctx = (struct hal_isp_ctx*)h;

    /* Convert HAL parameter structure to AnyCloud ISP_SOFT_PS_ATTR_S. */
    ISP_SOFT_PS_ATTR_S ak_param = {0};

    /* Night type: HAL enNightType maps directly to ISP_NIGHT_TYPE_E. */
    ak_param.enNightType = (ISP_NIGHT_TYPE_E)param->enNightType;
    ak_param.u32DayCheckFrameNum = param->u32DayCheckFrameNum;
    ak_param.u32NightCheckFrameNum = param->u32NightCheckFrameNum;
    ak_param.u32ExpStableCheckFrameNum = param->u32ExpStableCheckFrameNum;
    ak_param.s32ExpStableTimeOutMs = param->s32ExpStableTimeOutMs;

    /* Copy IR LED night parameters. */
    ak_param.stIrLedNightParm.u32DayToNightLum = param->stIrLedNightParm.u32DayToNightLum;
    ak_param.stIrLedNightParm.u32NightToDayLum = param->stIrLedNightParm.u32NightToDayLum;
    ak_param.stIrLedNightParm.u32AwbNightDisMax = param->stIrLedNightParm.u32AwbNightDisMax;
    ak_param.stIrLedNightParm.u32AwbNightDisMin = param->stIrLedNightParm.u32AwbNightDisMin;
    ak_param.stIrLedNightParm.u32GainR = param->stIrLedNightParm.u32GainR;
    ak_param.stIrLedNightParm.u32GainB = param->stIrLedNightParm.u32GainB;
    ak_param.stIrLedNightParm.s32OffsetR = param->stIrLedNightParm.s32OffsetR;
    ak_param.stIrLedNightParm.s32OffsetB = param->stIrLedNightParm.s32OffsetB;

    /* Call AnyCloud API to apply soft photo sensor parameters. */
    AK_S32 ret = AK_MPI_ISP_SetSoftPsParam(ctx->dev_id, &ak_param);
    return (ret == 0) ? 0 : -1;
}

int hal_isp_get_soft_ps_level(void *h, int pre_status, int* day_night)
{
    if (!h || !day_night) return -1;

    struct hal_isp_ctx* ctx = (struct hal_isp_ctx*)h;

    /* Call AnyCloud API to get soft photo sensor level. */
    AK_S32 ak_pre_status = (AK_S32)pre_status;
    AK_S32 ak_day_night = 0;

    AK_S32 ret = AK_MPI_ISP_GetSoftPsLevel(ctx->dev_id, ak_pre_status, &ak_day_night);
    if (ret != 0) {
        return -1;
    }

    /* Map result: 0=day, 1=night. */
    *day_night = (int)ak_day_night;
    return 0;
}

/* ========== Direct interfaces implementation (no handle required) ========== */

int hal_isp_switch_mode_direct(int dev_id, int mode)
{
    /* Create a temporary handle. */
    void *h = hal_isp_init(dev_id, NULL);
    if (!h) return -1;

    int ret = hal_isp_switch_mode(h, (hal_isp_mode_t)mode);
    hal_isp_deinit(h);
    return ret;
}

int hal_isp_set_soft_ps_param_direct(int dev_id, const void *param)
{
    /* Create a temporary handle. */
    void *h = hal_isp_init(dev_id, NULL);
    if (!h) return -1;

    int ret = hal_isp_set_soft_ps_param(h, (const hal_isp_soft_ps_param_t *)param);
    hal_isp_deinit(h);
    return ret;
}

int hal_isp_get_soft_ps_level_direct(int dev_id, int pre_status, int *day_night)
{
    /* Create a temporary handle. */
    void *h = hal_isp_init(dev_id, NULL);
    if (!h) return -1;

    int ret = hal_isp_get_soft_ps_level(h, pre_status, day_night);
    hal_isp_deinit(h);
    return ret;
}

/* Note: API layer (hal_isp_api.h) directly calls the implementation functions above.
 * No wrapper functions needed as function names are now identical. */
