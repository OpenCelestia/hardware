// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/network/net.
 *
 * Network utilities implementation.
 * - Provides low-level network configuration using Linux socket API.
 * - Shared by WiFi and Ethernet modules.
 */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <net/if.h>
#include <net/route.h>
#include <arpa/inet.h>
#include <sys/types.h>
#if !defined(KERNEL_AOS)
#include <ifaddrs.h>
#endif
#include <dirent.h>
#include <stddef.h>
#include <limits.h>

#include "system_core.h"
#include "net.h"
#include "log.h"

#define LOG_TAG "NET"

/*===========================================================================
 * Internal Type Definitions
 *===========================================================================*/

#define ifreq_offset(x) offsetof(struct ifreq, x)

/**
 * @brief Network option argument structure.
 */
struct arg1opt {
    const char *name;                /**< Command name */
    unsigned short selector;         /**< IOCTL command selector */
    unsigned short ifr_offset;       /**< Struct offset */
};

/*===========================================================================
 * Internal Constants
 *===========================================================================*/

static const char net_version[] = "libapp_net V1.0.00";

/**
 * @brief Network set/get option argument table.
 */
static struct arg1opt net_opt_arg[] = {
    /* get */
    { "GIFADDR",    SIOCGIFADDR,    ifreq_offset(ifr_addr) },     // Get address
    { "GIFNETMASK", SIOCGIFNETMASK, ifreq_offset(ifr_netmask) }, // Get netmask
    /* set */
    { "SIFADDR",    SIOCSIFADDR,    ifreq_offset(ifr_addr) },    // Set address
    { "SIFNETMASK", SIOCSIFNETMASK, ifreq_offset(ifr_netmask) },  // Set netmask
};

/** Same iface+errno: log_error only on 1st failure and every Nth thereafter (state unchanged spam). */
#define NET_GET_IOCTL_ERR_LOG_EVERY 30

static struct {
	char name[IFNAMSIZ];
	int errnum;
	unsigned count;
} s_net_ioctl_err_st[8];

/*===========================================================================
 * Public API Functions
 *===========================================================================*/

/**
 * @brief Get network module version string.
 * @return Version string.
 */
const char* net_get_version(void)
{
	return net_version;
}

/*===========================================================================
 * Internal Helper Functions
 *===========================================================================*/

/**
 * @brief Get network address or netmask using ioctl.
 * @param iface Interface name.
 * @param opt Option string ("GIFADDR" or "GIFNETMASK").
 * @param result Output buffer for result.
 * @return 0 on success, -1 on failure.
 */
