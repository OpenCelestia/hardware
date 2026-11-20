// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/network/wifi.
 *
 * AIC8800 WiFi HAL backend.
 * - Uses custom_msg private ioctl (SIOCDEVPRIVATE+2) on vnet0 and /dev/aic_cdev fasync (SIGIO).
 * - For scan results, driver may provide fasync MSG 3024 and/or ioctl cmd 19 (SCAN-RESULT:...).
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>

#include "aic8800_port.h"
#include "wifi_hw.h"
#include "net/net.h"
#include "log.h"

#define LOG_TAG "HAL WIFI AIC"

/*===========================================================================
 * Types (ActionCam aic_wifi_ctrl_port.c)
 *===========================================================================*/

typedef struct android_wifi_priv_cmd {
	char *buf;
	int used_len;
	int total_len;
} android_wifi_priv_cmd_t;

typedef struct rwnx_fasync_info {
	int mem_status;
	char mem[AIC_CDEV_BUF_MAX];
} rwnx_fasync_info_t;

/*===========================================================================
 * fasync / seqlock state
 *===========================================================================*/

static int s_fd = -1;
static volatile int s_run;
static volatile int s_fasync_ready; /* 1 only after SIGIO/FASYNC armed */
static pthread_t s_tid;

static volatile sig_atomic_t s_msg_seq;
static char s_msg_last[AIC_CDEV_BUF_MAX];
static int s_msg_ind;
static char s_msg_ip[16];
static char s_msg_gw[16];
static char s_msg_mk[16];
static volatile sig_atomic_t s_msg_raw_pending;

static volatile sig_atomic_t s_scan_seq;
static char s_scan_list[AIC_SCAN_LIST_MAX];

static char s_active_ifname[IFNAMSIZ];

static int aic_drv_ioctl(const char *ifname, char *cmd_buf);

/*===========================================================================
 * Parse MSG (ActionCam + mk from patched ko)
 *===========================================================================*/

static unsigned int aic_hex8_to_uint(const char *str)
{
	unsigned int v = 0;
	int i;

	for (i = 7; i >= 0; i--) {
		unsigned char c = (unsigned char)str[i];
		if (c >= '0' && c <= '9')
			c = (unsigned char)(c - '0');
		else if (c >= 'A' && c <= 'F')
			c = (unsigned char)(c - 'A' + 10);
		else if (c >= 'a' && c <= 'f')
			c = (unsigned char)(c - 'a' + 10);
		else
			c = 0;
		v |= (unsigned int)c << (4 * (7 - i));
	}
	return v;
}

static int aic_extract_msg_ind(const char *msg)
{
	const char *p;

	if (!msg)
		return 0;
	p = strstr(msg, "MSG:");
	if (!p)
		return 0;
	p += 4;
	while (*p == ' ')
		p++;
	return atoi(p);
}

