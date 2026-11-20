/*
 * GPIO HAL sample test
 */
#if CONFIG_HW_DRIVER_GPIO

#include "gpio.h"
#include "hardware.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "soc_platform.h"
#include "log.h"
#define LOG_TAG "HAL GPIO TEST"

static void print_help(void)
{
    printf("\n"
           "usage: <command> [args]\n"
           "\n"
           "commands:\n"
           "  help                show this help\n"
           "  in <gpio>           read input level\n"
           "  out <gpio> <0|1>    set output level\n"
           "\n"
           "example:\n"
           "  gpio in 10          read GPIO 10 input level\n"
           "  gpio out 10 1       set GPIO 10 output high\n"
           "  gpio out 10 0       set GPIO 10 output low\n");
}

#if CONFIG_OS_RTOS
static int hw_gpio_command(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
    /* SDK and log initialization */
    soc_platform_init(0);
    log_set_level(LOG_INFO);

    if (argc < 2 || strcmp(argv[1], "help") == 0) {
        print_help();
        return -1;
    }

    /* Initialize module */
    hw_module_init();

    const struct hw_module_t *module = NULL;
    hw_get_module(GPIO_HARDWARE_MODULE_ID, &module);
    if (module == NULL) {
        log_error("get module failed");
        return -1;
    }

    int ret = -1;

    if (strcmp(argv[1], "in") == 0) {
        if (argc < 3) {
            log_error("usage: in <gpio>");
            return -1;
        }

        const char *gpio = argv[2];
        struct hw_device_t *device = NULL;

        /* Open device */
        ret = module->methods->open(module, gpio, &device);
        if (ret != 0) {
            log_error("open gpio %s failed", gpio);
            return ret;
        }

        struct gpio_device *gpio_dev = (struct gpio_device *)device;

        /* Set direction to input */
        ret = gpio_dev->set_direction(gpio_dev, gpio, 0);
        if (ret != 0) {
            log_error("set direction failed");
            device->close(device);
            return ret;
        }

        /* Read input level */
        uint32_t level = 0;
        ret = gpio_dev->get_input_level(gpio_dev, gpio, &level);
        if (ret == 0) {
            log_info("gpio %s level: %u", gpio, level);
        } else {
            log_error("read gpio %s failed", gpio);
        }

        /* Close device */
        device->close(device);
    }
    else if (strcmp(argv[1], "out") == 0) {
        if (argc < 4) {
            log_error("usage: out <gpio> <0|1>");
            return -1;
        }

        const char *gpio = argv[2];
        uint32_t value = (uint32_t)atoi(argv[3]);
        struct hw_device_t *device = NULL;

        /* Open device */
        ret = module->methods->open(module, gpio, &device);
        if (ret != 0) {
            log_error("open gpio %s failed", gpio);
            return ret;
        }

        struct gpio_device *gpio_dev = (struct gpio_device *)device;

        /* Set direction to output */
        ret = gpio_dev->set_direction(gpio_dev, gpio, 1);
        if (ret != 0) {
            log_error("set direction failed");
            device->close(device);
            return ret;
        }

        /* Set output level */
        ret = gpio_dev->set_output_level(gpio_dev, gpio, value);
        if (ret == 0) {
            log_info("gpio %s set to %u", gpio, value);
        } else {
            log_error("set gpio %s failed", gpio);
        }

        /* Close device */
        device->close(device);
    }
    else {
        print_help();
        return -1;
    }

    return ret;
}

#if CONFIG_OS_RTOS
SHELL_CMD_EXPORT_ALIAS(hw_gpio_command, hw_gpio_cmd, gpio driver program);
#endif

#endif
