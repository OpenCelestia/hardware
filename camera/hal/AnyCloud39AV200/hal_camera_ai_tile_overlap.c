/**
 * @file hal_camera_ai_tile_overlap.c
 * @brief Tile-based detection algorithm implementation
 * @details Split input area into multiple fixed-size regions for detection, then merge final detection boxes
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ak_common.h"
#include "ak_log.h"
#include "ak_svp.h"
#include "ak_mem.h"
#include "hal_camera_ai_tile_overlap.h"
#include "log.h"
#define LOG_TAG "HAL CAMERA AI"
/*
 * Tuning macros for de-duplication behavior
 * How close two scores must be to enable aggressive merging (units: score points)
 */
#define SCORE_DIFF_STRICT_THRESHOLD 20

/* Simple center-distance and size-similarity thresholds (no IoU)
 * When scores are within threshold (strict), allow larger center distance and looser size similarity
 */
#define CENTER_THR_FACTOR_STRICT 0.75f    // center distance <= 75% of min(width,height)
#define CENTER_THR_FACTOR_BASE   0.45f    // center distance <= 45% of min(width,height)
#define SIZE_RATIO_STRICT        0.40f    // min/max side ratio >= 0.4
#define SIZE_RATIO_BASE          0.60f    // min/max side ratio >= 0.6
/* Area similarity ratios (min/max area ratio) */
#define AREA_RATIO_STRICT        0.50f
#define AREA_RATIO_BASE          0.70f

/* IoU thresholds for common NMS rule */
#define IOU_BASE_THRESHOLD       0.30f
#define IOU_RELAXED_THRESHOLD    0.10f

/* Legacy tuning macros kept for compatibility (IoU related) */
#define IOU_RELAX_FACTOR_BASE    0.35f
#define IOU_MIN_THRESHOLD        0.02f
#define IOU_MIN_THRESHOLD_STRICT 0.01f
#define IOU_MAX_THRESHOLD        0.70f

/* Center factor dynamic (legacy) */
#define CENTER_FACTOR_BASE       0.40f
#define CENTER_FACTOR_GAIN       0.40f
#define CENTER_FACTOR_MAX        0.95f

/* Strict range multiplier (legacy) */
#define STRICT_RANGE_MULTIPLIER  5.0f

/* Normalized center Manhattan distance thresholds (legacy) */
#define CLOSE_NORM_BASE          0.60f
#define CLOSE_NORM_MAX           0.95f

/* Size similar legacy thresholds */
#define SIZE_SIMILAR_RATIO               0.50f
#define SIZE_SIMILAR_CENTER_FACTOR_BASE  0.55f

/* Macro definitions for maximum and minimum values */
#define GET_MAX(a, b) ((a) > (b) ? (a) : (b))
#define GET_MIN(a, b) ((a) < (b) ? (a) : (b))

/**
 * @brief Calculate overlap ratio between two detection boxes
 * @param box1 Detection box 1
 * @param box2 Detection box 2
 * @return Overlap ratio (0.0-1.0)
 */
static float calculate_overlap(SVP_RECT_S *box1, SVP_RECT_S *box2)
{
    int x1 = GET_MAX(box1->u16Left, box2->u16Left);
    int y1 = GET_MAX(box1->u16Top, box2->u16Top);
    int x2 = GET_MIN(box1->u16Right, box2->u16Right);
    int y2 = GET_MIN(box1->u16Bottom, box2->u16Bottom);

    if (x2 <= x1 || y2 <= y1) {
        return 0.0f;
    }

    int intersection_area = (x2 - x1) * (y2 - y1);
    int area1 = (box1->u16Right - box1->u16Left) * (box1->u16Bottom - box1->u16Top);
    int area2 = (box2->u16Right - box2->u16Left) * (box2->u16Bottom - box2->u16Top);
    int union_area = area1 + area2 - intersection_area;
    if (union_area <= 0) {
        return 0.0f;
    }
    /* Use smaller area for overlap ratio calculation */
    int area = GET_MIN(area1, area2);
    return (float)intersection_area / (float)area;
}

static float score_similarity_ratio(AK_U32 sa, AK_U32 sb)
{
    AK_U32 maxv = sa > sb ? sa : sb;
    if (maxv == 0) return 1.0f;
    AK_U32 diff = sa > sb ? (sa - sb) : (sb - sa);
    return 1.0f - ((float)diff / (float)maxv); // 1.0 means identical
}

