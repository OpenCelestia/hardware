// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/network/ethernet.
 *
 * Ethernet module implementation.
 * - Implements ethernet_hal_device_t methods for network operations.
 * - Provides IP/IPv6 configuration, MAC address query, and interface management.
 * - Uses net module for low-level network operations.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>

#include "ethernet.h"  /* Public header from hardware/include (includes error codes) */
#include "net/net.h"
#include "hardware.h"
#include "log.h"

#define LOG_TAG "HAL_ETHERNET"

/*===========================================================================
 * Internal Type Definitions
 *===========================================================================*/

/**
 * @brief Ethernet private data structure.
 */
typedef struct {
    char default_interface[32];       /**< Default network interface name */
} ethernet_priv_t;

/**
 * @brief Ethernet HAL context structure.
 */
typedef struct {
    ethernet_hal_device_t device;     /**< Ethernet HAL device */
    ethernet_priv_t *priv;            /**< Private data pointer */
} ethernet_hal_context_t;

/*===========================================================================
 * Internal Helper Functions
 *===========================================================================*/

/**
 * @brief Get ethernet context from common hardware device.
 * @param device Pointer to hardware device.
 * @return Pointer to ethernet context, or NULL on failure.
 */
static inline ethernet_hal_context_t *ethernet_context_from_common(struct hw_device_t *device)
{
    return (ethernet_hal_context_t *)((char *)device - offsetof(ethernet_hal_context_t, device.common));
}

/**
 * @brief Get ethernet context from ethernet HAL device.
 * @param device Pointer to ethernet HAL device.
 * @return Pointer to ethernet context, or NULL on failure.
 */
static inline ethernet_hal_context_t *ethernet_context_from_device(struct ethernet_hal_device *device)
{
    return (ethernet_hal_context_t *)((char *)device - offsetof(ethernet_hal_context_t, device));
}

/**
 * @brief Check network interface status (UP/DOWN).
 * @param iface_name Interface name to check.
 * @return 1 if UP, 0 if DOWN, -1 on error.
 */
static int check_interface_status(const char *iface_name)
{
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        return -1;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface_name, IFNAMSIZ - 1);

    int ret = -1;
    if (ioctl(sockfd, SIOCGIFFLAGS, &ifr) >= 0) {
        ret = (ifr.ifr_flags & IFF_UP) ? 1 : 0;
    }

    close(sockfd);
    return ret;
}

/**
 * @brief Set network interface UP or DOWN flag.
 * @param iface_name Interface name.
 * @param up True to bring interface up, false to bring down.
 * @return 0 on success, -1 on failure.
 */
static int ethernet_set_interface_flag(const char *iface_name, bool up)
{
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        return -1;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface_name, IFNAMSIZ - 1);

    if (ioctl(sockfd, SIOCGIFFLAGS, &ifr) < 0) {
        close(sockfd);
        return -1;
    }

    if (up) {
        ifr.ifr_flags |= IFF_UP;
    } else {
        ifr.ifr_flags &= ~IFF_UP;
    }

    int ret = (ioctl(sockfd, SIOCSIFFLAGS, &ifr) >= 0) ? 0 : -1;
    close(sockfd);
    return ret;
}

/**
 * @brief Initialize ethernet private data structure.
 * @return Pointer to initialized private data, or NULL on failure.
 */
static ethernet_priv_t *ethernet_priv_init(void)
{
    ethernet_priv_t *priv = (ethernet_priv_t *)calloc(1, sizeof(ethernet_priv_t));
    if (!priv) {
        log_error("Failed to allocate memory for ethernet private data");
        return NULL;
    }
    if (net_get_cur_iface(priv->default_interface) != 0) {
        strcpy(priv->default_interface, "eth0");
    }

    log_info("Ethernet HAL initialized, default interface: %s\n", priv->default_interface);
    return priv;
}

/**
 * @brief Deinitialize ethernet private data structure.
 * @param priv Pointer to private data to free.
 */
static void ethernet_priv_deinit(ethernet_priv_t *priv)
{
    if (priv) {
        free(priv);
    }
}

