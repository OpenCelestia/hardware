// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * ISP AF statistics + VCM (user mode) / kernel af_ctrl (kernel mode).
 * Opt-in via hal_camera_config.ini [af] enable=1; default disabled.
 */

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ak_vpss_isp.h"
#include "camera.h"
#include "hal_camera_chip_config.h"
#include "hal_camera_config.h"
#include "hal_camera_vcm.h"
#include "log.h"

#define LOG_TAG "HAL_CAMERA_AF"

/* Match ak_isp_app.h AK_ISP_AF_* layout (kernel STABLE == 5) */
#define AK_ISP_AF_STATE_STABLE  5

/*
 * Kernel AF must reuse VPSS-owned isp fd inside libplat_isp_sdk.
 * Do NOT open /dev/isp-0-*-0 again — exclusive open returns EBUSY.
 */
extern AK_S32 AK_ISP_set_af_ctrl(int dev, const void *p_af_ctrl);
extern AK_S32 AK_ISP_get_af_status(int dev, void *p_af_status);

typedef struct {
    uint8_t width_from;
    uint8_t width_end;
    uint8_t height_from;
    uint8_t height_end;
} hal_isp_af_roi_t;

typedef struct {
    hal_isp_af_roi_t af_roi;
    uint8_t af_roi_type;
    uint8_t af_mode;
} hal_isp_af_ctrl_t;

typedef struct {
    uint8_t af_mode;
    uint8_t af_update;
    uint8_t af_state;
    uint8_t reserved;
    int32_t focus_pos;
} hal_isp_af_status_t;

typedef struct {
    int inited;
    int supported;
    int device_id;
    int run_mode; /* AF_RUN_MODE_* */
    hal_camera_vcm_ctx_t *vcm;
    int settle_ms;
} hal_camera_af_slot_t;

static hal_camera_af_slot_t s_af_slots[CAMERA_MAX_NUM];
static pthread_mutex_t s_af_global_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t hal_camera_af_score_from_stat(const ISP_AF_INFO_S *af)
{
    uint32_t score = 0;
    int r, c, k;

    if (!af) {
        return 0;
    }
    for (r = 2; r < 6; r++) {
        for (c = 4; c < 12; c++) {
            for (k = 0; k < 5; k++) {
                score += af->u32AfStatics[r][c][k];
            }
        }
    }
    return score;
}

static int hal_camera_af_enable_isp_stat(int device_id)
{
    ISP_AF_ATTR_S attr;
    AK_S32 ret;

    memset(&attr, 0, sizeof(attr));
    ret = AK_MPI_ISP_GetAfAttr(device_id, &attr);
    if (ret != 0) {
        log_warn("AF: GetAfAttr dev=%d failed ret=%d", device_id, (int)ret);
        return -1;
    }
    if (!attr.u16AfStatEnable) {
        attr.u16AfStatEnable = 1;
        ret = AK_MPI_ISP_SetAfAttr(device_id, &attr);
        if (ret != 0) {
            log_warn("AF: SetAfAttr dev=%d failed ret=%d", device_id, (int)ret);
            return -1;
        }
        log_info("AF: enabled ISP AF statistics on dev=%d", device_id);
    }
    return 0;
}

static void hal_camera_af_roi_to_isp(const camera_af_roi_t *roi, hal_isp_af_roi_t *out)
{
    float nx = 0.5f - (1.0f / 16.0f);
    float ny = 0.5f - (1.0f / 16.0f);
    float nw = 1.0f / 8.0f;
    float nh = 1.0f / 8.0f;
    int wf, we, hf, he;

    if (roi && roi->nw > 0.0f && roi->nh > 0.0f) {
        nx = roi->nx;
        ny = roi->ny;
        nw = roi->nw;
        nh = roi->nh;
    }
    if (nx < 0.0f) {
        nx = 0.0f;
    }
    if (ny < 0.0f) {
        ny = 0.0f;
    }
    if (nx + nw > 1.0f) {
        nx = 1.0f - nw;
    }
    if (ny + nh > 1.0f) {
        ny = 1.0f - nh;
    }

    /* ISP AF ROI fields are 0..255 style window ends (see kernel af path) */
    wf = (int)(nx * 255.0f);
    we = (int)((nx + nw) * 255.0f);
    hf = (int)(ny * 255.0f);
    he = (int)((ny + nh) * 255.0f);
    if (we > 255) {
        we = 255;
    }
    if (he > 255) {
        he = 255;
    }
    if (we < wf) {
        we = wf;
    }
    if (he < hf) {
        he = hf;
    }
    out->width_from = (uint8_t)wf;
    out->width_end = (uint8_t)we;
    out->height_from = (uint8_t)hf;
    out->height_end = (uint8_t)he;
}

