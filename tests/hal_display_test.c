/*
 * Display HAL sample test
 * Uses GPU for hardware-accelerated drawing
 */
#if CONFIG_HW_DISPLAY

#include "display.h"
#include "hardware.h"
#include "gpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "soc_platform.h"
#include "log.h"

#define LOG_TAG "DISPLAY"

/* Color definitions (ABGR format for this platform) */
#define COLOR_RED       0xFF0000FF
#define COLOR_GREEN     0xFF00FF00
#define COLOR_BLUE      0xFFFF0000
#define COLOR_WHITE     0xFFFFFFFF
#define COLOR_BLACK     0xFF000000
#define COLOR_YELLOW    0xFF00FFFF
#define COLOR_CYAN      0xFFFFFF00
#define COLOR_MAGENTA   0xFFFF00FF

/* Context for display operations */
typedef struct {
    struct display_device *display;
    struct gpu_device *gpu;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    pixel_format_t format;
    display_buffer_t buffer;
} display_ctx_t;

static inline uint8_t abgr_get_r(uint32_t abgr) { return (uint8_t)(abgr & 0xFF); }
static inline uint8_t abgr_get_g(uint32_t abgr) { return (uint8_t)((abgr >> 8) & 0xFF); }
static inline uint8_t abgr_get_b(uint32_t abgr) { return (uint8_t)((abgr >> 16) & 0xFF); }

/* CPU fill color swizzle for display-path quirks (project selectable).
 * Default is 0 (none). Legacy macro is kept for backward compatibility.
 */
static inline void abgr_get_rgb_compat(uint32_t abgr, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t rr = abgr_get_r(abgr);
    uint8_t gg = abgr_get_g(abgr);
    uint8_t bb = abgr_get_b(abgr);

    /* Default if not specified by project config. */
#ifndef CONFIG_HW_DISPLAY_COLOR_SWIZZLE
#define CONFIG_HW_DISPLAY_COLOR_SWIZZLE 0
#endif

    /* Legacy: old projects used CONFIG_HW_DISPLAY_COLOR_SWIZZLE_GBR to apply BRG. */
#if defined(CONFIG_HW_DISPLAY_COLOR_SWIZZLE_GBR) && (CONFIG_HW_DISPLAY_COLOR_SWIZZLE == 0)
#undef CONFIG_HW_DISPLAY_COLOR_SWIZZLE
#define CONFIG_HW_DISPLAY_COLOR_SWIZZLE 3
#endif

#if (CONFIG_HW_DISPLAY_COLOR_SWIZZLE == 1)      /* BGR */
    uint8_t t = rr; rr = bb; bb = t;
#elif (CONFIG_HW_DISPLAY_COLOR_SWIZZLE == 2)    /* GBR */
    uint8_t t = rr; rr = gg; gg = bb; bb = t;
#elif (CONFIG_HW_DISPLAY_COLOR_SWIZZLE == 3)    /* BRG */
    uint8_t t = rr; rr = bb; bb = gg; gg = t;
#endif /* 0: none */

    *r = rr;
    *g = gg;
    *b = bb;
}

