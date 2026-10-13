/**
 * @file page_aod.h
 * @brief 熄屏显示页面（Always-On Display / 屏幕锁定）
 *
 * 入口：功能菜单点击“屏幕锁定”进入。
 * 交互：从底部向上滑动返回主屏幕。
 */
#ifndef PAGE_AOD_H
#define PAGE_AOD_H

#include "lvgl/lvgl.h"

/* 熄屏显示样式 */
#define AOD_STYLE_STORAGE 0   /* 样式一：可用存储容量 + 拍摄模式/参数 */
#define AOD_STYLE_TIME    1   /* 样式二：系统时间 + 拍摄模式/参数 */

void page_aod_create(void);
void page_aod_open(void);
void page_aod_close(void);
void page_aod_destroy(void);

/* 设置当前样式（由 settings_aod 调用） */
void page_aod_set_style(int style);
int  page_aod_get_style(void);

#endif /* PAGE_AOD_H */
