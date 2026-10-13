/**
 * @file recorder_hal.h
 * @brief 录制抽象层（预留 4K60 接口）
 *
 * 设计目标：
 *   - 后端可插拔：FFmpeg / GStreamer / K230 VPU 硬件编码 / RK MPP
 *   - 支持 4K@60fps 录制（更换摄像头后启用）
 *   - 当前 GC2093 仅 1080p@60fps，录制降级运行
 *   - 与 camera_hal 解耦：录制独立于预览，可同时进行
 *
 * 使用流程：
 *   recorder_config_t cfg = {
 *       .width = 3840, .height = 2160, .fps = 60,
 *       .codec = REC_CODEC_H265,
 *       .container = REC_CONTAINER_MP4,
 *       .output_path = "/mnt/sdcard/DCIM/VIDEO_001.mp4"
 *   };
 *   recorder_handle_t *rec = recorder_create(&cfg);
 *   if (rec) {
 *       recorder_start(rec);
 *       while (recording) {
 *           camera_frame_t frame;
 *           camera_acquire_frame(cam, &frame);
 *           recorder_feed_frame(rec, &frame);  // 送编码器
 *           camera_release_frame(cam, &frame);
 *       }
 *       recorder_stop(rec);
 *       recorder_destroy(rec);
 *   }
 *
 * 当前状态（2026-08-01）：
 *   - 接口定义完成，软件后端（recorder_mjpeg.c）已实现
 *   - 软件后端输出 MJPEG/AVI，无硬件依赖，1080p@30fps 可用
 *   - 后续接入 RK MPP / K230 VPU 硬件编码后端后即可支持 4K60 录制
 */
#ifndef RECORDER_HAL_H
#define RECORDER_HAL_H

#include <stdint.h>
#include <stddef.h>
#include "camera_hal.h"   /* camera_frame_t 复用 */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 后端类型标识（magic 字段，放在 recorder_handle 开头用于区分后端）    */
/* recorder_hal.c 通过读取 handle 的第一个 uint32_t 判断后端类型，     */
/* 然后分发到对应的 mjpeg/rkmpp 后端实现。                             */
/* ------------------------------------------------------------------ */
#define REC_BACKEND_MAGIC_MJPEG  0x4D4A5047u   /* 'MJPG' - 软件后端 */
#define REC_BACKEND_MAGIC_RKMPP  0x524B4D50u   /* 'RKMP' - RK MPP 硬件后端 */

/* ------------------------------------------------------------------ */
/* 类型定义                                                            */
/* ------------------------------------------------------------------ */

/* 视频编码器 */
typedef enum {
    REC_CODEC_H264 = 0,    /* H.264/AVC（兼容性最好） */
    REC_CODEC_H265 = 1,    /* H.265/HEVC（4K 推荐，码率省 50%） */
    REC_CODEC_MJPEG = 2,   /* MJPEG（简单，文件大） */
} recorder_codec_t;

/* 容器格式 */
typedef enum {
    REC_CONTAINER_MP4 = 0,    /* MP4（通用） */
    REC_CONTAINER_MKV = 1,    /* MKV（支持无缝录制） */
    REC_CONTAINER_TS  = 2,    /* TS（直播流） */
    REC_CONTAINER_AVI = 3,    /* AVI（MJPEG 软件后端使用） */
} recorder_container_t;

/* 录制配置 */
typedef struct {
    int                   width;         /* 录制宽度（像素，4K=3840） */
    int                   height;        /* 录制高度（像素，4K=2160） */
    int                   fps;           /* 录制帧率（60） */
    int                   bitrate_kbps;  /* 码率（kbps，4K60 建议 80000） */
    recorder_codec_t      codec;         /* 编码器 */
    recorder_container_t  container;     /* 容器格式 */
    const char           *output_path;   /* 输出文件路径 */
    camera_rotate_t       rotate;        /* 旋转角度（与预览一致） */
} recorder_config_t;

/* 录制句柄（不透明指针，由具体后端实现） */
typedef struct recorder_handle recorder_handle_t;

/* 录制状态 */
typedef enum {
    REC_STATE_IDLE = 0,     /* 未初始化 */
    REC_STATE_READY = 1,    /* 已创建，等待开始 */
    REC_STATE_RECORDING = 2,/* 录制中 */
    REC_STATE_STOPPING = 3, /* 停止中（刷新尾帧） */
    REC_STATE_ERROR = 4,    /* 错误 */
} recorder_state_t;

/* ------------------------------------------------------------------ */
/* 生命周期管理                                                        */
/* ------------------------------------------------------------------ */

/* 创建录制器（不立即开始）。
 * 配置校验：分辨率/帧率不得超过 device.json 声明的 camera_caps。
 * 后端不可用时返回 NULL，调用方应回退到「录制不可用」提示。
 * 返回 NULL 的场景：
 *   - 后端未编译或不可用
 *   - 配置超出摄像头能力
 *   - 输出路径不可写 */
recorder_handle_t * recorder_create(const recorder_config_t *cfg);

/* 销毁录制器（必须先 stop） */
void recorder_destroy(recorder_handle_t *rec);

/* ------------------------------------------------------------------ */
/* 录制控制                                                            */
/* ------------------------------------------------------------------ */

/* 开始录制。返回 0 成功，负数失败。 */
int recorder_start(recorder_handle_t *rec);

/* 停止录制（刷新尾帧 + 关闭文件，可能耗时 1~2 秒）。 */
int recorder_stop(recorder_handle_t *rec);

/* 送入一帧（从 camera_hal 获取的帧）。
 * 返回 0 成功，负数失败（如队列满，应丢弃该帧避免阻塞预览）。
 * 注意：此函数不应阻塞超过 5ms，避免影响预览帧率。 */
int recorder_feed_frame(recorder_handle_t *rec, const camera_frame_t *frame);

/* 查询当前状态 */
recorder_state_t recorder_get_state(const recorder_handle_t *rec);

/* 查询已录制时长（毫秒） */
uint64_t recorder_get_elapsed_ms(const recorder_handle_t *rec);

/* ------------------------------------------------------------------ */
/* 能力查询                                                            */
/* ------------------------------------------------------------------ */

/* 查询后端是否可用（编译时/运行时检测）。
 * 返回 1 可用，0 不可用。软件 MJPEG 后端始终返回 1。 */
int recorder_is_available(void);

/* 查询后端支持的最大分辨率/帧率。
 * 返回 0 成功，负数不支持。
 * 软件 MJPEG 后端返回 1080p@30fps（受 CPU 性能约束）。 */
int recorder_get_max_capability(int *out_w, int *out_h, int *out_fps);

#ifdef __cplusplus
}
#endif

#endif /* RECORDER_HAL_H */
