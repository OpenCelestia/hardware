// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/connectivity/wifi.
 *
 * WiFi module public interface.
 * - Provides Android-style HAL interface for WiFi hardware devices.
 * - Supports STA mode, AP mode, P2P, and BLE provisioning.
 */
#ifndef HARDWARE_WIFI_H
#define HARDWARE_WIFI_H

#include <stdint.h>
#include <stddef.h>
#include "system_error.h"
#include "hardware.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * MODULE IDENTIFICATION
 * ============================================================================ */
#define WIFI_HARDWARE_MODULE_ID     "wifi"
#define WIFI_HARDWARE_INTERFACE     "wifi_hal"

/* ============================================================================
 * WIFI STATUS TYPES
 * ============================================================================ */

/**
 * @brief WiFi station connection status.
 */
typedef enum {
    WIFI_STA_STAT_IDLE = 0,         /**< Station connection disabled. */
    WIFI_STA_STAT_CONNECTING,       /**< Connecting to AP. */
    WIFI_STA_STAT_CONNECTED,        /**< Connected to AP. */
    WIFI_STA_STAT_ADDRESSED,        /**< IP address acquired. */
    WIFI_STA_STAT_PSK_FAIL,         /**< Password error. */
    WIFI_STA_STAT_AP_NOT_FOUND,     /**< AP not found. */
} wifi_hal_status_t;

/**
 * @brief WiFi connection status callback function type.
 * @param status Current WiFi connection status.
 * @return 0 on success, negative error code on failure.
 */
typedef int (*wifi_connect_status_cb_t)(wifi_hal_status_t status);

/**
 * @brief WiFi scan result information structure.
 */
typedef struct {
    char ssid[33];                  /**< SSID (max 32 chars + null). */
    char bssid[18];                 /**< BSSID (MAC address). */
    int rssi;                       /**< Signal strength (dBm). */
    int frequency;                  /**< Operating frequency (MHz). */
    char flags[128];                /**< Security flags (e.g., [WPA2-PSK-CCMP]). */
} wifi_scan_result_t;

#define WIFI_SCAN_MAX_RESULTS 64    /**< Maximum number of scan results. */

/* ============================================================================
 * P2P TYPES
 * ============================================================================ */

/**
 * @brief P2P device information structure.
 */
typedef struct {
    char device_name[33];           /**< Device name. */
    char device_address[18];        /**< MAC address. */
    int rssi;                       /**< Signal strength. */
    int frequency;                  /**< Operating frequency. */
} wifi_p2p_device_info_t;

/**
 * @brief P2P group information structure.
 */
typedef struct {
    char group_ssid[33];            /**< Group SSID. */
    char group_passphrase[64];      /**< Group passphrase. */
    char group_owner_address[18];   /**< Group owner MAC address. */
    int group_id;                   /**< Group ID. */
} wifi_p2p_group_info_t;

/* ============================================================================
 * WIFI HAL DEVICE INTERFACE
 * ============================================================================ */

/**
 * @brief WiFi HAL device structure.
 * @details Provides function pointers for WiFi operations including
 *          STA mode, AP mode, P2P, and BLE provisioning.
 */
typedef struct wifi_hw_device {
    struct hw_device_t common;
    
    /* STA mode operations */
    int (*load_config)(struct wifi_hw_device *dev, const char *cfg_path);
    int (*sta_scan)(struct wifi_hw_device *dev);
    int (*sta_get_scan_results)(struct wifi_hw_device *dev, wifi_scan_result_t *results, int *num_results);
    int (*sta_connect)(struct wifi_hw_device *dev, const char *ssid, const char *passphrase, int wait_ms);
    int (*sta_disconnect)(struct wifi_hw_device *dev);
    wifi_hal_status_t (*sta_status)(struct wifi_hw_device *dev);
    int (*set_connect_status_cb)(struct wifi_hw_device *dev, wifi_connect_status_cb_t cb);

    /* AP mode operations */
    int (*ap_start)(struct wifi_hw_device *dev);
    int (*ap_stop)(struct wifi_hw_device *dev);
    int (*ap_set_config)(struct wifi_hw_device *dev, const char *ssid, const char *pwd);
    int (*ap_get_config)(struct wifi_hw_device *dev, char *ssid, char *pwd);

    /* P2P operations */
    int (*p2p_discover_peers)(struct wifi_hw_device *dev);
    int (*p2p_stop_discover_peers)(struct wifi_hw_device *dev);
    int (*p2p_get_discovered_peers)(struct wifi_hw_device *dev, wifi_p2p_device_info_t *devices, int *num_devices);
    int (*p2p_connect)(struct wifi_hw_device *dev, const char *device_address);
    int (*p2p_disconnect)(struct wifi_hw_device *dev);
    int (*p2p_create_group)(struct wifi_hw_device *dev);
    int (*p2p_remove_group)(struct wifi_hw_device *dev);
    int (*p2p_is_group_owner)(struct wifi_hw_device *dev);
    int (*p2p_get_group_info)(struct wifi_hw_device *dev, wifi_p2p_group_info_t *info);

    /* BLE provisioning (app pairing). Optional; NULL means not supported. */
    int (*ble_start)(struct wifi_hw_device *dev);
    int (*ble_stop)(struct wifi_hw_device *dev);
    int (*ble_is_ready)(struct wifi_hw_device *dev);
} wifi_hw_device_t;

