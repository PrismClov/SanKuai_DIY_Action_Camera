/**
 * @file rga_accel.h
 * @brief RK3566 RGA 硬件加速器封装（YUV→RGB565 + 缩放 + 旋转）
 *
 * 核心价值：
 *   - RGA 是 RK3566 独立的 2D 硬件加速器，与 CPU 并行工作
 *   - 一次 improcess 调用同时完成：格式转换 + 缩放 + 旋转
 *   - 1080p NV12 → 800x480 RGB565 + 90° 旋转：<2ms（软件 8~12ms）
 *   - CPU 完全释放，可专注 UI 渲染和录制编码
 *
 * 工作流程：
 *   1. V4L2 采集到 NV12/YUYV 帧（mmap 在内核空间）
 *   2. rga_convert_frame 注册 src/dst 缓冲区，调用 improcess
 *   3. RGA 硬件完成转换+缩放+旋转，结果直接写入 LVGL canvas 缓冲区
 *   4. CPU 全程零拷贝、零计算
 *
 * 平台支持：
 *   - RK3566：原生支持（librga.so 已在 sysroot）
 *   - K230：不支持（返回 -1，调用方回退到软件路径）
 */
#ifndef RGA_ACCEL_H
#define RGA_ACCEL_H

#include "camera_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 检测 RGA 硬件加速器是否可用
 * @return 1=可用（RK3566），0=不可用（K230 或无 librga）
 */
int rga_is_available(void);

/**
 * @brief RGA 硬件加速：YUV/RGB565 → RGB565 + 缩放 + 旋转
 *
 * 一次调用完成：
 *   - 格式转换：NV12/NV21/YUYV/RGB565 → RGB565
 *   - 缩放：src_w×src_h → dst_w×dst_h（双线性插值，硬件实现）
 *   - 旋转：0°/90°/180°/270°（硬件实现，无 CPU 开销）
 *
 * @param frame   源帧（V4L2 采集的原始帧，data 指向 mmap 缓冲区）
 * @param dst     目标 RGB565 缓冲区（通常为 LVGL canvas 缓冲区）
 * @param dst_w   目标宽度
 * @param dst_h   目标高度
 * @param rotate  旋转角度
 * @return 0=成功，负数=失败（调用方应回退到软件路径）
 */
int rga_convert_frame(const camera_frame_t *frame,
                      uint16_t *dst, int dst_w, int dst_h,
                      camera_rotate_t rotate);

#ifdef __cplusplus
}
#endif

#endif /* RGA_ACCEL_H */
