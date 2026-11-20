/*
 * WDT HAL sample test - Simplified version
 */
#if CONFIG_HW_DRIVER_WDT

#include "wdt.h"
#include "hardware.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "soc_platform.h"
#include "log.h"
#define LOG_TAG "HAL WDT TEST"

static void print_help(void)
{
    printf("\n"
           "usage: <command> [args]\n"
           "\n"
           "commands:\n"
           "  help                      show this help\n"
           "  start <name> <ms>         start watchdog with timeout in ms\n"
           "  feed <name>               feed watchdog\n"
           "  stop <name>               stop watchdog\n"
           "\n"
           "example:\n"
           "  start wdt0 4000\n"
           "  feed wdt0\n"
           "  stop wdt0\n");
}

#if (defined OS_NAME_LINUX)
int main(int argc, char **argv)
#else
static int hw_wdt_command(int argc, char **argv)
#endif
{
    soc_platform_init(0);
    log_set_level(LOG_INFO);

    if (argc < 2 || strcmp(argv[1], "help") == 0) {
        print_help();
        return -1;
    }

    const struct hw_module_t *module = NULL;
    struct hw_device_t *device = NULL;
    int ret = -1;

    /* Initialize and get module */
    hw_module_init();
    if (hw_get_module(WDT_HARDWARE_MODULE_ID, &module) != 0 || module == NULL) {
        log_error("get module failed");
        return -1;
    }

    /* Get device name */
    const char *name = (argc > 2) ? argv[2] : "wdt0";

    /* Open device */
    ret = module->methods->open(module, name, &device);
    if (ret != 0 || device == NULL) {
        log_error("open %s failed", name);
        return -1;
    }
    log_info("open %s success", name);

    if (strcmp(argv[1], "start") == 0) {
        if (argc < 4) {
            log_error("usage: start <name> <ms>");
            device->close(device);
            return -1;
        }

        uint32_t ms = (uint32_t)atoi(argv[3]);
        struct timeval timeout = {0};
        timeout.tv_sec = ms / 1000;
        timeout.tv_usec = (ms % 1000) * 1000;

        ret = ((struct wdt_device *)device)->start((struct wdt_device *)device, name, &timeout);
        if (ret == 0) {
            log_info("started %s with timeout %u ms", name, ms);
        } else {
            log_error("start failed");
        }
    }
    else if (strcmp(argv[1], "feed") == 0) {
        ret = ((struct wdt_device *)device)->feed((struct wdt_device *)device, name);
        if (ret == 0) {
            log_info("feed %s success", name);
        } else {
            log_error("feed failed");
        }
    }
    else if (strcmp(argv[1], "stop") == 0) {
        ret = ((struct wdt_device *)device)->stop((struct wdt_device *)device, name);
        if (ret == 0) {
            log_info("stopped %s", name);
        } else {
            log_error("stop failed");
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
SHELL_CMD_EXPORT_ALIAS(hw_wdt_command, hw_wdt_cmd, wdt program);
#endif

#endif
