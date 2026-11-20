// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/input/evdev.
 *
 * Input evdev module implementation.
 * - Implements evdev_device_t methods for input event processing.
 * - Uses epoll + thread architecture for event monitoring.
 * - Supports device capability detection and auto-identification.
 */
#include "input_evdev.h"
#include "evdev_hw.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <dirent.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "log.h"
#include "ak_common.h"

#define LOG_TAG "HAL EVDEV"

#define DIV_ROUND_UP(n, d)  (((n) + (d) - 1) / (d))
#define BIT(nr)             (1UL << (nr))
#define BIT_MASK(nr)        (1UL << ((nr) % BITS_PER_LONG))
#define BIT_WORD(nr)        ((nr) / BITS_PER_LONG)
#define BITS_PER_BYTE       8
#define BITS_PER_LONG       (sizeof(long) * BITS_PER_BYTE)
#define BITS_TO_LONGS(nr)   DIV_ROUND_UP(nr, BITS_PER_BYTE * sizeof(long))

/**
 * @brief Check device capability using EVIOCGBIT.
 * @param fd Device file descriptor.
 * @param ev_type Event type to check.
 * @return 1 if capability exists, 0 otherwise.
 */
static int evdev_check_capability(int fd, unsigned int ev_type)
{
    long evbit[BITS_TO_LONGS(EV_CNT)];

    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit) < 0) {
        return 0;
    }

    return (evbit[BIT_WORD(ev_type)] & BIT_MASK(ev_type)) ? 1 : 0;
}

/**
 * @brief Read device name from sysfs.
 * @param event_id Event device ID.
 * @param name Buffer to store device name.
 * @param name_size Size of name buffer.
 * @return 0 on success, -1 on failure.
 */
static int evdev_get_device_name(int event_id, char *name, size_t name_size)
{
    char path[128];
    int fd;
    ssize_t ret;

    snprintf(path, sizeof(path), "/sys/class/input/event%d/device/name", event_id);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    ret = read(fd, name, name_size - 1);
    close(fd);

    if (ret > 0) {
        name[ret] = '\0';
        if (name[ret - 1] == '\n') {
            name[ret - 1] = '\0';
        }
        return 0;
    }

    return -1;
}

/**
 * @brief Identify device type based on capabilities and name.
 * @param fd Device file descriptor.
 * @param dev_name Device name.
 * @return Device type: 0=unknown, 1=keyboard, 2=touch, 3=mouse.
 */
static int evdev_identify_device_type(int fd, const char *dev_name)
{
    int has_key = evdev_check_capability(fd, EV_KEY);
    int has_abs = evdev_check_capability(fd, EV_ABS);
    int has_rel = evdev_check_capability(fd, EV_REL);

    /* Identify by device name */
    if (dev_name) {
        if (strstr(dev_name, "pir") || strstr(dev_name, "PIR") ||
            strstr(dev_name, "motion") || strstr(dev_name, "sensor")) {
            if (has_key && !has_abs && !has_rel) {
                return 1; /* keyboard (PIR as key) */
            }
        }
        if (strstr(dev_name, "gpiokeys") || strstr(dev_name, "adkeys") ||
            strstr(dev_name, "akgpio") || strstr(dev_name, "gpio-keys") ||
            strstr(dev_name, "keyboard") || strstr(dev_name, "keypad")) {
            return 1; /* keyboard */
        }
        if (strstr(dev_name, "touch") || strstr(dev_name, "ts") ||
            strstr(dev_name, "touchscreen") ||
            strstr(dev_name, "Touch") || strstr(dev_name, "TOUCH") ||
            /* KM01W CST816; harmless name hint on other boards */
            strstr(dev_name, "cst816") || strstr(dev_name, "hynitron")) {
            return 2; /* touch */
        }
        if (strstr(dev_name, "mouse")) {
            return 3; /* mouse */
        }
    }

    /* Identify by capabilities */
    if (has_key && !has_abs && !has_rel) {
        return 1; /* keyboard (EV_KEY only) */
    }
    if (has_abs) {
        return 2; /* touch (has EV_ABS) */
    }
    if (has_rel && has_key) {
        return 3; /* mouse (has EV_REL and EV_KEY) */
    }

    return 0; /* unknown */
}

/**
 * @brief Open a single evdev device node.
 * @param event_id Event device ID.
 * @param node_out Output node pointer.
 * @return 0 on success, negative error code on failure.
 */
