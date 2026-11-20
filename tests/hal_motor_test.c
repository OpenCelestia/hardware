/*
 * Motor HAL sample test - Simplified version
 */
#if CONFIG_HW_DRIVER_MOTOR

#include "motor.h"
#include "hardware.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "soc_platform.h"
#include "log.h"
#define LOG_TAG "HAL MOTOR TEST"

static void print_help(void)
{
    printf("\n"
           "usage: <command> [args]\n"
           "\n"
           "commands:\n"
           "  help                                                              show this help\n"
           "  config <name> <speed> <steps_circle> <total> <boundary> [pos]     configure motor\n"
           "  step <name> <value>                                               set step\n"
           "  angle <name> <value>                                              set angle\n"
           "  stop <name>                                                       stop motor\n"
           "  info <name>                                                       get motor info\n"
           "\n"
           "example:\n"
           "  config motor0 200 2000 4000 0 0\n"
           "  step motor0 100\n"
           "  angle motor0 90\n"
           "  info motor0\n"
           "  stop motor0\n");
}

#if (defined OS_NAME_LINUX)
int main(int argc, char **argv)
#else
static int hw_motor_command(int argc, char **argv)
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
    if (hw_get_module(MOTOR_HARDWARE_MODULE_ID, &module) != 0 || module == NULL) {
        log_error("get module failed");
        return -1;
    }

    /* Get device name */
    const char *name = (argc > 2) ? argv[2] : MOTOR_DEVICE_NAME_MOTOR0;

    /* Open device */
    ret = module->methods->open(module, name, &device);
    if (ret != 0 || device == NULL) {
        log_error("open %s failed", name);
        return -1;
    }
    log_info("open %s success", name);

    if (strcmp(argv[1], "config") == 0) {
        if (argc < 7) {
            log_error("usage: config <name> <speed> <steps_circle> <total> <boundary> [pos]");
            device->close(device);
            return -1;
        }
        struct motor_config config = {0};
        config.speed_step = atoi(argv[3]);
        config.steps_one_circle = atoi(argv[4]);
        config.total_steps = atoi(argv[5]);
        config.boundary_steps = atoi(argv[6]);
        config.pos = (argc > 7) ? atoi(argv[7]) : 0;
        ret = ((struct motor_device *)device)->config((struct motor_device *)device, name, &config);
        if (ret == 0) {
            log_info("config %s success", name);
        } else {
            log_error("config failed");
        }
    }
    else if (strcmp(argv[1], "step") == 0) {
        if (argc < 4) {
            log_error("usage: step <name> <value>");
            device->close(device);
            return -1;
        }
        ret = ((struct motor_device *)device)->set_step((struct motor_device *)device, name, atoi(argv[3]));
        if (ret == 0) {
            log_info("set step %s to %s success", name, argv[3]);
        } else {
            log_error("set step failed");
        }
    }
    else if (strcmp(argv[1], "angle") == 0) {
        if (argc < 4) {
            log_error("usage: angle <name> <value>");
            device->close(device);
            return -1;
        }
        ret = ((struct motor_device *)device)->set_angle((struct motor_device *)device, name, atoi(argv[3]));
        if (ret == 0) {
            log_info("set angle %s to %s success", name, argv[3]);
        } else {
            log_error("set angle failed");
        }
    }
    else if (strcmp(argv[1], "stop") == 0) {
        ret = ((struct motor_device *)device)->stop((struct motor_device *)device, name);
        if (ret == 0) {
            log_info("stopped %s", name);
        } else {
            log_error("stop failed");
        }
    }
    else if (strcmp(argv[1], "info") == 0) {
        struct motor_info info = {0};
        ret = ((struct motor_device *)device)->get_info((struct motor_device *)device, name, &info);
        if (ret == 0) {
            log_info("motor %s: pos=%d speed_step=%d status=%d", name, info.pos, info.speed_step, info.status);
        } else {
            log_error("get info failed");
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
SHELL_CMD_EXPORT_ALIAS(hw_motor_command, hw_motor_cmd, motor program);
#endif

#endif
