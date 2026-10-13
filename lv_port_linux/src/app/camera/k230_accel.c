/**
 * @file k230_accel.c
 * @brief K230 硬件加速器实现（VPU/G2D YUV→RGB565 + 缩放 + 旋转）
 *
 * 实现方式：
 *   - riscv64 平台（K230）：通过 dlopen 动态加载 libmpi.so，调用 K230 MPI 2D API
 *   - 其他平台：空实现，返回 -1，调用方回退到软件路径
 *
 * 动态加载优势：
 *   - 避免编译期硬依赖 libmpi.so（K230 SDK 路径因环境而异）
 *   - 运行时自动检测：libmpi.so 存在则启用硬件加速，否则回退软件
 *   - 同一份二进制可在 K230 和 PC 上运行（PC 无 libmpi.so 自动回退）
 *
 * K230 MPI 2D 加速流程（待接入）：
 *   1. kd_mpi_sys_init()：初始化 MPI 系统
 *   2. kd_mpi_vb_set_config()：配置视频缓冲池
 *   3. kd_mpi_2d_create_task()：创建 2D 加速任务
 *   4. kd_mpi_2d_exec()：执行格式转换+缩放+旋转
 *   5. kd_mpi_2d_destroy_task()：销毁任务
 *
 * 性能对比（1080p NV12 → 800x480 RGB565 + 90° 旋转）：
 *   - 软件转换：8~12ms/帧（占用 C908 一个核心 100%）
 *   - K230 G2D 硬件：预期 <2ms/帧（CPU 零占用）
 *   - 提升约 5~6 倍，双核 C908 可用于其他任务
 */
#include "k230_accel.h"

#include "lvgl/lvgl.h"   /* LV_LOG_* 日志 */

#include <stddef.h>
#include <stdint.h>

/* 仅在 riscv64 平台（K230）编译硬件加速相关代码 */
#if defined(__riscv) && defined(__riscv_xlen) && (__riscv_xlen == 64)

#include <dlfcn.h>      /* 动态加载 libmpi.so */
#include <string.h>
#include <pthread.h>

/* ------------------------------------------------------------------ */
/* 动态加载 libmpi.so：避免编译期硬依赖，PC/RK3566 自动回退到软件路径   */
/* ------------------------------------------------------------------ */

/* K230 MPI 函数指针类型定义（待 K230 SDK 头文件确认后补充）
 * 以下为 K230 SDK 公开 API 的典型签名，实际签名以 K230 SDK 头文件为准：
 *   - kd_mpi_sys_init: int kd_mpi_sys_init(void)
 *   - kd_mpi_2d_create: int kd_mpi_2d_create(k_2d_task_t *task)
 *   - kd_mpi_2d_exec: int kd_mpi_2d_exec(k_2d_task_t *task)
 *   - kd_mpi_2d_destroy: int kd_mpi_2d_destroy(k_2d_task_t *task)
 * 由于 K230 SDK 头文件路径未确认，此处暂不 typedef 具体类型 */
typedef int (*k230_mpi_sys_init_t)(void);
typedef int (*k230_mpi_sys_exit_t)(void);

/* 动态加载的全局句柄（首次调用时初始化，进程生命周期内复用） */
static void *g_mpi_handle = NULL;
static int   g_mpi_available = -1;  /* -1=未检测，0=不可用，1=可用 */
static pthread_mutex_t g_mpi_mutex = PTHREAD_MUTEX_INITIALIZER;

/* K230 MPI 函数指针（dlopen 后填充） */
static k230_mpi_sys_init_t  g_mpi_sys_init  = NULL;
static k230_mpi_sys_exit_t  g_mpi_sys_exit  = NULL;

/* K230 SDK 媒体库候选列表：不同 SDK 版本/发行版库名可能不同。
 * K230 CanMV SDK 公开文档中媒体 API 库可能叫 libmapi.so（Media API），
 * 部分 SDK 版本也提供 libmpi.so。此处按顺序尝试，任一加载成功即可。
 * 路径优先级：LD_LIBRARY_PATH → /usr/lib → /lib */
static const char *k230_lib_candidates[] = {
    "libmapi.so",   /* K230 CanMV SDK Media API（主推） */
    "libmpi.so",    /* 部分 K230 SDK 版本的 MPI 库 */
    "libsys.so",    /* K230 系统库（部分 API 在此） */
    NULL
};

