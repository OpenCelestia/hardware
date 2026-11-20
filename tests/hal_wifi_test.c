/*
 * WiFi HAL sample test
 * Supports STA, AP, and P2P modes
 */
#if CONFIG_HW_WIFI

#include "wifi.h"
#include "hardware.h"
#include "net/net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "soc_platform.h"
#include "system_thread.h"
#include "log.h"

#define LOG_TAG "WIFI"

static void print_help(const char *prog)
{
    printf("\n");
    printf("Usage: %s <command> [options]\n", prog);
    printf("\n");
    printf("=== STA Mode ===\n");
    printf("  scan                     scan and display WiFi networks\n");
    printf("  connect <ssid> <pass>    connect to WiFi\n");
    printf("  disconnect               disconnect from WiFi\n");
    printf("  status                   show connection status\n");
    printf("\n");
    printf("=== AP Mode ===\n");
    printf("  ap start <ssid> <pass>   start AP with SSID and password\n");
    printf("  ap stop                  stop AP\n");
    printf("  ap status                show AP status\n");
    printf("\n");
    printf("=== P2P Mode ===\n");
    printf("  p2p discover             discover P2P peers\n");
    printf("  p2p stop                 stop discovery\n");
    printf("  p2p list                 list discovered peers\n");
    printf("  p2p connect <addr>       connect to P2P device\n");
    printf("  p2p disconnect           disconnect P2P\n");
    printf("  p2p group create         create P2P group\n");
    printf("  p2p group remove         remove P2P group\n");
    printf("  p2p group info           show P2P group info\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s scan\n", prog);
    printf("  %s connect MyWiFi 12345678\n", prog);
    printf("  %s ap start MyAP 12345678\n", prog);
    printf("  %s p2p discover\n", prog);
    printf("\n");
}

/* ==================== STA Mode Functions ==================== */

static int wifi_sta_scan(wifi_hw_device_t *device)
{
    log_info("=== WiFi Scan ===");
    
    if (!device->sta_scan || !device->sta_get_scan_results) {
        log_error("scan not supported");
        return -1;
    }

    log_info("Scanning...");
    int ret = device->sta_scan(device);
    if (ret != 0) {
        log_error("Scan failed (ret=%d)", ret);
        return -1;
    }

    wifi_scan_result_t results[WIFI_SCAN_MAX_RESULTS];
    int num = WIFI_SCAN_MAX_RESULTS;
    ret = device->sta_get_scan_results(device, results, &num);
    if (ret != 0) {
        log_error("Get results failed (ret=%d)", ret);
        return -1;
    }

    if (num == 0) {
        /* No AP found is a normal case; keep output quiet as requested. */
        return 0;
    }

    log_info("Found %d networks:", num);
    log_info("%-4s %-32s %-18s %6s %6s %s", "No.", "SSID", "BSSID", "RSSI", "FREQ", "Security");
    log_info("%-4s %-32s %-18s %6s %6s %s", "----", "--------------------------------", "------------------", "------", "------", "--------");
    
    for (int i = 0; i < num; i++) {
        log_info("%-4d %-32s %-18s %6d %6d %s",
                 i + 1,
                 results[i].ssid[0] ? results[i].ssid : "(hidden)",
                 results[i].bssid,
                 results[i].rssi,
                 results[i].frequency,
                 results[i].flags);
    }
    return 0;
}

static int wifi_sta_status(wifi_hw_device_t *device)
{
    log_info("=== WiFi STA Status ===");
    
    if (!device->sta_status) {
        log_error("status not supported");
        return -1;
    }

    wifi_hal_status_t st = device->sta_status(device);
    const char *status_str;
    switch (st) {
        case WIFI_STA_STAT_IDLE:         status_str = "Idle"; break;
        case WIFI_STA_STAT_CONNECTING:   status_str = "Connecting"; break;
        case WIFI_STA_STAT_CONNECTED:    status_str = "Connected"; break;
        case WIFI_STA_STAT_ADDRESSED:    status_str = "IP Addressed"; break;
        case WIFI_STA_STAT_PSK_FAIL:     status_str = "Password Error"; break;
        case WIFI_STA_STAT_AP_NOT_FOUND: status_str = "AP Not Found"; break;
        default:                          status_str = "Unknown"; break;
    }
    log_info("Status: %s (%d)", status_str, st);

    if (st == WIFI_STA_STAT_ADDRESSED || st == WIFI_STA_STAT_CONNECTED) {
        const char *iface = getenv("WIFI_INTERFACE");
        char ip[32] = "";
        char mask[32] = "";

        if (!iface || !iface[0])
            iface = "vnet0";
        (void)net_get_ip(iface, ip);
        (void)net_get_netmask(iface, mask);
        if (ip[0])
            log_info("Interface %s: ip=%s mask=%s", iface, ip, mask[0] ? mask : "(none)");
    }
    return 0;
}

