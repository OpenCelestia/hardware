/*
 * ADC HAL sample test
 */
#if CONFIG_HW_ADC

#include "adc.h"
#include "hardware.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "soc_platform.h"
#include "log.h"
#define LOG_TAG "HAL ADC TEST"

static void print_help(void)
{
    printf("\n"
           "usage: <channel> [bytes]\n"
           "\n"
           "args:\n"
           "  channel    ADC channel number (e.g. 0, 1, 2)\n"
           "  bytes      number of bytes to read (default: 8, max: 64)\n"
           "\n"
           "example:\n"
           "  0          read 8 bytes from channel 0\n"
           "  0 4        read 4 bytes from channel 0\n"
           "  1 8        read 8 bytes from channel 1\n");
}

#if (defined OS_NAME_LINUX)
int main(int argc, char **argv)
#else
static int hw_adc_test(int argc, char **argv)
#endif
{
    soc_platform_init(0);
    log_set_level(LOG_INFO);

    if (argc < 2 || strcmp(argv[1], "help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_help();
        return -1;
    }

    const char *channel = argv[1];
    uint32_t bytes = (argc > 2) ? (uint32_t)atoi(argv[2]) : 8;
    if (bytes == 0 || bytes > 64) {
        bytes = 8;
    }

    const struct hw_module_t *module = NULL;
    struct hw_device_t *device = NULL;
    int ret = -1;

    /* Initialize and get module */
    hw_module_init();
    
    ret = hw_get_module(ADC_HARDWARE_MODULE_ID, &module);
    if (ret != 0 || module == NULL) {
        log_error("get module failed");
        return -1;
    }

    /* Open device */
    ret = module->methods->open(module, channel, &device);
    if (ret != 0 || device == NULL) {
        log_error("open channel %s failed", channel);
        return -1;
    }
    log_info("open channel %s success", channel);

    /* Read data */
    uint8_t buffer[64] = {0};
    ret = ((struct adc_device *)device)->read((struct adc_device *)device, channel, buffer, bytes);
    if (ret == 0) {
        log_info("read %u bytes from channel %s", bytes, channel);
        for (uint32_t i = 0; i < bytes; i++) {
            log_info("  [%u] = 0x%02x", i, buffer[i]);
        }
    } else {
        log_error("read failed");
    }

    /* Close device */
    device->close(device);
    log_info("closed");

    return ret;
}

#if ((defined OS_NAME_AOS) || (defined OS_NAME_RTT))
SHELL_CMD_EXPORT_ALIAS(hw_adc_test, hw_adc_test, adc test program);
#endif

#endif
