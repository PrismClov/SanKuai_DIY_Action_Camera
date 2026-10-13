/**
 * @file settings_language.c
 * @brief 语言设置页面
 */
#include "settings_language.h"
#include "../app_main.h"
#include "../common/ui_helpers.h"
#include "../common/app_i18n.h"

#include "lvgl/lvgl.h"

static lv_obj_t *g_overlay;
static lv_obj_t *g_body;
static int g_lang_sel = 0;

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

static void on_select(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    g_lang_sel = idx;
    LV_LOG_USER("Language selected: %d, applying...", idx);

    /* app_set_language() 内部调用 app_ui_recreate_async()，通过 lv_async_call
     * 在下一个 timer_handler 周期销毁并重建整个 UI。
     * 异步是必须的：本回调正在处理按钮点击事件，若直接同步销毁 UI 会
     * 删除事件源对象，导致 LVGL 事件循环 use-after-free 死机。 */
    app_set_language(idx);
}

static lv_obj_t *create_option(lv_obj_t *parent, const char *name, int idx)
{
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, ui_scale_min(56));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_radius(row, ui_scale_min(10), 0);
    lv_obj_set_style_pad_all(row, ui_scale_min(12), 0);
    lv_obj_add_event_cb(row, on_select, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    ui_attach_press_scale(row, 242);

    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, name);
    lv_obj_set_style_text_font(l, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COLOR_TEXT), 0);

    if (idx == g_lang_sel) {
        lv_obj_t *check = lv_label_create(row);
        lv_label_set_text(check, LV_SYMBOL_OK);
        lv_obj_set_style_text_font(check, ui_font_sym(), 0);
        lv_obj_set_style_text_color(check, lv_color_hex(COLOR_ACCENT), 0);
    }

    return row;
}

void settings_language_create(void)
{
    /* 同步当前语言选中态 */
    g_lang_sel = g_app_language;

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

    ui_make_header(g_overlay, tr("语言设置", "Language"),
                   close_cb, NULL, return_home_cb, NULL);

    g_body = lv_obj_create(g_overlay);
    /* 宽屏限宽 720px 居中，窄屏全宽 */
    lv_obj_set_width(g_body, ui_get_content_max_width());
    lv_obj_set_flex_grow(g_body, 1);
    lv_obj_set_flex_flow(g_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(g_body, ui_scale_min(12), 0);
    lv_obj_set_style_pad_gap(g_body, ui_scale_min(12), 0);
    lv_obj_set_style_bg_opa(g_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_body, 0, 0);

    /* 语言名称始终以本语言显示（便于用户识别），不翻译 */
    create_option(g_body, "简体中文", 0);
    create_option(g_body, "English", 1);

    /* 右滑返回手势 */
    ui_attach_back_gesture(g_overlay, close_cb);
}

void settings_language_open(void)
{
    /* 同步当前语言选中态，重建选项列表以反映最新选中标记 */
    g_lang_sel = g_app_language;
    if (g_body) {
        lv_obj_clean(g_body);
        create_option(g_body, "简体中文", 0);
        create_option(g_body, "English", 1);
    }
    ui_open_sub_page(g_overlay);
}

void settings_language_destroy(void)
{
    /* overlay 在 lv_layer_top() 上，旋转重建时必须删除避免内存泄漏 + 悬空指针 */
    if (g_overlay) {
        lv_obj_delete(g_overlay);
        g_overlay = NULL;
    }
    g_body = NULL;
}
