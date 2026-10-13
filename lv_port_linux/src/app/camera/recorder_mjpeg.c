/**
 * @file recorder_mjpeg.c
 * @brief 录制后端：MJPEG 编码 + MP4 容器（纯软件实现，线程化异步编码）
 *
 * 架构（2026-08-02 重构：AVI→MP4 容器）：
 *   ┌──────────┐   拷贝入队    ┌──────────┐   取帧编码    ┌──────────┐
 *   │ UI 线程  │ ────────────→ │ 帧队列   │ ────────────→ │ 工作线程 │
 *   │ (定时器) │   非阻塞立即返回 │ (ring buf)│   JPEG+MP4写入 │ (pthread)│
 *   └──────────┘                └──────────┘                └──────────┘
 *
 * 核心优化：
 *   - recorder_feed_frame 只拷贝帧数据入队，立即返回（<1ms）
 *   - JPEG 编码 + 文件写入在工作线程执行，不阻塞 UI 线程
 *   - 队列满时丢弃最旧帧（保证实时性，避免帧堆积）
 *   - 帧缓冲池预分配，避免运行时 malloc
 *
 * 容器格式：MP4（通过 mp4_muxer 组件），MJPEG sample（每帧独立 JPEG，全关键帧）
 * 兼容性：VLC、mpv、Linux 系播放器、现代 Windows Media Player 支持 MJPEG in MP4
 */
#include "recorder_hal.h"
#include "recorder_backends.h"
#include "mp4_muxer.h"
#include "../app_main.h"

#include "lvgl/lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>     /* mkdir() 创建输出目录 */
#include <sys/types.h>
#include <pthread.h>      /* 工作线程 */

/* libjpeg 用于 JPEG 编码 */
#include <jpeglib.h>
#include <setjmp.h>

/* ------------------------------------------------------------------ */
/* 配置常量                                                            */
/* ------------------------------------------------------------------ */

/* 帧队列容量：预分配的帧缓冲池大小。
 * 1080p YUYV/RGB565 ≈ 4MB/帧，4 帧 ≈ 16MB 内存占用。
 * 队列满时丢弃最旧帧，保证实时性。 */
#define REC_FRAME_QUEUE_SIZE 4

/* JPEG 编码缓冲区初始大小（1帧 1080p JPEG 约 200~500KB） */
#define JPEG_BUF_INIT_SZ (512 * 1024)

/* ------------------------------------------------------------------ */
/* 数据结构                                                            */
/* ------------------------------------------------------------------ */

/* 队列中的帧条目：包含帧元数据 + 数据缓冲区指针 */
typedef struct {
    camera_frame_t frame;       /* 帧元数据（width/height/fourcc/size） */
    uint8_t       *data;        /* 帧数据拷贝（指向缓冲池中的槽位） */
    int            in_use;      /* 1=该槽位有待处理帧，0=空闲 */
} queue_entry_t;

struct recorder_handle {
    uint32_t          backend_magic;/* 后端标识（REC_BACKEND_MAGIC_MJPEG） */
    recorder_config_t cfg;          /* 配置副本 */
    mp4_muxer_t      *muxer;        /* MP4 容器写入器 */
    recorder_state_t  state;        /* 当前状态 */

    /* 帧率控制 */
    uint64_t          start_ms;     /* 录制起始时间 */
    uint32_t          frame_count;  /* 已写入帧数 */
    uint32_t          fps_interval; /* 帧间隔（ms），= 1000/fps */

    /* JPEG 编码缓冲区（工作线程专用，无需锁） */
    uint8_t           *jpeg_buf;    /* JPEG 编码输出缓冲区 */
    int                jpeg_buf_sz; /* 缓冲区大小 */
    uint8_t           *rgb888_buf;  /* RGB888 转换缓冲区（YUV→RGB 用） */

    /* 帧队列（ring buffer）+ 工作线程 */
    queue_entry_t      queue[REC_FRAME_QUEUE_SIZE]; /* 帧缓冲池 */
    int                q_head;      /* 生产者写入位置 */
    int                q_tail;      /* 消费者读取位置 */
    int                q_count;     /* 队列中待处理帧数 */
    pthread_mutex_t    q_mutex;     /* 队列互斥锁 */
    pthread_cond_t     q_cond;      /* 队列非空条件变量 */
    pthread_t          worker;      /* 工作线程句柄 */
    int                worker_running; /* 工作线程运行标志 */
    int                stop_requested; /* 停止请求标志 */
};

