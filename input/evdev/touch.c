// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/input/evdev.
 *
 * Touch device implementation.
 * - Implements evdev_device_t methods for touchscreen input.
 * - Supports tslib on Linux and direct touch on RTOS.
 */
#include "hardware.h"
#include "input_evdev.h"
#include "evdev_hw.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>

#include "log.h"
#include "ak_common.h"
#include "app_globle_config.h"

#define LOG_TAG "HAL TOUCH"

#if CONFIG_OS_LINUX
#include "tslib.h"
#define TOUCH_DEVICE                "TSLIB_TSDEVICE"

static const char *k_tslib_plugin_dirs[] = {
    "/usr/bin/tslib/lib/ts",
    "/usr/lib/ts",
    "/usr/lib/tslib",
    "/usr/local/lib/ts",
    "/usr/local/lib/tslib",
};

static const char *k_tslib_conf_paths[] = {
    "/etc/config/ts.rgb.conf",
    "/etc/config/ts_calibrate.rgb.conf",
};

static int path_exists(const char *path)
{
    return (path != NULL && access(path, F_OK) == 0);
}

static const char *pick_tslib_conffile(void)
{
    const char *env = getenv("TSLIB_CONFFILE");
    size_t i;

    /* Trust env var directly: skip access() to avoid ~100ms squashfs cold read on NOR flash. */
    if (env && env[0])
        return env;
    for (i = 0; i < sizeof(k_tslib_conf_paths) / sizeof(k_tslib_conf_paths[0]); i++) {
        if (path_exists(k_tslib_conf_paths[i])) {
            return k_tslib_conf_paths[i];
        }
    }
    return NULL;
}

/* module_raw input in ts.conf needs ${TSLIB_PLUGINDIR}/input.so */
static int tslib_has_input_plugin(const char *plugindir)
{
    char path[256];

    if (!path_exists(plugindir)) {
        return 0;
    }
    snprintf(path, sizeof(path), "%s/input.so", plugindir);
    return path_exists(path);
}

static const char *pick_tslib_plugindir(void)
{
    const char *env = getenv("TSLIB_PLUGINDIR");
    size_t i;

    /* Trust env var directly: skip access() to avoid ~200ms squashfs cold read on NOR flash. */
    if (env && env[0])
        return env;
    for (i = 0; i < sizeof(k_tslib_plugin_dirs) / sizeof(k_tslib_plugin_dirs[0]); i++) {
        if (tslib_has_input_plugin(k_tslib_plugin_dirs[i])) {
            return k_tslib_plugin_dirs[i];
        }
    }
    return NULL;
}

static int tslib_stack_usable(const char **out_conf, const char **out_plugin)
{
    const char *conf = pick_tslib_conffile();
    const char *plugin = pick_tslib_plugindir();

    if (conf == NULL || plugin == NULL) {
        return 0;
    }
    if (out_conf != NULL) {
        *out_conf = conf;
    }
    if (out_plugin != NULL) {
        *out_plugin = plugin;
    }
    return 1;
}

static void ensure_env_contains(const char *key, const char *value)
{
    const char *oldv = getenv(key);
    if (oldv == NULL || strstr(oldv, value) == NULL) {
        char buf[512] = {0};
        if (oldv != NULL && oldv[0] != '\0') {
            snprintf(buf, sizeof(buf), "%s:%s", oldv, value);
        } else {
            snprintf(buf, sizeof(buf), "%s", value);
        }
        setenv(key, buf, 1);
    }
}

static const char *select_tslib_plugin_dir(void)
{
    return pick_tslib_plugindir();
}
#else
#include "ts_ioctl.h"
#define TOUCH_DEVICE                "/dev/ts"

#define TP_EVENT_NONE               0
#define TP_EVENT_DOWN               1
#define TP_EVENT_MOVE               2
#define TP_EVENT_STAY               3
#define TP_EVENT_UP                 4
#define TP_EVENT_RELEASE            5
#endif

#if CONFIG_OS_LINUX
static char *get_ts_device(void);
static int read_input_sysfs_name(int event_id, char *name, size_t name_size);
static int touch_name_matches(const char *name);

typedef struct {
    int abs_max_x;
    int abs_max_y;
    int cal_a;
    int cal_b;
    int cal_c;
    int cal_d;
    int cal_e;
    int cal_f;
    int cal_s;
    int cal_loaded;
} touch_raw_ctx_t;

