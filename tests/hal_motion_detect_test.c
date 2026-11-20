/*
 * Motion detect HAL sample test
 * - Option parsing style aligned with audio/camera tests
 * - Uses log_info/log_error and SDK init flow
 */
#if CONFIG_HW_VISION_MOTION_DETECT

#include "motion_detect.h"
#include "hardware.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "soc_platform.h"
#include "log.h"

#define LOG_TAG "HAL MOTION DETECT TEST"

typedef struct {
    const char *action;
    int enable;
    int sensitivity;
    int rect_x;
    int rect_y;
    int rect_w;
    int rect_h;
} md_opts_t;

static void print_help(const char *prog)
{
    printf("\n");
    printf("usage: %s --action <config> [options]\n", prog);
    printf("\n");
    printf("options:\n");
    printf("  --action       config (default)\n");
    printf("  --enable       0|1 (default: 1)\n");
    printf("  --sensitivity  0~100 (default: 50)\n");
    printf("  --rect         x y w h (default: 0 0 100 100)\n");
    printf("  --help         show this help\n");
    printf("\n");
    printf("example:\n");
    printf("  %s --action config --enable 1 --sensitivity 60 --rect 0 0 1280 720\n", prog);
}

static int parse_options(int argc, char **argv, md_opts_t *opts)
{
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--action") == 0 && (i + 1) < argc) {
            opts->action = argv[++i];
        } else if (strcmp(argv[i], "--enable") == 0 && (i + 1) < argc) {
            opts->enable = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--sensitivity") == 0 && (i + 1) < argc) {
            opts->sensitivity = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--rect") == 0 && (i + 4) < argc) {
            opts->rect_x = atoi(argv[++i]);
            opts->rect_y = atoi(argv[++i]);
            opts->rect_w = atoi(argv[++i]);
            opts->rect_h = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            return -1;
        } else {
            log_error("unknown option: %s", argv[i]);
            return -1;
        }
    }
    return 0;
}

static void on_motion_detect(int num_results, const motion_detect_result_t *results, void *user_data)
{
    (void)user_data;
    log_info("motion_detect results: %d", num_results);
    for (int i = 0; i < num_results; ++i) {
        log_info("rect[%d]: x=%d y=%d w=%d h=%d ratio=%.3f",
                 i,
                 results[i].rect.x,
                 results[i].rect.y,
                 results[i].rect.width,
                 results[i].rect.height,
                 results[i].area_ratio);
    }
}

static int on_day_night_status(void)
{
    return 0; /* day */
}

static int md_run(const md_opts_t *opts)
{
    const struct hw_module_t *module = NULL;
    motion_detect_device_t *device = NULL;

    if (hw_get_module(MOTION_DETECT_HARDWARE_MODULE_ID, &module) != 0 || module == NULL) {
        log_error("get motion_detect module failed");
        return -1;
    }

    if (((const motion_detect_module_t *)module)->open((const motion_detect_module_t *)module, &device) != 0 || device == NULL) {
        log_error("open motion_detect device failed");
        return -1;
    }

    if (device->init(device) != 0) {
        log_error("motion_detect init failed");
        device->common.close(&device->common);
        return -1;
    }

    if (strcmp(opts->action, "config") == 0) {
        motion_detect_config_t config = {0};
        config.enable = opts->enable;
        config.sensitivity = opts->sensitivity;
        config.valid_area.x = opts->rect_x;
        config.valid_area.y = opts->rect_y;
        config.valid_area.width = opts->rect_w;
        config.valid_area.height = opts->rect_h;

        device->set_day_night_callback(device, on_day_night_status);
        device->set_callback(device, on_motion_detect, NULL);

        if (device->set_config(device, &config) != 0) {
            log_error("motion_detect set_config failed");
        } else {
            log_info("motion_detect config applied");
        }

        if (device->get_config(device, &config) == 0) {
            log_info("config: enable=%d sensitivity=%d rect=%d,%d,%d,%d",
                     config.enable,
                     config.sensitivity,
                     config.valid_area.x,
                     config.valid_area.y,
                     config.valid_area.width,
                     config.valid_area.height);
        }
    } else {
        log_error("unsupported action: %s", opts->action);
    }

    device->deinit(device);
    device->common.close(&device->common);
    return 0;
}

#if (defined OS_NAME_LINUX)
int main(int argc, char **argv)
#else
static int hw_motion_detect_cmd(int argc, char **argv)
#endif
{
    md_opts_t opts = {
        .action = "config",
        .enable = 1,
        .sensitivity = 50,
        .rect_x = 0,
        .rect_y = 0,
        .rect_w = 100,
        .rect_h = 100,
    };

    soc_platform_init(0);
    log_set_level(LOG_INFO);
    hw_module_init();

    if (parse_options(argc, argv, &opts) != 0) {
        print_help(argv[0]);
        return -1;
    }

    return md_run(&opts);
}

#if ((defined OS_NAME_AOS) || (defined OS_NAME_RTT))
SHELL_CMD_EXPORT_ALIAS(hw_motion_detect_cmd, hw_motion_detect_cmd, motion detect program);
#endif

#endif
