// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/network/wifi.
 *
 * WPA / nl80211 WiFi backend.
 * - Uses wpa_supplicant and the wpa_ctrl control socket to manage STA connectivity.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

#include "wifi_hw.h"
#include "wpa_ctrl.h"
#include "net/net.h"
#include "log.h"
#include "system_core.h"
#include "system_thread.h"

#define LOG_TAG "HAL WIFI WPA"
#define WPA_STA_CTRL_DIR  "/var/run/wpa_supplicant"
#define WPA_STA_CONN_CFG  "/etc/config/wpa_supplicant.conf"
#define WPA_UDHCPC_SCRIPT "/usr/sbin/udhcpc_wlan.sh"

#define KILL_SYSCALL(__proc) \
    SYSCALL("killall -9 \"%s\" > /dev/null", __proc)

#define SYNTAX_ASSOCIATE_WITH                 "Associated with "
#define SYNTAX_WPA_4WAY_HANDSHAKE_FAILED      "WPA: 4-Way Handshake failed - "
#define SYNTAX_AUTHENTICATION_WITH            "Authentication with "
#define SYNTAX_CTRL_EVENT_CONNECTED           "CTRL-EVENT-CONNECTED - Connection to "
#define SYNTAX_CTRL_EVENT_DISCONNECTED        "CTRL-EVENT-DISCONNECTED"
#define SYNTAX_CTRL_EVENT_BSS_REMOVE          "CTRL-EVENT-BSS-REMOVED"
#define SYNTAX_FREQ                           "freq="
#define SYNTAX_PRE_SHARED_KEY_MAY_BE_INCORRECT "pre-shared key may be incorrect"

typedef struct {
    char ipaddr[32];
    char netmask[32];
    char gateway[32];
    char dns[32];
} wpa_net_info_t;

static wpa_net_info_t s_net;
static int s_ping_boot_done;
static struct wpa_ctrl *s_ctrl;
static char s_scan_ifname[32] = WIFI_HAL_DEFAULT_IFACE;

/** Command-only wpa_ctrl session (separate from s_ctrl event monitor). */
static int wpa_ctrl_run_cmd(const char *ifname, const char *cmd,
    char *reply, size_t reply_size)
{
	char sta_ctrl[64];
	struct wpa_ctrl *ctrl;
	size_t len;

	if (!ifname || !ifname[0] || !cmd || !reply || reply_size < 2)
		return -1;

	snprintf(sta_ctrl, sizeof(sta_ctrl), "%s/%s", WPA_STA_CTRL_DIR, ifname);
	ctrl = wpa_ctrl_open(sta_ctrl);
	if (!ctrl)
		return -1;

	len = reply_size - 1;
	if (wpa_ctrl_request(ctrl, cmd, strlen(cmd), reply, &len, NULL) < 0) {
		wpa_ctrl_close(ctrl);
		return -1;
	}
	reply[len] = '\0';
	wpa_ctrl_close(ctrl);
	return 0;
}

static char *wpa_trim_line(char *line)
{
	char *end;

	if (!line)
		return line;
	while (*line == ' ' || *line == '\t')
		line++;
	end = line + strlen(line);
	while (end > line && (end[-1] == '\r' || end[-1] == '\n' ||
		end[-1] == ' ' || end[-1] == '\t')) {
		end--;
	}
	*end = '\0';
	return line;
}

static int wpa_parse_scan_line(char *line, wifi_scan_result_t *out)
{
	char *fields[5];
	int i;
	char *p;

	if (!line || !out)
		return 0;
	line = wpa_trim_line(line);
	if (line[0] == '\0')
		return 0;
	if (strncmp(line, "bssid", 5) == 0)
		return 0;

	memset(out, 0, sizeof(*out));
	p = line;
	for (i = 0; i < 5; i++) {
		fields[i] = p;
		p = strchr(p, '\t');
		if (!p) {
			if (i < 4)
				return 0;
			break;
		}
		*p++ = '\0';
	}
	if (i < 4)
		return 0;

	strncpy(out->bssid, fields[0], sizeof(out->bssid) - 1);
	out->frequency = atoi(fields[1]);
	out->rssi = atoi(fields[2]);
	strncpy(out->flags, fields[3], sizeof(out->flags) - 1);
	if (i >= 4 && fields[4])
		strncpy(out->ssid, fields[4], sizeof(out->ssid) - 1);

	return (out->ssid[0] != '\0') ? 1 : 0;
}

