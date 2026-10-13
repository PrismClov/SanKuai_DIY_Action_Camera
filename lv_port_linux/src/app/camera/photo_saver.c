/**
 * @file photo_saver.c
 * @brief 拍照保存实现：RGB565 → JPEG（libjpeg）/ BMP（回退）
 *
 * 编码流程：
 *   1. RGB565 → RGB888 转换（LVGL v9 canvas 正序行，直接转换无需翻转）
 *   2. libjpeg 压缩为 JPEG（质量 90）
 *   3. 保存到 dir/IMG_YYYYMMDD_HHMMSS.jpg
 *
 * libjpeg 不可用时回退为 BMP（无压缩，文件大但兼容性好）。
 *
 * 异步拍照（photo_save_jpeg_scaled_async）：
 *   后台线程执行 JPEG 编码，不阻塞 UI 线程。
 *   UI 线程通过 photo_is_busy() 轮询状态，photo_async_done() 获取完成通知。
 */
#include "photo_saver.h"

#include "lvgl/lvgl.h"   /* LV_LOG_* 日志 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <setjmp.h>  /* libjpeg 错误处理：setjmp/longjmp */
#include <pthread.h>  /* 异步拍照后台线程 */
#include <unistd.h>
/* 尝试启用 libjpeg：buildroot 交叉编译环境通常提供 libjpeg */
#if __has_include(<jpeglib.h>)
#define USE_LIBJPEG 1
#include <jpeglib.h>
#else
#define USE_LIBJPEG 0
#endif

/* 上次保存的文件路径（供 photo_last_path 查询） */
static char s_last_path[256] = {0};

/* 连拍序号：> 0 时文件名追加 _NN 后缀避免连拍同秒覆盖。
 * 由 photo_set_burst_seq 设置，连拍结束后应置 0 */
static int s_burst_seq = 0;

/* ------------------------------------------------------------------ */
/* 工具函数                                                            */
/* ------------------------------------------------------------------ */

/* 递归创建目录（类似 mkdir -p） */
static int mkdir_p(const char *path)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len == 0) return -1;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                LV_LOG_WARN("mkdir %s 失败: %s", tmp, strerror(errno));
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        LV_LOG_WARN("mkdir %s 失败: %s", tmp, strerror(errno));
        return -1;
    }
    return 0;
}

/* 生成带时间戳的文件名：IMG_YYYYMMDD_HHMMSS.jpg
 * 连拍模式（s_burst_seq > 0）追加 _NN 后缀：IMG_YYYYMMDD_HHMMSS_03.jpg */
static void make_filename(char *buf, size_t buf_sz, const char *dir, const char *ext)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char ts[32];
    /* localtime 理论可能返回 NULL（时区配置异常），此时回退到时间戳数字 */
    if (t) {
        strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", t);
    } else {
        snprintf(ts, sizeof(ts), "%lld", (long long)now);
    }
    if (s_burst_seq > 0) {
        /* 连拍：追加 2 位序号后缀，避免同秒多张覆盖 */
        snprintf(buf, buf_sz, "%s/IMG_%s_%02d.%s", dir, ts, s_burst_seq, ext);
    } else {
        snprintf(buf, buf_sz, "%s/IMG_%s.%s", dir, ts, ext);
    }
}

/* 设置连拍序号。seq > 0 时文件名追加 _NN 后缀 */
void photo_set_burst_seq(int seq)
{
    s_burst_seq = seq > 0 ? seq : 0;
}

/* ------------------------------------------------------------------ */
/* RGB565 → RGB888 转换
 * LVGL v9 canvas 的 RGB565 数据在内存中是正序行（顶朝下，第 0 行在最上方），
 * JPEG 也需要正序行，因此直接逐行拷贝，无需翻转。 */