static uint16_t pack_rgb565_from_abgr(uint32_t abgr)
{
    uint8_t r, g, b;
    abgr_get_rgb_compat(abgr, &r, &g, &b);
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static uint16_t pack_bgr565_from_abgr(uint32_t abgr)
{
    uint8_t r, g, b;
    abgr_get_rgb_compat(abgr, &r, &g, &b);
    return (uint16_t)(((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3));
}

static int sw_fill_rect(display_ctx_t *ctx, const rect_t *rect, uint32_t abgr_color)
{
    if (!ctx || !rect || !ctx->buffer.vaddr || ctx->height == 0) {
        return -1;
    }

    uint32_t stride = ctx->buffer.size / ctx->height; /* bytes per line */
    uint8_t *base = (uint8_t *)ctx->buffer.vaddr;

    uint32_t x0 = (rect->x < 0) ? 0u : (uint32_t)rect->x;
    uint32_t y0 = (rect->y < 0) ? 0u : (uint32_t)rect->y;
    uint32_t x1 = (uint32_t)rect->x + rect->width;
    uint32_t y1 = (uint32_t)rect->y + rect->height;
    if (x1 > ctx->width) x1 = ctx->width;
    if (y1 > ctx->height) y1 = ctx->height;
    if (x0 >= x1 || y0 >= y1) {
        return 0;
    }

    if (ctx->format == SYSTEM_PIXEL_FORMAT_RGB565) {
        uint16_t px = pack_rgb565_from_abgr(abgr_color);
        for (uint32_t y = y0; y < y1; y++) {
            uint16_t *row = (uint16_t *)(base + y * stride);
            for (uint32_t x = x0; x < x1; x++) {
                row[x] = px;
            }
        }
        return 0;
    }

    if (ctx->format == SYSTEM_PIXEL_FORMAT_BGR565) {
        uint16_t px = pack_bgr565_from_abgr(abgr_color);
        for (uint32_t y = y0; y < y1; y++) {
            uint16_t *row = (uint16_t *)(base + y * stride);
            for (uint32_t x = x0; x < x1; x++) {
                row[x] = px;
            }
        }
        return 0;
    }

    /* Fallback: if format is unknown, attempt best-effort 16bpp fill */
    if (ctx->bpp == 2) {
        uint16_t px = pack_rgb565_from_abgr(abgr_color);
        for (uint32_t y = y0; y < y1; y++) {
            uint16_t *row = (uint16_t *)(base + y * stride);
            for (uint32_t x = x0; x < x1; x++) {
                row[x] = px;
            }
        }
        return 0;
    }

    return -1;
}

static void print_help(const char *prog)
{
    printf("\n");
    printf("Usage: %s <command> [options]\n", prog);
    printf("\n");
    printf("Commands:\n");
    printf("  info                          show display info\n");
    printf("  red                           fill with red\n");
    printf("  green                         fill with green\n");
    printf("  blue                          fill with blue\n");
    printf("  white                         fill with white\n");
    printf("  black                         fill with black\n");
    printf("  bars                          draw color bars\n");
    printf("  cycle                         cycle through colors\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s info\n", prog);
    printf("  %s red\n", prog);
    printf("\n");
}

static int display_init(display_ctx_t *ctx)
{
    const struct hw_module_t *disp_module = NULL;
    const struct hw_module_t *gpu_module = NULL;
    int ret;

    /* Initialize display module */
    ret = hw_display_module_init();
    if (ret != 0) {
        log_error("display module init failed");
        return -1;
    }

    /* Get and open display device */
    ret = hw_display_get_module(&disp_module, DISPLAY_DEVICE_INSTANCE_FB);
    if (ret != 0 || disp_module == NULL) {
        log_error("get display module failed");
        return -1;
    }

    hw_device_t *hw_device = NULL;
    ret = disp_module->methods->open(disp_module, DISPLAY_DEVICE_PATH_FB, &hw_device);
    if (ret != 0 || hw_device == NULL) {
        log_error("open display device failed");
        return -1;
    }
    ctx->display = (struct display_device *)hw_device;

    /* Get display info */
    ctx->display->get_resolution(ctx->display, &ctx->width, &ctx->height);
    ctx->display->get_bpp(ctx->display, &ctx->bpp);
    ctx->display->get_pixel_format(ctx->display, &ctx->format);
    ctx->display->get_offline_buffer(ctx->display, &ctx->buffer);

    /* Get and open GPU device */
    ret = hw_get_module(GPU_HARDWARE_MODULE_ID, &gpu_module);
    if (ret != 0 || gpu_module == NULL) {
        log_warn("get GPU module failed; will use CPU fill");
        ctx->gpu = NULL;
        return 0;
    }

    ret = gpu_module->methods->open(gpu_module, GPU_DEVICE_NAME, (hw_device_t **)&ctx->gpu);
    if (ret != 0 || ctx->gpu == NULL) {
        log_warn("open GPU device failed; will use CPU fill");
        ctx->gpu = NULL;
        return 0;
    }

    return 0;
}

static void display_deinit(display_ctx_t *ctx)
{
    if (ctx->gpu) {
        ctx->gpu->common.close(&ctx->gpu->common);
    }
    if (ctx->display) {
        ctx->display->common.close(&ctx->display->common);
    }
}

static int display_fill(display_ctx_t *ctx, uint32_t color)
{
    image_desc_t desc = {0};
    desc.u32Width = ctx->width;
    desc.u32Height = ctx->height;
    desc.vaddr = (uint8_t *)ctx->buffer.vaddr;
    desc.paddr = ctx->buffer.paddr;
    desc.format = ctx->format;
    desc.rect.x = 0;
    desc.rect.y = 0;
    desc.rect.width = ctx->width;
    desc.rect.height = ctx->height;
    desc.u8GlobalOpacity = 255;

    int ret = -1;
    if (ctx->gpu && ctx->gpu->image_fill) {
        ret = ctx->gpu->image_fill(ctx->gpu, &desc, color);
    }
    if (ret != 0) {
        /* GPU may be unavailable on some SoCs; fall back to CPU fill. */
        if (sw_fill_rect(ctx, &desc.rect, color) != 0) {
            log_error("image_fill failed (ret=%d) and sw fill failed", ret);
            return -1;
        }
        log_warn("GPU image_fill failed (ret=%d); used CPU fill", ret);
    }

    ctx->display->flush(ctx->display);
    return 0;
}

static int display_info(display_ctx_t *ctx)
{
    log_info("=== Display Info ===");
    log_info("Resolution: %ux%u", ctx->width, ctx->height);
    log_info("BPP: %u bytes (%u bits)", ctx->bpp, ctx->bpp * 8);
    log_info("Format: 0x%x", ctx->format);
    log_info("Buffer: vaddr=%p, paddr=0x%llx, size=%u",
             ctx->buffer.vaddr, (unsigned long long)ctx->buffer.paddr, ctx->buffer.size);
    return 0;
}

static int display_bars(display_ctx_t *ctx)
{
    /* Draw 8 color bars */
    uint32_t colors[] = {
        COLOR_WHITE, COLOR_YELLOW, COLOR_CYAN, COLOR_GREEN,
        COLOR_MAGENTA, COLOR_RED, COLOR_BLUE, COLOR_BLACK
    };
    
    uint32_t bar_width = ctx->width / 8;
    
    for (int i = 0; i < 8; i++) {
        image_desc_t desc = {0};
        desc.u32Width = ctx->width;
        desc.u32Height = ctx->height;
        desc.vaddr = (uint8_t *)ctx->buffer.vaddr;
        desc.paddr = ctx->buffer.paddr;
        desc.format = ctx->format;
        desc.rect.x = i * bar_width;
        desc.rect.y = 0;
        desc.rect.width = (i == 7) ? (ctx->width - desc.rect.x) : bar_width;
        desc.rect.height = ctx->height;
        desc.u8GlobalOpacity = 255;

        ctx->gpu->image_fill(ctx->gpu, &desc, colors[i]);
    }

    ctx->display->flush(ctx->display);
    log_info("Color bars displayed");
    return 0;
}

static int display_cycle(display_ctx_t *ctx)
{
    uint32_t colors[] = {
        COLOR_RED, COLOR_GREEN, COLOR_BLUE,
        COLOR_YELLOW, COLOR_CYAN, COLOR_MAGENTA,
        COLOR_WHITE, COLOR_BLACK
    };
    const char *names[] = {
        "RED", "GREEN", "BLUE",
        "YELLOW", "CYAN", "MAGENTA",
        "WHITE", "BLACK"
    };
    
    log_info("Cycling through colors (press Ctrl+C to stop)...");
    for (int i = 0; i < 8; i++) {
        log_info("  %s", names[i]);
        display_fill(ctx, colors[i]);
        sleep(1);
    }
    return 0;
}

#if (defined OS_NAME_LINUX)
int main(int argc, char **argv)
#else
static int hw_display_cmd(int argc, char **argv)
#endif
{
    display_ctx_t ctx = {0};
    const char *cmd = NULL;
    int ret = 0;

    soc_platform_init(0);
    log_set_level(LOG_INFO);
    hw_module_init();

    /* Parse arguments */
    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_help(argv[0]);
        return (argc < 2) ? -1 : 0;
    }
    cmd = argv[1];

    /* Initialize display and GPU */
    if (display_init(&ctx) != 0) {
        return -1;
    }

    /* Execute command */
    if (strcmp(cmd, "info") == 0) {
        ret = display_info(&ctx);
    } else if (strcmp(cmd, "red") == 0) {
        ret = display_fill(&ctx, COLOR_RED);
    } else if (strcmp(cmd, "green") == 0) {
        ret = display_fill(&ctx, COLOR_GREEN);
    } else if (strcmp(cmd, "blue") == 0) {
        ret = display_fill(&ctx, COLOR_BLUE);
    } else if (strcmp(cmd, "white") == 0) {
        ret = display_fill(&ctx, COLOR_WHITE);
    } else if (strcmp(cmd, "black") == 0) {
        ret = display_fill(&ctx, COLOR_BLACK);
    } else if (strcmp(cmd, "bars") == 0) {
        ret = display_bars(&ctx);
    } else if (strcmp(cmd, "cycle") == 0) {
        ret = display_cycle(&ctx);
    } else {
        log_error("Unknown command: %s", cmd);
        print_help(argv[0]);
        ret = -1;
    }

    display_deinit(&ctx);
    return ret;
}

#if ((defined OS_NAME_AOS) || (defined OS_NAME_RTT))
SHELL_CMD_EXPORT_ALIAS(hw_display_cmd, hw_display_cmd, display program);
#endif

#endif
