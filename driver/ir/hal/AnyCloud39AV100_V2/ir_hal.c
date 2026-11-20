// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/driver/ir/hal/AnyCloud39AV100_V2.
 *
 * IR HAL implementation for AnyCloud39AV100_V2 platform.
 * - Implements IRCUT and IRLED control via sysfs.
 * - Based on framework/driver/night_vision/nightvision_service.c.
 */

#include "ir_hal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include "log.h"
#include "system_config.h"

#define LOG_TAG "IR_HAL"

/*===========================================================================
 * Constants
 *===========================================================================*/

/** @brief IRCUT A file name */
#define IRCUT_A_FILE_NAME "ircut_a"

/** @brief IRCUT B file name */
#define IRCUT_B_FILE_NAME "ircut_b"

/** @brief IRLED file path */
#define IRLED_FILE_NAME "/sys/devices/platform/leds/leds/irled/brightness"

/** @brief KM01A (and similar) single sysfs IRCUT control — see PDK sample ir/ak_ir_sample.c */
#define IRCUT_SYSFS0_FILE_NAME "/sys/devices/platform/ircut0/state"

/*===========================================================================
 * Type Definitions
 *===========================================================================*/

/**
 * @brief IRCUT mode type.
 */
enum ircut_mode_type {
    IRCUT_MODE_1LINE,  /**< 1 line mode */
    IRCUT_MODE_2LINE,  /**< 2 line mode */
};

/**
 * @brief IR control structure.
 */
struct drv_ir {
    char *ircut_name;              /**< IRCUT name, defined as sysfs */
    int ircut_line_mode;           /**< Default 1 line mode */
    int init_flag;                 /**< Initialization flag */
    int sysfs_ircut0;              /**< 1: use IRCUT_SYSFS0_FILE_NAME (day/night strings) */
    char ircut_a_gpio_id[8];       /**< IRCUT A GPIO ID */
    char ircut_b_gpio_id[8];      /**< IRCUT B GPIO ID */
};

/**
 * @brief IRLED control structure.
 */
struct drv_irled_info {
    int init;                      /**< Initialization flag */
    int working_level;             /**< IR LED working level: 0=low active, 1=high active */
};

/*===========================================================================
 * Static Variables
 *===========================================================================*/

static struct drv_ir ir_ctrl = {0};
static struct drv_irled_info irled_info = {0};
static int ircut_time = 100;  /* Default switch time 100ms */

static char *ircut_a = "";
static char *ircut_b = "";
static int ircut_a_day_level = 0;
static int ircut_b_day_level = 0;

/*===========================================================================
 * Helper Functions
 *===========================================================================*/
static int file_read_value(const char *path, char *str, int len)
{
    int fd = -1;
    int ret;

    if (NULL == path || NULL == str) {
        log_error("param is null.");
        return -1;
    }

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        log_error("%s file open failed.", path);
        return -1;
    }

    ret = read(fd, str, len);
    if (ret < 0) {
        log_error("%s read failed.", path);
        close(fd);
        return -1;
    }
    close(fd);

    return 0;
}

static int file_write_value(const char *path, const char *str)
{
    int fd = -1;
    int ret;

    if (NULL == path || NULL == str) {
        log_error("param is null.");
        return -1;
    }

    fd = open(path, O_WRONLY);
    if (fd < 0) {
        log_error("%s file open failed.", path);
        return -1;
    }

    ret = write(fd, str, strlen(str));
    if (ret < 0) {
        log_error("%s write %s failed.", path, str);
        close(fd);
        return -1;
    }
    close(fd);

    return 0;
}

static int ircut_gpio_set(const char *path)
{
    char id_str[8] = {0};
    char gpio_path[64] = {0};
    struct stat st = {0};

    if (NULL == path) {
        log_error("path is null.");
        return -1;
    }

    if (!strcmp(IRCUT_A_FILE_NAME, path)) {
        strncpy(ir_ctrl.ircut_a_gpio_id, ircut_a, 8);
        strncpy(id_str, ircut_a, 8);
    } else if (!strcmp(IRCUT_B_FILE_NAME, path)) {
        strncpy(ir_ctrl.ircut_b_gpio_id, ircut_b, 8);
        strncpy(id_str, ircut_b, 8);
    } else {
        log_error("%s error", path);
        return -1;
    }

    memset(gpio_path, 0, 64);
    snprintf(gpio_path, sizeof(gpio_path), "/sys/class/gpio/gpio%s", id_str);

    if (-1 == stat(gpio_path, &st)) {
        if (file_write_value("/sys/class/gpio/export", id_str)) {
            log_error("%s can't set", path);
            return -1;
        }
    }

    memset(gpio_path, 0, 64);
    snprintf(gpio_path, sizeof(gpio_path), "/sys/class/gpio/gpio%s/direction", id_str);

    if (file_write_value(gpio_path, "out")) {
        log_error("%s can't set", path);
        return -1;
    }

    return 0;
}