static int s_skip_tslib_for_cst;

static touch_raw_ctx_t s_touch_raw = {
    .abs_max_x = LCD_HOR_RES_MAX - 1,
    .abs_max_y = LCD_VER_RES_MAX - 1,
    .cal_a = 65536,
    .cal_b = 0,
    .cal_c = 0,
    .cal_d = 0,
    .cal_e = 65536,
    .cal_f = 0,
    .cal_s = 65536,
    .cal_loaded = 0,
};

static void touch_raw_load_calib(void)
{
    const char *calib;
    FILE *fp;
    int vals[11] = {0};
    int n;

    if (s_skip_tslib_for_cst)
        return;

    calib = getenv("TSLIB_CALIBFILE");
    if (!path_exists(calib))
        calib = "/etc/config/ts_calibrate.rgb.conf";

    fp = fopen(calib, "r");
    if (!fp)
        return;

    n = fscanf(fp, "%d %d %d %d %d %d %d %d %d %d %d",
                   &vals[0], &vals[1], &vals[2], &vals[3], &vals[4],
                   &vals[5], &vals[6], &vals[7], &vals[8], &vals[9], &vals[10]);
    fclose(fp);

    if (n < 7)
        return;

    s_touch_raw.cal_a = vals[0];
    s_touch_raw.cal_b = vals[1];
    s_touch_raw.cal_c = vals[2];
    s_touch_raw.cal_d = vals[3];
    s_touch_raw.cal_e = vals[4];
    s_touch_raw.cal_f = vals[5];
    s_touch_raw.cal_s = (vals[6] != 0) ? vals[6] : 65536;
    s_touch_raw.cal_loaded = 1;
    log_info("touch raw calib loaded: %s", calib);
}

static void touch_raw_init_axis(int fd)
{
    struct input_absinfo absinfo;

    touch_raw_load_calib();

    if (ioctl(fd, EVIOCGABS(ABS_X), &absinfo) == 0 && absinfo.maximum > 0)
        s_touch_raw.abs_max_x = absinfo.maximum;
    if (ioctl(fd, EVIOCGABS(ABS_Y), &absinfo) == 0 && absinfo.maximum > 0)
        s_touch_raw.abs_max_y = absinfo.maximum;

    log_info("touch raw axis max: x=%d y=%d", s_touch_raw.abs_max_x, s_touch_raw.abs_max_y);
}

static void touch_raw_map(int *x, int *y)
{
    long long rx = *x;
    long long ry = *y;

    if (s_touch_raw.abs_max_x > 0 && s_touch_raw.abs_max_x != (LCD_HOR_RES_MAX - 1)) {
        rx = rx * (LCD_HOR_RES_MAX - 1) / s_touch_raw.abs_max_x;
    }
    if (s_touch_raw.abs_max_y > 0 && s_touch_raw.abs_max_y != (LCD_VER_RES_MAX - 1)) {
        ry = ry * (LCD_VER_RES_MAX - 1) / s_touch_raw.abs_max_y;
    }

    if (s_touch_raw.cal_loaded) {
        long long sx = s_touch_raw.cal_a * rx + s_touch_raw.cal_b * ry + s_touch_raw.cal_c;
        long long sy = s_touch_raw.cal_d * rx + s_touch_raw.cal_e * ry + s_touch_raw.cal_f;
        rx = sx / s_touch_raw.cal_s;
        ry = sy / s_touch_raw.cal_s;
    }

    if (rx < 0)
        rx = 0;
    if (ry < 0)
        ry = 0;
    if (rx >= LCD_HOR_RES_MAX)
        rx = LCD_HOR_RES_MAX - 1;
    if (ry >= LCD_VER_RES_MAX)
        ry = LCD_VER_RES_MAX - 1;

    *x = (int)rx;
    *y = (int)ry;
}

