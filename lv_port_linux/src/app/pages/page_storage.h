/**
 * @file page_storage.h
 * @brief 左滑存储查看页（参考图1）
 */
#ifndef PAGE_STORAGE_H
#define PAGE_STORAGE_H

#include "lvgl/lvgl.h"

lv_obj_t * page_storage_create(lv_obj_t *tv);
void page_storage_refresh(int sd);
void page_storage_tick(void);

#endif /* PAGE_STORAGE_H */
