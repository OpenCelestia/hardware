/*
 * SoC platform HAL smoke test (AnyCloud39AV100_V2: ak_sdk_init via hal_soc_platform).
 */
#if CONFIG_HW_SOC_PLATFORM

#include "soc_platform.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "HAL SOC PLATFORM TEST"

static void print_help(const char *prog)
{
    printf("\nusage: %s [--mode 0|1] [--log-level 1-6]\n\n", prog);
    printf("  --mode       0=normal, 1=debug (default 0)\n");
    printf("  --log-level  ak log level after init (optional)\n");
    printf("  --help       show help\n\n");
    printf("example:\n  %s\n  %s --mode 0 --log-level 4\n\n", prog, prog);
}

#if (defined OS_NAME_LINUX)
int main(int argc, char **argv)
#else
static int hw_soc_platform_test(int argc, char **argv)
#endif
{
    int mode = 0;
    int log_level = -1;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_help(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--mode") == 0 && (i + 1) < argc) {
            mode = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--log-level") == 0 && (i + 1) < argc) {
            log_level = atoi(argv[++i]);
        } else {
            log_error("unknown option: %s", argv[i]);
            print_help(argv[0]);
            return -1;
        }
    }

    log_set_level(LOG_INFO);

    int ret = soc_platform_init(mode);
    if (ret != SOC_PLATFORM_OK) {
        log_error("soc_platform_init(mode=%d) failed ret=%d", mode, ret);
        return -1;
    }
    log_info("soc_platform_init(mode=%d) ok", mode);

    if (log_level >= 1) {
        ret = soc_platform_set_log_level(log_level);
        if (ret != SOC_PLATFORM_OK) {
            log_error("soc_platform_set_log_level(%d) failed ret=%d", log_level, ret);
            return -1;
        }
        log_info("soc_platform_set_log_level(%d) ok", log_level);
    }

    log_info("phase1 soc_platform smoke passed");
    return 0;
}

#if ((defined OS_NAME_AOS) || (defined OS_NAME_RTT))
SHELL_CMD_EXPORT_ALIAS(hw_soc_platform_test, hw_soc_platform_test, soc platform test);
#endif

#endif /* CONFIG_HW_SOC_PLATFORM */