static int wpa_scan_merge_result(wifi_scan_result_t *list, int n, int max,
    const wifi_scan_result_t *item)
{
	int i;

	if (!list || !item || max <= 0)
		return n;
	if (item->ssid[0] == '\0')
		return n;

	for (i = 0; i < n; i++) {
		if (strcmp(list[i].ssid, item->ssid) == 0) {
			if (item->rssi > list[i].rssi)
				list[i] = *item;
			return n;
		}
	}
	if (n >= max)
		return n;
	list[n] = *item;
	return n + 1;
}

static void wpa_handle_result(wifi_hw_runtime_t *rt, char *pResult)
{
    char *str;
    int iTmp;
    char cTmp[128];

    if (!rt || !pResult)
        return;

    if (0 == strncmp(pResult, SYNTAX_ASSOCIATE_WITH, strlen(SYNTAX_ASSOCIATE_WITH))) {
        pResult += strlen(SYNTAX_ASSOCIATE_WITH);
        memset(rt->sta_bssid, 0, sizeof(rt->sta_bssid));
        strncpy(rt->sta_bssid, pResult, 17);
        str = strstr(pResult, SYNTAX_FREQ);
        if (str) {
            str += strlen(SYNTAX_FREQ);
            rt->sta_freq_khz = atoi(str);
        }
        if (rt->set_sta_status)
            rt->set_sta_status(rt, WIFI_STA_STAT_CONNECTING);
    } else if (0 == strncmp(pResult, SYNTAX_AUTHENTICATION_WITH, strlen(SYNTAX_AUTHENTICATION_WITH))) {
        pResult += strlen(SYNTAX_AUTHENTICATION_WITH);
        if (0 == strncmp(pResult, rt->sta_bssid, strlen(rt->sta_bssid))) {
            pResult += strlen(rt->sta_bssid) + 1;
            if (0 == strncmp(pResult, "timed out", strlen("timed out")) && rt->set_sta_status)
                rt->set_sta_status(rt, WIFI_STA_STAT_PSK_FAIL);
        }
    } else if (0 == strncmp(pResult, SYNTAX_CTRL_EVENT_DISCONNECTED, strlen(SYNTAX_CTRL_EVENT_DISCONNECTED))) {
        pResult += strlen(SYNTAX_CTRL_EVENT_DISCONNECTED) + 1 + strlen("bssid=");
        if (0 == strncmp(pResult, rt->sta_bssid, strlen(rt->sta_bssid))) {
            pResult += strlen(rt->sta_bssid) + 1;
            if (0 == strncmp(pResult, "reason=", strlen("reason="))) {
                pResult += strlen("reason=");
                if (0 == atoi(pResult) && rt->set_sta_status)
                    rt->set_sta_status(rt, WIFI_STA_STAT_PSK_FAIL);
            }
        }
    } else if (0 == strncmp(pResult, SYNTAX_WPA_4WAY_HANDSHAKE_FAILED,
                            strlen(SYNTAX_WPA_4WAY_HANDSHAKE_FAILED))) {
        pResult += strlen(SYNTAX_WPA_4WAY_HANDSHAKE_FAILED);
        if (strcmp(pResult, SYNTAX_PRE_SHARED_KEY_MAY_BE_INCORRECT) && rt->set_sta_status)
            rt->set_sta_status(rt, WIFI_STA_STAT_PSK_FAIL);
    } else if (0 == strncmp(pResult, SYNTAX_CTRL_EVENT_BSS_REMOVE, strlen(SYNTAX_CTRL_EVENT_BSS_REMOVE))) {
        pResult += strlen(SYNTAX_CTRL_EVENT_BSS_REMOVE);
        if (strlen(rt->sta_bssid) > 0 && sscanf(pResult, "%d %s", &iTmp, cTmp) == 2) {
            if (strcmp(rt->sta_bssid, cTmp) && rt->set_sta_status)
                rt->set_sta_status(rt, WIFI_STA_STAT_AP_NOT_FOUND);
        }
    } else if (0 == strncmp(pResult, SYNTAX_CTRL_EVENT_CONNECTED, strlen(SYNTAX_CTRL_EVENT_CONNECTED))) {
        if (rt->set_sta_status)
            rt->set_sta_status(rt, WIFI_STA_STAT_CONNECTED);
        (void)hal_wifi_cfg_write();
    }
}

static int wpa_supplicant_stop(wifi_hw_runtime_t *rt)
{
    (void)rt;
    if (s_ctrl) {
        wpa_ctrl_detach(s_ctrl);
        wpa_ctrl_close(s_ctrl);
        s_ctrl = NULL;
    }
    KILL_SYSCALL("wpa_supplicant");
    KILL_SYSCALL("udhcpc");
    SYSCALL("killall udhcpd");
    SYSCALL("killall hostapd");
    return 0;
}