static int read_touch_event_raw(int fd, struct evdev_event *event)
{
    static int last_x = 0;
    static int last_y = 0;
    static int last_pressed = 0;
    int updated = 0;
    struct input_event iev;

    while (1) {
        ssize_t ret = read(fd, &iev, sizeof(iev));
        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            return HARDWARE_INPUT_ERROR_OPERATION_FAILED;
        }

        if (ret != (ssize_t)sizeof(iev)) {
            break;
        }

        if (iev.type == EV_ABS) {
            if (iev.code == ABS_X || iev.code == ABS_MT_POSITION_X) {
                last_x = iev.value;
                updated = 1;
            } else if (iev.code == ABS_Y || iev.code == ABS_MT_POSITION_Y) {
                last_y = iev.value;
                updated = 1;
            } else if (iev.code == ABS_PRESSURE || iev.code == ABS_MT_PRESSURE) {
                last_pressed = (iev.value > 0) ? 1 : 0;
                updated = 1;
            }
        } else if (iev.type == EV_KEY) {
            if (iev.code == BTN_TOUCH || iev.code == BTN_LEFT) {
                last_pressed = (iev.value > 0) ? 1 : 0;
                updated = 1;
            }
        } else if (iev.type == EV_SYN && iev.code == SYN_REPORT && updated) {
            touch_raw_map(&last_x, &last_y);
            memset(event, 0, sizeof(*event));
            event->time = iev.time;
            event->type = EV_ABS;
            event->code = ABS_X;
            event->xpos = last_x;
            event->ypos = last_y;
            event->value = last_pressed;
            return HARDWARE_INPUT_OK;
        }
    }

    return HARDWARE_INPUT_ERROR_OPERATION_FAILED;
}

/*
 * Contact coalescing for noisy resistive / poorly calibrated panels.
 *
 * Shared by Luna (240x240), Nova (800x480), Nova-dev (1024x600):
 *  - Swallow brief pressure 1↔0 flicker during one finger stroke
 *  - Drop top-edge / teleport ghost samples while pressed
 *
 * Luna-only (wrong donor 1024x600 calib on 240x240 glass):
 *  - Stretch Y from ~0..120 onto full framebuffer
 *  - Shift X right ~45px so UI hit targets match the finger
 * Nova / Nova-dev must NOT get that XY remap — they use matching calib.
 */
#define TOUCH_REL_DEBOUNCE_MS      100
#define TOUCH_GHOST_Y_MAX          ((LCD_VER_RES_MAX) / 30)   /* ~8 on 240, ~16 on 480, ~20 on 600 */
#define TOUCH_GHOST_PREV_Y_MIN     ((LCD_VER_RES_MAX) / 15)
/* Fast swipe step scales with panel width (80px on 240, ~160 on 800, ~200 on 1024). */
#define TOUCH_TELEPORT_PX          (((LCD_HOR_RES_MAX) / 3) > 80 ? ((LCD_HOR_RES_MAX) / 3) : 80)

#if (LCD_HOR_RES_MAX == 240) && (LCD_VER_RES_MAX == 240)
#define TOUCH_LUNA_XY_FIX          1
#define TOUCH_Y_CALIB_SPAN         120
#define TOUCH_X_CALIB_OFFSET       45
#else
#define TOUCH_LUNA_XY_FIX          0
#endif

static int s_tc_down;
static int s_tc_rel_pend;
static uint32_t s_tc_rel_ms;
static int s_tc_x;
static int s_tc_y;
static int s_tc_have;

static uint32_t touch_mono_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static void touch_clamp_xy(struct evdev_event *event)
{
    if (!event)
        return;
    if (event->xpos < 0)
        event->xpos = 0;
    if (event->ypos < 0)
        event->ypos = 0;
    if (event->xpos >= LCD_HOR_RES_MAX)
        event->xpos = LCD_HOR_RES_MAX - 1;
    if (event->ypos >= LCD_VER_RES_MAX)
        event->ypos = LCD_VER_RES_MAX - 1;
}

static void touch_fix_panel_xy(struct evdev_event *event)
{
    if (!event)
        return;

#if TOUCH_LUNA_XY_FIX
    {
        int x;
        int y;

        /*
         * Luna KM01W only: donor 1024x600 calib compresses Y to ~0..120 and
         * shifts X left of the visual target. Do not apply on Nova panels.
         */
        y = event->ypos;
        if (y < 0)
            y = 0;
        y = y * LCD_VER_RES_MAX / TOUCH_Y_CALIB_SPAN;
        if (y >= LCD_VER_RES_MAX)
            y = LCD_VER_RES_MAX - 1;
        event->ypos = y;

        x = event->xpos + TOUCH_X_CALIB_OFFSET;
        if (x < 0)
            x = 0;
        if (x >= LCD_HOR_RES_MAX)
            x = LCD_HOR_RES_MAX - 1;
        event->xpos = x;
    }
#else
    touch_clamp_xy(event);
#endif
}

