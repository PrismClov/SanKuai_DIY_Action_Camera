/**
 * @file page_menu.c
 * @brief 上滑功能选择页（参考图5）
 *
 * 布局：
 *   竖屏(480x800)  → 2 列 × 4 行
 *   横屏(800x480)  → 4 列 × 2 行
 * 每张卡片用 FLEX_COLUMN + SPACE_EVENLY 布局，图标与标签作为整体在卡片内
 * 均衡分布；图标使用 40px 符号字体，避免"图标小、留白大"的问题。
 */
#include "page_menu.h"
#include "../app_main.h"
#include "../common/ui_helpers.h"
#include "../common/app_i18n.h"
#include "../settings/settings_main.h"
#include "../settings/settings_brightness.h"
#include "../settings/settings_storage.h"
#include "page_aod.h"

#include "lvgl/lvgl.h"

static void goto_quick_cb(lv_event_t *e)
{
    (void)e;
    lv_tileview_set_tile(g_tv, g_tile_quick, LV_ANIM_ON);
}

static void goto_album_cb(lv_event_t *e)
{
    (void)e;
    lv_tileview_set_tile(g_tv, g_tile_album, LV_ANIM_ON);
}

static void open_settings_cb(lv_event_t *e)
{
    (void)e;
    settings_page_open();
}

static void open_brightness_cb(lv_event_t *e)
{
    (void)e;
    settings_brightness_open();
}

static void open_aod_cb(lv_event_t *e)
{
    (void)e;
    page_aod_open();
}

static void on_swipe_left_storage(lv_event_t *e)
{
    (void)e;
    settings_storage_open();
}

/* 旋转屏幕：异步调用 app_ui_recreate() 切换横竖屏。
 * 必须用异步方式（lv_async_call），避免在按钮点击事件回调中
 * 直接销毁事件源对象导致 use-after-free 死机。 */
static void rotate_screen_cb(lv_event_t *e)
{
    (void)e;
    /* 关键：旋转前先把 tileview 切回 home tile，让 tileview 的
     * 当前活动 tile 标记处于已知状态。如果用户在 menu tile 上点击
     * 旋转按钮，tileview 内部可能正处于向 menu tile 的导航动画/状态中，
     * 此时 destroy + 重建会出现残留坐标导致首帧错位（花屏）。
     * LV_ANIM_OFF 仍可能产生 pending 的 scroll 动画，强制 update_layout
     * 并发送一次刷新，让 tileview 状态立即落定后再调度异步重建。 */
    if (g_tv && g_tile_home) {
        lv_tileview_set_tile(g_tv, g_tile_home, LV_ANIM_OFF);
        lv_obj_update_layout(g_tv);
        lv_obj_invalidate(g_tv);
    }
    app_ui_recreate_async();
}

static void placeholder_cb(lv_event_t *e)
{
    (void)e;
}

typedef struct {
    const char *icon;
    const lv_font_t *icon_font;
    const char *label_zh;
    const char *label_en;
    void (*cb)(lv_event_t *e);
} menu_item_t;

static const menu_item_t s_items[] = {
    { LV_SYMBOL_HOME,      NULL,       "用户",     "User",     placeholder_cb },
    { LV_SYMBOL_BARS,      NULL,       "快速设置", "Quick",    goto_quick_cb },
    { LV_SYMBOL_TINT,      NULL,       "亮度",     "Bright",   open_brightness_cb },
    { LV_SYMBOL_SETTINGS,  NULL,       "设置",     "Settings", open_settings_cb },
    { LV_SYMBOL_REFRESH,   NULL,       "旋转",     "Rotate",   rotate_screen_cb },
    { LV_SYMBOL_EYE_CLOSE, NULL,       "屏幕锁定", "Lock",     open_aod_cb },
    { LV_SYMBOL_VOLUME_MAX, NULL,      "语音",     "Voice",    placeholder_cb },
    { LV_SYMBOL_IMAGE,     NULL,       "相册",     "Album",    goto_album_cb },
};