static void hal_camera_af_slot_reset(hal_camera_af_slot_t *slot)
{
    if (!slot) {
        return;
    }
    if (slot->vcm) {
        hal_camera_vcm_close(slot->vcm);
        slot->vcm = NULL;
    }
    slot->inited = 0;
    slot->supported = 0;
    slot->device_id = 0;
    slot->run_mode = AF_RUN_MODE_DEFAULT;
    slot->settle_ms = AF_SETTLE_MS_DEFAULT;
}

static int hal_camera_af_slot_init_locked(hal_camera_af_slot_t *slot, int device_id)
{
    const hal_camera_config_t *cfg = hal_camera_get_config();

    if (!slot || device_id < 0 || device_id >= CAMERA_MAX_NUM) {
        return -EINVAL;
    }

    if (slot->inited && slot->device_id == device_id && slot->supported) {
        if (slot->run_mode == AF_RUN_MODE_KERNEL) {
            return 0;
        }
        if (slot->run_mode == AF_RUN_MODE_USER && slot->vcm) {
            return 0;
        }
    }
    if (slot->inited && slot->device_id == device_id && !slot->supported &&
        cfg && !cfg->af_enable) {
        return -ENOTSUP;
    }

    hal_camera_af_slot_reset(slot);
    slot->device_id = device_id;
    slot->settle_ms = AF_SETTLE_MS_DEFAULT;

    if (!cfg || !cfg->af_enable) {
        slot->inited = 1;
        slot->supported = 0;
        return -ENOTSUP;
    }

    slot->settle_ms = (cfg->af_settle_ms > 0) ? cfg->af_settle_ms : AF_SETTLE_MS_DEFAULT;
    slot->run_mode = (cfg->af_run_mode == AF_RUN_MODE_USER) ?
                     AF_RUN_MODE_USER : AF_RUN_MODE_KERNEL;

    if (slot->run_mode == AF_RUN_MODE_KERNEL) {
        hal_isp_af_status_t st;

        /* Probe via isp_sdk shared fd (opened by VPSS AK_ISP_sdk_init). */
        memset(&st, 0, sizeof(st));
        if (AK_ISP_get_af_status(device_id, &st) != 0) {
            log_info("AF: kernel mode isp_sdk not ready dev=%d (will retry)", device_id);
            return -ENOTSUP;
        }
        slot->inited = 1;
        slot->supported = 1;
        log_info("AF: supported on dev=%d mode=kernel (isp_sdk)", device_id);
        return 0;
    }

    {
        const char *vcm_type = (cfg->vcm_type[0] != '\0') ? cfg->vcm_type : "fp5510";
        int pos_min = cfg->focus_pos_min;
        int pos_max = cfg->focus_pos_max;

        slot->vcm = hal_camera_vcm_open(vcm_type, cfg->vcm_subdev_path, pos_min, pos_max);
        if (!slot->vcm) {
            log_info("AF: VCM probe failed on dev=%d (af_enable=1, will retry)", device_id);
            return -ENOTSUP;
        }
        if (hal_camera_af_enable_isp_stat(device_id) != 0) {
            hal_camera_af_slot_reset(slot);
            return -ENOTSUP;
        }
        slot->inited = 1;
        slot->supported = 1;
        log_info("AF: supported on dev=%d mode=user vcm=%s range [%d,%d] settle=%dms",
                 device_id, vcm_type, pos_min, pos_max, slot->settle_ms);
        return 0;
    }
}

void hal_camera_af_release(void)
{
    int i;

    pthread_mutex_lock(&s_af_global_lock);
    for (i = 0; i < CAMERA_MAX_NUM; i++) {
        hal_camera_af_slot_reset(&s_af_slots[i]);
    }
    pthread_mutex_unlock(&s_af_global_lock);
}

static hal_camera_af_slot_t *hal_camera_af_get_slot(int device_id)
{
    if (device_id < 0 || device_id >= CAMERA_MAX_NUM) {
        return NULL;
    }
    return &s_af_slots[device_id];
}

int hal_camera_af_is_supported(int device_id)
{
    hal_camera_af_slot_t *slot = hal_camera_af_get_slot(device_id);
    int ret;

    if (!slot) {
        return 0;
    }
    pthread_mutex_lock(&s_af_global_lock);
    ret = hal_camera_af_slot_init_locked(slot, device_id);
    pthread_mutex_unlock(&s_af_global_lock);
    return (ret == 0 && slot->supported) ? 1 : 0;
}

