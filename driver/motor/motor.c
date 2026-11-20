// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/driver/motor.
 *
 * Motor driver module implementation.
 * - Implements motor_device_t methods for stepper motor control.
 * - Supports step/angle control, limit setting, and status query.
 */

#include "motor.h"

#if CONFIG_OS_LINUX
#include <linux/kernel.h>
#endif
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <limits.h>
#include <sys/ioctl.h>

#include "log.h"
#include "ak_common.h"
#include "hardware.h"
#include "driver_error.h"

#if CONFIG_OS_LINUX
/*
 * The motor driver Makefile adds `hal/$(CONFIG_SOC_PLATFORM)` to the include
 * path, so the SoC-specific ak_motor.h is resolved automatically.  Avoid
 * hard-coding a single platform path so AV100_V2 / AV200 builds stay
 * consistent with the selected platform.
 */
#include "ak_motor.h"
#else
#include "motor_ioctl.h"
#endif

#define LOG_TAG "HAL MOTOR"

static int open_device(const char* name)
{
    if (!name) {
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    char path[32] = {0};
    snprintf(path, sizeof(path), "/dev/%s", name);

    if (access(path, F_OK) != 0)
    {
        log_error("device %s not exist", path);
        return HARDWARE_DRIVER_ERROR_NOT_FOUND;
    }

    int fd = -1;
    fd = open(path, O_RDWR);
    if (fd < 0)
    {
        log_error("failed to open %s", name);
        return HARDWARE_DRIVER_ERROR_IO;
    }

    return fd;
}

/**
 * @brief Configure motor device.
 */
static int hw_motor_config(struct motor_device *device, const char* name, struct motor_config *config)
{
    if ((NULL == device) || (NULL == name) || (NULL == config))
    {
        log_error("parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    int fd = open_device(name);
    if (fd < 0)
        return fd;

    int ret = ioctl(fd, MOTOR_PARM, (struct motor_parm *)config);
    if (ret < 0)
        log_error("failed to set angle of device");

    close(fd);

    return (ret < 0) ? HARDWARE_DRIVER_ERROR_IO : HARDWARE_DRIVER_OK;
}

/**
 * @brief Set motor step value.
 */
static int hw_motor_set_step(struct motor_device *device, const char* name, int value)
{
    if ((NULL == device) || (NULL == name))
    {
        log_error("parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    static int v = 0;
    if (value == v)
        return HARDWARE_DRIVER_OK;

    v = value;

    int fd = open_device(name);
    if (fd < 0)
        return fd;

    int ret = ioctl(fd, MOTOR_SPEED_STEP, &v);
    if (ret < 0)
        log_error("failed to set angle of device");

    close(fd);

    return (ret < 0) ? HARDWARE_DRIVER_ERROR_IO : HARDWARE_DRIVER_OK;
}

/**
 * @brief Set motor angle value.
 */
static int hw_motor_set_angle(struct motor_device *device, const char* name, int value)
{
    if ((NULL == device) || (NULL == name))
    {
        log_error("parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    static int v = 0;
    if (value == v)
        return HARDWARE_DRIVER_OK;

    v = value;

    int fd = open_device(name);
    if (fd < 0)
        return fd;

    int ret = ioctl(fd, MOTOR_SPEED_ANGLE, &v);
    if (ret < 0)
        log_error("failed to set angle of device");

    close(fd);

    return (ret < 0) ? HARDWARE_DRIVER_ERROR_IO : HARDWARE_DRIVER_OK;
}

/**
 * @brief Set motor limit.
 */
static int hw_motor_set_limit(struct motor_device *device, const char* name, bool enable, int value)
{
    if ((NULL == device) || (NULL == name))
    {
        log_error("parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    int fd = open_device(name);
    if (fd < 0)
        return fd;

    static int v = 0;
    v = value;

    int ret = enable ? ioctl(fd, MOTOR_MOVE_LIMIT, &v)
                     : ioctl(fd, MOTOR_MOVE_NOLIMIT, &v);

    if (ret < 0)
        log_error("faile to set limit of device");

    close(fd);

    return (ret < 0) ? HARDWARE_DRIVER_ERROR_IO : HARDWARE_DRIVER_OK;
}

/**
 * @brief Stop motor and wait for completion.
 */
static int hw_motor_stop(struct motor_device *device, const char* name)
{
    if ((NULL == device) || (NULL == name))
    {
        log_error("parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    int fd = open_device(name);
    if (fd < 0)
        return fd;

    int ret = ioctl(fd, MOTOR_STOP, NULL);
    if (ret < 0)
        log_error("failed to stop device");

    close(fd);

    return (ret < 0) ? HARDWARE_DRIVER_ERROR_IO : HARDWARE_DRIVER_OK;
}

/**
 * @brief Get motor information.
 */
static int hw_motor_get_info(struct motor_device *device, const char* name, struct motor_info *info)
{
    if ((NULL == device) || (NULL == name) || (NULL == info))
    {
        log_error("parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    int fd = open_device(name);
    if (fd < 0)
        return fd;

    int ret = ioctl(fd, MOTOR_GET_STATUS, (struct motor_message *)info);
    if (ret < 0)
        log_error("failed to get information from device");

    close(fd);

    return (ret < 0) ? HARDWARE_DRIVER_ERROR_IO : HARDWARE_DRIVER_OK;
}

/**
 * @brief Close motor device.
 */
static int hw_motor_close(struct hw_device_t* device)
{
    if (NULL == device)
    {
        log_error("parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    free(device);

    return HARDWARE_DRIVER_OK;
}

/**
 * @brief Open motor device.
 * @param module Hardware module.
 * @param name Device name.
 * @param device Output device handle.
 * @return 0 on success, negative error code on failure.
 */
int hw_motor_open(const hw_module_t* module, const char* name, hw_device_t** device)
{
    *device = NULL;

    if ((NULL == module) || (NULL == device))
    {
        log_error("parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    struct motor_device *dev = calloc(1, sizeof(struct motor_device));
    if (NULL == dev)
    {
        log_error("malloc for information struct of device failed");
        return HARDWARE_DRIVER_ERROR_MEMORY;
    }

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = MOTOR_DEVICE_API_VERSION_1_0;
    dev->common.module = (struct hw_module_t *) module;
    dev->common.close = hw_motor_close;

    dev->config = hw_motor_config;
    dev->set_step = hw_motor_set_step;
    dev->set_angle = hw_motor_set_angle;
    dev->set_limit = hw_motor_set_limit;
    dev->stop = hw_motor_stop;
    dev->get_info = hw_motor_get_info;

    *device = &dev->common;

    return HARDWARE_DRIVER_OK;
}


static struct hw_module_methods_t hw_module_methods = {
    .open = hw_motor_open,
};

static struct motor_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = MOTOR_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = MOTOR_HARDWARE_MODULE_ID,
        .name = "Motor Module",
        .author = "Anyka",
        .methods = &hw_module_methods,
    },
};

/**
 * @brief Initialize motor module.
 * @return 0 on success, negative error code on failure.
 */
int hw_motor_module_init(void)
{
    int ret = hw_register_module(&(HAL_MODULE_INFO_SYM.common));
    return (ret == 0) ? HARDWARE_DRIVER_OK : HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
}