static int wifi_sta_connect(wifi_hw_device_t *device, const char *ssid, const char *pass)
{
    log_info("=== WiFi Connect ===");
    
    if (!device->sta_connect) {
        log_error("connect not supported");
        return -1;
    }

    log_info("Connecting to '%s'...", ssid);
    int ret = device->sta_connect(device, ssid, pass, 10000);
    if (ret != 0) {
        log_error("Connect failed (ret=%d)", ret);
        return -1;
    }

    if (device->sta_status) {
        int waited_ms = 0;
        const int step_ms = 500;
        const int timeout_ms = 15000;
        wifi_hal_status_t st = WIFI_STA_STAT_CONNECTING;

        while (waited_ms < timeout_ms) {
            st = device->sta_status(device);
            if (st == WIFI_STA_STAT_CONNECTED || st == WIFI_STA_STAT_ADDRESSED) {
                log_info("Connected (status=%d)", st);
                return 0;
            }
            if (st == WIFI_STA_STAT_PSK_FAIL || st == WIFI_STA_STAT_AP_NOT_FOUND) {
                log_error("Connect failed (status=%d)", st);
                return -1;
            }
            usleep((useconds_t)step_ms * 1000);
            waited_ms += step_ms;
        }
        log_warn("Connect request sent; not confirmed within %d ms (status=%d). Run: hal_wifi_test status",
                 timeout_ms, st);
        return 0;
    }

    log_info("Connect request sent (no status API)");
    return 0;
}

static int wifi_sta_disconnect(wifi_hw_device_t *device)
{
    log_info("=== WiFi Disconnect ===");
    
    if (!device->sta_disconnect) {
        log_error("disconnect not supported");
        return -1;
    }

    int ret = device->sta_disconnect(device);
    if (ret != 0) {
        log_error("Disconnect failed (ret=%d)", ret);
        return -1;
    }
    log_info("Disconnected");
    return 0;
}

/* ==================== AP Mode Functions ==================== */

static int wifi_ap_start(wifi_hw_device_t *device, const char *ssid, const char *pass)
{
    log_info("=== WiFi AP Start ===");
    
    if (!device->ap_start || !device->ap_set_config) {
        log_error("AP mode not supported");
        return -1;
    }

    log_info("Setting AP config: SSID='%s'", ssid);
    int ret = device->ap_set_config(device, ssid, pass);
    if (ret != 0) {
        log_error("Set config failed (ret=%d)", ret);
        return -1;
    }

    log_info("Starting AP...");
    ret = device->ap_start(device);
    if (ret != 0) {
        log_error("AP start failed (ret=%d)", ret);
        return -1;
    }
    log_info("AP started successfully!");
    log_info("SSID: %s", ssid);
    return 0;
}

static int wifi_ap_stop(wifi_hw_device_t *device)
{
    log_info("=== WiFi AP Stop ===");
    
    if (!device->ap_stop) {
        log_error("AP stop not supported");
        return -1;
    }

    int ret = device->ap_stop(device);
    if (ret != 0) {
        log_error("AP stop failed (ret=%d)", ret);
        return -1;
    }
    log_info("AP stopped");
    return 0;
}

static int wifi_ap_status(wifi_hw_device_t *device)
{
    log_info("=== WiFi AP Status ===");
    
    if (!device->ap_get_config) {
        log_error("AP status not supported");
        return -1;
    }

    char ssid[33] = {0};
    char pwd[64] = {0};
    int ret = device->ap_get_config(device, ssid, pwd);
    if (ret != 0) {
        log_error("Get AP config failed (ret=%d)", ret);
        return -1;
    }
    log_info("SSID: %s", ssid[0] ? ssid : "(not set)");
    log_info("Password: %s", pwd[0] ? "****" : "(not set)");
    return 0;
}

/* ==================== P2P Mode Functions ==================== */

static int wifi_p2p_discover(wifi_hw_device_t *device)
{
    log_info("=== P2P Discover ===");
    
    if (!device->p2p_discover_peers) {
        log_error("P2P discover not supported");
        return -1;
    }

    log_info("Starting P2P discovery...");
    int ret = device->p2p_discover_peers(device);
    if (ret != 0) {
        log_error("P2P discover failed (ret=%d)", ret);
        return -1;
    }
    log_info("P2P discovery started");
    return 0;
}

