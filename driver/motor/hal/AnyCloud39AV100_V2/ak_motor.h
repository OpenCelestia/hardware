// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/driver/motor.
 *
 * Motor driver HAL header for AnyCloud39AV100_V2 platform.
 * - Defines motor control ioctl commands and data structures.
 * - Provides motor status, parameters, and message definitions.
 */

#ifndef HARDWARE_AK_MOTOR_HAL_ANYCLOUD39AV100_V2_AK_MOTOR_H
#define HARDWARE_AK_MOTOR_HAL_ANYCLOUD39AV100_V2_AK_MOTOR_H

#include <linux/ioctl.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AK_MOTOR_IOC_MAGIC 		'm'

/*===========================================================================
 * Motor IOCTL Commands - Startup Arguments
 *===========================================================================*/

/** @brief Set motor parameters */
#define MOTOR_PARM			_IOW(AK_MOTOR_IOC_MAGIC, 0x00, int)

/*===========================================================================
 * Motor IOCTL Commands - Real-time Arguments
 *===========================================================================*/

/** @brief Set speed in step (Hz) */
#define MOTOR_SPEED_STEP	_IOW(AK_MOTOR_IOC_MAGIC, 0x20, int)

/** @brief Set speed in angle */
#define MOTOR_SPEED_ANGLE	_IOW(AK_MOTOR_IOC_MAGIC, 0x21, int)

/*===========================================================================
 * Motor IOCTL Commands - Base Commands
 *===========================================================================*/

/**
 * @brief Move clockwise or anticlockwise with limit.
 * @details Clockwise: val > 0, anticlockwise: val < 0
 */
#define MOTOR_MOVE_LIMIT	_IOW(AK_MOTOR_IOC_MAGIC, 0x40, int)

/**
 * @brief Move clockwise or anticlockwise without limit.
 * @details Clockwise: val > 0, anticlockwise: val < 0
 */
#define MOTOR_MOVE_NOLIMIT	_IOW(AK_MOTOR_IOC_MAGIC, 0x41, int)

/** @brief Set stop and wait for stop to finish */
#define MOTOR_STOP			_IOW(AK_MOTOR_IOC_MAGIC, 0x42, int)

/** @brief Get motor status */
#define MOTOR_GET_STATUS	_IOW(AK_MOTOR_IOC_MAGIC, 0x43, int)

/*===========================================================================
 * Motor IOCTL Commands - Extended Commands
 *===========================================================================*/

/** @brief First go to actual boundary then go to middle */
#define MOTOR_RESET			_IOW(AK_MOTOR_IOC_MAGIC, 0x60, int)

/** @brief Turn to middle directly */
#define MOTOR_MIDDLE		_IOW(AK_MOTOR_IOC_MAGIC, 0x61, int)

/** @brief Turn clockwise and anticlockwise until set stop */
#define MOTOR_CRUISE		_IOW(AK_MOTOR_IOC_MAGIC, 0x62, int)

/**
 * @brief Move to actual boundary.
 * @details Move in clockwise or anticlockwise until stopped
 */
#define MOTOR_BOUNDARY		_IOW(AK_MOTOR_IOC_MAGIC, 0x63, int)

/*===========================================================================
 * Type Definitions
 *===========================================================================*/

/**
 * @brief Motor status enumeration.
 */
enum motor_status {
	MOTOR_IS_STOP = 0,        /**< Motor is stopped now */
	MOTOR_IS_RUNNING,          /**< Motor is running now */
};

/**
 * @brief Motor parameters structure.
 */
struct motor_parm {
	int pos;                   /**< Current position */
	int speed_step;            /**< Speed of step (Hz) */
	int steps_one_circle;      /**< Steps per circle */
	int total_steps;           /**< Total steps motor can run */
	int boundary_steps;        /**< Reserved boundary steps */
};

/**
 * @brief Motor message structure.
 */
struct motor_message {
	enum motor_status status;  /**< Motor working status */
	int pos;                   /**< Current position */
	int speed_step;            /**< Speed of step (Hz) */
	int speed_angle;          /**< Speed of angle */
	int steps_one_circle;      /**< Steps per circle */
	int total_steps;           /**< Total steps motor can run */
	int boundary_steps;        /**< Reserved boundary steps */
	int attach_timer;         /**< Attach to hardware timer */
};

/**
 * @brief Motor reset data structure.
 */
struct motor_reset_data {
	int total_steps;           /**< Total steps */
	int cur_step;              /**< Current step */
};

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_AK_MOTOR_HAL_ANYCLOUD39AV100_V2_AK_MOTOR_H */
