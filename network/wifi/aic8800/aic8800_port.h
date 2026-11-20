/*
 * AIC8800 port layer — constants aligned with luna driver / ActionCam.
 *
 * Reference:
 *   system/.../ActionCam/action_cam/src/wifi/aic_wifi_ctrl_port.c
 *   system/.../ActionCam/action_cam/src/wifi/wifi_netif.c
 *   system/.../os/driver_src/.../aic8800_netdrv/rwnx_main.h
 *   system/.../V2.04_luna/rootfs/utils/usr/modules/aic8800_netdrv.ko
 */
#ifndef HARDWARE_AIC8800_PORT_H
#define HARDWARE_AIC8800_PORT_H

#include <stddef.h>

#define AIC_CDEV_PATH           "/dev/aic_cdev"
#define AIC_MAX_DRV_CMD_SIZE    1536
#define AIC_CDEV_BUF_MAX        256
#define AIC_SCAN_LIST_MAX       4096

/* APP_CMD_* from rwnx_main.h */
#define AIC_CMD_CONNECT         1
#define AIC_CMD_DISCONNECT      2
#define AIC_CMD_START_AP        9
#define AIC_CMD_STOP_AP         11
#define AIC_CMD_SCAN_WIFI       12
#define AIC_CMD_GET_SCAN_RESULTS 19

/* fasync MSG id (hostif / rwnx_rx.c) */
#define AIC_MSG_STA_CONNECTING  3001
#define AIC_MSG_PSK_FAIL        3002
#define AIC_MSG_STA_GOT_IP      3003
#define AIC_MSG_CONNECT_CFM     3005
#define AIC_MSG_STA_DISCONNECT  3006
#define AIC_MSG_AP_GOT_IP       3017
#define AIC_MSG_SCAN_AP         3024

#define AIC_SCAN_MCU_WAIT_MS    4000
#define AIC_SCAN_FASYNC_WAIT_MS 3000

#endif /* HARDWARE_AIC8800_PORT_H */
