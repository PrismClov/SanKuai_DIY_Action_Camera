/**
 * @file camera_hal.h
 * @brief 摄像头 HAL 抽象层
 *
 * 设计目标：
 *   - 一份二进制兼容多板（RK3566 / K230）
 *   - 后端可插拔：V4L2（通用）、K230 MPI（预留）
 *   - 自动检测可用后端，运行时选择最优实现
 *   - 零拷贝 MMAP 采集，性能优先
 *
 * 使用流程：
 *   camera_hal_t *cam = camera_open_default(1920, 1080, 30);  // 协商格式
 *   if (cam) {
 *       camera_start(cam);
 *       camera_frame_t frame;
 *       while (running) {
 *           if (camera_acquire_frame(cam, &frame) == 0) {
 *               // 使用 frame.data / frame.width / frame.height
 *               camera_release_frame(cam, &frame);
 *           }
 *       }
 *       camera_stop(cam);
 *       camera_close(cam);
 *   }
 *
 * 帧格式优先级（协商时按序尝试）：
 *   1. RGB565（直接送 LVGL canvas，无需转换）
 *   2. NV12 / NV21（4:2:0 半平面，需 YUV→RGB 转换）
 *   3. YUYV（4:2:2 打包，需 YUV→RGB 转换）
 */
#ifndef CAMERA_HAL_H
#define CAMERA_HAL_H

#include <stdint.h>
#include <stddef.h>

/* 常用 V4L2 像素格式四字符码（避免依赖 videodev2.h 头文件） */
#define CAM_FOURCC_RGB565  0x36314752u   /* 'R' 'G' '1' '6' = RGB565 */
#define CAM_FOURCC_NV12    0x3231564Eu   /* 'N' 'V' '1' '2' */
#define CAM_FOURCC_NV21    0x3132564Eu   /* 'N' 'V' '2' '1' */
#define CAM_FOURCC_YUYV    0x56595559u   /* 'Y' 'U' 'Y' 'V' */
#define CAM_FOURCC_UYVY    0x59565955u   /* 'U' 'Y' 'V' 'Y' */
#define CAM_FOURCC_MJPEG   0x47504A4Du   /* 'M' 'J' 'P' 'G' */

/* 摄像头设备句柄（不透明指针，由具体后端实现） */
typedef struct camera_hal camera_hal_t;

/* 帧数据描述 */
typedef struct {
    const uint8_t *data;        /* 帧数据指针（MMAP 缓冲区，零拷贝，调用方不可修改） */
    size_t         size;        /* 数据字节数 */
    uint32_t       fourcc;      /* 像素格式（CAM_FOURCC_*） */
    int            width;       /* 帧宽（像素） */
    int            height;      /* 帧高（像素） */
    uint64_t       timestamp_us;/* 时间戳（微秒，单调时钟） */
    void          *internal;    /* 后端私有数据（release_frame 用） */
} camera_frame_t;

/* 当前协商的格式 */
typedef struct {
    int      width;
    int      height;
    uint32_t fourcc;
    int      fps;
} camera_format_t;

/* 旋转角度（顺时针），用于修正摄像头模组安装方向 + 横竖屏适配 */
typedef enum {
    CAM_ROTATE_0   = 0,    /* 不旋转 */
    CAM_ROTATE_90  = 90,   /* 顺时针 90°（竖屏：横向帧旋转为纵向） */
    CAM_ROTATE_180 = 180,  /* 180° 翻转（横屏：传感器倒装修正） */
    CAM_ROTATE_270 = 270,  /* 顺时针 270° = 逆时针 90° */
} camera_rotate_t;

/* 白平衡预设（对应 V4L2_VPX_PRESET_* / 自定义映射） */
typedef enum {
    CAM_WB_AUTO         = 0,  /* 自动白平衡 */
    CAM_WB_DAYLIGHT     = 1,  /* 日光 */
    CAM_WB_CLOUDY       = 2,  /* 阴天 */
    CAM_WB_INCANDESCENT = 3,  /* 白炽灯 */
    CAM_WB_FLUORESCENT  = 4,  /* 荧光灯 */
} cam_wb_t;

/* 测光模式 */
typedef enum {
    CAM_METERING_AVERAGE    = 0,  /* 平均测光 */
    CAM_METERING_CENTER     = 1,  /* 中央重点 */
    CAM_METERING_SPOT       = 2,  /* 点测光 */
} cam_metering_t;