/** @return 1 deliver event, 0 drop this sample */
static int touch_coalesce_sample(struct evdev_event *event)
{
    uint32_t now;
    int contact;
    int jump;

    if (!event)
        return 0;

    touch_fix_panel_xy(event);

    now = touch_mono_ms();
    contact = (event->value > 0) ? 1 : 0;

    if (contact) {
        if (s_tc_have) {
            jump = abs(event->xpos - s_tc_x) + abs(event->ypos - s_tc_y);
            if ((event->ypos <= TOUCH_GHOST_Y_MAX && s_tc_y > TOUCH_GHOST_PREV_Y_MIN) ||
                jump > TOUCH_TELEPORT_PX) {
                /* keep last good point — ghost track / lift glitch */
            } else {
                s_tc_x = event->xpos;
                s_tc_y = event->ypos;
            }
        } else if (!(event->xpos == 0 && event->ypos == 0)) {
            s_tc_x = event->xpos;
            s_tc_y = event->ypos;
            s_tc_have = 1;
        }
        if (s_tc_have) {
            event->xpos = s_tc_x;
            event->ypos = s_tc_y;
        }
        event->value = 1;
        s_tc_rel_pend = 0;
        s_tc_down = 1;
        return 1;
    }

    if (!s_tc_down)
        return 0;

    if (!s_tc_rel_pend) {
        s_tc_rel_pend = 1;
        s_tc_rel_ms = now;
        return 0;
    }
    if ((now - s_tc_rel_ms) < TOUCH_REL_DEBOUNCE_MS)
        return 0;

    event->value = 0;
    if (s_tc_have) {
        event->xpos = s_tc_x;
        event->ypos = s_tc_y;
    }
    s_tc_down = 0;
    s_tc_rel_pend = 0;
    s_tc_have = 0;
    return 1;
}

/** Synthesize UP when lift samples stop before debounce completes. */
static int touch_coalesce_timeout_up(struct evdev_event *event)
{
    uint32_t now;

    if (!event || !s_tc_down || !s_tc_rel_pend)
        return 0;

    now = touch_mono_ms();
    if ((now - s_tc_rel_ms) < TOUCH_REL_DEBOUNCE_MS)
        return 0;

    memset(event, 0, sizeof(*event));
    event->type = EV_ABS;
    event->value = 0;
    event->xpos = s_tc_x;
    event->ypos = s_tc_y;
    s_tc_down = 0;
    s_tc_rel_pend = 0;
    s_tc_have = 0;
    return 1;
}
#endif

/**
 * @brief Synchronize touch device and process pending events.
 * @param device Device context.
 * @param timeout Timeout for waiting events.
 * @return 0 on success, negative error code on failure.
 */
int hw_touch_sync(struct evdev_device *device, struct timeval *timeout)
{
    if (NULL == device) {
        log_error("parameter error");
        return HARDWARE_INPUT_ERROR_INVALID_PARAM;
    }

    struct evdev_event event = {0};
    evdev_subdev_context_t *ctx = (evdev_subdev_context_t *)device->context;

#if CONFIG_OS_LINUX
    struct tsdev *tsd = (struct tsdev *)(device->common.handle);
    if (tsd != NULL) {
        struct ts_sample samp = {0};

        if (ts_read(tsd, &samp, 1) == 1) {
            event.type = EV_ABS;
            event.xpos = samp.x;
            event.ypos = samp.y;
            event.value = samp.pressure;

            if (!touch_coalesce_sample(&event)) {
                if (touch_coalesce_timeout_up(&event) && ctx->event_callback) {
                    ctx->event_callback(&event);
                    return HARDWARE_INPUT_OK;
                }
                return HARDWARE_INPUT_ERROR_OPERATION_FAILED;
            }

            if (ctx->event_callback)
                ctx->event_callback(&event);

            log_debug("ts x: %6d, ts y: %6d, ts pressure: %6d",
                      event.xpos, event.ypos, event.value);
            return HARDWARE_INPUT_OK;
        }
        if (touch_coalesce_timeout_up(&event)) {
            if (ctx->event_callback)
                ctx->event_callback(&event);
            return HARDWARE_INPUT_OK;
        }
    } else if (device->common.fd >= 0) {
        if (read_touch_event_raw(device->common.fd, &event) == HARDWARE_INPUT_OK) {
            if (!touch_coalesce_sample(&event)) {
                if (touch_coalesce_timeout_up(&event) && ctx->event_callback) {
                    ctx->event_callback(&event);
                    return HARDWARE_INPUT_OK;
                }
                return HARDWARE_INPUT_ERROR_OPERATION_FAILED;
            }
            if (ctx->event_callback)
                ctx->event_callback(&event);
            return HARDWARE_INPUT_OK;
        }
        if (touch_coalesce_timeout_up(&event)) {
            if (ctx->event_callback)
                ctx->event_callback(&event);
            return HARDWARE_INPUT_OK;
        }
    }
#else
    int fd = device->common.fd;
    if (fd < 0) {
        log_error("device has not been opened");
        return HARDWARE_INPUT_ERROR_NOT_INIT;
    }

    struct ts_report report;

    int ret = read(fd, &report, sizeof(struct ts_report));
    if (sizeof(struct ts_report) == ret) {
        event.type = EV_ABS;
        event.xpos = report.xpos;
        event.ypos = report.ypos;
        if ((report.event_id == TP_EVENT_NONE) || (report.event_id == TP_EVENT_UP) || (report.event_id == TP_EVENT_RELEASE))
            event.value = 0;
        else
            event.value = 1;

        if (ctx->event_callback)
            ctx->event_callback(&event);

        log_debug("ts x: %6d, ts y: %6d, ts pressure: %6d", event.xpos, event.ypos, event.value);
        return HARDWARE_INPUT_OK;
    }
#endif

    return HARDWARE_INPUT_ERROR_OPERATION_FAILED;
}

