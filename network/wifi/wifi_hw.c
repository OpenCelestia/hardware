// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/network/wifi.
 *
 * WiFi HAL common layer (backend-agnostic).
 * - Provides a single HAL device entry and delegates operations to a build-selected backend.
 * - Backend differences (WPA vs AIC8800) must live under their own directories.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stddef.h>
#include <sys/stat.h>

#include "gthread.h"
#include "wifi_hw.h"
#include "net/net.h"
#include "log.h"
#include "hardware.h"
#include "system_thread.h"
#include "system_core.h"
#include "system_config.h"

#define LOG_TAG "HAL WIFI"
#define WIFI_CONFIG_FILE_NAME      "wifi_config.ini"
#define WIFI_CONFIG_FILE_FALLBACK  "hal_wifi_config.ini"

/* Built-in defaults when no wifi INI is deployed (settings.json holds these). */
#define WIFI_DEFAULT_ABILITY       2
#define WIFI_DEFAULT_STA_ENABLE    0
#define WIFI_DEFAULT_AP_ENABLE     0
#define WIFI_DEFAULT_AP_SSID       "wifi_ap"
#define WIFI_DEFAULT_AP_PWD        "87654321"

THREAD_RUN_DEF(thWifiRun);
REENTRY_INIT(wifi_modules);

typedef struct {
    wifi_hw_device_t device;
} wifi_hal_context_t;

static inline wifi_hal_context_t *wifi_context_from_common(struct hw_device_t *device)
{
    return (wifi_hal_context_t *)((char *)device - offsetof(wifi_hal_context_t, device.common));
}

static int wifi_check_initialized(void)
{
    if (REENTRY_GET_INIT_STATUS(wifi_modules) == 0) {
        log_error("%s wifi_modules not initialized", __FUNCTION__);
        return HARDWARE_WIFI_ERROR_INVALID_STATE;
    }
    return HARDWARE_WIFI_OK;
}

static int wifi_is_initialized(void)
{
    return REENTRY_GET_INIT_STATUS(wifi_modules) != 0;
}

static wifi_connect_status_cb_t g_wifi_connect_status_cb;

typedef struct {
    char ifname[32];
    int sta_status;
    int wifi_mode;
    int link_req;
    int ability;
    int sta_enable;
    int ap_enable;
    char cfg_path[256];
    char sta_ssid[32];
    char sta_pwd[64];
    char ap_ssid[32];
    char ap_pwd[64];
    char sta_bssid[64];
    int sta_freq_khz;
    GMutex hMutex;
} wifi_common_state_t;

static wifi_common_state_t g_state;

static int hal_wifi_init(void);

static void runtime_set_sta_status(wifi_hw_runtime_t *rt, int status)
{
    /* Keep rt and g_state in sync: runtime_apply() copies rt->sta_status back
     * to g_state after each service tick. Updating only g_state here used to
     * wipe CTRL-EVENT-CONNECTED (UI stuck on "connecting" until timeout). */
    if (rt)
        rt->sta_status = status;
    g_state.sta_status = status;
    if (g_wifi_connect_status_cb)
        g_wifi_connect_status_cb((wifi_hal_status_t)status);
}

static void runtime_fill(wifi_hw_runtime_t *rt)
{
    memset(rt, 0, sizeof(*rt));
    strncpy(rt->ifname, g_state.ifname, sizeof(rt->ifname) - 1);
    strncpy(rt->sta_ssid, g_state.sta_ssid, sizeof(rt->sta_ssid) - 1);
    strncpy(rt->sta_pwd, g_state.sta_pwd, sizeof(rt->sta_pwd) - 1);
    strncpy(rt->ap_ssid, g_state.ap_ssid, sizeof(rt->ap_ssid) - 1);
    strncpy(rt->ap_pwd, g_state.ap_pwd, sizeof(rt->ap_pwd) - 1);
    strncpy(rt->sta_bssid, g_state.sta_bssid, sizeof(rt->sta_bssid) - 1);
    rt->sta_freq_khz = g_state.sta_freq_khz;
    rt->link_request = g_state.link_req;
    rt->sta_status = g_state.sta_status;
    rt->wifi_mode = g_state.wifi_mode;
    rt->set_sta_status = runtime_set_sta_status;
}

