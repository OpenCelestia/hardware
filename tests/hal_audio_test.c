#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#include "hardware.h"
#include "audio.h"

#include "ak_common.h"//for temp, msleep
#include "soc_platform.h"
#include "system_thread.h"
#include "log.h"
#define LOG_TAG "HAL AUDIO TEST"



struct audio_test_play_data {
    char *file_name;
    struct audio_hw_device *audio_dev;

    audio_format_t format;
    audio_channel_mask_t channel;
    audio_devices_t device;
    audio_mode_t mode;
    int sample_rate;
};

struct audio_test_record_data {
    char *codec_file_name;
    char *pcm_file_name;
    FILE *pcm_file;
    FILE *codec_file;
    struct audio_hw_device *audio_dev;
    struct audio_stream_in *stream_in;
    struct timespec record_t0;
    int record_clock_started;
    uint64_t record_codec_bytes;

    audio_format_t format;
    audio_channel_mask_t channel;
    audio_devices_t device;
    audio_mode_t mode;
    audio_effect_type_t effect;
    int sample_rate;
};

typedef struct audio_test_play_data play_data_t;
typedef struct audio_test_record_data record_data_t;


play_data_t play_data;
record_data_t record_data;

static double audio_test_elapsed_sec(const struct timespec *t0, const struct timespec *t1)
{
    return (double)(t1->tv_sec - t0->tv_sec) +
           (double)(t1->tv_nsec - t0->tv_nsec) / 1000000000.0;
}

void process_capture_result(const audio_callback_ops_t *ops,const audio_capture_result_t *result);
void notify(const audio_callback_ops_t *ops, const audio_notify_msg_t *msg) ;

// 回调函数结构体
audio_callback_ops_t callback_ops = {
    .process_capture_result = process_capture_result,
    .notify = notify,
};


// 回调处理函数
void process_capture_result(const audio_callback_ops_t *ops,const audio_capture_result_t *result)
{
//    log_info("frame_number:%d, timestamp:%d,format = %x, data:%x,data_size:%d", \
    result->frame_number,result->timestamp,result->output_buffer->config->format, result->output_buffer->data,result->output_buffer->data_size);
    
//    log_info("input: data size:%d, address:%x\n", \
//        result->input_buffer->data_size, result->input_buffer->data);

    if (result->output_buffer != NULL && result->output_buffer->data != NULL &&
        result->output_buffer->data_size > 0) {
        if ((result->output_buffer->config->format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_PCM) {
            fwrite(result->output_buffer->data, 1, result->output_buffer->data_size, record_data.pcm_file);
        } else {
            fwrite(result->output_buffer->data, 1, result->output_buffer->data_size, record_data.codec_file);
            record_data.record_codec_bytes += result->output_buffer->data_size;
        }
    }
}


//事件通知
void notify(const audio_callback_ops_t *ops, const audio_notify_msg_t *msg)
{
//    log_info("notify：%x",msg->type);
    int32_t curAzimuth = 0;

    if (msg->type & AUDIO_EFFECT_DETECT_MASK)
    {
        log_error("detect:%x,value: %d", msg->type, msg->detect.value);
    }
    else if (msg->type & AUDIO_EFFECT_SSL_MASK)
    {
        curAzimuth = msg->ssl.azimuth * 90 / (1<<15);
        log_error("azimuth:%d, curAzimuth: %d",msg->ssl.azimuth, curAzimuth);
    }
}