/* ------------------------------------------------------------------ */
static uint8_t * rgb565_to_rgb888(const uint16_t *rgb565, int w, int h)
{
    /* 分配 RGB888 缓冲区：每像素 3 字节 */
    uint8_t *rgb888 = (uint8_t *)malloc((size_t)w * h * 3);
    if (!rgb888) return NULL;

    /* LVGL v9 canvas 内存布局：第 0 行在最上方（正序）。
     * JPEG 也使用正序行，直接逐行逐像素转换。 */
    for (int y = 0; y < h; y++) {
        const uint16_t *src_row = rgb565 + (size_t)y * w;
        uint8_t *dst_row = rgb888 + (size_t)y * w * 3;
        for (int x = 0; x < w; x++) {
            uint16_t px = src_row[x];
            /* RGB565: R(5) G(6) B(5)，扩展到 8 位 */
            dst_row[x * 3 + 0] = (uint8_t)((px >> 11) & 0x1F) << 3;  /* R */
            dst_row[x * 3 + 1] = (uint8_t)((px >> 5) & 0x3F) << 2;   /* G */
            dst_row[x * 3 + 2] = (uint8_t)(px & 0x1F) << 3;           /* B */
        }
    }
    return rgb888;
}

/* ------------------------------------------------------------------ */
/* JPEG 编码保存（libjpeg）                                            */
/* ------------------------------------------------------------------ */
#if USE_LIBJPEG
/* libjpeg 错误处理上下文：用于 setjmp/longjmp 跳过致命错误，
 * 避免默认 error_exit 调用 exit() 终止整个进程 */
struct jpeg_err_ctx {
    struct jpeg_error_mgr pub;  /* libjpeg 公共错误管理器 */
    jmp_buf jb;                 /* 错误恢复跳转点 */
};

/* libjpeg 致命错误回调：不调用 exit()，而是 longjmp 回到 setjmp 点 */
static void jpeg_error_exit(j_common_ptr cinfo)
{
    struct jpeg_err_ctx *err = (struct jpeg_err_ctx *)cinfo->err;
    longjmp(err->jb, 1);
}

static int save_jpeg_file(const uint8_t *rgb888, int w, int h, const char *path)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        LV_LOG_ERROR("无法创建 JPEG 文件: %s (%s)", path, strerror(errno));
        return -1;
    }

    struct jpeg_compress_struct cinfo;
    struct jpeg_err_ctx jerr;

    /* 安装自定义错误处理器，避免 libjpeg 出错时调用 exit() */
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit;

    /* setjmp 建立错误恢复点：libjpeg 后续调用出错时会跳回这里 */
    if (setjmp(jerr.jb)) {
        /* 错误路径：libjpeg 调用了 error_exit，longjmp 跳回这里。
         * 必须释放资源并返回错误，避免进程崩溃导致用户数据丢失 */
        jpeg_destroy_compress(&cinfo);
        fclose(fp);
        LV_LOG_ERROR("JPEG 编码失败: %s", path);
        return -1;
    }

    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, fp);

    cinfo.image_width = (JDIMENSION)w;
    cinfo.image_height = (JDIMENSION)h;
    cinfo.input_components = 3;       /* RGB888 */
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 90, TRUE);  /* 质量 90 */
    jpeg_start_compress(&cinfo, TRUE);

    /* 逐行写入 */
    JSAMPROW row_pointer[1];
    while (cinfo.next_scanline < cinfo.image_height) {
        row_pointer[0] = (JSAMPROW)(rgb888 + (size_t)cinfo.next_scanline * w * 3);
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    fclose(fp);
    return 0;
}
#endif /* USE_LIBJPEG */

