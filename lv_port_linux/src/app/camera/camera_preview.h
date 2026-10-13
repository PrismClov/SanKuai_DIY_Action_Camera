/**
 * @file camera_preview.h
 * @brief 摄像头预览后台线程模块（性能优化）
 *
 * 将摄像头帧采集 + YUV→RGB565 转换 + 录制器喂帧 从 UI 线程移到独立 worker 线程，
 * 彻底消除转换耗时（K230 软件 YUV→RGB565 约 8~12ms/帧）对 UI 响应的阻塞。
 *
 * 架构：3 缓冲区 + 帧跳过（drop-if-not-consumed）零拷贝方案
 *
 *   ┌──────────┐  acquire+convert   ┌──────────┐
 *   │  V4L2     │ ──────────────────→ │ worker   │ 独占写入
 *   │  camera   │                     │ buffer   │
 *   └──────────┘                     └────┬─────┘
 *                                           │ swap（仅当 UI 已消费）
 *                                           ▼
 *   ┌──────────┐  lv_canvas_set_buffer  ┌──────────┐
 *   │  LVGL    │ ←──────────────────── │ display  │ UI 读取（零拷贝）
 *   │  canvas  │                       │ buffer   │
 *   └──────────┘                       └──────────┘
 *                                           ▲
 *   ┌──────────┐                           │ 中转（LVGL 旧 render 完成后回收）
 *   │  free    │ ←─────────────────────────┘
 *   │  buffer  │
 *   └──────────┘
 *
 * 线程安全分析：
 *   - worker 线程独占 worker buffer，写入转换结果
 *   - UI 线程通过 lv_canvas_set_buffer 直接使用 display buffer（零拷贝）
 *   - free buffer 作为中转，确保 LVGL 仍在渲染的旧 display 不会被 worker 覆盖
 *   - 帧跳过：若 UI 尚未消费上一帧（new_frame_ready=1），worker 丢弃当前帧不交换。
 *     这保证两次 swap 之间 UI 必定消费过一次，LVGL 渲染中的 buffer 不会变成 worker buffer。
 *
 * 使用流程：
 *   camera_hal_t *cam = camera_open_default(...);
 *   camera_start(cam);
 *   camera_preview_t *pv = camera_preview_create(cam, 800, 480, CAM_ROTATE_180);
 *   camera_preview_start(pv);
 *   // UI 定时器中：
 *   if (camera_preview_consume_new_frame(pv)) {
 *       const uint16_t *buf = camera_preview_get_display(pv);
 *       lv_canvas_set_buffer(canvas, (void*)buf, w, h, LV_COLOR_FORMAT_RGB565);
 *       lv_obj_invalidate(canvas);
 *   }
 *   camera_preview_stop(pv);
 *   camera_preview_destroy(pv);
 */
#ifndef CAMERA_PREVIEW_H
#define CAMERA_PREVIEW_H

#include "camera_hal.h"
#include "recorder_hal.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct camera_preview camera_preview_t;

/* 创建预览工作线程模块（不立即启动）。
 * 参数：
 *   cam    - 已打开的摄像头句柄（调用方负责生命周期，preview 不 close 它）
 *   dst_w  - 显示缓冲区宽度（通常 = 屏幕宽度）
 *   dst_h  - 显示缓冲区高度（通常 = 屏幕高度）
 *   rotate - 旋转角度（CAM_ROTATE_0/90/180/270）
 * 返回 NULL 失败。 */
camera_preview_t * camera_preview_create(camera_hal_t *cam,
                                          int dst_w, int dst_h,
                                          camera_rotate_t rotate);

/* 停止工作线程并释放资源（不 close 摄像头） */
void camera_preview_destroy(camera_preview_t *pv);

/* 启动工作线程。返回 0 成功，负数失败。 */
int camera_preview_start(camera_preview_t *pv);

/* 停止工作线程（阻塞直到 worker 退出，通常 < 1 帧时间 ~33ms） */
void camera_preview_stop(camera_preview_t *pv);

/* 更新旋转角度（线程安全，下一帧生效） */
void camera_preview_set_rotate(camera_preview_t *pv, camera_rotate_t rotate);