static int centers_close_size_similar(SVP_RECT_S *a, SVP_RECT_S *b, int strict)
{
    int ax = ((int)a->u16Left + (int)a->u16Right) / 2;
    int ay = ((int)a->u16Top + (int)a->u16Bottom) / 2;
    int bx = ((int)b->u16Left + (int)b->u16Right) / 2;
    int by = ((int)b->u16Top + (int)b->u16Bottom) / 2;
    int dx = ax - bx; if (dx < 0) dx = -dx;
    int dy = ay - by; if (dy < 0) dy = -dy;
    int aw = (int)a->u16Right - (int)a->u16Left;
    int ah = (int)a->u16Bottom - (int)a->u16Top;
    int bw = (int)b->u16Right - (int)b->u16Left;
    int bh = (int)b->u16Bottom - (int)b->u16Top;
    int refw = GET_MIN(aw, bw);
    int refh = GET_MIN(ah, bh);
    float center_factor = strict ? CENTER_THR_FACTOR_STRICT : CENTER_THR_FACTOR_BASE;
    int thrx = (int)(center_factor * (float)refw);
    int thry = (int)(center_factor * (float)refh);
    if (dx > thrx || dy > thry) return 0;

    float wr = (float)GET_MIN(aw, bw) / (float)GET_MAX(aw, bw);
    float hr = (float)GET_MIN(ah, bh) / (float)GET_MAX(ah, bh);
    float area_a = (float)aw * (float)ah;
    float area_b = (float)bw * (float)bh;
    float area_ratio = (area_a < area_b ? area_a / area_b : area_b / area_a);
    float size_ratio = strict ? SIZE_RATIO_STRICT : SIZE_RATIO_BASE;
    float area_ratio_thr = strict ? AREA_RATIO_STRICT : AREA_RATIO_BASE;
    return (wr >= size_ratio) && (hr >= size_ratio) && (area_ratio >= area_ratio_thr);
}

/**
 * @brief Simple center proximity check with scale factor
 * @param a first box
 * @param b second box
 * @param factor threshold factor in [0,1], compares to min(width,height)
 * @return 1 if centers are within factor*min(width,height) in both axes
 */
static int are_centers_close(SVP_RECT_S *a, SVP_RECT_S *b, float factor)
{
    int ax = ((int)a->u16Left + (int)a->u16Right) / 2;
    int ay = ((int)a->u16Top + (int)a->u16Bottom) / 2;
    int bx = ((int)b->u16Left + (int)b->u16Right) / 2;
    int by = ((int)b->u16Top + (int)b->u16Bottom) / 2;
    int dx = ax - bx; if (dx < 0) dx = -dx;
    int dy = ay - by; if (dy < 0) dy = -dy;
    int aw = (int)a->u16Right - (int)a->u16Left;
    int ah = (int)a->u16Bottom - (int)a->u16Top;
    int bw = (int)b->u16Right - (int)b->u16Left;
    int bh = (int)b->u16Bottom - (int)b->u16Top;
    int refw = GET_MIN(aw, bw);
    int refh = GET_MIN(ah, bh);
    int thrx = (int)(factor * (float)refw);
    int thry = (int)(factor * (float)refh);
    return dx <= thrx && dy <= thry;
}

static int are_boxes_close(SVP_RECT_S *a, SVP_RECT_S *b, float norm_thr)
{
    // Manhattan distance between centers normalized by min(diagonal)
    int ax = ((int)a->u16Left + (int)a->u16Right) / 2;
    int ay = ((int)a->u16Top + (int)a->u16Bottom) / 2;
    int bx = ((int)b->u16Left + (int)b->u16Right) / 2;
    int by = ((int)b->u16Top + (int)b->u16Bottom) / 2;
    int dx = ax - bx; if (dx < 0) dx = -dx;
    int dy = ay - by; if (dy < 0) dy = -dy;
    int aw = (int)a->u16Right - (int)a->u16Left;
    int ah = (int)a->u16Bottom - (int)a->u16Top;
    int bw = (int)b->u16Right - (int)b->u16Left;
    int bh = (int)b->u16Bottom - (int)b->u16Top;
    float dia_a = sqrtf((float)aw * (float)aw + (float)ah * (float)ah);
    float dia_b = sqrtf((float)bw * (float)bw + (float)bh * (float)bh);
    float refd = dia_a < dia_b ? dia_a : dia_b;
    if (refd <= 1.0f) refd = 1.0f;
    float manhattan = (float)(dx + dy);
    return (manhattan / refd) <= norm_thr;
}

