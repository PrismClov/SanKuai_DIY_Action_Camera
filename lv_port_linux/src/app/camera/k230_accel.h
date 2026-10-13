/**
 * @file k230_accel.h
 * @brief K230 硬件加速器封装（VPU/G2D YUV→RGB565 + 缩放 + 旋转）
 *
 * 核心价值：
 *   - K230 内置 VPU 和 2D G2D 硬件加速器，与 CPU 并行工作
 *   - 一次调用同时完成：格式转换 + 缩放 + 旋转
 *   - 1080p NV12 → 800x480 RGB565 + 90° 旋转：预期 <2ms（软件 8~12ms）
 *   - CPU 完全释放，双核 C908 可专注 UI 渲染和录制编码
 *
 * 工作流程：
 *   1. V4L2 采集到 NV12/YUYV 帧（mmap 在内核空间）
 *   2. k230_accel_convert_frame 调用 MPI 2D/VPU 硬件加速
 *   3. 硬件完成转换+缩放+旋转，结果直接写入 LVGL canvas 缓冲区
 *   4. CPU 全程零拷贝、零计算
 *
 * 平台支持：
 *   - K230：原生支持（libmpi.so / libsys.so 在 K230 SDK sysroot）
 *   - RK3566：不支持（返回 -1，调用方使用 RGA 或软件路径）
 *   - PC：不支持（返回 -1，使用软件路径）
 *
 * K230 MPI API 接入说明：
 *   K230 SDK 提供 kd_mpi_* 系列 API，关键接口：
 *     - kd_mpi_sys_init() / kd_mpi_sys_exit()：MPI 系统初始化
 *     - kd_mpi_vb_*：视频缓冲池管理（VB = Video Buffer）
 *     - kd_mpi_2d_*：2D 硬件加速（G2D，格式转换+缩放+旋转）
 *     - kd_mpi_venc_*：H.264/H.265 硬件编码
 *     - kd_mpi_vo_*：Video Output 层，支持硬件 overlay plane
 *
 *   集成步骤（需 K230 SDK 实际验证）：
 *     1. 确认 K230 SDK sysroot 中存在 libmpi.so / libsys.so
 *     2. 在 k230_accel.c 中实现 kd_mpi_2d_* 调用逻辑
 *     3. CMakeLists.txt 链接 mpi / sys 库
 *     4. 真机验证性能（预期 <2ms/帧）
 */
#ifndef K230_ACCEL_H
#define K230_ACCEL_H

#include "camera_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 检测 K230 硬件加速器是否可用
 *
 * 首次调用时通过 dlopen 尝试加载 libmpi.so，成功则初始化 MPI 系统并缓存结果。
 * 后续调用直接返回缓存值，无开销。
 *
 * @return 1=可用（K230 + libmpi.so 加载成功），0=不可用
 */
int k230_accel_is_available(void);

/**
 * @brief K230 硬件加速：YUV/RGB565 → RGB565 + 缩放 + 旋转
 *
 * 一次调用完成：
 *   - 格式转换：NV12/NV21/YUYV/RGB565 → RGB565
 *   - 缩放：src_w×src_h → dst_w×dst_h（硬件双线性插值）
 *   - 旋转：0°/90°/180°/270°（硬件实现，无 CPU 开销）
 *
 * @param frame   源帧（V4L2 采集的原始帧，data 指向 mmap 缓冲区）
 * @param dst     目标 RGB565 缓冲区（通常为 LVGL canvas 缓冲区）
 * @param dst_w   目标宽度
 * @param dst_h   目标高度
 * @param rotate  旋转角度
 * @return 0=成功，负数=失败（调用方应回退到软件路径）
 */
int k230_accel_convert_frame(const camera_frame_t *frame,
                             uint16_t *dst, int dst_w, int dst_h,
                             camera_rotate_t rotate);

/**
 * @brief K230 硬件 H.264/H.265 编码（VPU）
 *
 * 用于录制功能，替代软件 MJPEG 编码。
 * 预期性能：1080p@30fps 硬件编码，CPU 零占用。
 *
 * @param frame   源帧（NV12 格式最佳，VPU 原生支持）
 * @param out_buf 输出缓冲区（H.264/H.265 码流）
 * @param out_size 输出缓冲区大小（输入）/ 实际编码大小（输出）
 * @param is_keyframe 1=强制关键帧，0=普通帧
 * @return 0=成功，负数=失败
 *
 * TODO: 待 K230 SDK 验证后实现
 */
int k230_accel_encode_h264(const camera_frame_t *frame,
                           uint8_t *out_buf, size_t *out_size,
                           int is_keyframe);

#ifdef __cplusplus
}
#endif

#endif /* K230_ACCEL_H */
