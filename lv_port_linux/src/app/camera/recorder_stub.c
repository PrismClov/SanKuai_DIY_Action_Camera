/**
 * @file recorder_stub.c
 * @brief 录制后端占位实现（所有接口返回失败，等待真实后端接入）
 *
 * 当前状态：录制功能未实现，所有函数返回失败，UI 显示「录制不可用」。
 * 后续接入 RK MPP / K230 VPU 后，替换本文件为真实后端实现即可。
 *
 * 接入步骤：
 *   1. 实现 recorder_create/start/stop/feed_frame/destroy
 *   2. recorder_is_available 改为返回 1
 *   3. recorder_get_max_capability 返回真实硬件能力
 *   4. 在 CMakeLists.txt 中替换 recorder_stub.c 为真实后端源文件
 */
#include "recorder_hal.h"
#include "../app_main.h"

/* 后端未实现，所有接口返回失败 */

recorder_handle_t * recorder_create(const recorder_config_t *cfg)
{
    (void)cfg;
    LV_LOG_WARN("录制后端未实现（stub），recorder_create 返回 NULL");
    return NULL;
}

void recorder_destroy(recorder_handle_t *rec)
{
    (void)rec;
}

int recorder_start(recorder_handle_t *rec)
{
    (void)rec;
    return -1;
}

int recorder_stop(recorder_handle_t *rec)
{
    (void)rec;
    return -1;
}

int recorder_feed_frame(recorder_handle_t *rec, const camera_frame_t *frame)
{
    (void)rec;
    (void)frame;
    return -1;
}

recorder_state_t recorder_get_state(const recorder_handle_t *rec)
{
    (void)rec;
    return REC_STATE_IDLE;
}

uint64_t recorder_get_elapsed_ms(const recorder_handle_t *rec)
{
    (void)rec;
    return 0;
}

/* 后端可用性检测：当前返回 0（不可用） */
int recorder_is_available(void)
{
    return 0;
}

int recorder_get_max_capability(int *out_w, int *out_h, int *out_fps)
{
    if (out_w)  *out_w  = 0;
    if (out_h)  *out_h  = 0;
    if (out_fps) *out_fps = 0;
    return -1;
}
