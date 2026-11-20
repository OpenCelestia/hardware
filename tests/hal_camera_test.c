
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "log.h"
#define LOG_TAG "HAL CAMERA TEST"

#include "hardware.h"
#include "camera.h"
#include "hal_camera_test_config.h"
#include "osd.h"
#include <unistd.h>
#include "soc_platform.h"
#include "system_thread.h"

/* OSD device for camera overlay */
static osd_hw_device_t *osd_dev = NULL;

/* When set in video test, save main stream to a single file under the given dir by codec:
 * H.264 -> video.h264 (raw append); H.265 -> video.h265 (raw append); MJPEG -> video.mjpeg (4-byte length + data per frame) */
#define SAVE_DIR_PATH_MAX  256
static char s_video_save_dir[SAVE_DIR_PATH_MAX];
static FILE *s_video_save_fp;

/* Sizes filled from hal_camera_test_profile.c via camera_test_apply_profile_to_static(). */
camera_channel_config_t splice_channel_config[2];
camera_channel_config_t single_main_channel_config[1];
camera_channel_config_t single_sub_channel_config[1];
camera_channel_config_t single_ai_channel_config[1];

/* In framework the array can be defined as NV config file */
camera_stream_t video_stream = {
    .stream_format = CAMERA_STREAM_FORMAT_SINGLE,
    .num_slices = 1,
    .fps = 15,
    .channel_config = single_main_channel_config,
    .num_channel = 1,

    .format = SYSTEM_PIXEL_FORMAT_NV12,
    .codec_config = {
        .codec_mode = CAMERA_CODEC_MODE_AVC,
        .profile = AVC_PROFILE_MAIN,
        .level = AVC_LEVEL_3
    },
    .rc_config = {
        .avc = {
            .rc_mode = CAMERA_CODEC_RC_MODE_VBR,
            .vbr = {
                .max_bitrate = 1024,
                .max_qp = 48,
                .min_qp = 20,
                .change_pos = 50
            }
        }
    },
    .gop_config = {
        /* ak_venc_sample: ve_param.goplen = 2*vi_fps; HAL: gop_len==0 -> 2*fps */
        .gop_len = 60
    }
};


camera_stream_t sub_stream = {
    .stream_format = CAMERA_STREAM_FORMAT_SINGLE,
    .num_slices = 1,
    .channel_config = single_sub_channel_config,

    .format = SYSTEM_PIXEL_FORMAT_NV12,
    .fps = 15,

    .codec_config = {
        .codec_mode = CAMERA_CODEC_MODE_YUV,
    },
    .gop_config = {
        .gop_len = 60
    }
};

camera_stream_t ai_stream = {
    .stream_format = CAMERA_STREAM_FORMAT_SINGLE,
    .num_slices = 1,
    .channel_config = single_ai_channel_config,

    .format = SYSTEM_PIXEL_FORMAT_RGB_HWC,
    .fps = 15,

    .codec_config = {
        .codec_mode = CAMERA_CODEC_MODE_YUV,
    },
    .gop_config = {
        .gop_len = 60
    }
};

camera_stream_t picture_stream = {
    .stream_format = CAMERA_STREAM_FORMAT_SINGLE,
    .num_slices = 1,
    .format = SYSTEM_PIXEL_FORMAT_NV12,
    .channel_config = single_main_channel_config,
    .fps = 15,

    .codec_config = {
        .codec_mode = CAMERA_CODEC_MODE_MJPEG,
        .profile = JPEG_PROFILE
    },
    .rc_config = {
        .mjpeg = {
            .quality = 0.8
        }
    },
    .gop_config = {
        .gop_len = 60
    }
};



#define VIDEO_STREAM 0
#define SUB_STREAM 2
#define AI_STREAM 3
#define PICTURE_STREAM 1

#define STREAM_BIT_VIDEO    (1u << VIDEO_STREAM)
#define STREAM_BIT_PICTURE  (1u << PICTURE_STREAM)
#define STREAM_BIT_SUB      (1u << SUB_STREAM)
#define STREAM_BIT_AI       (1u << AI_STREAM)
#define STREAM_BIT_ALL      (STREAM_BIT_VIDEO | STREAM_BIT_PICTURE | STREAM_BIT_SUB | STREAM_BIT_AI)

struct camera_test_data {
    char *file;
    camera_stream_t stream;
    camera_stream_configuration_t stream_config;

    camera_capture_request_t capture_request;
    camera_stream_buffer_t output_buffer;

    int numbmer_cameras;

    void *stream_buffer[4];
    int buffer_size;
    int num_stream_buffer;


    struct camera_hw_device *dev;
    hw_module_t *module;
};

typedef struct camera_test_data camera_data_t;

camera_data_t camera_data;

static int g_test_argc;
static char **g_test_argv;
static int g_test_quick;

static const char *get_opt_value(int argc, char **argv, const char *opt);
static int apply_test_channel_resolutions(int argc, char **argv, int camera_id);
static void camera_test_bind_argv(int argc, char **argv);
static void apply_video_codec(const char *codec_str);
static void apply_sub_codec(const char *codec_str);
static void camera_test_apply_profile_to_static(void);
static void camera_test_print_profile(void);
static int camera_test_duration_sec(int argc, char **argv, int profile_sec);
static unsigned camera_test_profile_stream_mask(void);
static int camera_test_profile_wants_ai(void);
static int run_all_tests(int argc, char **argv);

/** Camera module API (hardware.h); set by open_camera_module(). */
static inline camera_module_t *test_camera_module(void)
{
    return (camera_module_t *)camera_data.module;
}

void process_capture_result(const camera_callback_ops_t *ops, const camera_capture_result_t *result) ;
void notify(const camera_callback_ops_t *ops, const camera_notify_msg_t *msg) ;

/* Parameterized test case: name, description, run function */
typedef int (*camera_test_fn)(int argc, char **argv);

typedef struct camera_test_case {
    const char *name;
    const char *desc;
    camera_test_fn run;
} camera_test_case_t;

static int run_discover(int argc, char **argv);
static int run_app_flow(int argc, char **argv);
static int run_reconfigure(int argc, char **argv);
static int run_combo(int argc, char **argv);
static int run_quick(int argc, char **argv);
static int run_stream(int argc, char **argv);

static const camera_test_case_t camera_tests[] = {
    { "discover",  "枚举相机", run_discover },
    { "stream",    "按路出流：main|sub|ai|picture 可组合", run_stream },
    { "quick",     "快速冒烟（profile 短时长）", run_quick },
    { "all",       "全量：分路出流 + 重配置 + 多路 combo（参数见 profile）", run_all_tests },
};
static const int camera_test_count = sizeof(camera_tests) / sizeof(camera_tests[0]);

/** Parse --duration N from argv; default to default_sec */
static int get_duration(int argc, char **argv, int default_sec)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
            return atoi(argv[i + 1]);
    }
    return default_sec;
}

static void print_help(const char *prog)
{
    const camera_test_profile_t *p = camera_test_get_profile();

    printf("Usage: %s <--discover|--stream|--quick|--all> [options]\n\n", prog);
    for (int i = 0; i < camera_test_count; i++)
        printf("  --%-8s  %s\n", camera_tests[i].name, camera_tests[i].desc);
    printf("  --stream <main|sub|ai|picture>... [--duration N]  (AI 路自动用 CAMERA_TEMPLATE_AI)\n");
    printf("  --discover --probe-sensor: optional VI probe (all/quick must not use it)\n");
    printf("\nProfile \"%s\" (%s):\n", p->name, CAMERA_TEST_SOC_NAME);
    printf("  stream  main %ux%u  sub %ux%u  ai %ux%u  crop %ux%u  vi depth=%u slices=%u\n",
           (unsigned)p->stream.main_w, (unsigned)p->stream.main_h,
           (unsigned)p->stream.sub_w, (unsigned)p->stream.sub_h,
           (unsigned)p->stream.ai_w, (unsigned)p->stream.ai_h,
           (unsigned)p->stream.crop_w, (unsigned)p->stream.crop_h,
           (unsigned)p->stream.vi_frame_depth, (unsigned)p->stream.vi_num_slices);
    printf("  codec   video %s %u kbps  sub %s\n",
           p->codec.video_codec, (unsigned)p->codec.video_max_bitrate_kbps, p->codec.sub_codec);
    printf("  ai      classify %d  legacy %d  tile_overlap %d\n",
           p->ai.classify_threshold, p->ai.legacy_mode, p->ai.tile_overlap);
    printf("  run     phase %ds  combo %ds  (quick: %d/%d)\n",
           p->run.phase_sec, p->run.combo_sec,
           p->run.quick_phase_sec, p->run.quick_combo_sec);
    printf("\nExamples:\n  %s --stream main\n  %s --stream main ai --duration 15\n"
           "  %s --stream sub\n  %s --quick\n  %s --all\n",
           prog, prog, prog, prog, prog);
}