/**
 * @brief Select network interface name (use provided or default).
 * @param priv Pointer to ethernet private data.
 * @param iface_name Interface name (can be NULL to use default).
 * @return Selected interface name.
 */
static const char *ethernet_select_interface(ethernet_priv_t *priv, const char *iface_name)
{
    if (iface_name && iface_name[0] != '\0') {
        return iface_name;
    }

    if (priv->default_interface[0] == '\0') {
        strcpy(priv->default_interface, "eth0");
    }

    return priv->default_interface;
}

/**
 * @brief Get private data from ethernet device.
 * @param dev Pointer to ethernet HAL device.
 * @return Pointer to private data, or NULL on failure.
 */
static ethernet_priv_t *ethernet_get_priv(struct ethernet_hal_device *dev)
{
    ethernet_hal_context_t *ctx = ethernet_context_from_device(dev);
    return ctx ? ctx->priv : NULL;
}

/**
 * @brief Get ethernet module version string.
 * @param dev Pointer to ethernet HAL device.
 * @return Version string.
 */
static const char *ethernet_device_get_version(struct ethernet_hal_device *dev)
{
    (void)dev;
    return net_get_version();
}

/**
 * @brief Get IPv4 address of specified interface.
 * @param dev Pointer to ethernet HAL device.
 * @param iface_name Interface name (NULL to use default).
 * @param ip Output buffer for IP address string.
 * @param ip_len Length of output buffer.
 * @return HARDWARE_ETHERNET_OK on success, negative error code on failure.
 */
static int ethernet_device_get_ip(struct ethernet_hal_device *dev, const char *iface_name, char *ip, size_t ip_len)
{
    ethernet_priv_t *priv = ethernet_get_priv(dev);
    if (!priv || !ip || ip_len == 0) {
        return HARDWARE_ETHERNET_ERROR_INVALID_PARAM;
    }

    const char *iface = ethernet_select_interface(priv, iface_name);
    int ret = net_get_ip(iface, ip);
    return (ret == 0) ? HARDWARE_ETHERNET_OK : HARDWARE_ETHERNET_ERROR_OPERATION_FAILED;
}

/**
 * @brief Get IPv6 address of specified interface.
 * @param dev Pointer to ethernet HAL device.
 * @param iface_name Interface name (NULL to use default).
 * @param ipv6 Output buffer for IPv6 address string.
 * @param ipv6_len Length of output buffer.
 * @return HARDWARE_ETHERNET_OK on success, negative error code on failure.
 */
static int ethernet_device_get_ipv6(struct ethernet_hal_device *dev, const char *iface_name, char *ipv6, size_t ipv6_len)
{
    ethernet_priv_t *priv = ethernet_get_priv(dev);
    if (!priv || !ipv6 || ipv6_len == 0) {
        return HARDWARE_ETHERNET_ERROR_INVALID_PARAM;
    }

    const char *iface = ethernet_select_interface(priv, iface_name);
    int ret = net_get_ipv6(iface, ipv6);
    return (ret == 0) ? HARDWARE_ETHERNET_OK : HARDWARE_ETHERNET_ERROR_OPERATION_FAILED;
}

/**
 * @brief Set IPv4 address for specified interface.
 * @param dev Pointer to ethernet HAL device.
 * @param iface_name Interface name (NULL to use default).
 * @param ip IP address string to set.
 * @return HARDWARE_ETHERNET_OK on success, negative error code on failure.
 */
static int ethernet_device_set_ip(struct ethernet_hal_device *dev, const char *iface_name, const char *ip)
{
    ethernet_priv_t *priv = ethernet_get_priv(dev);
    if (!priv || !ip) {
        return HARDWARE_ETHERNET_ERROR_INVALID_PARAM;
    }

    const char *iface = ethernet_select_interface(priv, iface_name);
    int ret = net_set_ip(iface, ip);
    return (ret == 0) ? HARDWARE_ETHERNET_OK : HARDWARE_ETHERNET_ERROR_OPERATION_FAILED;
}

/**
 * @brief Get MAC address of specified interface.
 * @param dev Pointer to ethernet HAL device.
 * @param iface_name Interface name (NULL to use default).
 * @param mac Output buffer for MAC address string (must be at least 18 bytes).
 * @param mac_len Length of output buffer.
 * @return HARDWARE_ETHERNET_OK on success, negative error code on failure.
 */