/**
 * @brief Check whether two boxes are exactly identical (coords and score)
 */
static int boxes_equal(const SVP_RECT_S *a, const SVP_RECT_S *b)
{
    return (a->u16Left == b->u16Left) && (a->u16Top == b->u16Top) &&
           (a->u16Right == b->u16Right) && (a->u16Bottom == b->u16Bottom) &&
           (a->u64Score == b->u64Score);
}

/**
 * @brief Cross-compare all outputs (tiles + full) and suppress exactly equal duplicates
 *        Keeps the higher-score one (if equal scores, keeps the first) and sets the other score to 0
 *        Also expands the kept one's bounds to the union (safe no-op when identical).
 */
static void dedup_all_outputs_equal(SVP_HD_RESULT_S *results, int result_count)
{
    if (!results || result_count <= 1) return;
    for (int oi = 0; oi < result_count; ++oi) {
        SVP_HD_RESULT_S *ri = &results[oi];
        if (ri->s32TotalNum <= 0 || !ri->pstTargetBoxes) continue;
        for (int oj = oi + 1; oj < result_count; ++oj) {
            SVP_HD_RESULT_S *rj = &results[oj];
            if (rj->s32TotalNum <= 0 || !rj->pstTargetBoxes) continue;
            for (int ia = 0; ia < ri->s32TotalNum; ++ia) {
                SVP_RECT_S *a = &ri->pstTargetBoxes[ia];
                for (int jb = 0; jb < rj->s32TotalNum; ++jb) {
                    SVP_RECT_S *b = &rj->pstTargetBoxes[jb];
                    if (boxes_equal(a, b)) {
                        // choose keeper by score (equal => keep a)
                        if (b->u64Score > a->u64Score) {
                            // expand b with a, keep b's score; suppress a
                            b->u16Left   = GET_MIN(a->u16Left,   b->u16Left);
                            b->u16Top    = GET_MIN(a->u16Top,    b->u16Top);
                            b->u16Right  = GET_MAX(a->u16Right,  b->u16Right);
                            b->u16Bottom = GET_MAX(a->u16Bottom, b->u16Bottom);
                            a->u64Score = 0;
                        } else {
                            // expand a with b, keep a's score; suppress b
                            a->u16Left   = GET_MIN(a->u16Left,   b->u16Left);
                            a->u16Top    = GET_MIN(a->u16Top,    b->u16Top);
                            a->u16Right  = GET_MAX(a->u16Right,  b->u16Right);
                            a->u16Bottom = GET_MAX(a->u16Bottom, b->u16Bottom);
                            b->u64Score = 0;
                        }
                    }
                }
            }
        }
    }
}