/* Callback ops struct */
camera_callback_ops_t callback_ops = {
    .process_capture_result = process_capture_result,
    .notify = notify,
};

void m_sleep(const int ms)
{
    usleep(ms * 1000);
}

/* HAL callback: per-stream output frame counts [main, picture, sub, ai] */
static volatile uint32_t s_stream_out_frames[4];
static volatile uint32_t s_ai_notify_calls;

/* Static request buffers — HAL/capture thread keeps pointers after process_capture_request returns. */
static camera_stream_buffer_t s_static_capture_bufs[4];

#define CAMERA_TEST_TEARDOWN_MS      1500
#define CAMERA_TEST_POST_CLOSE_MS    1200

static void stream_count_out_frames(const camera_capture_result_t *result)
{
    if (result->output_buffers == NULL)
        return;
    for (int i = 0; i < (int)result->num_output_buffers; i++) {
        camera_stream_t *s = result->output_buffers[i].stream;
        if (s == NULL)
            continue;
        if (result->output_buffers[i].data == NULL || result->output_buffers[i].data_size == 0)
            continue;
        if (s == &video_stream)
            s_stream_out_frames[0]++;
        else if (s == &picture_stream)
            s_stream_out_frames[1]++;
        else if (s == &sub_stream)
            s_stream_out_frames[2]++;
        else if (s == &ai_stream)
            s_stream_out_frames[3]++;
    }
}

static void stream_frames_reset(void)
{
    for (int i = 0; i < 4; i++)
        s_stream_out_frames[i] = 0;
    s_ai_notify_calls = 0;
}

static unsigned camera_test_stream_name_to_bit(const char *name)
{
    if (name == NULL || name[0] == '\0')
        return 0;
    if (strcasecmp(name, "main") == 0 || strcasecmp(name, "video") == 0)
        return STREAM_BIT_VIDEO;
    if (strcasecmp(name, "sub") == 0)
        return STREAM_BIT_SUB;
    if (strcasecmp(name, "ai") == 0)
        return STREAM_BIT_AI;
    if (strcasecmp(name, "picture") == 0 || strcasecmp(name, "pic") == 0)
        return STREAM_BIT_PICTURE;
    return 0;
}

/** argv[2..] stream names until "--*"; at least one bit required. */
static int camera_test_parse_stream_argv(int argc, char **argv, unsigned *mask_out)
{
    unsigned mask = 0;

    if (mask_out == NULL)
        return -1;
    for (int i = 2; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == '-')
            break;
        unsigned bit = camera_test_stream_name_to_bit(argv[i]);
        if (bit == 0) {
            log_error("unknown stream \"%s\" (use main|sub|ai|picture)", argv[i]);
            return -1;
        }
        mask |= bit;
    }
    if (mask == 0) {
        log_error("stream: specify at least one of main sub ai picture");
        return -1;
    }
    *mask_out = mask;
    return 0;
}

static int camera_test_mask_to_streams(unsigned mask, camera_stream_t **list, int max_n)
{
    int n = 0;

    if (list == NULL || max_n <= 0)
        return 0;
    if ((mask & STREAM_BIT_VIDEO) && n < max_n)
        list[n++] = &video_stream;
    if ((mask & STREAM_BIT_PICTURE) && n < max_n)
        list[n++] = &picture_stream;
    if ((mask & STREAM_BIT_SUB) && n < max_n)
        list[n++] = &sub_stream;
    if ((mask & STREAM_BIT_AI) && n < max_n)
        list[n++] = &ai_stream;
    return n;
}

static void stream_log_out_frames(unsigned mask, int tick_sec)
{
    log_info("stream t=%ds mask=0x%x out_frames: main=%u pic=%u sub=%u ai=%u",
             tick_sec, mask,
             (unsigned)s_stream_out_frames[0],
             (unsigned)s_stream_out_frames[1],
             (unsigned)s_stream_out_frames[2],
             (unsigned)s_stream_out_frames[3]);
}

static camera_request_template_t camera_test_pick_request_template(unsigned mask)
{
    unsigned non_pic = mask & ~(STREAM_BIT_PICTURE);

    if (non_pic == STREAM_BIT_AI)
        return CAMERA_TEMPLATE_AI;
    if (mask == STREAM_BIT_PICTURE)
        return CAMERA_TEMPLATE_PICTURE;
    return CAMERA_TEMPLATE_VIDEO_RECORD;
}

static void app_flow_log_line(const char *phase, int tick_sec)
{
    log_info("[app_flow] %s t=%ds out_frames: main=%u pic=%u sub=%u ai=%u",
             phase, tick_sec,
             (unsigned)s_stream_out_frames[0],
             (unsigned)s_stream_out_frames[1],
             (unsigned)s_stream_out_frames[2],
             (unsigned)s_stream_out_frames[3]);
}

/* Enumerate cameras only (no ak_vi_open; safe before configure_streams). */
static int discover_devices_light(void)
{
    camera_module_t *cam = test_camera_module();
    const camera_test_profile_t *p = camera_test_get_profile();

    camera_data.numbmer_cameras = cam->get_number_of_cameras();
    log_info("Number of cameras: %d", camera_data.numbmer_cameras);
    log_info("Camera 0 profile: main %ux%u sub %ux%u ai %ux%u",
             (unsigned)p->stream.main_w, (unsigned)p->stream.main_h,
             (unsigned)p->stream.sub_w, (unsigned)p->stream.sub_h,
             (unsigned)p->stream.ai_w, (unsigned)p->stream.ai_h);
    return 0;
}

/* Optional: ak_vi_open + get_sensor_resolution (closes VI; do not call before configure). */
int discover_devices(void)
{
    camera_module_t *cam = test_camera_module();

    camera_data.numbmer_cameras = cam->get_number_of_cameras();
    log_info("Number of cameras: %d", camera_data.numbmer_cameras);

    for (int i = 0; i < camera_data.numbmer_cameras; i++) {
        struct camera_info info;
        if (cam->get_camera_info(i, &info) == 0) {
            log_info("Camera ID: %d, max_width:%d, max_height:%d",
                     i, info.max_width, info.max_height);
        } else {
            log_error("Failed to get info for camera ID: %d", i);
        }
    }
    return 0;
}



/* Capture result callback (HAL calls asynchronously; check for null and bounds) */
void process_capture_result(const camera_callback_ops_t *ops, const camera_capture_result_t *result)
{
    (void)ops;
    if (result == NULL)
        return;
    if (result->output_buffers == NULL || result->num_output_buffers == 0)
        return;

    stream_count_out_frames(result);

    /* In video test with save file open, append to single file by codec */
    if (s_video_save_fp != NULL && result->output_buffers[0].stream == &video_stream &&
        result->output_buffers[0].data != NULL && result->output_buffers[0].data_size > 0) {
        uint32_t size = result->output_buffers[0].data_size;
        if (video_stream.codec_config.codec_mode == CAMERA_CODEC_MODE_MJPEG) {
            /* MJPEG: write 4-byte length (LE) + one JPEG frame for later frame parsing */
            uint8_t len_buf[4];
            len_buf[0] = (uint8_t)(size);
            len_buf[1] = (uint8_t)(size >> 8);
            len_buf[2] = (uint8_t)(size >> 16);
            len_buf[3] = (uint8_t)(size >> 24);
            fwrite(len_buf, 1, 4, s_video_save_fp);
        }
        /* H.264/H.265 append raw NAL stream; MJPEG length already written, then data */
        if (fwrite(result->output_buffers[0].data, 1, size, s_video_save_fp) == (size_t)size &&
            (result->frame_number % 30) == 0)
            log_info("video save: frame %u, %u bytes", (unsigned)result->frame_number, (unsigned)size);
    }
}