static int net_get(const char *iface, const char *opt, char *result)
{
	if (!iface || !opt || !result) {
		log_error("Invalid argument");
		return SYSTEM_FAILED;
	}

    int ret = SYSTEM_FAILED;
	int sockfd = -1;
	int match_flag = SYSTEM_false;
	int ti;
	struct arg1opt *argp = NULL;

	/* find option */
	for (argp = net_opt_arg; argp->name; argp++) {
		if (!strcmp(opt, argp->name)) {
		    match_flag = SYSTEM_true;
		    break;
		}
	}

	if (match_flag) {
	    /* create socket */
    	sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    	if (sockfd < 0) {
    		log_error("socket: %s", strerror(errno));
    		goto net_get_end;
    	}

    	/* variable define */
    	struct ifreq ifr;
    	struct sockaddr_in s_addr;

    	/* set interface name */
    	strncpy(ifr.ifr_name, iface, IFNAMSIZ);
    	ifr.ifr_name[IFNAMSIZ - 1] = 0;

    	/* do get by ioctl */
    	if (ioctl(sockfd, argp->selector, &ifr)) {
    		int err = errno;
    		/* Expected on WiFi-only boards / iface without address — do not spam. */
    		if (err == EADDRNOTAVAIL || err == ENODEV || err == ENXIO) {
    			log_debug("ioctl: SIOC%s, %s, iface: %s",
    					argp->name, strerror(err), iface);
    		} else {
    			int si;
    			int slot = -1;
    			for (si = 0; si < (int)(sizeof(s_net_ioctl_err_st) / sizeof(s_net_ioctl_err_st[0])); si++) {
    				if (s_net_ioctl_err_st[si].name[0] != '\0'
    				    && strncmp(s_net_ioctl_err_st[si].name, iface, IFNAMSIZ) == 0
    				    && s_net_ioctl_err_st[si].errnum == err) {
    					slot = si;
    					break;
    				}
    			}
    			if (slot < 0) {
    				for (si = 0; si < (int)(sizeof(s_net_ioctl_err_st) / sizeof(s_net_ioctl_err_st[0])); si++) {
    					if (s_net_ioctl_err_st[si].name[0] == '\0') {
    						slot = si;
    						break;
    					}
    				}
    			}
    			if (slot < 0)
    				slot = 0;
    			strncpy(s_net_ioctl_err_st[slot].name, iface, IFNAMSIZ - 1);
    			s_net_ioctl_err_st[slot].name[IFNAMSIZ - 1] = '\0';
    			s_net_ioctl_err_st[slot].errnum = err;
    			s_net_ioctl_err_st[slot].count++;
    			if ((s_net_ioctl_err_st[slot].count % NET_GET_IOCTL_ERR_LOG_EVERY) == 1u)
    				log_error("ioctl: SIOC%s, %s, iface: %s",
    					  argp->name, strerror(err), iface);
    		}
    		goto net_get_end;
    	}

    	/* clean out s_addr */
    	memset(&s_addr, 0, sizeof(s_addr));
    	/* store result */
    	memcpy(&s_addr, ((char *)&ifr) + argp->ifr_offset, sizeof(s_addr));
    	/* format convert */
    	char *addr = inet_ntoa(s_addr.sin_addr);
    	if (result)
    		strcpy(result, addr);

		/* address updated: reset failure counters for this iface */
		for (ti = 0; ti < (int)(sizeof(s_net_ioctl_err_st) / sizeof(s_net_ioctl_err_st[0])); ti++) {
			if (s_net_ioctl_err_st[ti].name[0] != '\0'
			    && strncmp(s_net_ioctl_err_st[ti].name, iface, IFNAMSIZ) == 0)
				memset(&s_net_ioctl_err_st[ti], 0, sizeof(s_net_ioctl_err_st[ti]));
		}

        ret = SYSTEM_SUCCESS;
    	//ak_print_debug_ex(MODULE_ID_NET, "get addr: %s\n", addr);
	} else {
	    log_warn("unsupport opt: %s", opt);
	}

net_get_end:
	if (sockfd != -1)
		close(sockfd);

	return ret;
}

/**
 * @brief Set network address or netmask using ioctl.
 * @param iface Interface name.
 * @param opt Option string ("SIFADDR" or "SIFNETMASK").
 * @param arg Value to set (IP address or netmask string).
 * @return 0 on success, -1 on failure.
 */
