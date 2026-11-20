/**
 * @file hal_isp.c
 * @brief ISP HAL implementation for AnyCloud39AV100_V2
 * @details Chip-specific implementation bridging hal_isp.h to ak_vi/ak_vpss APIs.
 */

#include "hal_isp.h"
#include <stdlib.h>
#include <string.h>

#include "ak_common.h" /* for MODULE_ID_VI used by ak_vi.h */
#include "ak_common_video.h" /* for struct frame used by ak_vi.h */
#include "ak_vi.h"
#include "ak_vpss.h"

struct hal_isp_ctx {
    /*
     * VI device ID used when calling AV100_V2 PDK APIs (ak_vi_* / ak_vpss_*).
     * AV100_V2 SDK does not expose AK_MPI_ISP_* / ak_vpss_isp.h; mode switch
     * goes through ak_vi_switch_mode and soft photo sensor goes through
     * ak_vpss_*_soft_ps_*.
     */
    int dev_id;
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
    /*
     * Generic parameter setting entry; key would map to chip-specific PDK
     * tuning API on AV100_V2 (ak_vpss_*).  Kept as a stub until specific
     * keys are needed by the framework, mirroring AV200 behaviour.
     */
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

    /* Map HAL mode to AnyCloud VI day/night mode. */
    enum ak_vi_daynight_mode ak_mode;
    switch (mode) {
        case HAL_ISP_MODE_DAY:
            ak_mode = VI_MODE_DAY_OUTDOOR;
            break;
        case HAL_ISP_MODE_NIGHT:
            ak_mode = VI_MODE_NIGHTTIME;
            break;
        case HAL_ISP_MODE_USR_DEFINE_1:
            ak_mode = VI_MODE_USR_DEFINE_1;
            break;
        case HAL_ISP_MODE_USR_DEFINE_2:
            ak_mode = VI_MODE_USR_DEFINE_2;
            break;
        case HAL_ISP_MODE_USR_DEFINE_3:
            ak_mode = VI_MODE_USR_DEFINE_3;
            break;
        default:
            return -1;
    }

    /* Call AnyCloud VI API to switch sensor/ISP mode. */
    int ret = ak_vi_switch_mode(ctx->dev_id, ak_mode);
    return (ret == 0) ? 0 : -1;
}

int hal_isp_set_soft_ps_param(void *h, const hal_isp_soft_ps_param_t* param)
{
    if (!h || !param) return -1;

    struct hal_isp_ctx* ctx = (struct hal_isp_ctx*)h;

    /* Convert HAL parameter structure to AnyCloud ak_soft_ps_attr (vpss). */
    struct ak_soft_ps_attr ak_param;
    memset(&ak_param, 0, sizeof(ak_param));

    ak_param.day_to_night_lum = param->stIrLedNightParm.u32DayToNightLum;
    ak_param.night_to_day_lum = param->stIrLedNightParm.u32NightToDayLum;
    ak_param.awb_night_dis_min = param->stIrLedNightParm.u32AwbNightDisMin;
    ak_param.awb_night_dis_max = param->stIrLedNightParm.u32AwbNightDisMax;
    ak_param.day_check_frame_num = param->u32DayCheckFrameNum;
    ak_param.night_check_frame_num = param->u32NightCheckFrameNum;
    ak_param.exp_stable_check_frame_num = param->u32ExpStableCheckFrameNum;
    ak_param.stable_wait_timeout_ms = (param->s32ExpStableTimeOutMs > 0) ? (unsigned int)param->s32ExpStableTimeOutMs : 0;
    ak_param.gain_r = param->stIrLedNightParm.u32GainR;
    ak_param.offset_r = param->stIrLedNightParm.s32OffsetR;
    ak_param.gain_b = param->stIrLedNightParm.u32GainB;
    ak_param.offset_b = param->stIrLedNightParm.s32OffsetB;

    /*
     * NOTE: The HAL parameter struct still carries `enNightType` (AV200
     * ISP_SOFT_PS_ATTR_S inheritance), but `struct ak_soft_ps_attr` in
     * AV100_V2 PDK has no equivalent field — night-type is selected
     * implicitly by the sensor configuration loaded with
     * ak_vi_load_sensor_cfg.  The field is intentionally dropped here to
     * keep the behaviour aligned with the AV100_V2 SDK contract.
     */

    /* Call AnyCloud VPSS API to apply soft photo sensor parameters. */
    int ret = ak_vpss_set_soft_ps_param(ctx->dev_id, &ak_param);
    return (ret == 0) ? 0 : -1;
}

int hal_isp_get_soft_ps_level(void *h, int pre_status, int* day_night)
{
    if (!h || !day_night) return -1;

    struct hal_isp_ctx* ctx = (struct hal_isp_ctx*)h;

    /* Call AnyCloud VPSS API to get soft photo sensor level (0=day, 1=night). */
    int new_level = 0;
    int ret = ak_vpss_get_soft_ps_level(ctx->dev_id, pre_status, &new_level);
    if (ret != 0)
        return -1;

    *day_night = new_level;
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
