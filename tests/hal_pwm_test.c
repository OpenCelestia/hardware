/*
 * PWM HAL sample test - Simplified version
 */
#if CONFIG_HW_DRIVER_PWM

#include "pwm.h"
#include "hardware.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "soc_platform.h"
#include "log.h"
#define LOG_TAG "HAL PWM TEST"

static void print_help(void)
{
    printf("\n"
           "usage: <command> [args]\n"
           "\n"
           "commands:\n"
           "  help                              show this help\n"
           "  set <channel> <period> <duty>     config + enable pwm (period/duty in ns)\n"
           "  stop <channel>                    disable pwm\n"
           "\n"
           "channel: PWM channel number (0, 1, 2, ...)\n"
           "\n"
           "example:\n"
           "  set 0 400000 100000    (channel 0, period=400us, duty=100us)\n"
           "  stop 0\n");
}

#if (defined OS_NAME_LINUX)
int main(int argc, char **argv)
#else
static int hw_pwm_command(int argc, char **argv)
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
    if (hw_get_module(PWM_HARDWARE_MODULE_ID, &module) != 0 || module == NULL) {
        log_error("get module failed");
        return -1;
    }

    /* Get channel number (as string for HAL API) */
    const char *channel = (argc > 2) ? argv[2] : "0";

    /* Open device (channel number not used for open, but for config/enable) */
    ret = module->methods->open(module, channel, &device);
    if (ret != 0 || device == NULL) {
        log_error("open pwm module failed");
        return -1;
    }
    log_info("open pwm module success");

    if (strcmp(argv[1], "set") == 0) {
        if (argc < 5) {
            log_error("usage: set <channel> <period> <duty>");
            device->close(device);
            return -1;
        }

        struct pwm_info info = {0};
        info.period = (uint32_t)atoi(argv[3]);
        info.duty = (uint32_t)atoi(argv[4]);

        ret = ((struct pwm_device *)device)->config((struct pwm_device *)device, channel, &info);
        if (ret == 0) {
            log_info("config pwm%s: period=%u ns, duty=%u ns", channel, info.period, info.duty);
            ret = ((struct pwm_device *)device)->enable((struct pwm_device *)device, channel, 1);
            if (ret == 0) {
                log_info("enabled pwm%s", channel);
            } else {
                log_error("enable pwm%s failed", channel);
            }
        } else {
            log_error("config pwm%s failed", channel);
        }
    }
    else if (strcmp(argv[1], "stop") == 0) {
        ret = ((struct pwm_device *)device)->enable((struct pwm_device *)device, channel, 0);
        if (ret == 0) {
            log_info("disabled pwm%s", channel);
        } else {
            log_error("disable pwm%s failed", channel);
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
SHELL_CMD_EXPORT_ALIAS(hw_pwm_command, hw_pwm_cmd, pwm program);
#endif

#endif
