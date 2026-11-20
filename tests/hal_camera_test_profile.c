/*
 * Per-SoC camera test profile (single source of defaults).
 */

#include "hal_camera_test_config.h"

#if defined(CAMERA_TEST_SOC_AV100_V2) && CAMERA_TEST_SOC_AV100_V2

static const camera_test_profile_t s_profile = {
    .name = "av100_v2",
    .stream = {
        .main_w = 2560u,
        .main_h = 1920u,
        .sub_w = 1280u,
        .sub_h = 960u,   /* keep 16:9 vs main */
        .ai_w = 480u,
        .ai_h = 288u,
        .crop_w = 2560u,
        .crop_h = 1920u,
        .vi_frame_depth = 2u,
        .vi_num_slices = 1u,
        .vi_fps = 15u,
        .ai_pixel_format = SYSTEM_PIXEL_FORMAT_RGB_LINEINTL,
    },
    .codec = {
        .video_max_bitrate_kbps = 1024u,
        .video_codec = "h264",
        .sub_codec = "mjpeg",
    },
    .ai = {
        .classify_threshold = 600,
        .iou_threshold = 3,
        .legacy_mode = 0,
        .tile_overlap = 0,
        .tile_cols = 2,
        .tile_rows = 2,
        .tile_overlap_ratio = 0.25f,
    },
    .run = {
        .phase_sec = 4,
        .combo_sec = 10,
        .quick_phase_sec = 2,
        .quick_combo_sec = 5,
        .combo_stream_mask = CAMERA_TEST_STREAM_VIDEO | CAMERA_TEST_STREAM_SUB |
                             CAMERA_TEST_STREAM_AI | CAMERA_TEST_STREAM_PICTURE,
        .enable_picture = 1,
        .picture_at_sec = 3,
        .enable_osd = 0,
        .osd_font = NULL,
    },
};

#else /* AnyCloud39AV200 */

static const camera_test_profile_t s_profile = {
    .name = "av200",
    .stream = {
        .main_w = 2880u,
        .main_h = 1620u,
        .sub_w = 1024u,
        .sub_h = 600u,
        .ai_w = 640u,
        .ai_h = 384u,
        .crop_w = 2880u,
        .crop_h = 1620u,
        .vi_frame_depth = 2u,
        .vi_num_slices = 1u,
        .vi_fps = 15u,
        .ai_pixel_format = SYSTEM_PIXEL_FORMAT_RGB_HWC,
    },
    .codec = {
        .video_max_bitrate_kbps = 2048u,
        .video_codec = "h264",
        .sub_codec = "mjpeg",
    },
    .ai = {
        .classify_threshold = 700,
        .iou_threshold = 3,
        .legacy_mode = 1,
        .tile_overlap = 1,
        .tile_cols = 2,
        .tile_rows = 2,
        .tile_overlap_ratio = 0.25f,
    },
    .run = {
        .phase_sec = 4,
        .combo_sec = 10,
        .quick_phase_sec = 2,
        .quick_combo_sec = 5,
        .combo_stream_mask = CAMERA_TEST_STREAM_VIDEO | CAMERA_TEST_STREAM_SUB |
                             CAMERA_TEST_STREAM_AI | CAMERA_TEST_STREAM_PICTURE,
        .enable_picture = 1,
        .picture_at_sec = 3,
        .enable_osd = 1,
        .osd_font = NULL,
    },
};

#endif

const camera_test_profile_t *camera_test_get_profile(void)
{
    return &s_profile;
}