static void runtime_apply(const wifi_hw_runtime_t *rt)
{
    g_state.link_req = rt->link_request;
    g_state.sta_status = rt->sta_status;
    strncpy(g_state.sta_bssid, rt->sta_bssid, sizeof(g_state.sta_bssid) - 1);
    g_state.sta_freq_khz = rt->sta_freq_khz;
}

static void pick_iface_name(char *buf, size_t len)
{
    const char *env = getenv("WIFI_INTERFACE");
    if (env && env[0]) {
        strncpy(buf, env, len - 1);
        buf[len - 1] = '\0';
        return;
    }
    strncpy(buf, WIFI_HAL_DEFAULT_IFACE, len - 1);
    buf[len - 1] = '\0';
}

static const char *wifi_cfg_resolve_path(void)
{
    if (g_state.cfg_path[0])
        return g_state.cfg_path;

    if (config_build_file_path(WIFI_CONFIG_FILE_NAME, g_state.cfg_path,
                               sizeof(g_state.cfg_path)) != 0) {
        if (config_build_file_path(WIFI_CONFIG_FILE_FALLBACK, g_state.cfg_path,
                                   sizeof(g_state.cfg_path)) != 0) {
            strncpy(g_state.cfg_path, "/etc/config/" WIFI_CONFIG_FILE_NAME,
                    sizeof(g_state.cfg_path) - 1);
            g_state.cfg_path[sizeof(g_state.cfg_path) - 1] = '\0';
        }
    }
    return g_state.cfg_path;
}

static void wifi_cfg_apply_ability_rules(void)
{
    switch (g_state.ability) {
    case 0:
        g_state.ap_enable = 0;
        break;
    case 1:
        g_state.sta_enable = 0;
        break;
    case 2:
        if (g_state.sta_enable == 1 && g_state.ap_enable == 1) {
            g_state.sta_enable = 1;
            g_state.ap_enable = 0;
        }
        break;
    case 3:
        break;
    default:
        g_state.sta_enable = 0;
        g_state.ap_enable = 0;
        break;
    }
}

int hal_wifi_cfg_write(void)
{
    const char *cfg_path = wifi_cfg_resolve_path();
    dictionary *handle;

    if (access(cfg_path, R_OK) != 0) {
        log_warn("wifi INI not found for write: %s (credentials in settings.json)", cfg_path);
        return HARDWARE_WIFI_OK;
    }

    handle = iniparser_load(cfg_path);
    if (handle == NULL) {
        log_error("cannot parse file: %s", cfg_path);
        return HARDWARE_WIFI_ERROR_CONFIG_FAILED;
    }

    iniparser_seti(handle, "station:enable", g_state.sta_enable);
    iniparser_set(handle, "station:ssid", g_state.sta_ssid);
    iniparser_set(handle, "station:psk", g_state.sta_pwd);

    iniparser_seti(handle, "ap:enable", g_state.ap_enable);
    iniparser_set(handle, "ap:ssid", g_state.ap_ssid);
    iniparser_set(handle, "ap:psk", g_state.ap_pwd);

    iniparser_save_file(handle, cfg_path);
    iniparser_freedict(handle);
    return HARDWARE_WIFI_OK;
}