static int ethernet_device_get_mac(struct ethernet_hal_device *dev, const char *iface_name, char *mac, size_t mac_len)
{
    ethernet_priv_t *priv = ethernet_get_priv(dev);
    if (!priv || !mac || mac_len < 18) {
        return HARDWARE_ETHERNET_ERROR_INVALID_PARAM;
    }

    const char *iface = ethernet_select_interface(priv, iface_name);
    int ret = net_get_mac(iface, mac, mac_len);

    return (ret == 0) ? HARDWARE_ETHERNET_OK : HARDWARE_ETHERNET_ERROR_OPERATION_FAILED;
}

/**
 * @brief Get subnet mask of specified interface.
 * @param dev Pointer to ethernet HAL device.
 * @param iface_name Interface name (NULL to use default).
 * @param netmask Output buffer for netmask string.
 * @param netmask_len Length of output buffer.
 * @return HARDWARE_ETHERNET_OK on success, negative error code on failure.
 */
static int ethernet_device_get_netmask(struct ethernet_hal_device *dev, const char *iface_name, char *netmask, size_t netmask_len)
{
    ethernet_priv_t *priv = ethernet_get_priv(dev);
    if (!priv || !netmask || netmask_len == 0) {
        return HARDWARE_ETHERNET_ERROR_INVALID_PARAM;
    }

    const char *iface = ethernet_select_interface(priv, iface_name);
    int ret = net_get_netmask(iface, netmask);
    return (ret == 0) ? HARDWARE_ETHERNET_OK : HARDWARE_ETHERNET_ERROR_OPERATION_FAILED;
}

/**
 * @brief Set subnet mask for specified interface.
 * @param dev Pointer to ethernet HAL device.
 * @param iface_name Interface name (NULL to use default).
 * @param netmask Netmask string to set.
 * @return HARDWARE_ETHERNET_OK on success, negative error code on failure.
 */
static int ethernet_device_set_netmask(struct ethernet_hal_device *dev, const char *iface_name, const char *netmask)
{
    ethernet_priv_t *priv = ethernet_get_priv(dev);
    if (!priv || !netmask) {
        return HARDWARE_ETHERNET_ERROR_INVALID_PARAM;
    }

    const char *iface = ethernet_select_interface(priv, iface_name);
    int ret = net_set_netmask(iface, netmask);
    return (ret == 0) ? HARDWARE_ETHERNET_OK : HARDWARE_ETHERNET_ERROR_OPERATION_FAILED;
}

/**
 * @brief Get default gateway address for specified interface.
 * @param dev Pointer to ethernet HAL device.
 * @param iface_name Interface name (NULL to use default).
 * @param gateway Output buffer for gateway address string.
 * @param gateway_len Length of output buffer.
 * @return HARDWARE_ETHERNET_OK on success, negative error code on failure.
 */
static int ethernet_device_get_gateway(struct ethernet_hal_device *dev, const char *iface_name, char *gateway, size_t gateway_len)
{
    ethernet_priv_t *priv = ethernet_get_priv(dev);
    if (!priv || !gateway || gateway_len == 0) {
        return HARDWARE_ETHERNET_ERROR_INVALID_PARAM;
    }

    const char *iface = ethernet_select_interface(priv, iface_name);
    int ret = net_get_route(iface, gateway);
    return (ret == 0) ? HARDWARE_ETHERNET_OK : HARDWARE_ETHERNET_ERROR_OPERATION_FAILED;
}

/**
 * @brief Set default gateway address.
 * @param dev Pointer to ethernet HAL device.
 * @param gateway Gateway address string to set.
 * @return HARDWARE_ETHERNET_OK on success, negative error code on failure.
 */
static int ethernet_device_set_default_gateway(struct ethernet_hal_device *dev, const char *gateway)
{
    (void)dev;
    if (!gateway) {
        return HARDWARE_ETHERNET_ERROR_INVALID_PARAM;
    }

    int ret = net_set_default_gateway(gateway);
    return (ret == 0) ? HARDWARE_ETHERNET_OK : HARDWARE_ETHERNET_ERROR_OPERATION_FAILED;
}

