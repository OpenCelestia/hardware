// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/driver/pwm.
 *
 * PWM driver module implementation.
 * - Implements pwm_device_t methods for PWM operations.
 * - Supports Linux sysfs PWM interface.
 */

#include "pwm.h"

#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <getopt.h>

#if CONFIG_OS_LINUX
#include <linux/kernel.h>
#endif

#include "ak_common.h"
#include "log.h"
#include "driver_error.h"

#define LOG_TAG "HAL PWM"

#if CONFIG_OS_RTOS
#include "pwm_ioctl.h"
#endif

#define PWM_DEV_MAX_NUM             10
#define PERIOD_VALUE_MIN            167L
#define PERIOD_VALUE_MAX            10869565L

typedef enum channel {
    eDEVICE_CHANNEL0            = 0,
    eDEVICE_CHANNEL1,
    eDEVICE_CHANNEL2,
    eDEVICE_CHANNEL3,
    eDEVICE_CHANNEL4,
#if (defined (CONFIG_MACH_AK37E))
    eDEVICE_CHANNEL5,
#endif
    eDEVICE_CHANNEL_MAX
}device_channel_t;

static int open_device(const char* name)
{
    if (!name) {
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    int fd = -1;
    int ret = HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
    int channel = -1;
    char path[64] = {0};

    channel = atoi(name);
    if ((channel < eDEVICE_CHANNEL0) || (channel >= eDEVICE_CHANNEL_MAX))
    {
        log_error("Unsupported device");
        return HARDWARE_DRIVER_ERROR_NOT_SUPPORTED;
    }

#if CONFIG_OS_RTOS
    snprintf(path, sizeof(path), "/dev/pwm%d", channel);

    if (access(path, F_OK))
    {
        log_error("Access pwm%d failed.", channel);
        return HARDWARE_DRIVER_ERROR_NOT_FOUND;
    }

    fd = open(path, O_RDWR);
    if (fd < 0)
    {
        log_error("open pwm%d failed.", channel);
        return HARDWARE_DRIVER_ERROR_IO;
    }

    return fd;
#else

    snprintf(path, sizeof(path), "/sys/class/pwm/pwmchip%d/pwm0", channel);

    if (!access(path, F_OK))
        return HARDWARE_DRIVER_OK;

    /*
    *create pwm dev use export;
    *close pwm dev use unexport.
    */
    snprintf(path, sizeof(path), "/sys/class/pwm/pwmchip%d/export", channel);

    fd = open(path, O_WRONLY);
    if (fd < 0)
    {
        log_error("Open pwm channel %d fail", channel);
        return HARDWARE_DRIVER_ERROR_IO;
    }

    /*
    *write 0 to export, it will create pwm0 folder.
    */
    ret = write(fd, "0", strlen("0"));
    if (ret < 0)
        log_error("failed to export pwm device %s\n", name);

    close(fd);

    return (ret < 0) ? HARDWARE_DRIVER_ERROR_IO : HARDWARE_DRIVER_OK;
#endif
}

/**
 * @brief Configure PWM device.
 */
static int hw_pwm_config(struct pwm_device *device, const char *name, struct pwm_info *info)
{
    if ((NULL == device) || (NULL == name) || (NULL == info) || (info->period <= 0) || (info->duty < 0))
    {
        log_error("Parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    if (info->period < PERIOD_VALUE_MIN || info->period > PERIOD_VALUE_MAX)
    {
        log_error("period error, must in [%ld-%ld]", PERIOD_VALUE_MIN, PERIOD_VALUE_MAX);
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    int prep = open_device(name);
    if (prep < 0 && prep != HARDWARE_DRIVER_OK)
        return prep;

    uint32_t duty = info->duty;
    uint32_t period = info->period;

    if (duty > period)
        duty = period;

#if CONFIG_OS_RTOS
    int fd = prep;
    int channel = atoi(name);
    T_PWM_CHANNEL_Info pwm_info;

    pwm_info.pwm_id = channel;
    pwm_info.duty_cycle = duty * 100 / period;
    pwm_info.pwm_freq = 1000000000UL / period;

    log_info("[set pwm info] pwm dev: %s, duty_cycle: %g, pwm_freq: %g", name,
             pwm_info.duty_cycle, pwm_info.pwm_freq);

    int ret = ioctl(fd, IO_PWM_SET_CHANNEL, &pwm_info);

    close(fd);
    return (ret < 0) ? HARDWARE_DRIVER_ERROR_IO : HARDWARE_DRIVER_OK;
#else
    (void)prep;
    int channel = atoi(name);
    char path[64] = {0};
    char str[64] = {0};
    int fd = -1;
    int ret = 0;

    snprintf(path, sizeof(path), "/sys/class/pwm/pwmchip%d/pwm0/enable", channel);
    fd = open(path, O_RDWR);
    if (fd < 0)
    {
        log_error("open pwm%d failed.", channel);
        return HARDWARE_DRIVER_ERROR_IO;
    }

    memset(str, 0, sizeof(str));
    ret = read(fd, str, sizeof(str) - 1);
    if (ret < 0)
    {
        log_error("failed to read pwm%d", channel);
        close(fd);
        return HARDWARE_DRIVER_ERROR_IO;
    }

    if (strncmp(str, "1", 1) == 0)
    {
        if (write(fd, "0", 1) < 0)
        {
            log_error("failed to disable pwm%d.", channel);
            close(fd);
            return HARDWARE_DRIVER_ERROR_IO;
        }
    }

    close(fd);

    snprintf(path, sizeof(path), "/sys/class/pwm/pwmchip%d/pwm0/period", channel);
    fd = open(path, O_RDWR);
    if (fd < 0)
    {
        log_error("open period file of pwm%d failed.", channel);
        return HARDWARE_DRIVER_ERROR_IO;
    }

    snprintf(str, sizeof(str), "%u", period);
    if (write(fd, str, strlen(str)) < 0)
    {
        log_error("set period to pwm%d failed.", channel);
        close(fd);
        return HARDWARE_DRIVER_ERROR_IO;
    }

    close(fd);

    snprintf(path, sizeof(path), "/sys/class/pwm/pwmchip%d/pwm0/duty_cycle", channel);
    fd = open(path, O_RDWR);
    if (fd < 0)
    {
        log_error("open duty file of pwm%d failed.", channel);
        return HARDWARE_DRIVER_ERROR_IO;
    }

    snprintf(str, sizeof(str), "%u", duty);
    if (write(fd, str, strlen(str)) < 0)
    {
        log_error("set duty to pwm%d failed.", channel);
        close(fd);
        return HARDWARE_DRIVER_ERROR_IO;
    }

    close(fd);
    return HARDWARE_DRIVER_OK;
#endif
}

/**
 * @brief Enable or disable PWM device.
 */
static int hw_pwm_enable(struct pwm_device *device, const char *name, uint32_t enable)
{
    if (NULL == device)
    {
        log_error("Parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    int prep = open_device(name);
    if (prep < 0)
        return prep;

    int ret = HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
    int channel = atoi(name);
    int fd = -1;
#if CONFIG_OS_RTOS

    fd = prep;
    T_PWM_CHANNEL_Info pwm_info;
    pwm_info.pwm_id = channel;

    if (enable)
        ret = ioctl(fd, IO_PWM_SET_CHANNEL, &pwm_info);
    else
        ret = ioctl(fd, IO_PWM_CLOSE_CHANNEL, &pwm_info);
#else

    char path[64] = {0};

    snprintf(path, sizeof(path), "/sys/class/pwm/pwmchip%d/pwm0/enable", channel);
    fd = open(path, O_RDWR);
    if (fd < 0)
    {
        log_error("open enable file of pwm%d failed.", channel);
        return HARDWARE_DRIVER_ERROR_IO;
    }

    if (enable)
        ret = write(fd, "1", 1);
    else
        ret = write(fd, "0", 1);

    if (ret < 0)
        log_error("enable pwm%d failed.", channel);
#endif

    close(fd);

    return (ret < 0) ? HARDWARE_DRIVER_ERROR_IO : HARDWARE_DRIVER_OK;
}

/**
 * @brief Close PWM device.
 */
static int hw_pwm_close(struct hw_device_t* device)
{
    if (NULL == device)
    {
        log_error("Parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    free(device);

    return HARDWARE_DRIVER_OK;
}

/**
 * @brief Open PWM device.
 * @param module Hardware module.
 * @param name Device name (can be NULL).
 * @param device Output device handle.
 * @return 0 on success, negative error code on failure.
 */
int hw_pwm_open(const struct hw_module_t* module, const char* name, struct hw_device_t** device)
{
    if ((NULL == module) || (NULL == device))
    {
        log_error("Parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    struct pwm_device *dev;
    dev = calloc(1, sizeof(struct pwm_device));
    if (!dev)
        return HARDWARE_DRIVER_ERROR_MEMORY;

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = PWM_DEVICE_API_VERSION_1_0;
    dev->common.module = (struct hw_module_t *)module;
    dev->common.close = hw_pwm_close;

    dev->config = hw_pwm_config;
    dev->enable = hw_pwm_enable;

    *device = &dev->common;

    return HARDWARE_DRIVER_OK;
}

static struct hw_module_methods_t hw_module_methods = {
    .open = hw_pwm_open,
};

static struct pwm_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = PWM_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = PWM_HARDWARE_MODULE_ID,
        .name = "PWM Module",
        .author = "Anyka",
        .methods = &hw_module_methods,
    },
};

/**
 * @brief Initialize PWM module.
 * @return 0 on success, negative error code on failure.
 */
int hw_pwm_module_init(void)
{
    int ret = hw_register_module(&(HAL_MODULE_INFO_SYM.common));
    return (ret == 0) ? HARDWARE_DRIVER_OK : HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
}