static int hal_wifi_load_config(const char *cfg_path)
{
    char default_path[256];
    char loaded_cfg_path[256];
    int ability;
    int sta_enable;
    int ap_enable;
    char sta_ssid[32];
    char sta_pwd[64];
    char ap_ssid[32];
    char ap_pwd[64];
    dictionary *handle;

    if (!cfg_path) {
        if (config_build_file_path(WIFI_CONFIG_FILE_NAME, default_path,
                                   sizeof(default_path)) != 0) {
            if (config_build_file_path(WIFI_CONFIG_FILE_FALLBACK, default_path,
                                       sizeof(default_path)) != 0) {
                return HARDWARE_WIFI_ERROR_INVALID_PARAM;
            }
        }
        cfg_path = default_path;
    }

    /* Try INI; if absent use built-in defaults (settings.json now owns these). */
    if (access(cfg_path, R_OK) != 0) {
        log_warn("wifi INI not found: %s, using built-in defaults", cfg_path);
        ability = WIFI_DEFAULT_ABILITY;
        sta_enable = WIFI_DEFAULT_STA_ENABLE;
        sta_ssid[0] = '\0';
        sta_pwd[0] = '\0';
        ap_enable = WIFI_DEFAULT_AP_ENABLE;
        strncpy(ap_ssid, WIFI_DEFAULT_AP_SSID, sizeof(ap_ssid) - 1);
        ap_ssid[sizeof(ap_ssid) - 1] = '\0';
        strncpy(ap_pwd, WIFI_DEFAULT_AP_PWD, sizeof(ap_pwd) - 1);
        ap_pwd[sizeof(ap_pwd) - 1] = '\0';
        loaded_cfg_path[0] = '\0';
        goto apply;
    }

    handle = iniparser_load(cfg_path);
    if (handle == NULL) {
        log_error("cannot parse file: %s", cfg_path);
        return HARDWARE_WIFI_ERROR_CONFIG_FAILED;
    }

    snprintf(loaded_cfg_path, sizeof(loaded_cfg_path), "%s", cfg_path);
    ability = iniparser_dict_getint(handle, "main", "ability",
                                    WIFI_DEFAULT_ABILITY);
    sta_enable = iniparser_dict_getint(handle, "station", "enable",
                                       WIFI_DEFAULT_STA_ENABLE);
    snprintf(sta_ssid, sizeof(sta_ssid), "%s",
             iniparser_dict_getstring(handle, "station", "ssid",
                                      iniparser_dict_getstring(handle, "station",
                                                               "sta_ssid", "")));
    snprintf(sta_pwd, sizeof(sta_pwd), "%s",
             iniparser_dict_getstring(handle, "station", "psk",
                                      iniparser_dict_getstring(handle, "station",
                                                               "sta_pwd", "")));
    ap_enable = iniparser_dict_getint(handle, "ap", "enable",
                                      WIFI_DEFAULT_AP_ENABLE);
    snprintf(ap_ssid, sizeof(ap_ssid), "%s",
             iniparser_dict_getstring(handle, "ap", "ssid",
                                      iniparser_dict_getstring(handle, "ap",
                                                               "ap_ssid",
                                                               WIFI_DEFAULT_AP_SSID)));
    snprintf(ap_pwd, sizeof(ap_pwd), "%s",
             iniparser_dict_getstring(handle, "ap", "psk",
                                      iniparser_dict_getstring(handle, "ap",
                                                               "ap_pwd",
                                                               WIFI_DEFAULT_AP_PWD)));
    iniparser_freedict(handle);

apply:

    if (!wifi_is_initialized()) {
        int ret = hal_wifi_init();
        if (ret != HARDWARE_WIFI_OK && ret != HARDWARE_WIFI_ERROR_BUSY)
            return ret;
    }

    snprintf(g_state.cfg_path, sizeof(g_state.cfg_path), "%s", loaded_cfg_path);
    g_state.ability = ability;
    g_state.sta_enable = sta_enable;
    g_state.ap_enable = ap_enable;
    strncpy(g_state.sta_ssid, sta_ssid, sizeof(g_state.sta_ssid) - 1);
    strncpy(g_state.sta_pwd, sta_pwd, sizeof(g_state.sta_pwd) - 1);
    strncpy(g_state.ap_ssid, ap_ssid, sizeof(g_state.ap_ssid) - 1);
    strncpy(g_state.ap_pwd, ap_pwd, sizeof(g_state.ap_pwd) - 1);

    wifi_cfg_apply_ability_rules();

    /* Do not auto-connect from empty product station credentials. */
    if (g_state.sta_enable == 1 && g_state.sta_ssid[0] != '\0')
        g_state.link_req = 1;
    if (g_state.ap_enable == 1)
        g_state.wifi_mode = 2;

    return HARDWARE_WIFI_OK;
}

static int wifi_run(void *arg)
{
    const wifi_backend_ops_t *ops = wifi_backend_get();
    wifi_hw_runtime_t rt;
    int sleep_ms = 200;

    (void)arg;
    log_info("in backend=%s", ops ? ops->name : "?");
    while (THREAD_RUN_CHECK(thWifiRun)) {
        if (g_state.wifi_mode != 0) {
            system_sleep_ms(1000);
            continue;
        }
        g_mutex_lock(&g_state.hMutex);
        runtime_fill(&rt);
        if (ops && ops->service)
            sleep_ms = ops->service(&rt);
        runtime_apply(&rt);
        g_mutex_unlock(&g_state.hMutex);
        system_sleep_ms(sleep_ms > 0 ? sleep_ms : 200);
    }
    log_info("out");
    return 0;
}