/* Cross-output deduplication: merge duplicates across multiple outputs in place */
static void merge_across_outputs(SVP_HD_RESULT_S *results, int result_count, float base_overlap_threshold)
{
    if (!results || result_count <= 1) return;
    for (int oi = 0; oi < result_count; ++oi) {
        SVP_HD_RESULT_S *ri = &results[oi];
        if (ri->s32TotalNum <= 0 || !ri->pstTargetBoxes) continue;
        for (int oj = oi + 1; oj < result_count; ++oj) {
            SVP_HD_RESULT_S *rj = &results[oj];
            if (rj->s32TotalNum <= 0 || !rj->pstTargetBoxes) continue;
            for (int ia = 0; ia < ri->s32TotalNum; ++ia) {
                SVP_RECT_S *a = &ri->pstTargetBoxes[ia];
                for (int jb = 0; jb < rj->s32TotalNum; ) {
                    SVP_RECT_S *b = &rj->pstTargetBoxes[jb];
                    float iou = calculate_overlap(a, b);
                    float sim = score_similarity_ratio(a->u64Score, b->u64Score);
                    float dyn_iou = base_overlap_threshold - IOU_RELAX_FACTOR_BASE * sim;
                    if (dyn_iou < IOU_MIN_THRESHOLD) dyn_iou = IOU_MIN_THRESHOLD;
                    if (dyn_iou > IOU_MAX_THRESHOLD) dyn_iou = IOU_MAX_THRESHOLD;
                    float center_factor = CENTER_FACTOR_BASE + CENTER_FACTOR_GAIN * sim; // range ~0.40..0.80

                    AK_U32 sa = a->u64Score, sb = b->u64Score;
                    AK_U32 sdiff = (sa > sb) ? (sa - sb) : (sb - sa);
                    if (sdiff <= SCORE_DIFF_STRICT_THRESHOLD) {
                        dyn_iou *= (1.0f / STRICT_RANGE_MULTIPLIER);
                        if (dyn_iou < IOU_MIN_THRESHOLD_STRICT) dyn_iou = IOU_MIN_THRESHOLD_STRICT;
                        center_factor *= STRICT_RANGE_MULTIPLIER;
                        if (center_factor > CENTER_FACTOR_MAX) center_factor = CENTER_FACTOR_MAX;
                    }

                    float close_norm_thr = CLOSE_NORM_BASE;
                    if (sdiff <= SCORE_DIFF_STRICT_THRESHOLD) { close_norm_thr *= STRICT_RANGE_MULTIPLIER; if (close_norm_thr > CLOSE_NORM_MAX) close_norm_thr = CLOSE_NORM_MAX; }

                    int aw = (int)a->u16Right - (int)a->u16Left;
                    int ah = (int)a->u16Bottom - (int)a->u16Top;
                    int bw = (int)b->u16Right - (int)b->u16Left;
                    int bh = (int)b->u16Bottom - (int)b->u16Top;
                    float wr = (float)GET_MIN(aw, bw) / (float)GET_MAX(aw, bw);
                    float hr = (float)GET_MIN(ah, bh) / (float)GET_MAX(ah, bh);
                    int size_similar = (wr >= SIZE_SIMILAR_RATIO && hr >= SIZE_SIMILAR_RATIO);

                    if (iou >= dyn_iou || are_centers_close(a, b, center_factor) ||
                        are_boxes_close(a, b, close_norm_thr) ||
                        (size_similar && are_centers_close(a, b, (sdiff <= SCORE_DIFF_STRICT_THRESHOLD ? SIZE_SIMILAR_CENTER_FACTOR_BASE * STRICT_RANGE_MULTIPLIER : SIZE_SIMILAR_CENTER_FACTOR_BASE)))) {
                        // Merge b into a
                        a->u16Left   = GET_MIN(a->u16Left,   b->u16Left);
                        a->u16Top    = GET_MIN(a->u16Top,    b->u16Top);
                        a->u16Right  = GET_MAX(a->u16Right,  b->u16Right);
                        a->u16Bottom = GET_MAX(a->u16Bottom, b->u16Bottom);
                        a->u64Score = 0;
                        // remove b from rj
                        rj->pstTargetBoxes[jb] = rj->pstTargetBoxes[rj->s32TotalNum - 1];
                        rj->s32TotalNum--;
                        continue; // re-test swapped-in element
                    }
                    jb++;
                }
            }
        }
    }
}

/* Deduplicate only between full image result and tile results (do not rebuild) */
static void merge_full_vs_tiles(SVP_HD_RESULT_S *full,
                               SVP_HD_RESULT_S *tiles,
                               int tile_count,
                               float base_overlap_threshold)
{
    if (!full || !tiles || tile_count <= 0) return;
    for (int t = 0; t < tile_count; ++t) {
        SVP_HD_RESULT_S *rj = &tiles[t];
        if (rj->s32TotalNum <= 0 || !rj->pstTargetBoxes) continue;
        for (int ia = 0; ia < full->s32TotalNum; ++ia) {
            SVP_RECT_S *a = &full->pstTargetBoxes[ia];
            for (int jb = 0; jb < rj->s32TotalNum; ) {
                SVP_RECT_S *b = &rj->pstTargetBoxes[jb];
                AK_U32 sa = a->u64Score, sb = b->u64Score;
                AK_U32 sdiff = (sa > sb) ? (sa - sb) : (sb - sa);
                int strict = (sdiff <= SCORE_DIFF_STRICT_THRESHOLD) ? 1 : 0;
                if (centers_close_size_similar(a, b, strict)) {
                    // Merge bounds into the better-scoring representative, suppress the other by score=0
                    AK_U32 keep_a = (a->u64Score >= b->u64Score) ? 1u : 0u;
                    if (keep_a) {
                        a->u16Left   = GET_MIN(a->u16Left,   b->u16Left);
                        a->u16Top    = GET_MIN(a->u16Top,    b->u16Top);
                        a->u16Right  = GET_MAX(a->u16Right,  b->u16Right);
                        a->u16Bottom = GET_MAX(a->u16Bottom, b->u16Bottom);
                        // keep a's score
                        b->u64Score = 0;
                    } else {
                        // copy b into a, but expand with a to keep merged bounds
                        SVP_RECT_S merged = *b;
                        merged.u16Left   = GET_MIN(a->u16Left,   b->u16Left);
                        merged.u16Top    = GET_MIN(a->u16Top,    b->u16Top);
                        merged.u16Right  = GET_MAX(a->u16Right,  b->u16Right);
                        merged.u16Bottom = GET_MAX(a->u16Bottom, b->u16Bottom);
                        // keep b's score
                        a->u16Left = merged.u16Left;
                        a->u16Top = merged.u16Top;
                        a->u16Right = merged.u16Right;
                        a->u16Bottom = merged.u16Bottom;
                        a->u64Score = b->u64Score;
                        b->u64Score = 0;
                    }
                }
                jb++;
            }
        }
    }
}

