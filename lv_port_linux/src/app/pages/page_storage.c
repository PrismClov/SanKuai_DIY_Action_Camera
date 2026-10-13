/**
 * @file page_storage.c
 * @brief 存储设置页面（参考图1）
 *
 * 特性：
 *   - 实时读取真实存储容量（内置/SD卡）
 *   - 自适应横屏/竖屏
 *   - 卡片选中态（黄色高亮）
 *   - 右滑返回主页
 *   - 2秒自动刷新
 */
#include "page_storage.h"
#include "../app_main.h"
#include "../common/ui_helpers.h"
#include "../settings/settings_storage.h"  /* 存储管理（格式化） */

#include "lvgl/lvgl.h"

#include <stdio.h>
#include <string.h>

static lv_obj_t *g_tile;
static lv_obj_t *g_card_int;
static lv_obj_t *g_card_sd;
static lv_obj_t *g_arc_int;
static lv_obj_t *g_arc_sd;
static lv_obj_t *g_cap_int;
static lv_obj_t *g_cap_sd;
static lv_obj_t *g_storage_int_label;
static lv_obj_t *g_storage_sd_label;
static int g_sel = 1;  /* 默认选中 SD 卡 */

/* 格式化容量显示：MB -> "X.XGB" */
static void fmt_size(long long mb, char *buf, size_t sz)
{
    if (mb >= 1024 * 1024) {
        snprintf(buf, sz, "%.2fTB", mb / 1024.0 / 1024.0);
    } else if (mb >= 1024) {
        snprintf(buf, sz, "%.1fGB", mb / 1024.0);
    } else {
        snprintf(buf, sz, "%lldMB", mb);
    }
}

/* 重新读取所有存储 */
static void read_all(void)
{
    long long ti = 0, ui = 0;
    long long ts = 0, us = 0;
    int ok_int = ui_detect_internal_storage(&ti, &ui);
    int ok_sd  = ui_detect_sd_card(&ts, &us, NULL, 0);

    /* 更新内部存储显示 */
    if (g_storage_int_label) {
        char buf_t[32] = {0}, buf_u[32] = {0};
        char text[80] = {0};
        if (ok_int) {
            fmt_size(ui, buf_u, sizeof(buf_u));
            fmt_size(ti, buf_t, sizeof(buf_t));
            snprintf(text, sizeof(text), "%s\n%s", buf_u, buf_t);
        } else {
            snprintf(text, sizeof(text), "未检测\n--");
        }
        lv_label_set_text(g_storage_int_label, text);
    }
    if (g_cap_int) {
        long long total = ti, used = ui;
        if (ok_int && total > 0) {
            lv_arc_set_value(g_arc_int, (int)(used * 100 / total));
        } else {
            lv_arc_set_value(g_arc_int, 0);
        }
    }

    /* 更新 SD 卡显示 */
    if (g_storage_sd_label) {
        char buf_t[32] = {0}, buf_u[32] = {0};
        char text[80] = {0};
        if (ok_sd) {
            fmt_size(us, buf_u, sizeof(buf_u));
            fmt_size(ts, buf_t, sizeof(buf_t));
            snprintf(text, sizeof(text), "%s\n%s", buf_u, buf_t);
        } else {
            snprintf(text, sizeof(text), "未插入\n--");
        }
        lv_label_set_text(g_storage_sd_label, text);
    }
    if (g_cap_sd) {
        long long total = ts, used = us;
        if (ok_sd && total > 0) {
            lv_arc_set_value(g_arc_sd, (int)(used * 100 / total));
        } else {
            lv_arc_set_value(g_arc_sd, 0);
        }
    }
}