/**
 * @brief Get DNS server address.
 * @param dev Pointer to ethernet HAL device.
 * @param flag DNS server index (0: primary, 1: secondary).
 * @param dns Output buffer for DNS address string.
 * @param dns_len Length of output buffer.
 * @return HARDWARE_ETHERNET_OK on success, negative error code on failure.
 */
static int ethernet_device_get_dns(struct ethernet_hal_device *dev, int flag, char *dns, size_t dns_len)
{
    (void)dev;
    if (!dns || dns_len == 0) {
        return HARDWARE_ETHERNET_ERROR_INVALID_PARAM;
    }

    int ret = net_get_dns(flag, dns);
    return (ret == 0) ? HARDWARE_ETHERNET_OK : HARDWARE_ETHERNET_ERROR_OPERATION_FAILED;
}

/**
 * @brief Get current active network interface name.
 * @param dev Pointer to ethernet HAL device.
 * @param iface Output buffer for interface name.
 * @param iface_len Length of output buffer.
 * @return HARDWARE_ETHERNET_OK on success, negative error code on failure.
 */
static int ethernet_device_get_current_interface(struct ethernet_hal_device *dev, char *iface, size_t iface_len)
{
    ethernet_priv_t *priv = ethernet_get_priv(dev);
    if (!priv || !iface || iface_len == 0) {
        return HARDWARE_ETHERNET_ERROR_INVALID_PARAM;
    }

    if (net_get_cur_iface(iface) == 0) {
        return HARDWARE_ETHERNET_OK;
    }

    strncpy(iface, priv->default_interface, iface_len - 1);
    iface[iface_len - 1] = '\0';
    return HARDWARE_ETHERNET_OK;
}

/**
 * @brief Get network interface status (UP/DOWN/UNKNOWN).
 * @param dev Pointer to ethernet HAL device.
 * @param iface_name Interface name (NULL to use default).
 * @return Interface status (ETHERNET_STATUS_UP, ETHERNET_STATUS_DOWN, or ETHERNET_STATUS_UNKNOWN).
 */
static ethernet_status_t ethernet_device_get_status(struct ethernet_hal_device *dev, const char *iface_name)
{
    ethernet_priv_t *priv = ethernet_get_priv(dev);
    if (!priv || !iface_name) {
        return ETHERNET_STATUS_UNKNOWN;
    }

    const char *iface = ethernet_select_interface(priv, iface_name);
    int status = check_interface_status(iface);
    if (status == 1) {
        return ETHERNET_STATUS_UP;
    } else if (status == 0) {
        return ETHERNET_STATUS_DOWN;
    }

    return ETHERNET_STATUS_UNKNOWN;
}

/**
 * @brief Bring network interface up.
 * @param dev Pointer to ethernet HAL device.
 * @param iface_name Interface name (NULL to use default).
 * @return HARDWARE_ETHERNET_OK on success, negative error code on failure.
 */
static int ethernet_device_set_up(struct ethernet_hal_device *dev, const char *iface_name)
{
    ethernet_priv_t *priv = ethernet_get_priv(dev);
    if (!priv) {
        return HARDWARE_ETHERNET_ERROR_INVALID_PARAM;
    }

    const char *iface = ethernet_select_interface(priv, iface_name);
    int ret = ethernet_set_interface_flag(iface, true);
    if (ret != 0) {
        return HARDWARE_ETHERNET_ERROR_OPERATION_FAILED;
    }
    return HARDWARE_ETHERNET_OK;
}

/**
 * @brief Bring network interface down.
 * @param dev Pointer to ethernet HAL device.
 * @param iface_name Interface name (NULL to use default).
 * @return HARDWARE_ETHERNET_OK on success, negative error code on failure.
 */
