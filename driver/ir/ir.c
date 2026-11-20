// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/driver/ir.
 *
 * IR (IRCUT & IRLED) driver module implementation.
 * - Implements ir_device_t methods for IR operations.
 * - Dispatches calls to platform-specific HAL.
 */

#include "ir.h"
#include "ir_hal.h"
#include <string.h>
#include <stdlib.h>
#include "log.h"
#include "ak_common.h"
#include "driver_error.h"

#define LOG_TAG "IR_DRIVER"

/* ============================================================================
 * IRCUT DEVICE METHODS
 * ============================================================================ */

/**
 * @brief Initialize IRCUT.
 */
static int hw_ir_ircut_init(struct ir_device *device, const char* cfg_path)
{
    if (!device) {
        log_error("device is NULL");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    if (!cfg_path) {
        log_error("cfg_path is NULL");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    int ret = hal_ir_init(cfg_path);
    return (ret == 0) ? HARDWARE_DRIVER_OK : HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
}

/**
 * @brief Set IRCUT status.
 */
static int hw_ir_ircut_set(struct ir_device *device, int status)
{
    if (!device) {
        log_error("device is NULL");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    if (status != 0 && status != 1) {
        log_error("Invalid status: %d (must be 0 or 1)", status);
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    int ret = hal_ir_set_ircut(status);
    return (ret == 0) ? HARDWARE_DRIVER_OK : HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
}

/**
 * @brief Set IRCUT switch time.
 */
static int hw_ir_ircut_set_time(struct ir_device *device, int time_ms)
{
    if (!device) {
        log_error("device is NULL");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    hal_ir_set_ircut_time(time_ms);
    return HARDWARE_DRIVER_OK;
}

/**
 * @brief Get IRCUT switch time.
 */
static int hw_ir_ircut_get_time(struct ir_device *device, int* time_ms)
{
    if (!device) {
        log_error("device is NULL");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    if (!time_ms) {
        log_error("time_ms is NULL");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    int ret = hal_ir_get_ircut_time(time_ms);
    return (ret == 0) ? HARDWARE_DRIVER_OK : HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
}

/* ============================================================================
 * IRLED DEVICE METHODS
 * ============================================================================ */

/**
 * @brief Initialize IRLED.
 */
static int hw_ir_irled_init(struct ir_device *device, const irled_param_t* param)
{
    if (!device) {
        log_error("device is NULL");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    hal_irled_param_t hal_param = {0};

    if (param) {
        hal_param.working_level = param->working_level;
    } else {
        /* Use default value: high level active */
        hal_param.working_level = 1;
    }

    int ret = hal_irled_init(&hal_param);
    return (ret == 0) ? HARDWARE_DRIVER_OK : HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
}

/**
 * @brief Get IRLED status.
 */
static int hw_ir_irled_get_status(struct ir_device *device, int* status)
{
    if (!device) {
        log_error("device is NULL");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    if (!status) {
        log_error("status is NULL");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    int ret = hal_irled_get_status(status);
    return (ret == 0) ? HARDWARE_DRIVER_OK : HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
}

/**
 * @brief Set IRLED status.
 */
static int hw_ir_irled_set_status(struct ir_device *device, int status)
{
    if (!device) {
        log_error("device is NULL");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    if (status != 0 && status != 1) {
        log_error("Invalid status: %d (must be 0 or 1)", status);
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    int ret = hal_irled_set_status(status);
    return (ret == 0) ? HARDWARE_DRIVER_OK : HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
}

/* ============================================================================
 * DEVICE CLOSE
 * ============================================================================ */

/**
 * @brief Close IR device.
 */
static int hw_ir_close(struct hw_device_t *device)
{
    if (!device) {
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    free(device);
    return HARDWARE_DRIVER_OK;
}

/* ============================================================================
 * MODULE OPEN AND INITIALIZATION
 * ============================================================================ */

/**
 * @brief Open IR device.
 * @param module Hardware module.
 * @param name Device name (not used for IR module).
 * @param device Output device handle.
 * @return 0 on success, negative error code on failure.
 */
static int hw_ir_open(const hw_module_t* module, const char* name, hw_device_t** device)
{
    (void)name;  /* IR module does not require device name */

    if ((NULL == module) || (NULL == device)) {
        log_error("parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    struct ir_device *dev = calloc(1, sizeof(struct ir_device));
    if (NULL == dev) {
        log_error("malloc for ir_device failed");
        return HARDWARE_DRIVER_ERROR_MEMORY;
    }

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = IR_DEVICE_API_VERSION_1_0;
    dev->common.module = (struct hw_module_t *) module;
    dev->common.close = hw_ir_close;

    /* Set IRCUT methods */
    dev->ircut_init = hw_ir_ircut_init;
    dev->ircut_set = hw_ir_ircut_set;
    dev->ircut_set_time = hw_ir_ircut_set_time;
    dev->ircut_get_time = hw_ir_ircut_get_time;

    /* Set IRLED methods */
    dev->irled_init = hw_ir_irled_init;
    dev->irled_get_status = hw_ir_irled_get_status;
    dev->irled_set_status = hw_ir_irled_set_status;

    *device = &dev->common;

    return HARDWARE_DRIVER_OK;
}

static struct hw_module_methods_t hw_module_methods = {
    .open = hw_ir_open,
};

static struct ir_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = IR_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = IR_HARDWARE_MODULE_ID,
        .name = "IR Module",
        .author = "Anyka",
        .methods = &hw_module_methods,
    },
};

/**
 * @brief Initialize IR module.
 * @return 0 on success, negative error code on failure.
 */
int hw_ir_module_init(void)
{
    int ret = hw_register_module(&(HAL_MODULE_INFO_SYM.common));
    return (ret == 0) ? HARDWARE_DRIVER_OK : HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
}
