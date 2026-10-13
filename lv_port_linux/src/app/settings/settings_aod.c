/**
 * @file settings_aod.c
 * @brief 熄屏显示设置二级菜单
 *
 * 功能：
 *   - 在系统设置主页作为入口卡片
 *   - 二级菜单中选择熄屏显示样式：
 *     · 样式一：显示可用存储容量 + 拍摄参数
 *     · 样式二：显示系统时间 + 拍摄参数
 *   - 选中状态持久化到 aod.cfg
 */
#include "settings_aod.h"
#include "settings_storage.h"
#include "../pages/page_aod.h"
#include "../pages/page_home.h"
#include "../app_main.h"
#include "../common/ui_helpers.h"
#include "../common/app_i18n.h"

#include "lvgl/lvgl.h"

#include <stdio.h>
#include <time.h>

static lv_obj_t *g_overlay;
static lv_obj_t *g_btn_storage;
static lv_obj_t *g_btn_time;
static lv_obj_t *g_preview_line1;
static lv_obj_t *g_preview_line2;
static lv_timer_t *g_preview_timer = NULL;

#define AOD_CFG_FILE "aod.cfg"

static int s_aod_style = AOD_STYLE_STORAGE;

/* ------------------------------------------------------------------ */
/* 持久化                                                              */
/* ------------------------------------------------------------------ */
static void aod_cfg_save(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", app_get_app_dir(), AOD_CFG_FILE);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%d\n", s_aod_style);
        fclose(f);
        LV_LOG_USER("AOD style saved: %d", s_aod_style);
    } else {
        LV_LOG_WARN("Failed to save AOD cfg to %s", path);
    }
}

static void aod_cfg_load(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", app_get_app_dir(), AOD_CFG_FILE);
    FILE *f = fopen(path, "r");
    if (f) {
        int val = 0;
        if (fscanf(f, "%d", &val) == 1) {
            s_aod_style = (val == AOD_STYLE_TIME) ? AOD_STYLE_TIME : AOD_STYLE_STORAGE;
            LV_LOG_USER("AOD style loaded: %d", s_aod_style);
        }
        fclose(f);
    }
}

/* ------------------------------------------------------------------ */
/* 预览数据                                                            */
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

static void aod_preview_refresh(void)
{
    if (!g_preview_line1 || !g_preview_line2) return;

    /* 录制中时优先红色显示已录制时间 */
    if (page_home_is_recording()) {
        char rec[16];
        page_home_get_rec_time(rec, sizeof(rec));
        lv_obj_set_style_text_color(g_preview_line1, lv_color_hex(0xFF0000), 0);
        lv_label_set_text_fmt(g_preview_line1, "%s %s", LV_SYMBOL_VIDEO, rec);
    } else if (s_aod_style == AOD_STYLE_STORAGE) {
        char free_str[32];
        fmt_free_size(free_str, sizeof(free_str), get_free_mb_selected());
        lv_obj_set_style_text_color(g_preview_line1, lv_color_hex(COLOR_TEXT), 0);
        lv_label_set_text_fmt(g_preview_line1, "%s: %s", tr("可用存储", "Free"), free_str);
    } else {
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        char t[16];
        snprintf(t, sizeof(t), "%02d:%02d", tm ? tm->tm_hour : 0, tm ? tm->tm_min : 0);
        lv_obj_set_style_text_color(g_preview_line1, lv_color_hex(COLOR_TEXT), 0);
        lv_label_set_text(g_preview_line1, t);
    }

    char txt[64];
    page_home_get_mode_param_text(txt, sizeof(txt));
    lv_label_set_text(g_preview_line2, txt);
}

static void aod_preview_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (app_ui_is_recreating()) return;
    if (!g_overlay || lv_obj_has_flag(g_overlay, LV_OBJ_FLAG_HIDDEN)) return;
    aod_preview_refresh();
}