static void process_full_and_tiles(SVP_OUTPUT_S *hd_output,
                                    int tile_row,
                                    int tile_col,
                                    int t_cnt,
                                    float iou_threshold)
{
    SVP_HD_RESULT_S post_result;
    post_result.pstTargetBoxes = (SVP_RECT_S *)AK_BASE_MemAlloc(MODULE_ID_APP, t_cnt * sizeof(SVP_RECT_S));
    int full_idx = tile_row * tile_col;
    int full_num = hd_output[full_idx].result.stHdResult.s32TotalNum;
    int full_flag_arr[full_num];

    memset(full_flag_arr, 0, full_num * sizeof(int));
    post_result.s32TotalNum = full_num;
    memcpy(post_result.pstTargetBoxes, hd_output[full_idx].result.stHdResult.pstTargetBoxes, full_num * sizeof(SVP_RECT_S));

    for (int i = 0; i < full_idx; i ++)
    {
        int t_num = hd_output[i].result.stHdResult.s32TotalNum;
        if (t_num > 0)
        {
            for (int j = 0; j < t_num; j ++)
            {
                int tile_det_label = hd_output[i].result.stHdResult.pstTargetBoxes[j].enLabel;
                int m_found = 0;
                for (int m = i + 1; m < full_idx; m ++)
                {
                    int m_num = hd_output[m].result.stHdResult.s32TotalNum;
                    if (m_num > 0)
                    {
                        for (int n = 0; n < m_num; n ++)
                        {
                            int n_tile_det_label = hd_output[m].result.stHdResult.pstTargetBoxes[n].enLabel;
                            if (tile_det_label == n_tile_det_label)
                            {
                                SVP_RECT_S *a = &hd_output[i].result.stHdResult.pstTargetBoxes[j];
                                SVP_RECT_S *b = &hd_output[m].result.stHdResult.pstTargetBoxes[n];
                                float iou = calculate_overlap(a, b);
#if 0//def DEBUG_HAL_AI_CAMERA_OVERLAP
                                log_info("box[%d][%d] with box[%d][%d] iou: %.2f", i, j, m, n, iou);
#endif
                                if (iou > iou_threshold)
                                {
                                    m_found = 1;
                                }
                            }
                        }
                    }
                }
                if (m_found)
                {
                    continue;
                }
                int found = 0;
                for (int k = 0; k < full_num; k ++)
                {
                    if (full_flag_arr[k])
                    {
                        continue;
                    }
                    int full_det_label = hd_output[full_idx].result.stHdResult.pstTargetBoxes[k].enLabel;
                    if (tile_det_label == full_det_label)
                    {
                        SVP_RECT_S *a = &hd_output[i].result.stHdResult.pstTargetBoxes[j];
                        SVP_RECT_S *b = &hd_output[full_idx].result.stHdResult.pstTargetBoxes[k];
                        float iou = calculate_overlap(a, b);
#if 0//def DEBUG_HAL_AI_CAMERA_OVERLAP
                        log_info("box[%d][%d] with full[%d] iou: %.2f", i, j, k, iou);
#endif
                        if (iou > iou_threshold)
                        {
                            found = 1;
                            full_flag_arr[k] = 1;
                        }
                    }
                }
                if(!found)
                {
                    memcpy(&post_result.pstTargetBoxes[post_result.s32TotalNum ++], &hd_output[i].result.stHdResult.pstTargetBoxes[j], sizeof(SVP_RECT_S));
                }
            }
        }
    }

    SVP_HD_RESULT_S final_result;
    final_result.s32TotalNum = post_result.s32TotalNum;
    if (post_result.s32TotalNum != t_cnt)
    {
        if (post_result.s32TotalNum > 0)
        {
            final_result.pstTargetBoxes = (SVP_RECT_S *)AK_BASE_MemAlloc(MODULE_ID_APP, post_result.s32TotalNum * sizeof(SVP_RECT_S));
            memcpy(final_result.pstTargetBoxes, post_result.pstTargetBoxes, post_result.s32TotalNum * sizeof(SVP_RECT_S));
            free(post_result.pstTargetBoxes);
            post_result.pstTargetBoxes = NULL;
        }
    }
    else
    {
        final_result.pstTargetBoxes = post_result.pstTargetBoxes;
    }
    hd_output[full_idx].result.stHdResult.s32TotalNum = final_result.s32TotalNum;
    free(hd_output[full_idx].result.stHdResult.pstTargetBoxes);
    hd_output[full_idx].result.stHdResult.pstTargetBoxes = final_result.pstTargetBoxes;
#if 0//def DEBUG_HAL_AI_CAMERA_OVERLAP
    for (int i = 0; i < hd_output[full_idx].result.stHdResult.s32TotalNum; i ++)
    {
        log_info(" hd_output[%d] rect[(%d,%d), (%d,%d)], score: %d, type: %d", \
        i, hd_output[full_idx].result.stHdResult.pstTargetBoxes[i].u16Left, \
        hd_output[full_idx].result.stHdResult.pstTargetBoxes[i].u16Top, \
        hd_output[full_idx].result.stHdResult.pstTargetBoxes[i].u16Right, \
        hd_output[full_idx].result.stHdResult.pstTargetBoxes[i].u16Bottom, \
        hd_output[full_idx].result.stHdResult.pstTargetBoxes[i].u64Score, \
        hd_output[full_idx].result.stHdResult.pstTargetBoxes[i].enLabel);
    }
#endif
}