/* Notify callback (HAL calls asynchronously; check for null and bounds) */
#define NOTIFY_AI_OBJECT_MAX  64

void notify(const camera_callback_ops_t *ops, const camera_notify_msg_t *msg)
{
    uint32_t call_idx;

    (void)ops;
    if (msg == NULL)
        return;
    call_idx = ++s_ai_notify_calls;
    if (msg->ai_object == NULL) {
        log_info("notify #%u: num_total=%d but ai_object is null", (unsigned)call_idx, msg->num_total);
        return;
    }
    int n = msg->num_total;
    if (n <= 0) {
        /* HAL calls notify every SVP frame even with 0 boxes; log periodically to prove callback path. */
        if ((call_idx % 25u) == 1u)
            log_info("notify #%u: num_total=0 (callback ok, no detection this frame)",
                     (unsigned)call_idx);
        return;
    }
    if (n > NOTIFY_AI_OBJECT_MAX)
        n = NOTIFY_AI_OBJECT_MAX;
    log_info("notify!!,msg->num_total:%d ", msg->num_total);
    for (int i = 0; i < n; i++)
    {
        if (msg->ai_object[i].ai_type & CAMERA_AI_DETECT_MASK)
        {
            log_info("detect target[%d]: position[%d,%d,%d,%d], \
                ai_type:%x, score:%d", i, \
            msg->ai_object[i].rect.x, msg->ai_object[i].rect.y, \
            msg->ai_object[i].rect.width, msg->ai_object[i].rect.height, \
            msg->ai_object[i].ai_type, msg->ai_object[i].detect.score);
        }
        else if(msg->ai_object[i].ai_type & CAMERA_AI_RECOGNIZE_MASK)
        {
            log_info("recognize target[%d]: position[%d,%d,%d,%d], \
                ai_type:%x, data:%x, data_len:%d", i, \
            msg->ai_object[i].rect.x, msg->ai_object[i].rect.y, \
            msg->ai_object[i].rect.width, msg->ai_object[i].rect.height, \
            msg->ai_object[i].ai_type,  \
            msg->ai_object[i].recognize.data,msg->ai_object[i].recognize.data_len);
        }
        else
        {
            log_info("detect target[%d]: position[%d,%d,%d,%d], \
                ai_type:%x", i, \
            msg->ai_object[i].rect.x, msg->ai_object[i].rect.y, \
            msg->ai_object[i].rect.width, msg->ai_object[i].rect.height, \
            msg->ai_object[i].ai_type);
        }
    }

}



int open_camera_module()
{
    int ret = hw_get_module(CAMERA_HARDWARE_MODULE_ID,
                            (const hw_module_t **)&camera_data.module);
    if (ret != 0) {
        log_error("Failed to get camera module: %d", ret);
        return ret;
    }
    log_info("Camera module opened successfully.");
    return 0;
}



int open_camera_device(int camera_id) {
    char camera_id_str[3];
    int ret;
    snprintf(camera_id_str, sizeof(camera_id_str), "%d", camera_id);

    ret = camera_hw_device_open(camera_data.module, &camera_data.dev);
    if (ret) {
        log_error("Failed to open camera hardware device: %d", ret);
        return -1;
    }

    ret = camera_data.dev->set_callback_ops(camera_data.dev, &callback_ops);

    log_info("Camera device %d opened successfully.", camera_id);
    return ret;
}


static int configure_camera_streams_with_mask(unsigned stream_mask)
{
    camera_stream_t *list[4];
    int n = 0;
    int ret;

    log_verbose("configure_camera_streams mask=0x%x", stream_mask);

    (void)apply_test_channel_resolutions(g_test_argc, g_test_argv, 0);

    if (stream_mask & STREAM_BIT_VIDEO)
        list[n++] = &video_stream;
    if (stream_mask & STREAM_BIT_PICTURE)
        list[n++] = &picture_stream;
    if (stream_mask & STREAM_BIT_SUB)
        list[n++] = &sub_stream;
    if (stream_mask & STREAM_BIT_AI)
        list[n++] = &ai_stream;

    if (n <= 0) {
        log_error("configure_camera_streams: empty stream mask");
        return -1;
    }

    camera_data.stream_config.num_streams = n;
    camera_data.stream_config.streams = (camera_stream_t **)malloc((size_t)n * sizeof(camera_stream_t *));
    if (camera_data.stream_config.streams == NULL) {
        log_error("configure_camera_streams: malloc failed");
        return -1;
    }
    for (int i = 0; i < n; i++)
        camera_data.stream_config.streams[i] = list[i];

    ret = camera_data.dev->configure_streams(camera_data.dev, &camera_data.stream_config);
    free(camera_data.stream_config.streams);
    camera_data.stream_config.streams = NULL;

    if (ret != 0) {
        log_error("Failed to configure camera streams: %d", ret);
        return ret;
    }

    log_info("Camera streams configured successfully (%d stream(s)).", n);
    return 0;
}

int configure_camera_streams(void)
{
    return configure_camera_streams_with_mask(STREAM_BIT_ALL);
}



int reconfigure_camera_streams( void )
{
    int ret;
    log_verbose( "reconfigure_camera_streams");

    (void)apply_test_channel_resolutions(g_test_argc, g_test_argv, 0);

    camera_data.stream_config.num_streams = 1;
    camera_data.stream_config.streams = (camera_stream_t **)malloc(camera_data.stream_config.num_streams * sizeof(camera_stream_t));

    sub_stream.codec_config.codec_mode = CAMERA_CODEC_MODE_MJPEG;
    sub_stream.codec_config.profile = JPEG_PROFILE;
    sub_stream.rc_config.mjpeg.quality = 0.8;

    camera_data.stream_config.streams[0] = &ai_stream;

    ret = camera_data.dev->reconfigure_streams(camera_data.dev, &camera_data.stream_config);
    if (ret != 0) {
        log_error("Failed to configure camera streams: %d", ret);
        return ret;
    }



    log_info("Camera streams reconfigured successfully.");
    return 0;
}



int stream_process_request( camera_stream_t  *stream, camera_request_template_t request_template) {
    memset(&camera_data.capture_request, 0, sizeof(camera_data.capture_request));
    camera_data.capture_request.settings = NULL;
    memset(&camera_data.output_buffer, 0, sizeof(camera_data.output_buffer));

    camera_data.capture_request.num_output_buffers = 1;
    camera_data.capture_request.output_buffers = s_static_capture_bufs;
    camera_data.capture_request.output_buffers[0].stream = stream;
    camera_data.capture_request.output_buffers[0].data = NULL;
    camera_data.capture_request.frame_number = 1000;
    camera_data.capture_request.request_template = request_template;

    int ret = camera_data.dev->process_capture_request(camera_data.dev, &camera_data.capture_request);
    if (ret != 0) {
        log_error("Failed to process request: %d", ret);
        return ret;
    }
    log_info("request processed successfully.");
    return 0;
}

/**
 * Single request for multiple streams (video+sub+ai etc.) to avoid HAL switching per stream and closing VPSS.
 * streams and num_streams specify which streams to enable; request_template is VIDEO_RECORD or NONE.
 */
static int stream_process_multi_request(camera_stream_t **streams, int num_streams,
                                        camera_request_template_t request_template)
{
    if (streams == NULL || num_streams <= 0)
        return -1;
    if (num_streams > (int)(sizeof(s_static_capture_bufs) / sizeof(s_static_capture_bufs[0]))) {
        log_error("stream_process_multi_request: num_streams=%d too large", num_streams);
        return -1;
    }
    memset(&camera_data.capture_request, 0, sizeof(camera_data.capture_request));
    camera_data.capture_request.settings = NULL;
    memset(&camera_data.output_buffer, 0, sizeof(camera_data.output_buffer));

    camera_data.capture_request.num_output_buffers = num_streams;
    camera_data.capture_request.output_buffers = s_static_capture_bufs;
    for (int i = 0; i < num_streams; i++) {
        camera_data.capture_request.output_buffers[i].stream = streams[i];
        camera_data.capture_request.output_buffers[i].data = NULL;
    }
    camera_data.capture_request.frame_number = 1000;
    camera_data.capture_request.request_template = request_template;

    int ret = camera_data.dev->process_capture_request(camera_data.dev, &camera_data.capture_request);
    if (ret != 0) {
        log_error("Failed to process multi-stream request: %d", ret);
        return ret;
    }
    log_info("multi-stream request processed successfully (num=%d).", num_streams);
    return 0;
}




