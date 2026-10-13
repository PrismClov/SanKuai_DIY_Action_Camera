/**
 * @file album_image_cache.h
 * @brief 相册图片缓存接口
 *
 * 解决相册中直接加载高分辨率照片（如 20MP+）导致 LVGL 解码 OOM 或
 * 因尺寸限制被过滤而无法显示的问题。
 *
 * 使用方式：
 *   - 缩略图：album_image_get_path(src, ALBUM_IMAGE_CACHE_THUMB)
 *   - 全屏查看：album_image_get_path(src, ALBUM_IMAGE_CACHE_PREVIEW)
 *
 * 对于本身已小于目标尺寸的图片，直接返回原路径；
 * 对于过大的 JPEG，自动解码并重新编码为缩略图/预览图缓存。
 */
#ifndef ALBUM_IMAGE_CACHE_H
#define ALBUM_IMAGE_CACHE_H

#ifdef __cplusplus
extern "C" {
#endif

/* 缓存类型：缩略图 / 全屏预览 */
#define ALBUM_IMAGE_CACHE_THUMB   0
#define ALBUM_IMAGE_CACHE_PREVIEW 1

/* 最大边长 */
#define ALBUM_IMAGE_CACHE_THUMB_MAX   512
#define ALBUM_IMAGE_CACHE_PREVIEW_MAX 1280

/**
 * @brief 返回可用于 LVGL 显示的图片路径。
 *
 * @param src_path  原图路径，支持 "A:/path" 或普通绝对路径。
 * @param cache_type ALBUM_IMAGE_CACHE_THUMB 或 ALBUM_IMAGE_CACHE_PREVIEW。
 * @return  const char*  可直接传给 lv_image_set_src 的路径（含 "A:" 前缀）。
 *          失败返回 NULL。
 *
 * 注意：返回的指针指向内部静态缓冲区，下次调用会覆盖。
 */
const char * album_image_get_path(const char *src_path, int cache_type);

#ifdef __cplusplus
}
#endif

#endif /* ALBUM_IMAGE_CACHE_H */
