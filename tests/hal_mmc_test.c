/*
 * MMC HAL sample test - Simplified version
 */
#if CONFIG_HW_MMC

#include "mmc.h"
#include "hardware.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include "soc_platform.h"
#include "log.h"
#define LOG_TAG "HAL MMC TEST"

static void print_help(void)
{
    printf("\n"
           "usage: <command> [args]\n"
           "\n"
           "commands:\n"
           "  help                              show this help\n"
           "  info                              list mmc block devices\n"
           "  mount <device> <path> [fs]        mount device\n"
           "  unmount <path>                    unmount device\n"
           "  format <device> [fs]              format device\n"
           "  fsck <device> [fs]                check filesystem\n"
           "\n"
           "example:\n"
           "  mount /dev/mmcblk0p1 /tmp/sd_hal_test vfat   # KM01W: prefer p1, mount dir created if missing\n"
           "  mount /dev/mmcblk0 /mnt/sdcard vfat\n"
           "  unmount /mnt/sdcard\n"
           "  format /dev/mmcblk0 vfat\n"
           "  fsck /dev/mmcblk0 vfat\n"
           "  info                              probe mmcblk nodes\n");
}

static int mmc_list_devices(void)
{
    static const char *candidates[] = {
        MMC_DEVICE_NAME_BLK0P1,
        MMC_DEVICE_NAME_BLK0,
        MMC_DEVICE_NAME_BLK1P1,
        MMC_DEVICE_NAME_BLK1,
        NULL
    };
    int found = 0;

    log_info("MMC block device probe:");
    for (int i = 0; candidates[i] != NULL; ++i) {
        if (access(candidates[i], F_OK) == 0) {
            log_info("  [present] %s", candidates[i]);
            found++;
        } else {
            log_info("  [ absent] %s", candidates[i]);
        }
    }
    if (found == 0) {
        log_error("no mmcblk device found (insert TF card / load ak_mci.ko)");
        return -1;
    }
    return 0;
}

#if (defined OS_NAME_LINUX)
int main(int argc, char **argv)
#else
static int hw_mmc_command(int argc, char **argv)
#endif
{
    soc_platform_init(0);
    log_set_level(LOG_INFO);

    if (argc < 2 || strcmp(argv[1], "help") == 0) {
        print_help();
        return -1;
    }

    if (strcmp(argv[1], "info") == 0) {
        soc_platform_init(0);
        log_set_level(LOG_INFO);
        hw_module_init();
        return mmc_list_devices();
    }

    const struct hw_module_t *module = NULL;
    struct hw_device_t *device = NULL;
    int ret = -1;

    /* Initialize and get module */
    hw_module_init();
    if (hw_get_module(MMC_HARDWARE_MODULE_ID, &module) != 0 || module == NULL) {
        log_error("get module failed");
        return -1;
    }

    /* Get device name */
    const char *name = (argc > 2) ? argv[2] : MMC_DEVICE_NAME_BLK0;

    /* Open device */
    ret = module->methods->open(module, name, &device);
    if (ret != 0 || device == NULL) {
        log_error("open %s failed", name);
        return -1;
    }
    log_info("open %s success", name);

    if (strcmp(argv[1], "mount") == 0) {
        if (argc < 4) {
            log_error("usage: mount <device> <path> [fs]");
            device->close(device);
            return -1;
        }
        char *path = argv[3];
        char *fs = (argc > 4) ? argv[4] : MMC_DEVICE_MOUNT_FS;
        ret = ((struct mmc_device *)device)->mount((struct mmc_device *)device, name, path, fs);
        if (ret == 0) {
            log_info("mounted %s to %s (%s)", name, path, fs);
        } else {
            log_error("mount failed");
        }
    }
    else if (strcmp(argv[1], "unmount") == 0) {
        if (argc < 3) {
            log_error("usage: unmount <path>");
            device->close(device);
            return -1;
        }
        ret = ((struct mmc_device *)device)->unmount((struct mmc_device *)device, argv[2]);
        if (ret == 0) {
            log_info("unmounted %s", argv[2]);
        } else {
            log_error("unmount failed");
        }
    }
    else if (strcmp(argv[1], "format") == 0) {
        char *fs = (argc > 3) ? argv[3] : MMC_DEVICE_MOUNT_FS;
        ret = ((struct mmc_device *)device)->format((struct mmc_device *)device, name, fs);
        if (ret == 0) {
            log_info("formatted %s (%s)", name, fs);
        } else {
            log_error("format failed");
        }
    }
    else if (strcmp(argv[1], "fsck") == 0) {
        char *fs = (argc > 3) ? argv[3] : MMC_DEVICE_MOUNT_FS;
        ret = ((struct mmc_device *)device)->fsck((struct mmc_device *)device, name, fs);
        if (ret == 0) {
            log_info("fsck %s (%s) ok", name, fs);
        } else {
            log_error("fsck failed");
        }
    }
    else {
        print_help();
        device->close(device);
        return -1;
    }

    /* Close device */
    device->close(device);
    log_info("closed");

    return ret;
}

#if ((defined OS_NAME_AOS) || (defined OS_NAME_RTT))
SHELL_CMD_EXPORT_ALIAS(hw_mmc_command, hw_mmc_cmd, mmc program);
#endif

#endif