int hal_camera_af_get_sharpness(int device_id, uint32_t *score_out)
{
    hal_camera_af_slot_t *slot = hal_camera_af_get_slot(device_id);
    ISP_AF_INFO_S af;
    AK_S32 ret;

    if (!score_out) {
        return -EINVAL;
    }
    pthread_mutex_lock(&s_af_global_lock);
    if (hal_camera_af_slot_init_locked(slot, device_id) != 0 || !slot->supported ||
        slot->run_mode != AF_RUN_MODE_USER) {
        pthread_mutex_unlock(&s_af_global_lock);
        return -ENOTSUP;
    }
    pthread_mutex_unlock(&s_af_global_lock);

    memset(&af, 0, sizeof(af));
    ret = AK_MPI_ISP_GetAfStat(device_id, &af);
    if (ret != 0) {
        log_warn("AF: GetAfStat dev=%d failed ret=%d", device_id, (int)ret);
        return -EIO;
    }
    *score_out = hal_camera_af_score_from_stat(&af);
    return 0;
}

int hal_camera_af_set_focus_pos(int device_id, int pos)
{
    hal_camera_af_slot_t *slot = hal_camera_af_get_slot(device_id);
    int ret;

    if (!slot) {
        return -EINVAL;
    }
    pthread_mutex_lock(&s_af_global_lock);
    ret = hal_camera_af_slot_init_locked(slot, device_id);
    if (ret != 0 || !slot->supported || !slot->vcm) {
        pthread_mutex_unlock(&s_af_global_lock);
        return -ENOTSUP;
    }
    ret = hal_camera_vcm_set_pos(slot->vcm, pos);
    if (ret == 0 && slot->settle_ms > 0) {
        pthread_mutex_unlock(&s_af_global_lock);
        usleep((useconds_t)slot->settle_ms * 1000u);
        return 0;
    }
    pthread_mutex_unlock(&s_af_global_lock);
    return ret;
}

int hal_camera_af_get_focus_pos(int device_id, int *pos_out)
{
    hal_camera_af_slot_t *slot = hal_camera_af_get_slot(device_id);
    int ret;

    if (!pos_out) {
        return -EINVAL;
    }
    pthread_mutex_lock(&s_af_global_lock);
    ret = hal_camera_af_slot_init_locked(slot, device_id);
    if (ret != 0 || !slot->supported || !slot->vcm) {
        pthread_mutex_unlock(&s_af_global_lock);
        return -ENOTSUP;
    }
    ret = hal_camera_vcm_get_pos(slot->vcm, pos_out);
    pthread_mutex_unlock(&s_af_global_lock);
    return ret;
}

int hal_camera_af_get_pos_range(int device_id, int *min_out, int *max_out)
{
    hal_camera_af_slot_t *slot = hal_camera_af_get_slot(device_id);
    int ret;

    if (!min_out || !max_out) {
        return -EINVAL;
    }
    pthread_mutex_lock(&s_af_global_lock);
    ret = hal_camera_af_slot_init_locked(slot, device_id);
    if (ret != 0 || !slot->supported || !slot->vcm) {
        pthread_mutex_unlock(&s_af_global_lock);
        return -ENOTSUP;
    }
    ret = hal_camera_vcm_get_range(slot->vcm, min_out, max_out);
    pthread_mutex_unlock(&s_af_global_lock);
    return ret;
}

int hal_camera_af_get_settle_ms(int device_id)
{
    hal_camera_af_slot_t *slot = hal_camera_af_get_slot(device_id);

    if (!slot || !hal_camera_af_is_supported(device_id)) {
        return AF_SETTLE_MS_DEFAULT;
    }
    return slot->settle_ms > 0 ? slot->settle_ms : AF_SETTLE_MS_DEFAULT;
}

static int hal_camera_af_trigger_kernel(hal_camera_af_slot_t *slot, const camera_af_roi_t *roi)
{
    hal_isp_af_ctrl_t ctrl;
    AK_S32 ret;

    memset(&ctrl, 0, sizeof(ctrl));
    hal_camera_af_roi_to_isp(roi, &ctrl.af_roi);
    ctrl.af_roi_type = 0;
    ctrl.af_mode = 1; /* ONCE */
    ret = AK_ISP_set_af_ctrl(slot->device_id, &ctrl);
    if (ret != 0) {
        log_warn("AF: AK_ISP_set_af_ctrl dev=%d failed ret=%d", slot->device_id, (int)ret);
        return -EIO;
    }
    return 0;
}