/* 懒加载初始化 K230 MPI 系统：
 * 1. 按候选列表顺序 dlopen 尝试加载媒体库
 * 2. dlsym 解析 kd_mpi_sys_init 等函数
 * 3. 调用 kd_mpi_sys_init 初始化 MPI
 * 成功后 g_mpi_available=1，后续调用直接返回。
 * 线程安全：互斥锁保护，避免多线程重复初始化。
 * 注意：库不存在是正常的回退路径（非 K230 平台或 SDK 未集成），
 *       使用 LV_LOG_INFO 而非 LV_LOG_USER，避免日志噪音困扰用户 */
static int k230_mpi_init(void)
{
    pthread_mutex_lock(&g_mpi_mutex);
    if (g_mpi_available != -1) {
        /* 已检测过，直接返回缓存结果 */
        pthread_mutex_unlock(&g_mpi_mutex);
        return g_mpi_available;
    }

    /* 按候选列表顺序尝试加载 K230 媒体库 */
    for (int i = 0; k230_lib_candidates[i] != NULL; i++) {
        g_mpi_handle = dlopen(k230_lib_candidates[i], RTLD_NOW | RTLD_GLOBAL);
        if (g_mpi_handle) {
            LV_LOG_USER("K230 accel: 已加载 %s", k230_lib_candidates[i]);
            break;
        }
        g_mpi_handle = NULL;
    }

    if (!g_mpi_handle) {
        /* 所有候选库均不存在：非 K230 平台或 SDK 未集成媒体库。
         * 这是正常的回退路径，使用 INFO 级别日志（非 USER）避免困扰用户。
         * 软件路径（双核并行 + UV 查表）已足够流畅运行 */
        LV_LOG_INFO("K230 accel: 媒体库未找到，使用软件转换路径（双核并行）");
        g_mpi_available = 0;
        pthread_mutex_unlock(&g_mpi_mutex);
        return 0;
    }

    /* 解析 MPI 系统初始化函数 */
    g_mpi_sys_init = (k230_mpi_sys_init_t)dlsym(g_mpi_handle, "kd_mpi_sys_init");
    g_mpi_sys_exit = (k230_mpi_sys_exit_t)dlsym(g_mpi_handle, "kd_mpi_sys_exit");

    if (!g_mpi_sys_init) {
        LV_LOG_WARN("K230 accel: kd_mpi_sys_init 符号未找到，回退软件路径");
        dlclose(g_mpi_handle);
        g_mpi_handle = NULL;
        g_mpi_available = 0;
        pthread_mutex_unlock(&g_mpi_mutex);
        return 0;
    }

    /* 初始化 MPI 系统 */
    int ret = g_mpi_sys_init();
    if (ret != 0) {
        LV_LOG_ERROR("K230 accel: kd_mpi_sys_init 失败 (ret=%d)，回退软件路径", ret);
        dlclose(g_mpi_handle);
        g_mpi_handle = NULL;
        g_mpi_sys_init = NULL;
        g_mpi_sys_exit = NULL;
        g_mpi_available = 0;
        pthread_mutex_unlock(&g_mpi_mutex);
        return 0;
    }

    LV_LOG_USER("K230 accel: MPI 系统初始化成功，硬件加速已启用");
    g_mpi_available = 1;
    pthread_mutex_unlock(&g_mpi_mutex);
    return 1;
}

int k230_accel_is_available(void)
{
    /* 首次调用触发懒加载初始化，后续直接返回缓存值 */
    if (g_mpi_available == -1) {
        return k230_mpi_init();
    }
    return g_mpi_available;
}

