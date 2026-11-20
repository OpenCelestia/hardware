/* Aos uwlan backend: /dev/wifi, LwIP netdev wlan0, DHCP via dhclient. */
#include "wifi_hw.h"

#include <string.h>
#include <unistd.h>

#include "log.h"
#include "net/net.h"
#include "system_macros.h"
#include "uwlan_mgnt.h"
#include <wlan_ioctl.h>

#define LOG_TAG "HAL WIFI UWLAN"
#define UWLAN_LWIP_IFACE           "wlan0"
#define UWLAN_CONNECT_RETRY_MS     2000

static void *s_uwlan;
static int s_fd = -1;
static int s_dhcp_started;
static unsigned s_connect_backoff_ms;

static int backend_uwlan_init(void);

static int uwlan_lwip_iface_up(void)
{
    char mac[32];

    if (access("/sys/class/net/" UWLAN_LWIP_IFACE, F_OK) == 0)
        return 1;
    return net_get_mac(UWLAN_LWIP_IFACE, mac, (int)sizeof(mac)) == 0 ? 1 : 0;
}

static int uwlan_same_ssid_connected(const char *ssid)
{
    wlan_info_t cur;

    if (!s_uwlan || !ssid || !ssid[0])
        return 0;
    memset(&cur, 0, sizeof(cur));
    if (uwlan_get_sta_info(s_uwlan, &cur) != 0 || cur.ssid.len == 0)
        return 0;
    if (strncmp((const char *)cur.ssid.val, ssid, cur.ssid.len) != 0)
        return 0;
    return 1;
}

static void uwlan_maybe_start_dhcp(void)
{
    char ip[32];

    if (s_dhcp_started)
        return;
    if (!uwlan_lwip_iface_up())
        return;
    if (net_get_ip(UWLAN_LWIP_IFACE, ip) != 0)
        return;
    if (ip[0] != '\0' && strcmp(ip, "0.0.0.0") != 0) {
        s_dhcp_started = 1;
        return;
    }
    s_dhcp_started = 1;
    log_info("starting DHCP on %s", UWLAN_LWIP_IFACE);
    SYSCALL("dhclient " UWLAN_LWIP_IFACE);
}

static int backend_uwlan_ensure(void)
{
    if (s_uwlan != NULL)
        return HARDWARE_WIFI_OK;
    return backend_uwlan_init();
}

static int backend_uwlan_init(void)
{
    s_uwlan = uwlan_mgnt_init("/dev/wifi", &s_fd);
    if (s_uwlan == NULL) {
        log_error("uwlan_mgnt_init(/dev/wifi) failed (kernel WiFi driver /dev node missing?)");
        return HARDWARE_WIFI_ERROR_OPERATION_FAILED;
    }
    log_info("uwlan ready fd=%d (lwip iface %s)", s_fd, UWLAN_LWIP_IFACE);
#ifdef CONFIG_HW_WIFI_ENUM_DELAY_MS
    /* USB dongle node appears after uwlan_mgnt_init; delay is set in project config.mk. */
    usleep((unsigned)CONFIG_HW_WIFI_ENUM_DELAY_MS * 1000u);
#endif
    return HARDWARE_WIFI_OK;
}

static void backend_uwlan_deinit(void)
{
    s_uwlan = NULL;
    s_fd = -1;
    s_dhcp_started = 0;
    s_connect_backoff_ms = 0;
}

static int uwlan_sta_connect(wifi_hw_runtime_t *rt)
{
    wlan_info_t info;
    long err;
    size_t ssid_len;

    if (!rt || !rt->sta_ssid[0])
        return HARDWARE_WIFI_ERROR_INVALID_PARAM;
    if (backend_uwlan_ensure() != HARDWARE_WIFI_OK)
        return HARDWARE_WIFI_ERROR_OPERATION_FAILED;

    if (uwlan_lwip_iface_up() && uwlan_same_ssid_connected(rt->sta_ssid)) {
        log_info("already associated ssid=%s, skip uwlan_connect", rt->sta_ssid);
        rt->link_request = 0;
        if (rt->set_sta_status)
            rt->set_sta_status(rt, WIFI_STA_STAT_CONNECTED);
        uwlan_maybe_start_dhcp();
        return HARDWARE_WIFI_OK;
    }

    memset(&info, 0, sizeof(info));
    ssid_len = strlen(rt->sta_ssid);
    if (ssid_len > WLAN_SSID_MAX_LENGTH)
        ssid_len = WLAN_SSID_MAX_LENGTH;
    info.ssid.len = (unsigned char)ssid_len;
    memcpy(info.ssid.val, rt->sta_ssid, ssid_len);
    info.security = WPA2_SECURITY | WPA_SECURITY;

    (void)uwlan_reconn_switch(s_uwlan, 1, 10);
    err = uwlan_set_mode(s_uwlan, WLAN_STATION);
    if (err != 0) {
        log_error("uwlan_set_mode(STA) failed: %ld", err);
        rt->link_request = 1;
        return HARDWARE_WIFI_ERROR_OPERATION_FAILED;
    }

    log_info("uwlan_connect ssid=%s", rt->sta_ssid);
    err = uwlan_connect(s_uwlan, &info, rt->sta_pwd[0] ? rt->sta_pwd : NULL);
    if (err != 0) {
        log_error("uwlan_connect failed: %ld", err);
        if (rt->set_sta_status)
            rt->set_sta_status(rt, WIFI_STA_STAT_AP_NOT_FOUND);
        rt->link_request = 1;
        return HARDWARE_WIFI_ERROR_OPERATION_FAILED;
    }

    if (rt->set_sta_status)
        rt->set_sta_status(rt, WIFI_STA_STAT_CONNECTING);
    return HARDWARE_WIFI_OK;
}

