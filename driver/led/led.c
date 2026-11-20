// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/driver/led.
 *
 * LED driver module implementation.
 * - Implements led_device_t methods for LED operations.
 * - Supports Linux sysfs LED interface.
 */

#include "led.h"

#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>

#if CONFIG_OS_LINUX
#include <linux/kernel.h>
#endif

#include "ak_common.h"
#include "log.h"
#include "driver_error.h"

#define LOG_TAG "HAL LED"

/**
 * @brief Check if LED device is valid and exists.
 * @param device  Device object.
 * @param name    Device path.
 * @param function  Calling function name (for logging).
 * @param line    Calling line number (for logging).
 * @return 0 on success, negative error code on failure.
 */
static int check_device(struct led_device *device, char *name, const char *function, int line)
{
    if (NULL == device)
    {
        log_error("[%s][%d] invalid device object", function, line);
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    if ((NULL == name) || (0 != access(name, F_OK)))
    {
        log_error("[%s][%d] device doesn't exist: %s", function, line, name);
        return HARDWARE_DRIVER_ERROR_NOT_FOUND;
    }

    return HARDWARE_DRIVER_OK;
}

/**
 * @brief Set LED state.
 */
static int hw_led_set_state(struct led_device *device, const char *name, uint32_t state)
{
    char path[128] = {0};
    snprintf(path, sizeof(path), "/sys/class/leds/%s/brightness", name);

    if (HARDWARE_DRIVER_OK != check_device(device, path, __FUNCTION__, __LINE__))
        return HARDWARE_DRIVER_ERROR_NOT_FOUND;

    FILE *fp = fopen(path, "w");
    if (fp == NULL)
    {
        log_error("[%s][%d] failed to open device", __FUNCTION__, __LINE__);
        return HARDWARE_DRIVER_ERROR_IO;
    }

    char str[32] = {0};
    fseek(fp,0,SEEK_SET);
    snprintf(str, sizeof(str), "%d", state ? 1:0);
    size_t written = fwrite(str, 1,1,fp);

    fclose(fp);

    return (written == 1) ? HARDWARE_DRIVER_OK : HARDWARE_DRIVER_ERROR_IO;
}

/**
 * @brief Get LED state.
 */
static int hw_led_get_state(struct led_device *device, const char *name, uint32_t *state)
{
    char path[128] = {0};
    snprintf(path, sizeof(path), "/sys/class/leds/%s/brightness", name);

    if (HARDWARE_DRIVER_OK != check_device(device, path, __FUNCTION__, __LINE__))
        return HARDWARE_DRIVER_ERROR_NOT_FOUND;

    if (!state) {
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    *state = 0;

    FILE *fp = fopen(path, "r");
    if (fp == NULL)
    {
        log_error("[%s][%d] failed to open device", __FUNCTION__, __LINE__);
        return HARDWARE_DRIVER_ERROR_IO;
    }

    char str[32] = {0};
    size_t r = fread(str, 1, 1, fp);
    if (r == 1) {
        *state = (uint32_t)atoi(str);
    }

    fclose(fp);

    return (r == 1) ? HARDWARE_DRIVER_OK : HARDWARE_DRIVER_ERROR_IO;
}

/**
 * @brief Set LED brightness.
 */
static int hw_led_set_brightness(struct led_device *device, const char *name, uint32_t brightness)
{
    char path[128] = {0};
    snprintf(path, sizeof(path), "/sys/class/leds/%s/brightness", name);

    if (HARDWARE_DRIVER_OK != check_device(device, path, __FUNCTION__, __LINE__))
        return HARDWARE_DRIVER_ERROR_NOT_FOUND;

    FILE *fp = fopen(path, "w");
    if (fp == NULL)
    {
        log_error("[%s][%d] failed to open device", __FUNCTION__, __LINE__);
        return HARDWARE_DRIVER_ERROR_IO;
    }

    char str[32] = {0};
    snprintf(str, sizeof(str), "%u", brightness);
    size_t written = fwrite(str, 1, strlen(str), fp);
    fclose(fp);

    return (written == strlen(str)) ? HARDWARE_DRIVER_OK : HARDWARE_DRIVER_ERROR_IO;
}

/**
 * @brief Get LED brightness.
 */
static int hw_led_get_brightness(struct led_device *device, const char *name, uint32_t *brightness)
{
    char path[128] = {0};
    snprintf(path, sizeof(path), "/sys/class/leds/%s/brightness", name);

    if (HARDWARE_DRIVER_OK != check_device(device, path, __FUNCTION__, __LINE__))
        return HARDWARE_DRIVER_ERROR_NOT_FOUND;

    if (!brightness) {
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    FILE *fp = fopen(path, "r");
    if (fp == NULL)
    {
        log_error("[%s][%d] failed to open device", __FUNCTION__, __LINE__);
        return HARDWARE_DRIVER_ERROR_IO;
    }

    char str[32] = {0};
    size_t r = fread(str, 1, sizeof(str) - 1, fp);
    fclose(fp);

    if (r == 0) {
        return HARDWARE_DRIVER_ERROR_IO;
    }

    *brightness = (uint32_t)atoi(str);
    return HARDWARE_DRIVER_OK;
}

/**
 * @brief Close LED device.
 */
static int hw_led_close(struct hw_device_t* device)
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
 * @brief Open LED device.
 * @param module Hardware module.
 * @param name LED name (can be NULL).
 * @param device Output device handle.
 * @return 0 on success, negative error code on failure.
 */
static int hw_led_open(const hw_module_t* module, const char *name, hw_device_t** device)
{
    *device = NULL;
    (void)name;

    if ((NULL == module) || (NULL == device))
    {
        log_error("invalid parameter");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    struct led_device *dev = calloc(1, sizeof(struct led_device));
    if (NULL == dev)
    {
        log_error("malloc for information struct of device failed");
        return HARDWARE_DRIVER_ERROR_MEMORY;
    }

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = LED_DEVICE_API_VERSION_1_0;
    dev->common.module = (struct hw_module_t *) module;
    dev->common.close = hw_led_close;

    dev->set_state = hw_led_set_state;
    dev->get_state = hw_led_get_state;
    dev->set_brightness = hw_led_set_brightness;
    dev->get_brightness = hw_led_get_brightness;

    *device = &dev->common;

    return HARDWARE_DRIVER_OK;
}

static struct hw_module_methods_t hw_module_methods = {
    .open = hw_led_open,
};

static struct led_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = LED_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = LED_HARDWARE_MODULE_ID,
        .name = "LED Module",
        .author = "Anyka",
        .methods = &hw_module_methods,
    },
};

/**
 * @brief Initialize LED module.
 * @return 0 on success, negative error code on failure.
 */
int hw_led_module_init(void)
{
    int ret = hw_register_module(&(HAL_MODULE_INFO_SYM.common));
    return (ret == 0) ? HARDWARE_DRIVER_OK : HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
}


