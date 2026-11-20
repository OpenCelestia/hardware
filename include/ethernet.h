// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/connectivity/ethernet.
 *
 * Ethernet module public interface.
 * - Provides Android-style HAL interface for Ethernet hardware devices.
 * - Supports IP/IPv6 configuration, MAC address query, and interface management.
 */
#ifndef HARDWARE_ETHERNET_H
#define HARDWARE_ETHERNET_H

#include <stdint.h>
#include <stddef.h>
#include "hardware.h"
#include "system_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * MODULE IDENTIFICATION
 * ============================================================================ */
#define ETHERNET_HARDWARE_MODULE_ID     "ethernet"
#define ETHERNET_HARDWARE_INTERFACE     "ethernet_hal"

/* ============================================================================
 * ETHERNET STATUS TYPES
 * ============================================================================ */

/**
 * @brief Ethernet interface status.
 */
typedef enum {
    ETHERNET_STATUS_DOWN = 0,       /**< Interface is down. */
    ETHERNET_STATUS_UP,             /**< Interface is up. */
    ETHERNET_STATUS_UNKNOWN,        /**< Interface status unknown. */
} ethernet_status_t;

/* ============================================================================
 * ETHERNET HAL DEVICE INTERFACE
 * ============================================================================ */

/**
 * @brief Ethernet HAL device structure.
 * @details Provides function pointers for Ethernet operations including
 *          IP configuration, MAC address query, and interface management.
 */
typedef struct ethernet_hal_device {
    struct hw_device_t common;

    /* Version query */
    const char *(*get_version)(struct ethernet_hal_device *dev);

    /* IP address operations */
    int (*get_ip)(struct ethernet_hal_device *dev, const char *iface_name, char *ip, size_t ip_len);
    int (*get_ipv6)(struct ethernet_hal_device *dev, const char *iface_name, char *ipv6, size_t ipv6_len);
    int (*set_ip)(struct ethernet_hal_device *dev, const char *iface_name, const char *ip);
    int (*get_mac)(struct ethernet_hal_device *dev, const char *iface_name, char *mac, size_t mac_len);

    /* Network mask operations */
    int (*get_netmask)(struct ethernet_hal_device *dev, const char *iface_name, char *netmask, size_t netmask_len);
    int (*set_netmask)(struct ethernet_hal_device *dev, const char *iface_name, const char *netmask);

    /* Gateway operations */
    int (*get_gateway)(struct ethernet_hal_device *dev, const char *iface_name, char *gateway, size_t gateway_len);
    int (*set_default_gateway)(struct ethernet_hal_device *dev, const char *gateway);

    /* DNS and interface operations */
    int (*get_dns)(struct ethernet_hal_device *dev, int flag, char *dns, size_t dns_len);
    int (*get_current_interface)(struct ethernet_hal_device *dev, char *iface, size_t iface_len);

    /* Interface status operations */
    ethernet_status_t (*get_status)(struct ethernet_hal_device *dev, const char *iface_name);
    int (*set_up)(struct ethernet_hal_device *dev, const char *iface_name);
    int (*set_down)(struct ethernet_hal_device *dev, const char *iface_name);
} ethernet_hal_device_t;

/* ============================================================================
 * CONVENIENCE FUNCTIONS
 * ============================================================================ */

/**
 * @brief Open Ethernet HAL device.
 * @param module Pointer to the hardware module.
 * @param device Output pointer to the opened device handle.
 * @return 0 on success, negative error code on failure.
 */
static inline int ethernet_hal_device_open(const struct hw_module_t* module,
                                           struct ethernet_hal_device** device)
{
    return module->methods->open(module, ETHERNET_HARDWARE_INTERFACE,
                                 TO_HW_DEVICE_T_OPEN(device));
}

/**
 * @brief Close Ethernet HAL device.
 * @param device Pointer to the Ethernet device.
 * @return 0 on success, negative error code on failure.
 */
static inline int ethernet_hal_device_close(struct ethernet_hal_device* device)
{
    return device->common.close(&device->common);
}

/**
 * @brief Initialize Ethernet hardware module.
 * @return 0 on success, negative error code on failure.
 */
int hw_ethernet_module_init(void);

/* ============================================================================
 * ETHERNET ERROR CODES
 * ============================================================================ */
#define HARDWARE_ETHERNET_OK                        SYSTEM_SUCCESS
#define HARDWARE_ETHERNET_ERROR_INVALID_PARAM       SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_ETHERNET, ERROR_INVALID_PARAM)
#define HARDWARE_ETHERNET_ERROR_NOT_INIT            SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_ETHERNET, ERROR_NOT_INIT)
#define HARDWARE_ETHERNET_ERROR_MEMORY              SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_ETHERNET, ERROR_MEMORY)
#define HARDWARE_ETHERNET_ERROR_TIMEOUT             SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_ETHERNET, ERROR_TIMEOUT)
#define HARDWARE_ETHERNET_ERROR_NOT_FOUND           SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_ETHERNET, ERROR_NOT_FOUND)
#define HARDWARE_ETHERNET_ERROR_BUSY                SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_ETHERNET, ERROR_BUSY)
#define HARDWARE_ETHERNET_ERROR_IO                  SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_ETHERNET, ERROR_IO)
#define HARDWARE_ETHERNET_ERROR_NOT_SUPPORTED       SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_ETHERNET, ERROR_NOT_SUPPORTED)
#define HARDWARE_ETHERNET_ERROR_INVALID_STATE       SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_ETHERNET, ERROR_INVALID_STATE)
#define HARDWARE_ETHERNET_ERROR_OPERATION_FAILED    SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_ETHERNET, ERROR_OPERATION_FAILED)
#define HARDWARE_ETHERNET_ERROR_PERMISSION_DENIED   SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_ETHERNET, ERROR_PERMISSION_DENIED)
#define HARDWARE_ETHERNET_ERROR_INTERFACE_NOT_FOUND SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_ETHERNET, ERROR_INTERFACE_NOT_FOUND)
#define HARDWARE_ETHERNET_ERROR_INTERFACE_DOWN      SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_ETHERNET, 0x0017)
#define HARDWARE_ETHERNET_ERROR_CONFIG_FAILED       SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_ETHERNET, 0x0018)
#define HARDWARE_ETHERNET_ERROR_NETWORK_UNAVAILABLE SYSTEM_ERROR_CODE(HARDWARE_DOMAIN, HARDWARE_MODULE_ETHERNET, 0x0019)

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_ETHERNET_H */