static int hal_camera_af_get_hw_state_kernel(hal_camera_af_slot_t *slot, camera_af_hw_state_t *st)
{
    hal_isp_af_status_t kst;
    AK_S32 ret;

    if (!st) {
        return -EINVAL;
    }
    memset(&kst, 0, sizeof(kst));
    ret = AK_ISP_get_af_status(slot->device_id, &kst);
    if (ret != 0) {
        log_warn("AF: AK_ISP_get_af_status dev=%d failed ret=%d", slot->device_id, (int)ret);
        return -EIO;
    }
    st->af_update = kst.af_update;
    st->focus_pos = (int)kst.focus_pos;
    if (kst.af_update) {
        st->state = CAMERA_AF_HW_STATE_RUNNING;
    } else if (kst.af_state == AK_ISP_AF_STATE_STABLE) {
        st->state = CAMERA_AF_HW_STATE_STABLE;
    } else {
        st->state = CAMERA_AF_HW_STATE_IDLE;
    }
    return 0;
}

int hal_camera_af_control(int device_id, int cmd, void *arg)
{
    hal_camera_af_slot_t *slot = hal_camera_af_get_slot(device_id);
    int ret;

    if (!slot) {
        return -EINVAL;
    }

    switch (cmd) {
    case CAMERA_AF_CMD_IS_SUPPORTED: {
        int *out = (int *)arg;
        if (!out) {
            return -EINVAL;
        }
        *out = hal_camera_af_is_supported(device_id) ? 1 : 0;
        return 0;
    }
    case CAMERA_AF_CMD_GET_SHARPNESS:
        return hal_camera_af_get_sharpness(device_id, (uint32_t *)arg);
    case CAMERA_AF_CMD_SET_FOCUS_POS: {
        int *pos = (int *)arg;
        if (!pos) {
            return -EINVAL;
        }
        return hal_camera_af_set_focus_pos(device_id, *pos);
    }
    case CAMERA_AF_CMD_GET_FOCUS_POS:
        return hal_camera_af_get_focus_pos(device_id, (int *)arg);
    case CAMERA_AF_CMD_GET_POS_RANGE: {
        int *mm = (int *)arg;
        if (!mm) {
            return -EINVAL;
        }
        return hal_camera_af_get_pos_range(device_id, &mm[0], &mm[1]);
    }
    case CAMERA_AF_CMD_GET_RUN_MODE: {
        int *mode = (int *)arg;
        if (!mode) {
            return -EINVAL;
        }
        pthread_mutex_lock(&s_af_global_lock);
        ret = hal_camera_af_slot_init_locked(slot, device_id);
        if (ret != 0 || !slot->supported) {
            pthread_mutex_unlock(&s_af_global_lock);
            return -ENOTSUP;
        }
        *mode = (slot->run_mode == AF_RUN_MODE_USER) ?
                CAMERA_AF_RUN_MODE_USER : CAMERA_AF_RUN_MODE_KERNEL;
        pthread_mutex_unlock(&s_af_global_lock);
        return 0;
    }
    case CAMERA_AF_CMD_TRIGGER: {
        camera_af_trigger_arg_t *targ = (camera_af_trigger_arg_t *)arg;
        const camera_af_roi_t *roi = targ ? targ->roi : NULL;

        pthread_mutex_lock(&s_af_global_lock);
        ret = hal_camera_af_slot_init_locked(slot, device_id);
        if (ret != 0 || !slot->supported) {
            pthread_mutex_unlock(&s_af_global_lock);
            return -ENOTSUP;
        }
        if (slot->run_mode != AF_RUN_MODE_KERNEL) {
            pthread_mutex_unlock(&s_af_global_lock);
            return -ENOTSUP;
        }
        ret = hal_camera_af_trigger_kernel(slot, roi);
        pthread_mutex_unlock(&s_af_global_lock);
        if (ret == 0) {
            log_info("AF: kernel TRIGGER dev=%d", device_id);
        }
        return ret;
    }
    case CAMERA_AF_CMD_GET_HW_STATE: {
        camera_af_hw_state_t *st = (camera_af_hw_state_t *)arg;

        pthread_mutex_lock(&s_af_global_lock);
        ret = hal_camera_af_slot_init_locked(slot, device_id);
        if (ret != 0 || !slot->supported || slot->run_mode != AF_RUN_MODE_KERNEL) {
            pthread_mutex_unlock(&s_af_global_lock);
            return -ENOTSUP;
        }
        ret = hal_camera_af_get_hw_state_kernel(slot, st);
        pthread_mutex_unlock(&s_af_global_lock);
        return ret;
    }
    case CAMERA_AF_CMD_CANCEL:
        /* Kernel ONCE has no cancel ioctl yet; Framework stops polling. */
        return 0;
    default:
        return -EINVAL;
    }
}