int hal_wifi_init(void)
{
    if (REENTRY_GET_INIT_STATUS(wifi_modules)) {
        log_info("wifi already initialized");
        return HARDWARE_WIFI_OK;
    }
    memset(&g_state, 0, sizeof(g_state));
    pick_iface_name(g_state.ifname, sizeof(g_state.ifname));
    g_state.sta_status = WIFI_STA_STAT_IDLE;
    g_mutex_init(&g_state.hMutex);

    {
        const wifi_backend_ops_t *ops = wifi_backend_get();
        if (ops && ops->init && ops->init() != HARDWARE_WIFI_OK)
            log_error("wifi backend init failed (%s)", ops->name);
    }
    THREAD_RUN_START(thWifiRun, wifi_run, NULL, 0, 0);
    REENTRY_START(wifi_modules);
    return HARDWARE_WIFI_OK;
}

int hal_wifi_deinit(void)
{
    int init_ret = wifi_check_initialized();
    if (init_ret != HARDWARE_WIFI_OK)
        return init_ret;

    /* Stop worker thread before releasing shared state. */
    THREAD_RUN_STOP(thWifiRun);
    {
        const wifi_backend_ops_t *ops = wifi_backend_get();
        if (ops && ops->deinit)
            ops->deinit();
    }
    g_mutex_clear(&g_state.hMutex);
    REENTRY_STOP(wifi_modules);
    return HARDWARE_WIFI_OK;
}

static int hal_wifi_sta_connect(char *ssid, char *pwd, int wait_ms)
{
    const wifi_backend_ops_t *ops = wifi_backend_get();
    wifi_hw_runtime_t rt;

    int init_ret = wifi_check_initialized();
    if (init_ret != HARDWARE_WIFI_OK)
        return init_ret;
    if (!ssid || !ssid[0])
        return HARDWARE_WIFI_ERROR_INVALID_PARAM;
    if (!pwd)
        pwd = "";

    strncpy(g_state.sta_ssid, ssid, sizeof(g_state.sta_ssid) - 1);
    strncpy(g_state.sta_pwd, pwd, sizeof(g_state.sta_pwd) - 1);
    g_state.sta_enable = 1;
    g_state.wifi_mode = 0;

    g_mutex_lock(&g_state.hMutex);
    g_state.link_req = 1;
    runtime_fill(&rt);
    rt.link_request = 1;
    /* wait_ms==0: connect only from thWifiRun (uwlan_connect blocks). */
    if (wait_ms > 0 && ops && ops->sta_connect) {
        if (ops->sta_connect(&rt) != HARDWARE_WIFI_OK)
            rt.link_request = 1;
    }
    runtime_apply(&rt);
    g_mutex_unlock(&g_state.hMutex);

    if (wait_ms > 0) {
        int elapsed = 0;
        const int step_ms = 200;
        while (elapsed < wait_ms) {
            int st = g_state.sta_status;
            if (st == WIFI_STA_STAT_CONNECTED || st == WIFI_STA_STAT_ADDRESSED)
                return HARDWARE_WIFI_OK;
            if (st == WIFI_STA_STAT_PSK_FAIL || st == WIFI_STA_STAT_AP_NOT_FOUND)
                return HARDWARE_WIFI_ERROR_IO;
            usleep((useconds_t)step_ms * 1000);
            elapsed += step_ms;
        }
    }
    return HARDWARE_WIFI_OK;
}

int hal_wifi_sta_disconnect(void)
{
    const wifi_backend_ops_t *ops = wifi_backend_get();
    wifi_hw_runtime_t rt;
    int init_ret = wifi_check_initialized();
    if (init_ret != HARDWARE_WIFI_OK)
        return init_ret;

    g_mutex_lock(&g_state.hMutex);
    runtime_fill(&rt);
    if (ops && ops->sta_disconnect)
        (void)ops->sta_disconnect(&rt);
    rt.sta_status = WIFI_STA_STAT_IDLE;
    runtime_apply(&rt);
    g_mutex_unlock(&g_state.hMutex);
    return HARDWARE_WIFI_OK;
}

