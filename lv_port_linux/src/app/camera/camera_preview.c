/**
 * @file camera_preview.c
 * @brief 摄像头预览后台线程实现
 *
 * Worker 线程循环：acquire_frame → feed_recorder → convert_to_RGB565 → release_frame
 * 转换结果通过 3 缓冲区零拷贝方案传递给 UI 线程。
 *
 * 性能收益（K230）：
 *   - 转换前：UI 线程每 50ms 阻塞 8~12ms 做转换，整体卡顿
 *   - 转换后：UI 线程仅做 lv_canvas_set_buffer + invalidate（<0.1ms），
 *             转换在 worker 线程并行进行，UI 完全不阻塞
 *   - 副作用：K230 预览帧率从 20fps 提升到 30fps（不再需要降帧）
 */
#include "camera_preview.h"

#include "lvgl/lvgl.h"   /* LV_LOG_* */

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>

struct camera_preview {
    camera_hal_t       *cam;
    int                 dst_w;
    int                 dst_h;
    _Atomic int         rotate;        /* 原子旋转角度，worker 每帧读取 */
    _Atomic int         crop_ratio;    /* 原子画幅裁剪比例：-1=不裁剪, 0=4:3, 1=16:9, 2=1:1 */

    /* 3 缓冲区：display（UI 读）、worker（worker 写）、free（中转） */
    uint16_t           *buf[3];
    _Atomic int         display_idx;   /* 原子：UI 读，worker 写（swap 时） */
    int                 worker_idx;    /* worker 独占，无需原子 */
    int                 free_idx;      /* worker 独占，无需原子 */

    _Atomic int         new_frame_ready;  /* UI 消费后清零，worker 交换后置 1 */
    _Atomic int         running;          /* worker 运行标志 */

    /* 录制器访问（互斥保护，UI 线程 set，worker 线程 read） */
    pthread_mutex_t     rec_mutex;
    recorder_handle_t  *recorder;
    int                 recording;

    pthread_t           thread;
    int                 started;

    /* 全分辨率拍照快照缓冲区：worker 收到请求时对当前帧做"仅旋转不降采样"
     * 转换，结果存入 photo_buf。UI 取走后启动 JPEG 异步编码保存。
     * 这样保存的照片使用原始分辨率（如 1920x1080），而非降采样后的预览 buffer。 */
    uint16_t           *photo_buf;         /* 全分辨率 RGB565 buffer（仅旋转） */
    int                 photo_w;           /* photo_buf 宽（旋转后） */
    int                 photo_h;           /* photo_buf 高（旋转后） */
    _Atomic int         photo_request;     /* UI 请求拍照，worker 检测后处理 */
    _Atomic int         photo_ready;       /* worker 转换完成，UI 可取走 */
};

/* ------------------------------------------------------------------ */
/* Worker 线程主循环                                                    */
/* ------------------------------------------------------------------ */