static int wpa_wait_ctrl_socket(const char *sta_ctrl, int timeout_ms)
{
    struct stat st;
    int waited = 0;

    if (!sta_ctrl || !sta_ctrl[0])
        return -1;
    while (waited < timeout_ms) {
        if (stat(sta_ctrl, &st) == 0 && S_ISSOCK(st.st_mode))
            return 0;
        usleep(100 * 1000);
        waited += 100;
    }
    return -1;
}

static int wpa_supplicant_start(wifi_hw_runtime_t *rt)
{
    char text[1024];
    int iTextlen;
    char sta_ctrl[64];
    const char *key_mgmt;
    FILE *fp;
    size_t nw;

    if (!rt)
        return -1;
    if (rt->sta_ssid[0] == '\0') {
        log_error("wpa start aborted: empty ssid");
        return -1;
    }

    wpa_supplicant_stop(rt);

    /* Keep key_mgmt simple — broad lists can prevent association on some APs. */
    key_mgmt = (rt->sta_pwd[0] == '\0') ? "NONE" : "WPA-PSK";
    if (rt->sta_pwd[0] != '\0') {
        iTextlen = snprintf(text, sizeof(text),
                            "ctrl_interface=%s\n"
                            "update_config=1\n"
                            "network={\n"
                            "\tssid=\"%s\"\n"
                            "\tscan_ssid=1\n"
                            "\tkey_mgmt=%s\n"
                            "\tpsk=\"%s\"\n"
                            "}\n",
                            WPA_STA_CTRL_DIR, rt->sta_ssid, key_mgmt, rt->sta_pwd);
    } else {
        iTextlen = snprintf(text, sizeof(text),
                            "ctrl_interface=%s\n"
                            "update_config=1\n"
                            "network={\n"
                            "\tssid=\"%s\"\n"
                            "\tscan_ssid=1\n"
                            "\tkey_mgmt=%s\n"
                            "}\n",
                            WPA_STA_CTRL_DIR, rt->sta_ssid, key_mgmt);
    }
    if (iTextlen <= 0 || iTextlen >= (int)sizeof(text)) {
        log_error("wpa conf snprintf failed len=%d", iTextlen);
        return -1;
    }

    snprintf(sta_ctrl, sizeof(sta_ctrl), "%s/%s", WPA_STA_CTRL_DIR, rt->ifname);
    SYSCALL("mkdir -p %s", WPA_STA_CTRL_DIR);
    SYSCALL("mkdir -p /etc/config");
    SYSCALL("rm -Rf %s", sta_ctrl);

    /*
     * settings.json owns credentials; this file is runtime-generated.
     * Board may ship a ctrl_interface-only stub — connect must rewrite network={}.
     */
    fp = fopen(WPA_STA_CONN_CFG, "w");
    if (!fp) {
        log_error("wpa conf write failed: %s errno=%d (%s)",
                  WPA_STA_CONN_CFG, errno, strerror(errno));
        return -1;
    }
    nw = fwrite(text, 1, (size_t)iTextlen, fp);
    if (fflush(fp) != 0 || nw != (size_t)iTextlen) {
        log_error("wpa conf fwrite incomplete: %s wrote=%zu need=%d",
                  WPA_STA_CONN_CFG, nw, iTextlen);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    log_info("wpa conf written: %s ssid=%s key_mgmt=%s psk=%s",
             WPA_STA_CONN_CFG, rt->sta_ssid, key_mgmt,
             rt->sta_pwd[0] ? "yes" : "no");

    SYSCALL("wpa_supplicant -Dnl80211 -i %s -c %s -B", rt->ifname, WPA_STA_CONN_CFG);
    if (wpa_wait_ctrl_socket(sta_ctrl, 3000) != 0) {
        log_error("wpa ctrl socket missing after start: %s", sta_ctrl);
        return -1;
    }
    log_info("wpa ctrl ready: %s", sta_ctrl);
    return 0;
}

static int wpa_ctrl_poll(wifi_hw_runtime_t *rt)
{
	char text[1024];
	size_t textlen = sizeof(text) - 1;
	char *result;
	int iRet;
	char sta_ctrl[64];

	if (!rt)
		return -1;

	snprintf(sta_ctrl, sizeof(sta_ctrl), "%s/%s", WPA_STA_CTRL_DIR, rt->ifname);
	if (s_ctrl == NULL) {
		wpa_ctrl_cleanup();
		s_ctrl = wpa_ctrl_open(sta_ctrl);
		if (!s_ctrl)
			return -1;
		if (wpa_ctrl_attach(s_ctrl) != 0) {
			wpa_ctrl_close(s_ctrl);
			s_ctrl = NULL;
			return -1;
		}
	}

	if (wpa_ctrl_pending(s_ctrl) <= 0)
		return -1;

	iRet = wpa_ctrl_recv(s_ctrl, text, &textlen);
	if (iRet < 0 || textlen <= 0)
		return -1;

	text[textlen] = '\0';
	result = strchr(text, '>');
	result = !result ? text : result + 1;
	wpa_handle_result(rt, result);

    /* set_sta_status already syncs rt+g_state; kick DHCP without blocking the
     * HAL service mutex for a fixed 2s (was system_sleep_ms under wifi_run lock). */
    if (rt->sta_status == WIFI_STA_STAT_CONNECTED) {
		log_info("wpa associated, starting udhcpc on %s", rt->ifname);
        /* Board script publishes DHCP DNS to tmpfs (/ is read-only squashfs);
         * without it getaddrinfo() keeps failing after a GUI-initiated connect. */
		if (access(WPA_UDHCPC_SCRIPT, X_OK) == 0)
			SYSCALL("udhcpc -b -i %s -t 3 -A 5 -q -s %s", rt->ifname, WPA_UDHCPC_SCRIPT);
		else
			SYSCALL("udhcpc -b -i %s -t 3 -A 5 -q", rt->ifname);
        rt->sta_status = WIFI_STA_STAT_ADDRESSED;
		if (rt->set_sta_status)
			rt->set_sta_status(rt, WIFI_STA_STAT_ADDRESSED);
		log_info("wpa addressed ip path kicked iface=%s", rt->ifname);
	}
    if (rt->sta_status == WIFI_STA_STAT_ADDRESSED ||
        rt->sta_status == WIFI_STA_STAT_CONNECTED ||
        rt->sta_status == WIFI_STA_STAT_PSK_FAIL ||
        rt->sta_status == WIFI_STA_STAT_AP_NOT_FOUND) {
		wpa_ctrl_detach(s_ctrl);
		wpa_ctrl_close(s_ctrl);
		s_ctrl = NULL;
	}
	return 0;
}

static int wpa_ping_gateway(const char *ifname)
{
	char cmd[128];
	int iRet;

	net_get_ip(ifname, s_net.ipaddr);
	net_get_route(ifname, s_net.gateway);
	if (strlen(s_net.gateway) < 1)
		return -1;
	snprintf(cmd, sizeof(cmd), "ping -c 1 -W 2 %s > /dev/null 2>&1", s_net.gateway);
	iRet = system(cmd);
	return (iRet == 0) ? 0 : -1;
}

static int wpa_service(wifi_hw_runtime_t *rt)
{
	static int ping_failed;
	int iRet;

	if (!rt)
		return 500;

	if (!s_ping_boot_done) {
		s_ping_boot_done = 1;
		if (wpa_ping_gateway(rt->ifname) == 0) {
            rt->link_request = 0;
            rt->sta_status = WIFI_STA_STAT_ADDRESSED;
		}
	}

    if (rt->link_request == 1) {
		ping_failed = 0;
		if (wpa_supplicant_start(rt) != 0) {
			log_error("wpa_supplicant_start failed ssid=%s", rt->sta_ssid);
			rt->link_request = 0;
			rt->sta_status = WIFI_STA_STAT_AP_NOT_FOUND;
			if (rt->set_sta_status)
				rt->set_sta_status(rt, WIFI_STA_STAT_AP_NOT_FOUND);
			return 1000;
		}
        rt->link_request = 0;
        rt->sta_status = WIFI_STA_STAT_CONNECTING;
	}

    if (rt->sta_status == WIFI_STA_STAT_CONNECTED ||
        rt->sta_status == WIFI_STA_STAT_ADDRESSED) {
		if (wpa_ping_gateway(rt->ifname) != 0) {
			ping_failed++;
			if (ping_failed > 10) {
				ping_failed = 0;
				wpa_supplicant_start(rt);
                rt->sta_status = WIFI_STA_STAT_CONNECTING;
			}
			return 1000;
		}
		return 1000;
	}
    if (rt->sta_status == WIFI_STA_STAT_CONNECTING) {
		iRet = wpa_ctrl_poll(rt);
		return (iRet != 0) ? 100 : 200;
	}
	return 500;
}

static int wpa_sta_disconnect(wifi_hw_runtime_t *rt)
{
	if (!rt)
		return HARDWARE_WIFI_ERROR_INVALID_PARAM;
	wpa_supplicant_stop(rt);
    rt->sta_status = WIFI_STA_STAT_IDLE;
	return HARDWARE_WIFI_OK;
}

static int wpa_ap_start(const char *ifname, const char *ssid, const char *pwd)
{
	(void)ssid;
	(void)pwd;
	SYSCALL("killall wpa_supplicant");
	SYSCALL("killall udhcpc");
	SYSCALL("killall udhcpd");
	SYSCALL("killall hostapd");
	SYSCALL("ifconfig %s 192.168.43.1", ifname);
	SYSCALL("mkdir -p /var/lib/misc/");
	SYSCALL("touch /var/lib/misc/udhcpd.leases");
	SYSCALL("rm -f /dev/random");
	SYSCALL("ln -s /dev/urandom /dev/random");
	SYSCALL("udhcpd /etc/config/udhcpd.conf -S");
	SYSCALL("hostapd -dd -S -B /etc/config/hostapd.conf");
	sleep(2);
	return HARDWARE_WIFI_OK;
}

static int wpa_ap_stop(const char *ifname)
{
	(void)ifname;
	SYSCALL("killall wpa_supplicant");
	SYSCALL("killall udhcpc");
	SYSCALL("killall udhcpd");
	SYSCALL("killall hostapd");
	return HARDWARE_WIFI_OK;
}

static int wpa_scan(const char *ifname)
{
	struct stat st;
	char sta_ctrl[64];
	char reply[256];

	if (!ifname || !ifname[0])
		ifname = WIFI_HAL_DEFAULT_IFACE;

	snprintf(sta_ctrl, sizeof(sta_ctrl), "%s/%s", WPA_STA_CTRL_DIR, ifname);
	if (stat(sta_ctrl, &st) != 0 || !S_ISSOCK(st.st_mode))
		return HARDWARE_WIFI_ERROR_NOT_SUPPORTED;

	strncpy(s_scan_ifname, ifname, sizeof(s_scan_ifname) - 1);
	s_scan_ifname[sizeof(s_scan_ifname) - 1] = '\0';

	if (wpa_ctrl_run_cmd(ifname, "SCAN", reply, sizeof(reply)) != 0)
		return HARDWARE_WIFI_ERROR_OPERATION_FAILED;

	/* BUSY: scan already running; caller may still read SCAN_RESULTS. */
	if (strncmp(reply, "FAIL-BUSY", 9) == 0)
		return HARDWARE_WIFI_OK;
	if (strncmp(reply, "FAIL", 4) == 0)
		return HARDWARE_WIFI_ERROR_OPERATION_FAILED;

	return HARDWARE_WIFI_OK;
}

static int wpa_get_scan_results(wifi_scan_result_t *results, int *num_results)
{
	char reply[8192];
	char *copy;
	char *line;
	char *save = NULL;
	int max;
	int n = 0;
	wifi_scan_result_t entry;

	if (!results || !num_results || *num_results <= 0)
		return HARDWARE_WIFI_ERROR_INVALID_PARAM;

	max = *num_results;
	*num_results = 0;

	if (wpa_ctrl_run_cmd(s_scan_ifname, "SCAN_RESULTS", reply, sizeof(reply)) != 0)
		return HARDWARE_WIFI_ERROR_OPERATION_FAILED;

	copy = strdup(reply);
	if (!copy)
		return HARDWARE_WIFI_ERROR_MEMORY;

	for (line = strtok_r(copy, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		if (!wpa_parse_scan_line(line, &entry))
			continue;
		n = wpa_scan_merge_result(results, n, max, &entry);
	}
	free(copy);

	*num_results = n;
	return HARDWARE_WIFI_OK;
}

static int wpa_init(void)
{
	return HARDWARE_WIFI_OK;
}

static void wpa_deinit(void)
{
}

const wifi_backend_ops_t wifi_backend_wpa_ops = {
	.name = "wpa",
	.init = wpa_init,
	.deinit = wpa_deinit,
	.service = wpa_service,
	.sta_disconnect = wpa_sta_disconnect,
	.ap_start = wpa_ap_start,
	.ap_stop = wpa_ap_stop,
	.scan = wpa_scan,
	.get_scan_results = wpa_get_scan_results,
};

const wifi_backend_ops_t *wifi_backend_get(void)
{
	return &wifi_backend_wpa_ops;
}