int custom_process_request( ) {
    memset(&camera_data.capture_request, 0, sizeof(camera_data.capture_request));
    /* request_template: CIS 3A and other ISP settings;
     * if you need to modify after get_video_request_template, NULL uses default */
    camera_data.capture_request.settings = NULL;
    memset(&camera_data.output_buffer, 0, sizeof(camera_data.output_buffer));


    camera_data.capture_request.num_output_buffers = 1;
    camera_data.capture_request.output_buffers =
        malloc(sizeof(camera_stream_buffer_t) * camera_data.capture_request.num_output_buffers);
    camera_data.capture_request.output_buffers[0].stream = &video_stream;
    camera_data.capture_request.output_buffers[0].data = NULL;
    camera_data.capture_request.frame_number = 1000;
    camera_data.capture_request.request_template = CAMERA_TEMPLATE_VIDEO_RECORD;

    int ret = camera_data.dev->process_capture_request(camera_data.dev, &camera_data.capture_request);

    if (ret != 0) {
        log_error("Failed to process video request: %d", ret);
        return ret;
    }
    log_info("Video request processed successfully.");
    return 0;
}


void ai_enable(void)
{
    const camera_test_profile_t *prof = camera_test_get_profile();
    camera_ai_input_t parameter;

    /* Same as framework video_cfg: per-stream template for batch request (camera_hw reserved[0]). */
    ai_stream.reserved[0] = (void *)(intptr_t)CAMERA_TEMPLATE_AI;

    parameter.iou_threshold = prof->ai.iou_threshold;
    parameter.classify_threshold = prof->ai.classify_threshold;
    parameter.stream = &ai_stream;
    int len = sizeof(camera_ai_input_t);

    if (prof->ai.legacy_mode) {
        parameter.ai_type = CAMERA_AI_DETECT_UPPER_BODY;
        camera_data.dev->set_parameters(camera_data.dev, "AiConfig", (void *)&parameter, len);

        parameter.ai_type = CAMERA_AI_DETECT_GESTURE;
        camera_data.dev->set_parameters(camera_data.dev, "AiConfig", (void *)&parameter, len);

        parameter.ai_type = CAMERA_AI_DETECT_HUMAN_FACE;
        camera_data.dev->set_parameters(camera_data.dev, "AiConfig", (void *)&parameter, len);

        parameter.ai_type = CAMERA_AI_RECOGNIZE_HUMAN_FACE;
        camera_data.dev->set_parameters(camera_data.dev, "AiConfig", (void *)&parameter, len);

        parameter.ai_type = CAMERA_AI_DETECT_CLASSIFY_GESTURE;
        camera_data.dev->set_parameters(camera_data.dev, "AiConfig", (void *)&parameter, len);
    } else {
        parameter.ai_type = CAMERA_AI_DETECT_HUMAN_SHAPE;
        camera_data.dev->set_parameters(camera_data.dev, "AiConfig", (void *)&parameter, len);

        parameter.ai_type = CAMERA_AI_DETECT_PET;
        camera_data.dev->set_parameters(camera_data.dev, "AiConfig", (void *)&parameter, len);
    }

    if (prof->ai.legacy_mode && prof->ai.tile_overlap) {
        camera_ai_tile_overlap_t ai_tile_overlap_parameter;
        ai_tile_overlap_parameter.stream = &ai_stream;
        ai_tile_overlap_parameter.num_cols = prof->ai.tile_cols;
        ai_tile_overlap_parameter.num_rows = prof->ai.tile_rows;
        ai_tile_overlap_parameter.overlap_ratio = prof->ai.tile_overlap_ratio;
        camera_data.dev->set_parameters(camera_data.dev, "AiTileOverlap",
                                        (void *)&ai_tile_overlap_parameter,
                                        (unsigned int)sizeof(ai_tile_overlap_parameter));
        camera_data.dev->enable_camera_ai_tile_overlap(camera_data.dev, &ai_stream);
    }

    if (prof->ai.legacy_mode) {
        camera_data.dev->add_camera_ai(camera_data.dev, CAMERA_AI_DETECT_HUMAN_FACE);
        camera_data.dev->add_camera_ai(camera_data.dev, CAMERA_AI_DETECT_UPPER_BODY);
    } else {
        camera_data.dev->add_camera_ai(camera_data.dev, CAMERA_AI_DETECT_HUMAN_SHAPE);
        camera_data.dev->add_camera_ai(camera_data.dev, CAMERA_AI_DETECT_PET);
    }

    log_info("AI enabled");
}


void ai_disable(void)
{
    const camera_test_profile_t *prof = camera_test_get_profile();

    ai_stream.reserved[0] = NULL;

    if (prof->ai.legacy_mode && prof->ai.tile_overlap)
        camera_data.dev->disable_camera_ai_tile_overlap(camera_data.dev, &ai_stream);

    if (prof->ai.legacy_mode) {
        camera_data.dev->remove_camera_ai(camera_data.dev, CAMERA_AI_RECOGNIZE_HUMAN_FACE);
        camera_data.dev->remove_camera_ai(camera_data.dev, CAMERA_AI_DETECT_CLASSIFY_GESTURE);
        camera_data.dev->remove_camera_ai(camera_data.dev, CAMERA_AI_DETECT_UPPER_BODY);
        camera_data.dev->remove_camera_ai(camera_data.dev, CAMERA_AI_DETECT_GESTURE);
        camera_data.dev->remove_camera_ai(camera_data.dev, CAMERA_AI_DETECT_HUMAN_FACE);
    } else {
        camera_data.dev->remove_camera_ai(camera_data.dev, CAMERA_AI_DETECT_PET);
        camera_data.dev->remove_camera_ai(camera_data.dev, CAMERA_AI_DETECT_HUMAN_SHAPE);
    }

    log_info("AI disabled");
}

/** Stop streams, wait capture threads, then tear down AI (order matters for re-run). */
static void camera_test_stop_streams(camera_stream_t **streams, int num_streams, unsigned mask)
{
    log_info("CAM-DIAG test: stop_streams begin mask=0x%x num=%d", mask, num_streams);

    /* Stop AI thread before VI NONE so ch1 capture does not run into ak_vi_close. */
    if ((mask & STREAM_BIT_AI) != 0)
        ai_disable();
    m_sleep(200);

    log_info("CAM-DIAG test: send NONE");
    stream_process_multi_request(streams, num_streams, CAMERA_TEMPLATE_NONE);
    log_info("CAM-DIAG test: NONE sent, wait %dms", CAMERA_TEST_TEARDOWN_MS);
    m_sleep(CAMERA_TEST_TEARDOWN_MS);
    log_info("CAM-DIAG test: stop_streams end");
}

void close_camera_device() {
    log_info("CAM-DIAG test: close_device begin");
    memset(&camera_data.capture_request, 0, sizeof(camera_data.capture_request));
    camera_hw_device_close(camera_data.dev);
    camera_data.dev = NULL;
    /* Let ak_vi_close / ISP exit finish before next process opens VI (avoids 2nd-run CHN0 timeout). */
    log_info("CAM-DIAG test: cdev_close returned, wait %dms", CAMERA_TEST_POST_CLOSE_MS);
    m_sleep(CAMERA_TEST_POST_CLOSE_MS);
    log_info("CAM-DIAG test: close_device end");
    log_info("Camera device closed successfully.");
}

void close_camera_module() {
    if (camera_data.module) {
        camera_data.module = NULL;
        log_info("Camera module closed successfully.");
    }
}

/*===========================================================================
 * OSD Functions
 *===========================================================================*/

/**
 * @brief Initialize OSD device for camera overlay
 * @param font_file Font file path (can be NULL)
 * @return 0 on success, negative error code on failure
 */
int osd_init_device(const char *font_file)
{
    const struct hw_module_t *osd_module = NULL;
    int ret;

    if (osd_dev != NULL) {
        log_info("OSD already initialized");
        return 0;
    }

    ret = hw_get_module(OSD_HARDWARE_MODULE_ID, &osd_module);
    if (ret != 0 || osd_module == NULL) {
        log_error("get osd module failed");
        return -1;
    }

    ret = osd_hw_device_open(osd_module, &osd_dev);
    if (ret != 0 || osd_dev == NULL) {
        log_error("open osd device failed");
        return -1;
    }

    ret = osd_dev->init(osd_dev, NULL, font_file);
    if (ret != 0) {
        log_error("osd init failed");
        osd_hw_device_close(osd_dev);
        osd_dev = NULL;
        return -1;
    }

    log_info("OSD initialized successfully, font=%s", font_file ? font_file : "(default)");
    return 0;
}

