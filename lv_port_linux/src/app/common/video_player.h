/**
 * @file video_player.h
 * @brief 全屏视频播放器模块（基于 FFmpeg）
 *
 * 功能：
 *   - 点击视频缩略图后进入全屏播放
 *   - 点击画面切换 播放/暂停
 *   - 左上角返回按钮或右滑手势关闭播放器，回到相册主页面
 *   - 自动适应横竖屏，保持视频宽高比并黑边填充
 *
 * 编译：
 *   依赖 FFmpeg（libavformat/libavcodec/libavutil/libswscale）。
 *   CMake 通过 find_library 检测并定义 APP_HAS_FFMPEG=1。
 */
#ifndef VIDEO_PLAYER_H
#define VIDEO_PLAYER_H

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建并打开全屏视频播放器。
 *
 * @param parent        父对象（通常为相册 tile）
 * @param video_path    视频文件原始路径（不含 "A:" 等 LVGL 盘符前缀）
 * @param on_close_cb   关闭回调，播放器关闭时调用（可在此切回相册 tile）
 * @param user_data     关闭回调用户数据
 */
void video_player_open(lv_obj_t *parent, const char *video_path,
                       void (*on_close_cb)(void *), void *user_data);

/**
 * @brief 主动关闭视频播放器（释放所有资源）。
 */
void video_player_close(void);

/**
 * @brief 检查视频播放功能是否可用。
 */
int video_player_available(void);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_PLAYER_H */