static int evdev_open_device_node(int event_id, evdev_node_t **node_out)
{
    char path[64];
    int fd;
    evdev_node_t *node;
    char dev_name[64] = {0};
    int dev_type;

    snprintf(path, sizeof(path), "/dev/input/event%d", event_id);

    fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        return HARDWARE_INPUT_ERROR_OPERATION_FAILED;
    }

    evdev_get_device_name(event_id, dev_name, sizeof(dev_name));
    dev_type = evdev_identify_device_type(fd, dev_name);

    log_info("Found device: %s, name: %s, type: %d", path, dev_name, dev_type);

    node = (evdev_node_t *)malloc(sizeof(evdev_node_t));
    if (!node) {
        close(fd);
        return HARDWARE_INPUT_ERROR_MEMORY;
    }

    memset(node, 0, sizeof(evdev_node_t));
    node->fd = fd;
    strncpy(node->dev_path, path, sizeof(node->dev_path) - 1);
    if (dev_name[0]) {
        strncpy(node->dev_name, dev_name, sizeof(node->dev_name) - 1);
    }
    node->dev_type = dev_type;

    *node_out = node;
    return HARDWARE_INPUT_OK;
}

/**
 * @brief Enumerate and open all evdev devices.
 * @param list Output device list.
 * @return 0 on success, negative error code on failure.
 */
static int evdev_enumerate_devices(evdev_node_t **list)
{
    DIR *dir;
    struct dirent *entry;
    int event_id;
    evdev_node_t *node;
    int count = 0;

    dir = opendir("/dev/input");
    if (!dir) {
        log_error("Failed to open /dev/input");
        return HARDWARE_INPUT_ERROR_OPERATION_FAILED;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (sscanf(entry->d_name, "event%d", &event_id) == 1) {
            if (evdev_open_device_node(event_id, &node) == HARDWARE_INPUT_OK) {
                node->next = *list;
                *list = node;
                count++;
            }
        }
    }

    closedir(dir);
    log_info("Enumerated %d input devices", count);
    return count > 0 ? HARDWARE_INPUT_OK : HARDWARE_INPUT_ERROR_NOT_FOUND;
}

/**
 * @brief Convert struct input_event to evdev_event.
 * @param in Input event.
 * @param out Output evdev event.
 * @param xpos X position for touch.
 * @param ypos Y position for touch.
 */
static void evdev_convert_event(const struct input_event *in, struct evdev_event *out, int xpos, int ypos)
{
    memset(out, 0, sizeof(struct evdev_event));

    out->time = in->time;
    out->type = in->type;
    out->code = in->code;
    out->value = in->value;
    out->xpos = xpos;
    out->ypos = ypos;
}

/**
 * @brief Epoll event processing thread function.
 * @param arg Thread argument (context pointer).
 * @return NULL.
 */
