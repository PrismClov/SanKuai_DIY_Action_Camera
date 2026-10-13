/**
 * @file settings_wireless.c
 * @brief 无线连接设置页面（AP 热点模式）
 *
 * 菜单项：
 *   - 无线连接开关：启用/禁用 AP 热点
 *   - 无线信息：设置 WiFi 名称（SSID）和连接密码（二级菜单）
 *   - WiFi 频段：2.4G / 5.8G 选择（二级菜单）
 *   - 重置连接：恢复默认设置（SSID=设备名称, 密码=12345678, 频段=2.4G）
 *
 * 设置持久化到 /tmp/wireless_settings.conf
 */
#include "settings_wireless.h"
#include "../app_main.h"
#include "../common/ui_helpers.h"
#include "../common/app_i18n.h"

#include "lvgl/lvgl.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* 无线设置存储                                                        */
/* ------------------------------------------------------------------ */
#define WIRELESS_CONF_PATH "/tmp/wireless_settings.conf"
#define WIRELESS_SSID_MAX  64
#define WIRELESS_PSK_MAX   64

typedef struct {
    int  enabled;                           /* 0=off, 1=on */
    char ssid[WIRELESS_SSID_MAX];           /* AP 热点 SSID */
    char password[WIRELESS_PSK_MAX];        /* AP 热点密码 */
    int  band;                              /* 0=2.4G, 1=5.8G */
} wireless_settings_t;

static wireless_settings_t g_wireless;
static int g_wireless_loaded = 0;

/* 获取设备名称（hostname）作为默认 SSID */
static void get_device_name(char *buf, size_t sz)
{
    gethostname(buf, sz);
    if (!buf[0]) {
        strncpy(buf, "CameraAP", sz - 1);
        buf[sz - 1] = '\0';
    }
}

/* 加载设置（首次调用时从配置文件读取，失败则用默认值） */
static void wireless_load(void)
{
    if (g_wireless_loaded) return;
    g_wireless_loaded = 1;

    /* 默认值 */
    g_wireless.enabled  = 0;
    get_device_name(g_wireless.ssid, sizeof(g_wireless.ssid));
    strncpy(g_wireless.password, "12345678", sizeof(g_wireless.password) - 1);
    g_wireless.password[sizeof(g_wireless.password) - 1] = '\0';
    g_wireless.band = 0;  /* 2.4G */

    /* 从配置文件读取 */
    FILE *f = fopen(WIRELESS_CONF_PATH, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        /* 去掉换行 */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (strncmp(line, "enabled=", 8) == 0)
            g_wireless.enabled = atoi(line + 8);
        else if (strncmp(line, "ssid=", 5) == 0) {
            strncpy(g_wireless.ssid, line + 5, sizeof(g_wireless.ssid) - 1);
            g_wireless.ssid[sizeof(g_wireless.ssid) - 1] = '\0';
        }
        else if (strncmp(line, "password=", 9) == 0) {
            strncpy(g_wireless.password, line + 9, sizeof(g_wireless.password) - 1);
            g_wireless.password[sizeof(g_wireless.password) - 1] = '\0';
        }
        else if (strncmp(line, "band=", 5) == 0)
            g_wireless.band = atoi(line + 5);
    }
    fclose(f);
}

/* 保存设置到配置文件 */
static void wireless_save(void)
{
    FILE *f = fopen(WIRELESS_CONF_PATH, "w");
    if (!f) return;
    fprintf(f, "enabled=%d\n", g_wireless.enabled);
    fprintf(f, "ssid=%s\n", g_wireless.ssid);
    fprintf(f, "password=%s\n", g_wireless.password);
    fprintf(f, "band=%d\n", g_wireless.band);
    fclose(f);
}

/* 重置为默认值 */
static void wireless_reset_defaults(void)
{
    g_wireless.enabled  = 0;
    get_device_name(g_wireless.ssid, sizeof(g_wireless.ssid));
    strncpy(g_wireless.password, "12345678", sizeof(g_wireless.password) - 1);
    g_wireless.password[sizeof(g_wireless.password) - 1] = '\0';
    g_wireless.band = 0;
    wireless_save();
}

