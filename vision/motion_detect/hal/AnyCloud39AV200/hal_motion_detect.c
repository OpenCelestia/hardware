/**
 * @file hal_motion_detect.c
 * @brief Motion Detection HAL implementation for AnyCloud39AV200
 * @details Chip-specific motion detection operations using direct function calls
 */

#include "hal_motion_detect.h"
#include "ak_mrd.h"
#include "ak_vpss_isp.h"
#include "log.h"

#define LOG_TAG "HAL_MOTION_DETECT"

/**
 * @brief HAL private data structure (module internal)
 */
typedef struct {
    void *hMrd;           /**< MRD handle. */
    int day_night_status; /**< Day/night status. */
} hal_motion_detect_priv_t;

static hal_motion_detect_priv_t g_hal_priv = {0};

/* Constants from original implementation. */
#define STATE_DAY 0
#define STATE_NIGHT 1

/* Maximum capacity for MRD boxes buffer */
#define HAL_MOTION_DETECT_MAX_CAP 20

/* Note: ISP_MD_DIMENSION_H_MAX and ISP_MD_DIMENSION_V_MAX are defined in isp.h
 * and should be used from there if needed. These values are not used in this file.
 */

int hal_motion_detect_init(void)
{
    /* Chip-specific MRD initialization. */
    g_hal_priv.hMrd = ak_mrd_init();
    if (!g_hal_priv.hMrd) {
        log_error("ak_mrd_init failed");
        return -1;
    }

    g_hal_priv.day_night_status = -1;

    log_info("HAL motion detect initialized");
    return 0;
}

int hal_motion_detect_deinit(void)
{
    if (g_hal_priv.hMrd) {
        ak_mrd_destroy(g_hal_priv.hMrd);
        g_hal_priv.hMrd = NULL;
    }
    g_hal_priv.day_night_status = -1;
    return 0;
}

int hal_motion_detect_get_isp_md_info(void *md_info)
{
    /* Chip-specific ISP MD information retrieval.
     * If needed, this can call AK_MPI_ISP_GetMdStat.
     * Current implementation relies on callbacks providing ISP_MD_INFO_S.
     */
    (void)md_info;
    return 0;
}

int hal_motion_detect_process_mrd(unsigned short md_stat[][MD_SIZE], int vsize,
                                   void *boxes, int max_boxes)
{
    if (!g_hal_priv.hMrd || !md_stat) {
        return -1;
    }

    /* Chip-specific MRD invocation. */
    MRD_RECTANGLE mrd_boxes[HAL_MOTION_DETECT_MAX_CAP] = {0};
    int num_detected = ak_mrd(g_hal_priv.hMrd, md_stat, vsize, mrd_boxes);

    if (num_detected <= 0) {
        return 0;
    }

    if (num_detected > max_boxes) {
        num_detected = max_boxes;
    }

    /* Convert MRD_RECTANGLE to hal_motion_detect_box_t. */
    hal_motion_detect_box_t *out_boxes = (hal_motion_detect_box_t *)boxes;
    for (int i = 0; i < num_detected; i++) {
        out_boxes[i].left = mrd_boxes[i].left;
        out_boxes[i].top = mrd_boxes[i].top;
        out_boxes[i].right = mrd_boxes[i].right;
        out_boxes[i].bottom = mrd_boxes[i].bottom;
    }

    return num_detected;
}

int hal_motion_detect_set_mrd_day_mode(void)
{
    if (!g_hal_priv.hMrd) {
        return -1;
    }

    /* Chip-specific MRD configuration for day mode. */
    ak_mrd_set_filters(g_hal_priv.hMrd, 10000, 5000);
    ak_mrd_set_motion_region_size(g_hal_priv.hMrd, 12, 300);
    ak_mrd_set_mode(g_hal_priv.hMrd, 0);
    ak_mrd_set_floating_wadding_params(g_hal_priv.hMrd, 3, 2, 4, 36, 2, 3);

    return 0;
}

