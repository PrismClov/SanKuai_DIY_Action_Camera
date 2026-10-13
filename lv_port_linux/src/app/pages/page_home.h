/**
 * @file page_home.h
 * @brief 主屏幕（相机风格 UI）
 */
#ifndef PAGE_HOME_H
#define PAGE_HOME_H

#include "lvgl/lvgl.h"

lv_obj_t * page_home_create(lv_obj_t *tv);
void page_home_destroy(void);
void page_home_refresh(void);
void page_home_set_param(const char *res, const char *fps);
void page_home_set_zoom(const char *zoom);
void page_home_update_storage(void);

/* 压力测试入口：控制模式弹层打开/关闭 */
void page_home_stress_mode_overlay(int open);
void page_home_stress_param_overlay(int open);
void page_home_stress_zoom_overlay(int open);

const char ** page_home_res_opts(void);
const char ** page_home_fps_opts(void);
int * page_home_sel_res(void);
int * page_home_sel_fps(void);

/* 供快捷参数页读取拍照模式选项 */
const char ** page_home_ratio_opts(void);
const char ** page_home_photosize_opts(void);
const char ** page_home_burst_opts(void);
int * page_home_sel_ratio(void);
int * page_home_sel_photosize(void);
int * page_home_sel_burst(void);
void page_home_photo_param_changed(void);

/* 供熄屏显示页读取当前拍摄状态 */
int    page_home_get_mode(void);                 /* 当前模式索引 */
int    page_home_is_recording(void);             /* 是否正在录制 */
void   page_home_get_rec_time(char *buf, size_t sz); /* 已录制时间 MM:SS */
void   page_home_get_mode_param_text(char *buf, size_t sz); /* 模式图标 + 参数文本 */

#endif /* PAGE_HOME_H */
