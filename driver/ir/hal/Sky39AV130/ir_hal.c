/*
 * IRCUT / IRLED via /dev/gpio. Sequence matches apps/utils/cmd_ircut.c.
 * Pins follow EVB_CBDM_AK3918AV130N_V1.0.0.dts (closest CBD public board):
 * ircut_a_gpio = gpio 23, ircut_b_gpio = 0xffff (unused), irled_gpio = gpio 10.
 */
#include "ir_hal.h"

#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "gpio_ioctl.h"

#define MIRA_IRCUT_PIN_A 23
#define MIRA_IRCUT_PIN_B (-1)
#define MIRA_IRLED_PIN   10

static int s_ircut_ms = 100;
static int s_irled_level = 1;
static int s_irled_on;

static int mira_gpio_set(int pin, int value)
{
    t_gpio_cg p;
    int fd;
    int ret;

    fd = open("/dev/gpio", O_RDWR);
    if (fd < 0)
        return -1;
    memset(&p, 0, sizeof(p));
    p.nr = (unsigned int)pin;
    ret = ioctl(fd, IOC_GPIO_SET_PIN_AS_GPIO, &p);
    p.dir = 1;
    ret |= ioctl(fd, IOC_GPIO_SET_PIN_DIR, &p);
    p.config = 0x1010310;
    ret |= ioctl(fd, IOC_GPIO_SET_PIN_CONFIG, &p);
    p.data = (unsigned int)value;
    ret |= ioctl(fd, IOC_GPIO_SET_PIN_OUTPUT_DATA, &p);
    close(fd);
    return ret == 0 ? 0 : -1;
}

int hal_ir_init(const char *cfg_path)
{
    (void)cfg_path;
    return 0;
}

int hal_ir_set_ircut(int status)
{
    int a = status ? 0 : 1;
    int b = status ? 1 : 0;

    if (mira_gpio_set(MIRA_IRCUT_PIN_A, a) != 0)
        return -1;
    if (MIRA_IRCUT_PIN_B >= 0 && mira_gpio_set(MIRA_IRCUT_PIN_B, b) != 0)
        return -1;
    usleep((useconds_t)s_ircut_ms * 1000);
    (void)mira_gpio_set(MIRA_IRCUT_PIN_A, 0);
    if (MIRA_IRCUT_PIN_B >= 0)
        (void)mira_gpio_set(MIRA_IRCUT_PIN_B, 0);
    return 0;
}

void hal_ir_set_ircut_time(int time_ms)
{
    if (time_ms > 0)
        s_ircut_ms = time_ms;
}

int hal_ir_get_ircut_time(int *time_ms)
{
    if (time_ms == NULL)
        return -1;
    *time_ms = s_ircut_ms;
    return 0;
}

int hal_irled_init(const hal_irled_param_t *param)
{
    if (param != NULL)
        s_irled_level = param->working_level ? 1 : 0;
    return 0;
}

int hal_irled_get_status(int *status)
{
    if (status == NULL)
        return -1;
    *status = s_irled_on;
    return 0;
}

int hal_irled_set_status(int status)
{
    int level = status ? s_irled_level : !s_irled_level;

    if (mira_gpio_set(MIRA_IRLED_PIN, level) != 0)
        return -1;
    s_irled_on = status ? 1 : 0;
    return 0;
}
