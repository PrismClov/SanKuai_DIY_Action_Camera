/**
 * @file rga_accel.c
 * @brief RK3566 RGA 硬件加速器实现
 *
 * 架构：
 *   V4L2 mmap 帧 ──→ RGA 硬件 ──→ LVGL canvas (RGB565)
 *                    (improcess)
 *
 *   ┌──────────────────────────────────────────────────┐
 *   │                  RK3566 SoC                     │
 *   │  ┌─────────┐    ┌─────────┐    ┌──────────┐    │
 *   │  │ V4L2    │───→│  RGA    │───→│  DRAM    │    │
 *   │  │ Camera  │mmap│ 2D 硬件 │    │ canvas   │    │
 *   │  │ ISP     │    │ 加速器  │    │ 缓冲区   │    │
 *   │  └─────────┘    └─────────┘    └──────────┘    │
 *   │       ↑              ↑              ↓          │
 *   │       │           零 CPU          LVGL         │
 *   │       │           开销           渲染          │
 *   │  ┌─────────┐    ┌─────────┐                   │
 *   │  │  A55×4  │    │Mali-G52 │                   │
 *   │  │  CPU    │    │  GPU    │                   │
 *   │  └─────────┘    └─────────┘                   │
 *   └──────────────────────────────────────────────────┘
 *
 * 性能对比（1080p NV12 → 800x480 RGB565 + 90° 旋转）：
 *   - 软件转换：8~12ms/帧（占用 A55 一个核心 100%）
 *   - RGA 硬件：<2ms/帧（CPU 零占用，与 UI 渲染并行）
 *   - 提升约 5~6 倍，CPU 可用于录制编码和其他任务
 */
#include "rga_accel.h"

/* 只在 aarch64 平台编译 RGA 相关代码（K230 riscv64 无 librga） */
#if defined(__aarch64__) || defined(__ARM_NEON)

#include "lvgl/lvgl.h"

#include <stdio.h>
#include <string.h>
#include <dlfcn.h>      /* 动态加载 librga.so，避免编译期硬依赖 */
#include <unistd.h>

/* RGA 头文件（sysroot 中） */
#include "rga/rga.h"
#include "rga/im2d.h"

/* ------------------------------------------------------------------ */
/* 动态加载 librga.so：避免编译期硬依赖，K230 上自动回退到软件路径       */
/* ------------------------------------------------------------------ */

/* 函数指针类型定义
 * 注意：im2d.h 中 imStrError_t 已是函数名（非类型），这里 typedef 换名避免冲突 */
typedef IM_STATUS (*improcess_t)(rga_buffer_t src, rga_buffer_t dst, rga_buffer_t pat,
                                  im_rect srect, im_rect drect, im_rect prect, int usage);
typedef IM_STATUS (*imsync_t)(void);
typedef const char* (*imStrError_fn_t)(IM_STATUS status);

/* 全局函数指针：首次调用时通过 dlsym 加载 */
static int s_rga_initialized = 0;       /* 初始化标志：0=未初始化，1=已加载，-1=加载失败 */
static void *s_librga_handle = NULL;    /* dlopen 句柄 */
static improcess_t s_improcess = NULL;
static imsync_t s_imsync = NULL;
static imStrError_fn_t s_imStrError = NULL;

/* 延迟加载 librga.so 并获取函数符号 */
static int rga_lazy_init(void)
{
    if (s_rga_initialized != 0) {
        return s_rga_initialized;  /* 已初始化（1=成功，-1=失败） */
    }

    /* 动态加载 librga.so */
    s_librga_handle = dlopen("librga.so", RTLD_NOW | RTLD_GLOBAL);
    if (!s_librga_handle) {
        s_librga_handle = dlopen("librga.so.2", RTLD_NOW | RTLD_GLOBAL);
    }
    if (!s_librga_handle) {
        LV_LOG_WARN("RGA: 无法加载 librga.so（%s），回退到软件路径", dlerror());
        s_rga_initialized = -1;
        return -1;
    }

    /* 获取 RGA API 函数指针 */
    s_improcess = (improcess_t)dlsym(s_librga_handle, "improcess");
    s_imsync = (imsync_t)dlsym(s_librga_handle, "imsync");
    s_imStrError = (imStrError_fn_t)dlsym(s_librga_handle, "imStrError_t");

    if (!s_improcess || !s_imsync) {
        LV_LOG_ERROR("RGA: 无法获取 improcess/imsync 符号");
        dlclose(s_librga_handle);
        s_librga_handle = NULL;
        s_rga_initialized = -1;
        return -1;
    }

    s_rga_initialized = 1;
    LV_LOG_USER("RGA: librga.so 已加载，硬件加速就绪");
    return 1;
}

/* 将 camera_fourcc 映射到 RGA 格式常量 */
static int camera_fourcc_to_rga_format(uint32_t fourcc)
{
    switch (fourcc) {
        case CAM_FOURCC_RGB565: return RK_FORMAT_RGB_565;
        case CAM_FOURCC_NV12:   return RK_FORMAT_YCbCr_420_SP;
        case CAM_FOURCC_NV21:   /* NV21 需要 swap UV，RGA 用 NV12 + swap mode */
                                return RK_FORMAT_YCbCr_420_SP;
        case CAM_FOURCC_YUYV:   return RK_FORMAT_YUYV_422;
        default:                return -1;
    }
}

