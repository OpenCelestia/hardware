// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/input/evdev.
 *
 * Input evdev module internal header.
 * - Defines internal structures and helper functions.
 * - Contains device node and context definitions.
 */
#ifndef HARDWARE_EVDEV_HW_H
#define HARDWARE_EVDEV_HW_H

#include <stdint.h>
#include <pthread.h>
#include <linux/input.h>

#include "input_evdev.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * INTERNAL STRUCTURES
 * ============================================================================ */

/**
 * @brief Evdev node structure for device management.
 */
typedef struct evdev_node {
    struct evdev_device *dev;      /**< Associated evdev device */
    struct evdev_node *next;       /**< Next node in list */
    int fd;                        /**< Device file descriptor */
    char dev_path[64];             /**< Device path (e.g., /dev/input/event0) */
    char dev_name[64];             /**< Device name */
    int dev_type;                  /**< Device type: 0=unknown, 1=keyboard, 2=touch, 3=mouse */
} evdev_node_t;

/**
 * @brief Evdev context structure.
 */
typedef struct evdev_context {
    evdev_event_callback_t callback;   /**< Event callback function */
    evdev_node_t *list;                /**< Device node list */
    int epfd;                          /**< Epoll file descriptor */
    pthread_t thread;                  /**< Event processing thread */
    int running;                       /**< Thread running flag */
    pthread_mutex_t mutex;             /**< Mutex for thread safety */
} evdev_context_t;

/**
 * @brief Keyboard/touch context structure.
 */
typedef struct evdev_subdev_context {
    evdev_event_callback_t event_callback;    /**< Event callback function */
} evdev_subdev_context_t;

/* ============================================================================
 * INTERNAL FUNCTION DECLARATIONS
 * ============================================================================ */

/**
 * @brief Open keyboard device.
 * @param module Hardware module.
 * @param name Device name.
 * @param device Output device handle.
 * @return 0 on success, negative error code on failure.
 */
int hw_keyboard_open(const hw_module_t* module, const char* name, hw_device_t** device);

/**
 * @brief Open touch device.
 * @param module Hardware module.
 * @param name Device name.
 * @param device Output device handle.
 * @return 0 on success, negative error code on failure.
 */
int hw_touch_open(const hw_module_t* module, const char* name, hw_device_t** device);

/* ============================================================================
 * EVDEV CORE INTERFACE (for input_module.c)
 * ============================================================================ */

/**
 * @brief Start evdev core event processing.
 * @param on_event Event callback function.
 * @param user User data passed to callback.
 * @return 0 on success, negative error code on failure.
 */
int evdev_start(void (*on_event)(int dev_id, const struct input_event *ev, void *user), void *user);

/**
 * @brief Stop evdev core event processing.
 * @return 0 on success, negative error code on failure.
 */
int evdev_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_EVDEV_HW_H */

