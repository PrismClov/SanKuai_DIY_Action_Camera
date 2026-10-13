/**
 * @file device_config.h
 * @brief 设备配置加载模块
 *
 * 从部署目录下的 device.json 读取设备硬件与认证信息。
 * 12 项字段：序列号、厂商、型号、model_id、CMIITID、FCCID、IC CAN、ICES ID、
 *           设备序列号、固件版本、摄像头型号、相机固件版本。
 *
 * 项目信息源：app3/inf.json（编译时由 build 脚本读取并合并到 device.json）
 *
 * 文件位置优先级：
 *   1. APP_DIR 环境变量指定目录下的 device.json
 *   2. /userdata/app/device.json（泰山派实际部署路径）
 *   3. /app/app1/device.json（历史默认路径）
 *   4. ./device.json（当前工作目录）
 *
 * 字段缺失或文件不存在时返回安全默认值，不阻塞应用启动。
 */
#ifndef DEVICE_CONFIG_H
#define DEVICE_CONFIG_H

#include <stdbool.h>
#include <stddef.h>   /* size_t */

/* 摄像头能力声明：从 device.json 的 camera_capabilities 对象读取。
 * 决定预览/录制参数上限。更换摄像头时修改 device.json 即可，无需改代码。
 * 字段缺失时使用安全默认值（GC2093：1080p@30fps）。 */
typedef struct {
    int max_photo_width;     /* 最大拍照宽度（像素） */
    int max_photo_height;    /* 最大拍照高度（像素） */
    int max_video_width;     /* 最大录像宽度（像素） */
    int max_video_height;    /* 最大录像高度（像素） */
    int max_video_fps;       /* 最大录像帧率 */
} camera_caps_t;

/* 设备配置结构体：所有字段均为字符串指针，指向静态缓冲区，无需释放。
 * 缺失字段返回空字符串 ""（不返回 NULL，避免上层判空）。 */
typedef struct {
    const char *serial_number;            /* 硬件序列号（硬件ID，回退 device-tree/MAC） */
    const char *manufacturer;              /* 厂商名字 */
    const char *device_model;              /* 设备型号名字 */
    const char *model_id;                  /* 型号ID（如 dc001，用于 SN 生成） */
    const char *cmiit_id;                  /* CMIIT ID（中国无线电核准） */
    const char *fcc_id;                    /* FCC ID（美国 FCC 认证） */
    const char *ic_can;                    /* IC CAN（加拿大 IC 认证） */
    const char *ices_id;                   /* ICES ID（加拿大 ICES） */
    const char *device_serial;             /* 设备序列号（SN，可由规则生成） */
    const char *firmware_version;          /* 工程固件版本（非内核版本） */
    const char *camera_model;              /* 摄像头型号（如 "GC2093"） */
    const char *camera_firmware_version;   /* 相机固件版本（实际固件号） */
    camera_caps_t camera_caps;             /* 摄像头能力（数值字段，默认 1080p@30） */

    bool loaded;                           /* 是否成功加载 device.json */
} device_config_t;

/* 加载设备配置。优先级见文件头注释。
 * 失败时返回的结构体所有字段为空字符串、loaded=false。
 * 返回值指向内部静态缓冲区，无需释放；后续调用会覆盖前一次结果。 */
const device_config_t * device_config_load(void);

/* 获取最近一次加载的配置（若未加载则触发一次加载） */
const device_config_t * device_config_get(void);

/* 重新加载配置（用于配置文件热更新场景） */
const device_config_t * device_config_reload(void);

/* 获取 device.json 实际加载路径（用于日志/诊断） */
const char * device_config_path(void);

/* 按规则生成设备序列号 SN：s + 硬件ID + model_id + 今天日期最后一位
 *   - 硬件ID：优先 device.json 的 serial_number，回退 device-tree/MAC
 *   - model_id：从 device.json 读取
 *   - 今天日期最后一位：YYYYMMDD 的末位字符（如 2026-08-01 → '1'）
 * 结果写入 buf（buf_sz 含终止符），返回 buf 指针。 */
const char * device_config_generate_sn(char *buf, size_t buf_sz);

#endif /* DEVICE_CONFIG_H */