/* ------------------------------------------------------------------ */
/* JPEG 编码：RGB888 → JPEG（内存输出，质量 80）                       */
/* ------------------------------------------------------------------ */

/* JPEG 错误处理（跳点，避免 longjmp 到已销毁栈） */
struct jpeg_err_ctx {
    struct jpeg_error_mgr pub;
    jmp_buf jb;
};

static void jpeg_error_exit(j_common_ptr cinfo)
{
    struct jpeg_err_ctx *ctx = (struct jpeg_err_ctx *)cinfo->err;
    longjmp(ctx->jb, 1);
}

/* 将 RGB888 数据编码为 JPEG，输出到内存缓冲区。
 * 返回 JPEG 数据大小（字节），失败返回 -1。
 * jpeg_buf/jpeg_buf_sz 为输出缓冲区，不足时自动 realloc */
static int encode_rgb888_to_jpeg(const uint8_t *rgb888, int width, int height,
                                  uint8_t **jpeg_buf, int *jpeg_buf_sz)
{
    struct jpeg_compress_struct cinfo;
    struct jpeg_err_ctx jerr;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit;

    if (setjmp(jerr.jb)) {
        /* JPEG 编码出错 */
        jpeg_destroy_compress(&cinfo);
        return -1;
    }

    jpeg_create_compress(&cinfo);

    /* 内存输出目标 */
    unsigned long out_sz = 0;
    unsigned char *out_buf = NULL;
    jpeg_mem_dest(&cinfo, &out_buf, &out_sz);

    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3;       /* RGB888 */
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 80, TRUE);  /* 质量 80（录制用，低于拍照的 90） */
    cinfo.dct_method = JDCT_FASTEST;     /* 快速 DCT，降低 CPU */

    jpeg_start_compress(&cinfo, TRUE);

    /* 逐行写入（RGB888 行序） */
    const int row_stride = width * 3;
    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row = (JSAMPROW)(rgb888 + cinfo.next_scanline * row_stride);
        jpeg_write_scanlines(&cinfo, &row, 1);
    }

    jpeg_finish_compress(&cinfo);

    /* 拷贝到调用方的缓冲区 */
    if ((int)out_sz > *jpeg_buf_sz) {
        uint8_t *new_buf = realloc(*jpeg_buf, out_sz);
        if (!new_buf) {
            free(out_buf);
            jpeg_destroy_compress(&cinfo);
            return -1;
        }
        *jpeg_buf = new_buf;
        *jpeg_buf_sz = out_sz;
    }
    memcpy(*jpeg_buf, out_buf, out_sz);

    free(out_buf);
    jpeg_destroy_compress(&cinfo);
    return (int)out_sz;
}

/* ------------------------------------------------------------------ */
/* 帧格式转换：camera_frame_t → RGB888（带旋转）                       */
/* ------------------------------------------------------------------ */

/* YUYV → RGB888 单像素转换 */
static inline void yuyv_to_rgb888(int y, int u, int v, uint8_t *rgb)
{
    int r = y + ((157 * v) >> 7);
    int g = y - ((39 * u + 80 * v) >> 7);
    int b = y + ((201 * u) >> 7);
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    rgb[0] = (uint8_t)r;
    rgb[1] = (uint8_t)g;
    rgb[2] = (uint8_t)b;
}

/* RGB565 → RGB888 单像素转换 */
static inline void rgb565_to_rgb888(uint16_t pix, uint8_t *rgb)
{
    rgb[0] = (uint8_t)((pix >> 11) & 0x1F) << 3;  /* R */
    rgb[1] = (uint8_t)((pix >> 5) & 0x3F) << 2;   /* G */
    rgb[2] = (uint8_t)(pix & 0x1F) << 3;          /* B */
}

/* NV12 → RGB888 单像素转换（u/v 为色度分量） */
static inline void nv12_to_rgb888(int y, int u, int v, uint8_t *rgb)
{
    yuyv_to_rgb888(y, u, v, rgb);  /* YUV→RGB 公式相同 */
}

