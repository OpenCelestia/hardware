#include "hal_soc_platform_api.h"

#include <string.h>

#include "ak_common.h"
#include "system_config.h"

int hal_soc_platform_init(int mode)
{
    sdk_run_config cfg;

    (void)mode;
    (void)config_set_path(CONFIG_PATH_TYPE_LOG, "/tmp/log/");

    memset(&cfg, 0, sizeof(cfg));
    return ak_sdk_init(&cfg) == AK_SUCCESS ? 0 : -1;
}

int hal_soc_platform_set_log_level(int level)
{
    (void)level;
    return 0;
}