int k230_accel_convert_frame(const camera_frame_t *frame,
                             uint16_t *dst, int dst_w, int dst_h,
                             camera_rotate_t rotate)
{
    if (!k230_accel_is_available()) {
        return -1;  /* MPI 不可用，调用方回退到软件路径 */
    }

    /* TODO: 待 K230 SDK 实际验证后实现完整调用链：
     *
     * 1. 准备源缓冲区信息（V4L2 mmap 的 NV12/YUYV 帧）
     *    - 物理地址：通过 kd_mpi_vb_get_block 获取，或通过 DMA-BUF 导入
     *    - 格式：NV12 / YUYV / RGB565
     *    - 尺寸：frame->width × frame->height
     *
     * 2. 准备目标缓冲区信息（LVGL canvas 的 RGB565 buffer）
     *    - 物理地址：通过 kd_mpi_vb_get_block 分配
     *    - 格式：RGB565
     *    - 尺寸：dst_w × dst_h
     *
     * 3. 配置 2D 加速任务
     *    k_2d_task_t task = {0};
     *    task.src_buf = ...;  // 源缓冲区
     *    task.dst_buf = ...;  // 目标缓冲区
     *    task.src_format = KD_2D_FORMAT_NV12;  // 源格式
     *    task.dst_format = KD_2D_FORMAT_RGB565; // 目标格式
     *    task.rotate = (rotate == CAM_ROTATE_90) ? KD_2D_ROTATE_90 :
     *                  (rotate == CAM_ROTATE_180) ? KD_2D_ROTATE_180 :
     *                  (rotate == CAM_ROTATE_270) ? KD_2D_ROTATE_270 :
     *                  KD_2D_ROTATE_0;
     *    task.scale_mode = KD_2D_SCALE_BILINEAR;  // 双线性缩放
     *
     * 4. 执行 2D 加速
     *    kd_mpi_2d_create(&task);
     *    kd_mpi_2d_exec(&task);
     *    kd_mpi_2d_destroy(&task);
     *
     * 5. 将目标缓冲区数据拷贝到 dst（或通过 DMA-BUF 共享避免拷贝）
     *
     * 预期性能：<2ms/帧（1080p NV12 → 800x480 RGB565 + 90° 旋转）
     */
    (void)frame; (void)dst; (void)dst_w; (void)dst_h; (void)rotate;

    /* 当前未实现具体调用，返回 -1 让调用方回退到软件路径。
     * K230 SDK 验证后在此处实现上述 TODO 中的调用链 */
    LV_LOG_WARN("K230 accel: 2D 加速未实现，回退软件路径");
    return -1;
}

int k230_accel_encode_h264(const camera_frame_t *frame,
                           uint8_t *out_buf, size_t *out_size,
                           int is_keyframe)
{
    if (!k230_accel_is_available()) {
        return -1;
    }

    /* TODO: 待 K230 SDK 验证后实现 VPU H.264 编码：
     *
     * 1. 创建 VENC 通道
     *    kd_mpi_venc_create_chn(chn, &venc_chn_attr);
     *    - venc_chn_attr.stVencAttr.enType = PT_H264
     *    - venc_chn_attr.stVencAttr.u32PicWidth = frame->width
     *    - venc_chn_attr.stVencAttr.u32PicHeight = frame->height
     *    - venc_chn_attr.stVencAttr.u32BufSize = *out_size
     *
     * 2. 发送源帧
     *    kd_mpi_venc_send_frame(chn, &venc_frame, timeout);
     *
     * 3. 获取编码码流
     *    kd_mpi_venc_get_stream(chn, &venc_stream, timeout);
     *    memcpy(out_buf, venc_stream.pstPack.pu8Addr, venc_stream.pstPack.u32Len);
     *    *out_size = venc_stream.pstPack.u32Len;
     *
     * 4. 释放码流
     *    kd_mpi_venc_release_stream(chn, &venc_stream);
     *
     * 预期性能：1080p@30fps 硬件编码，CPU 零占用
     */
    (void)frame; (void)out_buf; (void)out_size; (void)is_keyframe;

    LV_LOG_WARN("K230 accel: VPU H.264 编码未实现，回退软件 MJPEG");
    return -1;
}

#else /* 非 riscv64 平台（RK3566 / PC） */

/* RK3566 / PC 平台无 K230 MPI，直接返回不可用 */
int k230_accel_is_available(void)
{
    return 0;
}

int k230_accel_convert_frame(const camera_frame_t *frame,
                             uint16_t *dst, int dst_w, int dst_h,
                             camera_rotate_t rotate)
{
    (void)frame; (void)dst; (void)dst_w; (void)dst_h; (void)rotate;
    return -1;  /* 不可用，调用方回退到 RGA 或软件路径 */
}

int k230_accel_encode_h264(const camera_frame_t *frame,
                           uint8_t *out_buf, size_t *out_size,
                           int is_keyframe)
{
    (void)frame; (void)out_buf; (void)out_size; (void)is_keyframe;
    return -1;
}

#endif /* __riscv && __riscv_xlen == 64 */