/* ------------------------------------------------------------------ */
/* BMP 保存（libjpeg 不可用时的回退方案）                              */
/* ------------------------------------------------------------------ */
#if !USE_LIBJPEG
static int save_bmp_file(const uint8_t *rgb888, int w, int h, const char *path)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        LV_LOG_ERROR("无法创建 BMP 文件: %s (%s)", path, strerror(errno));
        return -1;
    }

    /* BMP 文件头（14 字节）+ 信息头（40 字节） */
    uint32_t row_size = ((uint32_t)w * 3 + 3) & ~3u;  /* 4 字节对齐 */
    uint32_t data_size = row_size * (uint32_t)h;
    uint32_t file_size = 54 + data_size;

    /* 文件头 */
    uint8_t fh[14] = {0};
    fh[0] = 'B'; fh[1] = 'M';
    fh[2] = file_size & 0xFF;
    fh[3] = (file_size >> 8) & 0xFF;
    fh[4] = (file_size >> 16) & 0xFF;
    fh[5] = (file_size >> 24) & 0xFF;
    fh[10] = 54;  /* 数据偏移 */
    fwrite(fh, 1, 14, fp);

    /* 信息头（BITMAPINFOHEADER） */
    uint8_t ih[40] = {0};
    ih[0] = 40;   /* 头大小 */
    ih[4] = w & 0xFF; ih[5] = (w >> 8) & 0xFF; ih[6] = (w >> 16) & 0xFF; ih[7] = (w >> 24) & 0xFF;
    ih[8] = h & 0xFF; ih[9] = (h >> 8) & 0xFF; ih[10] = (h >> 16) & 0xFF; ih[11] = (h >> 24) & 0xFF;
    ih[12] = 1;   /* planes */
    ih[14] = 24;  /* bpp */
    ih[20] = data_size & 0xFF; ih[21] = (data_size >> 8) & 0xFF;
    ih[22] = (data_size >> 16) & 0xFF; ih[23] = (data_size >> 24) & 0xFF;
    fwrite(ih, 1, 40, fp);

    /* 像素数据（BMP 是 BGR 顺序，底朝下）。
     * rgb888 已是正序（顶朝下），BMP 需要底朝下，因此倒序写入。
     * BMP 像素是 BGR，我们的 rgb888 是 RGB，需交换 R/B。 */
    uint8_t *row = (uint8_t *)malloc(row_size);
    if (!row) { fclose(fp); return -1; }

    for (int y = h - 1; y >= 0; y--) {
        const uint8_t *src = rgb888 + (size_t)y * w * 3;
        memset(row, 0, row_size);
        for (int x = 0; x < w; x++) {
            row[x * 3 + 0] = src[x * 3 + 2];  /* B */
            row[x * 3 + 1] = src[x * 3 + 1];  /* G */
            row[x * 3 + 2] = src[x * 3 + 0];  /* R */
        }
        fwrite(row, 1, row_size, fp);
    }
    free(row);
    fclose(fp);
    return 0;
}
#endif /* !USE_LIBJPEG */

/* ------------------------------------------------------------------ */
/* 公共接口                                                            */
/* ------------------------------------------------------------------ */
int photo_save_jpeg(const uint16_t *rgb565, int width, int height, const char *dir)
{
    if (!rgb565 || width <= 0 || height <= 0 || !dir) return -1;

    /* 确保目录存在 */
    if (mkdir_p(dir) != 0) {
        LV_LOG_ERROR("无法创建拍照目录: %s", dir);
        return -1;
    }

    /* RGB565 → RGB888（LVGL v9 canvas 正序行，直接转换） */
    uint8_t *rgb888 = rgb565_to_rgb888(rgb565, width, height);
    if (!rgb888) {
        LV_LOG_ERROR("RGB888 缓冲区分配失败 (%dx%d)", width, height);
        return -1;
    }

    /* 生成文件名并保存 */
#if USE_LIBJPEG
    make_filename(s_last_path, sizeof(s_last_path), dir, "jpg");
    int ret = save_jpeg_file(rgb888, width, height, s_last_path);
#else
    make_filename(s_last_path, sizeof(s_last_path), dir, "bmp");
    int ret = save_bmp_file(rgb888, width, height, s_last_path);
    LV_LOG_WARN("libjpeg 不可用，已回退为 BMP 格式: %s", s_last_path);
#endif

    free(rgb888);

    if (ret == 0) {
        LV_LOG_USER("拍照保存成功: %s (%dx%d)", s_last_path, width, height);
    } else {
        s_last_path[0] = '\0';
    }
    return ret;
}

const char * photo_last_path(void)
{
    return s_last_path;
}

/* ------------------------------------------------------------------ */
/* 画幅裁剪 + 缩放保存                                                 */
/* ------------------------------------------------------------------ */