static int net_set(const char *iface, const char *opt, const char *arg)
{
	if (!iface || !opt || !arg) {
		log_error("Invalid argument");
		return SYSTEM_FAILED;
	}

    int ret = SYSTEM_FAILED;
	int sockfd = -1;
    int match_flag = SYSTEM_false;
	struct arg1opt *argp = NULL;

	/* find option */
	for (argp = net_opt_arg; argp->name; argp++) {
		if (!strcmp(opt, argp->name)) {
            match_flag = SYSTEM_true;
            break;
		}
	}

	if (match_flag) {
	    /* create socket */
    	sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    	if (sockfd < 0) {
    		log_error("socket: %s", strerror(errno));
    		goto net_set_end;
    	}

    	/* variable define */
    	struct ifreq ifr;
    	struct sockaddr_in s_addr;

    	memset(&s_addr, 0, sizeof(s_addr));
    	/* check whether arg is valid */
    	ret = inet_aton(arg, &s_addr.sin_addr);
    	if (!ret) {
    		log_error("invalid arg: %s", arg);
    		goto net_set_end;
    	}

    	/* set socket struct */
    	s_addr.sin_port = 0;
    	s_addr.sin_family = AF_INET;

    	/* set interface name */
    	strncpy(ifr.ifr_name, iface, IFNAMSIZ);
    	ifr.ifr_name[IFNAMSIZ - 1] = 0;

    	memcpy(((char *)&ifr) + argp->ifr_offset, &s_addr,
    	    sizeof(struct sockaddr));

    	/* do set by ioctl */
    	if (ioctl(sockfd, argp->selector, &ifr)) {
    		log_error("ioctl: SIOC%s, %s, iface: %s",
    				argp->name, strerror(errno), iface);
    	} else {
            ret = SYSTEM_SUCCESS;
    	    //ak_print_debug_ex(MODULE_ID_NET, "set addr: %s ok\n", arg);
    	}
	} else {
        log_warn("unsupport opt: %s", opt);
	}

net_set_end:
	if (sockfd != -1)
		close(sockfd);

	return ret;
}
/**
 * @brief Get IPv6 address of specified interface.
 * @param iface_name Interface name.
 * @param result Output buffer for IPv6 address string.
 * @return 0 on success, -1 on failure.
 */
int net_get_ipv6(const char *iface_name, char *result)
{
#if defined(KERNEL_AOS)
	(void)iface_name;
	if (result != NULL)
		result[0] = '\0';
	return -1;
#else
	struct ifaddrs *ifaddr, *ifa;

    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        return -1;
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr != NULL && ifa->ifa_addr->sa_family == AF_INET6) {
            struct sockaddr_in6 *addr = (struct sockaddr_in6 *)ifa->ifa_addr;
            char ip[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, &(addr->sin6_addr), ip, INET6_ADDRSTRLEN);
            log_debug("Interface: %s, IPv6 Address: %s", ifa->ifa_name, ip);
			if(strcmp(iface_name,ifa->ifa_name) == 0)
			{
				strcpy(result,ip);
			}
        }
    }

    freeifaddrs(ifaddr);

    return 0;
#endif
}

/**
 * @brief Get IPv4 address of specified interface.
 * @param iface_name Interface name.
 * @param result Output buffer for IP address string.
 * @return 0 on success, -1 on failure.
 */
int net_get_ip(const char *iface_name, char *result)
{
	return net_get(iface_name, "GIFADDR", result);
}

/**
 * @brief Set IPv4 address for specified interface.
 * @param iface Interface name.
 * @param ip IP address string to set.
 * @return 0 on success, -1 on failure.
 */
int net_set_ip(const char *iface, const char *ip)
{
	return net_set(iface, "SIFADDR", ip);
}

/**
 * @brief Get subnet mask of specified interface.
 * @param iface_name Interface name.
 * @param result Output buffer for netmask string.
 * @return 0 on success, -1 on failure.
 */
int net_get_netmask(const char *iface_name, char *result)
{
	return net_get(iface_name, "GIFNETMASK", result);
}

/**
 * @brief Set subnet mask for specified interface.
 * @param iface Interface name.
 * @param netmask Netmask string to set.
 * @return 0 on success, -1 on failure.
 */
int net_set_netmask(const char *iface, const char *netmask)
{
	return net_set(iface, "SIFNETMASK", netmask);
}

/**
 * @brief Get DNS server address.
 * @param flag DNS server index (0: primary, 1: secondary).
 * @param result Output buffer for DNS address string.
 * @return 0 on success, -1 on failure.
 */