int hal_motion_detect_set_mrd_night_mode(void)
{
    if (!g_hal_priv.hMrd) {
        return -1;
    }

    /* Chip-specific MRD configuration for night mode. */
    ak_mrd_set_filters(g_hal_priv.hMrd, 15000, 6000);
    ak_mrd_set_motion_region_size(g_hal_priv.hMrd, 50, 300);
    ak_mrd_set_mode(g_hal_priv.hMrd, 0);
    ak_mrd_set_floating_wadding_params(g_hal_priv.hMrd, 4, 3, 5, 36, 2, 3);

    return 0;
}

int hal_motion_detect_get_day_night_status(void)
{
    /* Day/night status is now managed by HW layer (motion_detect_hw.c).
     * This function returns the cached status from HAL private data.
     * HW layer should call hal_motion_detect_set_day_night_status() to update it.
     */
    return g_hal_priv.day_night_status;
}

int hal_motion_detect_set_day_night_status(int status)
{
    /* Set day/night status and update MRD mode accordingly.
     * Called by HW layer after getting status from framework layer.
     * status: 0=day, 1=night, <0=unknown/error
     */
    if (status < 0) {
        return -1;
    }

    if (g_hal_priv.day_night_status == status) {
        return 0;  /* No change */
    }

    g_hal_priv.day_night_status = status;

    /* Update MRD mode based on day/night status */
    if (status == STATE_DAY) {
        hal_motion_detect_set_mrd_day_mode();
    } else if (status == STATE_NIGHT) {
        hal_motion_detect_set_mrd_night_mode();
    }

    return 0;
}

int hal_motion_detect_get_vsize(int *vsize)
{
    /* Chip-specific MD vertical size retrieval (from video encoder parameters). */
    if (vsize == NULL) {
        return -1;
    }

    /* NOTE: original implementation used aa_ls_get_video_enc_param in framework layer.
     * HAL layer currently uses a default value. A future implementation could pass
     * video parameters down or use a callback.
     */
    *vsize = 24;

    return 0;
}

/* ========== MRD direct interface implementation ========== */

hal_mrd_handle_t hal_mrd_init(void)
{
    void *handle = ak_mrd_init();
    if (!handle) {
        log_error("ak_mrd_init failed");
        return NULL;
    }
    return (hal_mrd_handle_t)handle;
}

int hal_mrd_destroy(void *handle)
{
    if (!handle) {
        return -1;
    }
    return ak_mrd_destroy((void*)handle);
}

int hal_mrd_set_filters(void *handle, unsigned short flt_big, unsigned short flt_small)
{
    if (!handle) {
        return -1;
    }
    return ak_mrd_set_filters((void*)handle, flt_big, flt_small);
}

int hal_mrd_set_motion_region_size(void *handle, int size_min, int size_max)
{
    if (!handle) {
        return -1;
    }
    return ak_mrd_set_motion_region_size((void*)handle, size_min, size_max);
}

int hal_mrd_set_mode(void *handle, unsigned char mode)
{
    if (!handle) {
        return -1;
    }
    return ak_mrd_set_mode((void*)handle, mode);
}

int hal_mrd(void *handle, unsigned short md_stat[][64], int len, void *boxes)
{
    if (!handle || !md_stat || !boxes) {
        return -1;
    }
    
    /* Call ak_mrd and convert returned MRD_RECTANGLE to hal_motion_detect_box_t. */
    MRD_RECTANGLE mrd_boxes[HAL_MOTION_DETECT_MAX_CAP] = {0};
    int num_detected = ak_mrd((void*)handle, md_stat, len, mrd_boxes);
    
    if (num_detected <= 0) {
        return num_detected;
    }
    
    /* Convert MRD_RECTANGLE to hal_motion_detect_box_t. */
    int max_boxes = (num_detected > HAL_MOTION_DETECT_MAX_CAP) ? HAL_MOTION_DETECT_MAX_CAP : num_detected;
    hal_motion_detect_box_t *out_boxes = (hal_motion_detect_box_t *)boxes;
    for (int i = 0; i < max_boxes; i++) {
        out_boxes[i].left = mrd_boxes[i].left;
        out_boxes[i].top = mrd_boxes[i].top;
        out_boxes[i].right = mrd_boxes[i].right;
        out_boxes[i].bottom = mrd_boxes[i].bottom;
    }
    
    return num_detected;
}

/* Note: API layer (hal_motion_detect_api.h) directly calls the implementation functions above.
 * No wrapper functions needed as function names are now identical. */