/* RGB565 中心裁剪：按目标比例从源图像中心裁剪出最大区域。
 * ratio: 0=4:3, 1=16:9, 2=1:1
 * 输出到 out_w/out_h/out_buf（调用方负责释放 *out_buf）。
 * 返回 0 成功，负数失败。 */
static int rgb565_center_crop(const uint16_t *src, int src_w, int src_h,
                               int ratio,
                               uint16_t **out_buf, int *out_w, int *out_h)
{
    /* 目标宽高比（浮点避免整数除法精度问题） */
    double target_ratio;
    switch (ratio) {
        case 0:  target_ratio = 4.0 / 3.0;   break;  /* 4:3 */
        case 1:  target_ratio = 16.0 / 9.0;  break;  /* 16:9 */
        case 2:  target_ratio = 1.0;         break;  /* 1:1 */
        default: target_ratio = 16.0 / 9.0;  break;
    }

    double src_ratio = (double)src_w / (double)src_h;

    int crop_w, crop_h;
    /* 若源图比目标更宽，按高度裁剪宽度；否则按宽度裁剪高度 */
    if (src_ratio > target_ratio) {
        crop_h = src_h;
        crop_w = (int)(src_h * target_ratio + 0.5);
    } else {
        crop_w = src_w;
        crop_h = (int)(src_w / target_ratio + 0.5);
    }

    /* 边界保护 */
    if (crop_w > src_w) crop_w = src_w;
    if (crop_h > src_h) crop_h = src_h;
    if (crop_w <= 0 || crop_h <= 0) return -1;

    /* 中心偏移 */
    int off_x = (src_w - crop_w) / 2;
    int off_y = (src_h - crop_h) / 2;

    /* 分配裁剪结果缓冲区 */
    uint16_t *dst = (uint16_t *)malloc((size_t)crop_w * crop_h * sizeof(uint16_t));
    if (!dst) return -1;

    /* 逐行拷贝裁剪区域 */
    for (int y = 0; y < crop_h; y++) {
        const uint16_t *src_row = src + (size_t)(off_y + y) * src_w + off_x;
        memcpy(dst + (size_t)y * crop_w, src_row, (size_t)crop_w * sizeof(uint16_t));
    }

    *out_buf = dst;
    *out_w   = crop_w;
    *out_h   = crop_h;
    return 0;
}

/* RGB565 双线性缩放：将 src 缩放到 dst_w x dst_h。
 * 使用最近邻 + 坐标映射（双线性在 RGB565 上增益小且耗 CPU）。
 * 返回 0 成功，负数失败。 */
static int rgb565_resize_nearest(const uint16_t *src, int src_w, int src_h,
                                  uint16_t *dst, int dst_w, int dst_h)
{
    if (!src || !dst || dst_w <= 0 || dst_h <= 0) return -1;

    /* 预计算 X/Y 映射表，避免内层循环重复计算（嵌入式 CPU 友好） */
    int *map_x = (int *)malloc((size_t)dst_w * sizeof(int));
    int *map_y = (int *)malloc((size_t)dst_h * sizeof(int));
    if (!map_x || !map_y) {
        free(map_x);
        free(map_y);
        return -1;
    }

    for (int x = 0; x < dst_w; x++) {
        map_x[x] = (x * src_w) / dst_w;
        if (map_x[x] >= src_w) map_x[x] = src_w - 1;
    }
    for (int y = 0; y < dst_h; y++) {
        map_y[y] = (y * src_h) / dst_h;
        if (map_y[y] >= src_h) map_y[y] = src_h - 1;
    }

    for (int y = 0; y < dst_h; y++) {
        const uint16_t *src_row = src + (size_t)map_y[y] * src_w;
        uint16_t *dst_row = dst + (size_t)y * dst_w;
        for (int x = 0; x < dst_w; x++) {
            dst_row[x] = src_row[map_x[x]];
        }
    }

    free(map_x);
    free(map_y);
    return 0;
}