static void *evdev_thread_func(void *arg)
{
    evdev_context_t *ctx = (evdev_context_t *)arg;
    struct epoll_event events[16];
    struct input_event input_ev;
    struct evdev_event evdev_ev;
    evdev_node_t *node;
    ssize_t ret;

    log_info("evdev thread started");

    while (ctx->running) {
        int nfds = epoll_wait(ctx->epfd, events, 16, 200);

        if (nfds < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_error("epoll_wait failed: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            /* Find corresponding node */
            pthread_mutex_lock(&ctx->mutex);
            node = ctx->list;
            while (node && node->fd != fd) {
                node = node->next;
            }
            pthread_mutex_unlock(&ctx->mutex);

            if (!node) {
                continue;
            }

            /* Read events */
            static int xpos = 0, ypos = 0;
            while (1) {
                ret = read(fd, &input_ev, sizeof(struct input_event));
                if (ret < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    log_error("read failed: %s", strerror(errno));
                    break;
                }

                if (ret != sizeof(struct input_event)) {
                    break;
                }

                /* Process touch coordinates */
                if (input_ev.type == EV_ABS) {
                    if (input_ev.code == ABS_X) {
                        xpos = input_ev.value;
                    } else if (input_ev.code == ABS_Y) {
                        ypos = input_ev.value;
                    }
                }

                evdev_convert_event(&input_ev, &evdev_ev, xpos, ypos);

                /* Call callback */
                if (ctx->callback && (input_ev.type == EV_KEY || input_ev.type == EV_ABS || input_ev.type == EV_REL)) {
                    ctx->callback(&evdev_ev);
                }
            }
        }
    }

    log_info("evdev thread stopped");
    return NULL;
}

/**
 * @brief Add device to list.
 * @param list Device list.
 * @param device Device to add.
 * @return 0 on success, negative error code on failure.
 */
static int evdev_add_device(evdev_node_t **list, struct evdev_device *device)
{
    evdev_node_t *node = (evdev_node_t *)malloc(sizeof(evdev_node_t));
    if (NULL == node) {
        log_error("malloc for device node failed");
        return HARDWARE_INPUT_ERROR_MEMORY;
    }

    node->dev = device;
    node->next = NULL;
    node->fd = -1;

    if (NULL == *list) {
        *list = node;
        return HARDWARE_INPUT_OK;
    }

    evdev_node_t *next = *list;
    while (next->next) {
        next = next->next;
    }
    next->next = node;

    return HARDWARE_INPUT_OK;
}

/**
 * @brief Clear device list.
 * @param list Device list to clear.
 * @return 0 on success.
 */
static int evdev_clear_device(evdev_node_t *list)
{
    evdev_node_t *next = list;
    evdev_node_t *node = NULL;

    while (next) {
        node = next;
        next = next->next;

        if (node->fd >= 0) {
            close(node->fd);
        }

        if (node->dev) {
            hw_device_t *device = (hw_device_t *)node->dev;
            if (device->close) {
                device->close(device);
            }
        }

        free(node);
    }

    return HARDWARE_INPUT_OK;
}

/**
 * @brief Sync device - traverse all sub-devices and call sync.
 */
static int hw_input_evdev_sync(struct evdev_device *device, struct timeval *timeout)
{
    if ((NULL == device) || (NULL == (device->context))) {
        log_error("parameter error");
        return HARDWARE_INPUT_ERROR_INVALID_PARAM;
    }

    evdev_context_t *ctx = (evdev_context_t *)device->context;

    struct evdev_device *dev = NULL;
    evdev_node_t *next = ctx->list;

    pthread_mutex_lock(&ctx->mutex);
    while (next) {
        dev = next->dev;
        if (dev && dev->sync) {
            dev->sync(dev, timeout);
        }
        next = next->next;
    }
    pthread_mutex_unlock(&ctx->mutex);

    return HARDWARE_INPUT_OK;
}

/**
 * @brief Read event - traverse all sub-devices and call read.
 */
static int hw_input_evdev_read(struct evdev_device *device, struct evdev_event *event, struct timeval *timeout)
{
    if ((NULL == device) || (NULL == (device->context)) || !event) {
        log_error("parameter error: device=%p, context=%p, event=%p", 
                  (void*)device, device ? device->context : NULL, (void*)event);
        return HARDWARE_INPUT_ERROR_INVALID_PARAM;
    }

    evdev_context_t *ctx = (evdev_context_t *)device->context;

    struct evdev_device *dev = NULL;
    evdev_node_t *next = ctx->list;

    if (next == NULL) {
        //log_error("device list is empty");
        return HARDWARE_INPUT_ERROR_OPERATION_FAILED;
    }

    pthread_mutex_lock(&ctx->mutex);
    while (next) {
        dev = next->dev;
        if (dev && dev->read) {
            int ret = dev->read(dev, event, timeout);
            if (ret == HARDWARE_INPUT_OK) {
                pthread_mutex_unlock(&ctx->mutex);
                return HARDWARE_INPUT_OK;
            }
        }
        next = next->next;
    }
    pthread_mutex_unlock(&ctx->mutex);

    return HARDWARE_INPUT_ERROR_OPERATION_FAILED;
}

/**
 * @brief Set event callback.
 */
static int hw_input_evdev_set_callback(struct evdev_device *device, evdev_event_callback_t callback)
{
    if ((NULL == device) || (NULL == (device->context)) || (NULL == callback)) {
        log_error("parameter error");
        return HARDWARE_INPUT_ERROR_INVALID_PARAM;
    }

    evdev_context_t *ctx = (evdev_context_t *)device->context;

    pthread_mutex_lock(&ctx->mutex);
    ctx->callback = callback;

    /* Start thread if using epoll mode */
    if (!ctx->running && ctx->epfd >= 0) {
        ctx->running = 1;
        if (pthread_create(&ctx->thread, NULL, evdev_thread_func, ctx) != 0) {
            log_error("Failed to create evdev thread");
            ctx->running = 0;
            pthread_mutex_unlock(&ctx->mutex);
            return HARDWARE_INPUT_ERROR_OPERATION_FAILED;
        }
    }

    /* Set callback for sub-devices */
    struct evdev_device *dev = NULL;
    evdev_node_t *next = ctx->list;
    while (next) {
        dev = next->dev;
        if (dev && dev->set_event_callback) {
            dev->set_event_callback(dev, callback);
        }
        next = next->next;
    }
    pthread_mutex_unlock(&ctx->mutex);

    return HARDWARE_INPUT_OK;
}

/**
 * @brief Close evdev device.
 */
static int hw_input_evdev_close(struct hw_device_t* device)
{
    if (NULL == device) {
        log_error("parameter error");
        return HARDWARE_INPUT_ERROR_INVALID_PARAM;
    }

    struct evdev_device *dev = (struct evdev_device *)device;
    evdev_context_t *ctx = dev->context;

    if (ctx) {
        /* Stop thread */
        if (ctx->running) {
            ctx->running = 0;
            pthread_join(ctx->thread, NULL);
        }

        /* Clean up epoll */
        if (ctx->epfd >= 0) {
            close(ctx->epfd);
        }

        /* Clean up devices */
        evdev_clear_device(ctx->list);

        pthread_mutex_destroy(&ctx->mutex);
        free((void *)ctx);
    }

    free(device);

    return HARDWARE_INPUT_OK;
}

/**
 * @brief Open evdev device.
 * @param module Hardware module.
 * @param name Device name.
 * @param device Output device handle.
 * @return 0 on success, negative error code on failure.
 */
int hw_input_evdev_open(const hw_module_t* module, const char* name, hw_device_t** device)
{
    if ((NULL == module) || (NULL == name) || (NULL == device)) {
        log_error("parameter error");
        return HARDWARE_INPUT_ERROR_INVALID_PARAM;
    }

    if ((0 != strcmp(name, INPUT_EVDEV_NAME)) && (0 != strcmp(name, INPUT_EVDEV_NAME_KEYBOARD))
        && (0 != strcmp(name, INPUT_EVDEV_NAME_TOUCH)) && (0 != strcmp(name, INPUT_EVDEV_NAME_MOUSE))) {
        log_error("Unsupported device: %s", name);
        return HARDWARE_INPUT_ERROR_NOT_SUPPORTED;
    }

    log_info("Opening evdev device: %s", name);

    struct evdev_device *dev = NULL;
    dev = calloc(1, sizeof(struct evdev_device));
    if (NULL == dev) {
        log_error("malloc for information struct of device failed");
        return HARDWARE_INPUT_ERROR_MEMORY;
    }

    evdev_context_t *ctx = (evdev_context_t *)malloc(sizeof(evdev_context_t));
    if (NULL == ctx) {
        log_error("malloc for context data of device failed");
        free(dev);
        return HARDWARE_INPUT_ERROR_MEMORY;
    }
    memset((void *)ctx, 0, sizeof(evdev_context_t));

    pthread_mutex_init(&ctx->mutex, NULL);

    dev->common.common.tag = HARDWARE_DEVICE_TAG;
    dev->common.common.version = INPUT_EVDEV_API_VERSION_1_0;
    dev->common.common.module = (struct hw_module_t *) module;
    dev->common.common.close = hw_input_evdev_close;
    dev->common.name = INPUT_EVDEV_NAME;

    dev->sync = hw_input_evdev_sync;
    dev->read = hw_input_evdev_read;
    dev->set_event_callback = hw_input_evdev_set_callback;

    *device = (hw_device_t *)(&dev->common);
    dev->context = (void *)ctx;

    /* Create epoll */
    ctx->epfd = epoll_create1(0);
    if (ctx->epfd < 0) {
        log_error("Failed to create epoll: %s", strerror(errno));
        free(ctx);
        free(dev);
        return HARDWARE_INPUT_ERROR_OPERATION_FAILED;
    }

    /* Full enumerate only for generic evdev; touch/keyboard use dedicated sub-drivers */
    evdev_node_t *enum_list = NULL;
    int enumerate_all = (strcmp(name, INPUT_EVDEV_NAME) == 0);
    if (enumerate_all && evdev_enumerate_devices(&enum_list) == HARDWARE_INPUT_OK) {
        evdev_node_t *node = enum_list;
        while (node) {
            struct epoll_event ev;
            ev.events = EPOLLIN;
            ev.data.fd = node->fd;

            if (epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, node->fd, &ev) == 0) {
                node->next = ctx->list;
                ctx->list = node;
                log_info("Added device to epoll: %s (type: %d)", node->dev_path, node->dev_type);
            } else {
                log_error("Failed to add device to epoll: %s", strerror(errno));
                close(node->fd);
                evdev_node_t *tmp = node;
                node = node->next;
                free(tmp);
                continue;
            }

            node = node->next;
        }
    }

#if (CONFIG_HW_INPUT_KEYBOARD || CONFIG_HW_INPUT_TOUCH)
    int ret = HARDWARE_INPUT_ERROR_OPERATION_FAILED;
    struct hw_device_t *SubDevice = NULL;
#endif

#if CONFIG_HW_INPUT_KEYBOARD
    if ((0 == strcmp(name, INPUT_EVDEV_NAME)) || (0 == strcmp(name, INPUT_EVDEV_NAME_KEYBOARD))) {
        ret = hw_keyboard_open(module, INPUT_EVDEV_NAME_KEYBOARD, &SubDevice);
        if (ret == HARDWARE_INPUT_OK) {
            ret = evdev_add_device(&(ctx->list), ((struct evdev_device *)SubDevice));
            if (ret != HARDWARE_INPUT_OK)
                log_error("failed to adding keyboard device");
        }
    }
#endif

#if CONFIG_HW_INPUT_TOUCH
    if ((0 == strcmp(name, INPUT_EVDEV_NAME)) || (0 == strcmp(name, INPUT_EVDEV_NAME_TOUCH))) {
        ret = hw_touch_open(module, INPUT_EVDEV_NAME_TOUCH, &SubDevice);
        if (ret == HARDWARE_INPUT_OK) {
            struct evdev_device *touch_dev = (struct evdev_device *)SubDevice;
            log_info("touch sub-device: dev=%p, read=%p, handle=%p", 
                     (void*)touch_dev, (void*)touch_dev->read, touch_dev->common.handle);
            ret = evdev_add_device(&(ctx->list), touch_dev);
            if (ret != HARDWARE_INPUT_OK)
                log_error("failed to adding touch device");
        } else {
            log_error("hw_touch_open failed, ret=%d", ret);
        }
    }
#endif

    log_info("evdev device opened successfully");
    return HARDWARE_INPUT_OK;
}

