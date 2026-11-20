// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/input.
 *
 * Input module public interface.
 * - Provides Android-style HAL interface for input devices.
 * - Base module for evdev, keyboard, touch, and mouse input devices.
 */
#ifndef HARDWARE_INPUT_H
#define HARDWARE_INPUT_H

#include <stdint.h>
#include <sys/cdefs.h>
#include <linux/kernel.h>

#include "hardware.h"
#include "system_error.h"
#include <linux/input.h>

__BEGIN_DECLS

#define INPUT_MODULE_API_VERSION_1_0 HARDWARE_MODULE_API_VERSION(1, 0)
#define INPUT_HARDWARE_MODULE_ID "input"

/*****************************************************************************/

/*
 * Every hardware module must have a data structure named HAL_MODULE_INFO_SYM
 * and the fields of this data structure must begin with hw_module_t
 * followed by module specific information.
 */
/**
 * @brief Input event callback function type.
 */
typedef void (*input_hal_event_callback_t)(int dev_id, const struct input_event *ev, void *user);

/**
 * @brief Input device structure.
 */
typedef struct input_device {
    struct hw_device_t common;              /**< Base hardware device */
    const char *name;                       /**< Name of device */
    int fd;                                 /**< File descriptor of device */
    void *handle;                           /**< Implementation-specific handle */
    int  (*start)(struct input_device *dev);  /**< Start device operation */
    int  (*stop)(struct input_device *dev);   /**< Stop device operation */
    int  (*set_callback)(struct input_device *dev, input_hal_event_callback_t cb, void *user);
} input_device_t;

/**
 * @brief Input module structure.
 */
typedef struct input_module {
    struct hw_module_t common;              /**< Base hardware module */
    int (*open)(const struct input_module *module, input_device_t **device);
} input_module_t;

static inline int input_get_module(const struct hw_module_t** module, const char* inst) {
    return hw_get_module_by_class(INPUT_HARDWARE_MODULE_ID, inst, module);
}


/* ============================================================================
 * INPUT ERROR CODES
 * ============================================================================ */
 #define HARDWARE_INPUT_OK                       SYSTEM_SUCCESS
 #define HARDWARE_INPUT_ERROR_INVALID_PARAM      SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_INPUT, ERROR_INVALID_PARAM)
 #define HARDWARE_INPUT_ERROR_NOT_INIT           SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_INPUT, ERROR_NOT_INIT)
 #define HARDWARE_INPUT_ERROR_MEMORY             SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_INPUT, ERROR_MEMORY)
 #define HARDWARE_INPUT_ERROR_NOT_FOUND          SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_INPUT, ERROR_NOT_FOUND)
 #define HARDWARE_INPUT_ERROR_BUSY               SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_INPUT, ERROR_BUSY)
 #define HARDWARE_INPUT_ERROR_NOT_SUPPORTED      SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_INPUT, ERROR_NOT_SUPPORTED)
 #define HARDWARE_INPUT_ERROR_OPERATION_FAILED   SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_INPUT, ERROR_OPERATION_FAILED)
 

__END_DECLS

#endif /* HARDWARE_INPUT_H */