/* 设置画幅裁剪比例（线程安全，下一帧生效）。
 * crop_ratio: 0=4:3, 1=16:9, 2=1:1, -1=不裁剪（整帧缩放）
 * 预览 worker 转换时按画幅裁剪源帧中心区域，保证预览与保存照片内容一致。
 * 裁剪逻辑与 photo_saver.c 的 rgb565_center_crop 一致 */
void camera_preview_set_crop_ratio(camera_preview_t *pv, int crop_ratio);

/* 获取当前 display 缓冲区指针（线程安全，非阻塞）。
 * 该指针指向 worker 最近完成转换的 RGB565 帧。
 * UI 线程可用 lv_canvas_set_buffer 直接使用此指针（零拷贝）。
 * 返回 NULL 如果尚未有帧转换完成（此时 buffer 全黑）。
 * 指针在下次 camera_preview_get_display 调用前有效。 */
const uint16_t * camera_preview_get_display(camera_preview_t *pv);

/* 检查并消费 "新帧就绪" 标志。
 * 返回 1 如果自上次调用以来有新帧，0 表示无新帧。
 * 调用后标志自动清零，允许 worker 交换下一帧。 */
int camera_preview_consume_new_frame(camera_preview_t *pv);

/* 设置/清除录制器句柄（线程安全）。
 * worker 线程在每帧 acquire 后调用 recorder_feed_frame。
 * rec=NULL 时停止喂帧。
 * 调用方负责 recorder 的生命周期，preview 不 destroy 它。 */
void camera_preview_set_recorder(camera_preview_t *pv, recorder_handle_t *rec);

/* 设置录制开关（线程安全）。
 * recording=1 时 worker 喂帧给 recorder，=0 时不喂。 */
void camera_preview_set_recording(camera_preview_t *pv, int recording);

/* ======================== 全分辨率拍照快照 ========================
 *
 * 为什么需要：预览 buffer 是降采样后的屏幕分辨率（如 480x800≈0.4MP），
 * 直接拿来做拍照保存会严重损失画质（放大到 12MP 极度模糊）。
 * 拍照应使用原始分辨率（1920x1080≈2MP）的帧，仅做旋转不做降采样。
 *
 * 机制：worker 线程收到请求时，在转换预览帧的同时，把当前帧额外
 * 转换一份"全分辨率 RGB565"到 photo buffer。转换完成后通过回调
 * 通知 UI 线程，UI 线程在 camera_timer_cb 中取走 photo buffer
 * 启动异步 JPEG 编码保存。
 *
 * 内存：photo_buf 大小 = 原始帧旋转后的尺寸（约 4MB@1080p），
 * 仅在 take_photo 调用后到 consume_photo 期间有效，长期占用可接受。
 *
 * 线程安全：take_photo 设置原子请求标志，worker 检测后处理；
 * consume_photo 在 UI 线程调用，读取后清零 ready 标志。 */

/* 拍照快照请求：设置标志，worker 在下一帧转换时执行全分辨率转换。
 * 旋转角度与预览一致（避免预览/保存方向不一致）。
 * 返回 0 请求已接受，负数失败（preview 未启动或上次拍照未取走） */
int camera_preview_take_photo(camera_preview_t *pv);

/* 检查拍照快照是否就绪（UI 线程调用，非阻塞）。
 * 返回 1=就绪（可调用 camera_preview_consume_photo 取走） */
int camera_preview_is_photo_ready(camera_preview_t *pv);

/* 取走全分辨率 photo buffer（UI 线程调用）。
 * out_buf/out_w/out_h 输出 photo buffer 指针和尺寸。
 * 调用后内部 ready 标志清零，允许下次 take_photo。
 * 注意：返回的指针指向 preview 内部 buffer，下次 take_photo 时会被覆盖，
 *       调用方应在下次 take_photo 前完成使用（如 memcpy 到本地）。
 * 返回 0 成功，负数无就绪帧 */
int camera_preview_consume_photo(camera_preview_t *pv,
                                   const uint16_t **out_buf,
                                   int *out_w, int *out_h);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_PREVIEW_H */