/**
 * @brief Read event from touch device.
 * @param device Device context.
 * @param event Output event structure.
 * @param timeout Timeout for waiting events.
 * @return 0 on success, negative error code on failure.
 */
int hw_touch_read(struct evdev_device *device, struct evdev_event *event, struct timeval *timeout)
{
    if (NULL == device || !event) {
        log_error("parameter error");
        return HARDWARE_INPUT_ERROR_INVALID_PARAM;
    }

#if CONFIG_OS_LINUX
    struct tsdev *tsd = (struct tsdev *)(device->common.handle);
    if (tsd != NULL) {
        struct ts_sample samp = {0};

        if (ts_read(tsd, &samp, 1) == 1) {
            event->type = EV_ABS;
            event->xpos = samp.x;
            event->ypos = samp.y;
            event->value = samp.pressure;

            if (!touch_coalesce_sample(event)) {
                if (touch_coalesce_timeout_up(event))
                    return HARDWARE_INPUT_OK;
                return HARDWARE_INPUT_ERROR_OPERATION_FAILED;
            }

            log_debug("ts x: %6d, ts y: %6d, ts pressure: %6d",
                      event->xpos, event->ypos, event->value);
            return HARDWARE_INPUT_OK;
        }
        if (touch_coalesce_timeout_up(event))
            return HARDWARE_INPUT_OK;
    } else if (device->common.fd >= 0) {
        int fd = device->common.fd;
        fd_set readfds;
        struct timeval tv_zero = {0, 0};
        struct timeval *ptv = timeout;

        if (ptv == NULL) {
            ptv = &tv_zero;
        }

        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);
        if (select(fd + 1, &readfds, NULL, NULL, ptv) < 0) {
            log_error("touch select failed: %s", strerror(errno));
            return HARDWARE_INPUT_ERROR_OPERATION_FAILED;
        }
        if (!FD_ISSET(fd, &readfds)) {
            if (touch_coalesce_timeout_up(event))
                return HARDWARE_INPUT_OK;
            return HARDWARE_INPUT_ERROR_OPERATION_FAILED;
        }
        if (read_touch_event_raw(fd, event) == HARDWARE_INPUT_OK) {
            if (!touch_coalesce_sample(event)) {
                if (touch_coalesce_timeout_up(event))
                    return HARDWARE_INPUT_OK;
                return HARDWARE_INPUT_ERROR_OPERATION_FAILED;
            }
            return HARDWARE_INPUT_OK;
        }
        if (touch_coalesce_timeout_up(event))
            return HARDWARE_INPUT_OK;
    } else {
        log_error("touch input has neither tslib handle nor valid fd");
        return HARDWARE_INPUT_ERROR_NOT_INIT;
    }