static wifi_hal_status_t hal_wifi_sta_status(void)
{
    int init_ret = wifi_check_initialized();
    if (init_ret != HARDWARE_WIFI_OK)
        return WIFI_STA_STAT_IDLE;
    /* process-local status; interface may still hold IP from previous process */
    if (g_state.sta_status == WIFI_STA_STAT_IDLE) {
        char ip[32];
        if (net_get_ip(g_state.ifname, ip) == 0 && ip[0] && strcmp(ip, "0.0.0.0") != 0)
            return WIFI_STA_STAT_ADDRESSED;
    }
    return (wifi_hal_status_t)g_state.sta_status;
}

int hal_wifi_ap_start(void)
{
    const wifi_backend_ops_t *ops = wifi_backend_get();
    g_state.wifi_mode = 2;
    if (ops && ops->ap_start)
        return ops->ap_start(g_state.ifname, g_state.ap_ssid, g_state.ap_pwd);
    return HARDWARE_WIFI_ERROR_NOT_SUPPORTED;
}

int hal_wifi_ap_stop(void)
{
    const wifi_backend_ops_t *ops = wifi_backend_get();
    if (ops && ops->ap_stop)
        (void)ops->ap_stop(g_state.ifname);
    g_state.wifi_mode = 0;
    return HARDWARE_WIFI_OK;
}

int hal_wifi_ap_set_config(char *ssid, char *pwd)
{
    if (!ssid || !pwd)
        return HARDWARE_WIFI_ERROR_INVALID_PARAM;
    strncpy(g_state.ap_ssid, ssid, sizeof(g_state.ap_ssid) - 1);
    strncpy(g_state.ap_pwd, pwd, sizeof(g_state.ap_pwd) - 1);
    return HARDWARE_WIFI_OK;
}

int hal_wifi_ap_get_config(char *ssid, char *pwd)
{
    if (!ssid || !pwd)
        return HARDWARE_WIFI_ERROR_INVALID_PARAM;
    strncpy(ssid, g_state.ap_ssid, 32);
    strncpy(pwd, g_state.ap_pwd, 64);
    return HARDWARE_WIFI_OK;
}

static int wifi_device_load_config(struct wifi_hw_device *dev, const char *cfg_path)
{
    (void)dev;
    return hal_wifi_load_config(cfg_path);
}

static int wifi_device_sta_scan(struct wifi_hw_device *dev)
{
    const wifi_backend_ops_t *ops = wifi_backend_get();
    (void)dev;
    if (!ops || !ops->scan)
        return HARDWARE_WIFI_ERROR_NOT_SUPPORTED;
    return ops->scan(g_state.ifname);
}

static int wifi_device_sta_get_scan_results(struct wifi_hw_device *dev, wifi_scan_result_t *results, int *num_results)
{
    const wifi_backend_ops_t *ops = wifi_backend_get();
    (void)dev;
    if (!ops || !ops->get_scan_results)
        return HARDWARE_WIFI_ERROR_NOT_SUPPORTED;
    return ops->get_scan_results(results, num_results);
}

static int wifi_device_sta_connect(struct wifi_hw_device *dev, const char *ssid, const char *passphrase, int wait_ms)
{
    (void)dev;
    return hal_wifi_sta_connect((char *)ssid, (char *)passphrase, wait_ms);
}

static int wifi_device_sta_disconnect(struct wifi_hw_device *dev)
{
    (void)dev;
    return hal_wifi_sta_disconnect();
}

static wifi_hal_status_t wifi_device_sta_status(struct wifi_hw_device *dev)
{
    (void)dev;
    return hal_wifi_sta_status();
}

static int wifi_device_set_connect_status_cb(struct wifi_hw_device *dev, wifi_connect_status_cb_t cb)
{
    (void)dev;
    g_wifi_connect_status_cb = cb;
    return HARDWARE_WIFI_OK;
}

static int wifi_device_ap_start(struct wifi_hw_device *dev) { (void)dev; return hal_wifi_ap_start(); }
static int wifi_device_ap_stop(struct wifi_hw_device *dev)  { (void)dev; return hal_wifi_ap_stop(); }
static int wifi_device_ap_set_config(struct wifi_hw_device *dev, const char *ssid, const char *pwd)
{
    (void)dev;
    return hal_wifi_ap_set_config((char *)ssid, (char *)pwd);
}
static int wifi_device_ap_get_config(struct wifi_hw_device *dev, char *ssid, char *pwd)
{
    (void)dev;
    return hal_wifi_ap_get_config(ssid, pwd);
}

