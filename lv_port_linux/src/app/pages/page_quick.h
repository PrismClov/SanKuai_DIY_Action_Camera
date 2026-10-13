/**
 * @file page_quick.h
 * @brief 上滑拍摄参数页（参考图3）
 */
#ifndef PAGE_QUICK_H
#define PAGE_QUICK_H

#include "lvgl/lvgl.h"

lv_obj_t * page_quick_create(lv_obj_t *tv);
void page_quick_refresh(void);
void page_quick_destroy(void);  /* 旋转重建 UI 时调用，重置静态句柄防止悬空 */

#endif /* PAGE_QUICK_H */
