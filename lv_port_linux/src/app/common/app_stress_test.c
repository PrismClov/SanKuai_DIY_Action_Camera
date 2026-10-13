/**
 * @file app_stress_test.c
 * @brief UI 稳定性压力测试实现
 *
 * 测试序列（每步间隔 2 秒，一个循环约 16 秒）：
 *   0. 打开模式弹层
 *   1. 关闭模式弹层
 *   2. 打开 WiFi 弹层
 *   3. 关闭 WiFi 弹层
 *   4. 旋转屏幕（横/竖切换）
 *   5. 打开模式弹层（新方向）
 *   6. 关闭模式弹层
 *   7. 旋转屏幕（切回）
 * 默认跑 50 个循环后自动停止，可通过 APP_STRESS_LOOPS 环境变量调整。
 */
#include "app_stress_test.h"
#include "../app_main.h"
#include "../pages/page_home.h"
#include "app_i18n.h"

#include "lvgl/lvgl.h"
#include <stdlib.h>
#include <stdio.h>

#ifndef APP_WIFI_DISABLED
#include "../wifi/app_wifi.h"
#endif

typedef enum {
    STRESS_OPEN_MODE,
    STRESS_CLOSE_MODE,
    STRESS_OPEN_WIFI,
    STRESS_CLOSE_WIFI,
    STRESS_ROTATE_1,
    STRESS_OPEN_MODE_2,
    STRESS_CLOSE_MODE_2,
    STRESS_ROTATE_2,
    STRESS_STEP_COUNT
} stress_step_t;

static lv_timer_t *s_stress_timer = NULL;
static stress_step_t s_step = STRESS_OPEN_MODE;
static int s_loop = 0;
static int s_max_loops = 50;

bool app_stress_test_enabled(void)
{
    const char *env = getenv("APP_STRESS_TEST");
    return env && (*env == '1' || *env == 'y' || *env == 'Y');
}

static void stress_next_step(void)
{
    s_step = (stress_step_t)((s_step + 1) % STRESS_STEP_COUNT);
    if (s_step == STRESS_OPEN_MODE) {
        s_loop++;
        LV_LOG_USER("STRESS loop %d/%d completed", s_loop, s_max_loops);
    }
}

static void stress_timer_cb(lv_timer_t *t)
{
    (void)t;

    /* UI 正在重建（旋转/语言切换）时跳过本周期，避免在旧 UI 已销毁、
     * 新 UI 尚未创建的空窗期访问悬空句柄。 */
    if (app_ui_is_recreating()) {
        LV_LOG_USER("STRESS: UI recreating, skip this step");
        return;
    }

    if (s_loop >= s_max_loops) {
        LV_LOG_USER("STRESS test finished: %d loops", s_max_loops);
        app_stress_test_stop();
        return;
    }

    switch (s_step) {
    case STRESS_OPEN_MODE:
        LV_LOG_USER("STRESS step: open mode overlay");
        page_home_stress_mode_overlay(1);
        break;
    case STRESS_CLOSE_MODE:
        LV_LOG_USER("STRESS step: close mode overlay");
        page_home_stress_mode_overlay(0);
        break;
    case STRESS_OPEN_WIFI:
        LV_LOG_USER("STRESS step: open WiFi overlay");
#ifndef APP_WIFI_DISABLED
        app_wifi_open();
#endif
        break;
    case STRESS_CLOSE_WIFI:
        LV_LOG_USER("STRESS step: close WiFi overlay");
#ifndef APP_WIFI_DISABLED
        app_wifi_close();
#endif
        break;
    case STRESS_ROTATE_1:
        LV_LOG_USER("STRESS step: rotate screen (1)");
        app_ui_recreate_async();
        break;
    case STRESS_OPEN_MODE_2:
        LV_LOG_USER("STRESS step: open mode overlay after rotation");
        page_home_stress_mode_overlay(1);
        break;
    case STRESS_CLOSE_MODE_2:
        LV_LOG_USER("STRESS step: close mode overlay after rotation");
        page_home_stress_mode_overlay(0);
        break;
    case STRESS_ROTATE_2:
        LV_LOG_USER("STRESS step: rotate screen (2)");
        app_ui_recreate_async();
        break;
    default:
        break;
    }

    stress_next_step();
}

void app_stress_test_start(void)
{
    if (s_stress_timer) return;

    const char *loops_env = getenv("APP_STRESS_LOOPS");
    if (loops_env) {
        int n = atoi(loops_env);
        if (n > 0) s_max_loops = n;
    }

    s_step = STRESS_OPEN_MODE;
    s_loop = 0;
    LV_LOG_USER("STRESS test start: %d loops, step interval 2000ms", s_max_loops);
    /* 初始延迟 2 秒，让首页完全渲染后再开始 */
    s_stress_timer = lv_timer_create(stress_timer_cb, 2000, NULL);
    /* LVGL 定时器 repeat_count = -1 表示无限循环 */
    lv_timer_set_repeat_count(s_stress_timer, -1);
    lv_timer_ready(s_stress_timer);
}

void app_stress_test_stop(void)
{
    if (!s_stress_timer) return;
    lv_timer_delete(s_stress_timer);
    s_stress_timer = NULL;
    LV_LOG_USER("STRESS test stopped");
}
