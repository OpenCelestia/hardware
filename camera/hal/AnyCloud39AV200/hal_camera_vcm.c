// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * VCM actuator control for AnyCloud39AV200 (e.g. FP5510 on I2C 0x0c).
 * Userspace path: V4L2 subdev + V4L2_CID_FOCUS_ABSOLUTE (see vcm_fp5510.ko).
 */

#include "hal_camera_vcm.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/v4l2-controls.h>
#include <linux/v4l2-subdev.h>
#include <sys/ioctl.h>

#include "log.h"

#define LOG_TAG "HAL_CAMERA_VCM"

#define V4L_SUBDEV_SYSFS_DIR "/sys/class/video4linux"
#define V4L_SUBDEV_DEV_DIR   "/dev"

struct hal_camera_vcm_ctx {
    int fd;
    int pos_min;
    int pos_max;
    char dev_path[128];
};

static int hal_camera_vcm_clamp_pos(hal_camera_vcm_ctx_t *ctx, int pos)
{
    if (!ctx) {
        return 0;
    }
    if (pos < ctx->pos_min) {
        return ctx->pos_min;
    }
    if (pos > ctx->pos_max) {
        return ctx->pos_max;
    }
    return pos;
}

static int hal_camera_vcm_read_sysfs_name(const char *subdev_name, char *name_out,
                                          size_t name_len)
{
    char path[256];
    FILE *fp;

    snprintf(path, sizeof(path), "%s/%s/name", V4L_SUBDEV_SYSFS_DIR, subdev_name);
    fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }
    if (!fgets(name_out, (int)name_len, fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    size_t n = strlen(name_out);
    while (n > 0 && (name_out[n - 1] == '\n' || name_out[n - 1] == '\r')) {
        name_out[--n] = '\0';
    }
    return 0;
}

static int hal_camera_vcm_name_matches(const char *name, const char *vcm_type)
{
    if (!name || name[0] == '\0') {
        return 0;
    }
    if (vcm_type && vcm_type[0] != '\0') {
        return (strstr(name, vcm_type) != NULL);
    }
    return (strstr(name, "fp5510") != NULL || strstr(name, "FP5510") != NULL);
}

static void hal_camera_vcm_log_available_subdevs(void)
{
    DIR *dir = opendir(V4L_SUBDEV_SYSFS_DIR);
    struct dirent *ent;

    if (!dir) {
        return;
    }
    while ((ent = readdir(dir)) != NULL) {
        char name[64];

        if (strncmp(ent->d_name, "v4l-subdev", 10) != 0) {
            continue;
        }
        if (hal_camera_vcm_read_sysfs_name(ent->d_name, name, sizeof(name)) != 0) {
            continue;
        }
        log_info("VCM: available subdev %s name='%s'", ent->d_name, name);
    }
    closedir(dir);
}

static int hal_camera_vcm_discover_subdev_once(char *dev_path_out, size_t dev_path_len,
                                               const char *vcm_type)
{
    DIR *dir = opendir(V4L_SUBDEV_SYSFS_DIR);
    if (!dir) {
        log_warn("VCM: cannot open %s: %s", V4L_SUBDEV_SYSFS_DIR, strerror(errno));
        return -1;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "v4l-subdev", 10) != 0) {
            continue;
        }

        char name[64];
        if (hal_camera_vcm_read_sysfs_name(ent->d_name, name, sizeof(name)) != 0) {
            continue;
        }
        if (!hal_camera_vcm_name_matches(name, vcm_type)) {
            continue;
        }

        snprintf(dev_path_out, dev_path_len, "%s/%s", V4L_SUBDEV_DEV_DIR, ent->d_name);
        closedir(dir);
        log_info("VCM: discovered subdev '%s' at %s", name, dev_path_out);
        return 0;
    }

    closedir(dir);
    return -1;
}

/* v4l2_async may expose the VCM subdev only after VI media bind / stream setup. */
#define VCM_DISCOVER_RETRY_COUNT 8
#define VCM_DISCOVER_RETRY_MS    50

