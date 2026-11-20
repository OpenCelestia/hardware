#ifndef __CHIP_AK3918AV130_SERIES
#define __CHIP_AK3918AV130_SERIES 1
#endif

/*
 * Sky39AV130 ISP HAL. Day/night uses ak_vi_switch_mode and the soft-ps
 * calls in platform/include/ak_vpss.h.
 */
#include "hal_isp_api.h"

#include <string.h>

#include "ak_common.h"
#include "ak_vi.h"
#include "ak_vpss.h"

struct mira_isp {
    int dev_id;
};

static struct mira_isp s_isp;

void *hal_isp_init(int dev_id, void *config)
{
    (void)config;
    s_isp.dev_id = dev_id;
    return &s_isp;
}

int hal_isp_deinit(void *h)
{
    (void)h;
    return 0;
}

int hal_isp_set_param(void *h, int key, long value)
{
    (void)h;
    (void)key;
    (void)value;
    return 0;
}

int hal_isp_get_frame(void *h, void *frame_out)
{
    (void)h;
    (void)frame_out;
    return 0;
}

int hal_isp_switch_mode_direct(int dev_id, int mode)
{
    enum ak_vi_daynight_mode ak_mode;

    switch ((hal_isp_mode_t)mode) {
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
    return ak_vi_switch_mode(dev_id, ak_mode) == AK_SUCCESS ? 0 : -1;
}

int hal_isp_set_soft_ps_param_direct(int dev_id, const void *param)
{
    const hal_isp_soft_ps_param_t *in = param;
    struct ak_soft_ps_attr ak;

    if (in == NULL)
        return -1;
    memset(&ak, 0, sizeof(ak));
    ak.day_to_night_lum = in->stIrLedNightParm.u32DayToNightLum;
    ak.night_to_day_lum = in->stIrLedNightParm.u32NightToDayLum;
    ak.awb_night_dis_min = in->stIrLedNightParm.u32AwbNightDisMin;
    ak.awb_night_dis_max = in->stIrLedNightParm.u32AwbNightDisMax;
    ak.day_check_frame_num = in->u32DayCheckFrameNum;
    ak.night_check_frame_num = in->u32NightCheckFrameNum;
    ak.exp_stable_check_frame_num = in->u32ExpStableCheckFrameNum;
    ak.stable_wait_timeout_ms = in->s32ExpStableTimeOutMs > 0
        ? (unsigned int)in->s32ExpStableTimeOutMs : 0;
    ak.gain_r = in->stIrLedNightParm.u32GainR;
    ak.offset_r = in->stIrLedNightParm.s32OffsetR;
    ak.gain_b = in->stIrLedNightParm.u32GainB;
    ak.offset_b = in->stIrLedNightParm.s32OffsetB;
    return ak_vpss_set_soft_ps_param(dev_id, &ak) == AK_SUCCESS ? 0 : -1;
}

int hal_isp_get_soft_ps_level_direct(int dev_id, int pre_status, int *day_night)
{
    if (day_night == NULL)
        return -1;
    return ak_vpss_get_soft_ps_level(dev_id, pre_status, day_night) == AK_SUCCESS ? 0 : -1;
}

