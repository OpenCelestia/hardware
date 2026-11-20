// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/input/evdev.
 *
 * Keyboard device implementation.
 * - Implements evdev_device_t methods for keyboard input.
 * - Supports GPIO keys and AD keys.
 */
#include "input_evdev.h"
#include "evdev_hw.h"

#include <fcntl.h>
#include <unistd.h>
#include <linux/kernel.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/ioctl.h>

#include "log.h"
#include "ak_common.h"


#define LOG_TAG "HAL KEYBOARD"

/** Short-press debounce: same key within this ms is treated as one press (avoid multiple chars per tap). */
#define KEY_DEBOUNCE_MS  250

#if CONFIG_OS_LINUX
static int keyboard_sysfs_name_matches(const char *name)
{
    if (name == NULL || name[0] == '\0')
        return 0;

    if (strstr(name, "gpiokeys") != NULL || strstr(name, "gpio-keys") != NULL ||
        strstr(name, "adkeys") != NULL || strstr(name, "akgpio") != NULL)
        return 1;

    return 0;
}

static int keyboard_event_is_keypad(int event_id)
{
    char evpath[64];
    int fd;
    unsigned long evbit = 0;
    unsigned long keybit[(KEY_MAX + 1) / 8 / sizeof(unsigned long) + 1];
    size_t i;

    snprintf(evpath, sizeof(evpath), "/dev/input/event%d", event_id);
    fd = open(evpath, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return 0;

    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), &evbit) < 0) {
        close(fd);
        return 0;
    }
    if (!(evbit & (1UL << EV_KEY)) || (evbit & (1UL << EV_ABS))) {
        close(fd);
        return 0;
    }

    memset(keybit, 0, sizeof(keybit));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit) < 0) {
        close(fd);
        return 0;
    }
    close(fd);

    for (i = 0; i < sizeof(keybit) / sizeof(unsigned long); i++) {
        if (keybit[i] != 0)
            return 1;
    }
    return 0;
}

static int keyboard_find_event_device(void)
{
    int i;
    int dev_id = -1;
    char path[128];
    char device_name[64];
    int fd;

    for (i = 0; i < 16; i++) {
        snprintf(path, sizeof(path), "/sys/class/input/event%d/device/name", i);
        fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;

        memset(device_name, 0, sizeof(device_name));
        read(fd, device_name, sizeof(device_name) - 1);
        close(fd);

        if (keyboard_sysfs_name_matches(device_name)) {
            dev_id = i;
            break;
        }
    }

    if (dev_id < 0) {
        for (i = 0; i < 16; i++) {
            if (keyboard_event_is_keypad(i)) {
                dev_id = i;
                break;
            }
        }
    }

    return dev_id;
}
#endif

#if CONFIG_OS_LINUX
typedef struct input_event key_event_t;
#else
#include "key_ioctl.h"
typedef struct ak_key_event key_event_t;

#define KEYPAD_DEVICE_INPUT0        "/dev/input0"
#define KEYPAD_DEVICE_INPUT1        "/dev/input1"
#endif

/**
 * @brief Synchronize keyboard device and process pending events.
 * @param device Device context.
 * @param timeout Timeout for waiting events.
 * @return 0 on success, negative error code on failure.
 */
int hw_keyboard_sync(struct evdev_device *device, struct timeval *timeout)
{
    if (NULL == device) {
        log_error("parameter error");
        return HARDWARE_INPUT_ERROR_INVALID_PARAM;
    }

    int fd = device->common.fd;
    if (fd < 0) {
        log_error("device has not been opened");
        return HARDWARE_INPUT_ERROR_NOT_INIT;
    }

    int ret = 0, rdbyte;
    fd_set readfds;

    key_event_t key_event = {0};
    struct evdev_event event = {0};

    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);

    ret = select(FD_SETSIZE, &readfds, NULL, NULL, timeout);
    if (ret < 0) {
        log_error("select failed");
        return HARDWARE_INPUT_ERROR_OPERATION_FAILED;
    }

    if (FD_ISSET(fd, &readfds)) {
        rdbyte = read(fd, &key_event, sizeof(key_event_t));

#if CONFIG_OS_LINUX
        if ((rdbyte == sizeof(key_event_t)) && (EV_KEY == key_event.type))
#else
        if ((rdbyte == sizeof(key_event_t)) && (key_event.code >= 0))
#endif
        {
            event.type = EV_KEY;
            event.code = key_event.code;
#if CONFIG_OS_LINUX
            event.value = key_event.value;
#else
            event.value = (KEY_PRESS == key_event.status) ? 1 : 0;
#endif

            evdev_subdev_context_t *ctx = (evdev_subdev_context_t *)device->context;
            if (ctx->event_callback) {
                ctx->event_callback(&event);
            }

            return HARDWARE_INPUT_OK;
        }
    }

    return HARDWARE_INPUT_ERROR_OPERATION_FAILED;
}