/* ------------------------------------------------------------------ */
/* UI 刷新                                                             */
/* ------------------------------------------------------------------ */
static void aod_update_selection_ui(void)
{
    if (!g_btn_storage || !g_btn_time) return;

    lv_obj_set_style_border_color(g_btn_storage,
        (s_aod_style == AOD_STYLE_STORAGE) ? lv_color_hex(COLOR_ACCENT) : lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_color(g_btn_storage,
        (s_aod_style == AOD_STYLE_STORAGE) ? lv_color_hex(0x2A2210) : lv_color_hex(COLOR_PANEL), 0);

    lv_obj_set_style_border_color(g_btn_time,
        (s_aod_style == AOD_STYLE_TIME) ? lv_color_hex(COLOR_ACCENT) : lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_color(g_btn_time,
        (s_aod_style == AOD_STYLE_TIME) ? lv_color_hex(0x2A2210) : lv_color_hex(COLOR_PANEL), 0);
}

/* ------------------------------------------------------------------ */
/* 回调                                                                */
/* ------------------------------------------------------------------ */
static void close_cb(lv_event_t *e)
{
    (void)e;
    if (g_overlay) lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void return_home_cb(lv_event_t *e)
{
    (void)e;
    if (g_overlay) lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
    app_return_home();
}

static void on_sel_storage_style(lv_event_t *e)
{
    (void)e;
    s_aod_style = AOD_STYLE_STORAGE;
    page_aod_set_style(s_aod_style);
    aod_cfg_save();
    aod_update_selection_ui();
    aod_preview_refresh();
}

static void on_sel_time_style(lv_event_t *e)
{
    (void)e;
    s_aod_style = AOD_STYLE_TIME;
    page_aod_set_style(s_aod_style);
    aod_cfg_save();
    aod_update_selection_ui();
    aod_preview_refresh();
}

/* ------------------------------------------------------------------ */
/* 通用选项行                                                          */
/* ------------------------------------------------------------------ */
static lv_obj_t *make_style_option(lv_obj_t *parent, const char *title,
                                   const char *desc, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, lv_pct(100), ui_scale_min(72));
    lv_obj_set_style_bg_color(btn, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_radius(btn, ui_scale_min(12), 0);
    lv_obj_set_style_border_width(btn, ui_scale_min(3), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x333333), 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(btn, ui_scale_min(16), 0);
    lv_obj_set_style_pad_right(btn, ui_scale_min(16), 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    ui_attach_press_scale(btn, 242);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *t = lv_label_create(btn);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(t, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_width(t, lv_pct(100));

    lv_obj_t *d = lv_label_create(btn);
    lv_label_set_text(d, desc);
    lv_obj_set_style_text_font(d, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(d, lv_color_hex(COLOR_SECONDARY), 0);
    lv_obj_set_width(d, lv_pct(100));

    return btn;
}

/* ------------------------------------------------------------------ */
/* 页面创建                                                            */
/* ------------------------------------------------------------------ */
void settings_aod_create(void)
{
    aod_cfg_load();
    page_aod_set_style(s_aod_style);

    g_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(g_overlay, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(g_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(g_overlay, 0, 0);
    lv_obj_set_style_border_width(g_overlay, 0, 0);
    lv_obj_remove_flag(g_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_flex_flow(g_overlay, LV_FLEX_FLOW_COLUMN);
    /* 交叉轴居中：宽屏下 body 限宽后水平居中 */
    lv_obj_set_flex_align(g_overlay,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    ui_make_header(g_overlay, tr("熄屏显示", "Always-On Display"),
                   close_cb, NULL, return_home_cb, NULL);

    ui_attach_back_gesture(g_overlay, close_cb);

    const bool landscape = ui_is_landscape();

    lv_obj_t *body = lv_obj_create(g_overlay);
    /* 宽屏限宽 720px 居中，窄屏全宽 */
    lv_obj_set_width(body, ui_get_content_max_width());
    lv_obj_set_flex_grow(body, 1);
    /* 横屏：选项在左，预览在右；竖屏：选项在上，预览在下 */
    lv_obj_set_flex_flow(body, landscape ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(body, ui_scale_min(16), 0);
    lv_obj_set_style_pad_gap(body, ui_scale_min(12), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    /* 选项容器 */
    lv_obj_t *opts = lv_obj_create(body);
    lv_obj_set_width(opts, landscape ? lv_pct(48) : lv_pct(100));
    if (landscape) {
        lv_obj_set_height(opts, lv_pct(100));
    } else {
        /* 竖屏：高度按内容自适应，避免被 flex_grow 的 preview 挤压导致按钮重叠 */
        lv_obj_set_height(opts, LV_SIZE_CONTENT);
    }
    lv_obj_set_flex_flow(opts, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(opts, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(opts, ui_scale_min(12), 0);
    lv_obj_set_style_bg_opa(opts, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(opts, 0, 0);
    lv_obj_remove_flag(opts, LV_OBJ_FLAG_SCROLLABLE);

    g_btn_storage = make_style_option(opts,
        tr("样式一", "Style 1"),
        tr("可用存储容量 + 拍摄参数", "Free storage & shooting params"),
        on_sel_storage_style);

    g_btn_time = make_style_option(opts,
        tr("样式二", "Style 2"),
        tr("系统时间 + 拍摄参数", "System time & shooting params"),
        on_sel_time_style);

    /* 实时预览区域：黑色背景模拟熄屏效果 */
    lv_obj_t *preview = lv_obj_create(body);
    if (landscape) {
        lv_obj_set_size(preview, lv_pct(48), lv_pct(100));
    } else {
        lv_obj_set_width(preview, lv_pct(100));
        lv_obj_set_flex_grow(preview, 1);
    }
    lv_obj_set_style_bg_color(preview, lv_color_hex(0x000000), 0);
    lv_obj_set_style_radius(preview, ui_scale_min(16), 0);
    lv_obj_set_style_border_width(preview, ui_scale_min(2), 0);
    lv_obj_set_style_border_color(preview, lv_color_hex(0x333333), 0);
    lv_obj_set_flex_flow(preview, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(preview, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(preview, ui_scale_min(16), 0);
    lv_obj_remove_flag(preview, LV_OBJ_FLAG_SCROLLABLE);

    /* 预览标题 */
    lv_obj_t *preview_title = lv_label_create(preview);
    lv_label_set_text(preview_title, tr("预览", "Preview"));
    lv_obj_set_style_text_font(preview_title, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(preview_title, lv_color_hex(0x666666), 0);

    /* 预览内容容器：用 flex_grow 填充标题以下剩余空间，
     * 不能用 height=100% 否则会占满整个 preview 遮挡标题 */
    lv_obj_t *preview_box = lv_obj_create(preview);
    lv_obj_set_width(preview_box, lv_pct(100));
    lv_obj_set_flex_grow(preview_box, 1);
    lv_obj_set_flex_flow(preview_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(preview_box, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(preview_box, ui_scale_min(12), 0);
    lv_obj_set_style_bg_opa(preview_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(preview_box, 0, 0);
    lv_obj_remove_flag(preview_box, LV_OBJ_FLAG_SCROLLABLE);

    g_preview_line1 = lv_label_create(preview_box);
    lv_obj_set_style_text_font(g_preview_line1, ui_font_cjk_lg(), 0);
    lv_obj_set_style_text_color(g_preview_line1, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_width(g_preview_line1, lv_pct(100));
    lv_obj_set_style_text_align(g_preview_line1, LV_TEXT_ALIGN_CENTER, 0);

    g_preview_line2 = lv_label_create(preview_box);
    lv_obj_set_style_text_font(g_preview_line2, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(g_preview_line2, lv_color_hex(COLOR_SECONDARY), 0);
    lv_obj_set_width(g_preview_line2, lv_pct(100));
    lv_obj_set_style_text_align(g_preview_line2, LV_TEXT_ALIGN_CENTER, 0);

    aod_update_selection_ui();
    aod_preview_refresh();

    /* 每秒刷新预览（录制时间/系统时间） */
    if (!g_preview_timer) {
        g_preview_timer = lv_timer_create(aod_preview_timer_cb, 1000, NULL);
    }
}

void settings_aod_open(void)
{
    if (!g_overlay) settings_aod_create();
    if (!g_overlay) return;
    aod_cfg_load();
    page_aod_set_style(s_aod_style);
    aod_update_selection_ui();
    aod_preview_refresh();
    ui_open_sub_page(g_overlay);
}

void settings_aod_close(void)
{
    if (g_overlay) lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
}

void settings_aod_destroy(void)
{
    if (g_preview_timer) {
        lv_timer_delete(g_preview_timer);
        g_preview_timer = NULL;
    }
    if (g_overlay) {
        lv_obj_delete(g_overlay);
        g_overlay = NULL;
    }
    g_btn_storage = NULL;
    g_btn_time = NULL;
    g_preview_line1 = NULL;
    g_preview_line2 = NULL;
}