static int wifi_p2p_stop(wifi_hw_device_t *device)
{
    log_info("=== P2P Stop Discovery ===");
    
    if (!device->p2p_stop_discover_peers) {
        log_error("P2P stop not supported");
        return -1;
    }

    int ret = device->p2p_stop_discover_peers(device);
    if (ret != 0) {
        log_error("P2P stop failed (ret=%d)", ret);
        return -1;
    }
    log_info("P2P discovery stopped");
    return 0;
}

static int wifi_p2p_list(wifi_hw_device_t *device)
{
    log_info("=== P2P Peers ===");
    
    if (!device->p2p_get_discovered_peers) {
        log_error("P2P list not supported");
        return -1;
    }

    wifi_p2p_device_info_t peers[32];
    int num = 32;
    int ret = device->p2p_get_discovered_peers(device, peers, &num);
    if (ret != 0) {
        log_error("Get peers failed (ret=%d)", ret);
        return -1;
    }

    if (num == 0) {
        log_info("No P2P peers found");
        return 0;
    }

    log_info("Found %d P2P peers:", num);
    log_info("%-4s %-32s %-18s %6s %6s", "No.", "Device Name", "Address", "RSSI", "FREQ");
    log_info("%-4s %-32s %-18s %6s %6s", "----", "--------------------------------", "------------------", "------", "------");
    
    for (int i = 0; i < num; i++) {
        log_info("%-4d %-32s %-18s %6d %6d",
                 i + 1,
                 peers[i].device_name,
                 peers[i].device_address,
                 peers[i].rssi,
                 peers[i].frequency);
    }
    return 0;
}

static int wifi_p2p_connect(wifi_hw_device_t *device, const char *addr)
{
    log_info("=== P2P Connect ===");
    
    if (!device->p2p_connect) {
        log_error("P2P connect not supported");
        return -1;
    }

    log_info("Connecting to P2P device: %s", addr);
    int ret = device->p2p_connect(device, addr);
    if (ret != 0) {
        log_error("P2P connect failed (ret=%d)", ret);
        return -1;
    }
    log_info("P2P connected");
    return 0;
}

static int wifi_p2p_disconnect(wifi_hw_device_t *device)
{
    log_info("=== P2P Disconnect ===");
    
    if (!device->p2p_disconnect) {
        log_error("P2P disconnect not supported");
        return -1;
    }

    int ret = device->p2p_disconnect(device);
    if (ret != 0) {
        log_error("P2P disconnect failed (ret=%d)", ret);
        return -1;
    }
    log_info("P2P disconnected");
    return 0;
}

static int wifi_p2p_group_create(wifi_hw_device_t *device)
{
    log_info("=== P2P Create Group ===");
    
    if (!device->p2p_create_group) {
        log_error("P2P create group not supported");
        return -1;
    }

    int ret = device->p2p_create_group(device);
    if (ret != 0) {
        log_error("Create group failed (ret=%d)", ret);
        return -1;
    }
    log_info("P2P group created");
    return 0;
}

static int wifi_p2p_group_remove(wifi_hw_device_t *device)
{
    log_info("=== P2P Remove Group ===");
    
    if (!device->p2p_remove_group) {
        log_error("P2P remove group not supported");
        return -1;
    }

    int ret = device->p2p_remove_group(device);
    if (ret != 0) {
        log_error("Remove group failed (ret=%d)", ret);
        return -1;
    }
    log_info("P2P group removed");
    return 0;
}

static int wifi_p2p_group_info(wifi_hw_device_t *device)
{
    log_info("=== P2P Group Info ===");
    
    if (!device->p2p_get_group_info || !device->p2p_is_group_owner) {
        log_error("P2P group info not supported");
        return -1;
    }

    int is_go = device->p2p_is_group_owner(device);
    log_info("Is Group Owner: %s", is_go ? "Yes" : "No");

    wifi_p2p_group_info_t info;
    memset(&info, 0, sizeof(info));
    int ret = device->p2p_get_group_info(device, &info);
    if (ret != 0) {
        log_error("Get group info failed (ret=%d)", ret);
        return -1;
    }

    log_info("Group SSID: %s", info.group_ssid[0] ? info.group_ssid : "(none)");
    log_info("Group Owner: %s", info.group_owner_address[0] ? info.group_owner_address : "(none)");
    log_info("Group ID: %d", info.group_id);
    return 0;
}

/* ==================== Main Entry ==================== */

