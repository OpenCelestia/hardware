// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2025 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/hardware/camera.
 *
 * Tile-based detection algorithm header file.
 * - Splits input area into multiple fixed-size regions for detection.
 * - Merges final detection boxes from multiple tiles.
 */

#ifndef HARDWARE_HAL_CAMERA_AI_TILE_OVERLAP_HAL_ANYCLOUD39AV200_HAL_CAMERA_AI_TILE_OVERLAP_H
#define HARDWARE_HAL_CAMERA_AI_TILE_OVERLAP_HAL_ANYCLOUD39AV200_HAL_CAMERA_AI_TILE_OVERLAP_H

#include "ak_svp.h"

#ifdef __cplusplus
extern "C" {
#endif

//#define DEBUG_HAL_AI_CAMERA_OVERLAP

/**
 * @brief Main function for tile-based detection algorithm
 * @param input_width Input image width (before splitting)
 * @param input_height Input image height (before splitting)
 * @param detect_width Detection area width
 * @param detect_height Detection area height
 * @param num_rows Number of tile rows
 * @param num_cols Number of tile columns
 * @param overlap_ratio Tile overlap ratio (0.0-1.0)
 * @param hd_results Detection result array, containing num_rows*num_cols tile results and 1 full image result
 * @param final_result Final detection result
 * @return 0 success, -1 failure
 */
int hal_camera_ai_tile_overlap_detection(int input_width, int input_height,
                                        int tile_width, int tile_height,
                                        int num_rows, int num_cols, float overlap_ratio,
                                        SVP_OUTPUT_S *hd_output);

/**
 * @brief Calculate input image size based on tile parameters
 * @param tile_width Width of each tile
 * @param tile_height Height of each tile
 * @param num_rows Number of tile rows
 * @param num_cols Number of tile columns
 * @param overlap_ratio Tile overlap ratio (0.0-1.0)
 * @param input_width Output: Calculated input image width
 * @param input_height Output: Calculated input image height
 * @return 0 success, -1 failure
 */
int hal_camera_ai_tile_overlap_calculate_input_size(int tile_width, int tile_height,
                                      int num_rows, int num_cols, float overlap_ratio,
                                      int *input_width, int *input_height);

/**
 * @brief Calculate start coordinates for specified row and column tile
 * @param row Tile row number (starting from 0)
 * @param col Tile column number (starting from 0)
 * @param tile_width Width of each tile
 * @param tile_height Height of each tile
 * @param overlap_ratio Tile overlap ratio (0.0-1.0)
 * @param start_x Output: Starting X coordinate of tile in original image
 * @param start_y Output: Starting Y coordinate of tile in original image
 * @return 0 success, -1 failure
 */
int hal_camera_ai_calculate_tile_start_coords(int row, int col,
                                             int tile_width, int tile_height,
                                             float overlap_ratio,
                                             int *start_x, int *start_y);

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_HAL_CAMERA_AI_TILE_OVERLAP_HAL_ANYCLOUD39AV200_HAL_CAMERA_AI_TILE_OVERLAP_H */