int audio_test_record( void ) {
    int ret;
    struct hw_module_t *module = NULL;

    record_data.pcm_file = fopen(record_data.pcm_file_name, "wb");
    if (!record_data.pcm_file) {
        log_error( "Failed to open file for recording");
        audio_hw_device_close(record_data.audio_dev);
        return -1;
    }

    record_data.codec_file = fopen(record_data.codec_file_name, "wb");
    if (!record_data.codec_file) {
        log_error( "Failed to open file for recording");
        audio_hw_device_close(record_data.audio_dev);
        return -1;
    }

    ret = hw_get_module(AUDIO_HARDWARE_MODULE_ID, (const struct hw_module_t **)&module);
    if (ret) {
        log_error( "Failed to get audio hardware module: %d", ret);
        return -1;
    }

    ret = audio_hw_device_open(module, &record_data.audio_dev);
    if (ret) {
        log_error( "Failed to open audio hardware device: %d", ret);
        return -1;
    }

    ret = record_data.audio_dev->set_callback_ops(record_data.audio_dev, &callback_ops);

    struct audio_config config = {
        .sample_rate = record_data.sample_rate,
        .channel_mask = record_data.channel,
        .format = record_data.format,
    };

    record_data.audio_dev->set_mode(record_data.audio_dev, record_data.mode);

    record_data.stream_in = NULL;
    record_data.record_clock_started = 0;
    record_data.record_codec_bytes = 0;

    ret = record_data.audio_dev->open_input_stream(record_data.audio_dev, 0, record_data.device,
                                                   &config, &record_data.stream_in);
    if (ret) {
        log_error( "Failed to open input stream: %d", ret);
        audio_hw_device_close(record_data.audio_dev);
        return -1;
    }


    if (record_data.effect != AUDIO_EFFECT_NONE) {
        const struct audio_stream *effect_stream_in = (struct audio_stream *)record_data.stream_in;
        int len = sizeof(audio_effect_detect_parameter);
        audio_effect_detect_parameter parameter;

        parameter.sensitivity = 0.5;
        if (record_data.effect & AUDIO_EFFECT_NPU_CRY)
            record_data.stream_in->common.set_parameters(effect_stream_in, "NpuCry", &parameter, len);
        if (record_data.effect & AUDIO_EFFECT_VAD)
            record_data.stream_in->common.set_parameters(effect_stream_in, "Vad", &parameter, len);
        if (record_data.effect & AUDIO_EFFECT_LSSL)
            record_data.stream_in->common.set_parameters(effect_stream_in, "Lssl", &parameter, len);

        record_data.stream_in->common.add_audio_effect(effect_stream_in, record_data.effect);
    }

    if (clock_gettime(CLOCK_MONOTONIC, &record_data.record_t0) == 0)
        record_data.record_clock_started = 1;

    return 0;


}


int audio_test_play( void) {
    int ret;
    struct hw_module_t *module = NULL;

    ret = hw_get_module(AUDIO_HARDWARE_MODULE_ID, (const struct hw_module_t **)&module);
    if (ret) {
        log_error( "Failed to get audio hardware module: %d", ret);
        return -1;
    }

    ret = audio_hw_device_open(module, &play_data.audio_dev);
    if (ret) {
        log_error( "Failed to open audio hardware device: %d", ret);
        return -1;
    }

    struct audio_stream_out *stream_out = NULL;
    struct audio_config config = {
        .sample_rate = play_data.sample_rate,
        .channel_mask = play_data.channel,
        .format = play_data.format,
    };

    play_data.audio_dev->set_mode(play_data.audio_dev, play_data.mode);

    ret = play_data.audio_dev->open_output_stream(play_data.audio_dev, 0, play_data.device,  &config, &stream_out);
    if (ret) {
        log_error( "Failed to open output stream: %d", ret);
        audio_hw_device_close(play_data.audio_dev);
        return -1;
    }

    log_info("Playback file: %s", play_data.file_name);
    FILE *file = fopen(play_data.file_name, "rb");

    if (!file) {
        log_error("Failed to open file for playback: %s", play_data.file_name);
        audio_hw_device_close(play_data.audio_dev);
        return -1;
    }




    void *buffer = malloc(4096);
    if (!buffer) {
        log_error( "Failed to allocate buffer\n");
        fclose(file);
        audio_hw_device_close(play_data.audio_dev);
        return -1;
    }

    if (play_data.format == AUDIO_FORMAT_AMR) {
        fread(buffer, 1, 6, file);
    }

    log_info("Playing... Press Ctrl+C to stop.");
    while (1) {
        size_t bytes_read = fread(buffer, 1, 4096, file);

        if (bytes_read == 0) {
            break;
        }
        ret = stream_out->write(stream_out, buffer, bytes_read);

        if (ret < 0) {
            log_error( "Failed to write to output stream: %d\n", ret);
            break;
        }
    }

    free(buffer);
    fclose(file);

    play_data.audio_dev->close_output_stream(play_data.audio_dev, stream_out);
    audio_hw_device_close(play_data.audio_dev);

    return 0;
}



