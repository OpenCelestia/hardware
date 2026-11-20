// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2024 Anyka(GuangZhou) Microelectronics Co., Ltd.
 * @VERSION  1.0
*/

#include "hardware.h"

#if CONFIG_OS_LINUX
#include <linux/kernel.h>
#endif
#include <string.h>
#include <stdlib.h>
#include <limits.h>

#include "list.h"
#include "pthread.h"

#include "ak_common.h"
#include "log.h"

#define LOG_TAG "HARDWARE"

#include "header.h"

#ifndef NAME_MAX
#define NAME_MAX         255    /* # chars in a file name */
#endif

typedef struct module_node_t {
    /* module information */
    const struct hw_module_t* module;
    /* head of the list */
    struct list_head list;
}module_node_t;

typedef struct module_list_t {
    /* mutex of the list */
    pthread_mutex_t mutex;
    /* head of the list */
    struct list_head list;
    /* count of nodes in the list */
    uint32_t count;
}module_list_t;

/**
* @brief   initialize hardware sub modules.
* @param   void
* @return  int
* @retval  AK_SUCCESS: success; AK_FAILED: failed.
**/
static int hw_modules_init(void);

static module_list_t *s_modules = NULL;

/**
* @brief   initialize hardware module.
* @param   void
* @return  int
* @retval  AK_SUCCESS: success; AK_FAILED: failed.
**/
int hw_module_init(void)
{
    s_modules = (module_list_t *)malloc(sizeof(module_list_t));
    memset((void *)s_modules, 0, sizeof(module_list_t));

    int ret = AK_FAILED;
    ret = pthread_mutex_init(&(s_modules->mutex), NULL);
    if (0 != ret)
    {
        free(s_modules);
        log_error("initialize mutex for list object failed");
        return AK_FAILED;
    }

    INIT_LIST_HEAD(&s_modules->list);

    hw_modules_init();

    return AK_SUCCESS;
}

/**
* @brief   register a module info.
* @param   [IN]module: module info
* @return  int
* @retval  AK_SUCCESS: success; AK_FAILED: failed.
**/
int hw_register_module(const struct hw_module_t *module)
{
    if ((NULL == module) || (HARDWARE_MODULE_TAG != module->tag)|| (NULL == module->id))
    {
        log_error("parameter error");
        return AK_FAILED;
    }

    module_list_t *modules = s_modules;
    if (NULL == modules)
    {
        log_error("module hadn't been initialized");
        return AK_FAILED;
    }

    module_node_t *node = (module_node_t *)malloc(sizeof(module_node_t));
    if (NULL == node)
    {
        log_error("malloc for device failed");
        return AK_FAILED;
    }

    node->module = module;

    pthread_mutex_lock(&(modules->mutex));

    list_add_tail(&node->list, &modules->list);
    modules->count++;

    pthread_mutex_unlock(&(modules->mutex));

    return AK_SUCCESS;
}

/**
* @brief   unregister a module info.
* @param   [IN]module: module info
* @return  int
* @retval  AK_SUCCESS: success; AK_FAILED: failed.
**/
int hw_unregister_module(const struct hw_module_t *module)
{
    int ret = AK_FAILED;

    if ((NULL == module) || (HARDWARE_MODULE_TAG != module->tag) || (NULL == module->id))
    {
        log_error("parameter error");
        return AK_FAILED;
    }

    module_list_t *modules = s_modules;
    if (NULL == modules)
    {
        log_error("module hadn't been initialized");
        return ret;
    }

    pthread_mutex_lock(&(modules->mutex));

    if (list_empty(&(modules->list)))
    {
        pthread_mutex_unlock(&(modules->mutex));
        return ret;
    }

    struct list_head *pos, *next;
    module_node_t *node = NULL;

    list_for_each_safe(pos, next, &(modules->list))
    {
        node = list_entry(pos, module_node_t, list);
        if (0 == strcmp(module->id, node->module->id))
        {
            list_del(&node->list);
            free(node);

            if (modules->count > 0)
                modules->count--;

            ret = AK_SUCCESS;
            break;
        }
    }

    pthread_mutex_unlock(&(modules->mutex));

    return ret;
}