/**
 * @brief Deinitialize OSD device
 */
void osd_deinit_device(void)
{
    if (osd_dev == NULL) {
        log_info("OSD not initialized");
        return;
    }

    osd_dev->deinit(osd_dev);
    osd_hw_device_close(osd_dev);
    osd_dev = NULL;
    log_info("OSD deinitialized successfully");
}

/**
 * @brief Draw text on OSD
 * @param chn Channel number
 * @param obj Object number
 * @param x X coordinate
 * @param y Y coordinate
 * @param text Text to draw
 */
int osd_draw_text_on_channel(int chn, int obj, int x, int y, const char *text)
{
    if (osd_dev == NULL) {
        log_error("OSD not initialized");
        return -1;
    }

    int ret = osd_dev->draw_text(osd_dev, chn, obj, x, y, text, strlen(text));
    if (ret != 0) {
        log_error("osd draw text failed");
        return -1;
    }

    ret = osd_dev->copy_cavas_to_vi(osd_dev, chn);
    if (ret != 0) {
        log_error("osd copy to vi failed");
        return -1;
    }

    log_info("OSD text drawn: chn=%d obj=%d pos=(%d,%d) text=%s", chn, obj, x, y, text);
    return 0;
}

/**
 * @brief Enable/disable OSD on channel
 * @param chn Channel number
 * @param obj Object number (-1 for all)
 * @param enable Enable flag
 */
int osd_set_enable(int chn, int obj, int enable)
{
    if (osd_dev == NULL) {
        log_error("OSD not initialized");
        return -1;
    }

    int ret = osd_dev->set_enable(osd_dev, chn, obj, enable);
    if (ret != 0) {
        log_error("osd set enable failed");
        return -1;
    }

    log_info("OSD %s: chn=%d obj=%d", enable ? "enabled" : "disabled", chn, obj);
    return 0;
}

/** Parse optional value from argv, e.g. --font path; returns NULL if not present */
static const char *get_opt_value(int argc, char **argv, const char *opt)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], opt) == 0 && i + 1 < argc)
            return argv[i + 1];
    }
    return NULL;
}

static int parse_size_pair(const char *text, uint32_t *width, uint32_t *height)
{
    unsigned int w = 0;
    unsigned int h = 0;

    if (!text || !*text || !width || !height)
        return -1;
    if (sscanf(text, "%ux%u", &w, &h) != 2 &&
        sscanf(text, "%uX%u", &w, &h) != 2 &&
        sscanf(text, "%u*%u", &w, &h) != 2)
        return -1;
    if (w == 0 || h == 0)
        return -1;
    *width = (uint32_t)w;
    *height = (uint32_t)h;
    return 0;
}

/** --size main|sub|ai <WxH> shorthand (also m/s/a). */
static void camera_test_parse_size_argv(int argc, char **argv,
                                        int *has_main, uint32_t *main_w, uint32_t *main_h,
                                        int *has_sub, uint32_t *sub_w, uint32_t *sub_h,
                                        int *has_ai, uint32_t *ai_w, uint32_t *ai_h)
{
    for (int i = 1; i + 2 < argc; i++) {
        uint32_t w, h;

        if (strcmp(argv[i], "--size") != 0)
            continue;
        if (parse_size_pair(argv[i + 2], &w, &h) != 0)
            continue;
        if (strcmp(argv[i + 1], "main") == 0 || strcmp(argv[i + 1], "m") == 0) {
            *main_w = w;
            *main_h = h;
            *has_main = 1;
        } else if (strcmp(argv[i + 1], "sub") == 0 || strcmp(argv[i + 1], "s") == 0) {
            *sub_w = w;
            *sub_h = h;
            *has_sub = 1;
        } else if (strcmp(argv[i + 1], "ai") == 0 || strcmp(argv[i + 1], "a") == 0) {
            *ai_w = w;
            *ai_h = h;
            *has_ai = 1;
        }
    }
}

static uint32_t camera_test_even_size(uint32_t v)
{
    if (v < 2)
        return 2;
    return v & ~1u;
}

static uint64_t camera_test_nv12_bytes(uint32_t w, uint32_t h, unsigned frame_depth)
{
    return (uint64_t)w * (uint64_t)h * 3u / 2u * (uint64_t)frame_depth;
}

/** Common stream sizes (nova gc5603 / luna os05a20 PDK tables); ascending area. */
static const struct {
    uint32_t w;
    uint32_t h;
} camera_test_main_presets[] = {
    { 640,  360 },
    {1280,  720 },
    {1920, 1080 },
    {2304, 1296 },
    {2560, 1440 },
    {2560, 1920 },
    {2688, 1520 },
    {2880, 1620 },
    {2880, 2160},
};

/**
 * Pick largest preset that fits sensor and VI NV12 buffer budget (nova + luna).
 */
static void camera_test_pick_default_main(uint32_t sensor_w, uint32_t sensor_h,
                                        uint32_t *out_w, uint32_t *out_h)
{
    uint32_t pick_w = single_main_channel_config[0].width;
    uint32_t pick_h = single_main_channel_config[0].height;
    int i;

    for (i = (int)(sizeof(camera_test_main_presets) / sizeof(camera_test_main_presets[0])) - 1;
         i >= 0; i--) {
        uint32_t w = camera_test_main_presets[i].w;
        uint32_t h = camera_test_main_presets[i].h;

        if (sensor_w > 0 && w > sensor_w)
            continue;
        if (sensor_h > 0 && h > sensor_h)
            continue;
        if (camera_test_nv12_bytes(w, h, camera_test_get_profile()->stream.vi_frame_depth) >
            CAMERA_TEST_VI_NV12_BUDGET_BYTES)
            continue;
        pick_w = w;
        pick_h = h;
        break;
    }

    *out_w = pick_w;
    *out_h = pick_h;
}

static int camera_test_main_size_is_sensor_keyword(const char *text)
{
    return text && (strcmp(text, "sensor") == 0 || strcmp(text, "full") == 0
                    || strcmp(text, "max") == 0);
}

static void camera_test_scale_sub_size(uint32_t main_w, uint32_t main_h,
                                       uint32_t *sub_w, uint32_t *sub_h)
{
    const uint32_t cap_w = 1280;

    if (main_w == 0 || main_h == 0) {
        *sub_w = 640;
        *sub_h = 360;
        return;
    }
    if (main_w <= cap_w) {
        *sub_w = main_w;
        *sub_h = main_h;
    } else {
        *sub_w = cap_w;
        *sub_h = (uint32_t)((uint64_t)main_h * cap_w / main_w);
    }
    *sub_w = camera_test_even_size(*sub_w);
    *sub_h = camera_test_even_size(*sub_h);
}

static void camera_test_patch_channel(camera_channel_config_t *ch,
                                      uint32_t width, uint32_t height,
                                      uint32_t crop_w, uint32_t crop_h)
{
    ch->width = width;
    ch->height = height;
    ch->crop.x = 0;
    ch->crop.y = 0;
    ch->crop.width = (int32_t)crop_w;
    ch->crop.height = (int32_t)crop_h;
    ch->max_buffers = camera_test_get_profile()->stream.vi_frame_depth;
}

/** Update all static stream channel_config tables (public camera.h fields only). */
static void camera_test_apply_all_channel_configs(uint32_t main_w, uint32_t main_h,
                                                  uint32_t sub_w, uint32_t sub_h,
                                                  uint32_t ai_w, uint32_t ai_h,
                                                  uint32_t crop_w, uint32_t crop_h)
{
    camera_test_patch_channel(&single_main_channel_config[0], main_w, main_h, crop_w, crop_h);
    camera_test_patch_channel(&splice_channel_config[0], main_w, main_h, crop_w, crop_h);
    camera_test_patch_channel(&splice_channel_config[1], main_w, main_h, crop_w, crop_h);
    camera_test_patch_channel(&single_sub_channel_config[0], sub_w, sub_h, crop_w, crop_h);
    camera_test_patch_channel(&single_ai_channel_config[0], ai_w, ai_h, crop_w, crop_h);
}