/* ============================================================================
 * CONVENIENCE FUNCTIONS
 * ============================================================================ */

/**
 * @brief Open WiFi HAL device.
 * @param module Pointer to the hardware module.
 * @param device Output pointer to the opened device handle.
 * @return 0 on success, negative error code on failure.
 */
static inline int wifi_hw_device_open(const struct hw_module_t* module,
                                       struct wifi_hw_device** device)
{
    return module->methods->open(module, WIFI_HARDWARE_INTERFACE,
                                 TO_HW_DEVICE_T_OPEN(device));
}

/**
 * @brief Close WiFi HAL device.
 * @param device Pointer to the WiFi device.
 * @return 0 on success, negative error code on failure.
 */
static inline int wifi_hw_device_close(struct wifi_hw_device* device)
{
    return device->common.close(&device->common);
}

/**
 * @brief Initialize WiFi hardware module.
 * @return 0 on success, negative error code on failure.
 */
int hw_wifi_module_init(void);

/**
 * @brief Get WiFi HAL default network interface name.
 * @details Backend is selected at compile time (e.g. aic8800/vnet0, wpa/wlan0).
 * @return Non-NULL interface name (static storage, do not free).
 */
const char *wifi_get_default_iface(void);

/* ============================================================================
 * WIFI ERROR CODES
 * ============================================================================ */
#define HARDWARE_WIFI_OK                        SYSTEM_SUCCESS
#define HARDWARE_WIFI_ERROR_INVALID_PARAM       SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_WIFI, ERROR_INVALID_PARAM)
#define HARDWARE_WIFI_ERROR_NOT_INIT            SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_WIFI, ERROR_NOT_INIT)
#define HARDWARE_WIFI_ERROR_MEMORY              SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_WIFI, ERROR_MEMORY)
#define HARDWARE_WIFI_ERROR_TIMEOUT             SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_WIFI, ERROR_TIMEOUT)
#define HARDWARE_WIFI_ERROR_NOT_FOUND           SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_WIFI, ERROR_NOT_FOUND)
#define HARDWARE_WIFI_ERROR_BUSY                SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_WIFI, ERROR_BUSY)
#define HARDWARE_WIFI_ERROR_IO                  SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_WIFI, ERROR_IO)
#define HARDWARE_WIFI_ERROR_NOT_SUPPORTED       SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_WIFI, ERROR_NOT_SUPPORTED)
#define HARDWARE_WIFI_ERROR_INVALID_STATE       SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_WIFI, ERROR_INVALID_STATE)
#define HARDWARE_WIFI_ERROR_OPERATION_FAILED    SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_WIFI, ERROR_OPERATION_FAILED)
#define HARDWARE_WIFI_ERROR_PERMISSION_DENIED   SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_WIFI, ERROR_PERMISSION_DENIED)
#define HARDWARE_WIFI_ERROR_INTERFACE_NOT_FOUND SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_WIFI, ERROR_INTERFACE_NOT_FOUND)
#define HARDWARE_WIFI_ERROR_INTERFACE_DOWN      SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_WIFI, 0x0017)
#define HARDWARE_WIFI_ERROR_CONFIG_FAILED       SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_WIFI, 0x0018)
#define HARDWARE_WIFI_ERROR_NETWORK_UNAVAILABLE SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_WIFI, 0x0019)

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_WIFI_H */

