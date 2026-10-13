/**
 * @file video_thumb.h
 * @brief 视频缩略图提取模块
 *
 * 功能：
 *   - 从视频文件（MP4/MOV/AVI/MKV 等）提取第一帧，编码为 JPEG 缩略图
 *   - 缩略图缓存到 /tmp/.lv_album_thumbs/，按视频路径+文件大小+mtime 哈希命名
 *   - 相册网格通过本模块获取视频缩略图路径，交给 LVGL JPEG 解码器显示
 *
 * 编译：
 *   依赖 FFmpeg（libavformat/libavcodec/libavutil/libswscale）。
 *   CMake 通过 find_library 检测并定义 APP_HAS_FFMPEG=1；
 *   未找到 FFmpeg 时本模块所有 API 返回失败，相册回退到视频图标占位。
 */
#ifndef VIDEO_THUMB_H
#define VIDEO_THUMB_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 获取视频文件的缩略图 JPEG 路径。
 *
 * 首次调用会提取第一帧并保存到缓存目录；已缓存且视频未改变时直接返回缓存路径。
 * 返回的字符串指向内部静态缓冲区，无需释放，但后续调用会覆盖。
 *
 * @param video_path 视频文件原始路径（不含 "A:" 等 LVGL 盘符前缀）
 * @return 缩略图 JPEG 路径（带 "A:" 前缀，可直接传给 lv_image_set_src），
 *         失败返回 NULL（调用方应使用图标占位）
 */
const char * video_thumb_get(const char *video_path);

/**
 * @brief 强制重新生成视频缩略图（忽略缓存）。
 * @param video_path 视频文件原始路径
 * @return 0 成功，负数失败
 */
int video_thumb_generate(const char *video_path);

/**
 * @brief 检查视频缩略图功能是否可用（编译期/运行时 FFmpeg 是否可加载）。
 */
int video_thumb_available(void);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_THUMB_H */
