// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/driver/blkdev.
 *
 * MMC/SD card driver module implementation.
 * - Implements mmc_device_t methods for block device operations.
 * - Supports mount, unmount, format, and fsck operations.
 */

#include "mmc.h"

#if CONFIG_OS_LINUX
#include <linux/kernel.h>
#endif
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#if CONFIG_OS_LINUX
#include <fcntl.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#endif

#include "log.h"
#include "ak_common.h"
#include "driver_error.h"

#if !CONFIG_OS_LINUX
#include <fcntl.h>
#include <sys/ioctl.h>
#include "mmc_ioctl.h"
#include "mmc_akfat_aos.h"
#endif

#define LOG_TAG "HAL MMC"

#if !CONFIG_OS_LINUX
static const char *hw_mmc_aos_fstype(const char *fs)
{
    if (fs == NULL || fs[0] == '\0')
        return "akfat";
    if (strcmp(fs, "vfat") == 0 || strcmp(fs, "fat") == 0 || strcmp(fs, "fat32") == 0)
        return "akfat";
    return fs;
}
#endif

#if !CONFIG_OS_LINUX
static int hw_mmc_aos_prepare_card(const char *name)
{
    int fd;

    if (name == NULL || name[0] == '\0')
        return -1;

    fd = open(name, O_RDWR);
    if (fd < 0) {
        log_warn("open %s before format failed (%s)", name, strerror(errno));
        return -1;
    }

    (void)ioctl(fd, MMC_INIT_CARD, NULL);
    (void)ioctl(fd, MMC_IDENTIFICATION, NULL);
    close(fd);
    return 0;
}
#endif

/**
 * @brief Mount MMC device.
 */
static int hw_mmc_mount(struct mmc_device *device, const char *name, char *path, char *fs)
{
    if ((NULL == device) || (NULL == name) || (NULL == path) || (NULL == fs))
    {
        log_error("parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    if (access(path, F_OK) != 0) {
        if (mkdir(path, 0755) != 0 && errno != EEXIST) {
            log_error("mkdir mount point failed: %s (%s)", path, strerror(errno));
            return HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
        }
    }

#if CONFIG_OS_LINUX
    char cmd[128] = {0};
    snprintf(cmd, sizeof(cmd), "mount -t %s %s %s", fs, name, path);
    int ret = system(cmd);
#else
    const char *fstype = hw_mmc_aos_fstype(fs);
    int ret = mount(name, path, fstype, 0, NULL);
#endif
    return (ret == 0) ? HARDWARE_DRIVER_OK : HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
}

/**
 * @brief Unmount MMC device.
 */
static int hw_mmc_unmount(struct mmc_device *device, char *path)
{
    if ((NULL == device) || (NULL == path))
    {
        log_error("parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

#if CONFIG_OS_LINUX
    char cmd[128] = {0};
    snprintf(cmd, sizeof(cmd), "umount %s", path);
    int ret = system(cmd);
#else
    int ret = umount(path);
#endif
    return (ret == 0) ? HARDWARE_DRIVER_OK : HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
}

/**
 * @brief Format MMC device.
 */
static int hw_mmc_format(struct mmc_device *device, const char *name, char *fs)
{
    if ((NULL == device) || (NULL == name) || (NULL == fs))
    {
        log_error("parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

#if CONFIG_OS_LINUX
    char cmd[128] = {0};
    snprintf(cmd, sizeof(cmd), "mkfs.%s -vI %s", fs, name);
    int ret = system(cmd);
#else
    const char *fstype = hw_mmc_aos_fstype(fs);

    (void)fstype;
    (void)hw_mmc_aos_prepare_card(name);
    int ret = Driver_QuickFormat((unsigned char *)(uintptr_t)name);
    if (ret != 0)
        log_error("Driver_QuickFormat(%s) failed ret=%d", name, ret);
#endif

    return (ret == 0) ? HARDWARE_DRIVER_OK : HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
}

/**
 * @brief File system check.
 */
static int hw_mmc_fsck(struct mmc_device *device, const char *name, char *fs)
{
    if ((NULL == device) || (NULL == name) || (NULL == fs))
    {
        log_error("parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

#if CONFIG_OS_LINUX
    char cmd[128] = {0};
    snprintf(cmd, sizeof(cmd), "fsck.%s -av %s", fs, name);
    int ret = system(cmd);
#else
    (void)name;
    (void)fs;
    int ret = 0;
#endif

    return (ret == 0) ? HARDWARE_DRIVER_OK : HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
}

/**
 * @brief Close MMC device.
 */
static int hw_mmc_close(struct hw_device_t* device)
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
 * @brief Open MMC device.
 * @param module Hardware module.
 * @param name Device name.
 * @param device Output device handle.
 * @return 0 on success, negative error code on failure.
 */
int hw_mmc_open(const hw_module_t* module, const char* name, hw_device_t** device)
{
    *device = NULL;
    (void)name;

    if ((NULL == module) || (NULL == device))
    {
        log_error("parameter error");
        return HARDWARE_DRIVER_ERROR_INVALID_PARAM;
    }

    struct mmc_device *dev = calloc(1, sizeof(struct mmc_device));
    if (NULL == dev)
    {
        log_error("malloc for information struct of device failed");
        return HARDWARE_DRIVER_ERROR_MEMORY;
    }

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = MMC_DEVICE_API_VERSION_1_0;
    dev->common.module = (struct hw_module_t *) module;
    dev->common.close = hw_mmc_close;

    dev->mount = hw_mmc_mount;
    dev->unmount = hw_mmc_unmount;
    dev->format = hw_mmc_format;
    dev->fsck = hw_mmc_fsck;

    *device = &dev->common;

    return HARDWARE_DRIVER_OK;
}


static struct hw_module_methods_t hw_module_methods = {
    .open = hw_mmc_open,
};

static struct mmc_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = MMC_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = MMC_HARDWARE_MODULE_ID,
        .name = "MMC Module",
        .author = "Anyka",
        .methods = &hw_module_methods,
    },
};

/**
 * @brief Initialize MMC module.
 * @return 0 on success, negative error code on failure.
 */
int hw_mmc_module_init(void)
{
    int ret = hw_register_module(&(HAL_MODULE_INFO_SYM.common));
    return (ret == 0) ? HARDWARE_DRIVER_OK : HARDWARE_DRIVER_ERROR_OPERATION_FAILED;
}