/* 当前语言的菜单项标签 */
static const char * menu_item_label(int idx)
{
    if (idx < 0 || idx >= (int)(sizeof(s_items)/sizeof(s_items[0]))) return "";
    return g_app_language ? s_items[idx].label_en : s_items[idx].label_zh;
}

/* ------------------------------------------------------------------ */
/* 按钮点击防误触：滑动时取消点击                                      */
/* ------------------------------------------------------------------ */
#define MENU_BTN_CANCEL_DIST 24   /* 移动超过 24px 视为滑动，取消点击 */

typedef struct {
    lv_point_t press_pt;
    int        idx;
    uint8_t    moved : 1;
} menu_btn_track_t;

static menu_btn_track_t s_btn_tracks[8];

static void menu_btn_track_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    menu_btn_track_t *track = (menu_btn_track_t *)lv_event_get_user_data(e);
    if (!track) return;

    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;

    if (code == LV_EVENT_PRESSED) {
        lv_indev_get_point(indev, &track->press_pt);
        track->moved = 0;
    } else if (code == LV_EVENT_PRESSING) {
        lv_point_t pt;
        lv_indev_get_point(indev, &pt);
        int dx = pt.x - track->press_pt.x;
        int dy = pt.y - track->press_pt.y;
        if (dx * dx + dy * dy > MENU_BTN_CANCEL_DIST * MENU_BTN_CANCEL_DIST) {
            track->moved = 1;
        }
    } else if (code == LV_EVENT_CLICKED) {
        if (track->moved) {
            /* 滑动过程中释放，不触发点击 */
            LV_LOG_USER("Menu btn %d click cancelled (swipe)", track->idx);
            lv_event_stop_bubbling(e);
            return;
        }
        if (track->idx >= 0 && track->idx < (int)(sizeof(s_items)/sizeof(s_items[0]))) {
            s_items[track->idx].cb(e);
        }
    }
}

