/*
 * IR HAL sample test
 * - Option parsing style aligned with audio/camera tests
 * - Uses log_info/log_error and SDK init flow
 */
#if CONFIG_HW_DRIVER_IR

#include "ir.h"
#include "hardware.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "soc_platform.h"
#include "log.h"

#define LOG_TAG "HAL IR TEST"

typedef struct {
    const char *action;
    const char *device;
    const char *cfg;
    int irled_level;
    int ircut_status;
    int irled_status;
} ir_opts_t;

static void print_help(const char *prog)
{
    printf("\n");
    printf("usage: %s --action <status|ircut|irled> [options]\n", prog);
    printf("\n");
    printf("options:\n");
    printf("  --action     status (default) | ircut | irled\n");
    printf("  --device     device name (default: ir0)\n");
    printf("  --cfg        ircut config path (optional)\n");
    printf("  --ircut      0(day) | 1(night) (used with --action ircut)\n");
    printf("  --irled      0(off) | 1(on)   (used with --action irled)\n");
    printf("  --level      irled working level 0/1 (default: 1)\n");
    printf("  --help       show this help\n");
    printf("\n");
    printf("example:\n");
    printf("  %s --action status\n", prog);
    printf("  %s --action ircut --ircut 1 --cfg /etc/ircut.cfg\n", prog);
    printf("  %s --action irled --irled 1 --level 1\n", prog);
}

static int parse_options(int argc, char **argv, ir_opts_t *opts)
{
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--action") == 0 && (i + 1) < argc) {
            opts->action = argv[++i];
        } else if (strcmp(argv[i], "--device") == 0 && (i + 1) < argc) {
            opts->device = argv[++i];
        } else if (strcmp(argv[i], "--cfg") == 0 && (i + 1) < argc) {
            opts->cfg = argv[++i];
        } else if (strcmp(argv[i], "--ircut") == 0 && (i + 1) < argc) {
            opts->ircut_status = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--irled") == 0 && (i + 1) < argc) {
            opts->irled_status = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--level") == 0 && (i + 1) < argc) {
            opts->irled_level = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            return -1;
        } else {
            log_error("unknown option: %s", argv[i]);
            return -1;
        }
    }
    return 0;
}

static int ir_run(const ir_opts_t *opts)
{
    const struct hw_module_t *module = NULL;
    struct ir_device *device = NULL;

    if (hw_get_module(IR_HARDWARE_MODULE_ID, &module) != 0 || module == NULL) {
        log_error("get ir module failed");
        return -1;
    }

    if (module->methods->open(module, opts->device, TO_HW_DEVICE_T_OPEN(&device)) != 0 || device == NULL) {
        log_error("open ir device failed: %s", opts->device);
        return -1;
    }

    if (strcmp(opts->action, "status") == 0) {
        int status = 0;
        if (device->irled_init) {
            irled_param_t param = { .working_level = opts->irled_level };
            device->irled_init(device, &param);
        }
        if (device->irled_get_status && device->irled_get_status(device, &status) == 0) {
            log_info("irled status: %d", status);
        }
        if (device->ircut_get_time && device->ircut_get_time(device, &status) == 0) {
            log_info("ircut switch time: %d ms", status);
        }
    } else if (strcmp(opts->action, "ircut") == 0) {
        if (device->ircut_init) {
            device->ircut_init(device, opts->cfg);
        }
        if (device->ircut_set(device, opts->ircut_status) != 0) {
            log_error("ircut set failed");
        } else {
            log_info("ircut set ok: %d", opts->ircut_status);
        }
    } else if (strcmp(opts->action, "irled") == 0) {
        if (device->irled_init) {
            irled_param_t param = { .working_level = opts->irled_level };
            device->irled_init(device, &param);
        }
        if (device->irled_set_status(device, opts->irled_status) != 0) {
            log_error("irled set failed");
        } else {
            log_info("irled set ok: %d", opts->irled_status);
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
static int hw_ir_cmd(int argc, char **argv)
#endif
{
    ir_opts_t opts = {
        .action = "status",
        .device = "ir0",
        .cfg = NULL,
        .irled_level = 1,
        .ircut_status = 0,
        .irled_status = 0,
    };

    soc_platform_init(0);
    log_set_level(LOG_INFO);
    hw_module_init();

    if (parse_options(argc, argv, &opts) != 0) {
        print_help(argv[0]);
        return -1;
    }

    return ir_run(&opts);
}

#if ((defined OS_NAME_AOS) || (defined OS_NAME_RTT))
SHELL_CMD_EXPORT_ALIAS(hw_ir_cmd, hw_ir_cmd, ir program);
#endif

#endif