/** Optional rough ION hint (NV12 * max_buffers heuristic); not tied to VI mode (see HAL logs). */
static void camera_test_log_vi_memory_budget(uint32_t main_w, uint32_t main_h,
                                             uint32_t sub_w, uint32_t sub_h,
                                             uint32_t crop_w, uint32_t crop_h,
                                             unsigned max_buffers)
{
    const uint64_t main_frame = (uint64_t)main_w * main_h * 3u / 2u;
    const uint64_t sub_frame = (uint64_t)sub_w * sub_h * 3u / 2u;
    uint64_t ch0_kb;
    uint64_t ch1_kb;
    uint64_t venc_kb;
    uint64_t total_kb;

    ch0_kb = ((main_frame * (uint64_t)max_buffers + 1023u) / 1024u);
    ch1_kb = ((sub_frame * (uint64_t)max_buffers + 1023u) / 1024u);
    venc_kb = ((main_frame * 2u + 1023u) / 1024u);
    total_kb = ch0_kb + ch1_kb + venc_kb + (1843200u + 1023u) / 1024u;

    log_debug("ION rough hint: main %ux%u crop %ux%u max_buffers=%u (ch0~%llu + ch1~%llu + venc~%llu KB)",
              main_w, main_h, crop_w, crop_h, max_buffers,
              (unsigned long long)ch0_kb, (unsigned long long)ch1_kb, (unsigned long long)venc_kb);
    log_debug("  subtotal ~%llu KB (~%llu MiB)",
              (unsigned long long)total_kb, (unsigned long long)((total_kb + 1023u) / 1024u));
}

static void camera_test_apply_profile_to_static(void)
{
    const camera_test_profile_t *p = camera_test_get_profile();
    const camera_test_stream_group_t *s = &p->stream;

    single_main_channel_config[0].device_id = CAMERA_ID_1;
    single_sub_channel_config[0].device_id = CAMERA_ID_1;
    single_ai_channel_config[0].device_id = CAMERA_ID_1;
    splice_channel_config[0].device_id = CAMERA_ID_2;
    splice_channel_config[1].device_id = CAMERA_ID_3;

    camera_test_apply_all_channel_configs(s->main_w, s->main_h, s->sub_w, s->sub_h,
                                          s->ai_w, s->ai_h, s->crop_w, s->crop_h);
    video_stream.num_slices = s->vi_num_slices;
    sub_stream.num_slices = s->vi_num_slices;
    ai_stream.num_slices = s->vi_num_slices;
    picture_stream.num_slices = s->vi_num_slices;
    ai_stream.format = (system_pixel_format_t)s->ai_pixel_format;
    video_stream.fps = (int)s->vi_fps;
    sub_stream.fps = (int)s->vi_fps;
    ai_stream.fps = (int)s->vi_fps;
    picture_stream.fps = (int)s->vi_fps;
    video_stream.rc_config.avc.vbr.max_bitrate = p->codec.video_max_bitrate_kbps;
    apply_video_codec(p->codec.video_codec);
    apply_sub_codec(p->codec.sub_codec);
}

static void camera_test_print_profile(void)
{
    const camera_test_profile_t *p = camera_test_get_profile();

    log_info("profile \"%s\" stream %ux%u/%ux%u ai %ux%u crop %ux%u vi depth=%u slices=%u fps=%u",
             p->name, (unsigned)p->stream.main_w, (unsigned)p->stream.main_h,
             (unsigned)p->stream.sub_w, (unsigned)p->stream.sub_h,
             (unsigned)p->stream.ai_w, (unsigned)p->stream.ai_h,
             (unsigned)p->stream.crop_w, (unsigned)p->stream.crop_h,
             (unsigned)p->stream.vi_frame_depth, (unsigned)p->stream.vi_num_slices,
             (unsigned)p->stream.vi_fps);
}

static int camera_test_duration_sec(int argc, char **argv, int profile_sec)
{
    const char *v = get_opt_value(argc, argv, "--duration");

    if (v)
        return atoi(v) > 0 ? atoi(v) : profile_sec;
    if (g_test_quick) {
        const camera_test_profile_t *p = camera_test_get_profile();
        if (profile_sec == p->run.phase_sec)
            return p->run.quick_phase_sec;
        if (profile_sec == p->run.combo_sec)
            return p->run.quick_combo_sec;
    }
    return profile_sec;
}

static unsigned camera_test_profile_stream_mask(void)
{
    return camera_test_get_profile()->run.combo_stream_mask;
}

static int camera_test_profile_wants_ai(void)
{
    return (camera_test_profile_stream_mask() & CAMERA_TEST_STREAM_AI) != 0;
}

/** Profile defaults; optional CLI --size / --main-size overrides. */
static int apply_test_channel_resolutions(int argc, char **argv, int camera_id)
{
    const camera_test_profile_t *p = camera_test_get_profile();
    uint32_t main_w = p->stream.main_w;
    uint32_t main_h = p->stream.main_h;
    uint32_t sub_w = p->stream.sub_w;
    uint32_t sub_h = p->stream.sub_h;
    uint32_t ai_w = p->stream.ai_w;
    uint32_t ai_h = p->stream.ai_h;
    uint32_t crop_w = p->stream.crop_w;
    uint32_t crop_h = p->stream.crop_h;
    int has_main = 0;
    int has_sub = 0;
    int has_ai = 0;
    int has_crop = 0;
    const char *text;

    (void)camera_id;
    /*
     * Do NOT call get_camera_info() here: hal_camera_get_info() does ak_vi_open/close
     * and breaks the following configure_streams() (CHN[0] select timeout, main=0).
     */
    camera_test_apply_profile_to_static();

    camera_test_parse_size_argv(argc, argv, &has_main, &main_w, &main_h,
                                &has_sub, &sub_w, &sub_h, &has_ai, &ai_w, &ai_h);

    text = get_opt_value(argc, argv, "--main-size");
    if (text && parse_size_pair(text, &main_w, &main_h) == 0)
        has_main = 1;

    text = get_opt_value(argc, argv, "--sub-size");
    if (text && parse_size_pair(text, &sub_w, &sub_h) == 0)
        has_sub = 1;

    text = get_opt_value(argc, argv, "--ai-size");
    if (text && parse_size_pair(text, &ai_w, &ai_h) == 0)
        has_ai = 1;

    text = get_opt_value(argc, argv, "--crop-size");
    if (text && parse_size_pair(text, &crop_w, &crop_h) == 0)
        has_crop = 1;

    main_w = camera_test_even_size(main_w);
    main_h = camera_test_even_size(main_h);
    if (!has_sub) {
        sub_w = p->stream.sub_w;
        sub_h = p->stream.sub_h;
    }
    if (!has_ai) {
        ai_w = p->stream.ai_w;
        ai_h = p->stream.ai_h;
    }
    if (!has_crop) {
        crop_w = main_w;
        crop_h = main_h;
    }
    sub_w = camera_test_even_size(sub_w);
    sub_h = camera_test_even_size(sub_h);
    ai_w = camera_test_even_size(ai_w);
    ai_h = camera_test_even_size(ai_h);

    {
        const camera_test_stream_group_t *s = &camera_test_get_profile()->stream;

        video_stream.num_slices = s->vi_num_slices;
        sub_stream.num_slices = s->vi_num_slices;
        ai_stream.num_slices = s->vi_num_slices;
        picture_stream.num_slices = s->vi_num_slices;
    }

    camera_test_apply_all_channel_configs(main_w, main_h, sub_w, sub_h, ai_w, ai_h, crop_w, crop_h);
    ai_stream.format = (system_pixel_format_t)p->stream.ai_pixel_format;
    video_stream.fps = (int)p->stream.vi_fps;
    sub_stream.fps = (int)p->stream.vi_fps;
    ai_stream.fps = (int)p->stream.vi_fps;
    picture_stream.fps = (int)p->stream.vi_fps;
    video_stream.rc_config.avc.vbr.max_bitrate = p->codec.video_max_bitrate_kbps;

    log_info("channel: main=%ux%u sub=%ux%u ai=%ux%u crop=%ux%u fps=%u",
             main_w, main_h, sub_w, sub_h, ai_w, ai_h, crop_w, crop_h,
             (unsigned)p->stream.vi_fps);
    return 0;
}