/**
 * @brief Read event from keyboard device.
 * @param device Device context.
 * @param event Output event structure.
 * @param timeout Timeout for waiting events.
 * @return 0 on success, negative error code on failure.
 */
int hw_keyboard_read(struct evdev_device *device, struct evdev_event *event, struct timeval *timeout)
{
    static int s_last_key_code = -1;
    static unsigned long s_last_key_ts_ms = 0;

    if (NULL == device || !event) {
        log_error("parameter error");
        return HARDWARE_INPUT_ERROR_INVALID_PARAM;
    }

    int fd = device->common.fd;
    if (fd < 0) {
        log_error("device has not been opened");
        return HARDWARE_INPUT_ERROR_NOT_INIT;
    }

    int ret = 0, rdbyte;
    fd_set readfds;

    key_event_t key_event = {0};

    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);

    ret = select(FD_SETSIZE, &readfds, NULL, NULL, timeout);
    if (ret < 0) {
        log_error("select failed");
        return HARDWARE_INPUT_ERROR_OPERATION_FAILED;
    }

    if (FD_ISSET(fd, &readfds)) {
        rdbyte = read(fd, &key_event, sizeof(key_event_t));

#if CONFIG_OS_LINUX
        if ((rdbyte == sizeof(key_event_t)) && (EV_KEY == key_event.type))
#else
        if ((rdbyte == sizeof(key_event_t)) && (key_event.code >= 0))
#endif
        {
            event->type = EV_KEY;
            event->code = key_event.code;
#if CONFIG_OS_LINUX
            /* Standard evdev value: 0=up, 1=down, 2=autorepeat. Pass through as-is —
             * must match hw_keyboard_sync() and the raw evdev_core/notification path.
             * Inverting here flips press/release for the LVGL keypad indev, which then
             * (via lv_port_indev's held-state re-report) stays stuck "pressed" after a
             * physical release and fires the key continuously. */
            event->value = key_event.value;
#else
            event->value = (KEY_PRESS == key_event.status) ? 1 : 0;
#endif

            /* Short-press debounce: one physical press may yield multiple evdev events; report only one. */
            if (event->value == 1) {
                struct timeval tv;
                gettimeofday(&tv, NULL);
                unsigned long now_ms = (unsigned long)tv.tv_sec * 1000 + (unsigned long)tv.tv_usec / 1000;
                if (event->code == s_last_key_code && (now_ms - s_last_key_ts_ms) < KEY_DEBOUNCE_MS) {
                    return HARDWARE_INPUT_ERROR_OPERATION_FAILED; /* drop duplicate press */
                }
                s_last_key_code = event->code;
                s_last_key_ts_ms = now_ms;
            } else {
                s_last_key_code = -1;
            }

            return HARDWARE_INPUT_OK;
        }
    }

    return HARDWARE_INPUT_ERROR_OPERATION_FAILED;
}

/**
 * @brief Set keyboard event callback.
 * @param device Device context.
 * @param callback Event callback function.
 * @return 0 on success, negative error code on failure.
 */
int hw_keyboard_set_callback(struct evdev_device *device, evdev_event_callback_t callback)
{
    if ((NULL == device) || (NULL == callback)) {
        log_error("parameter error");
        return HARDWARE_INPUT_ERROR_INVALID_PARAM;
    }

    evdev_subdev_context_t *ctx = (evdev_subdev_context_t *)device->context;
    ctx->event_callback = callback;

    return HARDWARE_INPUT_OK;
}

/**
 * @brief Close keyboard device.
 */