static void * camera_preview_worker(void *arg)
{
    camera_preview_t *pv = (camera_preview_t *)arg;
    LV_LOG_USER("camera_preview: worker started (dst=%dx%d)",
                pv->dst_w, pv->dst_h);

    while (atomic_load(&pv->running)) {
        camera_frame_t frame;
        if (camera_acquire_frame(pv->cam, &frame) != 0) {
            /* select 超时或错误，重试（running 会在下次循环顶部检查） */
            continue;
        }

        /* 喂录制器（如有）：在转换前喂，使用原始 V4L2 帧 */
        pthread_mutex_lock(&pv->rec_mutex);
        if (pv->recorder && pv->recording) {
            recorder_feed_frame(pv->recorder, &frame);
        }
        pthread_mutex_unlock(&pv->rec_mutex);

        /* 转换到 worker 缓冲区（worker_idx 独占，无需锁）。
         * 预览按当前画幅裁剪源帧中心区域后缩放到屏幕尺寸，
         * 保证预览画面与保存照片内容一致（WYSIWYG）。
         * 拍照快照保持原始帧不裁剪（保存时由 photo_saver 按画幅裁剪） */
        camera_rotate_t rot = (camera_rotate_t)atomic_load(&pv->rotate);
        int crop_ratio = atomic_load(&pv->crop_ratio);
        uint16_t *dst = pv->buf[pv->worker_idx];
        if (camera_frame_to_rgb565_crop(&frame, dst,
                                         pv->dst_w, pv->dst_h, rot,
                                         crop_ratio) == 0) {
            /* 仅当 UI 已消费上一帧时才交换（帧跳过机制）。
             * 这保证了两次 swap 之间 UI 必定调用过 consume_new_frame，
             * 从而 LVGL 有机会渲染完旧 display buffer 后它才被回收为 free。 */
            if (atomic_load(&pv->new_frame_ready) == 0) {
                /* 原子交换 display_idx：worker 的转换结果成为新的 display，
                 * 旧 display 成为 free（中转），旧 free 成为新 worker。 */
                int old_display = atomic_exchange(&pv->display_idx, pv->worker_idx);
                pv->worker_idx = pv->free_idx;
                pv->free_idx   = old_display;
                atomic_store(&pv->new_frame_ready, 1);
            }
            /* else: UI 未消费上一帧，丢弃此帧（worker_idx 不变，下次覆盖） */
        }

        /* 全分辨率拍照快照：检测到 UI 请求时，对当前帧做"仅旋转不降采样"
         * 转换，结果存入 photo_buf 供 UI 取走保存。这样拍照保存使用原始
         * 分辨率（如 1920x1080），而非降采样后的预览 buffer，避免画质损失。
         * 必须在 camera_release_frame 之前处理，因为转换需要访问 frame.data */
        if (atomic_load(&pv->photo_request) && !atomic_load(&pv->photo_ready)) {
            /* 计算旋转后的全分辨率尺寸：
             * 0°/180°：宽高不变；90°/270°：宽高互换 */
            int photo_w = frame.width;
            int photo_h = frame.height;
            if (rot == CAM_ROTATE_90 || rot == CAM_ROTATE_270) {
                photo_w = frame.height;
                photo_h = frame.width;
            }
            /* 首次或尺寸变化时（重新）分配 photo_buf */
            if (!pv->photo_buf || pv->photo_w != photo_w || pv->photo_h != photo_h) {
                if (pv->photo_buf) free(pv->photo_buf);
                size_t photo_bytes = (size_t)photo_w * photo_h * 2;
                pv->photo_buf = (uint16_t *)malloc(photo_bytes);
                if (!pv->photo_buf) {
                    LV_LOG_ERROR("camera_preview: photo_buf 分配失败 (%zu 字节)", photo_bytes);
                    pv->photo_w = 0;
                    pv->photo_h = 0;
                    /* 分配失败也要通知 UI：设置 photo_ready=1 让 consume_photo
                     * 进入返回路径，consume_photo 检测 photo_buf=NULL 返回 -1，
                     * UI 收到 -1 后不启动编码，用户可通过日志感知失败。
                     * 否则 UI 永远等待 photo_ready，拍照静默丢失。 */
                    atomic_store(&pv->photo_ready, 1);
                    atomic_store(&pv->photo_request, 0);
                    /* 不能 continue：会跳过 camera_release_frame 导致 V4L2 buffer 泄漏。
                     * 让下方转换检测 photo_buf=NULL 自动失败，正常走 release_frame 路径 */
                } else {
                    pv->photo_w = photo_w;
                    pv->photo_h = photo_h;
                }
            }
            /* 转换：dst=photo_buf，宽高=原始帧旋转后尺寸（不降采样），
             * rotate=rot（与预览一致，保证方向正确）。
             * photo_buf=NULL（分配失败）时转换函数会返回 -1，进入 else 分支 */
            if (pv->photo_buf &&
                camera_frame_to_rgb565(&frame, pv->photo_buf,
                                        pv->photo_w, pv->photo_h, rot) == 0) {
                atomic_store(&pv->photo_ready, 1);
                atomic_store(&pv->photo_request, 0);
                LV_LOG_USER("camera_preview: 全分辨率拍照快照就绪 (%dx%d)",
                            pv->photo_w, pv->photo_h);
            } else {
                /* 转换失败（含 photo_buf=NULL 的分配失败场景）：
                 * photo_request 已在上方分配失败或此处清零，避免反复重试。
                 * photo_ready 已在分配失败路径置 1（若为转换失败则保持 0） */
                if (pv->photo_buf) {
                    /* photo_buf 有效但转换失败：清零请求，photo_ready 保持 0 */
                    LV_LOG_WARN("camera_preview: 全分辨率转换失败，丢弃拍照请求");
                    atomic_store(&pv->photo_request, 0);
                }
            }
        }

        /* 必须释放帧，归还 V4L2 buffer */
        camera_release_frame(pv->cam, &frame);
    }

    LV_LOG_USER("camera_preview: worker stopped");
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 生命周期管理                                                        */
/* ------------------------------------------------------------------ */

camera_preview_t * camera_preview_create(camera_hal_t *cam,
                                          int dst_w, int dst_h,
                                          camera_rotate_t rotate)
{
    if (!cam || dst_w <= 0 || dst_h <= 0) return NULL;

    camera_preview_t *pv = (camera_preview_t *)calloc(1, sizeof(*pv));
    if (!pv) return NULL;

    pv->cam   = cam;
    pv->dst_w = dst_w;
    pv->dst_h = dst_h;
    atomic_store(&pv->rotate, (int)rotate);

    /* 分配 3 个 RGB565 缓冲区，初始化为黑色（避免首帧前花屏） */
    size_t buf_bytes = (size_t)dst_w * dst_h * 2;
    for (int i = 0; i < 3; i++) {
        pv->buf[i] = (uint16_t *)malloc(buf_bytes);
        if (!pv->buf[i]) {
            LV_LOG_ERROR("camera_preview: 缓冲区 %d 分配失败 (%zu 字节)", i, buf_bytes);
            for (int j = 0; j < i; j++) free(pv->buf[j]);
            free(pv);
            return NULL;
        }
        memset(pv->buf[i], 0, buf_bytes);
    }

    /* 初始分配：buf[0]=display, buf[1]=worker, buf[2]=free */
    atomic_store(&pv->display_idx, 0);
    pv->worker_idx = 1;
    pv->free_idx   = 2;

    atomic_store(&pv->new_frame_ready, 0);
    atomic_store(&pv->running, 0);

    /* 画幅裁剪初始为不裁剪，UI 启动后通过 set_crop_ratio 设置实际画幅 */
    atomic_store(&pv->crop_ratio, -1);

    pthread_mutex_init(&pv->rec_mutex, NULL);
    pv->recorder  = NULL;
    pv->recording = 0;

    /* 拍照快照缓冲区延迟分配（首次 take_photo 时由 worker 分配） */
    pv->photo_buf = NULL;
    pv->photo_w   = 0;
    pv->photo_h   = 0;
    atomic_store(&pv->photo_request, 0);
    atomic_store(&pv->photo_ready, 0);

    pv->started = 0;

    LV_LOG_USER("camera_preview: created (3x %zu bytes, rotate=%d)",
                buf_bytes, (int)rotate);
    return pv;
}

void camera_preview_destroy(camera_preview_t *pv)
{
    if (!pv) return;
    camera_preview_stop(pv);
    for (int i = 0; i < 3; i++) {
        free(pv->buf[i]);
        pv->buf[i] = NULL;
    }
    /* 释放全分辨率拍照缓冲区 */
    free(pv->photo_buf);
    pv->photo_buf = NULL;
    pthread_mutex_destroy(&pv->rec_mutex);
    free(pv);
}

int camera_preview_start(camera_preview_t *pv)
{
    if (!pv || pv->started) return -1;
    atomic_store(&pv->running, 1);
    if (pthread_create(&pv->thread, NULL, camera_preview_worker, pv) != 0) {
        LV_LOG_ERROR("camera_preview: pthread_create 失败");
        atomic_store(&pv->running, 0);
        return -1;
    }
    pv->started = 1;
    LV_LOG_USER("camera_preview: worker thread started");
    return 0;
}

void camera_preview_stop(camera_preview_t *pv)
{
    if (!pv || !pv->started) return;
    atomic_store(&pv->running, 0);
    /* worker 在 camera_acquire_frame 中阻塞（select 1s 超时或帧到达）。
     * 摄像头正常流式中：下一帧 33ms 内到达，worker 检查 running=0 后退出。
     * 最坏情况（设备断开）：select 1s 超时后退出。 */
    pthread_join(pv->thread, NULL);
    pv->started = 0;
    LV_LOG_USER("camera_preview: worker thread joined");
}

/* ------------------------------------------------------------------ */
/* 线程安全 API                                                        */
/* ------------------------------------------------------------------ */

void camera_preview_set_rotate(camera_preview_t *pv, camera_rotate_t rotate)
{
    if (!pv) return;
    atomic_store(&pv->rotate, (int)rotate);
}

void camera_preview_set_crop_ratio(camera_preview_t *pv, int crop_ratio)
{
    if (!pv) return;
    /* 限制有效范围：-1=不裁剪, 0=4:3, 1=16:9, 2=1:1 */
    if (crop_ratio < -1 || crop_ratio > 2) crop_ratio = -1;
    atomic_store(&pv->crop_ratio, crop_ratio);
}

const uint16_t * camera_preview_get_display(camera_preview_t *pv)
{
    if (!pv) return NULL;
    /* display_idx 是原子的，读取一致值（0/1/2）。
     * 返回的指针指向一个完整的 RGB565 buffer，不会被部分写入。 */
    int idx = atomic_load(&pv->display_idx);
    return pv->buf[idx];
}

int camera_preview_consume_new_frame(camera_preview_t *pv)
{
    if (!pv) return 0;
    return atomic_exchange(&pv->new_frame_ready, 0);
}

void camera_preview_set_recorder(camera_preview_t *pv, recorder_handle_t *rec)
{
    if (!pv) return;
    pthread_mutex_lock(&pv->rec_mutex);
    pv->recorder = rec;
    pthread_mutex_unlock(&pv->rec_mutex);
}

void camera_preview_set_recording(camera_preview_t *pv, int recording)
{
    if (!pv) return;
    pthread_mutex_lock(&pv->rec_mutex);
    pv->recording = recording;
    pthread_mutex_unlock(&pv->rec_mutex);
}

/* ------------------------------------------------------------------ */
/* 全分辨率拍照快照 API                                                 */
/* ------------------------------------------------------------------ */

int camera_preview_take_photo(camera_preview_t *pv)
{
    if (!pv || !pv->started) return -1;
    /* 上一次拍照尚未取走：拒绝新请求，避免覆盖未保存的照片 */
    if (atomic_load(&pv->photo_ready)) {
        LV_LOG_WARN("camera_preview: 上次拍照快照未取走，跳过本次请求");
        return -2;
    }
    atomic_store(&pv->photo_request, 1);
    return 0;
}

int camera_preview_is_photo_ready(camera_preview_t *pv)
{
    if (!pv) return 0;
    return atomic_load(&pv->photo_ready);
}

int camera_preview_consume_photo(camera_preview_t *pv,
                                   const uint16_t **out_buf,
                                   int *out_w, int *out_h)
{
    if (!pv || !out_buf || !out_w || !out_h) return -1;
    if (!atomic_load(&pv->photo_ready)) return -1;
    if (!pv->photo_buf || pv->photo_w <= 0 || pv->photo_h <= 0) {
        atomic_store(&pv->photo_ready, 0);
        return -1;
    }
    *out_buf = pv->photo_buf;
    *out_w   = pv->photo_w;
    *out_h   = pv->photo_h;
    atomic_store(&pv->photo_ready, 0);
    return 0;
}