static struct hw_module_methods_t hw_module_methods = {
    .open = hw_input_evdev_open,
};

static char id[NAME_MAX] = {0};

static int generate_module_id(char *id)
{
    sprintf(id, "%s.%s", INPUT_HARDWARE_MODULE_ID, INPUT_INSTANCE_EVDEV);
    return HARDWARE_INPUT_OK;
}

static struct input_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = INPUT_MODULE_API_VERSION_1_0,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = id,
        .name = "Input evdev Module",
        .author = "Anyka",
        .methods = &hw_module_methods,
    },
};

/**
 * @brief Initialize evdev module.
 */
int hw_evdev_module_init(void)
{
    generate_module_id(id);
    int ret = hw_register_module(&(HAL_MODULE_INFO_SYM.common));
    log_info("evdev module init ret: %d", ret);
    return (ret == 0) ? HARDWARE_INPUT_OK : HARDWARE_INPUT_ERROR_OPERATION_FAILED;
}

/* ============================================================================
 * EVDEV CORE - Internal interface for input_module.c
 * ============================================================================ */

typedef struct evdev_core_ctx {
    int epfd;
    int fds[64];
    int fd_count;
    pthread_t thread;
    int running;
    void (*on_event)(int dev_id, const struct input_event *ev, void *user);
    void *user_data;
    pthread_mutex_t mutex;
} evdev_core_ctx_t;

