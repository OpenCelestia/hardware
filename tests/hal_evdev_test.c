/*
 * EVDEV (Input) HAL sample test
 */
#if CONFIG_HW_INPUT

#include "input_evdev.h"
#include "hardware.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#include "soc_platform.h"
#include "log.h"
#define LOG_TAG "HAL EVDEV TEST"

static struct hw_device_t *device = NULL;
static const struct hw_module_t *module = NULL;

static void print_help(void)
{
    printf("\n"
           "usage: <command> [args]\n"
           "\n"
           "commands:\n"
           "  help                              show this help\n"
           "  list                              list /dev/input/event* and sysfs names\n"
           "  probe <touch|keyboard>            open device and read one event (3s timeout)\n"
           "  open <touch|keyboard> [count]     open device and read events\n"
           "\n"
           "example:\n"
           "  list\n"
           "  probe touch\n"
           "  open touch 20\n"
           "  open keyboard 10\n");
}

static int ensure_module(void)
{
    if (module != NULL) {
        return 0;
    }

    hw_module_init();
    if (input_get_module(&module, INPUT_INSTANCE_EVDEV) != 0 || module == NULL) {
        log_error("get module failed");
        return -1;
    }
    return 0;
}

static int read_sysfs_input_name(int event_id, char *name, size_t name_size)
{
    char path[128];
    int fd;
    ssize_t ret;

    snprintf(path, sizeof(path), "/sys/class/input/event%d/device/name", event_id);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    memset(name, 0, name_size);
    ret = read(fd, name, name_size - 1);
    close(fd);

    if (ret <= 0) {
        return -1;
    }
    if (name[ret - 1] == '\n') {
        name[ret - 1] = '\0';
    }
    return 0;
}

static int evdev_list_devices(void)
{
    log_info("=== input devices ===");
    for (int i = 0; i < 16; i++) {
        char path[64];
        char name[64] = {0};

        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        if (access(path, F_OK) != 0) {
            continue;
        }

        read_sysfs_input_name(i, name, sizeof(name));
        log_info("  %s  name='%s'", path, name[0] ? name : "(unknown)");
    }
    return 0;
}

static int evdev_read_events(const char *name, int count, int require_one)
{
    int ret;
    int received = 0;

    if (ensure_module() != 0) {
        return -1;
    }

    ret = module->methods->open(module, name, &device);
    if (ret != 0 || device == NULL) {
        log_error("open %s failed, ret=%d", name, ret);
        return -1;
    }
    log_info("open %s success", name);

    struct evdev_device *dev = (struct evdev_device *)device;
    if (dev->read == NULL) {
        log_error("device read function is NULL");
        device->close(device);
        device = NULL;
        return -1;
    }

    log_info("reading up to %d events (touch screen to generate input)...", count);
    struct timeval timeout = {3, 0};

    for (int i = 0; i < count; i++) {
        struct evdev_event event = {0};
        ret = dev->read(dev, &event, &timeout);
        if (ret == 0) {
            received++;
            printf("[%3d] type=%u code=%u value=%d x=%d y=%d\n",
                   received, event.type, event.code, event.value, event.xpos, event.ypos);
            if (count == 1) {
                break;
            }
            timeout.tv_sec = 1;
            timeout.tv_usec = 0;
        }
    }

    device->close(device);
    device = NULL;
    log_info("received %d event(s)", received);

    if (require_one && received == 0) {
        log_error("no events (check driver: cst816x.ko / ak_gpio_keys.ko)");
        return -1;
    }
    return 0;
}

#if (defined OS_NAME_LINUX)
int main(int argc, char **argv)
#else
static int hw_evdev_command(int argc, char **argv)
#endif
{
    soc_platform_init(0);
    log_set_level(LOG_INFO);

    if (argc < 2 || strcmp(argv[1], "help") == 0) {
        print_help();
        return -1;
    }

    if (strcmp(argv[1], "list") == 0) {
        return evdev_list_devices();
    }

    if (strcmp(argv[1], "probe") == 0) {
        const char *name = (argc > 2) ? argv[2] : INPUT_EVDEV_NAME_TOUCH;
        return evdev_read_events(name, 1, 0);
    }

    if (strcmp(argv[1], "open") == 0) {
        const char *name = (argc > 2) ? argv[2] : INPUT_EVDEV_NAME_TOUCH;
        int count = (argc > 3) ? atoi(argv[3]) : 100;
        int require_one = (getenv("INPUT_STRICT") != NULL && strcmp(getenv("INPUT_STRICT"), "1") == 0);
        if (count <= 0) {
            count = 100;
        }
        return evdev_read_events(name, count, require_one);
    }

    print_help();
    return -1;
}

#if ((defined OS_NAME_AOS) || (defined OS_NAME_RTT))
SHELL_CMD_EXPORT_ALIAS(hw_evdev_command, hw_evdev_cmd, evdev program);
#endif

#endif