int net_get_dns(int flag, char *result)
{
	FILE *f = fopen("/etc/resolv.conf", "r");
	if (!f) {
		log_error("fopen: %s", strerror(errno));
		return SYSTEM_FAILED;
	}

	int ret = SYSTEM_FAILED;
	int match = 0;
	char *pdns = NULL;
	char dns[32] = {0}, line[1024] = {0};

	while (!feof(f)) {
		memset(line, 0x00, sizeof(line));
		pdns = fgets(line, sizeof(line), f);
		if (!pdns)
			goto get_dns_end;

		/* skip invalid characters */
		if (!strstr(pdns, "nameserver")) {
			log_debug("skip invalid line, %s", pdns);
			continue;
		} else {
			//ak_print_info_ex(MODULE_ID_NET, "get dns info: %s\n", pdns);
			break;
		}
	}

	/* main */
	switch (flag) {
	case 0:
	    match = sscanf(line, "%*s %s", dns);
	    break;
	case 1:
	    memset(line, 0x00, sizeof(line));
		pdns = fgets(line, 1024, f);
		match = 0;
		if (!pdns) /* no new line */
			goto get_dns_end;
		match = sscanf(line, "%*s %s", dns);
	    break;
	default:
	    break;
	}

get_dns_end:
	fclose(f);
	if (1 == match) {
	    ret = SYSTEM_SUCCESS;
		pdns = dns;
		if (result)
			strcpy(result, pdns);
	}

	return ret;
}

/**
 * @brief Get default gateway address for specified interface.
 * @param iface_name Interface name.
 * @param result Output buffer for gateway address string.
 * @return 0 on success, -1 on failure.
 */
int net_get_route(const char *iface_name, char *result)
{
	if (!iface_name) {
		log_error("Invalid argument");
		return SYSTEM_FAILED;
	}

	FILE *f = fopen("/proc/net/route", "r");
	if (!f) {
		log_error("fopen: %s", strerror(errno));
		return SYSTEM_FAILED;
	}

	char line[1024] = {0}, iface[20] = {0}, route[64] = {0};
	char *ret_buff;
	/* strip first identification */
	ret_buff = fgets(line, sizeof(line), f);

	while(ret_buff){   // Read each line until matching interface found or end of file		/* get second line */
		memset(line, 0x00, sizeof(line));
		ret_buff = fgets(line, sizeof(line), f);

		int ret = sscanf(line, "%s %*s %s", iface, route);
		if (ret != 2)
			log_warn("match failed");
		//ak_print_info(MODULE_ID_NET, "route: %s\n", route);
		

		/* compare iface name */
		if (strncmp(iface, iface_name, strlen(iface_name)) == 0) {
			break;
		}
		usleep(10*1000);
	}
	fclose(f);
	if(ret_buff == NULL)return SYSTEM_FAILED;	/* un match interface name */
	

	long long int val;
	char *endptr;

	val = strtoll(route, &endptr, 16);
	if ((errno == ERANGE && (val == LONG_MAX || val == LONG_MIN))
			|| (errno != 0 && val == 0)) {
		log_error("strtoll: %s, route: %s", strerror(errno), route);
		return SYSTEM_FAILED;
	}
	if (endptr == route) {
		log_error("NO digits were found");
		return SYSTEM_FAILED;
	}
	if (*endptr != '\0')
		log_warn("Further characters after number: %s", endptr);

	struct in_addr inp = {.s_addr = val};
	char *proute = inet_ntoa(inp);

	if (proute && result) {
		strcpy(result, proute);
	}

	return SYSTEM_SUCCESS;
}

/**
 * @brief Get current active network interface name.
 * @param iface Output buffer for interface name.
 * @return 0 on success, -1 on failure.
 */
int net_get_cur_iface(char *iface)
{
	const char *sys_net_entry = "/sys/class/net/";
	DIR *dir = opendir(sys_net_entry);

	struct dirent dp, *dp_result;
	int ret = SYSTEM_FAILED;
	char ip[16] = {0};

	do {
		ret = readdir_r(dir, &dp, &dp_result);
		/* if read success, and dp is not . or .. , do test */
		if (!ret && strcmp(dp.d_name, ".") && strcmp(dp.d_name, "..") &&
				strcmp(dp.d_name, "lo")) {
			bzero(ip, 16);
			net_get_ip(dp.d_name, ip);
			if (strlen(ip)) {
				if (iface)
					strncpy(iface, dp.d_name, strlen(dp.d_name));
				log_debug("current use iface: %s", dp.d_name);
				ret = SYSTEM_SUCCESS;
			}
		}
	} while ((!ret) && (dp_result != NULL));

	closedir(dir);

	return ret;
}

