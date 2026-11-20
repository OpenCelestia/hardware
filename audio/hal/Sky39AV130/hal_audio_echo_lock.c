// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * libakaudiofilter is not reentrant. Talk downlink (ak_ao_send_frame ->
 * _SD_Echo_FillFarStream), AoPlayPcmThread (_SD_Echo_GetDacStream) and
 * AiCapturePcmThread (_SD_Echo_FillAdcStream) enter it together and fault
 * at _SD_Filter_Control+0x3b0 (fault addr 0x18), often right after "realloc"
 * and "GetDacStream is locked by 0x8".
 *
 * hardware/Makefile --wrap binds the libplat_*.a calls to __wrap_* below.
 * __real_* is the copy in libakaudiofilter.so. The lock is held across that
 * call, so an internal Filter_Control in the .so does not run beside another
 * thread's Fill/Get. Do not SIGSTOP and do not raise SCHED_FIFO.
 */

#include <pthread.h>

#include "log.h"

#define LOG_TAG "HAL AUDIO"

static pthread_mutex_t echo_lock;
static pthread_once_t echo_once = PTHREAD_ONCE_INIT;
static void *echo_lr_stack[16];
static int echo_lr_depth;

static void echo_lock_init(void)
{
    pthread_mutexattr_t attr;

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&echo_lock, &attr);
    pthread_mutexattr_destroy(&attr);
    log_info("echo lock: serialize FillFar/GetDac/FillAdc/Filter_Control");
}

void hal_echo_sd_enter(void *caller_lr)
{
    pthread_once(&echo_once, echo_lock_init);
    pthread_mutex_lock(&echo_lock);
    if (echo_lr_depth < (int)(sizeof(echo_lr_stack) / sizeof(echo_lr_stack[0])))
        echo_lr_stack[echo_lr_depth] = caller_lr;
    echo_lr_depth++;
}

void *hal_echo_sd_leave(void)
{
    void *caller_lr = NULL;

    if (echo_lr_depth > 0) {
        echo_lr_depth--;
        if (echo_lr_depth < (int)(sizeof(echo_lr_stack) / sizeof(echo_lr_stack[0])))
            caller_lr = echo_lr_stack[echo_lr_depth];
    }
    pthread_mutex_unlock(&echo_lock);
    return caller_lr;
}

/*
 * FillFarStream and GetDacStream take a 5th argument on the stack, so sp
 * must be back to the caller's value before the real call. r0-r3 are
 * restored. The real function's return value is kept in the stacked r0.
 * Stay in the compiler's instruction set: this toolchain rejects
 * target("arm").
 */
#define ECHO_WRAP(sym)                                                         \
    void __real_##sym(void);                                                   \
    __attribute__((naked)) void __wrap_##sym(void)                             \
    {                                                                          \
        __asm__ volatile(                                                      \
            "push {r0-r3}\n\t"                                                 \
            "mov r0, lr\n\t"                                                   \
            "bl hal_echo_sd_enter\n\t"                                         \
            "pop {r0-r3}\n\t"                                                  \
            "bl __real_" #sym "\n\t"                                           \
            "push {r0, r4}\n\t"                                                \
            "bl hal_echo_sd_leave\n\t"                                         \
            "mov lr, r0\n\t"                                                   \
            "pop {r0, r4}\n\t"                                                 \
            "bx lr\n\t");                                                      \
    }

ECHO_WRAP(_SD_Echo_FillFarStream)
ECHO_WRAP(_SD_Echo_GetDacStream)
ECHO_WRAP(_SD_Echo_FillAdcStream)
ECHO_WRAP(_SD_Echo_FillDacLoopback)
ECHO_WRAP(_SD_Echo_GetResult)
ECHO_WRAP(_SD_Filter_Control)
#undef ECHO_WRAP