/* 更新卡片选中样式 */
static void update_card_style(void)
{
    /* SD 卡：选中时亮黄色，未选中时深灰 */
    if (g_sel == 1) {
        lv_obj_set_style_bg_color(g_card_sd, lv_color_hex(COLOR_YELLOW), 0);
        lv_obj_set_style_border_color(g_card_sd, lv_color_hex(COLOR_YELLOW_BD), 0);
        lv_obj_set_style_border_width(g_card_sd, 3, 0);
    } else {
        lv_obj_set_style_bg_color(g_card_sd, lv_color_hex(0x2A2A2A), 0);
        lv_obj_set_style_border_color(g_card_sd, lv_color_hex(0x555555), 0);
        lv_obj_set_style_border_width(g_card_sd, 1, 0);
    }
    /* 内置存储：选中时黄色边框 */
    if (g_sel == 0) {
        lv_obj_set_style_bg_color(g_card_int, lv_color_hex(0x2A2A2A), 0);
        lv_obj_set_style_border_color(g_card_int, lv_color_hex(COLOR_YELLOW_BD), 0);
        lv_obj_set_style_border_width(g_card_int, 3, 0);
    } else {
        lv_obj_set_style_bg_color(g_card_int, lv_color_hex(0x2A2A2A), 0);
        lv_obj_set_style_border_color(g_card_int, lv_color_hex(0x555555), 0);
        lv_obj_set_style_border_width(g_card_int, 1, 0);
    }
}

void page_storage_refresh(int sd)
{
    g_sel = sd;
    read_all();
    update_card_style();
}

/* 定时器调用：实时刷新存储数据（仅在 storage 页面显示时） */
void page_storage_tick(void)
{
    if (!g_tile) return;
    /* 仅当存储页可见时才刷新（避免浪费 CPU） */
    if (g_tile && !lv_obj_has_flag(g_tile, LV_OBJ_FLAG_HIDDEN)) {
        read_all();
    }
}

static void on_tab_int(lv_event_t *e)
{
    (void)e;
    page_storage_refresh(0);
}

static void on_tab_sd(lv_event_t *e)
{
    (void)e;
    page_storage_refresh(1);
}

static void on_format_cb(lv_event_t *e)
{
    (void)e;
    /* 打开存储设置 overlay，用户可在其中选择格式化内置存储或 SD 卡 */
    settings_storage_open();
}