#else
    int fd = device->common.fd;

    if (fd < 0) {
        log_error("device has not been opened");
        return HARDWARE_INPUT_ERROR_NOT_INIT;
    }

    struct ts_report report;

    int ret = read(fd, &report, sizeof(struct ts_report));
    if (sizeof(struct ts_report) == ret) {
        event->type = EV_ABS;
        event->xpos = report.xpos;
        event->ypos = report.ypos;
        if ((report.event_id == TP_EVENT_NONE) || (report.event_id == TP_EVENT_UP) || (report.event_id == TP_EVENT_RELEASE))
            event->value = 0;
        else
            event->value = 1;

        log_debug("ts x: %6d, ts y: %6d, ts pressure: %6d", event->xpos, event->ypos, event->value);
        return HARDWARE_INPUT_OK;
    }
#endif

    return HARDWARE_INPUT_ERROR_OPERATION_FAILED;
}

/**
 * @brief Set touch event callback.
 * @param device Device context.
 * @param callback Event callback function.
 * @return 0 on success, negative error code on failure.
 */
int hw_touch_set_callback(struct evdev_device *device, evdev_event_callback_t callback)
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
 * @brief Close touch device.
 */
static int hw_touch_close(struct hw_device_t* device)
{
    if (NULL == device) {
        log_error("parameter error");
        return HARDWARE_INPUT_ERROR_INVALID_PARAM;
    }

#if CONFIG_OS_LINUX
    struct tsdev *tsd = (struct tsdev *)((struct input_device *)device)->handle;
    int fd = ((struct input_device *)device)->fd;
    if (tsd != NULL) {
        ts_close(tsd);
    }
    if (fd >= 0) {
        close(fd);
    }
#elif CONFIG_OS_RTOS
    int fd = ((struct input_device *)device)->fd;

    if (fd < 0) {
        log_error("device hasn't been opened");
        return HARDWARE_INPUT_ERROR_NOT_INIT;
    }

    close(fd);
#endif

    void *ctx = ((struct evdev_device *)device)->context;
    free(ctx);
    free(device);

    return HARDWARE_INPUT_OK;
}

/**
 * @brief Open touch device.
 * @param module Hardware module.
 * @param name Device name.
 * @param device Output device handle.
 * @return 0 on success, negative error code on failure.
 */
