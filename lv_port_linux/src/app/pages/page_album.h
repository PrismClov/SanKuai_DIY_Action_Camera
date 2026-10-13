/**
 * @file page_album.h
 * @brief 相册查看页面（替换 storage 在 tileview 1,1）
 *
 * 功能：
 *   - 照片缩略图网格（3列/2列切换）
 *   - 点击照片全屏查看
 *   - 左上角浮动返回箭头
 *   - 右滑回主页（tileview 原生导航）
 */
#ifndef PAGE_ALBUM_H
#define PAGE_ALBUM_H

#include "lvgl/lvgl.h"

lv_obj_t * page_album_create(lv_obj_t *tv);
void page_album_destroy(void);
/* 刷新相册：重新扫描目录并重建网格。
 * 在从主页进入相册时调用，确保新拍摄的照片/视频立即可见 */
void page_album_refresh(void);

#endif /* PAGE_ALBUM_H */
