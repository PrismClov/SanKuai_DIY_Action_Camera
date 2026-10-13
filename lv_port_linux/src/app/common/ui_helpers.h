/**
 * @file ui_helpers.h
 * @brief 公共 UI 辅助函数、主题色、字体
 */
#ifndef UI_HELPERS_H
#define UI_HELPERS_H

#include "lvgl/lvgl.h"

/* ------------------------------------------------------------------ */
/* theme colors                                                         */
/* ------------------------------------------------------------------ */
#define COLOR_BG        0x050505
#define COLOR_PANEL     0x181818
#define COLOR_ACCENT    0xFFBF00
#define COLOR_TEXT      0xFFFFFF
#define COLOR_SECONDARY 0xAAAAAA
#define COLOR_YELLOW    0xFFC107  /* SD 卡选中态黄色 */
#define COLOR_YELLOW_BD 0xFFD600  /* SD 卡选中态黄色边框 */

/* ------------------------------------------------------------------ */
/* 全局方向标志（独立于 ui_helpers 之外使用）                            */
/* ------------------------------------------------------------------ */
extern int g_app_landscape;

/* ------------------------------------------------------------------ */
/* fonts                                                                */
/* ------------------------------------------------------------------ */
void ui_font_load(void);
void ui_font_unload(void);                /* 释放 FreeType 字体，必须在所有 UI 对象销毁后调用 */
const lv_font_t * ui_font_cjk(void);      /* 20px CJK 标准字体 */
const lv_font_t * ui_font_cjk_lg(void);   /* 28px CJK 大号字体（滚轮/标题） */
const lv_font_t * ui_font_sym(void);

/* ------------------------------------------------------------------ */
/* screen size helpers                                                  */
/* ------------------------------------------------------------------ */
int  ui_get_screen_width(void);
int  ui_get_screen_height(void);
int  ui_is_landscape(void);
int  ui_scale(int base);       /* 根据屏幕宽度缩放数值（基准 480） */
int  ui_scale_min(int base);   /* 根据 min(宽, 高) 缩放，保证正方形 */
int  ui_scale_w(int base);     /* 根据屏幕宽度缩放 */
int  ui_scale_h(int base);     /* 根据屏幕高度缩放 */

/* 横竖屏布局辅助：宽屏（横屏 800px）下限制内容最大宽度并居中显示，
 * 窄屏（竖屏 480px）下全屏宽度。用于设置子页面横竖屏统一适配。 */
bool ui_is_wide_layout(void);          /* 宽屏返回 true（屏幕宽 >= 720） */
int  ui_get_content_max_width(void);   /* 宽屏返回 720（像素），窄屏返回 lv_pct(100) */

/* ------------------------------------------------------------------ */
/* common widgets                                                       */
/* ------------------------------------------------------------------ */
/* 通用 header：左侧返回箭头(back_cb)、中间标题(title)、右侧×关闭按钮(home_cb)。
 * back_cb  —— 返回上一级（关闭当前页/返回上级菜单）
 * home_cb  —— 返回主屏幕；为 NULL 时不显示 × 按钮（右侧用等宽占位保持标题居中） */
lv_obj_t * ui_make_header(lv_obj_t *parent, const char *title,
                          lv_event_cb_t back_cb, void *back_user_data,
                          lv_event_cb_t home_cb, void *home_user_data);

lv_obj_t * ui_create_card(lv_obj_t *parent, const char *icon,
                          const lv_font_t *icon_font,
                          const char *title, const char *subtitle,
                          lv_event_cb_t cb);

lv_obj_t * ui_info_row(lv_obj_t *parent, const char *label);

void ui_open_sub_page(lv_obj_t *overlay);

/* 右滑返回手势：调用方提供 close_cb 用于关闭当前页面 */
void ui_attach_back_gesture(lv_obj_t *obj, lv_event_cb_t close_cb);

/* 右滑关闭手势（LV_DIR_RIGHT）：用于全屏页面右滑退出 */
void ui_attach_right_gesture(lv_obj_t *obj, lv_event_cb_t close_cb);

/* 多方向手势返回：dirs 为 LV_DIR_* 的组合（如 LV_DIR_LEFT | LV_DIR_BOTTOM） */
void ui_attach_gesture(lv_obj_t *obj, lv_dir_t dirs, lv_event_cb_t close_cb);

/* 双击事件：两次点击间隔小于阈值时触发 cb */
void ui_attach_double_click(lv_obj_t *obj, lv_event_cb_t cb, void *user_data);

/* 长按事件：press_time_ms 为 0 时使用 LVGL 默认长按时间 */
void ui_attach_long_press(lv_obj_t *obj, uint16_t press_time_ms,
                          lv_event_cb_t cb, void *user_data);

/* 按钮按下缩放反馈：按下时整体缩小到 scale/256 倍，释放后平滑恢复。
 * 典型值：scale=230（0.9x），scale=242（0.95x）。 */
void ui_attach_press_scale(lv_obj_t *obj, int scale_pressed_256);

/* overlay 淡入显示：调用前需先 lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN)。
 * 时间典型值 150~250ms。 */
void ui_fade_in_overlay(lv_obj_t *obj, uint32_t time_ms);

/* ------------------------------------------------------------------ */
/* system helpers                                                       */
/* ------------------------------------------------------------------ */
void ui_read_file_line(const char *path, char *buf, size_t sz);
void ui_read_storage(const char *path, long long *total_mb,
                     long long *used_mb, int *ok);
int  ui_detect_sd_card(long long *total_mb, long long *used_mb,
                       char *out_path, size_t sz);
int  ui_detect_internal_storage(long long *total_mb, long long *used_mb);

#endif /* UI_HELPERS_H */