int hw_touch_open(const hw_module_t* module, const char* name, hw_device_t** device)
{
    *device = NULL;

    if ((NULL == module) || (NULL == name) || (NULL == device)) {
        log_error("parameter error");
        return HARDWARE_INPUT_ERROR_INVALID_PARAM;
    }

    int fd = -1;
    void *handle = NULL;

#if CONFIG_OS_LINUX
    const char *conffile = NULL;
    const char *plugindir = NULL;
    int use_tslib = tslib_stack_usable(&conffile, &plugindir);

    if (use_tslib) {
        setenv("TSLIB_CONFFILE", conffile, 1);
        setenv("TSLIB_PLUGINDIR", plugindir, 1);
    } else if (pick_tslib_conffile() != NULL) {
        log_warn("ts.conf present but tslib input.so missing under TSLIB_PLUGINDIR, using raw evdev");
    }

    /* Trust TSLIB_CALIBFILE if already set by startup script; skip access() (NOR flash cold read). */
    const char *cal_env = getenv("TSLIB_CALIBFILE");
    if (!cal_env || !cal_env[0]) {
        static const char *cal_paths[] = {
            "/etc/config/ts_calibrate.rgb.conf",
        };
        size_t ci;
        for (ci = 0; ci < sizeof(cal_paths) / sizeof(cal_paths[0]); ci++) {
            if (path_exists(cal_paths[ci])) {
                setenv("TSLIB_CALIBFILE", cal_paths[ci], 1);
                break;
            }
        }
    }

    /* Do not pin event0/event1: keys often take event0. get_ts_device() scans by name. */
    if (getenv("TSLIB_CONSOLEDEVICE") == NULL) {
        setenv("TSLIB_CONSOLEDEVICE", "none", 0);
    }
    if (use_tslib) {
        ensure_env_contains("LD_LIBRARY_PATH", "/usr/bin/tslib/lib");
    }

    char *tsdevice = get_ts_device();
    if (NULL == tsdevice) {
        log_error("Failed to get handle of ts device.");
        return HARDWARE_INPUT_ERROR_NOT_FOUND;
    }

    {
        char sysfs_name[64] = {0};
        int evn = -1;

        if (sscanf(tsdevice, "/dev/input/event%d", &evn) == 1)
            (void)read_input_sysfs_name(evn, sysfs_name, sizeof(sysfs_name));
        if (touch_name_matches(sysfs_name)) {
            use_tslib = 0;
            s_skip_tslib_for_cst = 1;
            log_info("skip tslib for capacitive %s (%s)", tsdevice, sysfs_name);
        }
    }

    /* No tslib conf/plugins: raw evdev (CST816 on KM01W). */
    if (!use_tslib) {
        fd = open(tsdevice, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            log_error("raw evdev touch open failed: %s", tsdevice);
            return HARDWARE_INPUT_ERROR_OPERATION_FAILED;
        }
        touch_raw_init_axis(fd);
        handle = NULL;
        log_info("touch raw evdev (no tslib conf): %s, fd=%d", tsdevice, fd);
        goto touch_open_ok;
    }

    struct tsdev *tsd = ts_open(tsdevice, 1);
    if (NULL == tsd) {
        log_error("Failed to open ts device: %s", tsdevice);
        return HARDWARE_INPUT_ERROR_OPERATION_FAILED;
    }

    if (ts_config(tsd) != 0) {
        size_t i;
        int configured = 0;

        /* Retry with other common plugin dirs to tolerate rootfs path differences */
        for (i = 0; i < sizeof(k_tslib_plugin_dirs) / sizeof(k_tslib_plugin_dirs[0]); i++) {
            const char *cand = k_tslib_plugin_dirs[i];
            if (!path_exists(cand)) {
                continue;
            }

            if (getenv("TSLIB_PLUGINDIR") != NULL && strcmp(getenv("TSLIB_PLUGINDIR"), cand) == 0) {
                continue;
            }

            setenv("TSLIB_PLUGINDIR", cand, 1);
            if (ts_config(tsd) == 0) {
                configured = 1;
                log_info("tslib configured with fallback plugindir: %s", cand);
                break;
            }
        }

        if (configured) {
            handle = (void *)tsd;
            goto touch_open_ok;
        }

        log_warn("Failed to configure tslib, fallback to raw evdev touch");
        log_warn("  TSLIB_CONFFILE=%s", getenv("TSLIB_CONFFILE") ? getenv("TSLIB_CONFFILE") : "(null)");
        log_warn("  TSLIB_PLUGINDIR=%s", getenv("TSLIB_PLUGINDIR") ? getenv("TSLIB_PLUGINDIR") : "(null)");
        log_warn("  TSLIB_TSDEVICE=%s", getenv("TSLIB_TSDEVICE") ? getenv("TSLIB_TSDEVICE") : "(null)");
        ts_close(tsd);

        fd = open(tsdevice, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            log_error("raw evdev touch fallback open failed: %s", tsdevice);
            return HARDWARE_INPUT_ERROR_OPERATION_FAILED;
        }
        touch_raw_init_axis(fd);
        handle = NULL;
        log_info("touch raw fallback opened: %s, fd=%d", tsdevice, fd);
        goto touch_open_ok;
    }
    
    log_info("tslib opened: device=%s, handle=%p", tsdevice, (void*)tsd);
    handle = (void *)tsd;
touch_open_ok: ;
#else
    if (access(TOUCH_DEVICE, F_OK) != 0) {
        log_error("No ts device.");
        return HARDWARE_INPUT_ERROR_NOT_FOUND;
    }

    fd = open(TOUCH_DEVICE, O_RDONLY);
    if (fd < 0) {
        log_error("Failed to open ts device.");
        return HARDWARE_INPUT_ERROR_OPERATION_FAILED;
    }
#endif

    struct evdev_device *dev = NULL;
    dev = calloc(1, sizeof(struct evdev_device));
    if (NULL == dev) {
        log_error("malloc for information struct of device failed");
        if (handle) {
#if CONFIG_OS_LINUX
            ts_close((struct tsdev *)handle);
#else
            close(fd);
#endif
        }
        if (fd >= 0) {
            close(fd);
        }
        return HARDWARE_INPUT_ERROR_MEMORY;
    }

    dev->common.common.tag = HARDWARE_DEVICE_TAG;
    dev->common.common.version = INPUT_EVDEV_TOUCH_API_VERSION_1_0;
    dev->common.common.module = (struct hw_module_t *) module;
    dev->common.common.close = hw_touch_close;

    dev->sync = hw_touch_sync;
    dev->read = hw_touch_read;
    dev->set_event_callback = hw_touch_set_callback;

    dev->common.fd = fd;
    dev->common.handle = handle;

    *device = (hw_device_t *)(&dev->common);

    void *ctx = malloc(sizeof(evdev_subdev_context_t));
    if (!ctx) {
#if CONFIG_OS_LINUX
        if (handle) {
            ts_close((struct tsdev *)handle);
        }
#else
        close(fd);
#endif
        if (fd >= 0) {
            close(fd);
        }
        free(dev);
        return HARDWARE_INPUT_ERROR_MEMORY;
    }
    memset(ctx, 0, sizeof(evdev_subdev_context_t));
    dev->context = ctx;

    return HARDWARE_INPUT_OK;
}