static int camera_set_ircut(const char *name, int value)
{
    char value_str[8] = {0};
    char id_str[8] = {0};
    char gpio_path[64] = {0};

    if (NULL == name) {
        log_error("name is null.");
        return -1;
    }

    if ((0 != value) && (1 != value)) {
        log_error("invalid arg");
        return -1;
    }

    if (!strcmp(IRCUT_A_FILE_NAME, name))
        strncpy(id_str, ir_ctrl.ircut_a_gpio_id, 8);
    else if (!strcmp(IRCUT_B_FILE_NAME, name))
        strncpy(id_str, ir_ctrl.ircut_b_gpio_id, 8);
    else {
        log_error("%s error", name);
        return -1;
    }

    memset(gpio_path, 0, 64);
    snprintf(gpio_path, sizeof(gpio_path), "/sys/class/gpio/gpio%s/value", id_str);

    snprintf(value_str, sizeof(value_str), "%d", value);

    if (file_write_value(gpio_path, value_str)) {
        log_error("%s can't set", name);
        return -1;
    }

    log_info("set %s %d success!", name, value);

    return 0;
}

static int read_conf(const char *pc_conf)
{
    int i_size;
    char *pc_buff = NULL;

    if (NULL == (pc_buff = read_file((char *)pc_conf, &i_size))) {
        return -1;
    }

    /* Parse configuration using functions from system_config.h */
    get_conf_str(pc_buff, "ircut_a", &ircut_a);
    get_conf_str(pc_buff, "ircut_b", &ircut_b);
    get_conf_int(pc_buff, "ircut_a_day_level", &ircut_a_day_level);
    get_conf_int(pc_buff, "ircut_b_day_level", &ircut_b_day_level);

    free(pc_buff);
    return 0;
}

/*===========================================================================
 * IRCUT HAL Interface Implementation
 *===========================================================================*/

int hal_ir_init(const char* cfg_path)
{
    if (ir_ctrl.init_flag) {
        log_warn("ircut have been inited.");
        return 0;
    }

    /*
     * KM01A-class boards expose a single ircut0 sysfs node; GPIO ircut_a/b
     * config is unused.  Prefer this when the node is present so the same
     * binary works on AK3918AV100-style GPIO IRCUT and KM01A sysfs IRCUT.
     */
    if (access(IRCUT_SYSFS0_FILE_NAME, F_OK) == 0) {
        ir_ctrl.sysfs_ircut0 = 1;
        ir_ctrl.init_flag = 1;
        log_info("ircut init OK (sysfs %s day/night)", IRCUT_SYSFS0_FILE_NAME);
        return 0;
    }
    ir_ctrl.sysfs_ircut0 = 0;

    if (NULL == cfg_path) {
        log_error("cfg_path NULL.");
        return -1;
    }

    if (read_conf(cfg_path)) {
        log_error("read conf failed.");
        return -1;
    }

    int flags = 0;
    int ret;

    if (0 == strcmp(ircut_a, "null")) {
        log_info("not use ircut_a");
        flags |= 1;
    } else {
        ir_ctrl.ircut_name = IRCUT_A_FILE_NAME;
    }

    if (0 == strcmp(ircut_b, "null")) {
        log_info("not use ircut_b");
        flags |= 1 << 1;
    } else {
        ir_ctrl.ircut_name = IRCUT_B_FILE_NAME;
    }

    switch (flags) {
    case 0:
        log_info("Ircut a & b interface all can access");
        ir_ctrl.ircut_line_mode = IRCUT_MODE_2LINE;
        break;
    case 3:
        log_error("Ircut a & b interface can't access");
        return -1;
    default:
        log_info("Only can access:%s", ir_ctrl.ircut_name);
        ir_ctrl.ircut_line_mode = IRCUT_MODE_1LINE;
        break;
    }

    if (IRCUT_MODE_2LINE == ir_ctrl.ircut_line_mode) {
        ret = ircut_gpio_set(IRCUT_A_FILE_NAME);
        if (ret) {
            log_error("%s can't set gpio.", IRCUT_A_FILE_NAME);
            return ret;
        }

        ret = ircut_gpio_set(IRCUT_B_FILE_NAME);
        if (ret) {
            log_error("%s can't set gpio.", IRCUT_B_FILE_NAME);
            return ret;
        }
    } else {  /* 1 line mode */
        ret = ircut_gpio_set(ir_ctrl.ircut_name);
        if (ret) {
            log_error("%s can't set gpio.", ir_ctrl.ircut_name);
            return ret;
        }
    }

    ir_ctrl.init_flag = 1;
    log_info("ircut init OK, mode %s.",
             ir_ctrl.ircut_line_mode == IRCUT_MODE_1LINE ? "1 line" : "2 line");

    return 0;
}