static int hal_camera_ai_compare(const void *a, const void *b)
{
    const SVP_RECT_S *rectA = (const SVP_RECT_S *)a;
    const SVP_RECT_S *rectB = (const SVP_RECT_S *)b;

    if (rectA->enLabel != rectB->enLabel)
    {
        return rectA->enLabel - rectB->enLabel;
    }
    else
    {
        return rectB->u64Score - rectA->u64Score;
    }
}

/* Sort hd_output by enLabel and u64Score */
static void hal_camera_ai_sort_final_output(SVP_OUTPUT_S *hd_output)
{
    qsort(hd_output->result.stHdResult.pstTargetBoxes, hd_output->result.stHdResult.s32TotalNum, sizeof(SVP_RECT_S), hal_camera_ai_compare);
}

/**
 * @brief Merge overlapping detection boxes
 * @param boxes Detection box array
 * @param num_boxes Number of detection boxes
 * @param overlap_threshold Overlap threshold
 * @return Number of boxes after merging
 */
static int merge_overlapping_boxes(SVP_RECT_S *boxes, int num_boxes, float overlap_threshold)
{
    if (num_boxes <= 1) {
        return num_boxes;
    }

    int count = num_boxes;
    for (int i = 0; i < count; i++) {
        // Expand box i by merging any overlapping boxes j>i into it
        for (int j = i + 1; j < count; ) {
            AK_U32 si = boxes[i].u64Score;
            AK_U32 sj = boxes[j].u64Score;
            AK_U32 sdiff = (si > sj) ? (si - sj) : (sj - si);
            int strict = (sdiff <= SCORE_DIFF_STRICT_THRESHOLD) ? 1 : 0;
            if (centers_close_size_similar(&boxes[i], &boxes[j], strict)) {
                // Merge j into i (grow bounds), set score to 0 to indicate merged
                boxes[i].u16Left   = GET_MIN(boxes[i].u16Left,   boxes[j].u16Left);
                boxes[i].u16Top    = GET_MIN(boxes[i].u16Top,    boxes[j].u16Top);
                boxes[i].u16Right  = GET_MAX(boxes[i].u16Right,  boxes[j].u16Right);
                boxes[i].u16Bottom = GET_MAX(boxes[i].u16Bottom, boxes[j].u16Bottom);
                boxes[i].u64Score = 0;
                // Remove box j by moving the last active box into position j
                boxes[j] = boxes[count - 1];
                count--;
                // Do not advance j; need to test the swapped-in element
                continue;
            }
            j++;
        }
    }

    return count;
}
/**
 * @brief Convert detection box coordinates from tile coordinate system to input coordinate system
 * @param tile_box Detection box in tile coordinate system
 * @param tile_start_x Starting X coordinate of tile in input image
 * @param tile_start_y Starting Y coordinate of tile in input image
 * @param input_box Converted detection box in input coordinate system
 */