static evdev_core_ctx_t g_evdev_core = {0};

/**
 * @brief Evdev core thread function.
 */
static void *evdev_core_thread_func(void *arg)
{
    evdev_core_ctx_t *ctx = (evdev_core_ctx_t *)arg;
    struct epoll_event events[16];
    struct input_event input_ev;
    ssize_t ret;

    log_info("evdev_core thread started");

    while (ctx->running) {
        int nfds = epoll_wait(ctx->epfd, events, 16, 200);

        if (nfds < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_error("epoll_wait failed: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            while (1) {
                ret = read(fd, &input_ev, sizeof(struct input_event));
                if (ret < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    log_error("read failed: %s", strerror(errno));
                    break;
                }

                if (ret != sizeof(struct input_event)) {
                    break;
                }

                if (ctx->on_event) {
                    ctx->on_event(fd, &input_ev, ctx->user_data);
                }
            }
        }
    }

    log_info("evdev_core thread stopped");
    return NULL;
}

/**
 * @brief Add device to evdev core.
 */
static int evdev_core_add_device(int event_id, evdev_core_ctx_t *ctx)
{
    char path[64];
    int fd;
    struct epoll_event ev;

    snprintf(path, sizeof(path), "/dev/input/event%d", event_id);

    fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        return -1;
    }

    if (ctx->fd_count >= (int)(sizeof(ctx->fds)/sizeof(ctx->fds[0]))) {
        close(fd);
        return -1;
    }

    ev.events = EPOLLIN;
    ev.data.fd = fd;

    if (epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        close(fd);
        return -1;
    }

    ctx->fds[ctx->fd_count++] = fd;
    log_info("Added device to evdev_core: %s", path);
    return 0;
}

