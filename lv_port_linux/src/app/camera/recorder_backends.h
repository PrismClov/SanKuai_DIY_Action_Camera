/**
 * @file recorder_backends.h
 * @brief 录制后端内部接口（仅 recorder_hal.c 和各后端实现使用）
 *
 * 架构：
 *   recorder_hal.c（公共 API 分发层）
 *     ├─ recorder_mjpeg.c（软件后端，MJPEG+MP4，全平台可用）
 *     └─ recorder_rkmpp.c（硬件后端，H.264+MP4，仅 RK3566）
 *
 * 公共 API（recorder_create 等）在 recorder_hal.c 中实现，
 * 内部根据 board_detect + dlopen 探测选择后端。
 */
#ifndef RECORDER_BACKENDS_H
#define RECORDER_BACKENDS_H

#include "recorder_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* MJPEG 软件后端（全平台可用）                                         */
/* ------------------------------------------------------------------ */

recorder_handle_t * recorder_mjpeg_create(const recorder_config_t *cfg);
void                recorder_mjpeg_destroy(recorder_handle_t *rec);
int                 recorder_mjpeg_start(recorder_handle_t *rec);
int                 recorder_mjpeg_stop(recorder_handle_t *rec);
int                 recorder_mjpeg_feed_frame(recorder_handle_t *rec, const camera_frame_t *frame);
recorder_state_t    recorder_mjpeg_get_state(const recorder_handle_t *rec);
uint64_t            recorder_mjpeg_get_elapsed_ms(const recorder_handle_t *rec);
int                 recorder_mjpeg_is_available(void);
int                 recorder_mjpeg_get_max_capability(int *out_w, int *out_h, int *out_fps);

/* ------------------------------------------------------------------ */
/* RK MPP 硬件后端（仅 aarch64 平台编译，其他平台为空实现）             */
/* ------------------------------------------------------------------ */

recorder_handle_t * recorder_rkmpp_create(const recorder_config_t *cfg);
void                recorder_rkmpp_destroy(recorder_handle_t *rec);
int                 recorder_rkmpp_start(recorder_handle_t *rec);
int                 recorder_rkmpp_stop(recorder_handle_t *rec);
int                 recorder_rkmpp_feed_frame(recorder_handle_t *rec, const camera_frame_t *frame);
recorder_state_t    recorder_rkmpp_get_state(const recorder_handle_t *rec);
uint64_t            recorder_rkmpp_get_elapsed_ms(const recorder_handle_t *rec);

/* RK MPP 后端可用性检测：
 * - aarch64 平台：尝试 dlopen librockchip_mpp.so，成功返回 1
 * - 其他平台：始终返回 0（编译为空实现） */
int                 recorder_rkmpp_is_available(void);

/* RK MPP 后端最大能力（H.264 硬件编码，支持 4K60） */
int                 recorder_rkmpp_get_max_capability(int *out_w, int *out_h, int *out_fps);

#ifdef __cplusplus
}
#endif

#endif /* RECORDER_BACKENDS_H */