/* ActionCam parse_ip_gw: ip at "ip"+4, gw at ip+18 when present */
static void aic_parse_ip_gw(const char *msg, char ip_out[16], char gw_out[16])
{
	const char *p;
	size_t len;

	ip_out[0] = gw_out[0] = '\0';
	if (!msg)
		return;
	len = strlen(msg);
	p = strstr(msg, "ip");
	if (!p || (size_t)(p - msg) + 4u + 8u > len)
		return;

	{
		unsigned int ip = aic_hex8_to_uint(p + 4);
		snprintf(ip_out, 16, "%u.%u.%u.%u",
		         (ip >> 0) & 0xFF, (ip >> 8) & 0xFF,
		         (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
	}
	if ((size_t)(p - msg) + 18u + 8u <= len) {
		unsigned int gw = aic_hex8_to_uint(p + 18);
		snprintf(gw_out, 16, "%u.%u.%u.%u",
		         (gw >> 0) & 0xFF, (gw >> 8) & 0xFF,
		         (gw >> 16) & 0xFF, (gw >> 24) & 0xFF);
	}
}

static void aic_parse_ip_gw_mk(const char *msg, char ip_out[16], char gw_out[16], char mk_out[16])
{
	const char *p_mk;
	char buff[9];
	unsigned int mk;

	ip_out[0] = gw_out[0] = mk_out[0] = '\0';
	if (!msg)
		return;

	aic_parse_ip_gw(msg, ip_out, gw_out);

	p_mk = strstr(msg, "mk");
	if (!p_mk || strlen(p_mk) < 12)
		return;
	memset(buff, 0, sizeof(buff));
	memcpy(buff, p_mk + 4, 8);
	mk = aic_hex8_to_uint(buff);
	snprintf(mk_out, 16, "%u.%u.%u.%u",
	         (mk >> 0) & 0xFF, (mk >> 8) & 0xFF,
	         (mk >> 16) & 0xFF, (mk >> 24) & 0xFF);
}

/*===========================================================================
 * netif (ActionCam wifi_netif.c → HAL net/*)
 *===========================================================================*/

static void aic_netif_apply(const char *ifname, const char *ip, const char *mk, const char *gw)
{
	const char *mask = (mk && mk[0]) ? mk : "255.255.255.0";
	char cmd[192];
	/* Last successfully-applied tuple. aic_poll_sta_status() re-enters here on every
	 * STA-status poll while connected (s_msg_ind stays STA_GOT_IP), which re-ran
	 * `route del default` + `route add` each poll — spamming "File exists" and churning
	 * the default route (can disrupt the NFS/eth route). Re-apply only when something
	 * actually changed. */
	static char s_last_if[32];
	static char s_last_ip[16];
	static char s_last_gw[16];

	if (!ifname || !ifname[0] || !ip || !ip[0])
		return;

	if (strncmp(s_last_if, ifname, sizeof(s_last_if)) == 0 &&
	    strncmp(s_last_ip, ip, sizeof(s_last_ip)) == 0 &&
	    strncmp(s_last_gw, gw ? gw : "", sizeof(s_last_gw)) == 0) {
		return; /* unchanged — already applied, nothing to do */
	}

	if (net_set_ip(ifname, ip) != 0)
		log_warn("net_set_ip %s %s failed", ifname, ip);
	if (net_set_netmask(ifname, mask) != 0)
		log_warn("net_set_netmask %s %s failed", ifname, mask);
	if (gw && gw[0]) {
		/* Replace any existing default route for THIS iface only (idempotent), then add. */
		snprintf(cmd, sizeof(cmd), "route del default gw %s dev %s 2>/dev/null", gw, ifname);
		(void)system(cmd);
		snprintf(cmd, sizeof(cmd), "route add default gw %s dev %s", gw, ifname);
		if (system(cmd) != 0)
			log_warn("route add default gw %s dev %s failed", gw, ifname);
	}

	/* Cache the applied tuple so subsequent identical polls are no-ops. */
	snprintf(s_last_if, sizeof(s_last_if), "%s", ifname);
	snprintf(s_last_ip, sizeof(s_last_ip), "%s", ip);
	snprintf(s_last_gw, sizeof(s_last_gw), "%s", gw ? gw : "");
}

static void aic_maybe_apply_netif_from_fasync(void)
{
	static sig_atomic_t last_seen;
	sig_atomic_t a = s_msg_seq;
	char ip[16], gw[16], mk[16];
	int ind;
	const char *ifname;

	if (a == last_seen || (a & 1))
		return;

	for (;;) {
		sig_atomic_t s1 = s_msg_seq;
		if (s1 & 1)
			continue;
		ind = s_msg_ind;
		strncpy(ip, s_msg_ip, sizeof(ip) - 1);
		ip[sizeof(ip) - 1] = '\0';
		strncpy(gw, s_msg_gw, sizeof(gw) - 1);
		gw[sizeof(gw) - 1] = '\0';
		strncpy(mk, s_msg_mk, sizeof(mk) - 1);
		mk[sizeof(mk) - 1] = '\0';
		if (s_msg_seq != s1)
			continue;
		last_seen = s1;
		break;
	}

	if ((ind != AIC_MSG_STA_GOT_IP && ind != AIC_MSG_AP_GOT_IP) || !ip[0])
		return;

	ifname = s_active_ifname[0] ? s_active_ifname : WIFI_HAL_DEFAULT_IFACE;
	aic_netif_apply(ifname, ip, mk, gw);
}

/*===========================================================================
 * Scan list helpers (fasync 3024 + ioctl SCAN-RESULT:)
 *===========================================================================*/

static int aic_ioctl_reply_is_param_err(const char *s)
{
	if (!s || !s[0])
		return 0;
	return (strstr(s, "CFM-MSG:") != NULL && strstr(s, "param err") != NULL) ||
	       (strstr(s, "cmd-id") != NULL && strstr(s, "invaild") != NULL);
}

static void aic_scan_list_clear(void)
{
	s_scan_list[0] = '\0';
}

static void aic_scan_list_set(const char *payload)
{
	size_t plen;

	if (!payload)
		return;
	if (aic_ioctl_reply_is_param_err(payload)) {
		log_warn("scan: driver param err (need luna aic8800_netdrv.ko with cmd %d)",
		         AIC_CMD_GET_SCAN_RESULTS);
		return;
	}
	if (strncmp(payload, "empty", 5) == 0) {
		s_scan_list[0] = '\0';
		return;
	}
	plen = strlen(payload);
	if (plen >= AIC_SCAN_LIST_MAX)
		plen = AIC_SCAN_LIST_MAX - 1;
	memcpy(s_scan_list, payload, plen);
	s_scan_list[plen] = '\0';
}

static void aic_scan_list_append_fasync(const char *msg)
{
	const char *p;
	const char *prefix = "scan_wifi_list ssid=";
	size_t plen, cur;

	if (!msg)
		return;
	p = strstr(msg, prefix);
	if (!p) {
		p = strstr(msg, "scan_wifi_list");
		if (p) {
			p = strchr(p, '=');
			if (p)
				p++;
		}
	} else {
		p += strlen(prefix);
	}
	if (!p || !*p)
		return;

	plen = strlen(p);
	if (plen >= AIC_SCAN_LIST_MAX)
		plen = AIC_SCAN_LIST_MAX - 1;

	cur = strlen(s_scan_list);
	if (cur > 0 && cur < AIC_SCAN_LIST_MAX - 1) {
		s_scan_list[cur] = ';';
		s_scan_list[cur + 1] = '\0';
		cur++;
	}
	memcpy(s_scan_list + cur, p, plen);
	s_scan_list[cur + plen] = '\0';
}

static int aic_scan_pull_ioctl(const char *ifname)
{
	char buf[AIC_MAX_DRV_CMD_SIZE];
	const char *payload;
	const char *prefix = "SCAN-RESULT:";

	memset(buf, 0, sizeof(buf));
	snprintf(buf, sizeof(buf), "%d", AIC_CMD_GET_SCAN_RESULTS);
	if (aic_drv_ioctl(ifname, buf) != HARDWARE_WIFI_OK)
		return HARDWARE_WIFI_ERROR_IO;

	payload = strstr(buf, prefix);
	if (!payload) {
		if (aic_ioctl_reply_is_param_err(buf))
			log_warn("scan ioctl %d: ko lacks APP_CMD_GET_SCAN_WIFI_RESULTS", AIC_CMD_GET_SCAN_RESULTS);
		else
			log_debug("scan ioctl: no SCAN-RESULT in reply");
		return HARDWARE_WIFI_OK;
	}
	payload += strlen(prefix);
	aic_scan_list_set(payload);
	if (s_scan_list[0])
		log_info("scan ioctl: %u byte(s)", (unsigned)strlen(s_scan_list));
	else
		log_info("scan ioctl: empty list");
	return HARDWARE_WIFI_OK;
}

static int aic_scan_wait_fasync(int timeout_ms)
{
	int elapsed = 0;
	size_t last_len = 0;
	int idle_ms = 0;

	while (elapsed < timeout_ms) {
		sig_atomic_t a = s_scan_seq;
		size_t len;

		if (a & 1) {
			usleep(10000);
			elapsed += 10;
			continue;
		}
		len = strlen(s_scan_list);
		if (len > 0) {
			if (len == last_len) {
				idle_ms += 50;
				if (idle_ms >= 400)
					return HARDWARE_WIFI_OK;
			} else {
				last_len = len;
				idle_ms = 0;
			}
		}
		usleep(50000);
		elapsed += 50;
	}
	return HARDWARE_WIFI_ERROR_TIMEOUT;
}

/*===========================================================================
 * fasync thread (ActionCam aic_wifi_evt_start)
 *===========================================================================*/

static void aic_signal_handler(int signum)
{
	char data_buf[sizeof(rwnx_fasync_info_t)];
	rwnx_fasync_info_t *info = (rwnx_fasync_info_t *)data_buf;
	int ret;

	(void)signum;
	if (s_fd < 0)
		return;

	ret = (int)read(s_fd, data_buf, sizeof(rwnx_fasync_info_t));
	if (ret < 0)
		return;

	info->mem[AIC_CDEV_BUF_MAX - 1] = '\0';
	info->mem_status = 0;
	(void)write(s_fd, &info->mem_status, sizeof(info->mem_status));

	/*
	 * SIGIO handler must be async-signal-safe.
	 * Only copy raw message and mark it pending; parse in the fasync thread.
	 */
	s_msg_seq++;
	memcpy(s_msg_last, info->mem, sizeof(s_msg_last));
	s_msg_last[AIC_CDEV_BUF_MAX - 1] = '\0';
	s_msg_raw_pending = 1;
	s_msg_seq++;
}

static void aic_process_fasync_message(void)
{
	static sig_atomic_t last_seen_seq;
	sig_atomic_t a;
	char msg[AIC_CDEV_BUF_MAX];
	int ind;

	a = s_msg_seq;
	if (a == last_seen_seq || (a & 1) || !s_msg_raw_pending)
		return;

	/* seqlock: copy raw message consistently */
	for (;;) {
		sig_atomic_t s1 = s_msg_seq;
		if (s1 & 1)
			continue;
		memcpy(msg, s_msg_last, sizeof(msg));
		if (s_msg_seq != s1)
			continue;
		last_seen_seq = s1;
		break;
	}
	s_msg_raw_pending = 0;

	ind = aic_extract_msg_ind(msg);
	s_msg_ind = ind;

	if (ind == AIC_MSG_STA_GOT_IP || ind == AIC_MSG_AP_GOT_IP)
		aic_parse_ip_gw_mk(msg, s_msg_ip, s_msg_gw, s_msg_mk);
	else if (ind == AIC_MSG_STA_CONNECTING)
		aic_parse_ip_gw(msg, s_msg_ip, s_msg_gw);
	else if (ind == AIC_MSG_STA_DISCONNECT) {
		s_msg_ip[0] = '\0';
		s_msg_gw[0] = '\0';
		s_msg_mk[0] = '\0';
	} else if (ind == AIC_MSG_SCAN_AP) {
		s_scan_seq++;
		aic_scan_list_append_fasync(msg);
		s_scan_seq++;
	}
}

static void *aic_fasync_thread(void *arg)
{
	int wait_logged = 0;
	int fd = -1;

	(void)arg;

	/*
	 * aic8800_netdrv is loaded in background by app.sh (after ak_mci).
	 * /dev/aic_cdev and vnet0 appear several seconds after luna_main starts.
	 * Retry open instead of exiting — otherwise STA connect can succeed via
	 * ioctl but GOT_IP / DHCP events are never received (fasync dead).
	 *
	 * Important: do not publish s_fd / s_fasync_ready until FASYNC is armed,
	 * or a concurrent STA connect can miss STA_GOT_IP.
	 */
	while (s_run && fd < 0) {
		fd = open(AIC_CDEV_PATH, O_RDWR);
		if (fd >= 0)
			break;
		if (!wait_logged) {
			log_warn("waiting for %s (%s)", AIC_CDEV_PATH, strerror(errno));
			wait_logged = 1;
		}
		sleep(1);
	}
	if (!s_run) {
		if (fd >= 0)
			close(fd);
		return NULL;
	}
	if (fd < 0)
		return NULL;

	signal(SIGIO, aic_signal_handler);
	(void)fcntl(fd, F_SETOWN, getpid());
	{
		int flag = fcntl(fd, F_GETFL);
		(void)fcntl(fd, F_SETFL, flag | FASYNC);
	}

	s_fd = fd;
	s_fasync_ready = 1;
	log_info("opened %s (fasync armed)", AIC_CDEV_PATH);

	while (s_run) {
		aic_process_fasync_message();
		aic_maybe_apply_netif_from_fasync();
		sleep(1);
	}

	s_fasync_ready = 0;
	close(s_fd);
	s_fd = -1;
	return NULL;
}

/*===========================================================================
 * ioctl (ActionCam wifi_send_cmd_to_net_interface)
 *===========================================================================*/

static int aic_netif_if_up(const char *ifname)
{
	struct ifreq ifr;
	int sock;

	if (!ifname || !ifname[0])
		return HARDWARE_WIFI_ERROR_INVALID_PARAM;

	sock = socket(PF_INET, SOCK_DGRAM, 0);
	if (sock < 0)
		return HARDWARE_WIFI_ERROR_IO;

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	if (ioctl(sock, SIOCGIFFLAGS, &ifr) != 0) {
		close(sock);
		return HARDWARE_WIFI_ERROR_IO;
	}
	if (!(ifr.ifr_flags & IFF_UP)) {
		ifr.ifr_flags |= (IFF_UP | IFF_RUNNING);
		if (ioctl(sock, SIOCSIFFLAGS, &ifr) != 0) {
			close(sock);
			return HARDWARE_WIFI_ERROR_IO;
		}
	}
	close(sock);
	return HARDWARE_WIFI_OK;
}

static int aic_drv_ioctl(const char *ifname, char *cmd_buf)
{
	struct ifreq ifr;
	int sock;
	android_wifi_priv_cmd_t priv_cmd;

	if (!ifname || !ifname[0] || !cmd_buf)
		return HARDWARE_WIFI_ERROR_INVALID_PARAM;

	strncpy(s_active_ifname, ifname, sizeof(s_active_ifname) - 1);
	s_active_ifname[sizeof(s_active_ifname) - 1] = '\0';

	if (aic_netif_if_up(ifname) != HARDWARE_WIFI_OK)
		return HARDWARE_WIFI_ERROR_IO;

	sock = socket(PF_INET, SOCK_DGRAM, 0);
	if (sock < 0)
		return HARDWARE_WIFI_ERROR_IO;

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	if (ioctl(sock, SIOCGIFFLAGS, &ifr) != 0 || !(ifr.ifr_flags & IFF_UP)) {
		close(sock);
		return HARDWARE_WIFI_ERROR_IO;
	}

	memset(&priv_cmd, 0, sizeof(priv_cmd));
	priv_cmd.buf = cmd_buf;
	priv_cmd.used_len = (int)strlen(cmd_buf);
	priv_cmd.total_len = AIC_MAX_DRV_CMD_SIZE;
	ifr.ifr_data = (void *)&priv_cmd;

	if (ioctl(sock, SIOCDEVPRIVATE + 2, &ifr) < 0) {
		close(sock);
		return HARDWARE_WIFI_ERROR_IO;
	}
	if (ifr.ifr_data)
		memcpy(&priv_cmd, ifr.ifr_data, sizeof(priv_cmd));
	close(sock);
	return HARDWARE_WIFI_OK;
}

/*===========================================================================
 * STA state from fasync
 *===========================================================================*/

static int aic_poll_sta_status(const char *ifname, int *out_sta_status)
{
	char ip[16], gw[16], mk[16];
	int ind;

	if (!out_sta_status || !ifname || !ifname[0])
		return HARDWARE_WIFI_ERROR_INVALID_PARAM;

	/* Ensure the latest SIGIO raw message is parsed. */
	aic_process_fasync_message();

	for (;;) {
		sig_atomic_t a = s_msg_seq;
		if (a & 1)
			continue;
		ind = s_msg_ind;
		strncpy(ip, s_msg_ip, sizeof(ip) - 1);
		ip[sizeof(ip) - 1] = '\0';
		strncpy(gw, s_msg_gw, sizeof(gw) - 1);
		gw[sizeof(gw) - 1] = '\0';
		strncpy(mk, s_msg_mk, sizeof(mk) - 1);
		mk[sizeof(mk) - 1] = '\0';
		if (s_msg_seq != a)
			continue;
		break;
	}

	switch (ind) {
	case AIC_MSG_STA_CONNECTING:
	case AIC_MSG_CONNECT_CFM:
		*out_sta_status = WIFI_STA_STAT_CONNECTING;
		break;
	case AIC_MSG_STA_GOT_IP:
		aic_netif_apply(ifname, ip, mk, gw);
		*out_sta_status = WIFI_STA_STAT_ADDRESSED;
		break;
	case AIC_MSG_AP_GOT_IP:
		aic_netif_apply(ifname, ip, mk, gw);
		*out_sta_status = WIFI_STA_STAT_CONNECTED;
		break;
	case AIC_MSG_PSK_FAIL:
		*out_sta_status = WIFI_STA_STAT_PSK_FAIL;
		break;
	case AIC_MSG_STA_DISCONNECT:
		*out_sta_status = WIFI_STA_STAT_IDLE;
		break;
	default:
		break;
	}
	return HARDWARE_WIFI_OK;
}

/*===========================================================================
 * HAL backend ops
 *===========================================================================*/

static int aic_init(void)
{
	if (s_run)
		return HARDWARE_WIFI_OK;
	s_fasync_ready = 0;
	s_run = 1;
	if (pthread_create(&s_tid, NULL, aic_fasync_thread, NULL) != 0) {
		s_run = 0;
		return HARDWARE_WIFI_ERROR_IO;
	}
	return HARDWARE_WIFI_OK;
}

static void aic_deinit(void)
{
	if (!s_run)
		return;
	s_run = 0;
	pthread_join(s_tid, NULL);
	s_fasync_ready = 0;
}

static int aic_sta_connect(wifi_hw_runtime_t *rt)
{
	char buf[AIC_MAX_DRV_CMD_SIZE];
	int ret;

	if (!rt || !rt->ifname[0] || !rt->sta_ssid[0])
		return HARDWARE_WIFI_ERROR_INVALID_PARAM;

	/* Do not associate before fasync is armed — would miss STA_GOT_IP. */
	if (!s_fasync_ready || s_fd < 0)
		return HARDWARE_WIFI_ERROR_IO;

	/* Already associating: do not re-issue CONNECT (breaks in-flight assoc). */
	if (rt->sta_status == WIFI_STA_STAT_CONNECTING) {
		rt->link_request = 0;
		return HARDWARE_WIFI_OK;
	}

	snprintf(buf, sizeof(buf), "%d %s %s", AIC_CMD_CONNECT, rt->sta_ssid, rt->sta_pwd);
	log_info("STA connect: %s", rt->sta_ssid);
	ret = aic_drv_ioctl(rt->ifname, buf);
	if (ret != HARDWARE_WIFI_OK)
		return ret;

	rt->link_request = 0;
	rt->sta_status = WIFI_STA_STAT_CONNECTING;
	if (rt->set_sta_status)
		rt->set_sta_status(rt, WIFI_STA_STAT_CONNECTING);
	return HARDWARE_WIFI_OK;
}

static int aic_service(wifi_hw_runtime_t *rt)
{
	if (!rt)
		return 200;

	if (rt->link_request == 1) {
		int ret = aic_sta_connect(rt);
		/* Driver/iface not ready yet — back off to avoid log/ioctl spam. */
		if (ret != HARDWARE_WIFI_OK)
			return 1000;
	}

	{
		int st = rt->sta_status;
		if (aic_poll_sta_status(rt->ifname, &st) == HARDWARE_WIFI_OK &&
		    st != rt->sta_status) {
			rt->sta_status = st;
			if (rt->set_sta_status)
				rt->set_sta_status(rt, st);
		}
	}
	return 200;
}

static int aic_sta_disconnect(wifi_hw_runtime_t *rt)
{
    char buf[AIC_MAX_DRV_CMD_SIZE];

	if (!rt)
		return HARDWARE_WIFI_ERROR_INVALID_PARAM;
    memset(buf, 0, sizeof(buf));
	snprintf(buf, sizeof(buf), "%d", AIC_CMD_DISCONNECT);
	return aic_drv_ioctl(rt->ifname, buf);
}

static int aic_ap_start(const char *ifname, const char *ssid, const char *pwd)
{
	char buf[AIC_MAX_DRV_CMD_SIZE];

	if (!ssid)
		return HARDWARE_WIFI_ERROR_INVALID_PARAM;
	snprintf(buf, sizeof(buf), "%d %s %s 2.4G", AIC_CMD_START_AP, ssid, pwd ? pwd : "");
	return aic_drv_ioctl(ifname, buf);
}

static int aic_ap_stop(const char *ifname)
{
    char buf[AIC_MAX_DRV_CMD_SIZE];

    memset(buf, 0, sizeof(buf));
	snprintf(buf, sizeof(buf), "%d", AIC_CMD_STOP_AP);
	return aic_drv_ioctl(ifname, buf);
}

static int aic_parse_scan_entry(const char *entry, wifi_scan_result_t *r)
{
	char ssid[33];
	char bssid[18];
	int rssi = 0, ch = 0, akm = 0;
	int n;

	if (!entry || !*entry || !r)
		return 0;
	memset(r, 0, sizeof(*r));

	/* fasync MSG 3024 */
	n = sscanf(entry, "ssid=%32[^,],rssi=%d,ch=%d,akm=%u,bssid=%17s",
	           ssid, &rssi, &ch, &akm, bssid);
	if (n >= 2) {
		strncpy(r->ssid, ssid, sizeof(r->ssid) - 1);
		if (n >= 5)
			strncpy(r->bssid, bssid, sizeof(r->bssid) - 1);
		r->rssi = rssi;
		r->frequency = ch;
		snprintf(r->flags, sizeof(r->flags), "akm=%d", akm);
		return 1;
	}

	/* ioctl cmd 19: aicwf_format_scan_results — ssid,rssi=...,ch=...,akm=...,bssid=... */
	n = sscanf(entry, "%32[^,],rssi=%d,ch=%d,akm=%u,bssid=%17s",
	           ssid, &rssi, &ch, &akm, bssid);
	if (n >= 2) {
		strncpy(r->ssid, ssid, sizeof(r->ssid) - 1);
		if (n >= 5)
			strncpy(r->bssid, bssid, sizeof(r->bssid) - 1);
		r->rssi = rssi;
		r->frequency = ch;
		snprintf(r->flags, sizeof(r->flags), "akm=%d", akm);
		return 1;
	}

	return 0;
}

static int aic_parse_scan_list(wifi_scan_result_t *results, int max_results, int *num_results)
{
	char list_copy[AIC_SCAN_LIST_MAX];
	char *save = NULL;
	char *tok;
	int n = 0;

	if (!results || !num_results || max_results <= 0)
		return HARDWARE_WIFI_ERROR_INVALID_PARAM;

	*num_results = 0;
	if (!s_scan_list[0])
		return HARDWARE_WIFI_OK;

	strncpy(list_copy, s_scan_list, sizeof(list_copy) - 1);
	list_copy[sizeof(list_copy) - 1] = '\0';

	tok = strtok_r(list_copy, ";", &save);
	while (tok && n < max_results) {
		while (*tok == ' ' || *tok == '\t')
			tok++;
		if (*tok && aic_parse_scan_entry(tok, &results[n]))
			n++;
		tok = strtok_r(NULL, ";", &save);
	}

	*num_results = n;
	return HARDWARE_WIFI_OK;
}

static int aic_get_scan_results(wifi_scan_result_t *results, int *num_results)
{
	int max_results;
	int ret;
	sig_atomic_t a;

	if (!results || !num_results || *num_results <= 0)
		return HARDWARE_WIFI_ERROR_INVALID_PARAM;

	max_results = *num_results;
	*num_results = 0;

	for (;;) {
		a = s_scan_seq;
		if (a & 1)
			continue;
		ret = aic_parse_scan_list(results, max_results, num_results);
		if (s_scan_seq != a)
			continue;
		break;
	}

	if (*num_results == 0 && s_scan_list[0])
		log_warn("scan list present but unparsed: %.120s", s_scan_list);

	return ret;
}

static int aic_scan(const char *ifname)
{
    char buf[AIC_MAX_DRV_CMD_SIZE];
	int ret;
	char saved_fasync[AIC_SCAN_LIST_MAX];

	if (!ifname || !ifname[0])
		return HARDWARE_WIFI_ERROR_INVALID_PARAM;

	aic_scan_list_clear();
    memset(buf, 0, sizeof(buf));
	snprintf(buf, sizeof(buf), "%d", AIC_CMD_SCAN_WIFI);
	ret = aic_drv_ioctl(ifname, buf);
	if (ret != HARDWARE_WIFI_OK)
		return ret;

	usleep((useconds_t)AIC_SCAN_MCU_WAIT_MS * 1000);

	saved_fasync[0] = '\0';
	(void)aic_scan_wait_fasync(AIC_SCAN_FASYNC_WAIT_MS);
	if (s_scan_list[0]) {
		strncpy(saved_fasync, s_scan_list, sizeof(saved_fasync) - 1);
		saved_fasync[sizeof(saved_fasync) - 1] = '\0';
	}

	/* Luna ko: read kernel scan cache (authoritative if non-empty) */
	(void)aic_scan_pull_ioctl(ifname);
	if (!s_scan_list[0] && saved_fasync[0]) {
		strncpy(s_scan_list, saved_fasync, sizeof(s_scan_list) - 1);
		s_scan_list[sizeof(s_scan_list) - 1] = '\0';
	}

	if (!s_scan_list[0])
		log_warn("scan: empty (insmod luna aic8800_netdrv.ko; check dmesg MCU-SCAN-WIFI)");

	return HARDWARE_WIFI_OK;
}

const wifi_backend_ops_t wifi_backend_aic8800_ops = {
	.name = "aic8800",
	.init = aic_init,
	.deinit = aic_deinit,
	.service = aic_service,
	.sta_connect = aic_sta_connect,
	.sta_disconnect = aic_sta_disconnect,
	.ap_start = aic_ap_start,
	.ap_stop = aic_ap_stop,
	.scan = aic_scan,
	.get_scan_results = aic_get_scan_results,
};

const wifi_backend_ops_t *wifi_backend_get(void)
{
	return &wifi_backend_aic8800_ops;
}