/* ------------------------------------------------------------------ */
/* overlay 句柄                                                        */
/* ------------------------------------------------------------------ */
static lv_obj_t *g_overlay;           /* 主无线设置页 */
static lv_obj_t *g_info_overlay;      /* 无线信息二级页 */
static lv_obj_t *g_band_overlay;      /* WiFi频段二级页 */
static lv_obj_t *g_input_overlay;     /* 文本输入弹窗（SSID/密码） */

/* 主页 UI 句柄 */
static lv_obj_t *g_toggle_sw;         /* 开关 */
static lv_obj_t *g_ssid_value;        /* SSID 显示值（无线信息页） */
static lv_obj_t *g_psk_value;         /* 密码显示值（无线信息页） */
static lv_obj_t *g_band_value;        /* 频段显示值（主页） */

/* 文本输入弹窗上下文 */
static int g_input_is_password = 0;   /* 1=输入密码(掩码), 0=输入SSID */

/* ------------------------------------------------------------------ */
/* 前向声明                                                            */
/* ------------------------------------------------------------------ */
static void refresh_main_ui(void);
static void refresh_info_ui(void);
static void refresh_band_ui(void);

/* ------------------------------------------------------------------ */
/* 主页回调                                                            */
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

static void on_toggle(lv_event_t *e)
{
    (void)e;
    g_wireless.enabled = lv_obj_has_state(g_toggle_sw, LV_STATE_CHECKED) ? 1 : 0;
    wireless_save();
    LV_LOG_USER("Wireless %s", g_wireless.enabled ? "enabled" : "disabled");
}

static void on_info_card(lv_event_t *e)
{
    (void)e;
    refresh_info_ui();
    ui_open_sub_page(g_info_overlay);
}

static void on_band_card(lv_event_t *e)
{
    (void)e;
    refresh_band_ui();
    ui_open_sub_page(g_band_overlay);
}

static void on_reset_btn(lv_event_t *e)
{
    (void)e;
    wireless_reset_defaults();
    refresh_main_ui();
    LV_LOG_USER("Wireless settings reset to defaults");
}

