/*******************************************************************
 *
 * main.c - LVGL simulator for GNU/Linux
 *
 * Based on the original file from the repository
 *
 * @note eventually this file won't contain a main function and will
 * become a library supporting all major operating systems
 *
 * To see how each driver is initialized check the
 * 'src/lib/display_backends' directory
 *
 * - Clean up
 * - Support for multiple backends at once
 *   2025 EDGEMTech Ltd.
 *
 * Author: EDGEMTech Ltd, Erik Tagirov (erik.tagirov@edgemtech.ch)
 *
 ******************************************************************/
#include <unistd.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"
#include "lvgl/demos/lv_demos.h"
#if LV_USE_FS_STDIO
    #include "lvgl/src/libs/fsdrv/lv_fsdrv.h"
#endif

#include "src/lib/driver_backends.h"
#include "src/lib/simulator_util.h"
#include "src/lib/simulator_settings.h"
#include "src/app/app_main.h"
#include "src/app/common/app_stress_test.h"

/* Internal functions */
static void configure_simulator(int argc, char **argv);
static void print_lvgl_version(void);
static void print_usage(void);

/* contains the name of the selected backend if user
 * has specified one on the command line */
static char *selected_backend;

/* Global simulator settings, defined in lv_linux_backend.c */
extern simulator_settings_t settings;


/**
 * @brief Print LVGL version
 */
static void print_lvgl_version(void)
{
    fprintf(stdout, "%d.%d.%d-%s\n",
            LVGL_VERSION_MAJOR,
            LVGL_VERSION_MINOR,
            LVGL_VERSION_PATCH,
            LVGL_VERSION_INFO);
}

/**
 * @brief Print usage information
 */
static void print_usage(void)
{
    fprintf(stdout, "\nlvglsim [-V] [-B] [-b backend_name] [-W window_width] [-H window_height]\n\n");
    fprintf(stdout, "-V print LVGL version\n");
    fprintf(stdout, "-B list supported backends\n");
}

/**
 * @brief Configure simulator
 * @description process arguments recieved by the program to select
 * appropriate options
 * @param argc the count of arguments in argv
 * @param argv The arguments
 */
static void configure_simulator(int argc, char **argv)
{
    int opt = 0;

    selected_backend = NULL;
    driver_backends_register();

    const char *env_w = getenv("LV_SIM_WINDOW_WIDTH");
    const char *env_h = getenv("LV_SIM_WINDOW_HEIGHT");
    /* Default values */
    settings.window_width = atoi(env_w ? env_w : "800");
    settings.window_height = atoi(env_h ? env_h : "480");

    /* Parse the command-line options. */
    while ((opt = getopt (argc, argv, "b:fmW:H:BVh")) != -1) {
        switch (opt) {
        case 'h':
            print_usage();
            exit(EXIT_SUCCESS);
            break;
        case 'V':
            print_lvgl_version();
            exit(EXIT_SUCCESS);
            break;
        case 'B':
            driver_backends_print_supported();
            exit(EXIT_SUCCESS);
            break;
        case 'b':
            if (driver_backends_is_supported(optarg) == 0) {
                die("error no such backend: %s\n", optarg);
            }
            selected_backend = strdup(optarg);
            break;
        case 'W':
            settings.window_width = atoi(optarg);
            break;
        case 'H':
            settings.window_height = atoi(optarg);
            break;
        case ':':
            print_usage();
            die("Option -%c requires an argument.\n", optopt);
            break;
        case '?':
            print_usage();
            die("Unknown option -%c.\n", optopt);
        }
    }
}

/**
 * @brief entry point
 * @description start a demo
 * @param argc the count of arguments in argv
 * @param argv The arguments
 */
int main(int argc, char **argv)
{

    configure_simulator(argc, argv);

    // #region debug-point A:init
    dbg_log("A", "main.c:main", "start");
    // #endregion

    /* Initialize LVGL. */
    lv_init();
    // #region debug-point A:init
    dbg_log("A", "main.c:main", "lv_init done");
    // #endregion

    /* 注册 stdio 文件系统驱动（供相册图片解码使用）。
     * 必须在 app_ui_create / 相册页创建之前完成，否则 LVGL 无法通过 "A:/path" 路径加载图片。 */
#if LV_USE_FS_STDIO
    lv_fs_stdio_init();
    LV_LOG_USER("stdio FS driver initialized (letter='%c')", LV_FS_STDIO_LETTER);
#endif

    /* Initialize the configured backend */
    if (driver_backends_init_backend(selected_backend) == -1) {
        die("Failed to initialize display backend");
    }
    // #region debug-point A:init
    dbg_log("A", "main.c:main", "backend init done");
    // #endregion

    /* Enable for EVDEV support */
#if LV_USE_EVDEV
    if (driver_backends_init_backend("EVDEV") == -1) {
        die("Failed to initialize evdev");
    }
#endif

    /* 恢复持久化的横竖屏状态（仅由按钮触发旋转，不会自动恢复默认竖屏） */
    app_rotation_load();
    // #region debug-point A:init
    dbg_log("A", "main.c:main", "rotation loaded");
    // #endregion

    lv_display_t *disp = lv_display_get_default();
    if (disp) {
#if LV_USE_LINUX_DRM
        /* DRM 后端：直接设置 rotation，LVGL 按逻辑分辨率渲染，drm_flush 旋转复制 */
        lv_display_set_rotation(disp, g_app_landscape ? LV_DISPLAY_ROTATION_90 : LV_DISPLAY_ROTATION_0);
#else
        /* SDL 后端：根据保存的方向设置窗口分辨率（横屏 800x480 / 竖屏 480x800） */
        lv_display_set_resolution(disp,
                                  g_app_landscape ? 800 : 480,
                                  g_app_landscape ? 480 : 800);
#endif
    }
    // #region debug-point A:init
    dbg_log("A", "main.c:main", "display configured");
    // #endregion

    /*Create the main application UI*/
    app_ui_create();
    // #region debug-point A:init
    dbg_log("A", "main.c:main", "app_ui_create done");
    // #endregion

    /* 如启用压力测试，自动循环触发旋转/弹层/WiFi 等高频操作 */
    if (app_stress_test_enabled()) {
        app_stress_test_start();
    }

#if 1 /* 自动点击冒烟测试：dummy driver 下模拟用户点击，排查崩溃 */
    {
        const char *auto_click = getenv("APP_AUTO_CLICK");
        if (auto_click) {
            extern void app_auto_click_schedule(const char *target);
            app_auto_click_schedule(auto_click);
        }
    }
#endif

    /* Enter the run loop of the selected backend */
    driver_backends_run_loop();

    return 0;
}
