// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/input/evdev.
 *
 * Input evdev module public interface.
 * - Provides Android-style HAL interface for input event devices.
 * - Supports keyboard, touch, and mouse input devices.
 */
#ifndef HARDWARE_INPUT_EVDEV_H
#define HARDWARE_INPUT_EVDEV_H

#include <stdint.h>
#include <sys/cdefs.h>
#include <linux/kernel.h>
#include <linux/input.h>

#include "hardware.h"
#include "input.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * MODULE IDENTIFICATION
 * ============================================================================ */
#define INPUT_INSTANCE_EVDEV                "evdev"
#define INPUT_EVDEV_NAME                    "evdev"

/* ============================================================================
 * API VERSIONS
 * ============================================================================ */
#define INPUT_EVDEV_API_VERSION_1_0         HARDWARE_DEVICE_API_VERSION(1, 0)

#define INPUT_EVDEV_KEYBOARD_API_VERSION_1_0    HARDWARE_DEVICE_API_VERSION(1, 0)
#define INPUT_EVDEV_NAME_KEYBOARD               "keyboard"

#define INPUT_EVDEV_TOUCH_API_VERSION_1_0       HARDWARE_DEVICE_API_VERSION(1, 0)
#define INPUT_EVDEV_NAME_TOUCH                  "touch"

#define INPUT_EVDEV_MOUSE_API_VERSION_1_0       HARDWARE_DEVICE_API_VERSION(1, 0)
#define INPUT_EVDEV_NAME_MOUSE                  "mouse"

/* ============================================================================
 * EVDEV TYPES
 * ============================================================================ */

/**
 * @brief Evdev state enumeration.
 */
enum evdev_state {
    eEVDEV_STATE_NONE       = -1,   /**< No state. */
    eEVDEV_STATE_RELEASED   = 0,    /**< Key/button released. */
    eEVDEV_STATE_PRESSED    = 1,    /**< Key/button pressed. */
    eEVDEV_STATE_MAX
};

/**
 * @brief Evdev event structure.
 * @details Contains event information including type, code, value and position.
 */
struct evdev_event {
    struct timeval time;            /**< Time of event. */
    uint16_t type;                  /**< Event type: EV_KEY/EV_ABS. */
    uint16_t code;                  /**< Code of event. */
    int32_t value;                  /**< Value of event. */
    int32_t xpos;                   /**< X position (for touch). */
    int32_t ypos;                   /**< Y position (for touch). */
};

/**
 * @brief Evdev event callback function type.
 * @param event Pointer to the event structure.
 * @return 0 on success, negative error code on failure.
 */
typedef int (*evdev_event_callback_t)(struct evdev_event *event);

/* ============================================================================
 * EVDEV DEVICE INTERFACE
 * ============================================================================ */

/**
 * @brief Evdev device structure.
 * @details Provides function pointers for evdev device operations.
 */
typedef struct evdev_device {
    struct input_device common;     /**< Common input device structure. */

    /**
     * @brief Synchronize device and process pending events.
     * @param device Device context.
     * @param timeout Timeout for waiting events.
     * @return 0 on success, negative error code on failure.
     */
    int (*sync)(struct evdev_device *device, struct timeval *timeout);

    /**
     * @brief Read event from device.
     * @param device Device context.
     * @param event Output event structure.
     * @param timeout Timeout for waiting events.
     * @return 0 on success, negative error code on failure.
     */
    int (*read)(struct evdev_device *device, struct evdev_event *event, struct timeval *timeout);

    /**
     * @brief Set event callback function.
     * @param device Device context.
     * @param callback Event callback function.
     * @return 0 on success, negative error code on failure.
     */
    int (*set_event_callback)(struct evdev_device *device, evdev_event_callback_t callback);

    void *context;                  /**< Implementation-specific context. */
} evdev_device_t;

/* ============================================================================
 * MODULE FUNCTIONS
 * ============================================================================ */

/**
 * @brief Initialize evdev module.
 * @return 0 on success, negative error code on failure.
 */
int hw_evdev_module_init(void);

/**
 * @brief Start evdev event processing.
 * @param on_event Callback function for input events.
 * @param user User data passed to callback.
 * @return 0 on success, negative error code on failure.
 */
int evdev_start(void (*on_event)(int dev_id, const struct input_event *ev, void *user), void *user);

/**
 * @brief Stop evdev event processing.
 * @return 0 on success, negative error code on failure.
 */
int evdev_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_INPUT_EVDEV_H */