/* ------------------------------------------------------------------ */
/* 无线信息二级页回调                                                  */
/* ------------------------------------------------------------------ */
static void info_close_cb(lv_event_t *e)
{
    (void)e;
    if (g_info_overlay) lv_obj_add_flag(g_info_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void info_return_home_cb(lv_event_t *e)
{
    (void)e;
    if (g_info_overlay) lv_obj_add_flag(g_info_overlay, LV_OBJ_FLAG_HIDDEN);
    app_return_home();
}

/* ------------------------------------------------------------------ */
/* WiFi频段二级页回调                                                  */
/* ------------------------------------------------------------------ */
static void band_close_cb(lv_event_t *e)
{
    (void)e;
    if (g_band_overlay) lv_obj_add_flag(g_band_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void band_return_home_cb(lv_event_t *e)
{
    (void)e;
    if (g_band_overlay) lv_obj_add_flag(g_band_overlay, LV_OBJ_FLAG_HIDDEN);
    app_return_home();
}

static void on_band_select(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    g_wireless.band = idx;
    wireless_save();
    refresh_band_ui();
    refresh_main_ui();
    /* 选完后自动关闭频段页 */
    if (g_band_overlay) lv_obj_add_flag(g_band_overlay, LV_OBJ_FLAG_HIDDEN);
}

/* ------------------------------------------------------------------ */
/* 文本输入弹窗（SSID/密码）                                          */
/* ------------------------------------------------------------------ */
static lv_obj_t *g_input_ta;
static lv_obj_t *g_input_kb;

static void input_close_cb(lv_event_t *e)
{
    (void)e;
    if (g_input_overlay) lv_obj_add_flag(g_input_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void input_save_cb(lv_event_t *e)
{
    (void)e;
    if (!g_input_ta) return;
    const char *txt = lv_textarea_get_text(g_input_ta);
    if (txt && txt[0]) {
        if (g_input_is_password) {
            strncpy(g_wireless.password, txt, sizeof(g_wireless.password) - 1);
            g_wireless.password[sizeof(g_wireless.password) - 1] = '\0';
        } else {
            strncpy(g_wireless.ssid, txt, sizeof(g_wireless.ssid) - 1);
            g_wireless.ssid[sizeof(g_wireless.ssid) - 1] = '\0';
        }
        wireless_save();
        refresh_info_ui();
        refresh_main_ui();
    }
    if (g_input_overlay) lv_obj_add_flag(g_input_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void on_ssid_row(lv_event_t *e)
{
    (void)e;
    g_input_is_password = 0;
    if (!g_input_overlay) return;
    if (g_input_ta) {
        lv_textarea_set_text(g_input_ta, g_wireless.ssid);
        lv_textarea_set_password_mode(g_input_ta, false);
    }
    ui_open_sub_page(g_input_overlay);
    if (g_input_kb) lv_keyboard_set_textarea(g_input_kb, g_input_ta);
}

static void on_psk_row(lv_event_t *e)
{
    (void)e;
    g_input_is_password = 1;
    if (!g_input_overlay) return;
    if (g_input_ta) {
        lv_textarea_set_text(g_input_ta, g_wireless.password);
        lv_textarea_set_password_mode(g_input_ta, true);
        lv_textarea_set_password_bullet(g_input_ta, "*");
    }
    ui_open_sub_page(g_input_overlay);
    if (g_input_kb) lv_keyboard_set_textarea(g_input_kb, g_input_ta);
}

/* ------------------------------------------------------------------ */
/* UI 刷新                                                             */
/* ------------------------------------------------------------------ */
static void refresh_main_ui(void)
{
    if (g_toggle_sw) {
        if (g_wireless.enabled) lv_obj_add_state(g_toggle_sw, LV_STATE_CHECKED);
        else                    lv_obj_clear_state(g_toggle_sw, LV_STATE_CHECKED);
    }
    if (g_band_value) {
        lv_label_set_text(g_band_value, g_wireless.band ? "5.8G" : "2.4G");
    }
}

static void refresh_info_ui(void)
{
    if (g_ssid_value) lv_label_set_text(g_ssid_value, g_wireless.ssid);
    if (g_psk_value) {
        /* 密码用星号掩码显示 */
        int len = strlen(g_wireless.password);
        char masked[WIRELESS_PSK_MAX];
        int n = len > (int)sizeof(masked) - 1 ? (int)sizeof(masked) - 1 : len;
        for (int i = 0; i < n; i++) masked[i] = '*';
        masked[n] = '\0';
        lv_label_set_text(g_psk_value, masked);
    }
}

static void refresh_band_ui(void)
{
    /* 频段页的选中状态通过重建选项实现 */
}

/* ------------------------------------------------------------------ */
/* 创建可点击的信息行（label + value + 箭头）                         */
/* ------------------------------------------------------------------ */
static lv_obj_t *make_value_row(lv_obj_t *parent, const char *label,
                                 lv_event_cb_t cb, lv_obj_t **out_value)
{
    int pad = ui_scale_min(12);
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, ui_scale_min(56));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_radius(row, ui_scale_min(10), 0);
    lv_obj_set_style_pad_all(row, pad, 0);
    lv_obj_set_style_pad_gap(row, ui_scale_min(8), 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    ui_attach_press_scale(row, 242);
    if (cb) lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COLOR_SECONDARY), 0);

    /* 右侧：value + 箭头 */
    lv_obj_t *right = lv_obj_create(row);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(right, ui_scale_min(6), 0);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    if (out_value) {
        *out_value = lv_label_create(right);
        lv_obj_set_style_text_font(*out_value, ui_font_cjk(), 0);
        lv_obj_set_style_text_color(*out_value, lv_color_hex(COLOR_TEXT), 0);
    }

    lv_obj_t *arrow = lv_label_create(right);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(arrow, ui_font_sym(), 0);
    lv_obj_set_style_text_color(arrow, lv_color_hex(COLOR_SECONDARY), 0);

    return row;
}

/* 创建开关行（label + switch） */
static lv_obj_t *make_switch_row(lv_obj_t *parent, const char *label,
                                  lv_event_cb_t cb, lv_obj_t **out_sw)
{
    int pad = ui_scale_min(12);
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, ui_scale_min(56));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_radius(row, ui_scale_min(10), 0);
    lv_obj_set_style_pad_all(row, pad, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COLOR_SECONDARY), 0);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, ui_scale_min(56), ui_scale_min(28));
    if (cb) lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (out_sw) *out_sw = sw;

    return row;
}

/* ------------------------------------------------------------------ */
/* 创建各 overlay                                                      */
/* ------------------------------------------------------------------ */

/* 主无线设置页 */
static void make_main_overlay(void)
{
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

    ui_make_header(g_overlay, tr("无线连接", "Wireless"),
                   close_cb, NULL, return_home_cb, NULL);

    lv_obj_t *body = lv_obj_create(g_overlay);
    /* 宽屏限宽 720px 居中，窄屏全宽 */
    lv_obj_set_width(body, ui_get_content_max_width());
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    int pad = ui_scale_min(12);
    lv_obj_set_style_pad_all(body, pad, 0);
    lv_obj_set_style_pad_gap(body, pad, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);

    /* 无线连接开关 */
    make_switch_row(body, tr("无线连接", "Wireless Connection"),
                    on_toggle, &g_toggle_sw);

    /* 无线信息 */
    make_value_row(body, tr("无线信息", "Wireless Info"),
                   on_info_card, NULL);

    /* WiFi 频段 */
    make_value_row(body, tr("WiFi 频段", "WiFi Band"),
                   on_band_card, &g_band_value);

    /* 重置连接按钮 */
    lv_obj_t *reset_btn = lv_button_create(body);
    lv_obj_set_width(reset_btn, lv_pct(100));
    lv_obj_set_height(reset_btn, ui_scale_min(56));
    lv_obj_set_style_bg_color(reset_btn, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_radius(reset_btn, ui_scale_min(10), 0);
    lv_obj_set_style_border_width(reset_btn, 0, 0);
    lv_obj_remove_flag(reset_btn, LV_OBJ_FLAG_SCROLLABLE);
    ui_attach_press_scale(reset_btn, 242);
    lv_obj_add_event_cb(reset_btn, on_reset_btn, LV_EVENT_CLICKED, NULL);

    lv_obj_t *reset_lbl = lv_label_create(reset_btn);
    lv_label_set_text(reset_lbl, tr("重置连接", "Reset Connection"));
    lv_obj_set_style_text_font(reset_lbl, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(reset_lbl, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_center(reset_lbl);

    ui_attach_back_gesture(g_overlay, close_cb);
}

/* 无线信息二级页 */
static void make_info_overlay(void)
{
    g_info_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_info_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(g_info_overlay, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(g_info_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(g_info_overlay, 0, 0);
    lv_obj_set_style_border_width(g_info_overlay, 0, 0);
    lv_obj_remove_flag(g_info_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_info_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_flex_flow(g_info_overlay, LV_FLEX_FLOW_COLUMN);
    /* 交叉轴居中：宽屏下 body 限宽后水平居中 */
    lv_obj_set_flex_align(g_info_overlay,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    ui_make_header(g_info_overlay, tr("无线信息", "Wireless Info"),
                   info_close_cb, NULL, info_return_home_cb, NULL);

    lv_obj_t *body = lv_obj_create(g_info_overlay);
    /* 宽屏限宽 720px 居中，窄屏全宽 */
    lv_obj_set_width(body, ui_get_content_max_width());
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    int pad = ui_scale_min(12);
    lv_obj_set_style_pad_all(body, pad, 0);
    lv_obj_set_style_pad_gap(body, pad, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);

    /* WiFi 名称 */
    make_value_row(body, tr("WiFi 名称", "WiFi Name"),
                   on_ssid_row, &g_ssid_value);

    /* 连接密码 */
    make_value_row(body, tr("连接密码", "Password"),
                   on_psk_row, &g_psk_value);

    ui_attach_back_gesture(g_info_overlay, info_close_cb);
}

/* WiFi频段二级页 */
static void make_band_overlay(void)
{
    g_band_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_band_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(g_band_overlay, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(g_band_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(g_band_overlay, 0, 0);
    lv_obj_set_style_border_width(g_band_overlay, 0, 0);
    lv_obj_remove_flag(g_band_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_band_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_flex_flow(g_band_overlay, LV_FLEX_FLOW_COLUMN);
    /* 交叉轴居中：宽屏下 body 限宽后水平居中 */
    lv_obj_set_flex_align(g_band_overlay,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    ui_make_header(g_band_overlay, tr("WiFi 频段", "WiFi Band"),
                   band_close_cb, NULL, band_return_home_cb, NULL);

    lv_obj_t *body = lv_obj_create(g_band_overlay);
    /* 宽屏限宽 720px 居中，窄屏全宽 */
    lv_obj_set_width(body, ui_get_content_max_width());
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    int pad = ui_scale_min(12);
    lv_obj_set_style_pad_all(body, pad, 0);
    lv_obj_set_style_pad_gap(body, pad, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);

    /* 频段选项 */
    const char *bands_zh[] = {"2.4G", "5.8G"};
    const char *bands_en[] = {"2.4G", "5.8G"};
    const char **bands = g_app_language ? bands_en : bands_zh;
    for (int i = 0; i < 2; i++) {
        lv_obj_t *row = lv_button_create(body);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, ui_scale_min(56));
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_PANEL), 0);
        lv_obj_set_style_radius(row, ui_scale_min(10), 0);
        lv_obj_set_style_pad_all(row, pad, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        ui_attach_press_scale(row, 242);
        lv_obj_add_event_cb(row, on_band_select, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *l = lv_label_create(row);
        lv_label_set_text(l, bands[i]);
        lv_obj_set_style_text_font(l, ui_font_cjk(), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(COLOR_TEXT), 0);

        if (i == g_wireless.band) {
            lv_obj_t *check = lv_label_create(row);
            lv_label_set_text(check, LV_SYMBOL_OK);
            lv_obj_set_style_text_font(check, ui_font_sym(), 0);
            lv_obj_set_style_text_color(check, lv_color_hex(COLOR_ACCENT), 0);
        }
    }

    ui_attach_back_gesture(g_band_overlay, band_close_cb);
}

/* 文本输入弹窗（SSID/密码共用） */
static void make_input_overlay(void)
{
    g_input_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_input_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(g_input_overlay, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(g_input_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(g_input_overlay, 0, 0);
    lv_obj_set_style_border_width(g_input_overlay, 0, 0);
    lv_obj_remove_flag(g_input_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_input_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_flex_flow(g_input_overlay, LV_FLEX_FLOW_COLUMN);

    /* header: 返回箭头 + 标题(动态) + × 关闭 */
    /* 标题文字动态设置，这里先用占位 */
    ui_make_header(g_input_overlay, "",
                   input_close_cb, NULL, input_close_cb, NULL);

    /* 输入区 */
    lv_obj_t *input_area = lv_obj_create(g_input_overlay);
    lv_obj_set_width(input_area, lv_pct(100));
    lv_obj_set_height(input_area, ui_scale_min(130));
    lv_obj_set_flex_flow(input_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(input_area, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    int ip = ui_scale_min(12);
    lv_obj_set_style_pad_all(input_area, ip, 0);
    lv_obj_set_style_pad_gap(input_area, ip, 0);
    lv_obj_set_style_bg_opa(input_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(input_area, 0, 0);
    lv_obj_remove_flag(input_area, LV_OBJ_FLAG_SCROLLABLE);

    g_input_ta = lv_textarea_create(input_area);
    lv_obj_set_width(g_input_ta, lv_pct(90));
    lv_obj_set_height(g_input_ta, ui_scale_min(54));
    lv_textarea_set_one_line(g_input_ta, true);
    lv_textarea_set_max_length(g_input_ta, 63);
    lv_obj_set_style_text_font(g_input_ta, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(g_input_ta, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_bg_color(g_input_ta, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_border_color(g_input_ta, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(g_input_ta, 2, 0);

    /* 按钮行 */
    lv_obj_t *btn_row = lv_obj_create(input_area);
    lv_obj_set_width(btn_row, lv_pct(90));
    lv_obj_set_height(btn_row, ui_scale_min(48));
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(btn_row, ui_scale_min(12), 0);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_remove_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cancel_btn = lv_button_create(btn_row);
    lv_obj_set_size(cancel_btn, ui_scale_min(100), ui_scale_min(40));
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(cancel_btn, ui_scale_min(8), 0);
    lv_obj_set_style_border_width(cancel_btn, 0, 0);
    lv_obj_remove_flag(cancel_btn, LV_OBJ_FLAG_SCROLLABLE);
    ui_attach_press_scale(cancel_btn, 242);
    lv_obj_add_event_cb(cancel_btn, input_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, tr("取消", "Cancel"));
    lv_obj_set_style_text_font(cancel_lbl, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(cancel_lbl, lv_color_white(), 0);
    lv_obj_center(cancel_lbl);

    lv_obj_t *save_btn = lv_button_create(btn_row);
    lv_obj_set_size(save_btn, ui_scale_min(100), ui_scale_min(40));
    lv_obj_set_style_bg_color(save_btn, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_radius(save_btn, ui_scale_min(8), 0);
    lv_obj_set_style_border_width(save_btn, 0, 0);
    lv_obj_remove_flag(save_btn, LV_OBJ_FLAG_SCROLLABLE);
    ui_attach_press_scale(save_btn, 242);
    lv_obj_add_event_cb(save_btn, input_save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_lbl = lv_label_create(save_btn);
    lv_label_set_text(save_lbl, tr("保存", "Save"));
    lv_obj_set_style_text_font(save_lbl, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(save_lbl, lv_color_black(), 0);
    lv_obj_center(save_lbl);

    /* 键盘 */
    g_input_kb = lv_keyboard_create(g_input_overlay);
    lv_obj_set_height(g_input_kb, ui_scale_min(200));
    lv_obj_set_style_bg_color(g_input_kb, lv_color_hex(COLOR_PANEL), 0);
    lv_keyboard_set_mode(g_input_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
}

/* ------------------------------------------------------------------ */
/* 公共 API                                                            */
/* ------------------------------------------------------------------ */
void settings_wireless_create(void)
{
    wireless_load();
    make_main_overlay();
    make_info_overlay();
    make_band_overlay();
    make_input_overlay();
    refresh_main_ui();
    refresh_info_ui();
}

void settings_wireless_open(void)
{
    wireless_load();
    refresh_main_ui();
    ui_open_sub_page(g_overlay);
}

void settings_wireless_destroy(void)
{
    if (g_input_overlay) { lv_obj_delete(g_input_overlay); g_input_overlay = NULL; }
    if (g_band_overlay)  { lv_obj_delete(g_band_overlay);  g_band_overlay = NULL; }
    if (g_info_overlay)  { lv_obj_delete(g_info_overlay);  g_info_overlay = NULL; }
    if (g_overlay)       { lv_obj_delete(g_overlay);       g_overlay = NULL; }
    g_toggle_sw = NULL;
    g_ssid_value = NULL;
    g_psk_value = NULL;
    g_band_value = NULL;
    g_input_ta = NULL;
    g_input_kb = NULL;
}
