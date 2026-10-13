/**
 * @file settings_main.c
 * @brief 系统设置主页（卡片式入口）
 */
#include "settings_main.h"
#include "settings_device.h"
#include "settings_storage.h"
#include "settings_language.h"
#include "settings_auth.h"
#include "settings_wireless.h"
#include "settings_brightness.h"
#include "settings_aod.h"
#include "../app_main.h"
#include "../common/ui_helpers.h"
#include "../common/app_i18n.h"
#ifndef APP_WIFI_DISABLED
#include "../wifi/app_wifi.h"
#endif

#include "lvgl/lvgl.h"

static lv_obj_t *g_settings_overlay;

static void close_settings_cb(lv_event_t *e)
{
    (void)e;
    if (e && lv_event_get_code(e) == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        if (dir != LV_DIR_LEFT) return;
    }
    settings_page_close();
}

/* × 按钮：返回主屏幕（关闭设置 + 切回主页 tile） */
static void return_home_cb(lv_event_t *e)
{
    (void)e;
    app_return_home();
}

/* 右滑返回手势：设置主页右滑关闭（与 close_settings_cb 协同） */
static void back_gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    LV_LOG_USER("Settings: gesture dir=%d", dir);
    if (dir == LV_DIR_LEFT) {
        settings_page_close();
    }
}

#ifndef APP_WIFI_DISABLED
static void on_network_card(lv_event_t *e)
{
    (void)e;
    app_wifi_open();
}
#endif

static void on_storage_card(lv_event_t *e)
{
    (void)e;
    settings_storage_open();
}

static void on_device_card(lv_event_t *e)
{
    (void)e;
    settings_device_open();
}

static void on_language_card(lv_event_t *e)
{
    (void)e;
    settings_language_open();
}

static void on_auth_card(lv_event_t *e)
{
    (void)e;
    settings_auth_open();
}

static void on_wireless_card(lv_event_t *e)
{
    (void)e;
    settings_wireless_open();
}

static void on_aod_card(lv_event_t *e)
{
    (void)e;
    settings_aod_open();
}

void settings_page_create(void)
{
    g_settings_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_settings_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(g_settings_overlay, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(g_settings_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(g_settings_overlay, 0, 0);
    lv_obj_set_style_border_width(g_settings_overlay, 0, 0);
    lv_obj_remove_flag(g_settings_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_settings_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_flex_flow(g_settings_overlay, LV_FLEX_FLOW_COLUMN);
    /* 交叉轴居中：宽屏下 body 限宽 720px 后水平居中显示 */
    lv_obj_set_flex_align(g_settings_overlay,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 左滑返回手势：移除 GESTURE_BUBBLE（lv_obj_create 默认已设置），
     * 让手势在本对象停止并触发回调（详见 ui_attach_gesture 注释） */
    lv_obj_add_event_cb(g_settings_overlay, back_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_remove_flag(g_settings_overlay, LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* header：左返回箭头 + 中标题 + 右×返回主屏幕 */
    ui_make_header(g_settings_overlay, tr("设置", "Settings"),
                   close_settings_cb, g_settings_overlay,
                   return_home_cb, NULL);

    /* scrollable body */
    lv_obj_t *body = lv_obj_create(g_settings_overlay);
    /* 宽屏限宽 720px 居中，窄屏全宽 */
    lv_obj_set_width(body, ui_get_content_max_width());
    lv_obj_set_flex_grow(body, 1);
    /* 宽屏用 ROW_WRAP 实现双列网格（卡片宽 48%），竖屏用 COLUMN 单列 */
    lv_obj_set_flex_flow(body, ui_is_wide_layout() ? LV_FLEX_FLOW_ROW_WRAP : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START,
                          ui_is_wide_layout() ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    int body_pad = ui_scale_min(12);
    lv_obj_set_style_pad_all(body, body_pad, 0);
    lv_obj_set_style_pad_gap(body, body_pad, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);

    /* 设置卡片图标使用更大字体（28px），在 40px 圆形背景中更饱满、更居中 */
    const lv_font_t *sym_font_lg = &lv_font_montserrat_28;

#ifndef APP_WIFI_DISABLED
    ui_create_card(body, LV_SYMBOL_WIFI, sym_font_lg,
                   tr("网络", "Network"), tr("WiFi 连接", "WiFi Connection"), on_network_card);
#endif
    ui_create_card(body, LV_SYMBOL_WIFI, sym_font_lg,
                   tr("无线连接", "Wireless"), tr("AP 热点设置", "AP Hotspot Setup"), on_wireless_card);
    ui_create_card(body, LV_SYMBOL_SAVE, sym_font_lg,
                   tr("存储管理", "Storage"), tr("查看容量与格式化", "Capacity & Format"), on_storage_card);
    ui_create_card(body, LV_SYMBOL_EYE_OPEN, sym_font_lg,
                   tr("熄屏显示", "Always-On"), tr("样式一/样式二", "Style 1 / Style 2"), on_aod_card);
    ui_create_card(body, LV_SYMBOL_SETTINGS, sym_font_lg,
                   tr("设备信息", "Device"), tr("序列号、版本、网络", "SN, Version, Network"), on_device_card);
    ui_create_card(body, "文", ui_font_cjk_lg(),
                   tr("语言设置", "Language"), tr("简体中文", "Chinese / English"), on_language_card);
    ui_create_card(body, LV_SYMBOL_OK, sym_font_lg,
                   tr("认证信息", "Auth"), tr("设备认证", "Device Auth"), on_auth_card);

    /* create sub pages */
    settings_device_create();
    settings_aod_create();
    settings_storage_create();
    settings_language_create();
    settings_auth_create();
    settings_wireless_create();
    settings_brightness_create();
}

void settings_page_open(void)
{
    if (!g_settings_overlay) settings_page_create();
    if (g_settings_overlay) {
        lv_obj_remove_flag(g_settings_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(g_settings_overlay);
    }
}

void settings_page_close(void)
{
    if (g_settings_overlay)
        lv_obj_add_flag(g_settings_overlay, LV_OBJ_FLAG_HIDDEN);
}

void settings_page_destroy(void)
{
    if (g_settings_overlay) {
        lv_obj_delete(g_settings_overlay);
        g_settings_overlay = NULL;
    }
}
