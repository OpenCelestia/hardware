/*
 * Ethernet HAL sample test
 * - Option parsing style aligned with audio/camera tests
 * - Uses log_info/log_error and SDK init flow
 */
#if CONFIG_HW_ETHERNET

#include "ethernet.h"
#include "hardware.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "soc_platform.h"
#include "log.h"

#define LOG_TAG "HAL ETHERNET TEST"

typedef struct {
    const char *action;
    const char *iface;
} eth_opts_t;

static void print_help(const char *prog)
{
    printf("\n");
    printf("usage: %s --action <info|status|up|down> [--iface eth0]\n", prog);
    printf("\n");
    printf("options:\n");
    printf("  --action  info (default) | status | up | down\n");
    printf("  --iface   interface name (default: eth0)\n");
    printf("  --help    show this help\n");
    printf("\n");
    printf("example:\n");
    printf("  %s --action info --iface eth0\n", prog);
}

static int parse_options(int argc, char **argv, eth_opts_t *opts)
{
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--action") == 0 && (i + 1) < argc) {
            opts->action = argv[++i];
        } else if (strcmp(argv[i], "--iface") == 0 && (i + 1) < argc) {
            opts->iface = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            return -1;
        } else {
            log_error("unknown option: %s", argv[i]);
            return -1;
        }
    }
    return 0;
}

static int ethernet_run(const eth_opts_t *opts)
{
    const struct hw_module_t *module = NULL;
    ethernet_hal_device_t *device = NULL;

    if (hw_get_module(ETHERNET_HARDWARE_MODULE_ID, &module) != 0 || module == NULL) {
        log_error("get ethernet module failed");
        return -1;
    }

    if (ethernet_hal_device_open(module, &device) != 0 || device == NULL) {
        log_error("open ethernet device failed");
        return -1;
    }

    if (strcmp(opts->action, "info") == 0) {
        char buf[64] = {0};
        if (device->get_version) {
            log_info("version: %s", device->get_version(device));
        }
        if (device->get_ip && device->get_ip(device, opts->iface, buf, sizeof(buf)) == 0) {
            log_info("ip: %s", buf);
        }
        if (device->get_mac && device->get_mac(device, opts->iface, buf, sizeof(buf)) == 0) {
            log_info("mac: %s", buf);
        }
        if (device->get_netmask && device->get_netmask(device, opts->iface, buf, sizeof(buf)) == 0) {
            log_info("netmask: %s", buf);
        }
        if (device->get_gateway && device->get_gateway(device, opts->iface, buf, sizeof(buf)) == 0) {
            log_info("gateway: %s", buf);
        }
    } else if (strcmp(opts->action, "status") == 0) {
        ethernet_status_t st = device->get_status(device, opts->iface);
        log_info("status: %d", st);
    } else if (strcmp(opts->action, "up") == 0) {
        if (device->set_up(device, opts->iface) != 0) {
            log_error("set up failed");
        } else {
            log_info("set up ok");
        }
    } else if (strcmp(opts->action, "down") == 0) {
        if (device->set_down(device, opts->iface) != 0) {
            log_error("set down failed");
        } else {
            log_info("set down ok");
        }
    } else {
        log_error("unsupported action: %s", opts->action);
    }

    ethernet_hal_device_close(device);
    return 0;
}

#if (defined OS_NAME_LINUX)
int main(int argc, char **argv)
#else
static int hw_ethernet_cmd(int argc, char **argv)
#endif
{
    eth_opts_t opts = {
        .action = "info",
        .iface = "eth0",
    };

    soc_platform_init(0);
    log_set_level(LOG_INFO);
    hw_module_init();

    if (parse_options(argc, argv, &opts) != 0) {
        print_help(argv[0]);
        return -1;
    }

    return ethernet_run(&opts);
}

#if ((defined OS_NAME_AOS) || (defined OS_NAME_RTT))
SHELL_CMD_EXPORT_ALIAS(hw_ethernet_cmd, hw_ethernet_cmd, ethernet program);
#endif

#endif