static void camera_test_bind_argv(int argc, char **argv)
{
    g_test_argc = argc;
    g_test_argv = argv;
}

/** Parse --picture-at N from argv; default default_sec (combo test picture trigger second) */
static int get_picture_at_sec(int argc, char **argv, int default_sec)
{
    const char *v = get_opt_value(argc, argv, "--picture-at");
    return v ? atoi(v) : default_sec;
}

/** Return whether argv contains --osd */
static int has_opt(int argc, char **argv, const char *opt)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], opt) == 0)
            return 1;
    }
    return 0;
}

/** Set main stream codec from string; NULL or empty keeps default AVC */
static void apply_video_codec(const char *codec_str)
{
    if (codec_str == NULL || codec_str[0] == '\0')
        return;
    if (strcasecmp(codec_str, "h265") == 0 || strcasecmp(codec_str, "hevc") == 0) {
        video_stream.codec_config.codec_mode = CAMERA_CODEC_MODE_HEVC;
        video_stream.codec_config.profile = HEVC_PROFILE_MAIN;
        video_stream.codec_config.level = HEVC_LEVEL_3;
    } else if (strcasecmp(codec_str, "mjpeg") == 0 || strcasecmp(codec_str, "jpeg") == 0) {
        video_stream.codec_config.codec_mode = CAMERA_CODEC_MODE_MJPEG;
        video_stream.codec_config.profile = JPEG_PROFILE;
        video_stream.rc_config.mjpeg.quality = 0.8f;
    } else {
        /* h264 / avc or default */
        video_stream.codec_config.codec_mode = CAMERA_CODEC_MODE_AVC;
        video_stream.codec_config.profile = AVC_PROFILE_MAIN;
        video_stream.codec_config.level = AVC_LEVEL_3;
        video_stream.rc_config.avc.rc_mode = CAMERA_CODEC_RC_MODE_VBR;
        video_stream.rc_config.avc.vbr.max_bitrate =
            camera_test_get_profile()->codec.video_max_bitrate_kbps;
        video_stream.rc_config.avc.vbr.max_qp = 48;
        video_stream.rc_config.avc.vbr.min_qp = 20;
    }
}

/** Set sub stream codec from string; NULL or empty uses default MJPEG */
static void apply_sub_codec(const char *codec_str)
{
    if (codec_str == NULL || codec_str[0] == '\0') {
        sub_stream.codec_config.codec_mode = CAMERA_CODEC_MODE_MJPEG;
        sub_stream.codec_config.profile = JPEG_PROFILE;
        sub_stream.rc_config.mjpeg.quality = 0.8f;
        return;
    }
    if (strcasecmp(codec_str, "h265") == 0 || strcasecmp(codec_str, "hevc") == 0) {
        sub_stream.codec_config.codec_mode = CAMERA_CODEC_MODE_HEVC;
        sub_stream.codec_config.profile = HEVC_PROFILE_MAIN;
        sub_stream.codec_config.level = HEVC_LEVEL_3;
    } else if (strcasecmp(codec_str, "h264") == 0 || strcasecmp(codec_str, "avc") == 0) {
        sub_stream.codec_config.codec_mode = CAMERA_CODEC_MODE_AVC;
        sub_stream.codec_config.profile = AVC_PROFILE_MAIN;
        sub_stream.codec_config.level = AVC_LEVEL_3;
    } else {
        sub_stream.codec_config.codec_mode = CAMERA_CODEC_MODE_MJPEG;
        sub_stream.codec_config.profile = JPEG_PROFILE;
        sub_stream.rc_config.mjpeg.quality = 0.8f;
    }
}

/*===========================================================================
 * Parameterized test case implementations
 *===========================================================================*/

static int run_discover(int argc, char **argv)
{
    camera_test_bind_argv(argc, argv);
    int ret = open_camera_module();
    if (ret != 0) return ret;
    if (has_opt(argc, argv, "--probe-sensor"))
        ret = discover_devices();
    else
        ret = discover_devices_light();
    close_camera_module();
    return ret;
}

static int run_reconfigure(int argc, char **argv)
{
    camera_test_bind_argv(argc, argv);
    int ret = open_camera_module();
    if (ret != 0) return ret;
    ret = open_camera_device(0);
    if (ret != 0) { close_camera_module(); return ret; }
    ret = configure_camera_streams();
    if (ret != 0) { close_camera_device(); close_camera_module(); return ret; }
    if (camera_test_profile_wants_ai()) {
        ai_enable();
        stream_process_request(&ai_stream, CAMERA_TEMPLATE_VIDEO_RECORD);
        m_sleep(2000);
        ret = reconfigure_camera_streams();
        if (ret == 0)
            m_sleep(2000);
        stream_process_request(&ai_stream, CAMERA_TEMPLATE_NONE);
        ai_disable();
    } else {
        ret = 0;
    }
    close_camera_device();
    close_camera_module();
    return ret;
}

/** Multi-stream combo; streams/时长/OSD/拍照由 profile.run 决定。 */
static int run_combo(int argc, char **argv)
{
    const camera_test_profile_t *prof = camera_test_get_profile();
    unsigned mask = camera_test_profile_stream_mask();
    int duration = camera_test_duration_sec(argc, argv, prof->run.combo_sec);
    int use_picture = prof->run.enable_picture;
    int picture_at = prof->run.picture_at_sec;
    int use_osd = prof->run.enable_osd;
    const char *font = prof->run.osd_font;
    camera_stream_t *combo_streams[3];
    int num_streams = 0;

    camera_test_bind_argv(argc, argv);

    if (mask & STREAM_BIT_VIDEO)
        combo_streams[num_streams++] = &video_stream;
    if (mask & STREAM_BIT_SUB)
        combo_streams[num_streams++] = &sub_stream;
    if (mask & STREAM_BIT_AI)
        combo_streams[num_streams++] = &ai_stream;
    if (num_streams == 0) {
        log_error("combo: empty stream mask in profile");
        return -1;
    }
    if (use_picture && (picture_at < 0 || picture_at > duration))
        picture_at = duration > 0 ? duration / 2 : 0;

    int ret = open_camera_module();
    if (ret != 0) return ret;
    ret = open_camera_device(0);
    if (ret != 0) { close_camera_module(); return ret; }
    ret = configure_camera_streams();
    if (ret != 0) { close_camera_device(); close_camera_module(); return ret; }
    if (use_osd) {
        ret = osd_init_device(font);
        if (ret != 0) { close_camera_device(); close_camera_module(); return ret; }
        ret = osd_draw_text_on_channel(0, 0, 10, 10, "Combo Test");
        if (ret != 0) { osd_deinit_device(); close_camera_device(); close_camera_module(); return ret; }
    }

    if ((mask & STREAM_BIT_AI) != 0)
        ai_enable();

    ret = stream_process_multi_request(combo_streams, num_streams, CAMERA_TEMPLATE_VIDEO_RECORD);
    if (ret != 0) {
        if ((mask & STREAM_BIT_AI) != 0)
            ai_disable();
        if (use_osd)
            osd_deinit_device();
        close_camera_device();
        close_camera_module();
        return ret;
    }

    for (int t = 0; t < duration; t++) {
        if (use_picture && t == picture_at) {
            log_info("Combo: trigger picture at t=%d sec", t);
            stream_process_request(&picture_stream, CAMERA_TEMPLATE_PICTURE);
            m_sleep(200);
            stream_process_multi_request(combo_streams, num_streams, CAMERA_TEMPLATE_VIDEO_RECORD);
        }
        m_sleep(1000);
    }

    camera_test_stop_streams(combo_streams, num_streams, mask);
    if (use_osd)
        osd_deinit_device();

    close_camera_device();
    close_camera_module();
    return 0;
}