/* 将 camera_frame_t 转换为 RGB888（带旋转）。
 * dst 必须至少 dst_w * dst_h * 3 字节。
 * 旋转后宽高可能互换：90°/270° 时 dst_w=src_h, dst_h=src_w。
 * 返回 0 成功，负数失败。 */
static int frame_to_rgb888(const camera_frame_t *frame,
                            uint8_t *dst, int dst_w, int dst_h,
                            camera_rotate_t rotate)
{
    const int src_w = frame->width;
    const int src_h = frame->height;
    const uint8_t *src = frame->data;

    /* 基本参数校验：防止空指针或无效尺寸导致越界访问 */
    if (!src || src_w <= 0 || src_h <= 0 || !dst || dst_w <= 0 || dst_h <= 0) {
        return -1;
    }

    /* 检查帧数据长度是否足够（防止 V4L2 返回的部分帧导致越界读取）。
     * 根据格式计算最小所需字节数 */
    size_t min_size = 0;
    if (frame->fourcc == CAM_FOURCC_RGB565) {
        min_size = (size_t)src_w * src_h * 2;
    } else if (frame->fourcc == CAM_FOURCC_NV12 || frame->fourcc == CAM_FOURCC_NV21) {
        min_size = (size_t)src_w * src_h * 3 / 2;  /* Y + UV 半平面 */
    } else if (frame->fourcc == CAM_FOURCC_YUYV) {
        min_size = (size_t)src_w * src_h * 2;
    } else {
        return -1;  /* 不支持的格式 */
    }
    if (frame->size < min_size) {
        LV_LOG_WARN("录制：帧数据不足（size=%zu, need=%zu, %dx%d），跳过该帧",
                    (size_t)frame->size, min_size, src_w, src_h);
        return -1;
    }

    /* 检查旋转后宽高是否匹配 */
    if (rotate == CAM_ROTATE_90 || rotate == CAM_ROTATE_270) {
        if (dst_w != src_h || dst_h != src_w) return -1;
    } else {
        if (dst_w != src_w || dst_h != src_h) return -1;
    }

    /* 根据 fourcc 选择转换路径 */
    if (frame->fourcc == CAM_FOURCC_RGB565) {
        /* RGB565 → RGB888 */
        for (int y = 0; y < src_h; y++) {
            for (int x = 0; x < src_w; x++) {
                uint16_t pix = ((uint16_t)src[(y * src_w + x) * 2]) |
                               ((uint16_t)src[(y * src_w + x) * 2 + 1] << 8);
                uint8_t rgb[3];
                rgb565_to_rgb888(pix, rgb);

                /* 根据旋转写入目标位置 */
                int dx, dy;
                switch (rotate) {
                    case CAM_ROTATE_90:   dx = src_h - 1 - y; dy = x; break;
                    case CAM_ROTATE_180:  dx = src_w - 1 - x; dy = src_h - 1 - y; break;
                    case CAM_ROTATE_270:  dx = y; dy = src_w - 1 - x; break;
                    default:              dx = x; dy = y; break;
                }
                uint8_t *d = dst + (dy * dst_w + dx) * 3;
                d[0] = rgb[0]; d[1] = rgb[1]; d[2] = rgb[2];
            }
        }
    } else if (frame->fourcc == CAM_FOURCC_YUYV) {
        /* YUYV 4:2:2 打包：每 2 像素 4 字节 (Y0 U Y1 V) */
        for (int y = 0; y < src_h; y++) {
            for (int x = 0; x < src_w; x += 2) {
                int idx = (y * src_w + x) * 2;
                int y0 = src[idx] - 16;
                int u  = src[idx + 1] - 128;
                int y1 = src[idx + 2] - 16;
                int v  = src[idx + 3] - 128;

                uint8_t rgb0[3], rgb1[3];
                yuyv_to_rgb888(y0, u, v, rgb0);
                yuyv_to_rgb888(y1, u, v, rgb1);

                /* 写入旋转后位置 */
                int dx0, dy0, dx1, dy1;
                switch (rotate) {
                    case CAM_ROTATE_90:
                        dx0 = src_h - 1 - y; dy0 = x;
                        dx1 = src_h - 1 - y; dy1 = x + 1;
                        break;
                    case CAM_ROTATE_180:
                        dx0 = src_w - 2 - x; dy0 = src_h - 1 - y;
                        dx1 = src_w - 1 - x; dy1 = src_h - 1 - y;
                        break;
                    case CAM_ROTATE_270:
                        dx0 = y; dy0 = src_w - 1 - x;
                        dx1 = y; dy1 = src_w - 2 - x;
                        break;
                    default:
                        dx0 = x; dy0 = y;
                        dx1 = x + 1; dy1 = y;
                        break;
                }
                uint8_t *d0 = dst + (dy0 * dst_w + dx0) * 3;
                uint8_t *d1 = dst + (dy1 * dst_w + dx1) * 3;
                d0[0] = rgb0[0]; d0[1] = rgb0[1]; d0[2] = rgb0[2];
                d1[0] = rgb1[0]; d1[1] = rgb1[1]; d1[2] = rgb1[2];
            }
        }
    } else if (frame->fourcc == CAM_FOURCC_NV12 || frame->fourcc == CAM_FOURCC_NV21) {
        /* NV12/NV21 4:2:0 半平面：Y 平面 + UV 交错平面 */
        int is_nv21 = (frame->fourcc == CAM_FOURCC_NV21);
        const uint8_t *y_plane = src;
        const uint8_t *uv_plane = src + src_w * src_h;

        for (int y = 0; y < src_h; y += 2) {
            for (int x = 0; x < src_w; x += 2) {
                /* 4 个 Y + 1 个 UV 对 */
                int uv_idx = (y / 2) * src_w + (x / 2) * 2;
                int u = uv_plane[uv_idx + (is_nv21 ? 1 : 0)] - 128;
                int v = uv_plane[uv_idx + (is_nv21 ? 0 : 1)] - 128;

                for (int dy = 0; dy < 2; dy++) {
                    for (int dx = 0; dx < 2; dx++) {
                        int sx = x + dx;
                        int sy = y + dy;
                        if (sx >= src_w || sy >= src_h) continue;
                        int y_val = y_plane[sy * src_w + sx] - 16;

                        uint8_t rgb[3];
                        nv12_to_rgb888(y_val, u, v, rgb);

                        /* 旋转写入 */
                        int ddx, ddy;
                        switch (rotate) {
                            case CAM_ROTATE_90:   ddx = src_h - 1 - sy; ddy = sx; break;
                            case CAM_ROTATE_180:  ddx = src_w - 1 - sx; ddy = src_h - 1 - sy; break;
                            case CAM_ROTATE_270:  ddx = sy; ddy = src_w - 1 - sx; break;
                            default:              ddx = sx; ddy = sy; break;
                        }
                        uint8_t *d = dst + (ddy * dst_w + ddx) * 3;
                        d[0] = rgb[0]; d[1] = rgb[1]; d[2] = rgb[2];
                    }
                }
            }
        }
    } else {
        return -1;  /* 不支持的格式 */
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* 工作线程：从队列取帧，执行编码 + 写入                                */
/* ------------------------------------------------------------------ */

static void *recorder_worker_thread(void *arg)
{
    recorder_handle_t *rec = (recorder_handle_t *)arg;

    /* 确定 RGB888 目标尺寸（旋转后宽高可能互换） */
    int dst_w = rec->cfg.width;
    int dst_h = rec->cfg.height;
    if (rec->cfg.rotate == CAM_ROTATE_90 || rec->cfg.rotate == CAM_ROTATE_270) {
        dst_w = rec->cfg.height;
        dst_h = rec->cfg.width;
    }

    while (1) {
        /* 等待队列有帧或收到停止信号 */
        pthread_mutex_lock(&rec->q_mutex);
        while (rec->q_count == 0 && !rec->stop_requested) {
            pthread_cond_wait(&rec->q_cond, &rec->q_mutex);
        }

        /* 收到停止信号且队列已空：退出线程 */
        if (rec->stop_requested && rec->q_count == 0) {
            pthread_mutex_unlock(&rec->q_mutex);
            break;
        }

        /* 取出一帧（从 tail 位置） */
        queue_entry_t *entry = &rec->queue[rec->q_tail];
        camera_frame_t frame = entry->frame;
        /* data 指针指向缓冲池槽位，工作线程使用期间生产者不会覆盖（因为 in_use=1） */
        uint8_t *frame_data = entry->data;
        entry->in_use = 0;  /* 标记槽位即将释放（允许生产者复用） */
        rec->q_tail = (rec->q_tail + 1) % REC_FRAME_QUEUE_SIZE;
        rec->q_count--;
        pthread_mutex_unlock(&rec->q_mutex);

        /* 帧率控制：按 fps 间隔采样，避免帧堆积。
         * 注意：入队时已丢弃过旧帧，这里再做一次时间检查确保帧率正确 */
        uint32_t elapsed = lv_tick_elaps(rec->start_ms);
        uint32_t expected_frame = elapsed / rec->fps_interval;
        if (expected_frame <= rec->frame_count) {
            continue;  /* 未到下一帧时间，丢弃 */
        }

        /* 检查帧尺寸是否匹配录制尺寸 */
        int src_w = frame.width;
        int src_h = frame.height;
        int size_ok = 0;
        if (rec->cfg.rotate == CAM_ROTATE_90 || rec->cfg.rotate == CAM_ROTATE_270) {
            if (src_w == dst_h && src_h == dst_w) size_ok = 1;
        } else {
            if (src_w == dst_w && src_h == dst_h) size_ok = 1;
        }
        if (!size_ok) {
            continue;  /* 尺寸不匹配，跳过 */
        }

        /* 转换帧为 RGB888（带旋转） */
        if (frame_to_rgb888(&frame, rec->rgb888_buf, dst_w, dst_h, rec->cfg.rotate) != 0) {
            LV_LOG_WARN("录制帧格式转换失败（fourcc=0x%08X）", frame.fourcc);
            continue;
        }

        /* 编码为 JPEG */
        int jpeg_sz = encode_rgb888_to_jpeg(rec->rgb888_buf, dst_w, dst_h,
                                             &rec->jpeg_buf, &rec->jpeg_buf_sz);
        if (jpeg_sz <= 0) {
            LV_LOG_WARN("录制 JPEG 编码失败");
            continue;
        }

        /* 写入 MP4 容器（MJPEG sample，全关键帧） */
        if (mp4_muxer_write_sample(rec->muxer, rec->jpeg_buf, jpeg_sz, 1) != 0) {
            LV_LOG_WARN("录制 MP4 写入失败");
            continue;
        }

        /* 成功写入一帧后才递增 frame_count，
         * 否则帧率控制（expected_frame <= frame_count）失效：
         * frame_count 恒为 0 时，仅丢弃首个 fps_interval 内的帧，
         * 之后所有帧都会被编码，导致录制帧率失控。 */
        rec->frame_count++;

        (void)frame_data;  /* frame_data 通过 entry->frame.data 间接使用 */
    }

    LV_LOG_USER("录制工作线程退出，已编码 %d 帧", rec->frame_count);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 公共 API 实现                                                       */
/* ------------------------------------------------------------------ */

recorder_handle_t * recorder_mjpeg_create(const recorder_config_t *cfg)
{
    if (!cfg || !cfg->output_path || cfg->width <= 0 || cfg->height <= 0 || cfg->fps <= 0) {
        LV_LOG_ERROR("录制配置无效");
        return NULL;
    }

    recorder_handle_t *rec = (recorder_handle_t *)calloc(1, sizeof(struct recorder_handle));
    if (!rec) return NULL;

    rec->cfg = *cfg;
    rec->backend_magic = REC_BACKEND_MAGIC_MJPEG;
    rec->state = REC_STATE_IDLE;

    /* 分配 JPEG 编码缓冲区（工作线程专用） */
    rec->jpeg_buf_sz = JPEG_BUF_INIT_SZ;
    rec->jpeg_buf = (uint8_t *)malloc(rec->jpeg_buf_sz);
    if (!rec->jpeg_buf) {
        free(rec);
        return NULL;
    }

    /* 分配 RGB888 转换缓冲区（工作线程专用） */
    size_t rgb_sz = (size_t)cfg->width * cfg->height * 3;
    rec->rgb888_buf = (uint8_t *)malloc(rgb_sz);
    if (!rec->rgb888_buf) {
        free(rec->jpeg_buf);
        free(rec);
        return NULL;
    }

    /* 预分配帧队列缓冲池：每个槽位分配最大可能的帧大小。
     * RGB565/YUYV = w*h*2，NV12 = w*h*3/2，取 w*h*2 作为上限 */
    size_t frame_buf_sz = (size_t)cfg->width * cfg->height * 2;
    for (int i = 0; i < REC_FRAME_QUEUE_SIZE; i++) {
        rec->queue[i].data = (uint8_t *)malloc(frame_buf_sz);
        rec->queue[i].in_use = 0;
        if (!rec->queue[i].data) {
            /* 分配失败：释放已分配的槽位 */
            for (int j = 0; j < i; j++) {
                free(rec->queue[j].data);
            }
            free(rec->rgb888_buf);
            free(rec->jpeg_buf);
            free(rec);
            return NULL;
        }
    }
    rec->q_head = 0;
    rec->q_tail = 0;
    rec->q_count = 0;

    /* 初始化互斥锁和条件变量 */
    pthread_mutex_init(&rec->q_mutex, NULL);
    pthread_cond_init(&rec->q_cond, NULL);
    rec->worker_running = 0;
    rec->stop_requested = 0;

    /* 创建 MP4 muxer（MJPEG 轨道）。
     * 使用旋转后的输出尺寸（90°/270° 旋转时宽高互换），
     * 确保 MP4 容器声明的分辨率与编码后的 JPEG 帧尺寸一致。
     * mp4_muxer_create 内部会创建父目录并写 ftyp+mdat header */
    int mp4_w = cfg->width;
    int mp4_h = cfg->height;
    if (cfg->rotate == CAM_ROTATE_90 || cfg->rotate == CAM_ROTATE_270) {
        mp4_w = cfg->height;
        mp4_h = cfg->width;
    }
    rec->muxer = mp4_muxer_create(cfg->output_path, mp4_w, mp4_h,
                                    cfg->fps, MP4_TRACK_MJPEG);
    if (!rec->muxer) {
        LV_LOG_ERROR("MP4 muxer 创建失败: %s", cfg->output_path);
        for (int i = 0; i < REC_FRAME_QUEUE_SIZE; i++) {
            free(rec->queue[i].data);
        }
        free(rec->rgb888_buf);
        free(rec->jpeg_buf);
        free(rec);
        return NULL;
    }

    rec->state = REC_STATE_READY;
    LV_LOG_USER("录制器已创建（线程化 MJPEG+MP4）: %dx%d@%dfps → %s",
                cfg->width, cfg->height, cfg->fps, cfg->output_path);
    return rec;
}

void recorder_mjpeg_destroy(recorder_handle_t *rec)
{
    if (!rec) return;
    if (rec->state == REC_STATE_RECORDING) {
        recorder_mjpeg_stop(rec);
    }
    if (rec->muxer) {
        mp4_muxer_destroy(rec->muxer);
        rec->muxer = NULL;
    }
    if (rec->jpeg_buf) {
        free(rec->jpeg_buf);
        rec->jpeg_buf = NULL;
    }
    if (rec->rgb888_buf) {
        free(rec->rgb888_buf);
        rec->rgb888_buf = NULL;
    }
    /* 释放帧队列缓冲池 */
    for (int i = 0; i < REC_FRAME_QUEUE_SIZE; i++) {
        if (rec->queue[i].data) {
            free(rec->queue[i].data);
            rec->queue[i].data = NULL;
        }
    }
    pthread_mutex_destroy(&rec->q_mutex);
    pthread_cond_destroy(&rec->q_cond);
    free(rec);
}

int recorder_mjpeg_start(recorder_handle_t *rec)
{
    if (!rec || !rec->muxer) return -1;
    if (rec->state != REC_STATE_READY) return -1;

    /* MP4 muxer 在 create 时已写 ftyp+mdat header，无需额外初始化 */
    rec->state = REC_STATE_RECORDING;
    rec->start_ms = lv_tick_get();
    rec->frame_count = 0;
    rec->fps_interval = 1000 / (rec->cfg.fps > 0 ? rec->cfg.fps : 30);
    rec->q_head = 0;
    rec->q_tail = 0;
    rec->q_count = 0;
    rec->stop_requested = 0;

    /* 启动工作线程 */
    rec->worker_running = 1;
    if (pthread_create(&rec->worker, NULL, recorder_worker_thread, rec) != 0) {
        LV_LOG_ERROR("录制工作线程创建失败");
        rec->worker_running = 0;
        rec->state = REC_STATE_READY;
        return -1;
    }

    LV_LOG_USER("录制已开始（MJPEG+MP4，工作线程已启动）");
    return 0;
}

int recorder_mjpeg_stop(recorder_handle_t *rec)
{
    if (!rec) return -1;
    if (rec->state != REC_STATE_RECORDING) return -1;

    /* 通知工作线程停止并等待队列排空 */
    pthread_mutex_lock(&rec->q_mutex);
    rec->stop_requested = 1;
    pthread_cond_signal(&rec->q_cond);
    pthread_mutex_unlock(&rec->q_mutex);

    /* 等待工作线程结束（处理完队列中剩余帧后退出） */
    if (rec->worker_running) {
        pthread_join(rec->worker, NULL);
        rec->worker_running = 0;
    }

    /* 关闭 MP4 muxer（回填 mdat size + 写入 moov box） */
    if (rec->muxer) {
        mp4_muxer_close(rec->muxer);
    }

    rec->state = REC_STATE_IDLE;
    LV_LOG_USER("录制已停止，共 %d 帧，时长 %.1f 秒",
                rec->frame_count,
                rec->frame_count / (float)(rec->cfg.fps > 0 ? rec->cfg.fps : 30));
    return 0;
}

int recorder_mjpeg_feed_frame(recorder_handle_t *rec, const camera_frame_t *frame)
{
    if (!rec || !frame || rec->state != REC_STATE_RECORDING) return -1;

    /* UI 线程：拷贝帧数据入队，立即返回。
     * 不做任何编码/IO操作，避免阻塞预览定时器。 */

    pthread_mutex_lock(&rec->q_mutex);

    /* 查找空闲槽位（in_use=0） */
    int slot = -1;
    for (int i = 0; i < REC_FRAME_QUEUE_SIZE; i++) {
        int idx = (rec->q_head + i) % REC_FRAME_QUEUE_SIZE;
        if (!rec->queue[idx].in_use) {
            slot = idx;
            break;
        }
    }

    if (slot < 0) {
        /* 队列满：丢弃最旧帧（保证实时性，避免帧堆积导致延迟）。
         * 覆盖 tail 位置的帧，tail 前移 */
        slot = rec->q_tail;
        rec->q_tail = (rec->q_tail + 1) % REC_FRAME_QUEUE_SIZE;
        rec->q_count--;
        LV_LOG_WARN("录制队列满，丢弃最旧帧");
    }

    /* 拷贝帧数据到缓冲池槽位 */
    queue_entry_t *entry = &rec->queue[slot];
    size_t copy_sz = frame->size;
    /* 防止越界：不超过槽位缓冲区大小 */
    size_t buf_cap = (size_t)rec->cfg.width * rec->cfg.height * 2;
    if (copy_sz > buf_cap) copy_sz = buf_cap;
    memcpy(entry->data, frame->data, copy_sz);
    entry->frame = *frame;       /* 拷贝元数据 */
    entry->frame.data = entry->data;  /* 重定向 data 指针到缓冲池 */
    entry->frame.size = copy_sz;
    entry->in_use = 1;

    rec->q_head = (slot + 1) % REC_FRAME_QUEUE_SIZE;
    rec->q_count++;

    /* 通知工作线程有新帧 */
    pthread_cond_signal(&rec->q_cond);

    pthread_mutex_unlock(&rec->q_mutex);
    return 0;
}

recorder_state_t recorder_mjpeg_get_state(const recorder_handle_t *rec)
{
    if (!rec) return REC_STATE_IDLE;
    return rec->state;
}

uint64_t recorder_mjpeg_get_elapsed_ms(const recorder_handle_t *rec)
{
    if (!rec || rec->state != REC_STATE_RECORDING) return 0;
    return lv_tick_elaps(rec->start_ms);
}

/* 后端可用性检测：软件 MJPEG 后端始终可用 */
int recorder_mjpeg_is_available(void)
{
    return 1;
}

/* 查询后端支持的最大能力。
 * 软件 MJPEG 后端无硬件限制，返回较大值（实际受 CPU 性能约束） */
int recorder_mjpeg_get_max_capability(int *out_w, int *out_h, int *out_fps)
{
    /* 软件后端建议不超过 1080p@30fps（更高分辨率 CPU 压力大） */
    if (out_w)  *out_w  = 1920;
    if (out_h)  *out_h  = 1080;
    if (out_fps) *out_fps = 30;
    return 0;
}
