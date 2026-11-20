// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Weak fallback when hal_ipu.c is not linked (no GPU/camera).
 * Overridden by the strong symbol in hal_ipu.c when IPU HAL is built.
 */

#include "hal_ipu_api.h"

__attribute__((weak)) int hal_ipu_open(void)
{
    return -1;
}

__attribute__((weak)) int hal_ipu_close(void)
{
    return 0;
}

__attribute__((weak)) int hal_ipu_force_close(void)
{
    return 0;
}
