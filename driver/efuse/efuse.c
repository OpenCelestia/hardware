// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/driver/efuse.
 *
 * eFuse driver module implementation.
 * - Implements efuse_device_t methods for eFuse operations.
 * - Supports reading/writing chip ID.
 */

#include "efuse.h"

#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
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

#include "ak_efuse.h"

#define LOG_TAG "HAL EFUSE"

#define EFUSE_DEVICE_NAME               "/dev/efuse"

static int open_device(const char* name)
{
    if (!name) {
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    char path[64] = {0};
    snprintf(path, sizeof(path), "/dev/%s", name);

    struct stat st;

    if (stat(path, &st) != 0) {
        log_error("device %s not exist", path);
        return HARDWARE_DRIVER_ERROR_NOT_FOUND;
    }

    /* open device */
    int fd = open(path, O_RDWR);
    if (fd < 0)
    {
        log_error("failed to open device");
        return HARDWARE_DRIVER_ERROR_IO;
    }

    return fd;
}

/**
 * @brief Get ID length from eFuse.
 */
static int hw_efuse_get_length(struct efuse_device *device, const char *name, const char *option, uint32_t *length)
{
    if ((NULL == device) || (NULL == name) || (NULL == option) || (NULL == length))
    {
        log_error("Parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    int fd = open_device(name);
    if (fd < 0)
    {
        log_error("failed to open device");
        return fd;
    }

    int ret = HARDWARE_DRIVER_ERROR_NOT_SUPPORTED;

    if (0 == strcmp("global", option))
    {
        ret = ioctl(fd, DO_GLOBE_ID_GET_LEN, length);
    }
    else if (0 == strcmp("customer", option))
    {
        ret = ioctl(fd, DO_CUSTOMER_ID_GET_LEN, length);
    }
    else
    {
        log_error("Unsupported id");
    }

    int result = HARDWARE_DRIVER_OK;
    if (ret < 0)
    {
        log_error("ioctl get length failed");
        result = HARDWARE_DRIVER_ERROR_IO;
    }

    close(fd);

    return result;
}

/**
 * @brief Get ID from eFuse.
 */
static int hw_efuse_get_id(struct efuse_device *device, const char *name, const char *option, char *buffer)
{
    if ((NULL == device) || (NULL == name) || (NULL == option) || (NULL == buffer))
    {
        log_error("Parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    int fd = open_device(name);
    if (fd < 0)
    {
        log_error("failed to open device");
        return fd;
    }

    int ret = HARDWARE_DRIVER_ERROR_NOT_SUPPORTED;

    if (0 == strcmp("global", option))
    {
        ret = ioctl(fd, DO_GLOBE_ID_GET, buffer);
    }
    else if (0 == strcmp("customer", option))
    {
        ret = ioctl(fd, DO_CUSTOMER_ID_GET, buffer);
    }
    else
        log_error("Unsupported id");

    int result = (ret >= 0) ? HARDWARE_DRIVER_OK : HARDWARE_DRIVER_ERROR_IO;

    close(fd);

    return result;
}

/**
 * @brief Set ID to eFuse.
 */
static int hw_efuse_set_id(struct efuse_device *device, const char *name, const char *option, char *buffer)
{
    if ((NULL == device) || (NULL == name) || (NULL == option) || (NULL == buffer))
    {
        log_error("Parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    int fd = open_device(name);
    if (fd < 0)
    {
        log_error("failed to open device");
        return fd;
    }

    int result = HARDWARE_DRIVER_ERROR_NOT_SUPPORTED;

    if (0 == strcmp("customer", option))
    {
        char locked = 0;
        int ret = ioctl(fd, DO_READ_LOCK, &locked);
        if (ret < 0)
        {
            log_error("failed to read lock bit of device");
            result = HARDWARE_DRIVER_ERROR_IO;
        }
        else if (locked)
        {
            log_error("customer id is locked");
            result = HARDWARE_DRIVER_ERROR_INVALID_STATE;
        }
        else
        {
            ret = ioctl(fd, DO_CUSTOMER_ID_SET, buffer);
            if (ret < 0)
            {
                log_error("set customer id failed");
                result = HARDWARE_DRIVER_ERROR_IO;
            }
            else
            {
                ret = ioctl(fd, DO_WRITE_FINISH, NULL);
                result = (ret < 0) ? HARDWARE_DRIVER_ERROR_IO : HARDWARE_DRIVER_OK;
            }
        }
    }
    else
        log_error("Unsupported id");

    close(fd);

    return result;
}

/**
 * @brief Close eFuse device.
 */
static int hw_efuse_close(struct hw_device_t* device)
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
 * @brief Open eFuse device.
 * @param module Hardware module.
 * @param name Device name (can be NULL).
 * @param device Output device handle.
 * @return 0 on success, negative error code on failure.
 */
static int hw_efuse_open(const struct hw_module_t* module, const char* name, struct hw_device_t** device)
{
    (void)name;
    if ((NULL == module) || (NULL == device))
    {
        log_error("Parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    struct efuse_device *dev = calloc(1, sizeof(struct efuse_device));
    if (!dev)
        return HARDWARE_DRIVER_ERROR_MEMORY;

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = EFUSE_DEVICE_API_VERSION_1_0;
    dev->common.module = (struct hw_module_t *)module;
    dev->common.close = hw_efuse_close;

    dev->get_length = hw_efuse_get_length;
    dev->get_id = hw_efuse_get_id;
    dev->set_id = hw_efuse_set_id;

    *device = &dev->common;

    return HARDWARE_DRIVER_OK;
}

static struct hw_module_methods_t hw_module_methods = {
    .open = hw_efuse_open,
};

static struct efuse_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = EFUSE_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = EFUSE_HARDWARE_MODULE_ID,
        .name = "Efuse Module",
        .author = "Anyka",
        .methods = &hw_module_methods,
    },
};

/**
 * @brief Initialize eFuse module.
 * @return 0 on success, negative error code on failure.
 */
int hw_efuse_module_init(void)
{
    int ret = hw_register_module(&(HAL_MODULE_INFO_SYM.common));
    return (ret == 0) ? HARDWARE_DRIVER_OK : HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
}


