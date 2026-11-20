// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/network/net.
 *
 * Network utilities module.
 * - Provides low-level network configuration functions.
 * - Shared by WiFi and Ethernet modules.
 */
#ifndef HARDWARE_NET_H
#define HARDWARE_NET_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get network module version string.
 * @return Version string.
 */
const char* net_get_version(void);

/**
 * @brief Get IPv4 address of specified interface.
 * @param iface_name Interface name (e.g., "eth0", "wlan0").
 * @param result Buffer to store the IP address string.
 * @return 0 on success, negative error code on failure.
 */
int net_get_ip(const char *iface_name, char *result);

/**
 * @brief Get IPv6 address of specified interface.
 * @param iface_name Interface name.
 * @param result Buffer to store the IPv6 address string.
 * @return 0 on success, negative error code on failure.
 */
int net_get_ipv6(const char *iface_name, char *result);

/**
 * @brief Set IPv4 address of specified interface.
 * @param iface Interface name.
 * @param ip IP address string to set.
 * @return 0 on success, negative error code on failure.
 */
int net_set_ip(const char *iface, const char *ip);

/**
 * @brief Get subnet mask of specified interface.
 * @param iface_name Interface name.
 * @param result Buffer to store the netmask string.
 * @return 0 on success, negative error code on failure.
 */
int net_get_netmask(const char *iface_name, char *result);

/**
 * @brief Set subnet mask of specified interface.
 * @param iface Interface name.
 * @param netmask Netmask string to set.
 * @return 0 on success, negative error code on failure.
 */
int net_set_netmask(const char *iface, const char *netmask);

/**
 * @brief Get DNS server address.
 * @param flag DNS server index (0: primary, 1: secondary).
 * @param result Buffer to store the DNS address string.
 * @return 0 on success, negative error code on failure.
 */
int net_get_dns(int flag, char *result);

/**
 * @brief Get default route (gateway) for specified interface.
 * @param iface_name Interface name.
 * @param result Buffer to store the gateway address string.
 * @return 0 on success, negative error code on failure.
 */
int net_get_route(const char *iface_name, char *result);

/**
 * @brief Get current active network interface name.
 * @param iface Buffer to store the interface name.
 * @return 0 on success, negative error code on failure.
 */
int net_get_cur_iface(char *iface);

/**
 * @brief Get MAC address of specified interface.
 * @param iface Interface name.
 * @param mac Buffer to store the MAC address string.
 * @param len Length of the buffer (should be at least 18).
 * @return 0 on success, negative error code on failure.
 */
int net_get_mac(const char *iface, char *mac, int len);

/**
 * @brief Set default gateway.
 * @param gateway Gateway address string.
 * @return 0 on success, negative error code on failure.
 */
int net_set_default_gateway(const char *gateway);

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_NET_H */
