/*
 * GPU HAL sample test
 * - Option parsing style aligned with audio/camera tests
 * - Uses log_info/log_error and SDK init flow
 */
#if CONFIG_HW_GPU

#include "gpu.h"
#include "hardware.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "soc_platform.h"
#include "log.h"

#define LOG_TAG "HAL GPU TEST"

typedef struct {
    const char *action;
    const char *device;
} gpu_opts_t;

static void print_help(const char *prog)
{
    printf("\n");
    printf("usage: %s --action <info> [--device gpu]\n", prog);
    printf("\n");
    printf("options:\n");
    printf("  --action    info (default)\n");
    printf("  --device    gpu device name (default: gpu)\n");
    printf("  --help      show this help\n");
    printf("\n");
    printf("example:\n");
    printf("  %s --action info --device gpu\n", prog);
}

static int parse_options(int argc, char **argv, gpu_opts_t *opts)
{
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--action") == 0 && (i + 1) < argc) {
            opts->action = argv[++i];
        } else if (strcmp(argv[i], "--device") == 0 && (i + 1) < argc) {
            opts->device = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            return -1;
        } else {
            log_error("unknown option: %s", argv[i]);
            return -1;
        }
    }
    return 0;
}

static int gpu_info(const char *device_name)
{
    const struct hw_module_t *module = NULL;
    struct gpu_device *device = NULL;

    if (hw_get_module(GPU_HARDWARE_MODULE_ID, &module) != 0 || module == NULL) {
        log_error("get gpu module failed");
        return -1;
    }

    if (module->methods->open(module, device_name, TO_HW_DEVICE_T_OPEN(&device)) != 0 || device == NULL) {
        log_error("open gpu device failed: %s", device_name);
        return -1;
    }

    if (device->supported_min_width && device->supported_min_height) {
        log_info("min size: %dx%d", device->supported_min_width(), device->supported_min_height());
    }

    if (device->check_input_format && device->check_output_format) {
        log_info("input NV12: %s", device->check_input_format(SYSTEM_PIXEL_FORMAT_NV12) ? "yes" : "no");
        log_info("output ARGB32: %s", device->check_output_format(SYSTEM_PIXEL_FORMAT_ARGB32) ? "yes" : "no");
    }

    device->common.close(&device->common);
    return 0;
}

#if (defined OS_NAME_LINUX)
int main(int argc, char **argv)
#else
static int hw_gpu_cmd(int argc, char **argv)
#endif
{
    gpu_opts_t opts = {
        .action = "info",
        .device = GPU_DEVICE_NAME,
    };

    soc_platform_init(0);
    log_set_level(LOG_INFO);
    hw_module_init();

    if (parse_options(argc, argv, &opts) != 0) {
        print_help(argv[0]);
        return -1;
    }

    if (strcmp(opts.action, "info") == 0) {
        return gpu_info(opts.device);
    }

    log_error("unsupported action: %s", opts.action);
    print_help(argv[0]);
    return -1;
}

#if ((defined OS_NAME_AOS) || (defined OS_NAME_RTT))
SHELL_CMD_EXPORT_ALIAS(hw_gpu_cmd, hw_gpu_cmd, gpu program);
#endif

#endif