static void convert_tile_to_input_coords(SVP_RECT_S *box_inout, 
                                       int tile_start_x, int tile_start_y)
{
    box_inout->u16Left   = (AK_U16)(box_inout->u16Left   + tile_start_x);
    box_inout->u16Top    = (AK_U16)(box_inout->u16Top    + tile_start_y);
    box_inout->u16Right  = (AK_U16)(box_inout->u16Right  + tile_start_x);
    box_inout->u16Bottom = (AK_U16)(box_inout->u16Bottom + tile_start_y);
}

/**
 * @brief Convert detection box coordinates from input coordinate system to tile coordinate system
 * @param input_box Detection box in input coordinate system
 * @param tile_start_x Starting X coordinate of tile in input image
 * @param tile_start_y Starting Y coordinate of tile in input image
 * @param tile_box Converted detection box in tile coordinate system
 */
static void convert_input_to_tile_coords(SVP_RECT_S *box_inout,
                                       int tile_start_x, int tile_start_y)
{
    box_inout->u16Left   = (AK_U16)(box_inout->u16Left   - tile_start_x);
    box_inout->u16Top    = (AK_U16)(box_inout->u16Top    - tile_start_y);
    box_inout->u16Right  = (AK_U16)(box_inout->u16Right  - tile_start_x);
    box_inout->u16Bottom = (AK_U16)(box_inout->u16Bottom - tile_start_y);
}

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
                                             int *start_x, int *start_y)
{
    if (!start_x || !start_y || 
        row < 0 || col < 0 ||
        tile_width <= 0 || tile_height <= 0 ||
        overlap_ratio < 0.0f || overlap_ratio >= 1.0f) {
        log_info("Invalid parameters for tile start coordinates calculation\n");
        return -1;
    }
    
    // Calculate overlap pixels
    int overlap_width = (int)(tile_width * overlap_ratio);
    int overlap_height = (int)(tile_height * overlap_ratio);
    
    // Calculate step size (distance between adjacent tiles)
    int step_width = tile_width - overlap_width;
    int step_height = tile_height - overlap_height;
    
    // Calculate tile start coordinates
    *start_x = col * step_width;
    *start_y = row * step_height;
    
    
    return 0;
}

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
                                      int *input_width, int *input_height)
{
    if (!input_width || !input_height || 
        tile_width <= 0 || tile_height <= 0 ||
        num_rows <= 0 || num_cols <= 0 || 
        overlap_ratio < 0.0f || overlap_ratio >= 1.0f) {
        log_info("Invalid parameters for input size calculation\n");
        return -1;
    }
    
    // Calculate overlap pixels
    int overlap_width = (int)(tile_width * overlap_ratio);
    int overlap_height = (int)(tile_height * overlap_ratio);
    
    // Calculate step size (distance between adjacent tiles)
    int step_width = tile_width - overlap_width;
    int step_height = tile_height - overlap_height;
    
    // Calculate input image size
    // Width = first tile start position(0) + last tile end position
    // Last tile start position = (num_cols - 1) * step_width
    // Last tile end position = start position + tile_width
    *input_width = (num_cols - 1) * step_width + tile_width;
    
    // Height calculation is similar
    *input_height = (num_rows - 1) * step_height + tile_height;
    
    log_info("Calculated input size: %dx%d from tiles: %dx%d, overlap: %.2f, step: %dx%d\n",
             *input_width, *input_height, tile_width, tile_height, 
             overlap_ratio, step_width, step_height);
    
    return 0;
}

/**
 * @brief Main function for tile-based detection algorithm
 * @param input_width Input image width (before splitting)
 * @param input_height Input image height (before splitting)
 * @param tile_width Width of each tile
 * @param tile_height Height of each tile
 * @param num_rows Number of tile rows
 * @param num_cols Number of tile columns
 * @param overlap_ratio Tile overlap ratio (0.0-1.0)
 * @param hd_results Detection result array, containing num_rows*num_cols tile results and 1 full image result
 * @return 0 success, -1 failure
 */
