// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/driver/wdt.
 *
 * Watchdog timer driver module implementation.
 * - Implements wdt_device_t methods for watchdog operations.
 * - Supports Linux watchdog interface.
 */

#include "wdt.h"

#include <dirent.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#if CONFIG_OS_LINUX
#include <linux/kernel.h>
#endif

#include "log.h"
#include "ak_common.h"
#include "driver_error.h"

#define LOG_TAG "HAL WDT"

#define DEFAULT_TIMEOUT             4 /* Default timeout 4 seconds */ 
#define WATCHDOG_DEVICE             "/dev/watchdog"

#if CONFIG_OS_RTOS
#include "wtd_ioctl.h"
#define WATCHDOG_SET_TIME_OUT       WATCHDOG_SET_TIME
#define WATCHDOG_KEEP_ALIVE         WATCHDOG_FEED
#else
#include <linux/watchdog.h>
#define WDT_IOC                     'W'

#define WATCHDOG_KEEP_ALIVE         _IOR(WDT_IOC, 5, int)
#define WATCHDOG_SET_TIME_OUT       _IOWR(WDT_IOC, 6, int)
#define WATCHDOG_GET_TIME_OUT       _IOR(WDT_IOC, 7, int)
#endif


static int open_device(const char* name)
{
    if (!name) {
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    int fd = -1;
    char path[64] = {0};

    snprintf(path, sizeof(path), "/dev/%s", name);

    if (access(path, F_OK) != 0)
    {
        log_error("device %s not exist", path);
        return HARDWARE_DRIVER_ERROR_NOT_FOUND;
    }

    /* open device */
    fd = open(path, O_RDWR);
    if (fd < 0)
    {
        log_error("open watchdog timer device failed!");
        return HARDWARE_DRIVER_ERROR_IO;
    }

    return fd;
}

/**
 * @brief Start watchdog timer.
 */
static int hw_wdt_start(struct wdt_device *device, const char* name, struct timeval *timeout)
{
    if ((NULL == device) || (NULL == name))
    {
        log_error("parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    int fd = open_device(name);
    if (fd < 0)
        return fd;

#if CONFIG_OS_RTOS
    unsigned long time = 0;

    if (NULL == timeout)
        time = DEFAULT_TIMEOUT * 1000;
    else
        time = timeout->tv_sec * 1000 + (timeout->tv_usec / 1000);

    void *value = time;
    log_info("Watchdog start time: %lu", time);
#else
    int time = -1;
    int *value = &time;

    if (NULL == timeout)
        time = DEFAULT_TIMEOUT;
    else
        time = (int)(timeout->tv_sec + (timeout->tv_usec / 1000000));

    log_info("Watchdog start time: %d", time);
#endif

    int ret = ioctl(fd, WATCHDOG_SET_TIME_OUT, value);
    if (ret < 0)
        log_error("Watchdog set time error.");

    close(fd);

    return (ret < 0) ? HARDWARE_DRIVER_ERROR_IO : HARDWARE_DRIVER_OK;
}

/**
 * @brief Feed watchdog.
 */
static int hw_wdt_feed(struct wdt_device *device, const char* name)
{
    if ((NULL == device) || (NULL == name))
    {
        log_error("parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    int fd = open_device(name);
    if (fd < 0)
        return fd;

    int ret = HARDWARE_DRIVER_ERROR_OPERATION_FAILED;

#if CONFIG_OS_RTOS
    ret = ioctl(fd, WATCHDOG_KEEP_ALIVE, NULL);
#else
    int timeout = WDIOS_DISABLECARD;
    ioctl(fd, WATCHDOG_GET_TIME_OUT, &timeout);
    ret = ioctl(fd, WATCHDOG_KEEP_ALIVE, &timeout);
#endif

    if (ret < 0)
        log_error("failed to feed Watchdog");

    close(fd);

    return (ret < 0) ? HARDWARE_DRIVER_ERROR_IO : HARDWARE_DRIVER_OK;
}

/**
 * @brief Stop watchdog timer.
 */
static int hw_wdt_stop(struct wdt_device *device, const char* name)
{
    if ((NULL == device) || (NULL == name))
    {
        log_error("parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    int fd = open_device(name);
    if (fd < 0)
        return fd;

    int ret = HARDWARE_DRIVER_ERROR_OPERATION_FAILED;

#if CONFIG_OS_RTOS
    ret = ioctl(fd, WATCHDOG_CLOSE, NULL);
#else
    int timeout = WDIOS_DISABLECARD;
    ret = ioctl(fd, WDIOC_SETOPTIONS, &timeout);
#endif

    if (ret < 0)
        log_error("failed to stop Watchdog");

    close(fd);

    return (ret < 0) ? HARDWARE_DRIVER_ERROR_IO : HARDWARE_DRIVER_OK;
}

/**
 * @brief Close watchdog device.
 */
static int hw_wdt_close(struct hw_device_t* device)
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
 * @brief Open watchdog device.
 * @param module Hardware module.
 * @param name Device name (can be NULL).
 * @param device Output device handle.
 * @return 0 on success, negative error code on failure.
 */
int hw_wdt_open(const struct hw_module_t* module, const char* name, struct hw_device_t** device)
{
    *device = NULL;

    if ((NULL == module) || (NULL == device))
    {
        log_error("parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    struct wdt_device *dev;
    dev = calloc(1, sizeof(struct wdt_device));
    if (!dev)
        return HARDWARE_DRIVER_ERROR_MEMORY;

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = WDT_DEVICE_API_VERSION_1_0;
    dev->common.module = (struct hw_module_t *) module;
    dev->common.close = hw_wdt_close;

    dev->start = hw_wdt_start;
    dev->feed = hw_wdt_feed;
    dev->stop = hw_wdt_stop;

    *device = &dev->common;

    return HARDWARE_DRIVER_OK;
}

static struct hw_module_methods_t hw_module_methods = {
    .open = hw_wdt_open,
};

static struct wdt_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = WDT_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = WDT_HARDWARE_MODULE_ID,
        .name = "watchdog timer Module",
        .author = "Anyka",
        .methods = &hw_module_methods,
    },
};

/**
 * @brief Initialize watchdog timer module.
 * @return 0 on success, negative error code on failure.
 */
int hw_wdt_module_init(void)
{
    int ret = hw_register_module(&(HAL_MODULE_INFO_SYM.common));
    return (ret == 0) ? HARDWARE_DRIVER_OK : HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
}


