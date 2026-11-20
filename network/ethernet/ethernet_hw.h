// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/network/ethernet.
 *
 * Ethernet module internal header.
 * - Defines internal structures for Ethernet module implementation.
 * - Contains Ethernet device context and network information structures.
 */
#ifndef HARDWARE_ETHERNET_HW_H
#define HARDWARE_ETHERNET_HW_H

#include <stdint.h>
#include <stdbool.h>
#include "hardware.h"

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================
 * Internal Structures
 *===========================================================================*/

/**
 * @brief Ethernet device internal structure.
 * @details Contains device state and network configuration information.
 */
struct ethernet_device {
    struct hw_device_t common;      /**< Common hardware device interface */

    /* Device information */
    char interface_name[32];        /**< Interface name (e.g., "eth0") */

    /* Status information */
    int status;                     /**< Interface status: 0=DOWN, 1=UP, -1=UNKNOWN */

    /* Network information */
    char ip_address[32];            /**< IPv4 address */
    char ipv6_address[64];          /**< IPv6 address */
    char mac_address[18];           /**< MAC address */
    char netmask[32];               /**< Subnet mask */
    char gateway[32];               /**< Gateway address */
    char dns[32];                   /**< DNS address */

    /* Private data */
    void *priv;                     /**< Implementation-specific data */
};

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_ETHERNET_HW_H */