static int uwlan_sta_disconnect(wifi_hw_runtime_t *rt)
{
    (void)rt;
    if (s_uwlan == NULL)
        return HARDWARE_WIFI_OK;
    (void)uwlan_disconnect(s_uwlan);
    s_dhcp_started = 0;
    if (rt && rt->set_sta_status)
        rt->set_sta_status(rt, WIFI_STA_STAT_IDLE);
    return HARDWARE_WIFI_OK;
}

static int uwlan_service(wifi_hw_runtime_t *rt)
{
    wlan_state_t st = 0;
    char ip[32];

    if (!rt)
        return 1000;

    if (rt->link_request == 1) {
        int cr = uwlan_sta_connect(rt);
        if (cr != HARDWARE_WIFI_OK) {
            if (s_connect_backoff_ms < UWLAN_CONNECT_RETRY_MS)
                s_connect_backoff_ms = UWLAN_CONNECT_RETRY_MS;
            return (int)s_connect_backoff_ms;
        }
        s_connect_backoff_ms = 500;
    }

    if (s_uwlan == NULL)
        return 1000;

    if (uwlan_get_sta_state(s_uwlan, &st) == 0) {
        if (st & WLAN_STATE_WRONG_PASSWORD) {
            if (rt->set_sta_status)
                rt->set_sta_status(rt, WIFI_STA_STAT_PSK_FAIL);
            return 1000;
        }
    }

    if (uwlan_lwip_iface_up() &&
        (rt->sta_status == WIFI_STA_STAT_CONNECTING ||
         rt->sta_status == WIFI_STA_STAT_CONNECTED)) {
        uwlan_maybe_start_dhcp();
    }

    if (uwlan_lwip_iface_up()) {
        if (net_get_ip(UWLAN_LWIP_IFACE, ip) == 0 && ip[0] != '\0' &&
            strcmp(ip, "0.0.0.0") != 0) {
            if (rt->sta_status != WIFI_STA_STAT_ADDRESSED && rt->set_sta_status)
                rt->set_sta_status(rt, WIFI_STA_STAT_ADDRESSED);
            rt->link_request = 0;
            return 2000;
        }
    }

    if (rt->sta_status == WIFI_STA_STAT_CONNECTING)
        return 500;

    return 1000;
}

static int uwlan_ap_start(const char *ifname, const char *ssid, const char *pwd)
{
    (void)ifname;
    (void)ssid;
    (void)pwd;
    return HARDWARE_WIFI_ERROR_OPERATION_FAILED;
}

static int uwlan_ap_stop(const char *ifname)
{
    (void)ifname;
    return HARDWARE_WIFI_OK;
}

static int backend_uwlan_scan(const char *ifname)
{
    (void)ifname;
    if (backend_uwlan_ensure() != HARDWARE_WIFI_OK)
        return HARDWARE_WIFI_ERROR_OPERATION_FAILED;
    return uwlan_scan(s_uwlan, NULL) == 0 ? HARDWARE_WIFI_OK : HARDWARE_WIFI_ERROR_IO;
}

static int uwlan_get_scan_results(wifi_scan_result_t *results, int *num_results)
{
    (void)results;
    if (num_results)
        *num_results = 0;
    return HARDWARE_WIFI_OK;
}

static const wifi_backend_ops_t wifi_backend_uwlan_ops = {
    .name = "uwlan",
    .init = backend_uwlan_init,
    .deinit = backend_uwlan_deinit,
    .service = uwlan_service,
    .sta_connect = uwlan_sta_connect,
    .sta_disconnect = uwlan_sta_disconnect,
    .ap_start = uwlan_ap_start,
    .ap_stop = uwlan_ap_stop,
    .scan = backend_uwlan_scan,
    .get_scan_results = uwlan_get_scan_results,
};

const wifi_backend_ops_t *wifi_backend_get(void)
{
    return &wifi_backend_uwlan_ops;
}
