/*
 * eFuse HAL sample test - Simplified version
 */
#if CONFIG_HW_DRIVER_EFUSE

#include "efuse.h"
#include "hardware.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "soc_platform.h"
#include "log.h"
#define LOG_TAG "HAL EFUSE TEST"

static void print_help(void)
{
    printf("\n"
           "usage: <command> [args]\n"
           "\n"
           "commands:\n"
           "  help                                  show this help\n"
           "  get <name> <global|customer> <length|id>   get efuse info\n"
           "  set <name> customer <hex-string>      set customer id\n"
           "\n"
           "example:\n"
           "  get efuse0 global length\n"
           "  get efuse0 global id\n"
           "  set efuse0 customer 0011223344556677\n");
}

#if (defined OS_NAME_LINUX)
int main(int argc, char **argv)
#else
static int hw_efuse_command(int argc, char **argv)
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
    if (hw_get_module(EFUSE_HARDWARE_MODULE_ID, &module) != 0 || module == NULL) {
        log_error("get module failed");
        return -1;
    }

    /* Open device */
    const char *name = (argc > 2) ? argv[2] : "efuse0";
    ret = module->methods->open(module, name, &device);
    if (ret != 0 || device == NULL) {
        log_error("open %s failed", name);
        return -1;
    }
    log_info("open %s success", name);

    if (strcmp(argv[1], "set") == 0) {
        if (argc < 5 || strcmp(argv[3], "customer") != 0) {
            print_help();
            device->close(device);
            return -1;
        }
        ret = ((struct efuse_device *)device)->set_id((struct efuse_device *)device, argv[2], argv[3], argv[4]);
        if (ret == 0) {
            log_info("set customer id success");
        } else {
            log_error("set customer id failed");
        }
    }
    else if (strcmp(argv[1], "get") == 0) {
        if (argc < 5) {
            print_help();
            device->close(device);
            return -1;
        }

        const char *option = argv[3];
        if (strcmp(argv[4], "length") == 0) {
            uint32_t length = 0;
            ret = ((struct efuse_device *)device)->get_length((struct efuse_device *)device, argv[2], option, &length);
            if (ret == 0) {
                log_info("efuse %s %s length: %u", argv[2], option, length);
            } else {
                log_error("get length failed");
            }
        } else if (strcmp(argv[4], "id") == 0) {
            uint32_t length = 0;
            ret = ((struct efuse_device *)device)->get_length((struct efuse_device *)device, argv[2], option, &length);
            if (ret != 0 || length == 0 || length > 1024) {
                log_error("invalid length");
                device->close(device);
                return -1;
            }
            char *buffer = (char *)malloc(length + 1);
            if (!buffer) {
                log_error("malloc failed");
                device->close(device);
                return -1;
            }
            memset(buffer, 0, length + 1);
            ret = ((struct efuse_device *)device)->get_id((struct efuse_device *)device, argv[2], option, buffer);
            if (ret == 0) {
                log_info("efuse %s %s id: %s", argv[2], option, buffer);
            } else {
                log_error("get id failed");
            }
            free(buffer);
        } else {
            print_help();
            device->close(device);
            return -1;
        }
    } else {
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
SHELL_CMD_EXPORT_ALIAS(hw_efuse_command, hw_efuse_cmd, efuse program);
#endif

#endif