/** 按参数选择一路或多路出流；configure/request 使用相同 stream mask。 */
static int run_stream(int argc, char **argv)
{
    const camera_test_profile_t *prof = camera_test_get_profile();
    unsigned mask = 0;
    int duration = camera_test_duration_sec(argc, argv, prof->run.combo_sec);
    camera_stream_t *streams[4];
    int num_streams = 0;
    int tpl;
    int ret;

    camera_test_bind_argv(argc, argv);
    if (camera_test_parse_stream_argv(argc, argv, &mask) != 0)
        return -1;

    num_streams = camera_test_mask_to_streams(mask, streams, 4);
    if (num_streams <= 0)
        return -1;

    tpl = camera_test_pick_request_template(mask);

    log_info("stream: mask=0x%x num=%d duration=%ds (profile %s)",
             mask, num_streams, duration, prof->name);

    ret = open_camera_module();
    if (ret != 0)
        return ret;
    ret = open_camera_device(0);
    if (ret != 0) {
        close_camera_module();
        return ret;
    }

    ret = configure_camera_streams_with_mask(mask);
    if (ret != 0) {
        close_camera_device();
        close_camera_module();
        return ret;
    }

    if ((mask & STREAM_BIT_AI) != 0)
        ai_enable();

    stream_frames_reset();
    log_info("CAM-DIAG test: stream start tpl=%d mask=0x%x", tpl, mask);
    ret = stream_process_multi_request(streams, num_streams, tpl);
    if (ret != 0) {
        if ((mask & STREAM_BIT_AI) != 0)
            ai_disable();
        close_camera_device();
        close_camera_module();
        return ret;
    }

    for (int t = 0; t < duration; t++) {
        m_sleep(1000);
        stream_log_out_frames(mask, t + 1);
    }

    camera_test_stop_streams(streams, num_streams, mask);
    close_camera_device();
    close_camera_module();
    return 0;
}

/**
 * Application-style flow: one configure_streams with all streams (main/sub/AI/picture),
 * then enable requests per need; between phases use NONE to stop previous stream(s),
 * and periodic logs verify each request path is delivering frames.
 * --duration: seconds per streaming phase (default 4). Picture phase is one shot + short wait.
 */
static int run_app_flow(int argc, char **argv)
{
    const camera_test_profile_t *prof = camera_test_get_profile();
    int phase_sec;

    camera_test_bind_argv(argc, argv);
    phase_sec = camera_test_duration_sec(argc, argv, prof->run.phase_sec);
    if (phase_sec < 1)
        phase_sec = 1;

    int ret = open_camera_module();
    if (ret != 0)
        return ret;
    ret = open_camera_device(0);
    if (ret != 0) {
        close_camera_module();
        return ret;
    }

    ret = configure_camera_streams();
    if (ret != 0) {
        close_camera_device();
        close_camera_module();
        return ret;
    }
    /* 1) Main only（ai_enable 延至第 3 阶段，避免 SVP/NPU 占 ION 导致主码流 CHN0 无帧） */
    stream_frames_reset();
    ret = stream_process_request(&video_stream, CAMERA_TEMPLATE_VIDEO_RECORD);
    if (ret != 0)
        goto app_flow_cleanup;
    for (int t = 0; t < phase_sec; t++) {
        m_sleep(1000);
        app_flow_log_line("main_only", t + 1);
    }
    stream_process_request(&video_stream, CAMERA_TEMPLATE_NONE);
    m_sleep(200);

    /* 2) Sub only */
    stream_frames_reset();
    ret = stream_process_request(&sub_stream, CAMERA_TEMPLATE_VIDEO_RECORD);
    if (ret != 0)
        goto app_flow_cleanup;
    for (int t = 0; t < phase_sec; t++) {
        m_sleep(1000);
        app_flow_log_line("sub_only", t + 1);
    }
    stream_process_request(&sub_stream, CAMERA_TEMPLATE_NONE);
    m_sleep(200);

    /* 3) AI only */
    if (camera_test_profile_wants_ai())
        ai_enable();

    stream_frames_reset();
    ret = stream_process_request(&ai_stream, CAMERA_TEMPLATE_VIDEO_RECORD);
    if (ret != 0)
        goto app_flow_cleanup;
    for (int t = 0; t < phase_sec; t++) {
        m_sleep(1000);
        app_flow_log_line("ai_only", t + 1);
    }
    stream_process_request(&ai_stream, CAMERA_TEMPLATE_NONE);
    m_sleep(200);

    /* 4) Picture one-shot (template PICTURE), then log accumulated pic count */
    stream_frames_reset();
    ret = stream_process_request(&picture_stream, CAMERA_TEMPLATE_PICTURE);
    if (ret != 0)
        goto app_flow_cleanup;
    m_sleep(800);
    app_flow_log_line("picture_once", 0);
    m_sleep(200);

    /* 5) Multi-stream request: main + sub + AI together */
    stream_frames_reset();
    {
        camera_stream_t *triple[] = { &video_stream, &sub_stream, &ai_stream };
        ret = stream_process_multi_request(triple,
                                         (int)(sizeof(triple) / sizeof(triple[0])),
                                         CAMERA_TEMPLATE_VIDEO_RECORD);
        if (ret != 0)
            goto app_flow_cleanup;
        for (int t = 0; t < phase_sec; t++) {
            m_sleep(1000);
            app_flow_log_line("multi_main_sub_ai", t + 1);
        }
        stream_process_multi_request(triple,
                                   (int)(sizeof(triple) / sizeof(triple[0])),
                                   CAMERA_TEMPLATE_NONE);
    }

    ret = 0;

app_flow_cleanup:
    m_sleep(CAMERA_TEST_TEARDOWN_MS);
    if (camera_test_profile_wants_ai())
        ai_disable();
    m_sleep(200);
    close_camera_device();
    close_camera_module();
    return ret;
}

static int run_all_tests(int argc, char **argv)
{
    int ret;

    g_test_quick = 0;
    camera_test_bind_argv(argc, argv);
    log_info("======== camera test ALL (%s%s) ========",
             g_test_quick ? "quick, " : "", camera_test_get_profile()->name);

    log_info("--- [1/4] discover (no VI probe) ---");
    ret = open_camera_module();
    if (ret != 0)
        return ret;
    ret = discover_devices_light();
    close_camera_module();
    if (ret != 0)
        return ret;

    log_info("--- [2/4] app_flow (per-stream) ---");
    ret = run_app_flow(argc, argv);
    if (ret != 0)
        return ret;

    log_info("--- [3/4] reconfigure ---");
    ret = run_reconfigure(argc, argv);
    if (ret != 0)
        return ret;

    log_info("--- [4/4] combo ---");
    ret = run_combo(argc, argv);
    if (ret == 0)
        log_info("======== camera test ALL pass ========");
    return ret;
}

static int run_quick(int argc, char **argv)
{
    int ret;

    g_test_quick = 1;
    camera_test_bind_argv(argc, argv);
    log_info("======== camera test QUICK (%s) ========", camera_test_get_profile()->name);

    ret = open_camera_module();
    if (ret != 0)
        return ret;
    ret = discover_devices_light();
    close_camera_module();
    if (ret != 0)
        return ret;
    ret = run_app_flow(argc, argv);
    if (ret != 0)
        return ret;
    ret = run_combo(argc, argv);
    if (ret == 0)
        log_info("======== camera test QUICK pass ========");
    return ret;
}

#ifdef CONFIG_OS_RTOS
static int hw_camera_cmd(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
    soc_platform_init(0);

    log_set_level(LOG_VERBOSE);
    thread_init();
    camera_test_apply_profile_to_static();
    log_info("camera test [%s]: %s", CAMERA_TEST_SOC_NAME, argc > 1 ? argv[1] : "help");
    camera_test_print_profile();

    hw_module_init();

    if (argc < 2 || strcmp(argv[1], "help") == 0 ||
        strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_help(argv[0]);
        return (argc < 2) ? -1 : 0;
    }

    /* Accept both "stream" and "--stream" for compatibility. */
    const char *test_name = argv[1];
    if (test_name[0] == '-' && test_name[1] == '-') {
        test_name += 2;
    }
    const camera_test_case_t *tc = NULL;
    for (int i = 0; i < camera_test_count; i++) {
        if (strcmp(camera_tests[i].name, test_name) == 0) {
            tc = &camera_tests[i];
            break;
        }
    }
    if (tc == NULL) {
        log_error("Unknown test case: %s", test_name);
        print_help(argv[0]);
        return -1;
    }

    log_info("=== Running test: %s - %s ===", tc->name, tc->desc);
    return tc->run(argc, argv);
}

#ifdef CONFIG_OS_RTOS
SHELL_CMD_EXPORT_ALIAS(hw_camera_cmd, hw_camera_cmd, camera test);
#endif
