/*
 * hal_camera_test_config.h — grouped test profile (one struct per SoC in profile.c).
 *
 * Groups: stream (分辨率/crop/VI depth·slice/AI格式), codec, ai, run.
 */

#ifndef HARDWARE_TESTS_HAL_CAMERA_TEST_CONFIG_H
#define HARDWARE_TESTS_HAL_CAMERA_TEST_CONFIG_H

#include <stdint.h>
#include "camera.h"

#if defined(CAMERA_TEST_SOC_AV100_V2) && CAMERA_TEST_SOC_AV100_V2
#elif defined(CAMERA_TEST_SOC_AV200) && CAMERA_TEST_SOC_AV200
#elif defined(AnyCloud39AV100_V2)
#define CAMERA_TEST_SOC_AV100_V2  1
#elif defined(AnyCloud39AV200)
#define CAMERA_TEST_SOC_AV200     1
#else
#define CAMERA_TEST_SOC_AV200     1
#endif

#if defined(CAMERA_TEST_SOC_AV100_V2) && CAMERA_TEST_SOC_AV100_V2
#define CAMERA_TEST_SOC_NAME  "AnyCloud39AV100_V2"
#elif defined(CAMERA_TEST_SOC_AV200) && CAMERA_TEST_SOC_AV200
#define CAMERA_TEST_SOC_NAME  "AnyCloud39AV200"
#else
#define CAMERA_TEST_SOC_NAME  "unknown"
#endif

/* Stream index bits for run.combo_stream_mask */
#define CAMERA_TEST_STREAM_VIDEO    (1u << 0)
#define CAMERA_TEST_STREAM_PICTURE  (1u << 1)
#define CAMERA_TEST_STREAM_SUB      (1u << 2)
#define CAMERA_TEST_STREAM_AI       (1u << 3)
#define CAMERA_TEST_STREAM_ALL      (CAMERA_TEST_STREAM_VIDEO | CAMERA_TEST_STREAM_PICTURE | \
                                     CAMERA_TEST_STREAM_SUB | CAMERA_TEST_STREAM_AI)

/** 分辨率、VI crop/缓冲、slice、AI 像素格式 */
typedef struct {
    uint32_t main_w;
    uint32_t main_h;
    uint32_t sub_w;
    uint32_t sub_h;
    uint32_t ai_w;
    uint32_t ai_h;
    uint32_t crop_w;
    uint32_t crop_h;
    uint32_t vi_frame_depth;   /* channel max_buffers */
    uint32_t vi_num_slices;    /* stream num_slices */
    uint32_t vi_fps;           /* 各路 stream fps */
    uint32_t ai_pixel_format;  /* SYSTEM_PIXEL_FORMAT_* */
} camera_test_stream_group_t;

/** 编码与码率 */
typedef struct {
    uint32_t video_max_bitrate_kbps;
    const char *video_codec; /* h264 | h265 | mjpeg */
    const char *sub_codec;
} camera_test_codec_group_t;

/** AI 检测参数 */
typedef struct {
    int classify_threshold;
    int iou_threshold;
    int legacy_mode;
    int tile_overlap;
    int tile_cols;
    int tile_rows;
    float tile_overlap_ratio;
} camera_test_ai_group_t;

/** 运行时长与组合测试项（命令行一般只改 --duration / --quick） */
typedef struct {
    int phase_sec;           /* app_flow 每阶段秒数 */
    int combo_sec;           /* 多路 combo 秒数 */
    int quick_phase_sec;
    int quick_combo_sec;
    unsigned combo_stream_mask; /* CAMERA_TEST_STREAM_* */
    int enable_picture;
    int picture_at_sec;
    int enable_osd;
    const char *osd_font;
} camera_test_run_group_t;

typedef struct {
    const char *name;
    camera_test_stream_group_t stream;
    camera_test_codec_group_t codec;
    camera_test_ai_group_t ai;
    camera_test_run_group_t run;
} camera_test_profile_t;

const camera_test_profile_t *camera_test_get_profile(void);

#if defined(CAMERA_TEST_SOC_AV100_V2) && CAMERA_TEST_SOC_AV100_V2
#define CAMERA_TEST_MAIN_USE_AUTO_PICK  0
#define CAMERA_TEST_SUB_USE_AUTO_SCALE  0
#define CAMERA_TEST_VI_NV12_BUDGET_BYTES (20u * 1024u * 1024u)
#else
#define CAMERA_TEST_MAIN_USE_AUTO_PICK  0
#define CAMERA_TEST_SUB_USE_AUTO_SCALE  0
#define CAMERA_TEST_VI_NV12_BUDGET_BYTES (14u * 1024u * 1024u)
#endif

#endif /* HARDWARE_TESTS_HAL_CAMERA_TEST_CONFIG_H */
