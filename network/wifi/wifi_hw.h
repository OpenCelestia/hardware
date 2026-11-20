// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * WiFi HAL internal header (single entry for wifi_hw.c and driver backends).
 *
 * Layout:
 *   wifi_hw.c              — HAL device API + wifi_backend_get()
 *   aic8800/               — AIC8800 (custom_msg + aic_cdev), CONFIG_WIFI_TYPE=aic8800*
 *   wpa/                   — WPA/nl80211 backend
 *     wifi_backend_wpa.c
 *     wpa_ctrl/            — wpa_supplicant control socket (WPA build only)
 *     utils/               — wpa_supplicant helper sources (WPA build only)
 *
 * Backend is chosen at compile time from CONFIG_WIFI_TYPE (see Makefile).
 */
#ifndef HARDWARE_WIFI_HW_H
#define HARDWARE_WIFI_HW_H

#include <stdint.h>
#include <stdbool.h>
#include "hardware.h"
#include "wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================
 * Compile-time backend (from -DWIFI_HAL_BACKEND_* in Makefile)
 *===========================================================================*/

#if defined(WIFI_HAL_BACKEND_AIC8800)
#define WIFI_HAL_USE_AIC8800 1
#elif defined(WIFI_HAL_BACKEND_UWLAN)
#define WIFI_HAL_USE_UWLAN 1
#elif defined(WIFI_HAL_BACKEND_WPA)
#define WIFI_HAL_USE_WPA 1
#else
#define WIFI_HAL_USE_WPA 1
#endif

#ifndef WIFI_HAL_DEFAULT_IFACE
#if WIFI_HAL_USE_AIC8800
#define WIFI_HAL_DEFAULT_IFACE "vnet0"
#elif WIFI_HAL_USE_UWLAN
#define WIFI_HAL_DEFAULT_IFACE "/dev/wifi"
#else
#define WIFI_HAL_DEFAULT_IFACE "wlan0"
#endif
#endif

/*===========================================================================
 * Driver backend ops (wifi_hw.c ↔ aic8800/ or wpa/)
 *===========================================================================*/

/** Snapshot passed into backend service(); caller holds module mutex. */
typedef struct wifi_hw_runtime {
    char ifname[32];
    char sta_ssid[32];
    char sta_pwd[64];
    char ap_ssid[32];
    char ap_pwd[64];
    char sta_bssid[64];
    int  sta_freq_khz;
    int  link_request;
    int  sta_status;
    int  wifi_mode;
    /** Backend-private state (owned by backend). */
    void *backend_priv;
    void (*set_sta_status)(struct wifi_hw_runtime *rt, int status);
} wifi_hw_runtime_t;

typedef struct wifi_backend_ops {
    const char *name;
    int (*init)(void);
    void (*deinit)(void);
    /** Worker loop; returns sleep ms before next iteration. */
    int (*service)(wifi_hw_runtime_t *rt);
    /** Immediate connect request; backend-specific. */
    int (*sta_connect)(wifi_hw_runtime_t *rt);
    int (*sta_disconnect)(wifi_hw_runtime_t *rt);
    int (*ap_start)(const char *ifname, const char *ssid, const char *pwd);
    int (*ap_stop)(const char *ifname);
    int (*scan)(const char *ifname);
    int (*get_scan_results)(wifi_scan_result_t *results, int *num_results);
} wifi_backend_ops_t;

const wifi_backend_ops_t *wifi_backend_get(void);

/*===========================================================================
 * Module-internal API (wifi_hw.c implementation)
 *===========================================================================*/

int hal_wifi_cfg_write(void);

/*===========================================================================
 * Legacy internal device struct (retained for compatibility)
 *===========================================================================*/

struct wifi_device {
    struct hw_device_t common;
    char interface_name[32];
    int wifi_mode;
    int sta_status;
    int ap_status;
    char sta_ssid[32];
    char sta_passphrase[64];
    char ap_ssid[32];
    char ap_passphrase[64];
    char ip_address[32];
    char mac_address[18];
    char netmask[32];
    char gateway[32];
    char dns[32];
    void *priv;
};

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_WIFI_HW_H */