#include <pthread.h>
# if 0
void *record_thread(void *arg) {
    record_pcm();
    return NULL;
}

void *play_thread(void *arg) {
    play_pcm();
    return NULL;
}

int loopback( play_data_t play, record_data_t record ) {
    pthread_t record_tid, play_tid;

    pthread_create(&record_tid, NULL, record_thread, NULL);
    pthread_create(&play_tid, NULL, play_thread, NULL);

    pthread_join(record_tid, NULL);
    pthread_join(play_tid, NULL);

    return 0;
}

#endif

int audio_test_loopback( play_data_t play, record_data_t record ) {




    return 0;
}




//***************************************************
//check input type
//***************************************************


static audio_format_t get_codec_format(const char *type)
{
    audio_format_t format = -1;

    /* check the audio codec format */
    if (0 == strcmp(type, "mp3"))
        format = AUDIO_FORMAT_MP3;
    else if (0 == strcmp(type, "amr"))
        format = AUDIO_FORMAT_AMR;
    else if (0 == strcmp(type, "aac"))
        format = AUDIO_FORMAT_AAC;
    else if (0 == strcmp(type, "g711a"))
        format = AUDIO_FORMAT_ALAW;
    else if (0 == strcmp(type, "g711u"))
        format = AUDIO_FORMAT_ULAW;
    else if (0 == strcmp(type, "pcm"))
        format = AUDIO_FORMAT_PCM;
    else /* unknow type */
        format = -1;

    return format;
}


static audio_devices_t get_in_device(const char *type)
{
    audio_devices_t device = -1;

    /* check the audio device */
    if (0 == strcmp(type, "AUTO"))
        device = AUDIO_DEVICE_NONE;
    if (0 == strcmp(type, "AMIC"))
        device = AUDIO_DEVICE_IN_AMIC;
    else if (0 == strcmp(type, "DMIC"))
        device = AUDIO_DEVICE_IN_DMIC;
    else if (0 == strcmp(type, "LINE"))
        device = AUDIO_DEVICE_IN_LINE;
    else if (0 == strcmp(type, "BUS0"))
        device = AUDIO_DEVICE_IN_BUS0;
    else if (0 == strcmp(type, "BUS1"))
        device = AUDIO_DEVICE_IN_BUS1;
    else if (0 == strcmp(type, "USB"))
        device = AUDIO_DEVICE_IN_USB_DEVICE;
    else /* unknow type */
        device = -1;

    return device;
}

static audio_devices_t get_out_device(const char *type)
{
    audio_devices_t device = -1;

    /* check the audio device */
    if (0 == strcmp(type, "SPEAKER"))
        device = AUDIO_DEVICE_OUT_SPEAKER;
    else if (0 == strcmp(type, "LINE"))
        device = AUDIO_DEVICE_OUT_LINE;
    else if (0 == strcmp(type, "USB"))
        device = AUDIO_DEVICE_OUT_USB_DEVICE;
    else if (0 == strcmp(type, "BUS"))
        device = AUDIO_DEVICE_OUT_BUS;
    else /* unknow type */
        device = -1;

    return device;
}

static audio_channel_mask_t get_in_channel(const char *type)
{
    audio_channel_mask_t channel = -1;

    /* check the audio channel */
    if (0 == strcmp(type, "MONO"))
        channel = AUDIO_CHANNEL_IN_MONO;
    else if (0 == strcmp(type, "STEREO"))
        channel = AUDIO_CHANNEL_IN_STEREO;
    else if (0 == strcmp(type, "TRI"))
        channel = AUDIO_CHANNEL_IN_TRI;
    else if (0 == strcmp(type, "QUAD"))
        channel = AUDIO_CHANNEL_IN_QUAD;
    else /* unknow type */
        channel = -1;

    return channel;
}