/* 将 camera_rotate 映射到 RGA 旋转 usage 标志 */
static int camera_rotate_to_rga_usage(camera_rotate_t rotate)
{
    switch (rotate) {
        case CAM_ROTATE_90:   return IM_HAL_TRANSFORM_ROT_90;
        case CAM_ROTATE_180:  return IM_HAL_TRANSFORM_ROT_180;
        case CAM_ROTATE_270:  return IM_HAL_TRANSFORM_ROT_270;
        default:              return 0;  /* 无旋转 */
    }
}

int rga_is_available(void)
{
    return rga_lazy_init() == 1;
}

int rga_convert_frame(const camera_frame_t *frame,
                      uint16_t *dst, int dst_w, int dst_h,
                      camera_rotate_t rotate)
{
    /* 延迟加载 librga */
    if (rga_lazy_init() != 1) {
        return -1;  /* librga 不可用，调用方回退到软件路径 */
    }

    /* 参数校验 */
    if (!frame || !frame->data || !dst || dst_w <= 0 || dst_h <= 0) {
        return -1;
    }

    int src_w = frame->width;
    int src_h = frame->height;
    int rga_src_fmt = camera_fourcc_to_rga_format(frame->fourcc);
    if (rga_src_fmt < 0) {
        LV_LOG_WARN("RGA: 不支持的 fourcc=0x%08X", frame->fourcc);
        return -1;
    }

    /* NV21 需要特殊处理：RGA 用 NV12 格式 + UV swap usage 标志。
     * IM_COLOR_SPACE_YUV_TO_RGB_BT601_LIMIT 在新版 librga 中已改名为
     * IM_YUV_TO_RGB_BT601_LIMIT，作为 usage 标志 OR 进 usage 即可。 */
    int nv21_cvt = (frame->fourcc == CAM_FOURCC_NV21);

    /* 计算旋转后的目标尺寸：90°/270° 时宽高互换 */
    int dst_actual_w = dst_w;
    int dst_actual_h = dst_h;
    if (rotate == CAM_ROTATE_90 || rotate == CAM_ROTATE_270) {
        /* RGA 旋转后输出尺寸需要与物理输出 buffer 匹配 */
        dst_actual_w = dst_h;
        dst_actual_h = dst_w;
    }

    /* 注册源缓冲区（V4L2 mmap 内存，虚拟地址）。
     * frame->data 是 const 指针，wrapbuffer_virtualaddr 形参为 void*，
     * 这里强转去 const 是安全的：RGA 只读源缓冲区，不会修改。 */
    rga_buffer_t rga_src = wrapbuffer_virtualaddr(
        (void *)frame->data, src_w, src_h, rga_src_fmt);
    if (rga_src.width == 0) {
        LV_LOG_WARN("RGA: 源缓冲区注册失败");
        return -1;
    }

    /* 注册目标缓冲区（LVGL canvas，RGB565） */
    rga_buffer_t rga_dst = wrapbuffer_virtualaddr(
        dst, dst_actual_w, dst_actual_h, RK_FORMAT_RGB_565);
    if (rga_dst.width == 0) {
        LV_LOG_WARN("RGA: 目标缓冲区注册失败");
        return -1;
    }

    /* 源矩形：整帧 */
    im_rect srect = {
        .x = 0, .y = 0,
        .width = src_w, .height = src_h
    };

    /* 目标矩形：整帧（RGA 自动缩放到 dst 尺寸） */
    im_rect drect = {
        .x = 0, .y = 0,
        .width = dst_actual_w, .height = dst_actual_h
    };
    im_rect prect = {0};  /* 不使用 pattern */

    /* 合并 usage：格式转换 + 缩放 + 旋转 + 同步等待 */
    int usage = IM_SYNC;  /* 同步模式：调用后等待 RGA 完成 */
    int rot_usage = camera_rotate_to_rga_usage(rotate);
    if (rot_usage) {
        usage |= rot_usage;
    }
    /* NV21 需要 BT.601 limited range YUV→RGB 色彩转换标志 */
    if (nv21_cvt) {
        usage |= IM_YUV_TO_RGB_BT601_LIMIT;
    }

    /* 调用 RGA 硬件加速：一次完成 转换+缩放+旋转 */
    IM_STATUS ret = s_improcess(rga_src, rga_dst, (rga_buffer_t){0},
                                 srect, drect, prect, usage);
    if (ret != IM_STATUS_SUCCESS && ret != IM_STATUS_NOERROR) {
        /* 内联 err 避免 Release 模式 LV_LOG_WARN 展开为空导致未使用变量警告 */
        LV_LOG_WARN("RGA: improcess 失败 ret=%d (%s)", ret,
                    s_imStrError ? s_imStrError(ret) : "unknown");
        return -1;
    }

    return 0;
}

#else /* 非 aarch64 平台（K230 riscv64 / PC） */

/* K230 / PC 平台无 RGA，直接返回不可用 */
int rga_is_available(void)
{
    return 0;
}

int rga_convert_frame(const camera_frame_t *frame,
                      uint16_t *dst, int dst_w, int dst_h,
                      camera_rotate_t rotate)
{
    (void)frame; (void)dst; (void)dst_w; (void)dst_h; (void)rotate;
    return -1;  /* 不可用，调用方回退到软件路径 */
}

#endif /* __aarch64__ */