static int ethernet_device_set_down(struct ethernet_hal_device *dev, const char *iface_name)
{
    ethernet_priv_t *priv = ethernet_get_priv(dev);
    if (!priv) {
        return HARDWARE_ETHERNET_ERROR_INVALID_PARAM;
    }

    const char *iface = ethernet_select_interface(priv, iface_name);
    int ret = ethernet_set_interface_flag(iface, false);
    if (ret != 0) {
        return HARDWARE_ETHERNET_ERROR_OPERATION_FAILED;
    }
    return HARDWARE_ETHERNET_OK;
}

/**
 * @brief Close ethernet hardware device.
 * @param device Pointer to hardware device.
 * @return HARDWARE_ETHERNET_OK on success, negative error code on failure.
 */
static int edev_close(struct hw_device_t *device)
{
    if (!device) {
        return HARDWARE_ETHERNET_ERROR_INVALID_PARAM;
    }

    ethernet_hal_context_t *ctx = ethernet_context_from_common(device);
    ethernet_priv_deinit(ctx->priv);
    free(ctx);
    return HARDWARE_ETHERNET_OK;
}

/**
 * @brief Open ethernet hardware device.
 * @param module Pointer to hardware module.
 * @param id Device interface ID.
 * @param device Output pointer to opened device handle.
 * @return HARDWARE_ETHERNET_OK on success, negative error code on failure.
 */
static int edev_open(const struct hw_module_t *module, const char *id, struct hw_device_t **device)
{
    if (!module || !id || !device) {
        return HARDWARE_ETHERNET_ERROR_INVALID_PARAM;
    }

    if (strcmp(id, ETHERNET_HARDWARE_INTERFACE) != 0) {
        return HARDWARE_ETHERNET_ERROR_INVALID_PARAM;
    }

    ethernet_hal_context_t *ctx = (ethernet_hal_context_t *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return HARDWARE_ETHERNET_ERROR_MEMORY;
    }

    ctx->priv = ethernet_priv_init();
    if (!ctx->priv) {
        free(ctx);
        return HARDWARE_ETHERNET_ERROR_MEMORY;
    }

    ethernet_hal_device_t *eth_dev = &ctx->device;
    eth_dev->common.tag = HARDWARE_DEVICE_TAG;
    eth_dev->common.version = 1;
    eth_dev->common.module = (struct hw_module_t *)module;
    eth_dev->common.close = edev_close;

    eth_dev->get_version = ethernet_device_get_version;
    eth_dev->get_ip = ethernet_device_get_ip;
    eth_dev->get_ipv6 = ethernet_device_get_ipv6;
    eth_dev->set_ip = ethernet_device_set_ip;
    eth_dev->get_mac = ethernet_device_get_mac;
    eth_dev->get_netmask = ethernet_device_get_netmask;
    eth_dev->set_netmask = ethernet_device_set_netmask;
    eth_dev->get_gateway = ethernet_device_get_gateway;
    eth_dev->set_default_gateway = ethernet_device_set_default_gateway;
    eth_dev->get_dns = ethernet_device_get_dns;
    eth_dev->get_current_interface = ethernet_device_get_current_interface;
    eth_dev->get_status = ethernet_device_get_status;
    eth_dev->set_up = ethernet_device_set_up;
    eth_dev->set_down = ethernet_device_set_down;

    *device = &eth_dev->common;
    return HARDWARE_ETHERNET_OK;
}

static struct hw_module_methods_t ethernet_module_methods = {
    .open = edev_open,
};

static struct hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = 1,
    .hal_api_version = HARDWARE_HAL_API_VERSION,
    .id = ETHERNET_HARDWARE_MODULE_ID,
    .name = "Celestia Ethernet HW HAL",
    .author = "AnyKa Celestia",
    .methods = &ethernet_module_methods,
    .dso = NULL,
    .reserved = {0}
};

/*===========================================================================
 * Public API Functions
 *===========================================================================*/

/**
 * @brief Initialize Ethernet hardware module.
 * @details Registers the Ethernet module with the hardware module system.
 * @return HARDWARE_ETHERNET_OK on success, negative error code on failure.
 */
int hw_ethernet_module_init(void)
{
    int ret = hw_register_module(&HAL_MODULE_INFO_SYM);
    return (ret == 0) ? HARDWARE_ETHERNET_OK : HARDWARE_ETHERNET_ERROR_OPERATION_FAILED;
}