static audio_channel_mask_t get_out_channel(const char *type)
{
    audio_channel_mask_t channel = -1;

    /* check the audio channel */
    if (0 == strcmp(type, "MONO"))
        channel = AUDIO_CHANNEL_OUT_MONO;
    else if (0 == strcmp(type, "STEREO"))
        channel = AUDIO_CHANNEL_OUT_STEREO;
    else /* unknow type */
        channel = -1;

    return channel;
}


static audio_effect_type_t get_effect(const char *type)
{
    audio_effect_type_t effect = -1;

    /* check the audio channel */
    if (0 == strcmp(type, "VAD"))
        effect = AUDIO_EFFECT_VAD;
    else if (0 == strcmp(type, "CPU_CRY"))
        effect = AUDIO_EFFECT_CPU_CRY;
    else if (0 == strcmp(type, "NPU_CRY"))
        effect = AUDIO_EFFECT_NPU_CRY;
    else /* unknow type */
        effect = -1;

    return effect;
}

static audio_mode_t get_mode(const char *type)
{
    audio_mode_t mode = -1;

    /* check the audio channel */
    if (0 == strcmp(type, "NORMAL"))
        mode = AUDIO_MODE_NORMAL;
    else if (0 == strcmp(type, "CALL"))
        mode = AUDIO_MODE_CALL;
    else if (0 == strcmp(type, "MUSIC"))
        mode = AUDIO_MODE_MUSIC;
    else /* unknow type */
        mode = -1;

    return mode;
}

static int get_sample_rate(const char *type)
{
    int  sample_rate = -1;

    /* check the audio channel */
    if ((0 == strcmp(type, "8000")) || (0 == strcmp(type, "16000")) ||
        (0 == strcmp(type, "32000")) || (0 == strcmp(type, "44100"))||
        (0 == strcmp(type, "11025")) || (0 == strcmp(type, "12000"))||
        (0 == strcmp(type, "22050")) || (0 == strcmp(type, "24000"))||
        (0 == strcmp(type, "32000")) || (0 == strcmp(type, "48000")))

        sample_rate = atoi(type);
    else /* unknow type */
        sample_rate = -1;

    return sample_rate;
}

#define AUDIO_TEST_PATH_MAX 512

static char *audio_test_strdup(const char *s)
{
    size_t n;
    char *p;

    if (s == NULL)
        return NULL;
    n = strlen(s) + 1;
    p = malloc(n);
    if (p != NULL)
        memcpy(p, s, n);
    return p;
}

/* Absolute path as-is; otherwise relative to the executable directory. */
static char *audio_test_get_exe_dir(void)
{
    char link_buf[AUDIO_TEST_PATH_MAX];
    char *dir;
    ssize_t n;

#if defined(OS_NAME_LINUX) || defined(__linux__)
    n = readlink("/proc/self/exe", link_buf, sizeof(link_buf) - 1);
    if (n > 0) {
        link_buf[n] = '\0';
        dir = audio_test_strdup(link_buf);
        if (dir != NULL) {
            char *slash = strrchr(dir, '/');
            if (slash != NULL) {
                if (slash == dir)
                    slash[1] = '\0';
                else
                    *slash = '\0';
            }
            return dir;
        }
    }
#endif
    {
        char cwd[AUDIO_TEST_PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) != NULL)
            return audio_test_strdup(cwd);
    }
    return audio_test_strdup(".");
}

static char *audio_test_path_join_dir(const char *dir, const char *name)
{
    size_t len;
    char *out;

    if (dir == NULL || name == NULL || name[0] == '\0')
        return NULL;

    len = strlen(dir) + 1 + strlen(name) + 1;
    out = malloc(len);
    if (out == NULL)
        return NULL;
    snprintf(out, len, "%s/%s", dir, name);
    return out;
}