static int hal_camera_vcm_discover_subdev(char *dev_path_out, size_t dev_path_len,
                                          const char *vcm_type)
{
    int i;

    for (i = 0; i < VCM_DISCOVER_RETRY_COUNT; i++) {
        if (hal_camera_vcm_discover_subdev_once(dev_path_out, dev_path_len, vcm_type) == 0) {
            return 0;
        }
        if (i + 1 < VCM_DISCOVER_RETRY_COUNT) {
            usleep(VCM_DISCOVER_RETRY_MS * 1000u);
        }
    }

    log_info("VCM: no subdev matching type=%s after %d tries",
             vcm_type ? vcm_type : "fp5510", VCM_DISCOVER_RETRY_COUNT);
    hal_camera_vcm_log_available_subdevs();
    return -1;
}

static int hal_camera_vcm_set_ctrl(int fd, uint32_t id, int32_t value)
{
    struct v4l2_control ctrl;

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = id;
    ctrl.value = value;
    if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0) {
        log_warn("VCM: VIDIOC_S_CTRL id=0x%x val=%d failed: %s", id, value, strerror(errno));
        return -1;
    }
    return 0;
}

static int hal_camera_vcm_get_ctrl(int fd, uint32_t id, int32_t *value_out)
{
    struct v4l2_control ctrl;

    if (!value_out) {
        return -EINVAL;
    }

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = id;
    if (ioctl(fd, VIDIOC_G_CTRL, &ctrl) < 0) {
        log_warn("VCM: VIDIOC_G_CTRL id=0x%x failed: %s", id, strerror(errno));
        return -1;
    }
    *value_out = ctrl.value;
    return 0;
}

hal_camera_vcm_ctx_t *hal_camera_vcm_open(const char *vcm_type,
                                          const char *subdev_path_override,
                                          int pos_min, int pos_max)
{
    hal_camera_vcm_ctx_t *ctx;
    char dev_path[128];

    if (subdev_path_override && subdev_path_override[0] != '\0') {
        strncpy(dev_path, subdev_path_override, sizeof(dev_path) - 1);
        dev_path[sizeof(dev_path) - 1] = '\0';
    } else if (hal_camera_vcm_discover_subdev(dev_path, sizeof(dev_path), vcm_type) != 0) {
        return NULL;
    }

    int fd = open(dev_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        log_warn("VCM: open %s failed: %s", dev_path, strerror(errno));
        return NULL;
    }

    ctx = (hal_camera_vcm_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        close(fd);
        return NULL;
    }

    ctx->fd = fd;
    ctx->pos_min = pos_min;
    ctx->pos_max = (pos_max >= pos_min) ? pos_max : pos_min;
    strncpy(ctx->dev_path, dev_path, sizeof(ctx->dev_path) - 1);
    ctx->dev_path[sizeof(ctx->dev_path) - 1] = '\0';

    log_info("VCM: opened %s range [%d,%d]", ctx->dev_path, ctx->pos_min, ctx->pos_max);
    return ctx;
}

void hal_camera_vcm_close(hal_camera_vcm_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    if (ctx->fd >= 0) {
        close(ctx->fd);
    }
    free(ctx);
}

int hal_camera_vcm_set_pos(hal_camera_vcm_ctx_t *ctx, int pos)
{
    if (!ctx || ctx->fd < 0) {
        return -EINVAL;
    }

    pos = hal_camera_vcm_clamp_pos(ctx, pos);
    if (hal_camera_vcm_set_ctrl(ctx->fd, V4L2_CID_FOCUS_ABSOLUTE, pos) != 0) {
        return -EIO;
    }
    return 0;
}

int hal_camera_vcm_get_pos(hal_camera_vcm_ctx_t *ctx, int *pos_out)
{
    int32_t val;

    if (!ctx || ctx->fd < 0 || !pos_out) {
        return -EINVAL;
    }
    if (hal_camera_vcm_get_ctrl(ctx->fd, V4L2_CID_FOCUS_ABSOLUTE, &val) != 0) {
        return -EIO;
    }
    *pos_out = (int)val;
    return 0;
}

int hal_camera_vcm_get_range(hal_camera_vcm_ctx_t *ctx, int *min_out, int *max_out)
{
    if (!ctx || !min_out || !max_out) {
        return -EINVAL;
    }
    *min_out = ctx->pos_min;
    *max_out = ctx->pos_max;
    return 0;
}
