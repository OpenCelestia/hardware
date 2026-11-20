// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/driver/adc.
 *
 * ADC driver module implementation.
 * - Implements adc_device_t methods for ADC operations.
 * - Supports Linux IIO and device node interfaces.
 */
#include "adc.h"

#include "unistd.h"
#include "fcntl.h"
#include <stdlib.h>
#include <string.h>
#if CONFIG_OS_LINUX
#include <linux/kernel.h>
#endif
#include <sys/ioctl.h>

#include "log.h"
#include "ak_common.h"
#include "driver_error.h"

#define LOG_TAG "HAL ADC"

/* Maximum length of file path */
#define FILE_PATH_MAX_LENGTH        128

#if CONFIG_OS_LINUX
#define DEVICE_PATH                 "/sys/bus/iio/devices/iio:device0/in_voltage%s_raw"
#else
#define DEVICE_PATH                 "/dev/ain%s"
#endif


static int open_device(const char* name)
{
    if (!name) {
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    char path[FILE_PATH_MAX_LENGTH] = {0};
    snprintf(path, sizeof(path), DEVICE_PATH, name);

    if (access(path, F_OK) != 0)
    {
        log_error("device %s not exist", path);
        return HARDWARE_DRIVER_ERROR_NOT_FOUND;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        log_error("failed to open %s", name);
        return HARDWARE_DRIVER_ERROR_IO;
    }

    return fd;
}

/**
 * @brief Read value from ADC device.
 */
static int hw_adc_read(struct adc_device *device, const char* name, uint8_t *buffer, uint32_t bytes)
{
    if ((NULL == device) || (NULL == name) || (NULL == buffer) || (bytes == 0))
    {
        log_error("parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    int fd = open_device(name);
    if (fd < 0)
        return fd;

    ssize_t ret = read(fd, buffer, bytes);
    close(fd);

    if (ret < 0)
    {
        log_error("failed to read from device");
        return HARDWARE_DRIVER_ERROR_IO;
    }

    return HARDWARE_DRIVER_OK;
}

/**
 * @brief Close ADC device.
 */
static int hw_adc_close(struct hw_device_t* device)
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
 * @brief Open ADC device.
 * @param module Hardware module.
 * @param name Device name (e.g., "ain0", "ain1", "bat").
 * @param device Output device handle.
 * @return 0 on success, negative error code on failure.
 */
static int hw_adc_open(const hw_module_t* module, const char* name, hw_device_t** device)
{
    (void)name;
    *device = NULL;

    if ((NULL == module) || (NULL == device))
    {
        log_error("parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    struct adc_device *dev = calloc(1, sizeof(struct adc_device));
    if (!dev)
    {
        log_error("failed to malloc for device");
        return HARDWARE_DRIVER_ERROR_MEMORY;
    }

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = ADC_DEVICE_API_VERSION_1_0;
    dev->common.module = (struct hw_module_t *)module;
    dev->common.close = hw_adc_close;

    dev->read = hw_adc_read;

    *device = &dev->common;

    return HARDWARE_DRIVER_OK;
}

static struct hw_module_methods_t hw_module_methods = {
    .open = hw_adc_open,
};

static struct adc_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = ADC_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = ADC_HARDWARE_MODULE_ID,
        .name = "ADC Module",
        .author = "Anyka",
        .methods = &hw_module_methods,
    },
};

/**
 * @brief Initialize ADC module.
 * @return 0 on success, negative error code on failure.
 */
int hw_adc_module_init(void)
{
    int ret = hw_register_module(&(HAL_MODULE_INFO_SYM.common));
    return (ret == 0) ? HARDWARE_DRIVER_OK : HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
}