/* 摄像头控制参数：由 V4L2 VIDIOC_S_CTRL 设置到驱动 */
typedef struct {
    int           has_exposure;     /* 是否设置曝光值（0/1） */
    int           exposure_val;     /* 曝光值（V4L2_CID_EXPOSURE，单位由驱动定义） */
    int           has_ev;           /* 是否设置曝光补偿 */
    int           ev_val;           /* 曝光补偿（-2..+2，映射到 V4L2_CID_AUTO_EXPOSURE_BIAS） */
    int           has_wb;           /* 是否设置白平衡 */
    cam_wb_t      wb;               /* 白平衡预设 */
    int           has_iso;          /* 是否设置 ISO */
    int           iso_val;          /* ISO 值（V4L2_CID_ISO_SENSITIVITY） */
    int           has_metering;     /* 是否设置测光模式 */
    cam_metering_t metering;        /* 测光模式 */
    int           has_brightness;   /* 是否设置亮度 */
    int           brightness_val;   /* 亮度（0~100，映射到驱动范围） */
    int           has_saturation;   /* 是否设置饱和度 */
    int           saturation_val;   /* 饱和度（0~100） */
    int           has_contrast;     /* 是否设置对比度 */
    int           contrast_val;     /* 对比度（0~100） */
    int           has_sharpness;    /* 是否设置锐度 */
    int           sharpness_val;    /* 锐度（0~100） */
    int           has_zoom;         /* 是否设置变焦 */
    int           zoom_val;         /* 变焦值（1.0x=100, 2.0x=200, 4.0x=400，映射到 V4L2_CID_ZOOM_ABSOLUTE） */
} cam_control_t;

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 生命周期管理                                                        */
/* ------------------------------------------------------------------ */

/* 自动检测并打开默认摄像头。
 * 参数为期望格式（协商失败时后端可回退到接近的格式）。
 * 失败返回 NULL（如无可用设备、权限不足等）。 */
camera_hal_t * camera_open_default(int width, int height, int fps);

/* 关闭并释放摄像头资源（停止采集后调用） */
void camera_close(camera_hal_t *cam);

/* 获取当前协商的格式 */
int camera_get_format(camera_hal_t *cam, camera_format_t *fmt);

/* 开始/停止采集 */
int camera_start(camera_hal_t *cam);
int camera_stop(camera_hal_t *cam);

/* 获取一帧（阻塞或非阻塞取决于后端实现）。
 * 返回 0 成功，负数失败。
 * 获取到的帧必须配对调用 camera_release_frame 释放。 */
int camera_acquire_frame(camera_hal_t *cam, camera_frame_t *frame);

/* 释放已获取的帧（归还给采集队列） */
int camera_release_frame(camera_hal_t *cam, camera_frame_t *frame);

/* ------------------------------------------------------------------ */
/* 摄像头控制（V4L2 VIDIOC_S_CTRL）                                    */
/* ------------------------------------------------------------------ */

/* 应用摄像头控制参数（曝光/白平衡/ISO/测光等）。
 *   cam  - 摄像头句柄
 *   ctrl - 控制参数（has_*=1 的字段会被应用，其他保持不变）
 * 返回：0 成功（部分控制项失败不影响整体），负数严重错误。
 * 单项失败在内部记录日志，不中断其他项设置。 */
int camera_set_controls(camera_hal_t *cam, const cam_control_t *ctrl);

/* ------------------------------------------------------------------ */
/* 像素格式转换（YUV → RGB565，软件实现，供 LVGL canvas 使用）        */
/* ------------------------------------------------------------------ */

/* 将任意格式的摄像头帧转换并降采样到 RGB565 缓冲区（带旋转）。
 * 参数：
 *   frame     - 摄像头帧（任何 CAM_FOURCC_* 格式）
 *   dst       - 目标 RGB565 缓冲区（dst_w * dst_h * 2 字节）
 *   dst_w     - 目标宽度（像素）
 *   dst_h     - 目标高度（像素）
 *   rotate    - 旋转角度（CAM_ROTATE_0/90/180/270）
 * 说明：
 *   - 自动按 fourcc 选择转换路径
 *   - 旋转合并到转换流程中，无二次遍历，性能最优
 *   - 2x2 区域平均采样，减少降采样锯齿，提升清晰度
 *   - 90°/270° 旋转时 dst 宽高对应 src 高宽（竖屏适配）
 *   - 不支持的格式返回 -1 */
int camera_frame_to_rgb565(const camera_frame_t *frame,
                            uint16_t *dst, int dst_w, int dst_h,
                            camera_rotate_t rotate);

/* 按画幅裁剪源帧中心区域后转换到 RGB565（预览专用）。
 * 裁剪逻辑与 photo_saver.c 的 rgb565_center_crop 一致，保证预览与保存照片内容一致。
 * 参数：
 *   frame      - 摄像头帧
 *   dst        - 目标 RGB565 缓冲区（dst_w * dst_h * 2 字节）
 *   dst_w      - 目标宽度（通常 = 屏幕宽度）
 *   dst_h      - 目标高度（通常 = 屏幕高度）
 *   rotate     - 旋转角度
 *   crop_ratio - 画幅：0=4:3, 1=16:9, 2=1:1, -1=不裁剪
 * 说明：
 *   - 裁剪在旋转后坐标系中进行（先旋转再裁剪）
 *   - 硬件加速路径暂不支持裁剪，自动回退软件路径
 *   - NV12/NV21 支持双核并行裁剪转换 */
int camera_frame_to_rgb565_crop(const camera_frame_t *frame,
                                 uint16_t *dst, int dst_w, int dst_h,
                                 camera_rotate_t rotate, int crop_ratio);

/* 获取 fourcc 对应的人类可读名称（用于日志） */
const char * camera_fourcc_name(uint32_t fourcc);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_HAL_H */
