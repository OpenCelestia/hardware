// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/driver/gpio.
 *
 * GPIO driver module implementation.
 * - Implements gpio_device_t methods for GPIO control.
 * - Supports Linux sysfs interface and RTOS ioctl interface.
 */
#include "gpio.h"
#include "gpio_hw.h"

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

#define LOG_TAG "HAL GPIO"

#if CONFIG_OS_RTOS
#include "gpio_ioctl.h"
#endif

#if CONFIG_OS_LINUX
static int ak_drv_gpio_open(int gpio);
static int ak_drv_gpio_close(int gpio);
static int ak_drv_gpio_read(int gpio, char *node, char *result, int count);
static int ak_drv_gpio_write(int gpio, char *node, char *value, int count);
#endif

/**
 * @brief Set GPIO direction.
 */
static int hw_gpio_set_direction(struct gpio_device *device, const char* name, uint32_t dir)
{
    if (NULL == device) {
        log_error("parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    int ret = HARDWARE_DRIVER_OK;
    int gpio = 0;
    sscanf(name, "%d", &gpio);
    if (gpio < 0) {
        log_error("device number of gpio device error");
        return HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
    }

#if CONFIG_OS_LINUX
    if (HARDWARE_DRIVER_OK != ak_drv_gpio_open(gpio))
        return HARDWARE_DRIVER_ERROR_OPERATION_FAILED;

    if (dir) {
        ret = ak_drv_gpio_write(gpio, "direction", "out", strlen("out"));
    } else {
        ret = ak_drv_gpio_write(gpio, "direction", "in", strlen("in"));
    }
#else
    if (0 != access(GPIO_DEVICE_PATH, F_OK)) {
        log_error("can't find gpio device");
        return HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
    }

    int fd = GPIO_INVALID_HANDLE;
    fd = open(GPIO_DEVICE_PATH, O_RDWR);
    if (fd < 0) {
        fd = GPIO_INVALID_HANDLE;
        log_error("open gpio device failed");
        return HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
    }

    t_gpio_cg p;
    memset(&p, 0x00, sizeof(t_gpio_cg));
    p.nr = gpio;
    ioctl(fd, IOC_GPIO_SET_PIN_AS_GPIO, &p);

    if (0 >= dir)
        p.dir = 0;
    else
        p.dir = 1;

    ret = ioctl(fd, IOC_GPIO_SET_PIN_DIR, &p);
    close(fd);
    fd = GPIO_INVALID_HANDLE;
#endif

    if (HARDWARE_DRIVER_OK != ret)
        log_error("failed to set direction");

    return ret;
}

/**
 * @brief Set GPIO output level.
 */
static int hw_gpio_set_output_level(struct gpio_device *device, const char* name, uint32_t level)
{
    if (NULL == device) {
        log_error("parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    int ret = HARDWARE_DRIVER_OK;
    int gpio = 0;
    sscanf(name, "%d", &gpio);
    if (gpio < 0) {
        log_error("device number of gpio device error");
        return HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
    }

#if CONFIG_OS_LINUX
    if (HARDWARE_DRIVER_OK != ak_drv_gpio_open(gpio))
        return HARDWARE_DRIVER_ERROR_OPERATION_FAILED;

    if (HARDWARE_DRIVER_OK == ak_drv_gpio_write(gpio, "direction", "out", strlen("out"))) {
        if (level)
            ret = ak_drv_gpio_write(gpio, "value", "1", strlen("1"));
        else
            ret = ak_drv_gpio_write(gpio, "value", "0", strlen("0"));
    }
#else
    if (0 != access(GPIO_DEVICE_PATH, F_OK)) {
        log_error("can't find gpio device");
        return HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
    }

    int fd = GPIO_INVALID_HANDLE;
    fd = open(GPIO_DEVICE_PATH, O_RDWR);
    if (fd < 0) {
        fd = GPIO_INVALID_HANDLE;
        log_error("open gpio device failed");
        return HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
    }

    t_gpio_cg p;
    memset(&p, 0x00, sizeof(t_gpio_cg));
    p.nr = gpio;
    ioctl(fd, IOC_GPIO_SET_PIN_AS_GPIO, &p);

    p.config = 0x01000300;
    ioctl(fd, IOC_GPIO_SET_PIN_CONFIG, &p);

    p.dir = 1;
    ioctl(fd, IOC_GPIO_SET_PIN_DIR, &p);

    p.data = level;
    ret = ioctl(fd, IOC_GPIO_SET_PIN_OUTPUT_DATA, &p);

    close(fd);
    fd = GPIO_INVALID_HANDLE;
#endif

    if (HARDWARE_DRIVER_OK != ret)
        log_error("failed to set output level");

    return ret;
}

/**
 * @brief Get GPIO input level.
 */
static int hw_gpio_get_input_level(struct gpio_device *device, const char* name, uint32_t *level)
{
    if (NULL == device) {
        log_error("parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    *level = 0;

    int gpio = 0;
    sscanf(name, "%d", &gpio);
    if (gpio < 0) {
        log_error("device number of gpio device error");
        return HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
    }

#if CONFIG_OS_LINUX
    if (HARDWARE_DRIVER_OK != ak_drv_gpio_open(gpio))
        return HARDWARE_DRIVER_ERROR_OPERATION_FAILED;

    char value[GPIO_ARRAY_MAX_LENGTH] = {0};
    memset(value, 0, GPIO_ARRAY_MAX_LENGTH);

    if (HARDWARE_DRIVER_OK == ak_drv_gpio_write(gpio, "direction", "in", strlen("in"))) {
        if (HARDWARE_DRIVER_OK == ak_drv_gpio_read(gpio, "value", value, GPIO_ARRAY_MAX_LENGTH)) {
            *level = atoi(value);
            return HARDWARE_DRIVER_OK;
        }
    }

    return HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
#else
    if (0 != access(GPIO_DEVICE_PATH, F_OK)) {
        log_error("can't find gpio device");
        return HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
    }

    int fd = GPIO_INVALID_HANDLE;
    fd = open(GPIO_DEVICE_PATH, O_RDWR);
    if (fd < 0) {
        fd = GPIO_INVALID_HANDLE;
        log_error("open gpio device failed");
        return HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
    }

    t_gpio_cg p;
    memset(&p, 0x00, sizeof(t_gpio_cg));
    p.nr = gpio;

    ioctl(fd, IOC_GPIO_SET_PIN_AS_GPIO, &p);

    p.config = 0x01010000;
    ioctl(fd, IOC_GPIO_SET_PIN_CONFIG, &p);

    ioctl(fd, IOC_GPIO_GET_PIN_INPUT_DATA, &p);

    if (close(fd) == HARDWARE_DRIVER_OK)
        fd = GPIO_INVALID_HANDLE;
    else
        log_error("close gpio device failed");

    *level = p.data;

    return HARDWARE_DRIVER_OK;
#endif
}

/**
 * @brief Set GPIO drive strength.
 */
static int hw_gpio_set_drive_strength(struct gpio_device *device, const char* name, uint32_t strength)
{
    if (NULL == device) {
        log_error("parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    int gpio = 0;
    sscanf(name, "%d", &gpio);
    if (gpio < 0) {
        log_error("device number of gpio device error");
        return HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
    }

    int ret = HARDWARE_DRIVER_OK;

#if CONFIG_OS_LINUX
    if (HARDWARE_DRIVER_OK != ak_drv_gpio_open(gpio))
        return HARDWARE_DRIVER_ERROR_OPERATION_FAILED;

    char value[GPIO_ARRAY_MAX_LENGTH] = {0};
    memset(value, 0, GPIO_ARRAY_MAX_LENGTH);

    snprintf(value, sizeof(value), "%d", strength);
    ret = ak_drv_gpio_write(gpio, "drive", value, strlen(value));
#else
    return HARDWARE_DRIVER_OK;
#endif

    if (HARDWARE_DRIVER_OK != ret)
        log_error("failed to set drive strength");

    return ret;
}

/**
 * @brief Enable GPIO pull.
 */
static int hw_gpio_enable_pull(struct gpio_device *device, const char* name, uint32_t enable)
{
    if (NULL == device) {
        log_error("parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    int ret = HARDWARE_DRIVER_ERROR_OPERATION_FAILED;

    int gpio = 0;
    sscanf(name, "%d", &gpio);
    if (gpio < 0) {
        log_error("device number of gpio device error");
        return HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
    }

#if CONFIG_OS_LINUX
    if (HARDWARE_DRIVER_OK != ak_drv_gpio_open(gpio))
        return HARDWARE_DRIVER_ERROR_OPERATION_FAILED;

    if (enable) {
        ret = ak_drv_gpio_write(gpio, "pull_enable", "1", strlen("1"));
    } else {
        ret = ak_drv_gpio_write(gpio, "pull_enable", "0", strlen("0"));
    }
#else
    /* RTOS implementation not available */
#endif

    if (HARDWARE_DRIVER_OK != ret)
        log_error("failed to enable pull");

    return ret;
}

/**
 * @brief Set GPIO pull polarity.
 */
static int hw_gpio_set_pull_polarity(struct gpio_device *device, const char* name, uint32_t polarity)
{
    if (NULL == device) {
        log_error("parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    int ret = HARDWARE_DRIVER_ERROR_OPERATION_FAILED;

    int gpio = 0;
    sscanf(name, "%d", &gpio);
    if (gpio < 0) {
        log_error("device number of gpio device error");
        return HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
    }

#if CONFIG_OS_LINUX
    if (HARDWARE_DRIVER_OK != ak_drv_gpio_open(gpio))
        return HARDWARE_DRIVER_ERROR_OPERATION_FAILED;

    if (polarity) {
        ret = ak_drv_gpio_write(gpio, "pull_polarity", "pullup", strlen("pullup"));
    } else {
        ret = ak_drv_gpio_write(gpio, "pull_polarity", "pulldown", strlen("pulldown"));
    }
#else
    /* RTOS implementation not available */
#endif

    if (HARDWARE_DRIVER_OK != ret)
        log_error("failed to set pull polarity");

    return ret;
}

/**
 * @brief Set GPIO slew rate.
 */
static int hw_gpio_set_slew_rate(struct gpio_device *device, const char* name, uint32_t rate)
{
    if (NULL == device) {
        log_error("parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    int ret = HARDWARE_DRIVER_ERROR_OPERATION_FAILED;

    int gpio = 0;
    sscanf(name, "%d", &gpio);
    if (gpio < 0) {
        log_error("device number of gpio device error");
        return HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
    }

#if CONFIG_OS_LINUX
    if (HARDWARE_DRIVER_OK != ak_drv_gpio_open(gpio))
        return HARDWARE_DRIVER_ERROR_OPERATION_FAILED;

    if (rate) {
        ret = ak_drv_gpio_write(gpio, "slew_rate", "fast", strlen("fast"));
    } else {
        ret = ak_drv_gpio_write(gpio, "slew_rate", "slow", strlen("slow"));
    }
#else
    /* RTOS implementation not available */
#endif

    return ret;
}

#if CONFIG_OS_LINUX
/**
 * @brief Open GPIO device via sysfs.
 */
static int ak_drv_gpio_open(int gpio)
{
    int fd = -1;
    int ret = 0;
    char value[GPIO_FILE_PATH_MAX_LENGTH] = {0};
    char path[GPIO_FILE_PATH_MAX_LENGTH] = {0};

    memset(path, 0, GPIO_FILE_PATH_MAX_LENGTH);
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d", gpio);

    if (access(path, F_OK)) {
        memset(path, 0, GPIO_FILE_PATH_MAX_LENGTH);
        snprintf(path, sizeof(path), "/sys/class/gpio/export");
        fd = open(path, O_WRONLY);
        if (fd < 0) {
            log_error("failed to open gpio %d", gpio);
            return HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
        }

        memset(value, 0, GPIO_FILE_PATH_MAX_LENGTH);
        snprintf(value, sizeof(value), "%d\n", gpio);

        ret = write(fd, value, strlen(value));
        if (ret < 0) {
            log_error("failed to write gpio %d", gpio);
            close(fd);
            return HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
        }

        close(fd);
    }

    return HARDWARE_DRIVER_OK;
}

/**
 * @brief Close GPIO device via sysfs.
 */
static int ak_drv_gpio_close(int gpio)
{
    int fd = -1;
    int ret = 0;
    char value[GPIO_FILE_PATH_MAX_LENGTH] = {0};
    char path[GPIO_FILE_PATH_MAX_LENGTH] = {0};

    memset(path, 0, GPIO_FILE_PATH_MAX_LENGTH);
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d", gpio);

    if (!access(path, F_OK)) {
        memset(path, 0, GPIO_FILE_PATH_MAX_LENGTH);
        snprintf(path, sizeof(path), "/sys/class/gpio/unexport");
        fd = open(path, O_WRONLY);
        if (fd < 0) {
            return HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
        }

        memset(value, 0, GPIO_FILE_PATH_MAX_LENGTH);
        snprintf(value, sizeof(value), "%d\n", gpio);

        ret = write(fd, value, strlen(value));
        if (ret < 0) {
            close(fd);
            return HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
        }

        close(fd);
    }

    return HARDWARE_DRIVER_OK;
}

/**
 * @brief Read GPIO attribute via sysfs.
 */
static int ak_drv_gpio_read(int gpio, char *node, char *result, int count)
{
    int fd = -1;
    int ret = 0;
    char path[GPIO_FILE_PATH_MAX_LENGTH] = {0};

    memset(path, 0, GPIO_FILE_PATH_MAX_LENGTH);
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/%s", gpio, node);

    if (!access(path, F_OK)) {
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            return HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
        }

        ret = read(fd, result, count);
        if (ret < 0) {
            close(fd);
            return HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
        }

        close(fd);
    }

    return HARDWARE_DRIVER_OK;
}

/**
 * @brief Write GPIO attribute via sysfs.
 */
static int ak_drv_gpio_write(int gpio, char *node, char *value, int count)
{
    int fd = -1;
    int ret = 0;
    char path[GPIO_FILE_PATH_MAX_LENGTH] = {0};

    memset(path, 0, GPIO_FILE_PATH_MAX_LENGTH);
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/%s", gpio, node);

    if (!access(path, F_OK)) {
        fd = open(path, O_WRONLY);
        if (fd < 0) {
            log_error("failed to opening %s", path);
            return HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
        }
        ret = write(fd, value, strlen(value));
        if (ret < 0) {
            close(fd);
            return HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
        }

        close(fd);
    }

    return HARDWARE_DRIVER_OK;
}
#endif

/**
 * @brief Close GPIO device.
 */
static int hw_gpio_close(struct hw_device_t* device)
{
    if (NULL == device) {
        log_error("parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    free(device);

    return HARDWARE_DRIVER_OK;
}

/**
 * @brief Open GPIO device.
 * @param module Hardware module.
 * @param name Device name (can be NULL).
 * @param device Output device handle.
 * @return 0 on success, negative error code on failure.
 */
static int hw_gpio_open(const hw_module_t* module, const char* name, hw_device_t** device)
{
    *device = NULL;

    if ((NULL == module) || (NULL == device)) {
        log_error("parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    struct gpio_device *dev = calloc(1, sizeof(struct gpio_device));
    if (NULL == dev) {
        log_error("malloc for information struct of device failed");
        return HARDWARE_DRIVER_ERROR_MEMORY;
    }

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = GPIO_DEVICE_API_VERSION_1_0;
    dev->common.module = (struct hw_module_t *) module;
    dev->common.close = hw_gpio_close;

    dev->set_direction = hw_gpio_set_direction;
    dev->set_output_level = hw_gpio_set_output_level;
    dev->get_input_level = hw_gpio_get_input_level;
    dev->set_drive_strength = hw_gpio_set_drive_strength;
    dev->enable_pull = hw_gpio_enable_pull;
    dev->set_pull_polarity = hw_gpio_set_pull_polarity;
    dev->set_slew_rate = hw_gpio_set_slew_rate;

    *device = &dev->common;

    return HARDWARE_DRIVER_OK;
}

static struct hw_module_methods_t hw_module_methods = {
    .open = hw_gpio_open,
};

static struct gpio_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = GPIO_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = GPIO_HARDWARE_MODULE_ID,
        .name = "GPIO Module",
        .author = "Anyka",
        .methods = &hw_module_methods,
    },
};

/**
 * @brief Initialize GPIO module.
 */
int hw_gpio_module_init(void)
{
    int ret = hw_register_module(&(HAL_MODULE_INFO_SYM.common));
    return (ret == 0) ? HARDWARE_DRIVER_OK : HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
}