lv_obj_t * page_menu_create(lv_obj_t *tv)
{
    // #region debug-point E:menu
    dbg_log("E", "page_menu.c:page_menu_create", "start");
    // #endregion
    /* 功能菜单 tile (0,0)：tileview 原生滑动导航
     * LV_DIR_BOTTOM = 向下滑 → home (0,1) */
    lv_obj_t *tile = lv_tileview_add_tile(tv, 0, 0, LV_DIR_BOTTOM);
    lv_obj_set_style_bg_color(tile, lv_color_hex(COLOR_BG), 0);
    /* tile 自身不要内边距，由内部容器控制；否则横屏(480 高)的可用
     * 空间被挤掉一截（16*800/480 ≈ 27px 上下共 54px 浪费）。 */
    lv_obj_set_style_pad_all(tile, 0, 0);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

    /* 根据屏幕方向选择列数 + 边距：
     *   竖屏(480x800)  → 2 列 × 4 行 → 卡片约 215×180
     *   横屏(800x480)  → 4 列 × 2 行 → 卡片约 184×200
     * 两方向均接近正方形，且横屏用更小内边距保留更多高度给图标。
     */
    const bool landscape = ui_is_landscape();
    const int cols = landscape ? 4 : 2;
    const int rows = 8 / cols;

    const int tile_pad  = landscape ? ui_scale_min(10) : ui_scale_min(16);
    const int card_gap  = landscape ? ui_scale_min(8)  : ui_scale_min(12);
    const int card_rad  = landscape ? ui_scale_min(10) : ui_scale_min(14);

    /* 网格容器：占满整个 tile（不再显示顶部“功能菜单”标题，避免与数字/指示器重叠） */
    lv_obj_t *grid = lv_obj_create(tile);
    if (landscape) {
        lv_obj_set_size(grid, lv_pct(100), lv_pct(100));
    } else {
        /* 竖屏：标题占顶部 ~36px，网格占剩余 ~764px */
        lv_obj_set_size(grid, lv_pct(100), lv_pct(100));
        lv_obj_align(grid, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, tile_pad, 0);
    lv_obj_set_style_pad_gap(grid, card_gap, 0);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    /* 使用 static 数组避免 VLA 释放后 LVGL 悬空指针 */
    static lv_coord_t col_dsc_4[5] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static lv_coord_t row_dsc_2[3] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static lv_coord_t col_dsc_2[3] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static lv_coord_t row_dsc_4[5] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};

    const lv_coord_t *col_dsc = (cols == 4) ? col_dsc_4 : col_dsc_2;
    const lv_coord_t *row_dsc = (rows == 2) ? row_dsc_2 : row_dsc_4;
    lv_obj_set_grid_dsc_array(grid, col_dsc, row_dsc);

    /* 菜单图标统一使用 40px 符号字体，在 200px 级卡片中更饱满 */
    const lv_font_t *menu_icon_font = &lv_font_montserrat_40;

    for (int i = 0; i < 8; i++) {
        lv_obj_t *btn = lv_button_create(grid);
        lv_obj_set_grid_cell(btn,
                             LV_GRID_ALIGN_STRETCH, i % cols, 1,
                             LV_GRID_ALIGN_STRETCH, i / cols, 1);
        lv_obj_set_style_bg_color(btn, lv_color_hex(COLOR_PANEL), 0);
        lv_obj_set_style_radius(btn, card_rad, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        /* 按压反馈：整体轻微缩小 */
        ui_attach_press_scale(btn, 230);

        /* 卡片内部用 FLEX_COLUMN + SPACE_EVENLY：图标与标签作为整体
         * 在卡片垂直方向均衡分布，消除"图标悬顶、标签贴底、中间空洞"的
         * 留白问题，横竖屏都视觉居中。 */
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(btn,
                              LV_FLEX_ALIGN_SPACE_EVENLY,   /* 主轴(垂直)均衡分布 */
                              LV_FLEX_ALIGN_CENTER,           /* 交叉轴(水平)居中 */
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_top(btn, ui_scale_min(8), 0);
        lv_obj_set_style_pad_bottom(btn, ui_scale_min(8), 0);
        lv_obj_set_style_pad_left(btn, ui_scale_min(6), 0);
        lv_obj_set_style_pad_right(btn, ui_scale_min(6), 0);

        /* Icon：自然高度，水平居中 */
        lv_obj_t *ic = lv_label_create(btn);
        lv_label_set_text(ic, s_items[i].icon);
        lv_obj_set_style_text_font(ic,
            s_items[i].icon_font ? s_items[i].icon_font : menu_icon_font, 0);
        lv_obj_set_style_text_color(ic, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_set_style_text_align(ic, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(ic, lv_pct(100));

        /* Label：自然高度，水平居中；限定宽度防止长文字撑破卡片 */
        lv_obj_t *lb = lv_label_create(btn);
        lv_label_set_text(lb, menu_item_label(i));
        lv_obj_set_style_text_font(lb, ui_font_cjk(), 0);
        lv_obj_set_style_text_color(lb, lv_color_hex(COLOR_SECONDARY), 0);
        lv_obj_set_width(lb, lv_pct(100));
        lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);

        /* 用 press tracker 包装点击回调，避免滑动时被识别为点击 */
        s_btn_tracks[i].idx = i;
        s_btn_tracks[i].moved = 0;
        lv_obj_add_event_cb(btn, menu_btn_track_cb, LV_EVENT_PRESSED, &s_btn_tracks[i]);
        lv_obj_add_event_cb(btn, menu_btn_track_cb, LV_EVENT_PRESSING, &s_btn_tracks[i]);
        lv_obj_add_event_cb(btn, menu_btn_track_cb, LV_EVENT_CLICKED, &s_btn_tracks[i]);
    }

    /* 左滑进入存储设置页 */
    ui_attach_gesture(tile, LV_DIR_LEFT, on_swipe_left_storage);

    return tile;
}

/* 旋转 / 语言切换重建 UI 时调用。
 * 当前 menu 页没有需要手动释放的静态 LVGL 句柄（tile 由调用方保存并在
 * lv_obj_delete(g_scr) 时自动释放），但保留统一 destroy 入口，便于后续扩展
 * 并确保 app_ui_destroy() 的调用链完整。 */
void page_menu_destroy(void)
{
    /* 当前无静态句柄需要清理 */
}
