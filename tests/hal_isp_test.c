/*
 * ISP HAL sample test
 * - Option parsing style aligned with audio/camera tests
 * - Uses log_info/log_error and SDK init flow
 */
#if CONFIG_HW_VISION_ISP

#include "isp.h"
#include "hardware.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "soc_platform.h"
#include "log.h"

#define LOG_TAG "HAL ISP TEST"

typedef struct {
    const char *action;
    const char *mode;
} isp_opts_t;

static void print_help(const char *prog)
{
    printf("\n");
    printf("usage: %s --action <init|mode|deinit> [--mode day|night]\n", prog);
    printf("\n");
    printf("options:\n");
    printf("  --action  init (default) | mode | deinit\n");
    printf("  --mode    day | night (used with --action mode)\n");
    printf("  --help    show this help\n");
    printf("\n");
    printf("example:\n");
    printf("  %s --action mode --mode day\n", prog);
}

static int parse_options(int argc, char **argv, isp_opts_t *opts)
{
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--action") == 0 && (i + 1) < argc) {
            opts->action = argv[++i];
        } else if (strcmp(argv[i], "--mode") == 0 && (i + 1) < argc) {
            opts->mode = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            return -1;
        } else {
            log_error("unknown option: %s", argv[i]);
            return -1;
        }
    }
    return 0;
}

static int isp_run(const isp_opts_t *opts)
{
    const struct hw_module_t *module = NULL;
    isp_device_t *device = NULL;

    if (hw_get_module(ISP_HARDWARE_MODULE_ID, &module) != 0 || module == NULL) {
        log_error("get isp module failed");
        return -1;
    }

    if (((const isp_module_t *)module)->open((const isp_module_t *)module, &device) != 0 || device == NULL) {
        log_error("open isp device failed");
        return -1;
    }

    if (strcmp(opts->action, "init") == 0) {
        int ret = device->init(device, NULL);
        if (ret != 0) {
            log_error("isp init failed");
        } else {
            log_info("isp init success");
        }
    } else if (strcmp(opts->action, "mode") == 0) {
        isp_mode_t mode = ISP_MODE_DAY;
        if (opts->mode && strcmp(opts->mode, "night") == 0) {
            mode = ISP_MODE_NIGHT;
        }
        int ret = device->switch_mode(device, mode);
        if (ret != 0) {
            log_error("isp set mode failed");
        } else {
            log_info("isp set mode success: %s", mode == ISP_MODE_DAY ? "day" : "night");
        }
    } else if (strcmp(opts->action, "deinit") == 0) {
        int ret = device->deinit(device);
        if (ret != 0) {
            log_error("isp deinit failed");
        } else {
            log_info("isp deinit success");
        }
    } else {
        log_error("unsupported action: %s", opts->action);
    }

    device->common.close(&device->common);
    return 0;
}

#if (defined OS_NAME_LINUX)
int main(int argc, char **argv)
#else
static int hw_isp_cmd(int argc, char **argv)
#endif
{
    isp_opts_t opts = {
        .action = "init",
        .mode = "day",
    };

    soc_platform_init(0);
    log_set_level(LOG_INFO);
    hw_module_init();

    if (parse_options(argc, argv, &opts) != 0) {
        print_help(argv[0]);
        return -1;
    }

    return isp_run(&opts);
}

#if ((defined OS_NAME_AOS) || (defined OS_NAME_RTT))
SHELL_CMD_EXPORT_ALIAS(hw_isp_cmd, hw_isp_cmd, isp program);
#endif

#endif