static int hw_keyboard_close(struct hw_device_t* device)
{
    if (NULL == device) {
        log_error("parameter error");
        return HARDWARE_INPUT_ERROR_INVALID_PARAM;
    }

    int fd = ((struct input_device *)device)->fd;
    if (fd < 0) {
        log_error("device hasn't been opened");
        return HARDWARE_INPUT_ERROR_NOT_INIT;
    }

    close(fd);

    void *ctx = ((struct evdev_device *)device)->context;
    free(ctx);
    free(device);

    return HARDWARE_INPUT_OK;
}

/**
 * @brief Open keyboard device.
 * @param module Hardware module.
 * @param name Device name.
 * @param device Output device handle.
 * @return 0 on success, negative error code on failure.
 */
int hw_keyboard_open(const hw_module_t* module, const char* name, hw_device_t** device)
{
    *device = NULL;

    if ((NULL == module) || (NULL == name) || (NULL == device)) {
        log_error("parameter error");
        return HARDWARE_INPUT_ERROR_INVALID_PARAM;
    }

    int fd = -1;
    char path[128] = {0};

#if CONFIG_OS_LINUX
    int dev_id = -1;
    int attempt;

    /* gpiokeys may register shortly after ak_gpio_keys.ko; AV200 names device "akgpio-keys". */
    /* One immediate scan, then a short retry. Do not block products with no keypad. */
    for (attempt = 0; attempt < 5 && dev_id < 0; attempt++) {
        dev_id = keyboard_find_event_device();
        if (dev_id >= 0)
            break;
        usleep(40000);
    }

    if (dev_id < 0) {
        int i;
        int seen = 0;
        char dbg_path[128];
        char dbg_name[64];

        for (i = 0; i < 16; i++) {
            snprintf(dbg_path, sizeof(dbg_path), "/sys/class/input/event%d/device/name", i);
            fd = open(dbg_path, O_RDONLY);
            if (fd < 0)
                continue;
            seen = 1;
            memset(dbg_name, 0, sizeof(dbg_name));
            read(fd, dbg_name, sizeof(dbg_name) - 1);
            close(fd);
            log_error("find key device fail (seen event%d name=%s)", i, dbg_name);
        }
        if (!seen)
            log_error("find key device fail (no input event nodes; check ak_gpio_keys + TCA9555 i2c2@0x20)");
        return HARDWARE_INPUT_ERROR_NOT_FOUND;
    }

    sprintf(path, "/dev/input/event%d", dev_id);
#else
    if (access(KEYPAD_DEVICE_INPUT0, F_OK) == 0) {
        strcpy(path, KEYPAD_DEVICE_INPUT0);
    } else if (access(KEYPAD_DEVICE_INPUT1, F_OK) == 0) {
        strcpy(path, KEYPAD_DEVICE_INPUT1);
    } else {
        log_error("No key device");
        return HARDWARE_INPUT_ERROR_NOT_FOUND;
    }
#endif

    log_info("key device: %s", path);

    if (0 != access(path, F_OK)) {
        log_error("device %s doesn't exist", path);
        return HARDWARE_INPUT_ERROR_NOT_FOUND;
    }

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        log_error("Open key device fail");
        return HARDWARE_INPUT_ERROR_OPERATION_FAILED;
    } else {
        log_info("Open key device ok id = %d", fd);
    }

    struct evdev_device *dev = NULL;
    dev = calloc(1, sizeof(struct evdev_device));
    if (NULL == dev) {
        log_error("malloc for information struct of device failed");
        return HARDWARE_INPUT_ERROR_MEMORY;
    }

    dev->common.common.tag = HARDWARE_DEVICE_TAG;
    dev->common.common.version = INPUT_EVDEV_KEYBOARD_API_VERSION_1_0;
    dev->common.common.module = (struct hw_module_t *) module;
    dev->common.common.close = hw_keyboard_close;

    dev->sync = hw_keyboard_sync;
    dev->read = hw_keyboard_read;
    dev->set_event_callback = hw_keyboard_set_callback;

    dev->common.fd = fd;

    *device = (hw_device_t *)(&dev->common);

    void *ctx = malloc(sizeof(evdev_subdev_context_t));
    if (!ctx) {
        free(dev);
        close(fd);
        return HARDWARE_INPUT_ERROR_MEMORY;
    }
    memset(ctx, 0, sizeof(evdev_subdev_context_t));
    dev->context = ctx;

    return HARDWARE_INPUT_OK;
}