static char *audio_test_path_resolve(const char *path, const char *exe_dir, int *owned)
{
    if (path == NULL) {
        if (owned)
            *owned = 0;
        return NULL;
    }
    if (path[0] == '/') {
        if (owned)
            *owned = 0;
        return (char *)path;
    }
    if (owned)
        *owned = 1;
    return audio_test_path_join_dir(exe_dir, path);
}

static void audio_test_setup_paths(const char *audio_format,
                                   int *owned_play, int *owned_pcm, int *owned_codec)
{
    char *exe_dir;
    char name[64];

    *owned_play = *owned_pcm = *owned_codec = 0;
    exe_dir = audio_test_get_exe_dir();
    if (exe_dir == NULL)
        exe_dir = audio_test_strdup(".");

    if (play_data.file_name != NULL && play_data.file_name == record_data.pcm_file_name &&
        play_data.file_name == record_data.codec_file_name) {
        play_data.file_name = audio_test_path_resolve(play_data.file_name, exe_dir, owned_play);
        record_data.pcm_file_name = record_data.codec_file_name = play_data.file_name;
        *owned_pcm = *owned_codec = *owned_play;
        free(exe_dir);
        return;
    }

    if (play_data.file_name != NULL)
        play_data.file_name = audio_test_path_resolve(play_data.file_name, exe_dir, owned_play);
    else {
        snprintf(name, sizeof(name), "play.%s", audio_format);
        play_data.file_name = audio_test_path_join_dir(exe_dir, name);
        *owned_play = (play_data.file_name != NULL);
    }

    if (record_data.pcm_file_name != NULL)
        record_data.pcm_file_name = audio_test_path_resolve(record_data.pcm_file_name, exe_dir, owned_pcm);
    else {
        record_data.pcm_file_name = audio_test_path_join_dir(exe_dir, "record.pcm");
        *owned_pcm = (record_data.pcm_file_name != NULL);
    }

    if (record_data.codec_file_name != NULL)
        record_data.codec_file_name = audio_test_path_resolve(record_data.codec_file_name, exe_dir, owned_codec);
    else {
        snprintf(name, sizeof(name), "record.%s", audio_format);
        record_data.codec_file_name = audio_test_path_join_dir(exe_dir, name);
        *owned_codec = (record_data.codec_file_name != NULL);
    }

    free(exe_dir);
}

static void audio_test_free_paths(int owned_play, int owned_pcm, int owned_codec)
{
    if (owned_play && play_data.file_name != NULL)
        free(play_data.file_name);
    else {
        if (owned_pcm && record_data.pcm_file_name != NULL)
            free(record_data.pcm_file_name);
        if (owned_codec && record_data.codec_file_name != NULL)
            free(record_data.codec_file_name);
    }
}

static void audio_test_set_file_arg(const char *path, const char **audio_format)
{
    play_data.file_name = record_data.pcm_file_name = record_data.codec_file_name = (char *)path;
    if (path != NULL && audio_format != NULL) {
        const char *dot = strrchr(path, '.');
        if (dot != NULL && dot[1] != '\0') {
            audio_format_t fmt = get_codec_format(dot + 1);
            if (fmt != (audio_format_t)-1) {
                play_data.format = record_data.format = fmt;
                *audio_format = dot + 1;
            }
        }
    }
}


static int audio_test_is_command(const char *arg)
{
    return arg != NULL &&
           (!strcmp(arg, "record") || !strcmp(arg, "play") || !strcmp(arg, "loopback"));
}

void print_help(const char *program_name)
{
    printf("Usage: %s <command> [options]\n", program_name);
    printf("Command:\n");
    printf("    play                            Play an audio file\n");
    printf("    record                          Record audio to a file\n");
    printf("    loopback                        Loopback audio from input device to output device\n");

    printf("Options:\n");
    printf("    --format <format>               pcm/aac/mp3/amr/g711u/g711a \n");
    printf("    --file <path>                   path (absolute or relative to executable dir)\n");
    printf("    --in_device <device>            AUTO/AMIC/DMIC/BUS0/BUS1/LINE/USB \n");
    printf("    --out_device <device>           SPEAKER/BUS/LINE \n");
    printf("    --in_channel <chanel number>    MONO/STEREO/TRI/QUAD \n");
    printf("    --out_channel <chanel number>   MONO/STEREO \n");
    printf("    --mode <mode>                   NORMAL/CALL/MUSIC \n");
    printf("    --effect <effect type>          CPU_CRY/NPU_CRY/VAD \n");
    printf("    --sample_rate <sample_rate>     8000/11025/12000/16000/22050/24000/32000/44100/48000 \n");
    printf("    --time <seconds>                auto exit after N seconds (for record) \n");
}