/* 将 RGB565 帧按指定画幅裁剪后缩放到目标尺寸保存为 JPEG。
 * 流程：
 *   1. 中心裁剪到目标画幅（4:3 / 16:9 / 1:1）
 *   2. 缩放到 target_w x target_h（对应 12MP/20MP/24MP/48MP 档位）
 *   3. RGB565 → RGB888 → JPEG 编码保存
 * 若 target_w/target_h <= 0，跳过缩放，按裁剪后原尺寸保存 */
int photo_save_jpeg_scaled(const uint16_t *rgb565, int src_w, int src_h,
                            int target_ratio, int target_w, int target_h,
                            const char *dir)
{
    if (!rgb565 || src_w <= 0 || src_h <= 0 || !dir) return -1;

    /* 确保目录存在 */
    if (mkdir_p(dir) != 0) {
        LV_LOG_ERROR("无法创建拍照目录: %s", dir);
        return -1;
    }

    /* 步骤 1：中心裁剪到目标画幅 */
    uint16_t *cropped = NULL;
    int crop_w = 0, crop_h = 0;
    if (rgb565_center_crop(rgb565, src_w, src_h, target_ratio,
                            &cropped, &crop_w, &crop_h) != 0) {
        LV_LOG_ERROR("画幅裁剪失败 (src=%dx%d, ratio=%d)",
                     src_w, src_h, target_ratio);
        return -1;
    }
    LV_LOG_USER("画幅裁剪: %dx%d → %dx%d (ratio=%d)",
                src_w, src_h, crop_w, crop_h, target_ratio);

    /* 步骤 2：缩放到目标尺寸（如指定） */
    uint16_t *final_buf = cropped;
    int final_w = crop_w, final_h = crop_h;
    int need_free_final = 0;  /* 标记 final_buf 是否为独立分配 */

    if (target_w > 0 && target_h > 0 &&
        (target_w != crop_w || target_h != crop_h)) {
        final_buf = (uint16_t *)malloc((size_t)target_w * target_h * sizeof(uint16_t));
        if (!final_buf) {
            free(cropped);
            LV_LOG_ERROR("缩放缓冲区分配失败 (%dx%d)", target_w, target_h);
            return -1;
        }
        need_free_final = 1;

        if (rgb565_resize_nearest(cropped, crop_w, crop_h,
                                    final_buf, target_w, target_h) != 0) {
            free(cropped);
            free(final_buf);
            LV_LOG_ERROR("图像缩放失败");
            return -1;
        }
        final_w = target_w;
        final_h = target_h;
        /* 裁剪缓冲区已无用，释放 */
        free(cropped);
        LV_LOG_USER("图像缩放: → %dx%d", final_w, final_h);
    }

    /* 步骤 3：RGB565 → RGB888（LVGL v9 canvas 正序行，直接转换） */
    uint8_t *rgb888 = rgb565_to_rgb888(final_buf, final_w, final_h);
    if (need_free_final) free(final_buf);

    if (!rgb888) {
        LV_LOG_ERROR("RGB888 缓冲区分配失败 (%dx%d)", final_w, final_h);
        return -1;
    }

    /* 步骤 4：生成文件名并保存 */
    make_filename(s_last_path, sizeof(s_last_path), dir, "jpg");
    int ret;
#if USE_LIBJPEG
    ret = save_jpeg_file(rgb888, final_w, final_h, s_last_path);
#else
    ret = save_bmp_file(rgb888, final_w, final_h, s_last_path);
    LV_LOG_WARN("libjpeg 不可用，已回退为 BMP 格式: %s", s_last_path);
#endif

    free(rgb888);

    if (ret == 0) {
        LV_LOG_USER("拍照保存成功: %s (%dx%d, ratio=%d)",
                    s_last_path, final_w, final_h, target_ratio);
    } else {
        s_last_path[0] = '\0';
    }
    return ret;
}

/* ------------------------------------------------------------------ */
/* 异步拍照（后台线程）                                                  */
/* ------------------------------------------------------------------ */

