/**
 * @file hal_isp_api.h
 * @brief ISP HAL unified public interface (chip-agnostic, used by isp_hw)
 *
 * This file defines only function interfaces for ISP HAL layer.
 * All type definitions are in isp_hw.h (for logic layer) or hal_isp.h (for HAL internal).
 * Upper layer (`isp_hw.c`) should only depend on this file and not on chip HAL.
 */

#ifndef HARDWARE_HAL_ISP_API_H
#define HARDWARE_HAL_ISP_API_H

#include "isp_hw.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize ISP HAL handle.
 *
 * @param dev_id device ID (usually VIDEO_DEV0).
 * @param config reserved configuration pointer, may be NULL.
 * @return non-NULL handle on success, NULL on failure.
 */
void *hal_isp_init(int dev_id, void *config);

/**
 * @brief Deinitialize and free ISP HAL handle.
 *
 * @param h handle returned by @ref hal_isp_init.
 * @return 0 on success, non-zero on failure.
 */
int hal_isp_deinit(void *h);

/**
 * @brief Set generic ISP parameter.
 *
 * @param h     ISP HAL handle.
 * @param key   parameter key (semantics defined by HAL implementation).
 * @param value parameter value.
 * @return 0 on success, non-zero on failure.
 */
int hal_isp_set_param(void *h, int key, long value);

/**
 * @brief Get one frame or statistics from ISP.
 *
 * @param h         ISP HAL handle.
 * @param frame_out output buffer, allocated by caller.
 * @return 0 on success, non-zero on failure.
 */
int hal_isp_get_frame(void *h, void *frame_out);

/**
 * @brief Switch ISP working mode (direct-call style API).
 *
 * @param dev_id device ID (usually VIDEO_DEV0).
 * @param mode   target mode (isp_mode_t).
 * @return 0 on success, non-zero on failure.
 */
int hal_isp_switch_mode_direct(int dev_id, int mode);

/**
 * @brief Set ISP soft photo sensor parameters (direct-call style API).
 *
 * @param dev_id device ID (usually VIDEO_DEV0).
 * @param param  soft photo sensor parameters (isp_soft_ps_param_t*), must not be NULL.
 * @return 0 on success, non-zero on failure.
 */
int hal_isp_set_soft_ps_param_direct(int dev_id, const void *param);

/**
 * @brief Calculate current soft photo sensor day/night status (direct-call).
 *
 * @param dev_id     device ID (usually VIDEO_DEV0).
 * @param pre_status previous status (implementation may use this for hysteresis).
 * @param day_night  output status: 0=day, 1=night.
 * @return 0 on success, non-zero on failure.
 */
int hal_isp_get_soft_ps_level_direct(int dev_id, int pre_status, int *day_night);

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_HAL_ISP_API_H */