int hal_camera_ai_tile_overlap_detection(int input_width, int input_height,
                                        int tile_width, int tile_height,
                                        int num_rows, int num_cols, float overlap_ratio,
                                        SVP_OUTPUT_S *hd_output)
{
    if (!hd_output || 
        num_rows <= 0 || num_cols <= 0 || overlap_ratio < 0.0f || overlap_ratio >= 1.0f ||
        tile_width <= 0 || tile_height <= 0) {
        log_info("Invalid parameters for tile detection\n");
        return -1;
    }
    
    // Calculate overlap and step for tiles
    int overlap_width = (int)(tile_width * overlap_ratio);
    int overlap_height = (int)(tile_height * overlap_ratio);
    int step_width = tile_width - overlap_width;
    int step_height = tile_height - overlap_height;

    
    /* Step 1: Convert all tile results to input coordinates, then scale to tile size */
    float sx = (float)tile_width  / (float)input_width;
    float sy = (float)tile_height / (float)input_height;
    int t_cnt = 0;
    for (int row = 0; row < num_rows; row++) {
        for (int col = 0; col < num_cols; col++) {
            int tile_id = row * num_cols + col;
            SVP_HD_RESULT_S *tile_result = &hd_output[tile_id].result.stHdResult;
            
            // Calculate tile start coordinates in input image
            int tile_start_x = col * step_width;
            int tile_start_y = row * step_height;
            
            
            if (tile_result->s32TotalNum > 0 && tile_result->pstTargetBoxes) {
                // Convert to input coords then scale to tile size (in place)
                for (int i = 0; i < tile_result->s32TotalNum; i++) {
                    convert_tile_to_input_coords(&tile_result->pstTargetBoxes[i], tile_start_x, tile_start_y);
                    // Clamp to input boundaries then scale to tile size and clamp to tile boundaries
                    SVP_RECT_S *b = &tile_result->pstTargetBoxes[i];
                    if (b->u16Left > (AK_U16)input_width)  b->u16Left = (AK_U16)input_width;
                    if (b->u16Top > (AK_U16)input_height)  b->u16Top = (AK_U16)input_height;
                    if (b->u16Right > (AK_U16)input_width) b->u16Right = (AK_U16)input_width;
                    if (b->u16Bottom > (AK_U16)input_height) b->u16Bottom = (AK_U16)input_height;
                    int left   = (int)(b->u16Left   * sx);
                    int right  = (int)(b->u16Right  * sx);
                    int top    = (int)(b->u16Top    * sy);
                    int bottom = (int)(b->u16Bottom * sy);
                    if (left < 0) left = 0; if (top < 0) top = 0;
                    if (right > tile_width) right = tile_width;
                    if (bottom > tile_height) bottom = tile_height;
                    b->u16Left = (AK_U16)left; b->u16Right = (AK_U16)right;
                    b->u16Top = (AK_U16)top;   b->u16Bottom = (AK_U16)bottom;
                    t_cnt ++;
                    //log_info("after converting[%d][%d]'s[%d]: [(%d,%d),(%d,%d)], score: %d, type: %d, t_cnt: %d\n",
                    //       row, col, i, b->u16Left, b->u16Top, b->u16Right, b->u16Bottom,
                    //       b->u64Score, b->enLabel, t_cnt);
                }
            }
        }
    }
    
    /* Step 2: Collect all detection boxes (already in tile-size coords) */
#if 0
    int full_image_index = num_rows * num_cols;
    SVP_HD_RESULT_S *full_image_result = &hd_output[full_image_index].result.stHdResult;
    int capacity_full = full_image_result->s32TotalNum; // use initial value as capacity
    int total_boxes = 0;

    // Append all tiles' boxes (now in input coordinate system)
    for (int i = 0; i < n_tiles && total_boxes < capacity_full; i++) {
        SVP_HD_RESULT_S *tile_result = &hd_output[i].result.stHdResult;
        if (tile_result->s32TotalNum > 0 && tile_result->pstTargetBoxes) {
            int copy_num = tile_result->s32TotalNum;
            if (total_boxes + copy_num > capacity_full) {
                copy_num = capacity_full - total_boxes;
            }
            for (int j = 0; j < copy_num; j++) {
                full_image_result->pstTargetBoxes[total_boxes++] = tile_result->pstTargetBoxes[j];
            }
        }
    }
#endif
    t_cnt += hd_output[num_rows * num_cols].result.stHdResult.s32TotalNum;
    /* Step 3: Deduplicate only between full image and tiles */
    if (t_cnt > 0)
    {
        process_full_and_tiles(hd_output, num_rows, num_cols, t_cnt, IOU_MAX_THRESHOLD);
    }
    hal_camera_ai_sort_final_output(&hd_output[num_rows * num_cols]);
    /* Results are in tile-size coordinate system */
    
    return 0;
}