/* 异步拍照上下文：传递给后台线程的参数 */
typedef struct {
    uint16_t *src_buf;    /* 源 RGB565 数据拷贝（线程负责释放） */
    int       src_w;
    int       src_h;
    int       target_ratio;
    int       target_w;
    int       target_h;
    char      dir[256];
} photo_async_ctx_t;

/* 异步拍照状态（原子操作，无需锁） */
static volatile int s_async_busy = 0;     /* 1=后台线程正在编码 */
static volatile int s_async_done = 0;    /* 1=编码完成（UI 可读取结果） */
static volatile int s_async_result = 0;  /* 编码结果：0=成功，负数=失败 */
static pthread_t    s_async_thread;

/* 后台线程函数：执行 crop → resize → RGB888 → JPEG 编码 → 文件保存 */
static void * photo_async_worker(void *arg)
{
    photo_async_ctx_t *ctx = (photo_async_ctx_t *)arg;

    LV_LOG_USER("异步拍照: 开始编码 %dx%d → %dx%d (ratio=%d)",
                ctx->src_w, ctx->src_h, ctx->target_w, ctx->target_h, ctx->target_ratio);

    /* 调用同步编码函数（复用已有逻辑） */
    int ret = photo_save_jpeg_scaled(ctx->src_buf, ctx->src_w, ctx->src_h,
                                       ctx->target_ratio, ctx->target_w, ctx->target_h,
                                       ctx->dir);

    s_async_result = ret;
    s_async_done = 1;
    s_async_busy = 0;

    /* 释放源数据拷贝 */
    free(ctx->src_buf);
    free(ctx);

    LV_LOG_USER("异步拍照: 编码完成 ret=%d, path=%s", ret, photo_last_path());
    return NULL;
}

int photo_save_jpeg_scaled_async(const uint16_t *rgb565, int src_w, int src_h,
                                  int target_ratio, int target_w, int target_h,
                                  const char *dir)
{
    if (!rgb565 || src_w <= 0 || src_h <= 0 || !dir) return -1;

    /* 上一次异步保存仍在进行中：拒绝新请求 */
    if (s_async_busy) {
        LV_LOG_WARN("异步拍照: 上次编码仍在进行中，跳过本次请求");
        return -2;
    }

    /* 拷贝源数据到独立缓冲区（后台线程编码期间，UI 线程可能继续更新预览帧） */
    size_t buf_sz = (size_t)src_w * src_h * sizeof(uint16_t);
    uint16_t *src_copy = (uint16_t *)malloc(buf_sz);
    if (!src_copy) {
        LV_LOG_ERROR("异步拍照: 源数据拷贝失败 (%zu 字节)", buf_sz);
        return -1;
    }
    memcpy(src_copy, rgb565, buf_sz);

    /* 创建上下文 */
    photo_async_ctx_t *ctx = (photo_async_ctx_t *)malloc(sizeof(photo_async_ctx_t));
    if (!ctx) {
        free(src_copy);
        return -1;
    }
    ctx->src_buf      = src_copy;
    ctx->src_w        = src_w;
    ctx->src_h        = src_h;
    ctx->target_ratio = target_ratio;
    ctx->target_w     = target_w;
    ctx->target_h     = target_h;
    snprintf(ctx->dir, sizeof(ctx->dir), "%s", dir);

    /* 清除上次的完成标志 */
    s_async_done  = 0;
    s_async_result = 0;
    s_async_busy  = 1;

    /* 启动后台线程 */
    if (pthread_create(&s_async_thread, NULL, photo_async_worker, ctx) != 0) {
        LV_LOG_ERROR("异步拍照: 线程创建失败");
        free(src_copy);
        free(ctx);
        s_async_busy = 0;
        return -1;
    }
    pthread_detach(s_async_thread);  /* 自动回收，无需 join */

    LV_LOG_USER("异步拍照: 已启动后台线程 (%dx%d → %dx%d)",
                src_w, src_h, target_w, target_h);
    return 0;
}

int photo_is_busy(void)
{
    return s_async_busy;
}

int photo_async_done(void)
{
    if (s_async_done) {
        s_async_done = 0;  /* 读取后清零 */
        return 1;
    }
    return 0;
}