/**
 * Get the module info associated with a module by id.
 *
 * @return: 0 == success, <0 == error and *module == NULL
 */
int hw_get_module(const char *id, const struct hw_module_t **module)
{
    return hw_get_module_by_class(id, NULL, module);
}

/**
 * Get the module info associated with a module instance by class 'class_id'
 * and instance 'inst'.
 *
 * Some modules types necessitate multiple instances. For example audio supports
 * multiple concurrent interfaces and thus 'audio' is the module class
 * and 'primary' or 'a2dp' are module interfaces. This implies that the files
 * providing these modules would be named audio.primary.<variant>.so and
 * audio.a2dp.<variant>.so
 *
 * @return: 0 == success, <0 == error and *module == NULL
 */
int hw_get_module_by_class(const char *class_id, const char *inst,
                           const struct hw_module_t **module)
{
    int ret = AK_FAILED;

    if ((NULL == class_id) || (NULL == module))
    {
        log_error("parameter error");
        return AK_FAILED;
    }

    *module = NULL;

    module_list_t *modules = s_modules;
    if (NULL == modules)
    {
        log_error("module hadn't been initialized");
        return ret;
    }

    char name[NAME_MAX] = {0};
    if (inst)
        snprintf(name, NAME_MAX, "%s.%s", class_id, inst);
    else
        snprintf(name, NAME_MAX, "%s", class_id);

    pthread_mutex_lock(&(modules->mutex));

    if (list_empty(&(modules->list)))
    {
        pthread_mutex_unlock(&(modules->mutex));
        return ret;
    }

    struct list_head *pos, *next;
    module_node_t *node = NULL;

    list_for_each_safe(pos, next, &(modules->list))
    {
        node = list_entry(pos, module_node_t, list);
        if (0 == strcmp(name, node->module->id))
        {
            *module = node->module;
            ret = AK_SUCCESS;
            break;
        }
    }

    pthread_mutex_unlock(&(modules->mutex));

    return ret;
}

/**
* @brief   initialize hardware sub modules.
* @param   void
* @return  int
* @retval  AK_SUCCESS: success; AK_FAILED: failed.
**/
static int hw_modules_init(void)
{
    /* initialize hardware module */
#if CONFIG_HW_DRIVER_ADC
    hw_adc_module_init();
#endif

#if CONFIG_HW_DRIVER_GPIO
    hw_gpio_module_init();
#endif

#if CONFIG_HW_DRIVER_MMC
    hw_mmc_module_init();
#endif

#if CONFIG_HW_DRIVER_MOTOR
    hw_motor_module_init();
#endif

#if CONFIG_HW_DRIVER_PWM
    hw_pwm_module_init();
#endif

#if CONFIG_HW_DRIVER_EFUSE
    hw_efuse_module_init();
#endif

#if CONFIG_HW_DRIVER_WDT
    hw_wdt_module_init();
#endif


#if CONFIG_HW_INPUT
    hw_evdev_module_init();
#endif

#if CONFIG_HW_DRIVER_LED
    hw_led_module_init();
#endif

#if CONFIG_HW_DRIVER_IR
    hw_ir_module_init();
#endif

#if CONFIG_HW_GPU
    hw_gpu_module_init();
#endif

#if CONFIG_HW_CAMERA
    hw_camera_module_init();
#endif

#if CONFIG_HW_AUDIO
    hw_audio_module_init();
#endif

#if CONFIG_HW_DISPLAY
    hw_display_module_init();
#endif

#if CONFIG_HW_OSD
    hw_osd_module_init();
#endif

#if CONFIG_HW_WIFI
    hw_wifi_module_init();
#endif

#if CONFIG_HW_ETHERNET
    hw_ethernet_module_init();
#endif

#if CONFIG_HW_VISION_ISP
    hw_isp_module_init();
#endif

#if CONFIG_HW_VISION_MOTION_DETECT
    hw_motion_detect_module_init();
#endif

    return AK_SUCCESS;
}
