/**
 * @file page_menu.h
 * @brief 上滑功能选择页（参考图5）
 */
#ifndef PAGE_MENU_H
#define PAGE_MENU_H

#include "lvgl/lvgl.h"

lv_obj_t * page_menu_create(lv_obj_t *tv);
void page_menu_back_gesture_cb(lv_event_t *e);

/* 旋转 / 语言切换重建 UI 时调用。
 * menu tile 本身由 lv_obj_delete(g_scr) 自动释放，但保留 destroy 入口可统一
 * 清理未来可能增加的静态句柄，保持与其他页面一致的销毁契约。 */
void page_menu_destroy(void);

#endif /* PAGE_MENU_H */