static int wifi_device_p2p_not_supported(struct wifi_hw_device *dev)
{
    (void)dev;
    return HARDWARE_WIFI_ERROR_NOT_SUPPORTED;
}

static int wifi_device_p2p_connect_not_supported(struct wifi_hw_device *dev, const char *device_address)
{
    (void)dev;
    (void)device_address;
    return HARDWARE_WIFI_ERROR_NOT_SUPPORTED;
}

static int wifi_device_p2p_disconnect_not_supported(struct wifi_hw_device *dev)
{
    (void)dev;
    return HARDWARE_WIFI_ERROR_NOT_SUPPORTED;
}

static int wifi_device_p2p_get_peers(struct wifi_hw_device *dev, wifi_p2p_device_info_t *devices, int *num_devices)
{
    (void)dev;
    (void)devices;
    if (num_devices)
        *num_devices = 0;
    return HARDWARE_WIFI_ERROR_NOT_SUPPORTED;
}

static int wifi_device_p2p_group_owner(struct wifi_hw_device *dev) { (void)dev; return 0; }

static int wifi_device_p2p_group_info(struct wifi_hw_device *dev, wifi_p2p_group_info_t *info)
{
    (void)dev;
    (void)info;
    return HARDWARE_WIFI_ERROR_NOT_SUPPORTED;
}

static int s_ble_started;

static int wifi_path_exists(const char *path)
{
    struct stat st;

    return (path && stat(path, &st) == 0) ? 1 : 0;
}

static int wifi_ko_loaded(const char *mod)
{
    FILE *fp;
    char line[160];
    size_t n;

    if (!mod || !mod[0])
        return 0;
    n = strlen(mod);
    fp = fopen("/proc/modules", "r");
    if (!fp)
        return 0;
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (strncmp(line, mod, n) == 0 && (line[n] == ' ' || line[n] == '\t')) {
            fclose(fp);
            return 1;
        }
    }
    fclose(fp);
    return 0;
}

/* miio_client embeds NimBLE. A second nimble_linux resets HCI (reason=19). */
static int wifi_ble_host_is_miio(void)
{
    return wifi_path_exists("/usr/sbin/miio_client") ? 1 : 0;
}

static int wifi_device_ble_start(struct wifi_hw_device *dev)
{
    (void)dev;

    if (wifi_ble_host_is_miio()) {
        if (s_ble_started && wifi_ko_loaded("atbm_ble"))
            return HARDWARE_WIFI_OK;
        if (!wifi_path_exists("/usr/modules/atbm_ble.ko")) {
            log_error("atbm_ble.ko missing");
            return HARDWARE_WIFI_ERROR_NOT_SUPPORTED;
        }
        if (!wifi_ko_loaded("atbm_ble")) {
            SYSCALL("insmod /usr/modules/atbm_ble.ko");
            if (!wifi_ko_loaded("atbm_ble")) {
                log_error("insmod atbm_ble.ko failed");
                return HARDWARE_WIFI_ERROR_OPERATION_FAILED;
            }
        }
        SYSCALL("killall nimble_linux 2>/dev/null");
        s_ble_started = 1;
        log_info("BLE HCI ready (atbm_ble.ko, miio_client)");
        return HARDWARE_WIFI_OK;
    }

    /* No miio_client: previous bring-up, including boards without BLE. */
    if (s_ble_started)
        return HARDWARE_WIFI_OK;
    if (wifi_path_exists("/usr/modules/atbm_ble.ko"))
        SYSCALL("insmod /usr/modules/atbm_ble.ko");
    if (wifi_path_exists("/usr/sbin/nimble_linux"))
        SYSCALL("/usr/sbin/nimble_linux &");
    s_ble_started = 1;
    log_info("BLE provisioning started");
    return HARDWARE_WIFI_OK;
}

static int wifi_device_ble_stop(struct wifi_hw_device *dev)
{
    (void)dev;

    /* Vendor wifi_bt_driver_*.sh stop path: kill the NimBLE host, then
     * unload the BLE ko. */
    SYSCALL("killall nimble_linux 2>/dev/null");
    if (wifi_path_exists("/usr/modules/atbm_ble.ko"))
        SYSCALL("rmmod atbm_ble 2>/dev/null");
    s_ble_started = 0;
    return HARDWARE_WIFI_OK;
}

