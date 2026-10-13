/**
 * @file settings_auth.c
 * @brief 认证信息页面
 */
#include "settings_auth.h"
#include "../app_main.h"
#include "../common/ui_helpers.h"
#include "../common/app_i18n.h"

#include "lvgl/lvgl.h"
#include <stdio.h>

static lv_obj_t *g_overlay;

static void close_cb(lv_event_t *e)
{
    (void)e;
    if (e && lv_event_get_code(e) == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        if (dir != LV_DIR_RIGHT) return;  /* 右滑关闭，与 ui_attach_back_gesture 一致 */
    }
    if (g_overlay) lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
}

/* × 按钮：关闭当前页 + 返回主屏幕 */
static void return_home_cb(lv_event_t *e)
{
    (void)e;
    if (g_overlay) lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
    app_return_home();
}

/* 创建一条居中的信息文本行 */
static lv_obj_t *auth_add_info_row(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

/* 创建认证图标行：左侧圆形徽章 + 右侧认证编号 */
static lv_obj_t *auth_add_cert_row(lv_obj_t *parent, const char *mark, const char *id)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row, ui_scale_min(10), 0);

    const int badge_size = ui_scale_min(40);
    lv_obj_t *badge = lv_obj_create(row);
    lv_obj_set_size(badge, badge_size, badge_size);
    lv_obj_set_style_bg_color(badge, lv_color_white(), 0);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(badge, 0, 0);
    lv_obj_set_style_pad_all(badge, 0, 0);

    lv_obj_t *mark_lbl = lv_label_create(badge);
    lv_label_set_text(mark_lbl, mark);
    lv_obj_set_style_text_font(mark_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(mark_lbl, lv_color_black(), 0);
    lv_obj_center(mark_lbl);

    lv_obj_t *id_lbl = lv_label_create(row);
    lv_label_set_text(id_lbl, id);
    lv_obj_set_style_text_font(id_lbl, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(id_lbl, lv_color_white(), 0);

    return row;
}

void settings_auth_create(void)
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

    ui_make_header(g_overlay, tr("认证信息", "Auth Info"),
                   close_cb, NULL, return_home_cb, NULL);

    /* 可滚动内容区 */
    lv_obj_t *body = lv_obj_create(g_overlay);
    /* 宽屏限宽 720px 居中，窄屏全宽 */
    lv_obj_set_width(body, ui_get_content_max_width());
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(body, ui_scale_min(24), 0);
    lv_obj_set_style_pad_gap(body, ui_scale_min(16), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);

    /* 1. 设备 Logo + 名称 */
    lv_obj_t *logo_box = lv_obj_create(body);
    lv_obj_set_size(logo_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(logo_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(logo_box, 0, 0);
    lv_obj_set_flex_flow(logo_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(logo_box, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(logo_box, ui_scale_min(10), 0);

    lv_obj_t *logo = lv_label_create(logo_box);
    lv_label_set_text(logo, "SanKuai_WanJi");
    lv_obj_set_style_text_font(logo, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(logo, lv_color_white(), 0);

    lv_obj_t *name = lv_label_create(logo_box);
    lv_label_set_text(name, "DION 5 Pro");
    lv_obj_set_style_text_font(name, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(name, lv_color_white(), 0);

    /* 2. 各种 ID 信息 */
    lv_obj_t *info_box = lv_obj_create(body);
    lv_obj_set_width(info_box, lv_pct(100));
    lv_obj_set_height(info_box, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(info_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(info_box, 0, 0);
    lv_obj_set_flex_flow(info_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(info_box, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(info_box, ui_scale_min(8), 0);

    auth_add_info_row(info_box, tr("型号 / 型號 / Model: DC001", "Model: DC001"));
    auth_add_info_row(info_box, "CMIIT ID: 24J446567822");
    auth_add_info_row(info_box, "FCC ID: 2ANDR-DC0012024");
    auth_add_info_row(info_box, "IC: 23060-DC0012024");
    auth_add_info_row(info_box, "CAN ICES-003(B) / NMB-003(B)");

    /* 3. 输入电气参数 */
    auth_add_info_row(info_box, tr("输入: 5-12V == 3A Max", "Input: 5-12V == 3A Max"));

    /* 4. 认证图标区 */
    lv_obj_t *cert_box = lv_obj_create(body);
    lv_obj_set_width(cert_box, lv_pct(100));
    lv_obj_set_height(cert_box, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(cert_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cert_box, 0, 0);
    lv_obj_set_flex_flow(cert_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cert_box, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(cert_box, ui_scale_min(16), 0);

    auth_add_cert_row(cert_box, "KC", "R-C-osm-AC004");
    auth_add_cert_row(cert_box, "R",  "018-240083");
    auth_add_cert_row(cert_box, "CE", "AC0042024");

    /* 右滑返回手势 */
    ui_attach_back_gesture(g_overlay, close_cb);
}

void settings_auth_open(void)
{
    ui_open_sub_page(g_overlay);
}

void settings_auth_destroy(void)
{
    /* overlay 在 lv_layer_top() 上，旋转重建时必须删除避免内存泄漏 + 悬空指针 */
    if (g_overlay) {
        lv_obj_delete(g_overlay);
        g_overlay = NULL;
    }
}