#if CONFIG_OS_LINUX

#define DIV_ROUND_UP(n, d)  (((n) + (d) - 1) / (d))
#define BIT(nr)             (1UL << (nr))
#define BIT_MASK(nr)        (1UL << ((nr) % BITS_PER_LONG))
#define BIT_WORD(nr)        ((nr) / BITS_PER_LONG)
#define BITS_PER_BYTE       8
#define BITS_PER_LONG       (sizeof(long) * BITS_PER_BYTE)
#define BITS_TO_LONGS(nr)   DIV_ROUND_UP(nr, BITS_PER_BYTE * sizeof(long))

/**
 * @brief Find touchscreen device path.
 * @return Device path or NULL if not found.
 */
/* Shared AV100_V2/AV200: sysfs name + EV_ABS scan (no SoC-specific API). */

static int read_input_sysfs_name(int event_id, char *name, size_t name_size)
{
    char path[128];
    int fd;
    ssize_t ret;

    snprintf(path, sizeof(path), "/sys/class/input/event%d/device/name", event_id);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    memset(name, 0, name_size);
    ret = read(fd, name, name_size - 1);
    close(fd);

    if (ret <= 0) {
        return -1;
    }
    if (name[ret - 1] == '\n') {
        name[ret - 1] = '\0';
    }
    return 0;
}

static int touch_name_matches(const char *name)
{
    if (!name || !name[0]) {
        return 0;
    }
    return (strstr(name, "cst816") != NULL) ||
           (strstr(name, "CST816") != NULL) ||
           (strstr(name, "touch") != NULL) ||
           (strstr(name, "Touch") != NULL) ||
           (strstr(name, "TOUCH") != NULL);
}

static int event_has_abs(const char *path)
{
    int fd;
    long evbit[BITS_TO_LONGS(EV_CNT)];

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    memset(evbit, 0, sizeof(evbit));
    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit) <= 0) {
        close(fd);
        return 0;
    }
    close(fd);
    return (evbit[BIT_WORD(EV_ABS)] & BIT_MASK(EV_ABS)) != 0;
}

static char *scan_ts_device_once(void)
{
    int i;
    static char str[32];
    static char abs_fallback[32];
    char dev_name[64];
    char *env_dev = getenv("TSLIB_TSDEVICE");

    /* Trust TSLIB_TSDEVICE only if it is an absolute-axis node (not gpiokeys). */
    if (env_dev != NULL && env_dev[0] && access(env_dev, F_OK) == 0 &&
        event_has_abs(env_dev)) {
        log_info("using TSLIB_TSDEVICE: %s", env_dev);
        return env_dev;
    }

    for (i = 0; i < 16; i++) {
        snprintf(str, sizeof(str), "/dev/input/event%d", i);
        if (access(str, F_OK) != 0) {
            continue;
        }
        if (read_input_sysfs_name(i, dev_name, sizeof(dev_name)) != 0) {
            continue;
        }
        if (!touch_name_matches(dev_name)) {
            continue;
        }
        if (event_has_abs(str)) {
            log_info("found touch device by name '%s': %s", dev_name, str);
            return str;
        }
    }

    for (i = 0; i < 16; i++) {
        snprintf(abs_fallback, sizeof(abs_fallback), "/dev/input/event%d", i);
        if (access(abs_fallback, F_OK) != 0) {
            continue;
        }
        if (event_has_abs(abs_fallback)) {
            log_info("found touch device (EV_ABS): %s", abs_fallback);
            return abs_fallback;
        }
    }

    return NULL;
}

static char *get_ts_device(void)
{
    static char found[32];
    int attempt;
    char *dev;

    for (attempt = 0; attempt < 3; attempt++) {
        dev = scan_ts_device_once();
        if (dev) {
            snprintf(found, sizeof(found), "%s", dev);
            return found;
        }
        usleep(40000);
    }
    return NULL;
}
#endif

