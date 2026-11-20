// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/input.
 *
 * Input HAL module wrapper implementation.
 * - Provides unified input device interface for application layer.
 * - Wraps evdev core functionality and manages input event callbacks.
 */
#include "hardware.h"
#include <stdlib.h>
#include <linux/input.h>
#include "input.h"
#include "input_evdev.h"

/* ============================================================================
 * Type Definitions
 * ============================================================================ */

/* Use input_hal_event_callback_t from input.h */

/* ============================================================================
 * Module State
 * ============================================================================ */

static input_hal_event_callback_t g_cb = NULL;                        /**< Global event callback */
static void *g_cb_user = NULL;                                    /**< User data for callback */

/* ============================================================================
 * Internal Helper Functions
 * ============================================================================ */

/**
 * @brief Forward callback from evdev core to application callback.
 * @param dev_id Device ID.
 * @param ev Input event structure.
 * @param user User data (unused).
 */
static void hal_forward_cb(int dev_id, const struct input_event *ev, void *user)
{
    (void)user;
    if (g_cb) {
        g_cb(dev_id, ev, g_cb_user);
    }
}

/**
 * @brief Start input device.
 * @param dev Input device pointer.
 * @return 0 on success, negative error code on failure.
 */
static int input_dev_start(struct input_device *dev)
{
    if (!dev) {
        return HARDWARE_INPUT_ERROR_INVALID_PARAM;
    }

    int ret = evdev_start(hal_forward_cb, NULL);
    return (ret == 0) ? HARDWARE_INPUT_OK : HARDWARE_INPUT_ERROR_OPERATION_FAILED;
}

/**
 * @brief Stop input device.
 * @param dev Input device pointer.
 * @return 0 on success, negative error code on failure.
 */
static int input_dev_stop(struct input_device *dev)
{
    if (!dev) {
        return HARDWARE_INPUT_ERROR_INVALID_PARAM;
    }

    int ret = evdev_stop();
    return (ret == 0) ? HARDWARE_INPUT_OK : HARDWARE_INPUT_ERROR_OPERATION_FAILED;
}

/**
 * @brief Set input event callback.
 * @param dev Input device pointer.
 * @param cb Event callback function.
 * @param user User data passed to callback.
 * @return 0 on success, negative error code on failure.
 */
static int input_dev_set_callback(struct input_device *dev, input_hal_event_callback_t cb, void *user)
{
    if (!dev || !cb) {
        return HARDWARE_INPUT_ERROR_INVALID_PARAM;
    }

    g_cb = cb;
    g_cb_user = user;
    return HARDWARE_INPUT_OK;
}

/* ============================================================================
 * Device Instance
 * ============================================================================ */

static struct input_device g_device = {
    .common = {
        .tag = HARDWARE_DEVICE_TAG,
        .version = 0,
        .module = NULL,
        .close = NULL,
    },
    .start = input_dev_start,
    .stop = input_dev_stop,
    .set_callback = input_dev_set_callback,
};

/* ============================================================================
 * Module Interface Implementation
 * ============================================================================ */

/**
 * @brief Open input device.
 * @param module Input module pointer.
 * @param device Output device handle.
 * @return 0 on success, negative error code on failure.
 */
static int input_module_open(const struct input_module *module, input_device_t **device)
{
    if (!module || !device) {
        return HARDWARE_INPUT_ERROR_INVALID_PARAM;
    }

    *device = &g_device;
    return HARDWARE_INPUT_OK;
}

/* ============================================================================
 * HAL Module Registration
 * ============================================================================ */

static struct input_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .version_major = 1,
        .version_minor = 0,
        .id = "input",
        .name = "Input HAL Wrapper",
        .author = "Celestia",
        .methods = &(struct hw_module_methods_t){ .open = (int (*)(const struct hw_module_t*, const char*, struct hw_device_t**))NULL },
    },
    .open = input_module_open,
};