int hal_ir_set_ircut(int status)
{
    int day_level = 0;
    int level = 0;

    if (!ir_ctrl.init_flag) {
        log_error("ircut not inited");
        return -1;
    }

    if ((0 != status) && (1 != status)) {
        log_error("invalid arg");
        return -1;
    }

    if (ir_ctrl.sysfs_ircut0) {
        const char *s = (status == 0) ? "day" : "night";
        if (file_write_value(IRCUT_SYSFS0_FILE_NAME, s)) {
            log_error("%s write %s failed", IRCUT_SYSFS0_FILE_NAME, s);
            return -1;
        }
        log_debug("ircut sysfs0 -> %s", s);
        return 0;
    }

    if (IRCUT_MODE_1LINE == ir_ctrl.ircut_line_mode) {
        log_debug("Ircut 1line mode, ir_feed:%d", status);
        if (0 == strcmp(IRCUT_A_FILE_NAME, ir_ctrl.ircut_name)) {
            day_level = ircut_a_day_level;
        } else {
            day_level = ircut_b_day_level;
        }

        if (0 == day_level) {
            level = status;
        } else {
            level = !status;
        }
        return camera_set_ircut(ir_ctrl.ircut_name, level);
    }

    log_debug("Ircut 2line mode, ir_feed:%d", status);

    day_level = ircut_a_day_level;
    if (0 == day_level)
        level = status;
    else
        level = !status;

    camera_set_ircut(IRCUT_A_FILE_NAME, level);

    day_level = ircut_b_day_level;
    if (0 == day_level)
        level = status;
    else
        level = !status;
    camera_set_ircut(IRCUT_B_FILE_NAME, level);

    usleep(ircut_time * 1000);
    /* Set IRCUT to idle state */
    camera_set_ircut(IRCUT_A_FILE_NAME, 0);
    camera_set_ircut(IRCUT_B_FILE_NAME, 0);

    return 0;
}

void hal_ir_set_ircut_time(int time_ms)
{
    ircut_time = time_ms;
}

int hal_ir_get_ircut_time(int* time_ms)
{
    if (NULL == time_ms) {
        log_error("time_ms is NULL");
        return -1;
    }

    *time_ms = ircut_time;
    return 0;
}

/*===========================================================================
 * IRLED HAL Interface Implementation
 *===========================================================================*/

int hal_irled_init(const hal_irled_param_t* param)
{
    int ret = access(IRLED_FILE_NAME, F_OK);
    if (ret) {
        log_error("access %s error %d", IRLED_FILE_NAME, ret);
        return -1;
    }

    if (param) {
        irled_info.working_level = param->working_level ? 1 : 0;
    } else {
        irled_info.working_level = 1;  /* Default high level active */
    }

    irled_info.init = 1;

    log_info("irled init OK, working_level:%d", irled_info.working_level);
    return 0;
}

int hal_irled_get_status(int* status)
{
    char value_str[8] = {0};
    int level;

    if (irled_info.init != 1) {
        log_error("irled not init.");
        return -1;
    }

    if (!status) {
        log_error("status is NULL");
        return -1;
    }

    if (file_read_value(IRLED_FILE_NAME, value_str, 8)) {
        log_error("%s can't get", IRLED_FILE_NAME);
        return -1;
    }
    level = atoi(value_str);

    /* Invert to logical on/off if the LED is wired low-active. */
    *status = (irled_info.working_level == 0) ? !level : level;
    return 0;
}

int hal_irled_set_status(int status)
{
    char value_str[8] = {0};
    int level;

    if (irled_info.init != 1) {
        log_error("irled not init.");
        return -1;
    }

    if ((0 != status) && (1 != status)) {
        log_error("invalid arg");
        return -1;
    }

    /*
     * working_level describes the hardware-active polarity of the LED:
     *   1 = active high (default): driving "1" turns the LED ON
     *   0 = active low:           driving "0" turns the LED ON
     * Without this remap a low-active board would appear inverted.
     */
    if (irled_info.working_level == 0) {
        level = !status;
    } else {
        level = status;
    }

    snprintf(value_str, sizeof(value_str), "%d", level);

    if (file_write_value(IRLED_FILE_NAME, value_str)) {
        log_error("%s can't set", IRLED_FILE_NAME);
        return -1;
    }

    return 0;
}

