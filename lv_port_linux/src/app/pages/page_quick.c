/**
 * @file page_quick.c
 * @brief 下拉设置菜单（分辨率/帧率参数选择）
 *
 * 特性：
 *   - 自适应横屏/竖屏（横屏：选项行改为多列网格）
 *   - 卡片式选项按钮（圆角、阴影、选中态高亮）
 *   - 右滑返回主页
 *   - 下拉返回主页
 */
#include "page_quick.h"
#include "page_home.h"
#include "../app_main.h"
#include "../common/ui_helpers.h"
#include "../common/app_i18n.h"
#include "../settings/settings_storage.h"

#include "lvgl/lvgl.h"

#include <stdio.h>

static lv_obj_t *g_tile;

/* ------------------------------------------------------------------ */
/* 漂亮的选项按钮（自适应大小）                                          */
/* ------------------------------------------------------------------ */
static lv_obj_t *make_option_btn(lv_obj_t *parent, const char *text, int sel)
{
    /* 自适应按钮尺寸：基于 min(宽, 高) 保证比例一致 */
    int btn_h = ui_scale_min(44);
    int radius = ui_scale_min(10);

    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_height(btn, btn_h);
    lv_obj_set_flex_grow(btn, 1);  /* 弹性增长填充剩余宽度 */
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    /* 选中和未选中样式 */
    if (sel) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(COLOR_ACCENT), 0);
        lv_obj_set_style_bg_grad_color(btn, lv_color_hex(0xFFD600), 0);
        lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_VER, 0);
        /* 性能：关闭阴影 */
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
    } else {
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x202020), 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x333333), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
    }
    lv_obj_set_style_radius(btn, radius, 0);
    lv_obj_set_style_pad_all(btn, ui_scale_min(4), 0);

    /* 按下状态 */
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x404040), LV_STATE_PRESSED);

    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(l, sel ? lv_color_black() : lv_color_white(), 0);
    /* 限制 label 宽度为按钮内容区，并设置过长时以省略号显示，
     * 避免文字因超出按钮边界被截断/压缩而显示不全。 */
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_center(l);

    return btn;
}

