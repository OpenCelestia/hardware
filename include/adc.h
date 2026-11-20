// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/driver/adc.
 *
 * ADC driver module public interface.
 * - Provides Android-style HAL interface for ADC operations.
 * - Supports reading analog values from ADC channels.
 */
#ifndef HARDWARE_ADC_H
#define HARDWARE_ADC_H

#include <stdint.h>
#include <sys/cdefs.h>

#include "hardware.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * MODULE IDENTIFICATION
 * ============================================================================ */
#define ADC_MODULE_API_VERSION_0_1      HARDWARE_MODULE_API_VERSION(0, 1)
#define ADC_DEVICE_API_VERSION_1_0      HARDWARE_DEVICE_API_VERSION(1, 0)
#define ADC_HARDWARE_MODULE_ID          "adc"

/* ============================================================================
 * ADC MODULE STRUCTURE
 * ============================================================================ */

/**
 * @brief ADC module structure.
 */
typedef struct adc_module {
    struct hw_module_t common;
} adc_module_t;

/* ============================================================================
 * ADC DEVICE INTERFACE
 * ============================================================================ */

/**
 * @brief ADC device structure.
 * @details Provides function pointers for ADC operations.
 */
typedef struct adc_device {
    struct hw_device_t common;

    /**
     * @brief Read value from ADC channel.
     * @param device Device context.
     * @param name ADC channel name.
     * @param buffer Buffer to store the value.
     * @param bytes Number of bytes to read.
     * @return 0 on success, negative error code on failure.
     */
    int (*read)(struct adc_device *device, const char* name, uint8_t *buffer, uint32_t bytes);
} adc_device_t;

/* ============================================================================
 * MODULE FUNCTIONS
 * ============================================================================ */

/**
 * @brief Initialize ADC module.
 * @return 0 on success, negative error code on failure.
 */
int hw_adc_module_init(void);

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_ADC_H */

