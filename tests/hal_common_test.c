/*
 * HAL common smoke test.
 * This validates that hardware modules can be queried via HAL registry.
 */
#include <stdio.h>

#include "hardware.h"
#include "header.h"
#include "log.h"

#define LOG_TAG "HAL COMMON TEST"

static int probe_module(const char *id)
{
    const struct hw_module_t *module = NULL;
    int ret = hw_get_module(id, &module);
    if (ret == 0 && module != NULL) {
        log_info("[ok] module %s", id);
        return 0;
    }
    log_error("[fail] module %s (ret=%d)", id, ret);
    return -1;
}

#if (defined OS_NAME_LINUX)
int main(int argc, char **argv)
#else
static int hw_common_test(int argc, char **argv)
#endif
{
    (void)argc;
    (void)argv;

    int ret = hw_module_init();
    if (ret != 0) {
        log_error("hw_module_init failed ret=%d", ret);
        return ret;
    }

    log_info("probing hardware modules...");

#if CONFIG_HW_AUDIO
    probe_module(AUDIO_HARDWARE_MODULE_ID);
#endif
#if CONFIG_HW_CAMERA
    probe_module(CAMERA_HARDWARE_MODULE_ID);
#endif
#if CONFIG_HW_DISPLAY
    probe_module(DISPLAY_HARDWARE_MODULE_ID);
#endif
#if CONFIG_HW_GPU
    probe_module(GPU_HARDWARE_MODULE_ID);
#endif
#if CONFIG_HW_DRIVER_MMC
    probe_module(MMC_HARDWARE_MODULE_ID);
#endif
#if CONFIG_HW_DRIVER_ADC
    probe_module(ADC_HARDWARE_MODULE_ID);
#endif
#if CONFIG_HW_DRIVER_GPIO
    probe_module(GPIO_HARDWARE_MODULE_ID);
#endif
#if CONFIG_HW_DRIVER_PWM
    probe_module(PWM_HARDWARE_MODULE_ID);
#endif
#if CONFIG_HW_DRIVER_WDT
    probe_module(WDT_HARDWARE_MODULE_ID);
#endif
#if CONFIG_HW_DRIVER_MOTOR
    probe_module(MOTOR_HARDWARE_MODULE_ID);
#endif
#if CONFIG_HW_DRIVER_EFUSE
    probe_module(EFUSE_HARDWARE_MODULE_ID);
#endif
#if CONFIG_HW_DRIVER_LED
    probe_module(LED_HARDWARE_MODULE_ID);
#endif
#if CONFIG_HW_DRIVER_IR
    probe_module(IR_HARDWARE_MODULE_ID);
#endif
#if CONFIG_HW_WIFI
    probe_module(WIFI_HARDWARE_MODULE_ID);
#endif
#if CONFIG_HW_ETHERNET
    probe_module(ETHERNET_HARDWARE_MODULE_ID);
#endif
#if CONFIG_HW_INPUT
    probe_module(INPUT_HARDWARE_MODULE_ID);
#endif

    log_info("probe complete");
    return 0;
}

#if ((defined OS_NAME_AOS) || (defined OS_NAME_RTT))
SHELL_CMD_EXPORT_ALIAS(hw_common_test, hw_common_test, hardware common test);
#endif
