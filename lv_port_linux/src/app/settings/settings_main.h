/**
 * @file settings_main.h
 * @brief 系统设置主页（卡片式入口）
 */
#ifndef SETTINGS_MAIN_H
#define SETTINGS_MAIN_H

#include "lvgl/lvgl.h"

void settings_page_create(void);
void settings_page_open(void);
void settings_page_close(void);
void settings_page_destroy(void);

#endif /* SETTINGS_MAIN_H */