/* ------------------------------------------------------------------ */
/* 自适应布局的选项行                                                  */
/* ------------------------------------------------------------------ */
static lv_obj_t *make_option_row(lv_obj_t *parent, const char *title,
                                 const char **opts, int count, int sel,
                                 lv_event_cb_t cb)
{
    int gap = ui_scale_min(10);

    /* 外层容器 */
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_set_size(cont, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_style_pad_gap(cont, gap, 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    /* 标题行 */
    lv_obj_t *title_box = lv_obj_create(cont);
    lv_obj_remove_style_all(title_box);
    lv_obj_set_size(title_box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(title_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_box, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(title_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(title_box, 0, 0);
    lv_obj_remove_flag(title_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tl = lv_label_create(title_box);
    lv_label_set_text(tl, title);
    lv_obj_set_style_text_font(tl, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(tl, lv_color_hex(COLOR_SECONDARY), 0);

    /* 选中项指示 */
    lv_obj_t *ind = lv_label_create(title_box);
    lv_label_set_text_fmt(ind, "%s: %s", tr("已选", "Selected"), opts[sel]);
    lv_obj_set_style_text_font(ind, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(ind, lv_color_hex(COLOR_ACCENT), 0);

    /* 选项容器：竖屏单列 flex，横屏双列 flex row wrap */
    lv_obj_t *row = lv_obj_create(cont);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(row, gap, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    if (ui_is_landscape() && count >= 2) {
        /* 横屏：双列 flex row wrap，按钮固定宽高，避免 grid stretch 压扁文字。
         * 之前使用 LV_GRID_ALIGN_STRETCH 时，行高被 FR 均分，按钮过扁导致
         * 文字显示被压扁；改用固定 48% 宽 + 56px 高，文字居中不变形。 */
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

        for (int i = 0; i < count; i++) {
            lv_obj_t *btn = make_option_btn(row, opts[i], i == sel);
            lv_obj_set_flex_grow(btn, 0);  /* 固定尺寸，不需要 flex_grow */
            lv_obj_set_size(btn, lv_pct(48), ui_scale_min(56));
            lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
    } else {
        /* 竖屏：单列 flex */
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        for (int i = 0; i < count; i++) {
            lv_obj_t *btn = make_option_btn(row, opts[i], i == sel);
            lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
    }

    return cont;
}

/* ------------------------------------------------------------------ */
/* 回调                                                                */
/* ------------------------------------------------------------------ */
static void on_res_btn(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    int *sel = page_home_sel_res();
    const char **opts = page_home_res_opts();
    if (idx < 0 || idx >= 3) return;
    *sel = idx;
    page_home_set_param(opts[*sel], page_home_fps_opts()[*page_home_sel_fps()]);
    page_quick_refresh();
}

static void on_fps_btn(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    int *sel = page_home_sel_fps();
    const char **opts = page_home_fps_opts();
    if (idx < 0 || idx >= 5) return;
    *sel = idx;
    page_home_set_param(page_home_res_opts()[*page_home_sel_res()], opts[*sel]);
    page_quick_refresh();
}

/* 拍照模式快捷参数：画幅 */
static void on_ratio_btn(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    int *sel = page_home_sel_ratio();
    if (idx < 0 || idx >= 3) return;
    *sel = idx;
    page_home_photo_param_changed();
    page_quick_refresh();
}

/* 拍照模式快捷参数：图片大小 */
static void on_photosize_btn(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    int *sel = page_home_sel_photosize();
    if (idx < 0 || idx >= 4) return;
    *sel = idx;
    page_home_photo_param_changed();
    page_quick_refresh();
}

/* 拍照模式快捷参数：连拍 */
static void on_burst_btn(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    int *sel = page_home_sel_burst();
    if (idx < 0 || idx >= 4) return;
    *sel = idx;
    page_home_photo_param_changed();
    page_quick_refresh();
}

/* 左滑进入存储设置页 */
static void on_swipe_left_storage(lv_event_t *e)
{
    (void)e;
    settings_storage_open();
}

/* ------------------------------------------------------------------ */
/* 创建页面                                                            */
/* ------------------------------------------------------------------ */
lv_obj_t * page_quick_create(lv_obj_t *tv)
{
    // #region debug-point D:quick
    dbg_log("D", "page_quick.c:page_quick_create", "start");
    // #endregion
    /* 快速设置 tile (0,2)：tileview 原生滑动导航
     * LV_DIR_TOP = 向上滑 → home (0,1) */
    g_tile = lv_tileview_add_tile(tv, 0, 2, LV_DIR_TOP);
    lv_obj_set_style_bg_color(g_tile, lv_color_hex(COLOR_BG), 0);

    /* 使用顶部安全距 + 弹性布局 */
    int pad = ui_scale_min(16);
    lv_obj_set_style_pad_top(g_tile, pad, 0);
    lv_obj_set_style_pad_bottom(g_tile, pad, 0);
    lv_obj_set_style_pad_left(g_tile, pad, 0);
    lv_obj_set_style_pad_right(g_tile, pad, 0);
    lv_obj_set_flex_flow(g_tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_tile, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(g_tile, ui_scale_min(20), 0);
    lv_obj_remove_flag(g_tile, LV_OBJ_FLAG_SCROLLABLE);

    /* tileview 原生滑动处理返回导航，无需手动 GESTURE 回调 */

    /* 左滑进入存储设置页 */
    ui_attach_gesture(g_tile, LV_DIR_LEFT, on_swipe_left_storage);

    page_quick_refresh();
    return g_tile;
}

void page_quick_refresh(void)
{
    if (!g_tile) return;
    lv_obj_clean(g_tile);

    /* 根据当前拍摄模式切换快捷参数：
     * 拍照/夜景/延时显示画幅、图片大小、连拍；
     * 视频/慢动作/跟随显示分辨率、帧率。
     * 需与 page_home 的参数面板保持一致。 */
    int mode = page_home_get_mode();
    int is_photo_mode = (mode == 0 || mode == 2 || mode == 5);

    if (is_photo_mode) {
        make_option_row(g_tile, tr("画幅", "Aspect Ratio"),
                        page_home_ratio_opts(), 3, *page_home_sel_ratio(), on_ratio_btn);
        make_option_row(g_tile, tr("图片大小", "Photo Size"),
                        page_home_photosize_opts(), 4, *page_home_sel_photosize(), on_photosize_btn);
        make_option_row(g_tile, tr("连拍参数", "Burst"),
                        page_home_burst_opts(), 4, *page_home_sel_burst(), on_burst_btn);
    } else {
        make_option_row(g_tile, tr("分辨率", "Resolution"),
                        page_home_res_opts(), 3, *page_home_sel_res(), on_res_btn);
        make_option_row(g_tile, tr("帧率 fps", "Frame Rate fps"),
                        page_home_fps_opts(), 5, *page_home_sel_fps(), on_fps_btn);
    }
}

/* 旋转 / 语言切换重建 UI 时调用。
 * 实际 LVGL 对象由 app_ui_destroy() 递归删除 g_scr->tileview->g_tile 时自动释放，
 * 此处只重置静态指针避免悬空引用导致 page_quick_refresh() 等后续调用误用旧指针。
 *
 * 注意：page_quick_refresh() 在 lv_task_handler 周期可能被 tileview 事件触发，
 * 若不重置 g_tile，访问已删除的 tileview 节点会触发 use-after-free 死机。 */
void page_quick_destroy(void)
{
    g_tile = NULL;
}
