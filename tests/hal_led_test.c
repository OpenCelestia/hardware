/*
 * LED HAL sample test - Simplified version
 */
#include "led.h"
#include "hardware.h"

#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>

#include "soc_platform.h"
#include "log.h"
#define LOG_TAG "HAL LED TEST"

static void print_help(void)
{
    printf("\n"
           "usage: <command> [args]\n"
           "\n"
           "commands:\n"
           "  help                          show this help\n"
           "  list                          list available leds\n"
           "  on <name>                     turn on led\n"
           "  off <name>                    turn off led\n"
           "  get <name>                    get led state\n"
           "  brightness <name> <val>       set brightness (0-255)\n"
           "\n"
           "example:\n"
           "  list\n"
           "  on red_led\n"
           "  brightness red_led 128\n");
}

static int list_leds(void)
{
    DIR *dir = opendir("/sys/class/leds");
    if (!dir) {
        log_error("cannot open /sys/class/leds");
        return -1;
    }

    struct dirent *entry;
    int count = 0;

    log_info("Available LEDs:");
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        log_info("  %d: %s", count + 1, entry->d_name);
        count++;
    }
    closedir(dir);

    if (count == 0) {
        log_info("  (none)");
    }
    log_info("Total: %d LED(s)", count);
    return 0;
}

#ifdef CONFIG_OS_RTOS
static int hw_led_command(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
    soc_platform_init(0);
    log_set_level(LOG_INFO);

    if (argc < 2 || strcmp(argv[1], "help") == 0) {
        print_help();
        return (argc < 2) ? -1 : 0;
    }

    if (strcmp(argv[1], "list") == 0) {
        return list_leds();
    }

    const struct hw_module_t *module = NULL;
    struct hw_device_t *device = NULL;
    int ret = -1;

    /* Initialize and get module */
    hw_module_init();
    if (hw_get_module(LED_HARDWARE_MODULE_ID, &module) != 0 || module == NULL) {
        log_error("get module failed");
        return -1;
    }

    /* Get device name */
    const char *name = (argc > 2) ? argv[2] : "led0";

    /* Open device */
    ret = module->methods->open(module, name, &device);
    if (ret != 0 || device == NULL) {
        log_error("open %s failed", name);
        return -1;
    }
    log_info("open %s success", name);

    if (strcmp(argv[1], "on") == 0) {
        ret = ((struct led_device *)device)->set_state((struct led_device *)device, name, 1);
        if (ret == 0) {
            log_info("led %s turned on", name);
        } else {
            log_error("set state failed");
        }
    }
    else if (strcmp(argv[1], "off") == 0) {
        ret = ((struct led_device *)device)->set_state((struct led_device *)device, name, 0);
        if (ret == 0) {
            log_info("led %s turned off", name);
        } else {
            log_error("set state failed");
        }
    }
    else if (strcmp(argv[1], "get") == 0) {
        uint32_t value = 0;
        ret = ((struct led_device *)device)->get_state((struct led_device *)device, name, &value);
        if (ret == 0) {
            log_info("led %s state: %s", name, value ? "on" : "off");
        } else {
            log_error("get state failed");
        }
    }
    else if (strcmp(argv[1], "brightness") == 0) {
        if (argc < 4) {
            log_error("usage: brightness <name> <val>");
            device->close(device);
            return -1;
        }
        uint32_t value = (uint32_t)atoi(argv[3]);
        ret = ((struct led_device *)device)->set_brightness((struct led_device *)device, name, value);
        if (ret == 0) {
            log_info("led %s brightness set to %u", name, value);
        } else {
            log_error("set brightness failed");
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

#if CONFIG_OS_RTOS
SHELL_CMD_EXPORT_ALIAS(hw_led_command, hw_led_cmd, led driver program);
#endif