/* ------------------------------------------------------------------ */
/* 创建单个存储卡片（横向布局：左侧标题，右侧圆环+容量）                */
/* ------------------------------------------------------------------ */
static lv_obj_t *create_storage_card(lv_obj_t *parent, const char *title,
                                     lv_obj_t **arc_out, lv_obj_t **cap_out,
                                     lv_event_cb_t cb)
{
    int card_h = ui_scale_min(120);
    int arc_size = ui_scale_min(90);

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, lv_pct(100), card_h);
    lv_obj_set_flex_grow(card, 1);  /* 横屏 row 中均分宽度 */
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(card, ui_scale_min(12), 0);
    lv_obj_set_style_pad_gap(card, ui_scale_min(16), 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, ui_scale_min(10), 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, NULL);
    /* 按下反馈 */
    lv_obj_set_style_bg_color(card, lv_color_hex(0x3A3A3A), LV_STATE_PRESSED);

    /* 左侧：标题 */
    lv_obj_t *tl = lv_label_create(card);
    lv_label_set_text(tl, title);
    lv_obj_set_style_text_font(tl, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(tl, lv_color_white(), 0);
    lv_obj_set_width(tl, ui_scale_min(90));

    /* 右侧：圆环 + 容量文字 */
    lv_obj_t *right_box = lv_obj_create(card);
    lv_obj_remove_style_all(right_box);
    lv_obj_set_size(right_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(right_box, 1);
    lv_obj_set_style_bg_opa(right_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_box, 0, 0);
    /* LVGL v9: lv_obj_create 默认带 CLICKABLE，会拦截父卡片的点击事件 */
    lv_obj_remove_flag(right_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* 圆环（仅展示，不可拖动） */
    lv_obj_t *arc = lv_arc_create(right_box);
    lv_obj_set_size(arc, arc_size, arc_size);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_rotation(arc, 270);
    lv_obj_set_style_arc_color(arc, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x404040), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, ui_scale_min(6), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, ui_scale_min(6), LV_PART_INDICATOR);
    lv_obj_align(arc, LV_ALIGN_RIGHT_MID, 0, 0);
    /* arc 默认可拖动，移除 CLICKABLE 让点击穿透到 card */
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);

    /* 容量文字（叠加在圆环上）*/
    lv_obj_t *cap = lv_label_create(right_box);
    lv_label_set_text(cap, "--");
    lv_obj_set_style_text_font(cap, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(cap, lv_color_white(), 0);
    lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(cap, arc_size);
    lv_obj_align(cap, LV_ALIGN_RIGHT_MID, 0, -4);

    *arc_out = arc;
    *cap_out = cap;
    return card;
}

/* ------------------------------------------------------------------ */
/* 创建页面                                                            */
/* ------------------------------------------------------------------ */
lv_obj_t * page_storage_create(lv_obj_t *tv)
{
    /* 从主页右滑进入：tile 在 (1,1) 位置 */
    g_tile = lv_tileview_add_tile(tv, 1, 1, LV_DIR_RIGHT);
    lv_obj_set_style_bg_color(g_tile, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(g_tile, 0, 0);
    lv_obj_remove_flag(g_tile, LV_OBJ_FLAG_SCROLLABLE);

    /* 黑色全屏背景 */
    lv_obj_t *bg = lv_obj_create(g_tile);
    lv_obj_set_size(bg, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(bg, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_set_style_pad_all(bg, ui_scale_min(16), 0);
    lv_obj_remove_flag(bg, LV_OBJ_FLAG_SCROLLABLE);

    /* 卡片容器：竖屏垂直堆叠，横屏水平并排 */
    lv_obj_t *cards = lv_obj_create(bg);
    lv_obj_remove_style_all(cards);
    lv_obj_set_size(cards, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_align(cards, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_flex_flow(cards, ui_is_landscape() ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cards, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(cards, ui_scale_min(12), 0);
    lv_obj_set_style_bg_opa(cards, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cards, 0, 0);
    lv_obj_remove_flag(cards, LV_OBJ_FLAG_SCROLLABLE);

    g_card_int = create_storage_card(cards, "内置存储",
                                     &g_arc_int, &g_cap_int, on_tab_int);
    g_storage_int_label = g_cap_int;  /* 用于显示容量 */

    g_card_sd  = create_storage_card(cards, "SD 卡",
                                     &g_arc_sd,  &g_cap_sd,  on_tab_sd);
    g_storage_sd_label = g_cap_sd;  /* 用于显示容量 */

    /* 底部"存储管理"按钮 */
    lv_obj_t *manage = lv_button_create(bg);
    lv_obj_set_size(manage, lv_pct(80), ui_scale_min(48));
    lv_obj_align(manage, LV_ALIGN_BOTTOM_MID, 0, -ui_scale_min(16));
    lv_obj_set_style_bg_color(manage, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_radius(manage, ui_scale_min(10), 0);
    lv_obj_set_style_border_width(manage, 0, 0);
    lv_obj_set_style_shadow_width(manage, 0, 0);
    lv_obj_remove_flag(manage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(manage, on_format_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ml = lv_label_create(manage);
    lv_label_set_text(ml, "存储管理");
    lv_obj_set_style_text_font(ml, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(ml, lv_color_white(), 0);
    lv_obj_center(ml);

    /* tileview 原生滑动处理返回导航（LV_DIR_RIGHT 向右滑 → home），
     * 无需手动 GESTURE 回调 */

    /* 存储刷新由 app_main.c 的全局定时器统一驱动，页面自身不再创建本地定时器 */

    page_storage_refresh(1);  /* 默认选中 SD 卡 */
    return g_tile;
}
