/**
 * @file page_aod.c
 * @brief 熄屏显示页面（Always-On Display / 屏幕锁定）
 *
 * 布局：全屏黑色背景，内容垂直居中。
 *   样式一：
 *     第一行（录制时红色）：已录制时间 / 可用存储容量
 *     第二行：拍摄模式图标 + 当前参数
 *   样式二：
 *     第一行（录制时红色）：已录制时间 / 系统时间
 *     第二行：拍摄模式图标 + 当前参数
 * 交互：从底部向上滑动（LV_DIR_TOP）返回主屏幕。
 */
#include "page_aod.h"
#include "page_home.h"
#include "../settings/settings_storage.h"
#include "../app_main.h"
#include "../common/ui_helpers.h"
#include "../common/app_i18n.h"

#include "lvgl/lvgl.h"

#include <stdio.h>
#include <time.h>

static lv_obj_t *g_aod_overlay;
static lv_obj_t *g_line1;   /* 录制时间 / 存储容量 / 系统时间 */
static lv_obj_t *g_line2;   /* 模式图标 + 参数 */

static int s_aod_style = AOD_STYLE_STORAGE;
static lv_timer_t *s_refresh_timer = NULL;

/* ------------------------------------------------------------------ */
/* 数据刷新                                                            */
/* ------------------------------------------------------------------ */
static long long get_free_mb_selected(void)
{
    const char *path = settings_storage_get_path();
    long long total = 0, used = 0;
    int ok = 0;
    ui_read_storage(path, &total, &used, &ok);
    if (!ok || total <= used) return 0;
    return total - used;
}

static void fmt_free_size(char *buf, size_t sz, long long free_mb)
{
    if (free_mb >= 1024 * 1024) {
        snprintf(buf, sz, "%.2fTB", free_mb / 1024.0 / 1024.0);
    } else if (free_mb >= 1024) {
        snprintf(buf, sz, "%.1fGB", free_mb / 1024.0);
    } else {
        snprintf(buf, sz, "%.0fMB", (double)free_mb);
    }
}

static void aod_refresh_time(void)
{
    if (!g_line1) return;

    /* 录制中时第一行红色显示已录制时间（两种样式都优先显示录制时间） */
    if (page_home_is_recording()) {
        char rec[16];
        page_home_get_rec_time(rec, sizeof(rec));
        lv_obj_set_style_text_color(g_line1, lv_color_hex(0xFF0000), 0);
        lv_label_set_text_fmt(g_line1, "%s %s", LV_SYMBOL_VIDEO, rec);
        return;
    }

    lv_obj_set_style_text_color(g_line1, lv_color_hex(COLOR_TEXT), 0);

    if (s_aod_style == AOD_STYLE_STORAGE) {
        char free_str[32];
        fmt_free_size(free_str, sizeof(free_str), get_free_mb_selected());
        lv_label_set_text_fmt(g_line1, "%s: %s", tr("可用存储", "Free"), free_str);
    } else {
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        char t[16];
        snprintf(t, sizeof(t), "%02d:%02d", tm ? tm->tm_hour : 0, tm ? tm->tm_min : 0);
        lv_label_set_text(g_line1, t);
    }
}

static void aod_refresh_param(void)
{
    if (!g_line2) return;
    char txt[64];
    page_home_get_mode_param_text(txt, sizeof(txt));
    lv_label_set_text(g_line2, txt);
}

static void aod_refresh_cb(lv_timer_t *t)
{
    (void)t;
    if (app_ui_is_recreating()) return;
    if (!g_aod_overlay || lv_obj_has_flag(g_aod_overlay, LV_OBJ_FLAG_HIDDEN)) return;
    aod_refresh_time();
    aod_refresh_param();
}

/* ------------------------------------------------------------------ */
/* 交互回调                                                            */
/* ------------------------------------------------------------------ */
static void aod_return_home_cb(lv_event_t *e)
{
    (void)e;
    page_aod_close();
}

/* ------------------------------------------------------------------ */
/* UI 创建                                                            */
/* ------------------------------------------------------------------ */
void page_aod_create(void)
{
    if (g_aod_overlay) return;

    g_aod_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_aod_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(g_aod_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_aod_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(g_aod_overlay, 0, 0);
    lv_obj_set_style_border_width(g_aod_overlay, 0, 0);
    lv_obj_remove_flag(g_aod_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_aod_overlay, LV_OBJ_FLAG_HIDDEN);

    /* 底部上滑返回主屏（手指从下往上滑，方向为 LV_DIR_TOP） */
    ui_attach_gesture(g_aod_overlay, LV_DIR_TOP, aod_return_home_cb);

    const bool landscape = ui_is_landscape();

    /* 内容容器：垂直居中 */
    lv_obj_t *box = lv_obj_create(g_aod_overlay);
    lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(box, landscape ? ui_scale_min(12) : ui_scale_min(20), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    /* 第一行使用 CJK 大号字体，避免中文“可用存储”乱码 */
    g_line1 = lv_label_create(box);
    lv_obj_set_style_text_font(g_line1, ui_font_cjk_lg(), 0);
    lv_obj_set_style_text_color(g_line1, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_width(g_line1, lv_pct(100));
    lv_obj_set_style_text_align(g_line1, LV_TEXT_ALIGN_CENTER, 0);

    g_line2 = lv_label_create(box);
    lv_obj_set_style_text_font(g_line2, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(g_line2, lv_color_hex(COLOR_SECONDARY), 0);
    lv_obj_set_width(g_line2, lv_pct(100));
    lv_obj_set_style_text_align(g_line2, LV_TEXT_ALIGN_CENTER, 0);

    /* 底部提示：上滑解锁 */
    lv_obj_t *hint = lv_label_create(g_aod_overlay);
    lv_label_set_text(hint, tr("上滑解锁", "Swipe up to unlock"));
    lv_obj_set_style_text_font(hint, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x666666), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -ui_scale_min(40));

    /* 每秒刷新一次时间/录制时间 */
    if (!s_refresh_timer) {
        s_refresh_timer = lv_timer_create(aod_refresh_cb, 1000, NULL);
    }

    aod_refresh_time();
    aod_refresh_param();
}

void page_aod_open(void)
{
    if (!g_aod_overlay) page_aod_create();
    if (!g_aod_overlay) return;

    aod_refresh_time();
    aod_refresh_param();
    lv_obj_remove_flag(g_aod_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_aod_overlay);
}

void page_aod_close(void)
{
    if (g_aod_overlay) {
        lv_obj_add_flag(g_aod_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    app_return_home();
}

void page_aod_destroy(void)
{
    if (s_refresh_timer) {
        lv_timer_delete(s_refresh_timer);
        s_refresh_timer = NULL;
    }
    if (g_aod_overlay) {
        lv_obj_delete(g_aod_overlay);
        g_aod_overlay = NULL;
    }
    g_line1 = NULL;
    g_line2 = NULL;
}

void page_aod_set_style(int style)
{
    s_aod_style = (style == AOD_STYLE_TIME) ? AOD_STYLE_TIME : AOD_STYLE_STORAGE;
    aod_refresh_time();
}

int page_aod_get_style(void)
{
    return s_aod_style;
}
