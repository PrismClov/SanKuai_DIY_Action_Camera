/**
 * @file photo_saver.h
 * @brief 拍照保存模块：RGB565 → JPEG 编码 + 文件保存
 *
 * 将摄像头预览帧（RGB565）编码为 JPEG 文件保存到指定目录。
 * 文件名规则：IMG_YYYYMMDD_HHMMSS.jpg（避免覆盖）
 *
 * 依赖 libjpeg（buildroot 默认提供）。libjpeg 不可用时回退为 BMP。
 *
 * 使用流程：
 *   photo_save_jpeg(rgb565_buf, 800, 480, "/mnt/sdcard/DCIM");
 */
#ifndef PHOTO_SAVER_H
#define PHOTO_SAVER_H

#include <stdint.h>

/* 将 RGB565 帧保存为 JPEG 文件（自动生成带时间戳的文件名）
 * 参数：
 *   rgb565  - RGB565 像素数据（width * height * 2 字节）
 *   width   - 图像宽度（像素）
 *   height  - 图像高度（像素）
 *   dir     - 保存目录（如 "/mnt/sdcard/DCIM"），不存在时自动创建
 * 返回：0 成功，负数失败 */
int photo_save_jpeg(const uint16_t *rgb565, int width, int height, const char *dir);

/* 将 RGB565 帧按指定画幅裁剪后缩放到目标尺寸保存为 JPEG。
 * 用于拍照模式实现"画幅+图片大小"参数：
 *   1. 从源图像中心按 target_ratio 裁剪出最大区域
 *      - 0=4:3, 1=16:9, 2=1:1
 *   2. 将裁剪结果缩放到 target_w x target_h（对应图片大小档位）
 *   3. 编码为 JPEG 保存到 dir
 * 若 target_w/target_h <= 0，跳过缩放，按裁剪后原尺寸保存。
 * 返回：0 成功，负数失败 */
int photo_save_jpeg_scaled(const uint16_t *rgb565, int src_w, int src_h,
                            int target_ratio, int target_w, int target_h,
                            const char *dir);

/* 获取上次保存的文件完整路径（用于日志/UI提示） */
const char * photo_last_path(void);

/* 设置连拍序号（用于连拍模式下避免文件名冲突）。
 * seq > 0 时，生成的文件名追加 _NN 后缀（如 IMG_20260801_120000_03.jpg）。
 * seq = 0 时恢复正常命名（无序号后缀）。
 * 应在每次连拍序列开始前调用 */
void photo_set_burst_seq(int seq);

/* ======================== 异步拍照（后台线程编码） ======================== */

/* 异步保存照片：拷贝源帧到独立缓冲区 → 启动后台线程编码 JPEG。
 * 立即返回，不阻塞 UI 线程。
 * 编码完成后，photo_last_path() 返回最新路径。
 * 若上次异步保存仍在进行中，返回 -2（调用方可跳过或等待）。
 * 参数同 photo_save_jpeg_scaled() */
int photo_save_jpeg_scaled_async(const uint16_t *rgb565, int src_w, int src_h,
                                  int target_ratio, int target_w, int target_h,
                                  const char *dir);

/* 检查异步拍照是否正在进行中。
 * 返回 1=正在编码，0=空闲 */
int photo_is_busy(void);

/* 检查异步拍照是否已完成（结果可用）。
 * 返回 1=已完成（可读取 photo_last_path），0=尚未完成。
 * 读取后内部标志自动清零。 */
int photo_async_done(void);

#endif /* PHOTO_SAVER_H */
