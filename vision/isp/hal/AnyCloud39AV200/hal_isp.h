/**
 * @file hal_isp.h
 * @brief ISP HAL implementation header for AnyCloud39AV200
 * @details Chip-specific ISP operations and soft photo sensor parameters.
 */

#ifndef HARDWARE_HAL_ISP_HAL_ISP_ANYCLOUD39AV200_H
#define HARDWARE_HAL_ISP_HAL_ISP_ANYCLOUD39AV200_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/** @brief ISP HAL handle type (opaque pointer owned by HAL implementation). */
typedef void *hal_isp_handle_t;

/**
 * @brief ISP day/night mode enumeration (maps to ISP_DAY_NIGHT_MODE_E in ak_vpss_isp.h).
 */
typedef enum {
    HAL_ISP_MODE_DAY = 0,
    HAL_ISP_MODE_NIGHT,
    HAL_ISP_MODE_USR_DEFINE_1,
    HAL_ISP_MODE_USR_DEFINE_2,
    HAL_ISP_MODE_USR_DEFINE_3,
} hal_isp_mode_t;

/* ISP motion detection dimension constants (mapped from ak_vpss_isp.h). */
#define HAL_ISP_MD_DIMENSION_H_MAX  64
#define HAL_ISP_MD_DIMENSION_V_MAX  36

/**
 * @brief ISP motion detect information (corresponds to ISP_MD_INFO_S in ak_vpss_isp.h).
 */
typedef struct {
    uint16_t u16Stat[HAL_ISP_MD_DIMENSION_V_MAX][HAL_ISP_MD_DIMENSION_H_MAX];
} hal_isp_md_info_t;

/**
 * @brief Software photo sensor parameters (maps to ISP_SOFT_PS_ATTR_S in ak_vpss_isp.h).
 */
typedef struct {
    uint32_t enNightType;              /* ISP_NIGHT_TYPE_E */
    uint32_t u32DayCheckFrameNum;
    uint32_t u32NightCheckFrameNum;
    uint32_t u32ExpStableCheckFrameNum;
    int32_t  s32ExpStableTimeOutMs;
    struct {
        uint32_t u32DayToNightLum;
        uint32_t u32NightToDayLum;
        uint32_t u32AwbNightDisMax;
        uint32_t u32AwbNightDisMin;
        uint32_t u32GainR;
        uint32_t u32GainB;
        int32_t  s32OffsetR;
        int32_t  s32OffsetB;
    } stIrLedNightParm;
} hal_isp_soft_ps_param_t;

/* Initialization / deinitialization. */
void *hal_isp_init(int dev_id, void* config);
int hal_isp_deinit(void *h);

/* Generic parameter setting API. */
int hal_isp_set_param(void *h, int key, long value);

/* Get frame or statistics data from ISP. */
int hal_isp_get_frame(void *h, void* frame_out);

/* Switch ISP mode (day / night / user defined). */
int hal_isp_switch_mode(void *h, hal_isp_mode_t mode);

/* Set software photo sensor parameters. */
int hal_isp_set_soft_ps_param(void *h, const hal_isp_soft_ps_param_t* param);

/* Get software photo sensor result (day / night). */
int hal_isp_get_soft_ps_level(void *h, int pre_status, int* day_night);

/* ========== Direct interfaces (without handle, use dev_id directly) ========== */
/* These interfaces create a temporary handle internally and are kept for compatibility. */

/* Switch ISP mode (day / night) - direct interface. */
int hal_isp_switch_mode_direct(int dev_id, int mode);

/* Set software photo sensor parameters - direct interface. */
int hal_isp_set_soft_ps_param_direct(int dev_id, const void *param);

/* Get software photo sensor result (day / night) - direct interface. */
int hal_isp_get_soft_ps_level_direct(int dev_id, int pre_status, int *day_night);

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_HAL_ISP_HAL_ISP_ANYCLOUD39AV200_H */