static int wifi_device_ble_is_ready(struct wifi_hw_device *dev)
{
    (void)dev;
    if (wifi_ble_host_is_miio())
        return wifi_ko_loaded("atbm_ble") ? 1 : 0;
    return s_ble_started ? 1 : 0;
}

static int wdev_close(struct hw_device_t *device)
{
    if (!device)
        return HARDWARE_WIFI_ERROR_INVALID_PARAM;
    wifi_hal_context_t *ctx = wifi_context_from_common(device);
    if (wifi_is_initialized())
        (void)hal_wifi_deinit();
    free(ctx);
    return HARDWARE_WIFI_OK;
}

static int wdev_open(const struct hw_module_t *module, const char *id, struct hw_device_t **device)
{
    (void)id;
    if (!module || !device)
        return HARDWARE_WIFI_ERROR_INVALID_PARAM;

    wifi_hal_context_t *ctx = (wifi_hal_context_t *)calloc(1, sizeof(*ctx));
    if (!ctx)
        return HARDWARE_WIFI_ERROR_MEMORY;

    int init_ret = hal_wifi_init();
    if (init_ret != HARDWARE_WIFI_OK) {
        free(ctx);
        return init_ret;
    }

    wifi_hw_device_t *wifi_dev = &ctx->device;
    wifi_dev->common.tag = HARDWARE_DEVICE_TAG;
    wifi_dev->common.version = 0;
    wifi_dev->common.module = (struct hw_module_t *)module;
    wifi_dev->common.close = wdev_close;

    wifi_dev->load_config = wifi_device_load_config;
    wifi_dev->sta_scan = wifi_device_sta_scan;
    wifi_dev->sta_get_scan_results = wifi_device_sta_get_scan_results;
    wifi_dev->sta_connect = wifi_device_sta_connect;
    wifi_dev->sta_disconnect = wifi_device_sta_disconnect;
    wifi_dev->sta_status = wifi_device_sta_status;
    wifi_dev->set_connect_status_cb = wifi_device_set_connect_status_cb;

    wifi_dev->ap_start = wifi_device_ap_start;
    wifi_dev->ap_stop = wifi_device_ap_stop;
    wifi_dev->ap_set_config = wifi_device_ap_set_config;
    wifi_dev->ap_get_config = wifi_device_ap_get_config;

    wifi_dev->p2p_discover_peers = wifi_device_p2p_not_supported;
    wifi_dev->p2p_stop_discover_peers = wifi_device_p2p_not_supported;
    wifi_dev->p2p_get_discovered_peers = wifi_device_p2p_get_peers;
    wifi_dev->p2p_connect = wifi_device_p2p_connect_not_supported;
    wifi_dev->p2p_disconnect = wifi_device_p2p_disconnect_not_supported;
    wifi_dev->p2p_create_group = wifi_device_p2p_not_supported;
    wifi_dev->p2p_remove_group = wifi_device_p2p_not_supported;
    wifi_dev->p2p_is_group_owner = wifi_device_p2p_group_owner;
    wifi_dev->p2p_get_group_info = wifi_device_p2p_group_info;

    wifi_dev->ble_start = wifi_device_ble_start;
    wifi_dev->ble_stop = wifi_device_ble_stop;
    wifi_dev->ble_is_ready = wifi_device_ble_is_ready;

    *device = &wifi_dev->common;
    return HARDWARE_WIFI_OK;
}

static struct hw_module_methods_t wifi_module_methods = {
    .open = wdev_open,
};

static struct hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = 1,
    .hal_api_version = HARDWARE_HAL_API_VERSION,
    .id = WIFI_HARDWARE_MODULE_ID,
    .name = "Celestia WiFi HW HAL",
    .author = "AnyKa Celestia",
    .methods = &wifi_module_methods,
    .dso = NULL,
    .reserved = {0}
};

/**
 * @brief Initialize WiFi hardware module.
 * @details Registers the WiFi module with the hardware module system.
 * @return HARDWARE_WIFI_OK on success, negative error code on failure.
 */
int hw_wifi_module_init(void)
{
    int ret = hw_register_module(&HAL_MODULE_INFO_SYM);
    return (ret == 0) ? HARDWARE_WIFI_OK : HARDWARE_WIFI_ERROR_OPERATION_FAILED;
}

const char *wifi_get_default_iface(void)
{
    return WIFI_HAL_DEFAULT_IFACE;
}
