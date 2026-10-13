/**
 * @file recorder_hal.c
 * @brief 录制抽象层 - 公共 API 分发器
 *
 * 架构：
 *   page_home.c → recorder_create()（本文件）→ 选择后端
 *                                          ├─ RK3566+H264 → recorder_rkmpp_*
 *                                          └─ 其他/回退   → recorder_mjpeg_*
 *
 * 后端区分：recorder_handle 的第一个 uint32_t 为 magic 字段
 *   - REC_BACKEND_MAGIC_MJPEG (0x4D4A5047) → MJPEG 软件后端
 *   - REC_BACKEND_MAGIC_RKMPP (0x524B4D50) → RK MPP 硬件后端
 *
 * 后端选择策略：
 *   1. aarch64 平台 + H.264 请求 + MPP 可用 → RKMPP 后端
 *   2. 其他情况 → MJPEG 软件后端（全平台兜底）
 */
#include "recorder_hal.h"
#include "recorder_backends.h"
#include "../common/board_detect.h"
#include "../app_main.h"

#include "lvgl/lvgl.h"

/* 读取 handle 的 magic 字段（第一个 uint32_t）判断后端类型 */
static inline uint32_t recorder_get_magic(const recorder_handle_t *rec)
{
    if (!rec) return 0;
    return *(const uint32_t *)rec;
}

/* ------------------------------------------------------------------ */
/* 公共 API：后端选择 + 分发                                            */
/* ------------------------------------------------------------------ */

recorder_handle_t * recorder_create(const recorder_config_t *cfg)
{
    if (!cfg) return NULL;

    /* aarch64 平台 + H.264 请求：优先尝试 MPP 硬件后端 */
#if defined(__aarch64__)
    if (cfg->codec == REC_CODEC_H264 && board_detect() == BOARD_RK3566) {
        if (recorder_rkmpp_is_available()) {
            recorder_handle_t *rec = recorder_rkmpp_create(cfg);
            if (rec) {
                LV_LOG_USER("录制后端: RK MPP H.264+MP4（硬件编码）");
                return rec;
            }
            LV_LOG_WARN("RKMPP 后端创建失败，回退到 MJPEG 软件后端");
        } else {
            LV_LOG_USER("RKMPP 后端不可用（librockchip_mpp.so 未加载），使用 MJPEG 软件后端");
        }
    }
#else
    (void)board_detect;  /* 非 aarch64 平台，board_detect 不使用 */
#endif

    /* 回退到 MJPEG 软件后端（全平台可用，输出 MJPEG+MP4） */
    LV_LOG_USER("录制后端: MJPEG+MP4（软件编码）");
    return recorder_mjpeg_create(cfg);
}

void recorder_destroy(recorder_handle_t *rec)
{
    if (!rec) return;
    uint32_t magic = recorder_get_magic(rec);
    if (magic == REC_BACKEND_MAGIC_MJPEG) {
        recorder_mjpeg_destroy(rec);
    }
#if defined(__aarch64__)
    else if (magic == REC_BACKEND_MAGIC_RKMPP) {
        recorder_rkmpp_destroy(rec);
    }
#endif
    else {
        LV_LOG_ERROR("recorder_destroy: 未知后端 magic=0x%08X", magic);
    }
}

int recorder_start(recorder_handle_t *rec)
{
    if (!rec) return -1;
    uint32_t magic = recorder_get_magic(rec);
    if (magic == REC_BACKEND_MAGIC_MJPEG) return recorder_mjpeg_start(rec);
#if defined(__aarch64__)
    if (magic == REC_BACKEND_MAGIC_RKMPP) return recorder_rkmpp_start(rec);
#endif
    return -1;
}

int recorder_stop(recorder_handle_t *rec)
{
    if (!rec) return -1;
    uint32_t magic = recorder_get_magic(rec);
    if (magic == REC_BACKEND_MAGIC_MJPEG) return recorder_mjpeg_stop(rec);
#if defined(__aarch64__)
    if (magic == REC_BACKEND_MAGIC_RKMPP) return recorder_rkmpp_stop(rec);
#endif
    return -1;
}

int recorder_feed_frame(recorder_handle_t *rec, const camera_frame_t *frame)
{
    if (!rec) return -1;
    uint32_t magic = recorder_get_magic(rec);
    if (magic == REC_BACKEND_MAGIC_MJPEG) return recorder_mjpeg_feed_frame(rec, frame);
#if defined(__aarch64__)
    if (magic == REC_BACKEND_MAGIC_RKMPP) return recorder_rkmpp_feed_frame(rec, frame);
#endif
    return -1;
}

recorder_state_t recorder_get_state(const recorder_handle_t *rec)
{
    if (!rec) return REC_STATE_IDLE;
    uint32_t magic = recorder_get_magic(rec);
    if (magic == REC_BACKEND_MAGIC_MJPEG) return recorder_mjpeg_get_state(rec);
#if defined(__aarch64__)
    if (magic == REC_BACKEND_MAGIC_RKMPP) return recorder_rkmpp_get_state(rec);
#endif
    return REC_STATE_IDLE;
}

uint64_t recorder_get_elapsed_ms(const recorder_handle_t *rec)
{
    if (!rec) return 0;
    uint32_t magic = recorder_get_magic(rec);
    if (magic == REC_BACKEND_MAGIC_MJPEG) return recorder_mjpeg_get_elapsed_ms(rec);
#if defined(__aarch64__)
    if (magic == REC_BACKEND_MAGIC_RKMPP) return recorder_rkmpp_get_elapsed_ms(rec);
#endif
    return 0;
}

/* 后端可用性：MJPEG 始终可用，RKMPP 在 aarch64+RK3566 时可用。
 * 只要任一后端可用，录制功能即可用 */
int recorder_is_available(void)
{
    /* MJPEG 软件后端始终可用，所以录制功能始终可用 */
    return 1;
}

/* 查询后端最大能力：优先返回 RKMPP 硬件能力（如果可用），否则 MJPEG 软件能力 */
int recorder_get_max_capability(int *out_w, int *out_h, int *out_fps)
{
#if defined(__aarch64__)
    if (board_detect() == BOARD_RK3566 && recorder_rkmpp_is_available()) {
        return recorder_rkmpp_get_max_capability(out_w, out_h, out_fps);
    }
#endif
    return recorder_mjpeg_get_max_capability(out_w, out_h, out_fps);
}