#if (defined OS_NAME_LINUX)
int main(int argc, char **argv)
#else
static int hw_wifi_cmd(int argc, char **argv)
#endif
{
    const struct hw_module_t *module = NULL;
    wifi_hw_device_t *device = NULL;
    int ret = 0;

    soc_platform_init(0);
    log_set_level(LOG_INFO);
    thread_init();
    hw_module_init();

    /* Get WiFi module */
    if (hw_get_module(WIFI_HARDWARE_MODULE_ID, &module) != 0 || module == NULL) {
        log_error("WiFi module not found");
        return -1;
    }

    if (wifi_hw_device_open(module, &device) != 0 || device == NULL) {
        log_error("Failed to open WiFi device");
        return -1;
    }

    if (device->load_config != NULL) {
        const char *cfg = getenv("HAL_WIFI_CONFIG");
        int lret = device->load_config(device, cfg);
        if (lret != 0) {
            log_warn("load_config failed (%d), AP/STA may need %s on rootfs",
                     lret, "/etc/config/hal_wifi_config.ini");
        }
    }

    /* Parse command */
    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_help(argv[0]);
        ret = (argc < 2) ? -1 : 0;
    }
    /* STA commands */
    else if (strcmp(argv[1], "scan") == 0) {
        ret = wifi_sta_scan(device);
    }
    else if (strcmp(argv[1], "connect") == 0) {
        if (argc < 4) {
            log_error("Usage: %s connect <ssid> <password>", argv[0]);
            ret = -1;
        } else {
            ret = wifi_sta_connect(device, argv[2], argv[3]);
        }
    }
    else if (strcmp(argv[1], "disconnect") == 0) {
        ret = wifi_sta_disconnect(device);
    }
    else if (strcmp(argv[1], "status") == 0) {
        ret = wifi_sta_status(device);
    }
    /* AP commands */
    else if (strcmp(argv[1], "ap") == 0) {
        if (argc < 3) {
            log_error("Usage: %s ap <start|stop|status> [ssid] [pass]", argv[0]);
            ret = -1;
        } else if (strcmp(argv[2], "start") == 0) {
            if (argc < 5) {
                log_error("Usage: %s ap start <ssid> <password>", argv[0]);
                ret = -1;
            } else {
                ret = wifi_ap_start(device, argv[3], argv[4]);
            }
        } else if (strcmp(argv[2], "stop") == 0) {
            ret = wifi_ap_stop(device);
        } else if (strcmp(argv[2], "status") == 0) {
            ret = wifi_ap_status(device);
        } else {
            log_error("Unknown AP command: %s", argv[2]);
            ret = -1;
        }
    }
    /* P2P commands */
    else if (strcmp(argv[1], "p2p") == 0) {
        if (argc < 3) {
            log_error("Usage: %s p2p <discover|stop|list|connect|disconnect|group>", argv[0]);
            ret = -1;
        } else if (strcmp(argv[2], "discover") == 0) {
            ret = wifi_p2p_discover(device);
        } else if (strcmp(argv[2], "stop") == 0) {
            ret = wifi_p2p_stop(device);
        } else if (strcmp(argv[2], "list") == 0) {
            ret = wifi_p2p_list(device);
        } else if (strcmp(argv[2], "connect") == 0) {
            if (argc < 4) {
                log_error("Usage: %s p2p connect <device_address>", argv[0]);
                ret = -1;
            } else {
                ret = wifi_p2p_connect(device, argv[3]);
            }
        } else if (strcmp(argv[2], "disconnect") == 0) {
            ret = wifi_p2p_disconnect(device);
        } else if (strcmp(argv[2], "group") == 0) {
            if (argc < 4) {
                log_error("Usage: %s p2p group <create|remove|info>", argv[0]);
                ret = -1;
            } else if (strcmp(argv[3], "create") == 0) {
                ret = wifi_p2p_group_create(device);
            } else if (strcmp(argv[3], "remove") == 0) {
                ret = wifi_p2p_group_remove(device);
            } else if (strcmp(argv[3], "info") == 0) {
                ret = wifi_p2p_group_info(device);
            } else {
                log_error("Unknown P2P group command: %s", argv[3]);
                ret = -1;
            }
        } else {
            log_error("Unknown P2P command: %s", argv[2]);
            ret = -1;
        }
    }
    else {
        log_error("Unknown command: %s", argv[1]);
        print_help(argv[0]);
        ret = -1;
    }

    wifi_hw_device_close(device);
    return ret;
}

#if ((defined OS_NAME_AOS) || (defined OS_NAME_RTT))
SHELL_CMD_EXPORT_ALIAS(hw_wifi_cmd, hw_wifi_cmd, wifi program);
#endif

#endif