//*******************************************************


//static int hw_audio_command(int argc, char **argv)
#ifdef CONFIG_OS_RTOS
static int hw_audio_cmd(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
    /* start the application */
    soc_platform_init(0);

    log_set_level(LOG_INFO);
    thread_init();

    hw_module_init();

    if (argc < 2 ) {
        print_help(argv[0]);
        return -1;
    }




    const char *command;
    const char *audio_format = "pcm";
    int run_seconds = -1;

    bool set_type_error_flag = false;
    int owned_play_path = 0;
    int owned_pcm_path = 0;
    int owned_codec_path = 0;

    //init
    play_data.file_name = NULL;
    play_data.format = AUDIO_FORMAT_PCM;
    play_data.channel = AUDIO_CHANNEL_OUT_MONO;
    play_data.device = AUDIO_DEVICE_OUT_SPEAKER;
    play_data.mode = AUDIO_MODE_NORMAL;
    play_data.sample_rate = 16000;

    record_data.pcm_file_name = NULL;
    record_data.codec_file_name = NULL;
    record_data.stream_in = NULL;
    record_data.record_clock_started = 0;
    record_data.record_codec_bytes = 0;

    record_data.format = AUDIO_FORMAT_PCM;
    record_data.channel = AUDIO_CHANNEL_IN_MONO;
    record_data.device = AUDIO_DEVICE_NONE; /* AUTO: let HAL select DMIC/AMIC */
    record_data.mode = AUDIO_MODE_NORMAL;
    record_data.effect = AUDIO_EFFECT_NONE;
    record_data.sample_rate = 16000;

    command = NULL;
    int opt_index = 1;
    for (int i = 1; i < argc; i++) {
        if (audio_test_is_command(argv[i])) {
            command = argv[i];
            opt_index = i + 1;
            break;
        }
    }
    if (command == NULL) {
        print_help(argv[0]);
        return -1;
    }

    for (int i = opt_index; i < argc; i++)
    {
        if(strcmp(argv[i], "--file") == 0 && i+1 < argc)
        {
            play_data.file_name = record_data.pcm_file_name = record_data.codec_file_name = argv[++i];
        }
        else if(strcmp(argv[i], "--format") == 0 && i+1 < argc)
        {
            audio_format = argv[++i];
            play_data.format = record_data.format = get_codec_format(audio_format);
            if(play_data.format == -1)
            {
                log_error("set format error!");
                set_type_error_flag = true;
            }
        }
        else if(strcmp(argv[i], "--in_device") == 0 && i+1 < argc)
        {
            record_data.device = get_in_device(argv[++i]);
            if(record_data.device == -1)
            {
                log_error("set in device error!");
                set_type_error_flag = true;
            }
        }
        else if(strcmp(argv[i], "--out_device") == 0 && i+1 < argc)
        {
            play_data.device = get_out_device(argv[++i]);
            if(play_data.device == -1)
            {
                log_error("set out device error!");
                set_type_error_flag = true;
            }
        }
        else if(strcmp(argv[i], "--in_channel") == 0 && i+1 < argc)
        {
            record_data.channel = get_in_channel(argv[++i]);
            if(record_data.channel == -1)
            {
                log_error("set in channel error!");
                set_type_error_flag = true;
            }
        }
        else if(strcmp(argv[i], "--out_channel") == 0 && i+1 < argc)
        {
            play_data.channel = get_out_channel(argv[++i]);
            if(play_data.channel == -1)
            {
                log_error("set out channel error!");
                set_type_error_flag = true;
            }
        }
        else if((strcmp(argv[i], "--effect") == 0 || strcmp(argv[i], "--efect") == 0) && i+1 < argc)
        {
            record_data.effect = get_effect(argv[++i]);
            if(record_data.effect == -1)
            {
                log_error("set effect error!");
                set_type_error_flag = true;
            }
        }
        else if(strcmp(argv[i], "--mode") == 0 && i+1 < argc)
        {
            play_data.mode = record_data.mode = get_mode(argv[++i]);
            if(play_data.mode == -1)
            {
                log_error("set mode error!");
                set_type_error_flag = true;
            }
        }
        else if(strcmp(argv[i], "--sample_rate") == 0 && i+1 < argc)
        {
            play_data.sample_rate = record_data.sample_rate = get_sample_rate(argv[++i]);
            if(play_data.sample_rate == -1)
            {
                log_error("set sample rate error!");
                set_type_error_flag = true;
            }
        }
        else if((strcmp(argv[i], "--time") == 0 || strcmp(argv[i], "-time") == 0) && i+1 < argc)
        {
            run_seconds = atoi(argv[++i]);
            if(run_seconds <= 0)
            {
                log_error("set time error! --time must be > 0");
                set_type_error_flag = true;
            }
        }
        else if (argv[i][0] != '-')
        {
            if (record_data.pcm_file_name == NULL)
                audio_test_set_file_arg(argv[i], &audio_format);
            else {
                log_error("unexpected argument: %s", argv[i]);
                set_type_error_flag = true;
            }
        }
        else
        {
            log_error("unknown option: %s", argv[i]);
            set_type_error_flag = true;
        }
    }


    if(set_type_error_flag == true)
    {
        print_help(argv[0]);
        return 0;
    }

    audio_test_setup_paths(audio_format, &owned_play_path, &owned_pcm_path, &owned_codec_path);

    if (strcmp(command, "record") == 0) {
        int ret = audio_test_record();
        if (ret != 0) {
            if (record_data.pcm_file) fclose(record_data.pcm_file);
            if (record_data.codec_file) fclose(record_data.codec_file);
            log_error("Record start failed (check mic/device or hal_audio_config.ini)");
            audio_test_free_paths(owned_play_path, owned_pcm_path, owned_codec_path);
            return -1;
        }
        int time_limit = (run_seconds > 0) ? run_seconds : 2;
        struct timespec now;

        log_info("Recording to %s (pcm), %s (codec), %d seconds",
                 record_data.pcm_file_name, record_data.codec_file_name, time_limit);

        if (!record_data.record_clock_started)
            log_warn("Record clock not started (open_input_stream failed?)");

        while (record_data.record_clock_started) {
            if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
                break;
            if (audio_test_elapsed_sec(&record_data.record_t0, &now) >= (double)time_limit)
                break;
            usleep(10000);
        }

        if (record_data.stream_in != NULL) {
            record_data.audio_dev->close_input_stream(record_data.audio_dev,
                                                      record_data.stream_in);
            record_data.stream_in = NULL;
        }

        fflush(record_data.pcm_file);
        fflush(record_data.codec_file);
        log_info("Recorded codec bytes: %llu", (unsigned long long)record_data.record_codec_bytes);

        fclose(record_data.pcm_file);
        fclose(record_data.codec_file);
        record_data.pcm_file = NULL;
        record_data.codec_file = NULL;

        audio_hw_device_close(record_data.audio_dev);

    }
    else if (strcmp(command, "play") == 0) {
        audio_test_play( );
    } else if (strcmp(command, "loopback") == 0) {
        audio_test_loopback( play_data, record_data);
    } else {
        log_error("Unknown command: %s", command);
        audio_test_free_paths(owned_play_path, owned_pcm_path, owned_codec_path);
        return -1;
    }

    audio_test_free_paths(owned_play_path, owned_pcm_path, owned_codec_path);
    return 0;

}

#ifdef CONFIG_OS_RTOS
SHELL_CMD_EXPORT_ALIAS(hw_audio_command, hw_audio_cmd, audio program);
#endif