/**
 * @brief Enumerate devices for evdev core.
 */
static int evdev_core_enumerate_devices(evdev_core_ctx_t *ctx)
{
    DIR *dir;
    struct dirent *entry;
    int event_id;
    int count = 0;

    dir = opendir("/dev/input");
    if (!dir) {
        log_error("Failed to open /dev/input");
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (sscanf(entry->d_name, "event%d", &event_id) == 1) {
            if (evdev_core_add_device(event_id, ctx) == 0) {
                count++;
            }
        }
    }

    closedir(dir);
    log_info("evdev_core enumerated %d devices", count);
    return count > 0 ? 0 : -1;
}

/**
 * @brief Start evdev core (for input_module.c).
 */
int evdev_start(void (*on_event)(int dev_id, const struct input_event *ev, void *user), void *user)
{
    if (g_evdev_core.running) {
        log_warn("evdev_core already running");
        return 0;
    }

    memset(&g_evdev_core, 0, sizeof(g_evdev_core));
    g_evdev_core.on_event = on_event;
    g_evdev_core.user_data = user;

    pthread_mutex_init(&g_evdev_core.mutex, NULL);

    g_evdev_core.epfd = epoll_create1(0);
    if (g_evdev_core.epfd < 0) {
        log_error("Failed to create epoll: %s", strerror(errno));
        pthread_mutex_destroy(&g_evdev_core.mutex);
        return -1;
    }

    if (evdev_core_enumerate_devices(&g_evdev_core) != 0) {
        close(g_evdev_core.epfd);
        pthread_mutex_destroy(&g_evdev_core.mutex);
        return -1;
    }

    g_evdev_core.running = 1;
    if (pthread_create(&g_evdev_core.thread, NULL, evdev_core_thread_func, &g_evdev_core) != 0) {
        log_error("Failed to create evdev_core thread");
        g_evdev_core.running = 0;
        for (int i = 0; i < g_evdev_core.fd_count; i++) {
            close(g_evdev_core.fds[i]);
        }
        close(g_evdev_core.epfd);
        pthread_mutex_destroy(&g_evdev_core.mutex);
        return -1;
    }

    log_info("evdev_core started");
    return 0;
}

/**
 * @brief Stop evdev core (for input_module.c).
 */
int evdev_stop(void)
{
    if (!g_evdev_core.running) {
        return 0;
    }

    g_evdev_core.running = 0;
    pthread_join(g_evdev_core.thread, NULL);

    for (int i = 0; i < g_evdev_core.fd_count; i++) {
        if (g_evdev_core.fds[i] >= 0) {
            close(g_evdev_core.fds[i]);
        }
    }

    if (g_evdev_core.epfd >= 0) {
        close(g_evdev_core.epfd);
    }

    pthread_mutex_destroy(&g_evdev_core.mutex);
    memset(&g_evdev_core, 0, sizeof(g_evdev_core));

    log_info("evdev_core stopped");
    return 0;
}