/**
 * @brief Get MAC address of specified interface.
 * @param iface Interface name.
 * @param mac Output buffer for MAC address string (must be at least 18 bytes).
 * @param len Length of output buffer.
 * @return 0 on success, -1 on failure.
 */
int net_get_mac(const char *iface, char *mac, int len)
{
	if (!mac || len < 18) {
		log_error("invalid arg: macaddr: %p, len: %d", mac, len);
		return SYSTEM_FAILED;
	}

	/* create socket to INET kernel */
	int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd == -1) {
		log_error("socket: %s", strerror(errno));
		return SYSTEM_FAILED;
	}

	/* variable define */
	int ret = SYSTEM_FAILED;
	struct ifreq req;

	/* clean up req and set interface name */
	memset(&req, 0x00, sizeof(req));
	strncpy(req.ifr_name, iface, IFNAMSIZ - 1);

	/* do get mac addr */
	if (ioctl(sockfd, SIOCGIFHWADDR, &req) >= 0) {
	    ret = SYSTEM_SUCCESS;
		snprintf(mac, len, "%02x:%02x:%02x:%02x:%02x:%02x",
				(unsigned char)req.ifr_hwaddr.sa_data[0],
				(unsigned char)req.ifr_hwaddr.sa_data[1],
				(unsigned char)req.ifr_hwaddr.sa_data[2],
				(unsigned char)req.ifr_hwaddr.sa_data[3],
				(unsigned char)req.ifr_hwaddr.sa_data[4],
				(unsigned char)req.ifr_hwaddr.sa_data[5]);
		//ak_print_info_ex(MODULE_ID_NET, "mac addr: %s\n", mac);
	}

	/* clean up resource */
	if (sockfd != -1)
		close(sockfd);

	return ret;
}

/**
 * @brief Set default gateway address.
 * @param gateway Gateway address string to set.
 * @return 0 on success, -1 on failure.
 */
int net_set_default_gateway(const char *gateway)
{
	if (!gateway) {
		log_error("invalid args");
		return SYSTEM_FAILED;
	}

	/*
	 * It use two step:
	 * 1. delete old default gateway
	 * 2. add new gateway for default
	 */
	int ret = SYSTEM_FAILED;
	int sockfd = -1;
	char rt_buf[sizeof(struct rtentry)] = {0};
	struct rtentry *const rt = (void *)rt_buf;

	/* default destination addr */
	struct sockaddr_in *sin = (struct sockaddr_in *)&rt->rt_dst;
	sin->sin_addr.s_addr = INADDR_ANY;
	sin->sin_port = 0;
	sin->sin_family = AF_INET;

	/* set gateway addr */
	sin = (struct sockaddr_in *)&rt->rt_gateway;
	sin->sin_port = 0;
	sin->sin_family = AF_INET;
	/* check whether gateway is valid */
	if (!inet_aton(gateway, &(sin->sin_addr))) {
		log_error("invalid gateway addr: %s", gateway);
		goto set_gateway_end;
	}

	/* set route flags */
	rt->rt_flags |= (RTF_UP | RTF_GATEWAY);

	/* create socket to INET kernel */
	sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd < 0) {
		log_error("socket: %s", strerror(errno));
		goto set_gateway_end;
	}

	/* do delete route */
	int r = ioctl(sockfd, SIOCDELRT, rt);
	if (r != 0 && errno != ESRCH) {
		log_error("SIOCDELRT failed: %s", strerror(errno));
		goto set_gateway_end;
	}

	/* do add route */
	if (ioctl(sockfd, SIOCADDRT, rt)) {
		log_error("SIOCADDRT failed: %s", strerror(errno));
	} else {
	    ret = SYSTEM_SUCCESS;
	}

set_gateway_end:
	if (sockfd != -1)
		close(sockfd);

	return ret;
}
