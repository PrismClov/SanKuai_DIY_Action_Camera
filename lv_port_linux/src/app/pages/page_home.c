/**
 * @file page_home.c
 * @brief 主屏幕（相机风格 UI）
 *
 * 布局：
 *   - 顶部状态栏：存储信息 | 信息状态图标组 | 电池
 *   - 左侧：相册入口
 *   - 右侧：拍摄参数设置入口
 *   - 底部：当前拍摄模式 + 当前参数 | 变焦按钮
 *
 * 弹层：
 *   - 右侧参数设置面板
 *   - 右侧参数二级选择器
 *   - 底部模式切换网格
 *   - 右侧变焦滑块面板
 */
#include "page_home.h"
#include "page_album.h"               /* 相册刷新：拍照后进入相册需重新扫描 */
#include "page_quick.h"               /* 上滑菜单：模式切换后需刷新快捷参数页 */
#include "../app_main.h"
#include "../common/board_detect.h"   /* 板子识别：K230 降帧优化 */
#include "../common/ui_helpers.h"
#include "../common/app_i18n.h"
#include "../camera/camera_hal.h"
#include "../camera/camera_preview.h"  /* 预览后台线程：YUV→RGB565 转换移出 UI 线程 */
#include "../common/device_config.h"
#include "../camera/recorder_hal.h"   /* 录制抽象层（预留 4K60 接口） */
#include "../camera/photo_saver.h"    /* 拍照保存：RGB565→JPEG */
#include "../settings/settings_storage.h"  /* 存储路径选择 */
#ifndef APP_WIFI_DISABLED
#include "../../wifi/wpa_ctrl_wrap.h"
#endif

#include "lvgl/lvgl.h"
#include <stdio.h>
#include <stdlib.h>   /* malloc / free（摄像头 RGB565 缓冲区分配） */
#include <string.h>
#include <limits.h>
#include <time.h>     /* 时间戳文件名（拍照/录像） */

/* ------------------------------------------------------------------ */
/* 现有公共变量（供 page_quick.c 等外部使用）                          */
/* ------------------------------------------------------------------ */
static const char *g_res_opts[] = {"2.7K/4:3", "4K/16:9", "4K/4:3"};
static const char *g_fps_opts[] = {"24", "25", "30", "48", "50", "60"};
static int g_sel_res = 1;
static int g_sel_fps = 2;

/* ------------------------------------------------------------------ */
/* 主页状态                                                            */
/* ------------------------------------------------------------------ */
/* 拍摄功能模式：0=拍照,1=视频,2=超级夜景,3=人物跟随,4=慢动作,5=静止延时 */
static int g_func_mode = 0;  /* 默认拍照模式（相机应用通常默认拍照） */

/* 参数模式：0=自动,1=手动 */
static int g_param_mode = 0;

/* 顶部状态标志位（预留可配置） */
#define STATUS_GPS  (1 << 0)
#define STATUS_WIFI (1 << 1)
#define STATUS_BT   (1 << 2)
static int g_status_flags = STATUS_GPS;

/* WiFi 连接状态定时检查（每 2 秒检查 wpa_status，连接时显示 WiFi 图标） */
static lv_timer_t *s_wifi_status_timer = NULL;

/* 照片参数 */
static const char *g_exp_opts[] = {"-2.0", "-1.0", "0", "+1.0", "+2.0"};
static const char *g_wb_opts[]  = {"自动", "日光", "阴天", "白炽灯", "荧光灯"};
static const char *g_wb_opts_en[]  = {"Auto", "Daylight", "Cloudy", "Incandescent", "Fluorescent"};
static const char *g_fmt_opts[] = {"JPG", "RAW", "JPG+RAW"};
static int g_sel_exp = 2;
static int g_sel_wb  = 0;
static int g_sel_fmt = 0;
/* 当前语言的白平衡选项（语言切换后立即生效） */
#define WB_OPTS (g_app_language ? g_wb_opts_en : g_wb_opts)

/* 视频参数 */
static const char *g_color_opts[]       = {"标准", "鲜艳", "自然", "黑白"};
static const char *g_color_opts_en[]    = {"Standard", "Vivid", "Natural", "B&W"};
static const char *g_tone_opts[]        = {"标准", "高对比", "低对比", "电影"};
static const char *g_tone_opts_en[]     = {"Standard", "High Contrast", "Low Contrast", "Cinematic"};
static const char *g_adj_opts[]         = {"关", "锐度+", "降噪+", "锐度+/降噪+"};
static const char *g_adj_opts_en[]       = {"Off", "Sharpness+", "Denoise+", "Sharp+/Denoise+"};
static const char *g_vstab_scene_opts[] = {"普通", "运动", "车载", "手持"};
static const char *g_vstab_scene_opts_en[] = {"Normal", "Sport", "Vehicle", "Handheld"};
#define COLOR_OPTS      (g_app_language ? g_color_opts_en    : g_color_opts)
#define TONE_OPTS       (g_app_language ? g_tone_opts_en     : g_tone_opts)
#define ADJ_OPTS        (g_app_language ? g_adj_opts_en      : g_adj_opts)
#define VSTAB_SCENE_OPTS (g_app_language ? g_vstab_scene_opts_en : g_vstab_scene_opts)
static int g_sel_color       = 0;
static int g_sel_tone        = 0;
static int g_sel_adj         = 0;
static int g_sel_vstab_scene = 0;

/* 拍照模式扩展参数 */
static const char *g_iso_opts[]     = {"自动", "100", "200", "400", "800", "1600"};
static const char *g_iso_opts_en[]  = {"Auto", "100", "200", "400", "800", "1600"};
static const char *g_metering_opts[]    = {"平均测光", "中央重点", "点测光"};
static const char *g_metering_opts_en[] = {"Average", "Center", "Spot"};
static const char *g_timer_opts[]    = {"关闭", "2秒", "5秒", "10秒"};
static const char *g_timer_opts_en[] = {"Off", "2s", "5s", "10s"};
#define ISO_OPTS      (g_app_language ? g_iso_opts_en     : g_iso_opts)
#define METERING_OPTS (g_app_language ? g_metering_opts_en : g_metering_opts)
#define TIMER_OPTS    (g_app_language ? g_timer_opts_en    : g_timer_opts)
static int g_sel_iso       = 0;
static int g_sel_metering  = 0;
static int g_sel_timer     = 0;

/* 视频模式扩展参数 */
static const char *g_switch_opts[]    = {"关闭", "开启"};
static const char *g_switch_opts_en[] = {"Off", "On"};
#define SWITCH_OPTS (g_app_language ? g_switch_opts_en : g_switch_opts)
static int g_sel_mic        = 1;  /* 麦克风默认开 */
static int g_sel_grid       = 0;  /* 网格线默认关 */
static int g_sel_distortion = 0;  /* 畸变矫正默认关 */

/* 底部显示 */
/* 画幅选项：显示为 "尺寸等级 (比例)" 格式，尺寸等级 S/M/L 对应不同像素总量。
 * 拍照模式下分辨率以画幅形式呈现，M=Medium 为默认推荐档 */
static const char *g_ratio_opts[] = {"S (4:3)", "M (16:9)", "L (1:1)"};
static const char *g_stab_opts[]  = {"普通防抖", "超稳+", "地平线"};
static const char *g_stab_opts_en[]  = {"Standard Stab", "Super Stab+", "Horizon"};
#define STAB_OPTS (g_app_language ? g_stab_opts_en : g_stab_opts)
static int g_sel_ratio = 1;
static int g_sel_stab  = 1;

/* 拍照模式扩展：图片拍摄大小（像素总量）和连拍参数。
 * 图片大小对应不同输出分辨率，连拍参数控制单次快门连拍张数/速度。
 * 连拍第 0 项为"关闭"（单张拍摄），其余为连拍速度 */
static const char *g_photosize_opts[]    = {"12MP", "20MP", "24MP", "48MP"};
static const char *g_photosize_opts_en[] = {"12MP", "20MP", "24MP", "48MP"};
#define PHOTOSIZE_OPTS (g_app_language ? g_photosize_opts_en : g_photosize_opts)
static const char *g_burst_opts[]    = {"关闭", "3 张/秒", "5 张/秒", "10 张/秒"};
static const char *g_burst_opts_en[] = {"Off", "3 fps", "5 fps", "10 fps"};
#define BURST_OPTS (g_app_language ? g_burst_opts_en : g_burst_opts)
static int g_sel_photosize = 0;  /* 默认 12MP */
static int g_sel_burst     = 0;  /* 默认关闭连拍 */

/* 图片大小档位 → 实际像素尺寸映射（按 16:9 画幅计算宽高）。
 * 12MP → 4608x2592 (≈12MP)
 * 20MP → 5952x3354 (≈20MP)
 * 24MP → 6512x3666 (≈24MP)
 * 48MP → 9216x5184 (≈48MP)
 * 注意：实际拍摄时按所选画幅比例调整宽高，保持像素总量接近目标值。
 * 嵌入式平台 48MP 需较大内存（≈90MB RGB565），低内存板子可能 OOM */
static const struct {
    int mp;       /* 目标像素总量（百万像素） */
    int target_w; /* 16:9 基准宽度（实际按画幅比例换算） */
    int target_h; /* 16:9 基准高度 */
} g_photosize_dims[] = {
    { 12, 4608, 2592 },
    { 20, 5952, 3354 },
    { 24, 6512, 3666 },
    { 48, 9216, 5184 },
};

/* 根据画幅比例换算图片大小的实际宽高。
 * 16:9 基准 → 4:3 时高度增大，1:1 时取较小边。
 * out_w/out_h 输出实际目标尺寸 */
static void photosize_get_dims(int photosize_idx, int ratio_idx,
                                int *out_w, int *out_h)
{
    if (photosize_idx < 0 || photosize_idx >= (int)(sizeof(g_photosize_dims)/sizeof(g_photosize_dims[0]))) {
        *out_w = 0;
        *out_h = 0;
        return;
    }
    int base_w = g_photosize_dims[photosize_idx].target_w;
    int base_h = g_photosize_dims[photosize_idx].target_h;

    /* 按画幅比例换算（保持像素总量接近） */
    switch (ratio_idx) {
        case 0:  /* 4:3 */
            /* 从 16:9 转 4:3：高度放大，宽度略减，保持像素总量 */
            *out_w = (int)((double)base_w * 0.866);  /* sqrt(3/4) */
            *out_h = (int)((double)*out_w * 3.0 / 4.0);
            break;
        case 2:  /* 1:1 */
            /* 正方形：取较小边 */
            *out_w = base_h;
            *out_h = base_h;
            break;
        case 1:  /* 16:9 */
        default:
            *out_w = base_w;
            *out_h = base_h;
            break;
    }
}

/* 变焦 */
static float g_zoom = 1.0f;

/* 录像状态 */
static int        g_recording    = 0;
static uint32_t   g_rec_start_ms = 0;
static lv_timer_t *g_rec_timer   = NULL;
static lv_anim_t  g_rec_pulse;    /* 录像按钮脉冲缩放动画 */
static recorder_handle_t *g_recorder = NULL;  /* 录制器句柄（NULL=未创建） */

/* 拍照状态：g_take_photo_pending=1 时，下一帧 camera_timer_cb 中保存当前画面 */
static int g_take_photo_pending = 0;

/* 拍照保存目录缓存：home_take_photo 设置，camera_timer_cb 的 photo_ready 路径使用。
 * 避免在 worker 线程转换完成后再查询存储路径（settings_storage_get_path 非线程安全） */
static char g_photo_save_dir[256] = {0};

/* 拍照倒计时状态 */
static int       g_countdown_remaining = 0;  /* 剩余秒数（0=无倒计时） */
static lv_timer_t *g_countdown_timer   = NULL;
static lv_obj_t  *g_countdown_label    = NULL;  /* 倒计时数字 label */
static lv_obj_t  *g_countdown_overlay  = NULL;  /* 倒计时全屏 overlay */

/* 连拍状态：g_burst_remaining>0 时表示连拍进行中。
 * 连拍逻辑：快门按下后按选定速度连续拍摄 N 张。
 *   g_sel_burst=0 关闭：单张拍摄
 *   g_sel_burst=1 (3fps)：连拍 3 张，间隔 333ms
 *   g_sel_burst=2 (5fps)：连拍 5 张，间隔 200ms
 *   g_sel_burst=3 (10fps)：连拍 10 张，间隔 100ms
 * 倒计时期间不启动连拍；录制中不响应连拍 */
static int         g_burst_remaining = 0;   /* 连拍剩余张数（含当前） */
static lv_timer_t *g_burst_timer     = NULL;
static int         g_burst_active    = 0;   /* 连拍序列是否进行中 */
static int         g_burst_shot_num  = 0;   /* 当前连拍序号（1,2,3...） */

/* 延时拍摄状态：定时拍照序列，间隔 5 秒拍一张，直到再次点击停止 */
static int       g_timelapse_active    = 0;  /* 延时拍摄是否进行中 */
static lv_timer_t *g_timelapse_timer   = NULL;  /* 延时拍摄定时器 */
#define TIMELAPSE_INTERVAL_MS  5000  /* 延时拍摄间隔：5 秒/张 */

/* 快门闪烁 overlay（拍照成功视觉反馈） */
static lv_obj_t  *g_flash_overlay     = NULL;

/* 网格线 overlay（三分线，辅助构图） */
static lv_obj_t  *g_grid_overlay      = NULL;

/* 画幅裁剪引导 overlay（WYSIWYG）：在预览上用黑色遮罩条标出"将不被保存"
 * 的区域，使预览中可见的亮区与最终保存的照片画幅完全一致。
 * - 拍照模式（4:3/16:9/1:1）时显示，视频/慢动作模式隐藏
 * - 遮罩条位置/尺寸与 photo_saver.c 的 rgb565_center_crop 逻辑严格一致 */
static lv_obj_t  *g_crop_guide_overlay = NULL;  /* 容器（占满 view） */
static lv_obj_t  *g_crop_bar_a         = NULL;  /* 上/左 遮罩条 */
static lv_obj_t  *g_crop_bar_b         = NULL;  /* 下/右 遮罩条 */

/* 摄像头预览状态 */
static camera_hal_t *g_camera        = NULL;   /* 摄像头 HAL 句柄，NULL 表示未打开 */
static camera_preview_t *g_preview  = NULL;   /* 预览后台线程模块（YUV→RGB565 在 worker 线程） */
static lv_obj_t     *g_camera_canvas = NULL;   /* LVGL 画布，承载摄像头帧 */
static lv_obj_t     *g_camera_view   = NULL;   /* 预览区父容器（重启摄像头时用） */
/* 摄像头延迟启动定时器：page_home_create 中创建，100ms 后触发 home_camera_start。
 * 这样 UI 框架可以先渲染首帧（状态栏/按钮等），避免 V4L2 打开+MMAP+STREAMON
 * 阻塞 UI 创建导致启动白屏/黑屏时间过长。page_home_destroy 中需先删除此定时器。 */
static lv_timer_t   *g_camera_start_timer = NULL;
static lv_timer_t   *g_camera_timer  = NULL;   /* 帧拉取定时器 */
static uint16_t     *g_camera_buf    = NULL;   /* RGB565 显示缓冲区（指向 preview 管理的 buffer） */
static int           g_camera_buf_w  = 0;      /* 缓冲区宽度（屏幕显示尺寸） */
static int           g_camera_buf_h  = 0;      /* 缓冲区高度（屏幕显示尺寸） */
static int           g_cam_src_w     = 0;      /* 摄像头实际协商的原始帧宽度 */
static int           g_cam_src_h     = 0;      /* 摄像头实际协商的原始帧高度 */
static int           g_cam_src_fps   = 0;      /* 摄像头实际协商的帧率 */

/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* UI 句柄                                                            */
/* ------------------------------------------------------------------ */
static lv_obj_t *g_storage_label;
static lv_obj_t *g_status_box;
static lv_obj_t *g_rec_btn;
static lv_obj_t *g_rec_label;
static lv_obj_t *g_mode_icon_btn;
static lv_obj_t *g_mode_param_label;
static lv_obj_t *g_zoom_btn;
static lv_obj_t *g_zoom_btn_label;

/* 弹层 */
static lv_obj_t *g_param_overlay   = NULL;
static lv_obj_t *g_selector_overlay= NULL;
static lv_obj_t *g_mode_overlay    = NULL;
static lv_obj_t *g_zoom_overlay    = NULL;
static lv_obj_t *g_zoom_arc        = NULL;   /* 变焦弧形滑块句柄，打开时同步当前倍率 */
static lv_obj_t *g_mode_list       = NULL;   /* 拍摄模式滚动选择列表 */
static int       g_mode_sel_idx    = 0;      /* 列表当前选中项（滚动居中项） */
#define MODE_ICON_MAX 8
static lv_obj_t *g_mode_icons[MODE_ICON_MAX]; /* 各模式项的图标 label */
static lv_obj_t *g_mode_texts[MODE_ICON_MAX]; /* 各模式项的文字 label */
static lv_obj_t *g_mode_items[MODE_ICON_MAX]; /* 各模式项容器（动画遍历用，跳过占位） */
static lv_obj_t *g_mode_spacer_start = NULL;  /* 列表首部占位：使首项也能滚动居中 */
static lv_obj_t *g_mode_spacer_end   = NULL;  /* 列表尾部占位：使末项也能滚动居中 */

/* 二级选择器当前上下文 */
static const char **g_sel_opts     = NULL;
static int         g_sel_count     = 0;
static int        *g_sel_idx       = NULL;
static void       (*g_sel_changed)(void) = NULL;
static lv_obj_t   *g_selector_list = NULL;

/* ------------------------------------------------------------------ */
/* 模式定义（图标 + 中英文标签）                                        */
/* ------------------------------------------------------------------ */
typedef struct {
    const char *icon;
    const char *label_zh;
    const char *label_en;
} mode_def_t;

static const mode_def_t g_modes[] = {
    { LV_SYMBOL_IMAGE,    "拍照", "Photo"   },
    { LV_SYMBOL_VIDEO,    "视频", "Video"   },
    { LV_SYMBOL_EYE_OPEN, "夜景", "Night"   },
    { LV_SYMBOL_LOOP,     "跟随", "Track"   },
    { LV_SYMBOL_PAUSE,    "慢动作", "Slow-Mo" },
    { LV_SYMBOL_REFRESH,  "延时", "Lapse"   },
};

#define MODE_COUNT (sizeof(g_modes)/sizeof(g_modes[0]))

/* ------------------------------------------------------------------ */
/* 各拍摄模式独立参数                                                  */
/* ------------------------------------------------------------------ */
typedef struct {
    int ratio;          /* 画幅 */
    int res;            /* 分辨率 */
    int fps;            /* 帧率 */
    int stab;           /* 增稳模式 */
    int exp;            /* 曝光补偿 */
    int wb;             /* 白平衡 */
    int fmt;            /* 存储格式 */
    int color;          /* 色彩 */
    int tone;           /* 影调 */
    int adj;            /* 图像调节 */
    int vstab_scene;    /* 增稳场景 */
    int param_mode;     /* 0=自动, 1=手动 */
    /* 拍照模式扩展参数 */
    int iso;            /* ISO 感光度（0=自动, 100/200/400/800/1600） */
    int metering;       /* 测光模式（0=平均, 1=中央, 2=点测） */
    int timer;          /* 拍照倒计时（0=关, 1=2s, 2=5s, 3=10s） */
    int photosize;      /* 图片拍摄大小（0=12MP, 1=20MP, 2=24MP, 3=48MP） */
    int burst;          /* 连拍参数（0=关, 1=3fps, 2=5fps, 3=10fps） */
    /* 视频模式扩展参数 */
    int mic;            /* 麦克风（0=关, 1=开） */
    int grid;           /* 网格线（0=关, 1=开） */
    int distortion;     /* 畸变矫正（0=关, 1=开） */
} mode_params_t;

static mode_params_t g_mode_params[MODE_COUNT];

/* 初始化各模式默认参数 */
static void mode_params_init(void)
{
    for (size_t i = 0; i < MODE_COUNT; i++) {
        g_mode_params[i].ratio = 1;       /* 默认 16:9 */
        g_mode_params[i].res   = 1;       /* 默认 4K/16:9 */
        g_mode_params[i].fps   = 2;       /* 默认 30fps */
        g_mode_params[i].stab  = 1;       /* 默认 超稳+ */
        g_mode_params[i].exp   = 2;       /* 默认 0 */
        g_mode_params[i].wb    = 0;       /* 默认 自动 */
        g_mode_params[i].fmt   = 0;       /* 默认 JPG */
        g_mode_params[i].color = 0;       /* 默认 标准 */
        g_mode_params[i].tone  = 0;       /* 默认 标准 */
        g_mode_params[i].adj   = 0;       /* 默认 关 */
        g_mode_params[i].vstab_scene = 0; /* 默认 普通 */
        g_mode_params[i].param_mode  = 0; /* 默认 自动 */
        /* 拍照扩展默认 */
        g_mode_params[i].iso       = 0;   /* 自动 ISO */
        g_mode_params[i].metering  = 0;   /* 平均测光 */
        g_mode_params[i].timer     = 0;   /* 无倒计时 */
        g_mode_params[i].photosize = 0;   /* 默认 12MP */
        g_mode_params[i].burst     = 0;   /* 默认关闭连拍 */
        /* 视频扩展默认 */
        g_mode_params[i].mic       = 1;   /* 麦克风开 */
        g_mode_params[i].grid      = 0;   /* 网格关 */
        g_mode_params[i].distortion = 0;  /* 畸变矫正关 */
    }
    /* 拍照模式默认 4:3 画幅 */
    g_mode_params[0].ratio = 0;
    /* 慢动作默认 48fps */
    g_mode_params[4].fps = 3;
}

/* 保存当前全局参数到指定模式 */
static void mode_params_save(int mode)
{
    if (mode < 0 || mode >= (int)MODE_COUNT) return;
    mode_params_t *p = &g_mode_params[mode];
    p->ratio       = g_sel_ratio;
    p->res         = g_sel_res;
    p->fps         = g_sel_fps;
    p->stab        = g_sel_stab;
    p->exp         = g_sel_exp;
    p->wb          = g_sel_wb;
    p->fmt         = g_sel_fmt;
    p->color       = g_sel_color;
    p->tone        = g_sel_tone;
    p->adj         = g_sel_adj;
    p->vstab_scene = g_sel_vstab_scene;
    p->param_mode  = g_param_mode;
    /* 拍照扩展 */
    p->iso         = g_sel_iso;
    p->metering    = g_sel_metering;
    p->timer       = g_sel_timer;
    p->photosize   = g_sel_photosize;
    p->burst       = g_sel_burst;
    /* 视频扩展 */
    p->mic         = g_sel_mic;
    p->grid        = g_sel_grid;
    p->distortion  = g_sel_distortion;
}

/* 从指定模式加载参数到全局变量 */
static void mode_params_load(int mode)
{
    if (mode < 0 || mode >= (int)MODE_COUNT) return;
    const mode_params_t *p = &g_mode_params[mode];
    g_sel_ratio       = p->ratio;
    g_sel_res         = p->res;
    g_sel_fps         = p->fps;
    g_sel_stab        = p->stab;
    g_sel_exp         = p->exp;
    g_sel_wb          = p->wb;
    g_sel_fmt         = p->fmt;
    g_sel_color       = p->color;
    g_sel_tone        = p->tone;
    g_sel_adj         = p->adj;
    g_sel_vstab_scene = p->vstab_scene;
    g_param_mode      = p->param_mode;
    /* 拍照扩展 */
    g_sel_iso         = p->iso;
    g_sel_metering    = p->metering;
    g_sel_timer       = p->timer;
    g_sel_photosize   = p->photosize;
    g_sel_burst       = p->burst;
    /* 视频扩展 */
    g_sel_mic         = p->mic;
    g_sel_grid        = p->grid;
    g_sel_distortion  = p->distortion;
}

/* ------------------------------------------------------------------ */
/* 参数持久化：保存到 capture_params.cfg，下次启动自动恢复              */
/* ------------------------------------------------------------------ */
#define PARAMS_CFG_FILE "capture_params.cfg"

static void mode_params_save_to_file(void)
{
    char path[256];
    const char *dir = app_get_app_dir();
    if (!dir || !dir[0]) dir = ".";  /* 防御性检查：app_get_app_dir 理论上不返回 NULL */
    snprintf(path, sizeof(path), "%s/%s", dir, PARAMS_CFG_FILE);

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        LV_LOG_WARN("无法保存拍摄参数到 %s", path);
        return;
    }

    /* 简单二进制格式：魔数 + 版本 + 各模式参数。
     * 魔数 "CPA2" 标识 v2 版本（新增 photosize/burst 字段）。
     * 旧版 "CPA1" 文件因结构体大小不兼容会被忽略，使用默认值重新初始化 */
    static const char magic[4] = {'C', 'P', 'A', '2'};  /* Capture Params v2 */
    fwrite(magic, 1, 4, fp);
    uint32_t version = 2;
    fwrite(&version, sizeof(version), 1, fp);
    fwrite(g_mode_params, sizeof(g_mode_params), 1, fp);
    fclose(fp);
    LV_LOG_USER("拍摄参数已保存: %s", path);
}

static void mode_params_load_from_file(void)
{
    char path[256];
    const char *dir = app_get_app_dir();
    if (!dir || !dir[0]) dir = ".";
    snprintf(path, sizeof(path), "%s/%s", dir, PARAMS_CFG_FILE);

    FILE *fp = fopen(path, "rb");
    if (!fp) return;

    char magic[4];
    if (fread(magic, 1, 4, fp) != 4 || memcmp(magic, "CPA2", 4) != 0) {
        /* 旧版 "CPA1" 文件或格式错误：结构体大小已变更，不兼容，使用默认值 */
        fclose(fp);
        LV_LOG_WARN("拍摄参数文件版本过旧或格式错误，使用默认值");
        mode_params_init();
        return;
    }

    uint32_t version = 0;
    if (fread(&version, sizeof(version), 1, fp) != 1 || version != 2) {
        fclose(fp);
        LV_LOG_WARN("拍摄参数版本不兼容（v%u），使用默认值", version);
        mode_params_init();
        return;
    }

    /* 读取各模式参数。结构体大小变化时需版本升级 */
    if (fread(g_mode_params, sizeof(g_mode_params), 1, fp) == 1) {
        LV_LOG_USER("拍摄参数已加载: %s", path);
    } else {
        LV_LOG_WARN("拍摄参数读取失败，使用默认值");
        mode_params_init();
    }
    fclose(fp);
}

/* 前向声明（函数实际定义在文件后部） */
static void home_refresh_bottom_params(void);
static void home_refresh_status_icons(void);
static void home_refresh_grid(void);
static void home_create_crop_guide(void);  /* 画幅裁剪引导 overlay 创建 */
static void home_update_crop_guide(void);  /* 画幅裁剪引导：与 photo_saver 裁剪一致 */
static void home_param_overlay_refresh(void);
static void mode_list_animate_to_center(void);
static void rec_btn_set_scale(void *obj, int32_t v);
/* 摄像头预览：前向声明，供 camera_timer_cb 在失败计数超限时调用 */
static void home_camera_stop(void);
/* 摄像头重启：前向声明，供模式切换/分辨率帧率变更时调用 */
static void home_camera_restart(void);
/* 连拍控制：前向声明，供倒计时完成/快门点击调用 */
static void home_start_burst(void);
static void home_stop_burst(void);

/* 将当前拍摄参数应用到摄像头 HAL（V4L2 控制）。
 * 在参数变更后调用，使曝光/白平衡/ISO/测光等设置实时生效。
 * - 手动模式（g_param_mode==1）：应用用户选择的自定义参数
 * - 自动模式（g_param_mode==0）：将曝光/白平衡/ISO 重置为自动，
 *   避免之前手动设置的参数残留影响画面
 * 底部参数（色彩/影调/图像调节）无论自动/手动模式都应用，
 * 因为它们是图像风格设置，不属于曝光控制 */
static void home_apply_camera_controls(void)
{
    if (!g_camera) return;

    cam_control_t ctrl;
    memset(&ctrl, 0, sizeof(ctrl));

    if (g_param_mode == 1) {
        /* 手动模式：应用用户选择的参数 */

        /* 曝光补偿：g_sel_exp 索引 0~4 对应 -2.0~+2.0 */
        ctrl.has_ev = 1;
        ctrl.ev_val = g_sel_exp - 2;  /* -2..+2 */

        /* 白平衡：g_sel_wb 索引直接映射到 cam_wb_t */
        ctrl.has_wb = 1;
        ctrl.wb = (cam_wb_t)g_sel_wb;

        /* ISO：g_sel_iso=0 为自动（iso_val=0），其他为具体值。
         * 无论自动还是手动都需要设置 has_iso=1，确保切换时能重置驱动状态 */
        ctrl.has_iso = 1;
        {
            /* ISO 选项: 0=Auto, 1=100, 2=200, 3=400, 4=800, 5=1600 */
            static const int iso_values[] = {0, 100, 200, 400, 800, 1600};
            if (g_sel_iso >= 0 && g_sel_iso < (int)(sizeof(iso_values)/sizeof(iso_values[0]))) {
                ctrl.iso_val = iso_values[g_sel_iso];
            }
        }

        /* 测光模式：g_sel_metering 直接映射 */
        ctrl.has_metering = 1;
        ctrl.metering = (cam_metering_t)g_sel_metering;
    } else {
        /* 自动模式：重置为驱动自动控制，避免手动参数残留 */
        ctrl.has_wb   = 1;
        ctrl.wb       = CAM_WB_AUTO;
        ctrl.has_iso  = 1;
        ctrl.iso_val  = 0;   /* 0 = ISO 自动 */
        ctrl.has_metering = 1;
        ctrl.metering = CAM_METERING_AVERAGE;
        /* 曝光补偿清零（EV=0） */
        ctrl.has_ev   = 1;
        ctrl.ev_val   = 0;
    }

    /* 底部图像风格参数（无论自动/手动模式都应用） */

    /* 色彩选择：0=标准, 1=鲜艳, 2=自然, 3=黑白
     * 映射到饱和度/对比度组合：
     *   标准:  饱和度 50, 对比度 50
     *   鲜艳:  饱和度 80, 对比度 60
     *   自然:  饱和度 40, 对比度 45
     *   黑白:  饱和度 0,  对比度 50 */
    ctrl.has_saturation = 1;
    ctrl.has_contrast   = 1;
    switch (g_sel_color) {
        case 1: ctrl.saturation_val = 80; ctrl.contrast_val = 60; break;  /* 鲜艳 */
        case 2: ctrl.saturation_val = 40; ctrl.contrast_val = 45; break;  /* 自然 */
        case 3: ctrl.saturation_val = 0;  ctrl.contrast_val = 50; break;  /* 黑白 */
        default: ctrl.saturation_val = 50; ctrl.contrast_val = 50; break; /* 标准 */
    }

    /* 影调：0=标准, 1=高对比, 2=低对比, 3=电影
     * 映射到对比度：
     *   标准:   50（已被色彩设置，此处覆盖）
     *   高对比: 75
     *   低对比: 35
     *   电影:   55 + 略低饱和 */
    if (g_sel_tone > 0) {
        switch (g_sel_tone) {
            case 1: ctrl.contrast_val = 75; break;  /* 高对比 */
            case 2: ctrl.contrast_val = 35; break;  /* 低对比 */
            case 3: ctrl.contrast_val = 55; ctrl.saturation_val = 40; break; /* 电影 */
            default: break;
        }
    }

    /* 图像调节：0=关, 1=锐度+, 2=降噪+, 3=锐度+/降噪+
     * 锐度映射：关=50, 锐度+=75
     * 降噪通过降低锐度实现（驱动通常无独立降噪控制）：降噪+=35 */
    ctrl.has_sharpness = 1;
    switch (g_sel_adj) {
        case 1: ctrl.sharpness_val = 75; break;  /* 锐度+ */
        case 2: ctrl.sharpness_val = 35; break;  /* 降噪+（降低锐度减少噪声放大） */
        case 3: ctrl.sharpness_val = 65; break;  /* 锐度+/降噪+（折中） */
        default: ctrl.sharpness_val = 50; break; /* 关 */
    }

    /* 应用到摄像头（单项失败不影响整体，camera_set_controls 内部处理） */
    camera_set_controls(g_camera, &ctrl);
}

/* 参数选择器通用变更回调：保存到当前模式 + 刷新底部显示 + 应用到摄像头 */
static void on_param_changed(void)
{
    mode_params_save(g_func_mode);
    home_refresh_bottom_params();
    /* 参数变更后立即应用到摄像头 HAL，使设置实时生效 */
    home_apply_camera_controls();
    /* 网格线开关变更后需刷新 overlay 显示/隐藏 */
    home_refresh_grid();
    /* 画幅变更后需刷新裁剪引导遮罩位置（WYSIWYG） */
    home_update_crop_guide();
    /* 画幅变更后同步到预览 worker，使预览按画幅裁剪源帧（与保存照片一致） */
    if (g_preview) {
        camera_preview_set_crop_ratio(g_preview, g_sel_ratio);
    }
    /* 持久化到文件（下次启动自动恢复） */
    mode_params_save_to_file();
}

/* 当前语言的模式标签 */
static const char * mode_label(int idx)
{
    if (idx < 0 || idx >= (int)MODE_COUNT) return "";
    return g_app_language ? g_modes[idx].label_en : g_modes[idx].label_zh;
}

/* ------------------------------------------------------------------ */
/* 工具函数                                                            */
/* ------------------------------------------------------------------ */
/* 检测存储类型并计算录制时长 */
static void update_storage_info(lv_obj_t *label)
{
    long long total_int = 0, used_int = 0, total_sd = 0, used_sd = 0;
    int ok_int = 0, ok_sd = 0;

    ui_read_storage("/", &total_int, &used_int, &ok_int);
    ui_read_storage("/mnt/sdcard", &total_sd, &used_sd, &ok_sd);

    long long free_mb = 0;
    const char *icon = LV_SYMBOL_SD_CARD;

    if (ok_sd && total_sd > 0) {
        free_mb = total_sd - used_sd;
        icon = LV_SYMBOL_SD_CARD;
    } else if (ok_int && total_int > 0) {
        free_mb = total_int - used_int;
        icon = LV_SYMBOL_DRIVE;
    }

    if (free_mb > 0) {
        long long minutes = free_mb * 10 / 1024;
        long long hours = minutes / 60;
        minutes = minutes % 60;
        lv_label_set_text_fmt(label, "%s %lldh%lldm", icon, hours, minutes);
    } else {
        lv_label_set_text_fmt(label, "%s --:--", icon);
    }
}

/* 格式化录像时长 mm:ss */
static void fmt_rec_time(uint32_t elapsed_ms, char *buf, size_t sz)
{
    uint32_t total_sec = elapsed_ms / 1000;
    uint32_t min = total_sec / 60;
    uint32_t sec = total_sec % 60;
    snprintf(buf, sz, "%02u:%02u", min, sec);
}

/* 创建大号正方形图标按钮 */
static lv_obj_t *make_large_icon_btn(lv_obj_t *parent, const char *icon,
                                     int size, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_60, 0);
    lv_obj_set_style_radius(btn, ui_scale_min(12), 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    ui_attach_press_scale(btn, 230);

    lv_obj_t *ic = lv_label_create(btn);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_font(ic, ui_font_sym(), 0);
    lv_obj_set_style_text_color(ic, lv_color_white(), 0);
    lv_obj_center(ic);

    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);
    }

    return btn;
}

/* 创建大号带文字图标按钮（底部模式/变焦用） */
static lv_obj_t *make_text_icon_btn(lv_obj_t *parent, const char *icon,
                                    int size, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_60, 0);
    lv_obj_set_style_radius(btn, ui_scale_min(12), 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    ui_attach_press_scale(btn, 230);

    lv_obj_t *ic = lv_label_create(btn);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_font(ic, ui_font_sym(), 0);
    lv_obj_set_style_text_color(ic, lv_color_white(), 0);
    lv_obj_center(ic);

    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);
    }

    return btn;
}

/* ------------------------------------------------------------------ */
/* 主页刷新函数                                                        */
/* ------------------------------------------------------------------ */

/* WiFi 连接状态定时检查：连接时设置 STATUS_WIFI 显示图标，断开时清除 */
#ifndef APP_WIFI_DISABLED
static void wifi_status_timer_cb(lv_timer_t *t)
{
    (void)t;
    // #region debug-point B:wifi-timer
    dbg_log("B", "page_home.c:wifi_status_timer_cb", "fire");
    // #endregion
    char ssid[WIFI_SSID_MAX] = {0};
    char ip[32] = {0};
    char state[32] = {0};
    int connected = (wpa_status(ssid, sizeof(ssid), ip, sizeof(ip),
                                state, sizeof(state)) == 0
                     && strcmp(state, "COMPLETED") == 0);
    if (connected && !(g_status_flags & STATUS_WIFI)) {
        g_status_flags |= STATUS_WIFI;
        home_refresh_status_icons();
    } else if (!connected && (g_status_flags & STATUS_WIFI)) {
        g_status_flags &= ~STATUS_WIFI;
        home_refresh_status_icons();
    }
}
#endif

static void home_refresh_status_icons(void)
{
    if (!g_status_box) return;
    lv_obj_clean(g_status_box);

    /* 默认显示 GPS 占位，后续按 g_status_flags 扩展 */
    if (g_status_flags & STATUS_GPS) {
        lv_obj_t *ic = lv_label_create(g_status_box);
        lv_label_set_text(ic, LV_SYMBOL_GPS);
        lv_obj_set_style_text_font(ic, ui_font_sym(), 0);
        lv_obj_set_style_text_color(ic, lv_color_white(), 0);
    }
    if (g_status_flags & STATUS_WIFI) {
        lv_obj_t *ic = lv_label_create(g_status_box);
        lv_label_set_text(ic, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_font(ic, ui_font_sym(), 0);
        lv_obj_set_style_text_color(ic, lv_color_white(), 0);
    }
    if (g_status_flags & STATUS_BT) {
        lv_obj_t *ic = lv_label_create(g_status_box);
        lv_label_set_text(ic, LV_SYMBOL_BLUETOOTH);
        lv_obj_set_style_text_font(ic, ui_font_sym(), 0);
        lv_obj_set_style_text_color(ic, lv_color_white(), 0);
    }
}

static void home_refresh_mode_icon(void)
{
    if (!g_mode_icon_btn) return;
    lv_obj_t *ic = lv_obj_get_child(g_mode_icon_btn, 0);
    if (ic) {
        lv_label_set_text(ic, g_modes[g_func_mode].icon);
    }
}

static void home_refresh_bottom_params(void)
{
    if (!g_mode_param_label) return;
    if (g_func_mode == 0 || g_func_mode == 2) {
        /* 拍照/夜景模式：显示画幅 */
        lv_label_set_text_fmt(g_mode_param_label, "%s", g_ratio_opts[g_sel_ratio]);
    } else if (g_func_mode == 5) {
        /* 延时模式：显示画幅 + 延时标识 */
        lv_label_set_text_fmt(g_mode_param_label, "%s · %s",
                              g_ratio_opts[g_sel_ratio],
                              g_app_language ? "Lapse" : "延时");
    } else if (g_func_mode == 4) {
        /* 慢动作模式：显示帧率 */
        lv_label_set_text_fmt(g_mode_param_label, "%s · %s",
                              g_res_opts[g_sel_res],
                              g_app_language ? "Slow-Mo" : "慢动作");
    } else {
        /* 视频及跟随模式：显示分辨率 / 增稳 */
        lv_label_set_text_fmt(g_mode_param_label, "%s · %s",
                              g_res_opts[g_sel_res], STAB_OPTS[g_sel_stab]);
    }
}

static void home_refresh_zoom_btn(void)
{
    if (!g_zoom_btn_label) return;
    lv_label_set_text_fmt(g_zoom_btn_label, "%.1fx", g_zoom);
}

/* 刷新网格线显示：根据 g_sel_grid 创建或隐藏三分线 overlay。
 * 网格线为 4 条线（2 横 2 竖），将画面分为 3x3 等分，辅助构图。
 * overlay 全屏铺满，网格线相对屏幕定位。 */
static void home_refresh_grid(void)
{
    if (g_sel_grid) {
        /* 显示网格线：首次创建，后续复用 */
        if (!g_grid_overlay) {
            g_grid_overlay = lv_obj_create(lv_layer_top());
            lv_obj_set_size(g_grid_overlay, lv_pct(100), lv_pct(100));
            lv_obj_set_style_bg_opa(g_grid_overlay, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(g_grid_overlay, 0, 0);
            lv_obj_set_style_pad_all(g_grid_overlay, 0, 0);
            lv_obj_remove_flag(g_grid_overlay, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

            /* 屏幕像素尺寸用于计算网格线位置 */
            int sw = ui_get_screen_width();
            int sh = ui_get_screen_height();

            /* 创建 4 条线：2 横（1/3, 2/3 处）+ 2 竖（1/3, 2/3 处） */
            lv_obj_t *lines[4];
            for (int i = 0; i < 4; i++) {
                lines[i] = lv_obj_create(g_grid_overlay);
                lv_obj_set_style_bg_color(lines[i], lv_color_white(), 0);
                lv_obj_set_style_bg_opa(lines[i], LV_OPA_40, 0);
                lv_obj_set_style_border_width(lines[i], 0, 0);
                lv_obj_remove_flag(lines[i], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
            }
            /* 横线 1：y = sh/3 */
            lv_obj_set_size(lines[0], lv_pct(100), 1);
            lv_obj_align(lines[0], LV_ALIGN_TOP_LEFT, 0, sh / 3);
            /* 横线 2：y = sh*2/3 */
            lv_obj_set_size(lines[1], lv_pct(100), 1);
            lv_obj_align(lines[1], LV_ALIGN_TOP_LEFT, 0, sh * 2 / 3);
            /* 竖线 1：x = sw/3 */
            lv_obj_set_size(lines[2], 1, lv_pct(100));
            lv_obj_align(lines[2], LV_ALIGN_TOP_LEFT, sw / 3, 0);
            /* 竖线 2：x = sw*2/3 */
            lv_obj_set_size(lines[3], 1, lv_pct(100));
            lv_obj_align(lines[3], LV_ALIGN_TOP_LEFT, sw * 2 / 3, 0);
        }
        lv_obj_remove_flag(g_grid_overlay, LV_OBJ_FLAG_HIDDEN);
        /* 网格线移到 layer_top 最底层，避免遮挡参数面板/倒计时等模态 overlay */
        lv_obj_move_background(g_grid_overlay);
    } else {
        /* 隐藏网格线 */
        if (g_grid_overlay) {
            lv_obj_add_flag(g_grid_overlay, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 画幅裁剪引导（WYSIWYG）                                             */
/* ------------------------------------------------------------------ */
/* 计算画幅裁剪区域，与 photo_saver.c 的 rgb565_center_crop 逻辑严格一致。
 * 输入：scr_w/scr_h（预览缓冲区=屏幕逻辑分辨率），ratio（0=4:3,1=16:9,2=1:1）
 * 输出：crop_w/crop_h（裁剪后尺寸），off_x/off_y（中心偏移）。
 * 两侧被裁掉的尺寸 = (scr - crop) / 2（off 即每侧遮罩宽度/高度）。 */
static void home_compute_crop_region(int scr_w, int scr_h, int ratio,
                                      int *crop_w, int *crop_h,
                                      int *off_x, int *off_y)
{
    double target_ratio;
    switch (ratio) {
        case 0:  target_ratio = 4.0 / 3.0;   break;  /* 4:3 */
        case 1:  target_ratio = 16.0 / 9.0;  break;  /* 16:9 */
        case 2:  target_ratio = 1.0;         break;  /* 1:1 */
        default: target_ratio = 16.0 / 9.0;  break;
    }
    double src_ratio = (double)scr_w / (double)scr_h;

    int cw, ch;
    if (src_ratio > target_ratio) {
        /* 源图更宽：保留全高，按目标比例裁宽度 → 左右遮罩 */
        ch = scr_h;
        cw = (int)((double)scr_h * target_ratio + 0.5);
    } else {
        /* 源图更高：保留全宽，按目标比例裁高度 → 上下遮罩 */
        cw = scr_w;
        ch = (int)((double)scr_w / target_ratio + 0.5);
    }
    if (cw > scr_w) cw = scr_w;
    if (ch > scr_h) ch = scr_h;

    *crop_w = cw;
    *crop_h = ch;
    *off_x = (scr_w - cw) / 2;
    *off_y = (scr_h - ch) / 2;
}

/* 创建画幅裁剪引导 overlay（在 home_camera_start 中、canvas 之后调用）。
 * 必须在顶部状态栏/按钮/底部栏创建之前调用，以保证 z-order：
 *   canvas（最底层） < 裁剪引导 overlay < 控件（最顶层）。
 * overlay 全屏铺满，遮罩条相对屏幕边缘定位。
 * 初始隐藏，由 home_update_crop_guide 按需显示。 */
static void home_create_crop_guide(void)
{
    if (g_crop_guide_overlay || !g_camera_view) return;

    g_crop_guide_overlay = lv_obj_create(g_camera_view);
    lv_obj_remove_style_all(g_crop_guide_overlay);
    lv_obj_set_size(g_crop_guide_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(g_crop_guide_overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_crop_guide_overlay, 0, 0);
    lv_obj_set_style_pad_all(g_crop_guide_overlay, 0, 0);
    lv_obj_remove_flag(g_crop_guide_overlay,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    /* 保留 GESTURE_BUBBLE：让 tileview 滑动导航手势穿透遮罩到 tile */

    /* 两条黑色不透明遮罩条（不捕获输入，事件穿透到 view） */
    g_crop_bar_a = lv_obj_create(g_crop_guide_overlay);
    g_crop_bar_b = lv_obj_create(g_crop_guide_overlay);
    lv_obj_t *bars[2] = { g_crop_bar_a, g_crop_bar_b };
    for (int i = 0; i < 2; i++) {
        lv_obj_remove_style_all(bars[i]);
        lv_obj_set_style_bg_color(bars[i], lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(bars[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bars[i], 0, 0);
        lv_obj_remove_flag(bars[i],
                           LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }
    /* 初始隐藏，等待 home_update_crop_guide 按画幅定位 */
    lv_obj_add_flag(g_crop_guide_overlay, LV_OBJ_FLAG_HIDDEN);
}

/* 刷新画幅裁剪引导显示。
 * - 拍照类模式（0/2/5）且摄像头预览存在时显示遮罩条
 * - 视频录制中/视频模式/慢动作模式隐藏（录制画面不应被裁剪引导遮挡）
 * - 遮罩条使用不透明黑色，使预览可见亮区 == 最终保存的照片画幅 */
static void home_update_crop_guide(void)
{
    /* overlay 未创建（摄像头未启动）时直接返回 */
    if (!g_crop_guide_overlay || !lv_obj_is_valid(g_crop_guide_overlay)) return;

    /* 录制中隐藏裁剪引导，避免遮挡录制画面 */
    int is_photo_mode = (g_func_mode == 0 || g_func_mode == 2 || g_func_mode == 5);
    int show = is_photo_mode && !g_recording &&
               g_camera_canvas && g_camera_buf_w > 0 && g_camera_buf_h > 0;

    if (!show) {
        lv_obj_add_flag(g_crop_guide_overlay, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    /* 计算裁剪区域（与 photo_saver 的 rgb565_center_crop 一致）。
     * canvas 全屏铺满，遮罩条直接相对屏幕边缘定位 */
    int cw, ch, ox, oy;
    home_compute_crop_region(g_camera_buf_w, g_camera_buf_h, g_sel_ratio,
                              &cw, &ch, &ox, &oy);

    if (oy > 0) {
        /* 上下遮罩：保留全宽，裁高度 */
        lv_obj_set_size(g_crop_bar_a, lv_pct(100), oy);
        lv_obj_align(g_crop_bar_a, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_size(g_crop_bar_b, lv_pct(100), oy);
        lv_obj_align(g_crop_bar_b, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    } else if (ox > 0) {
        /* 左右遮罩：保留全高，裁宽度 */
        lv_obj_set_size(g_crop_bar_a, ox, lv_pct(100));
        lv_obj_align(g_crop_bar_a, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_size(g_crop_bar_b, ox, lv_pct(100));
        lv_obj_align(g_crop_bar_b, LV_ALIGN_TOP_RIGHT, 0, 0);
    } else {
        /* 比例刚好匹配，无需遮罩：把两条缩到 0 */
        lv_obj_set_size(g_crop_bar_a, 0, 0);
        lv_obj_set_size(g_crop_bar_b, 0, 0);
    }

    lv_obj_remove_flag(g_crop_guide_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void home_refresh_rec_ui(void)
{
    if (!g_rec_btn || !g_rec_label) return;

    if (g_func_mode == 0 || g_func_mode == 2) {
        /* 拍照/夜景模式：显示白色快门按钮，无计时标签 */
        lv_obj_remove_flag(g_rec_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_rec_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(g_rec_btn, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_opa(g_rec_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(g_rec_btn, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_width(g_rec_btn, ui_scale_min(3), 0);
        lv_label_set_text(g_rec_label, "");
        lv_anim_del(g_rec_btn, rec_btn_set_scale);
        lv_obj_set_style_transform_scale(g_rec_btn, 256, 0);
        return;
    }

    if (g_func_mode == 5) {
        /* 延时模式：显示快门按钮，活跃时变红色 */
        lv_obj_remove_flag(g_rec_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_rec_label, LV_OBJ_FLAG_HIDDEN);
        if (g_timelapse_active) {
            lv_obj_set_style_bg_color(g_rec_btn, lv_color_hex(0xFF0000), 0);
            lv_obj_set_style_border_opa(g_rec_btn, LV_OPA_TRANSP, 0);
        } else {
            lv_obj_set_style_bg_color(g_rec_btn, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_border_opa(g_rec_btn, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(g_rec_btn, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_border_width(g_rec_btn, ui_scale_min(3), 0);
        }
        lv_label_set_text(g_rec_label, "");
        lv_anim_del(g_rec_btn, rec_btn_set_scale);
        lv_obj_set_style_transform_scale(g_rec_btn, 256, 0);
        return;
    }

    if (g_func_mode != 1 && g_func_mode != 4) {
        /* 其他模式隐藏按钮 */
        lv_obj_add_flag(g_rec_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_rec_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    /* 视频模式：显示录制按钮 */
    lv_obj_remove_flag(g_rec_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(g_rec_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_border_width(g_rec_btn, ui_scale_min(2), 0);

    if (g_recording) {
        lv_obj_set_style_bg_color(g_rec_btn, lv_color_hex(0xFF0000), 0);
        lv_obj_set_style_border_opa(g_rec_btn, LV_OPA_TRANSP, 0);
        char buf[16];
        fmt_rec_time(lv_tick_elaps(g_rec_start_ms), buf, sizeof(buf));
        lv_label_set_text(g_rec_label, buf);

        /* 启动录像脉冲缩放动画（1.0x ~ 1.12x 循环），提示正在录制。
         * 先删除可能存在的旧动画，避免每秒重复创建导致动画节点累积泄漏。
         * rec_timer_cb 每 1s 调用本函数刷新录制时间，若不删除旧动画，
         * 长时间录制会累积大量 INFINITE 动画节点（约 3600/小时） */
        lv_anim_del(g_rec_btn, rec_btn_set_scale);
        lv_anim_init(&g_rec_pulse);
        lv_anim_set_var(&g_rec_pulse, g_rec_btn);
        lv_anim_set_values(&g_rec_pulse, 256, 286);
        lv_anim_set_exec_cb(&g_rec_pulse, rec_btn_set_scale);
        lv_anim_set_time(&g_rec_pulse, 700);
        lv_anim_set_playback_time(&g_rec_pulse, 700);
        lv_anim_set_repeat_count(&g_rec_pulse, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&g_rec_pulse, lv_anim_path_ease_in_out);
        /* 从中心缩放 */
        lv_obj_set_style_transform_pivot_x(g_rec_btn, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(g_rec_btn, LV_PCT(50), 0);
        lv_anim_start(&g_rec_pulse);
    } else {
        lv_obj_set_style_bg_color(g_rec_btn, lv_color_hex(0x202020), 0);
        lv_obj_set_style_border_opa(g_rec_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(g_rec_btn, lv_color_hex(0xFF0000), 0);
        lv_label_set_text(g_rec_label, "");
        /* 停止脉冲动画并恢复 1x */
        lv_anim_del(g_rec_btn, rec_btn_set_scale);
        lv_obj_set_style_transform_scale(g_rec_btn, 256, 0);
    }
}

/* 动画 exec cb：为录像按钮设置整体缩放 */
static void rec_btn_set_scale(void *obj, int32_t v)
{
    lv_obj_t *btn = (lv_obj_t *)obj;
    if (!btn) return;
    lv_obj_set_style_transform_scale(btn, (int32_t)v, 0);
}

static void rec_timer_cb(lv_timer_t *t)
{
    (void)t;
    home_refresh_rec_ui();
}

/* ------------------------------------------------------------------ */
/* 轻量 Toast 提示（屏幕中央临时文字，2 秒后自动消失）                   */
/* ------------------------------------------------------------------ */
/* toast 定时器回调：销毁 toast label */
static void toast_timer_cb(lv_timer_t *t)
{
    /* LVGL v9 中 lv_timer_t 为不透明结构，须用 API 获取 user_data */
    lv_obj_t *toast = (lv_obj_t *)lv_timer_get_user_data(t);
    if (toast && lv_obj_is_valid(toast)) {
        lv_obj_delete(toast);
    }
    lv_timer_delete(t);
}

/* 显示一个临时 toast 提示（屏幕中央，2 秒后自动消失）。
 * 用于录制不可用等非阻塞式用户反馈 */
static void home_show_toast(const char *text)
{
    if (!text) return;
    lv_obj_t *toast = lv_label_create(lv_layer_top());
    lv_label_set_text(toast, text);
    lv_obj_set_style_text_font(toast, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(toast, lv_color_white(), 0);
    /* 半透明黑底，便于在不同背景上可读 */
    lv_obj_set_style_bg_color(toast, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_70, 0);
    lv_obj_set_style_pad_hor(toast, ui_scale_min(16), 0);
    lv_obj_set_style_pad_ver(toast, ui_scale_min(10), 0);
    lv_obj_set_style_radius(toast, ui_scale_min(8), 0);
    lv_obj_center(toast);
    lv_obj_move_foreground(toast);

    /* 2 秒后自动销毁 */
    lv_timer_t *tm = lv_timer_create(toast_timer_cb, 2000, toast);
    lv_timer_set_repeat_count(tm, 1);
}

/* ------------------------------------------------------------------ */
/* 页面跳转回调                                                        */
/* ------------------------------------------------------------------ */
static void on_album_click(lv_event_t *e)
{
    (void)e;
    LV_LOG_USER("Home: album button clicked, goto album tile");
    if (!g_tv || !g_tile_album) {
        LV_LOG_WARN("Home: album tile not ready");
        return;
    }
    if (!lv_obj_is_valid(g_tv) || !lv_obj_is_valid(g_tile_album)) {
        LV_LOG_WARN("Home: album tileview/tile invalid");
        return;
    }
    lv_tileview_set_tile(g_tv, g_tile_album, LV_ANIM_ON);
    /* 进入相册前刷新扫描：确保新拍摄的照片/视频立即可见 */
    page_album_refresh();
}

/* ------------------------------------------------------------------ */
/* 录像控制                                                            */
/* ------------------------------------------------------------------ */
/* 前向声明：on_shutter_btn_click 定义在拍照控制段，此处提前引用 */
static void on_shutter_btn_click(lv_event_t *e);
/* 前向声明：home_camera_start 定义在摄像头预览段，camera_start_timer_cb 提前引用 */
static void home_camera_start(lv_obj_t *parent_view);

/* 延时拍摄定时器回调：每 TIMELAPSE_INTERVAL_MS 触发一次拍照 */
static void timelapse_timer_cb(lv_timer_t *t)
{
    (void)t;
    /* 设置拍照标志，下一帧保存 */
    g_take_photo_pending = 1;
    LV_LOG_USER("延时拍摄：触发拍照");
}

/* 启动/停止延时拍摄 */
static void home_timelapse_toggle(void)
{
    if (g_timelapse_active) {
        /* 停止延时拍摄 */
        if (g_timelapse_timer) {
            lv_timer_delete(g_timelapse_timer);
            g_timelapse_timer = NULL;
        }
        g_timelapse_active = 0;
        home_show_toast(tr("延时拍摄已停止", "Timelapse stopped"));
        LV_LOG_USER("延时拍摄已停止");
    } else {
        /* 启动延时拍摄 */
        if (!g_camera || !g_camera_buf) {
            LV_LOG_WARN("延时拍摄失败：摄像头未就绪");
            return;
        }
        g_timelapse_active = 1;
        if (g_timelapse_timer) {
            lv_timer_delete(g_timelapse_timer);
        }
        g_timelapse_timer = lv_timer_create(timelapse_timer_cb, TIMELAPSE_INTERVAL_MS, NULL);
        /* 立即拍第一张 */
        g_take_photo_pending = 1;
        home_show_toast(tr("延时拍摄已启动", "Timelapse started"));
        LV_LOG_USER("延时拍摄已启动，间隔 %dms", TIMELAPSE_INTERVAL_MS);
    }
    home_refresh_rec_ui();
}

/* 拍摄按钮点击：拍照模式走拍照逻辑，视频模式走录制逻辑。
 * 按钮在不同模式下复用（快门/录制），通过 g_func_mode 区分行为。
 * - mode 0 (拍照)：触发快门（含倒计时）
 * - mode 1 (视频)：录制控制
 * - mode 2 (夜景)：触发快门（夜景参数已预设）
 * - mode 3 (跟随)：暂未实现，提示用户
 * - mode 4 (慢动作)：录制控制（高帧率协商）
 * - mode 5 (延时)：启动/停止定时拍照序列 */
static void on_rec_btn_click(lv_event_t *e)
{
    (void)e;

    /* 拍照/夜景模式：触发快门 */
    if (g_func_mode == 0 || g_func_mode == 2) {
        on_shutter_btn_click(e);
        return;
    }

    /* 延时模式：启动/停止定时拍照序列 */
    if (g_func_mode == 5) {
        home_timelapse_toggle();
        return;
    }

    /* 跟随模式：需 AI 算法支持，暂未实现 */
    if (g_func_mode == 3) {
        home_show_toast(tr("跟随模式即将上线", "Tracking coming soon"));
        return;
    }

    /* 视频模式(1) / 慢动作模式(4)：录制控制 */
    if (g_func_mode != 1 && g_func_mode != 4) return;
    if (!g_recording) {
        /* 启动录制：先检查后端是否可用 */
        if (!recorder_is_available()) {
            LV_LOG_WARN("录制后端不可用，后续接入 RK MPP/K230 VPU 后启用");
            home_show_toast(tr("录制功能即将上线", "Recording coming soon"));
            return;
        }
        /* 必须先启动摄像头，才能获取实际协商的帧尺寸 */
        if (!g_camera || g_cam_src_w <= 0 || g_cam_src_h <= 0) {
            LV_LOG_WARN("录制失败：摄像头未就绪或未协商到有效格式");
            home_show_toast(tr("请先开启摄像头", "Camera not ready"));
            return;
        }

        const device_config_t *cfg = device_config_get();

        /* 生成带时间戳的输出路径：settings_storage_get_path()/DCIM/VID_YYYYMMDD_HHMMSS.mp4
         * MP4 容器：RK3566 用 H.264 硬件编码，K230/PC 用 MJPEG 软件编码（hal 层自动选择）。
         * 注意：rec_path 必须使用 static，因为 recorder_create 内部仅浅拷贝 output_path 指针，
         * 录制期间该指针需持续有效（栈变量在函数返回后失效会导致悬空指针） */
        static char rec_path[256];
        {
            const char *base = settings_storage_get_path();
            char ts[32];
            time_t now = time(NULL);
            struct tm *tm_info = localtime(&now);
            if (tm_info) {
                strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", tm_info);
            } else {
                snprintf(ts, sizeof(ts), "%lld", (long long)now);
            }
            snprintf(rec_path, sizeof(rec_path), "%s/DCIM/VID_%s.mp4", base, ts);
        }

        /* 录制配置：使用摄像头实际协商的原始帧尺寸（非 camera_caps 上限）。
         * codec=H264：RK3566 上 hal 层自动选择 RKMPP 硬件后端，其他平台回退 MJPEG 软件后端。
         * 旋转角度与预览一致（横屏 180°，竖屏 90°） */
        recorder_config_t rec_cfg = {
            .width   = g_cam_src_w,
            .height  = g_cam_src_h,
            .fps     = (g_cam_src_fps > 0) ? g_cam_src_fps : 30,
            .bitrate_kbps = 20000,   /* H.264 硬件编码码率，MJPEG 软件后端忽略此字段 */
            .codec   = REC_CODEC_H264,         /* 优先 H.264，无 MPP 时 hal 层回退 MJPEG */
            .container = REC_CONTAINER_MP4,    /* 统一 MP4 容器 */
            .output_path = rec_path,
            .rotate  = (g_camera_buf_w >= g_camera_buf_h) ? CAM_ROTATE_180 : CAM_ROTATE_90,
        };

        /* 限制不超过摄像头能力上限（安全检查） */
        if (rec_cfg.fps > cfg->camera_caps.max_video_fps) {
            rec_cfg.fps = cfg->camera_caps.max_video_fps;
        }

        g_recorder = recorder_create(&rec_cfg);
        if (!g_recorder) {
            LV_LOG_ERROR("recorder_create 失败（配置: %dx%d@%d）",
                         rec_cfg.width, rec_cfg.height, rec_cfg.fps);
            home_show_toast(tr("录制启动失败", "Recording start failed"));
            return;
        }
        /* 诊断日志：记录录制方向配置，用于排查"存储与预览方向不一致"问题。
         * rotate 值与预览 camera_timer_cb 中一致（横屏 180°，竖屏 90°） */
        LV_LOG_USER("录制启动: %dx%d rotate=%d (%s), buf=%dx%d, src=%dx%d",
                    rec_cfg.width, rec_cfg.height, (int)rec_cfg.rotate,
                    rec_cfg.rotate == CAM_ROTATE_180 ? "180°横屏" :
                    rec_cfg.rotate == CAM_ROTATE_90 ? "90°竖屏" : "其他",
                    g_camera_buf_w, g_camera_buf_h, g_cam_src_w, g_cam_src_h);
        if (recorder_start(g_recorder) != 0) {
            LV_LOG_ERROR("recorder_start 失败");
            recorder_destroy(g_recorder);
            g_recorder = NULL;
            home_show_toast(tr("录制启动失败", "Recording start failed"));
            return;
        }

        g_recording = 1;
        g_rec_start_ms = lv_tick_get();
        /* 录制开始：隐藏画幅裁剪引导，避免遮挡录制画面 */
        home_update_crop_guide();
        if (!g_rec_timer) {
            g_rec_timer = lv_timer_create(rec_timer_cb, 1000, NULL);
        }
        /* 关联录制器到 preview worker：worker 线程在每帧 acquire 后自动喂帧给录制器。
         * 取代旧的 camera_timer_cb 中手动 recorder_feed_frame 调用，
         * 录制器现在以摄像头原始帧率（30fps）接收帧，而非 UI 定时器帧率。 */
        if (g_preview) {
            camera_preview_set_recorder(g_preview, g_recorder);
            camera_preview_set_recording(g_preview, 1);
        }
        LV_LOG_USER("录制已启动: %dx%d@%dfps MP4 → %s",
                    rec_cfg.width, rec_cfg.height, rec_cfg.fps,
                    rec_cfg.output_path);
    } else {
        /* 停止录制 */
        /* 先通知 preview worker 停止喂帧，再停止录制器，避免竞争 */
        if (g_preview) {
            camera_preview_set_recording(g_preview, 0);
            camera_preview_set_recorder(g_preview, NULL);
        }
        if (g_recorder) {
            recorder_stop(g_recorder);
            recorder_destroy(g_recorder);
            g_recorder = NULL;
        }
        g_recording = 0;
        if (g_rec_timer) {
            lv_timer_delete(g_rec_timer);
            g_rec_timer = NULL;
        }
        LV_LOG_USER("录制已停止");
        /* 录制停止：恢复画幅裁剪引导显示（拍照模式下） */
        home_update_crop_guide();
        /* 刷新存储信息显示（录制生成了视频文件，可用空间变化） */
        if (g_storage_label) update_storage_info(g_storage_label);
    }
    home_refresh_rec_ui();
}

/* ------------------------------------------------------------------ */
/* 拍照控制                                                            */
/* ------------------------------------------------------------------ */

/* 快门闪烁动画回调：透明度从 255→0 快速淡出，模拟相机快门闪光 */
static void flash_anim_cb(void *obj, int32_t v)
{
    lv_obj_t *flash = (lv_obj_t *)obj;
    if (!flash) return;
    lv_obj_set_style_bg_opa(flash, (lv_opa_t)v, 0);
}

/* 快门闪烁动画完成回调：隐藏 overlay */
static void flash_anim_ready(lv_anim_t *a)
{
    lv_obj_t *flash = (lv_obj_t *)a->var;
    if (flash) lv_obj_add_flag(flash, LV_OBJ_FLAG_HIDDEN);
}

/* 触发快门闪烁动画：白色全屏覆盖快速淡出（约 200ms） */
static void home_flash_shutter(void)
{
    if (!g_flash_overlay) {
        g_flash_overlay = lv_obj_create(lv_layer_top());
        lv_obj_set_size(g_flash_overlay, lv_pct(100), lv_pct(100));
        lv_obj_set_style_bg_color(g_flash_overlay, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(g_flash_overlay, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(g_flash_overlay, 0, 0);
        lv_obj_set_style_pad_all(g_flash_overlay, 0, 0);
        lv_obj_remove_flag(g_flash_overlay, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(g_flash_overlay, LV_OBJ_FLAG_HIDDEN);
    }

    /* 显示 overlay 并设置初始不透明度 */
    lv_obj_set_style_bg_opa(g_flash_overlay, LV_OPA_COVER, 0);
    lv_obj_remove_flag(g_flash_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_flash_overlay);

    /* 启动淡出动画：255 → 0，200ms */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, g_flash_overlay);
    lv_anim_set_values(&a, 255, 0);
    lv_anim_set_exec_cb(&a, flash_anim_cb);
    lv_anim_set_time(&a, 200);
    lv_anim_set_ready_cb(&a, flash_anim_ready);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* 倒计时定时器回调：每秒减 1，到 0 时触发拍照 */
static void countdown_timer_cb(lv_timer_t *t)
{
    (void)t;
    g_countdown_remaining--;

    if (g_countdown_remaining > 0) {
        /* 更新倒计时数字显示 */
        if (g_countdown_label) {
            lv_label_set_text_fmt(g_countdown_label, "%d", g_countdown_remaining);
        }
        LV_LOG_USER("倒计时: %d", g_countdown_remaining);
    } else {
        /* 倒计时结束：触发拍照 */
        if (g_countdown_timer) {
            lv_timer_delete(g_countdown_timer);
            g_countdown_timer = NULL;
        }
        if (g_countdown_overlay) {
            lv_obj_add_flag(g_countdown_overlay, LV_OBJ_FLAG_HIDDEN);
        }
        g_countdown_remaining = 0;

        /* 设置拍照标志，下一帧保存 */
        g_take_photo_pending = 1;
        /* 启动连拍序列（若开启）：第一张已触发，后续由定时器按间隔触发 */
        home_start_burst();
        LV_LOG_USER("倒计时结束，触发拍照");
    }
}

/* 启动拍照倒计时。根据 g_sel_timer 选择时长：
 * 1=2s, 2=5s, 3=10s。倒计时期间显示大号数字 overlay。 */
static void home_start_countdown(void)
{
    int seconds = 0;
    switch (g_sel_timer) {
        case 1: seconds = 2;  break;
        case 2: seconds = 5;  break;
        case 3: seconds = 10; break;
        default: seconds = 0; break;
    }

    if (seconds <= 0) {
        /* 无倒计时，直接拍照 */
        g_take_photo_pending = 1;
        /* 启动连拍序列（若开启）：第一张已触发，后续由定时器按间隔触发 */
        home_start_burst();
        LV_LOG_USER("无倒计时，直接拍照");
        return;
    }

    /* 创建倒计时 overlay（首次） */
    if (!g_countdown_overlay) {
        g_countdown_overlay = lv_obj_create(lv_layer_top());
        lv_obj_set_size(g_countdown_overlay, lv_pct(100), lv_pct(100));
        lv_obj_set_style_bg_color(g_countdown_overlay, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(g_countdown_overlay, LV_OPA_50, 0);
        lv_obj_set_style_border_width(g_countdown_overlay, 0, 0);
        lv_obj_set_style_pad_all(g_countdown_overlay, 0, 0);
        lv_obj_remove_flag(g_countdown_overlay, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        g_countdown_label = lv_label_create(g_countdown_overlay);
        lv_obj_set_style_text_font(g_countdown_label, ui_font_cjk_lg(), 0);
        lv_obj_set_style_text_color(g_countdown_label, lv_color_white(), 0);
        lv_obj_center(g_countdown_label);
    }

    /* 显示倒计时数字（初始显示 seconds，定时器每秒减 1） */
    lv_label_set_text_fmt(g_countdown_label, "%d", seconds);
    lv_obj_remove_flag(g_countdown_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_countdown_overlay);

    /* remaining 初始值 = seconds，定时器首次触发在 1 秒后，
     * 此时 remaining-- 变为 seconds-1 并显示，符合预期：
     * 设置 2 秒倒计时 → 显示 2 → (1s后)显示 1 → (2s后)拍照 */
    g_countdown_remaining = seconds;
    if (g_countdown_timer) {
        lv_timer_delete(g_countdown_timer);
    }
    g_countdown_timer = lv_timer_create(countdown_timer_cb, 1000, NULL);
    LV_LOG_USER("拍照倒计时启动: %d 秒", seconds);
}

/* 快门按钮点击：根据倒计时设置决定立即拍照或启动倒计时。
 * 倒计时结束或无倒计时 → 设置 g_take_photo_pending，下一帧保存 */
static void on_shutter_btn_click(lv_event_t *e)
{
    (void)e;
    /* 拍照模式(0) 和夜景模式(2) 都走拍照逻辑 */
    if (g_func_mode != 0 && g_func_mode != 2) return;
    if (!g_camera || !g_camera_buf) {
        LV_LOG_WARN("拍照失败：摄像头未就绪");
        return;
    }
    /* 已在倒计时中，忽略重复点击 */
    if (g_countdown_remaining > 0) return;
    /* 延时拍摄中不响应手动快门（避免冲突） */
    if (g_timelapse_active) return;
    /* 连拍进行中不响应重复点击（避免叠加连拍序列） */
    if (g_burst_active) return;

    /* 根据倒计时设置启动 */
    home_start_countdown();
}

/* 根据连拍档位获取连拍张数和间隔（毫秒）。
 * burst_idx: 0=关闭, 1=3fps, 2=5fps, 3=10fps
 * out_count: 连拍总张数（含第一张）
 * out_interval_ms: 张与张之间的间隔 */
static void burst_get_params(int burst_idx, int *out_count, int *out_interval_ms)
{
    switch (burst_idx) {
        case 1:  *out_count = 3;  *out_interval_ms = 333; break;  /* 3 fps */
        case 2:  *out_count = 5;  *out_interval_ms = 200; break;  /* 5 fps */
        case 3:  *out_count = 10; *out_interval_ms = 100; break;  /* 10 fps */
        default: *out_count = 1;  *out_interval_ms = 0;   break;  /* 关闭：单张 */
    }
}

/* 连拍定时器回调：触发下一张拍照。
 * 每次触发设置 g_take_photo_pending=1，由 camera_timer_cb 实际保存帧。
 * 连拍完成后停止定时器 */
static void burst_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (g_burst_remaining <= 0) {
        /* 连拍已结束，停止定时器 */
        if (g_burst_timer) {
            lv_timer_delete(g_burst_timer);
            g_burst_timer = NULL;
        }
        return;
    }

    /* 触发一次拍照（下一帧 camera_timer_cb 中执行 home_take_photo） */
    g_take_photo_pending = 1;
    g_burst_remaining--;

    LV_LOG_USER("连拍触发: 剩余 %d 张", g_burst_remaining);

    if (g_burst_remaining <= 0 && g_burst_timer) {
        /* 最后一张已触发，延迟停止定时器（等当前帧保存完成）。
         * 定时器设为单次模式即可自动停止 */
        lv_timer_set_repeat_count(g_burst_timer, 0);
    }
}

/* 启动连拍序列。在第一张照片触发后调用（g_take_photo_pending 已置 1）。
 * 根据连拍档位创建定时器，按指定间隔触发后续拍照。
 * 连拍关闭时（g_sel_burst=0）不启动定时器，仅拍单张 */
static void home_start_burst(void)
{
    /* 仅拍照模式支持连拍，其他模式忽略 */
    if (g_func_mode != 0 && g_func_mode != 2) return;
    /* 延时拍摄中不启动连拍（避免冲突） */
    if (g_timelapse_active) return;

    int count = 1, interval_ms = 0;
    burst_get_params(g_sel_burst, &count, &interval_ms);

    if (count <= 1) {
        /* 单张拍摄，无需连拍定时器 */
        g_burst_remaining = 0;
        g_burst_active    = 0;
        g_burst_shot_num  = 0;
        return;
    }

    /* 第一张已通过 g_take_photo_pending 触发，剩余 count-1 张由定时器触发 */
    g_burst_remaining = count - 1;
    g_burst_active    = 1;
    g_burst_shot_num  = 0;  /* 将在 home_take_photo 中递增为 1 */

    /* 清理旧定时器（避免重复连拍叠加） */
    if (g_burst_timer) {
        lv_timer_delete(g_burst_timer);
        g_burst_timer = NULL;
    }

    g_burst_timer = lv_timer_create(burst_timer_cb, interval_ms, NULL);
    /* 显示连拍启动提示（仅连拍模式，不干扰单张拍摄） */
    home_show_toast(tr("连拍已启动", "Burst started"));
    LV_LOG_USER("连拍启动: 共 %d 张, 间隔 %dms", count, interval_ms);
}

/* 停止连拍（用于模式切换/页面销毁时清理） */
static void home_stop_burst(void)
{
    if (g_burst_timer) {
        lv_timer_delete(g_burst_timer);
        g_burst_timer = NULL;
    }
    g_burst_remaining = 0;
    g_burst_active    = 0;
    g_burst_shot_num  = 0;
    photo_set_burst_seq(0);  /* 恢复正常文件名 */
}

/* 在 camera_timer_cb 中调用：保存当前预览帧为 JPEG 文件。
 * 保存路径：settings_storage_get_path()/DCIM/IMG_YYYYMMDD_HHMMSS.jpg
 * 按用户选择的画幅（4:3/16:9/1:1）裁剪 + 图片大小（12MP/20MP/24MP/48MP）缩放保存。
 * 保存成功后触发快门闪烁动画。
 * 性能优化：使用异步后台线程编码 JPEG，不阻塞 UI 线程。
 * 快门闪烁立即触发，JPEG 编码在后台完成后更新存储信息。 */
static void home_take_photo(void)
{
    if (!g_preview) {
        LV_LOG_WARN("拍照失败: preview 未就绪");
        return;
    }

    /* 获取存储路径：用户选择的存储位置（内置/SD卡），缓存到全局供
     * camera_timer_cb 的 photo_ready 路径使用（避免拍照快照就绪时再查询） */
    const char *base = settings_storage_get_path();
    snprintf(g_photo_save_dir, sizeof(g_photo_save_dir), "%s/DCIM", base);

    /* 请求 worker 线程做全分辨率拍照快照：worker 在下一帧转换时，
     * 对当前帧做"仅旋转不降采样"转换到 photo_buf（原始分辨率，如 1920x1080）。
     * 转换完成后通过 camera_preview_is_photo_ready 通知 UI 线程，
     * 由 camera_timer_cb 取走 photo_buf 启动异步 JPEG 编码保存。
     *
     * 这样保存的照片使用原始分辨率，而非降采样后的预览 buffer（480x800），
     * 避免画质损失和预览/保存画面不一致问题。 */
    int ret = camera_preview_take_photo(g_preview);
    if (ret == 0) {
        /* 拍照请求成功：设置连拍序号（仅成功时递增，避免跳号）。
         * 连拍进行中时文件名追加 _NN 后缀避免同秒覆盖 */
        if (g_burst_active) {
            g_burst_shot_num++;
            photo_set_burst_seq(g_burst_shot_num);
        } else {
            photo_set_burst_seq(0);
        }
        LV_LOG_USER("已请求全分辨率拍照快照 (画幅=%s, 连拍=%s)",
                    g_ratio_opts[g_sel_ratio], g_burst_active ? "是" : "否");
    } else if (ret == -2) {
        LV_LOG_WARN("拍照跳过: 上次拍照快照未取走");
        /* 连拍模式：重试本次拍照（下一帧再次触发）。
         * worker 处理一帧约 33ms，下一帧 camera_timer_cb 应已就绪。
         * 非连拍模式（用户手动点击）：跳过即可，用户可再次点击快门 */
        if (g_burst_active) {
            g_take_photo_pending = 1;
        }
    } else {
        LV_LOG_ERROR("拍照失败: camera_preview_take_photo 返回 %d", ret);
    }

    /* 连拍序列结束检测：所有连拍张数已拍完，重置状态。
     * 注意：连拍定时器通过 g_take_photo_pending 触发 home_take_photo，
     * 但实际保存是异步的（worker 转换 + 后台 JPEG 编码），
     * 连拍序号已在拍照请求时记录，文件名不会冲突。 */
    if (g_burst_active && g_burst_remaining == 0) {
        LV_LOG_USER("连拍序列完成: 共 %d 张", g_burst_shot_num);
        g_burst_active   = 0;
        g_burst_shot_num = 0;
        photo_set_burst_seq(0);
        /* 定时器已通过 repeat_count=0 自动删除，置 NULL 避免悬空指针 */
        g_burst_timer    = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* 双击/长按回调（预览区手势）                                          */
/* ------------------------------------------------------------------ */
static void on_view_double_click(lv_event_t *e)
{
    (void)e;
    // #region debug-point C:mode-switch
    dbg_log("C", "page_home.c:on_view_double_click", "start");
    // #endregion
    /* 录制中切换模式：先停止录制，避免状态混乱（录制器需视频模式持续 feed 帧） */
    if (g_recording && g_func_mode == 1) {
        /* 先通知 preview worker 停止喂帧 */
        if (g_preview) {
            camera_preview_set_recording(g_preview, 0);
            camera_preview_set_recorder(g_preview, NULL);
        }
        if (g_recorder) {
            recorder_stop(g_recorder);
            recorder_destroy(g_recorder);
            g_recorder = NULL;
        }
        g_recording = 0;
        if (g_rec_timer) {
            lv_timer_delete(g_rec_timer);
            g_rec_timer = NULL;
        }
        LV_LOG_USER("切换模式前自动停止录制");
    }
    /* 倒计时中切换模式：取消倒计时，避免切到视频模式后仍触发拍照 */
    if (g_countdown_remaining > 0) {
        if (g_countdown_timer) {
            lv_timer_delete(g_countdown_timer);
            g_countdown_timer = NULL;
        }
        if (g_countdown_overlay) {
            lv_obj_add_flag(g_countdown_overlay, LV_OBJ_FLAG_HIDDEN);
        }
        g_countdown_remaining = 0;
    }
    /* 延时拍摄中切换模式：停止延时拍摄 */
    if (g_timelapse_active) {
        if (g_timelapse_timer) {
            lv_timer_delete(g_timelapse_timer);
            g_timelapse_timer = NULL;
        }
        g_timelapse_active = 0;
    }
    /* 切换模式时停止连拍（避免跨模式连拍） */
    home_stop_burst();
    /* 双击切换拍照/视频模式，同时保存/加载对应模式参数 */
    int old_mode = g_func_mode;
    g_func_mode = (g_func_mode == 0) ? 1 : 0;
    mode_params_save(old_mode);
    mode_params_load(g_func_mode);
    home_refresh_mode_icon();
    home_refresh_bottom_params();
    home_refresh_rec_ui();
    home_refresh_grid();              /* 不同模式网格线开关可能不同 */
    home_apply_camera_controls();     /* 不同模式参数不同，需重新应用到摄像头 */
    home_param_overlay_refresh();
    page_quick_refresh();             /* 同步刷新上滑菜单：不同模式显示不同快捷参数 */
    LV_LOG_USER("Double click: switch to mode %d", g_func_mode);
}

static void on_view_long_press(lv_event_t *e)
{
    (void)e;
    /* 长按跳转到快速设置 */
    lv_tileview_set_tile(g_tv, g_tile_quick, LV_ANIM_ON);
    LV_LOG_USER("Long press: goto quick settings");
}

/* ================================================================== */
/* 参数设置弹层                                                        */
/* ================================================================== */
static void home_param_overlay_refresh(void);
static void home_selector_overlay_close(void);

static void home_param_overlay_close(void)
{
    home_selector_overlay_close();
    if (g_param_overlay) {
        lv_obj_add_flag(g_param_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

static void param_overlay_bg_click(lv_event_t *e)
{
    (void)e;
    home_param_overlay_close();
}

static void param_overlay_back(lv_event_t *e)
{
    (void)e;
    home_param_overlay_close();
}

static void on_param_mode_click(lv_event_t *e)
{
    (void)e;
    g_param_mode = !g_param_mode;
    mode_params_save(g_func_mode);
    home_param_overlay_refresh();
    /* 模式切换后立即应用到摄像头 HAL，使曝光/白平衡等实时生效
     * （自动模式重置为驱动自动控制，手动模式应用用户参数） */
    home_apply_camera_controls();
    /* 持久化到文件，下次启动自动恢复 */
    mode_params_save_to_file();
}

/* 二级选择器关闭 */
static void home_selector_overlay_close(void)
{
    if (g_selector_overlay) {
        lv_obj_add_flag(g_selector_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    g_sel_opts     = NULL;
    g_sel_count    = 0;
    g_sel_idx      = NULL;
    g_sel_changed  = NULL;
}

static void selector_overlay_bg_click(lv_event_t *e)
{
    (void)e;
    home_selector_overlay_close();
}

static void selector_item_click(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (g_sel_idx && idx >= 0 && idx < g_sel_count) {
        *g_sel_idx = idx;
        if (g_sel_changed) g_sel_changed();
    }
    home_selector_overlay_close();
    home_param_overlay_refresh();
    home_refresh_bottom_params();
}

/* 创建/更新二级选择器 */
static void home_selector_overlay_open(const char *title,
                                       const char **opts, int count,
                                       int *sel, void (*on_changed)(void))
{
    if (!g_selector_overlay) return;

    g_sel_opts    = opts;
    g_sel_count   = count;
    g_sel_idx     = sel;
    g_sel_changed = on_changed;

    /* 更新标题：overlay → panel(0) → header(0) → back_btn(0), title(1) */
    lv_obj_t *panel = lv_obj_get_child(g_selector_overlay, 0);
    if (panel) {
        lv_obj_t *header = lv_obj_get_child(panel, 0);
        if (header) {
            /* header 子对象：[0]=back_btn, [1]=title_label */
            lv_obj_t *title_lbl = lv_obj_get_child(header, 1);
            if (title_lbl) lv_label_set_text(title_lbl, title);
        }
    }

    /* 选项列表容器：放在 panel 内，占据标题栏以下全部空间。
     * 竖屏：单列列表，按钮 100% 宽度。
     * 横屏：双列 wrap（LV_FLEX_FLOW_ROW_WRAP），按钮约 48% 宽度，避免文字过扁。 */
    if (g_selector_list) {
        lv_obj_clean(g_selector_list);
    } else {
        /* 注意：列表容器必须是 panel 的子对象，否则层级错误会覆盖标题栏。
         * 高度使用 flex_grow=1 填充标题栏以下剩余空间，避免 100% 高度溢出 panel */
        g_selector_list = lv_obj_create(panel);
        lv_obj_set_width(g_selector_list, lv_pct(100));
        lv_obj_set_flex_grow(g_selector_list, 1);
        lv_obj_set_style_pad_all(g_selector_list, ui_scale_min(8), 0);
        lv_obj_set_style_pad_gap(g_selector_list, ui_scale_min(8), 0);
        lv_obj_set_style_bg_opa(g_selector_list, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(g_selector_list, 0, 0);
        /* 选项较多时允许列表内部滚动 */
        lv_obj_add_flag(g_selector_list, LV_OBJ_FLAG_SCROLLABLE);
    }

    if (ui_is_wide_layout()) {
        lv_obj_set_flex_flow(g_selector_list, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(g_selector_list, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    } else {
        lv_obj_set_flex_flow(g_selector_list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(g_selector_list, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    }

    for (int i = 0; i < count; i++) {
        lv_obj_t *row = lv_button_create(g_selector_list);
        if (ui_is_wide_layout()) {
            lv_obj_set_size(row, lv_pct(48), ui_scale_min(64));
        } else {
            lv_obj_set_size(row, lv_pct(100), ui_scale_min(56));
        }
        lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_PANEL), 0);
        lv_obj_set_style_radius(row, ui_scale_min(10), 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        if (sel && i == *sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_ACCENT), 0);
        }

        lv_obj_t *l = lv_label_create(row);
        lv_label_set_text(l, opts[i]);
        lv_obj_set_style_text_font(l, ui_font_cjk(), 0);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_obj_center(l);

        lv_obj_add_event_cb(row, selector_item_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    lv_obj_remove_flag(g_selector_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_selector_overlay);
}

/* 参数行点击回调 */
static void on_exp_click(lv_event_t *e)
{
    (void)e;
    home_selector_overlay_open(tr("曝光", "Exposure"), g_exp_opts,
                               sizeof(g_exp_opts)/sizeof(g_exp_opts[0]),
                               &g_sel_exp, on_param_changed);
}

static void on_wb_click(lv_event_t *e)
{
    (void)e;
    home_selector_overlay_open(tr("白平衡", "White Balance"), WB_OPTS,
                               sizeof(g_wb_opts)/sizeof(g_wb_opts[0]),
                               &g_sel_wb, on_param_changed);
}

static void on_fmt_click(lv_event_t *e)
{
    (void)e;
    home_selector_overlay_open(tr("存储格式", "Format"), g_fmt_opts,
                               sizeof(g_fmt_opts)/sizeof(g_fmt_opts[0]),
                               &g_sel_fmt, on_param_changed);
}

static void on_color_click(lv_event_t *e)
{
    (void)e;
    home_selector_overlay_open(tr("色彩选择", "Color"), COLOR_OPTS,
                               sizeof(g_color_opts)/sizeof(g_color_opts[0]),
                               &g_sel_color, on_param_changed);
}

static void on_tone_click(lv_event_t *e)
{
    (void)e;
    home_selector_overlay_open(tr("影调", "Tone"), TONE_OPTS,
                               sizeof(g_tone_opts)/sizeof(g_tone_opts[0]),
                               &g_sel_tone, on_param_changed);
}

static void on_adj_click(lv_event_t *e)
{
    (void)e;
    home_selector_overlay_open(tr("图像调节", "Adjust"), ADJ_OPTS,
                               sizeof(g_adj_opts)/sizeof(g_adj_opts[0]),
                               &g_sel_adj, on_param_changed);
}

static void on_vstab_scene_click(lv_event_t *e)
{
    (void)e;
    home_selector_overlay_open(tr("增稳场景", "Stab Scene"), VSTAB_SCENE_OPTS,
                               sizeof(g_vstab_scene_opts)/sizeof(g_vstab_scene_opts[0]),
                               &g_sel_vstab_scene, on_param_changed);
}

static void on_ratio_click(lv_event_t *e)
{
    (void)e;
    home_selector_overlay_open(tr("画幅", "Aspect Ratio"), g_ratio_opts,
                               sizeof(g_ratio_opts)/sizeof(g_ratio_opts[0]),
                               &g_sel_ratio, on_param_changed);
}

static void on_stab_click(lv_event_t *e)
{
    (void)e;
    home_selector_overlay_open(tr("增稳模式", "Stabilization"), STAB_OPTS,
                               sizeof(g_stab_opts)/sizeof(g_stab_opts[0]),
                               &g_sel_stab, on_param_changed);
}

static void on_res_click(lv_event_t *e)
{
    (void)e;
    home_selector_overlay_open(tr("录制分辨率", "Resolution"), g_res_opts,
                               sizeof(g_res_opts)/sizeof(g_res_opts[0]),
                               &g_sel_res, on_param_changed);
}

/* 帧率变更专用回调：在通用参数变更处理基础上，重启摄像头重新协商帧率。
 * 录像中不重启（避免中断录制），仅记录新帧率供下次启动时生效 */
static void on_fps_changed(void)
{
    on_param_changed();
    if (g_camera && !g_recording) {
        LV_LOG_USER("帧率变更，重启摄像头重新协商格式");
        home_camera_restart();
    }
}

/* 视频模式帧率选择：帧率变更后需重启摄像头重新协商格式 */
static void on_fps_click(lv_event_t *e)
{
    (void)e;
    home_selector_overlay_open(tr("帧率", "Frame Rate"), g_fps_opts,
                               sizeof(g_fps_opts)/sizeof(g_fps_opts[0]),
                               &g_sel_fps, on_fps_changed);
}

/* 拍照模式扩展参数回调 */
static void on_iso_click(lv_event_t *e)
{
    (void)e;
    home_selector_overlay_open(tr("ISO 感光度", "ISO"), ISO_OPTS,
                               sizeof(g_iso_opts)/sizeof(g_iso_opts[0]),
                               &g_sel_iso, on_param_changed);
}

static void on_metering_click(lv_event_t *e)
{
    (void)e;
    home_selector_overlay_open(tr("测光模式", "Metering"), METERING_OPTS,
                               sizeof(g_metering_opts)/sizeof(g_metering_opts[0]),
                               &g_sel_metering, on_param_changed);
}

static void on_timer_click(lv_event_t *e)
{
    (void)e;
    home_selector_overlay_open(tr("拍照倒计时", "Timer"), TIMER_OPTS,
                               sizeof(g_timer_opts)/sizeof(g_timer_opts[0]),
                               &g_sel_timer, on_param_changed);
}

/* 图片拍摄大小选择：影响拍照输出像素总量 */
static void on_photosize_click(lv_event_t *e)
{
    (void)e;
    home_selector_overlay_open(tr("图片大小", "Photo Size"), PHOTOSIZE_OPTS,
                               sizeof(g_photosize_opts)/sizeof(g_photosize_opts[0]),
                               &g_sel_photosize, on_param_changed);
}

/* 连拍参数选择：0=单张拍摄，1/2/3=不同连拍速度 */
static void on_burst_click(lv_event_t *e)
{
    (void)e;
    home_selector_overlay_open(tr("连拍参数", "Burst"), BURST_OPTS,
                               sizeof(g_burst_opts)/sizeof(g_burst_opts[0]),
                               &g_sel_burst, on_param_changed);
}

/* 视频模式扩展参数回调 */
static void on_mic_click(lv_event_t *e)
{
    (void)e;
    home_selector_overlay_open(tr("麦克风", "Microphone"), SWITCH_OPTS,
                               sizeof(g_switch_opts)/sizeof(g_switch_opts[0]),
                               &g_sel_mic, on_param_changed);
}

static void on_grid_click(lv_event_t *e)
{
    (void)e;
    home_selector_overlay_open(tr("网格线", "Grid Lines"), SWITCH_OPTS,
                               sizeof(g_switch_opts)/sizeof(g_switch_opts[0]),
                               &g_sel_grid, on_param_changed);
}

static void on_distortion_click(lv_event_t *e)
{
    (void)e;
    home_selector_overlay_open(tr("畸变矫正", "Distortion Correction"), SWITCH_OPTS,
                               sizeof(g_switch_opts)/sizeof(g_switch_opts[0]),
                               &g_sel_distortion, on_param_changed);
}

/* 创建参数行 */
static lv_obj_t *make_param_row(lv_obj_t *parent, const char *title,
                                const char *value, lv_event_cb_t cb)
{
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_size(row, lv_pct(100), ui_scale_min(60));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_radius(row, ui_scale_min(10), 0);
    lv_obj_set_style_pad_all(row, ui_scale_min(12), 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tl = lv_label_create(row);
    lv_label_set_text(tl, title);
    lv_obj_set_style_text_font(tl, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(tl, lv_color_white(), 0);

    lv_obj_t *vl = lv_label_create(row);
    lv_label_set_text(vl, value);
    lv_obj_set_style_text_font(vl, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(vl, lv_color_hex(COLOR_ACCENT), 0);

    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);
    return row;
}

static void home_param_overlay_refresh(void)
{
    if (!g_param_overlay) return;

    /* 防御：g_func_mode 越界时回退到拍照模式，避免参数菜单异常 */
    if (g_func_mode < 0 || g_func_mode >= (int)MODE_COUNT) {
        LV_LOG_WARN("参数菜单刷新时 g_func_mode 越界 (%d)，回退到拍照模式", g_func_mode);
        g_func_mode = 0;
    }
    LV_LOG_USER("参数菜单刷新: mode=%d (%s)", g_func_mode, mode_label(g_func_mode));

    /* overlay 的第 1 个子对象是右侧面板 */
    lv_obj_t *panel = lv_obj_get_child(g_param_overlay, 0);
    if (!panel) return;
    /* 面板的第 2 个子对象是内容区 */
    lv_obj_t *body = lv_obj_get_child(panel, 1);
    if (!body) return;

    lv_obj_clean(body);

    /* 模式切换按钮 */
    lv_obj_t *mode_row = lv_button_create(body);
    lv_obj_set_size(mode_row, lv_pct(100), ui_scale_min(48));
    lv_obj_set_flex_flow(mode_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mode_row, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(mode_row, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(mode_row, ui_scale_min(8), 0);
    lv_obj_set_style_border_width(mode_row, 0, 0);
    lv_obj_set_style_pad_all(mode_row, ui_scale_min(4), 0);
    lv_obj_remove_flag(mode_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(mode_row, on_param_mode_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *auto_lbl = lv_label_create(mode_row);
    lv_label_set_text(auto_lbl, g_param_mode == 0 ? tr("[自动]", "[Auto]") : tr("自动", "Auto"));
    lv_obj_set_style_text_font(auto_lbl, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(auto_lbl, g_param_mode == 0 ? lv_color_hex(COLOR_ACCENT) : lv_color_white(), 0);

    lv_obj_t *manual_lbl = lv_label_create(mode_row);
    lv_label_set_text(manual_lbl, g_param_mode == 1 ? tr("[手动]", "[Manual]") : tr("手动", "Manual"));
    lv_obj_set_style_text_font(manual_lbl, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(manual_lbl, g_param_mode == 1 ? lv_color_hex(COLOR_ACCENT) : lv_color_white(), 0);

    /* 判断当前模式应显示拍照参数还是视频参数。
     * 拍照/夜景/延时：显示画幅、图片大小、连拍等拍照参数。
     * 视频/慢动作/跟随：显示录制分辨率、帧率等视频参数。
     * 此判断必须与主页底部参数、快门行为保持一致，否则用户会感到困惑 */
    int is_photo_mode = (g_func_mode == 0 || g_func_mode == 2 || g_func_mode == 5);

    if (is_photo_mode) {
        /* 拍照相关模式参数。
         * 顶部三项为拍摄规格：画幅（含尺寸等级）/图片大小/连拍参数，
         * 对应录像模式的 分辨率/帧率 结构，符合用户从上到下的操作直觉。
         * 画幅显示为 "M (16:9)" 格式：M=尺寸等级，16:9=画幅比例 */
        make_param_row(body, tr("画幅", "Aspect Ratio"), g_ratio_opts[g_sel_ratio], on_ratio_click);
        make_param_row(body, tr("图片大小", "Photo Size"), PHOTOSIZE_OPTS[g_sel_photosize], on_photosize_click);
        make_param_row(body, tr("连拍参数", "Burst"), BURST_OPTS[g_sel_burst], on_burst_click);
        make_param_row(body, tr("曝光", "Exposure"), g_exp_opts[g_sel_exp], on_exp_click);
        make_param_row(body, tr("白平衡", "White Balance"), WB_OPTS[g_sel_wb], on_wb_click);
        make_param_row(body, tr("ISO 感光度", "ISO"), ISO_OPTS[g_sel_iso], on_iso_click);
        make_param_row(body, tr("测光模式", "Metering"), METERING_OPTS[g_sel_metering], on_metering_click);
        make_param_row(body, tr("存储格式", "Format"), g_fmt_opts[g_sel_fmt], on_fmt_click);
        make_param_row(body, tr("拍照倒计时", "Timer"), TIMER_OPTS[g_sel_timer], on_timer_click);
        make_param_row(body, tr("网格线", "Grid Lines"), SWITCH_OPTS[g_sel_grid], on_grid_click);
    } else {
        /* 视频/慢动作模式参数。
         * 顶部两项为录制规格：分辨率+帧率，紧跟拍照模式的画幅+连拍结构，
         * 保持两种模式顶部参数的视觉一致性 */
        make_param_row(body, tr("录制分辨率", "Resolution"), g_res_opts[g_sel_res], on_res_click);
        make_param_row(body, tr("帧率", "Frame Rate"), g_fps_opts[g_sel_fps], on_fps_click);
        make_param_row(body, tr("增稳模式", "Stabilization"), STAB_OPTS[g_sel_stab], on_stab_click);
        make_param_row(body, tr("麦克风", "Microphone"), SWITCH_OPTS[g_sel_mic], on_mic_click);
        make_param_row(body, tr("色彩选择", "Color"), COLOR_OPTS[g_sel_color], on_color_click);
        make_param_row(body, tr("影调", "Tone"), TONE_OPTS[g_sel_tone], on_tone_click);
        make_param_row(body, tr("图像调节", "Adjust"), ADJ_OPTS[g_sel_adj], on_adj_click);
        make_param_row(body, tr("增稳场景", "Stab Scene"), VSTAB_SCENE_OPTS[g_sel_vstab_scene], on_vstab_scene_click);
        make_param_row(body, tr("网格线", "Grid Lines"), SWITCH_OPTS[g_sel_grid], on_grid_click);
        make_param_row(body, tr("畸变矫正", "Distortion Correction"), SWITCH_OPTS[g_sel_distortion], on_distortion_click);
    }
}

static void make_param_overlay(void)
{
    g_param_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_param_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(g_param_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_param_overlay, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(g_param_overlay, 0, 0);
    lv_obj_set_style_border_width(g_param_overlay, 0, 0);
    lv_obj_remove_flag(g_param_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_param_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_param_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_param_overlay, param_overlay_bg_click, LV_EVENT_CLICKED, NULL);
    /* 左滑（LV_DIR_LEFT）关闭 overlay，与点击背景关闭协同 */
    ui_attach_back_gesture(g_param_overlay, param_overlay_bg_click);

    /* 参数面板：横竖屏自适应。
     * 竖屏：右侧滑出（82%宽 × 100%高），符合单手操作习惯。
     * 横屏：底部弹出（限宽 720px 居中 × 70%高），避免横屏右侧面板过窄挤压参数行 */
    lv_obj_t *panel = lv_obj_create(g_param_overlay);
    lv_obj_set_style_bg_color(panel, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    if (ui_is_wide_layout()) {
        lv_obj_set_width(panel, ui_get_content_max_width());
        lv_obj_set_height(panel, lv_pct(70));
        lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_radius(panel, ui_scale_min(12), 0);
    } else {
        lv_obj_set_size(panel, lv_pct(82), lv_pct(100));
        lv_obj_align(panel, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_radius(panel, 0, 0);
    }

    /* 顶部栏 */
    lv_obj_t *header = lv_obj_create(panel);
    lv_obj_set_size(header, lv_pct(100), ui_scale_min(48));
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x222222), 0);
    lv_obj_set_style_pad_all(header, ui_scale_min(8), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_button_create(header);
    lv_obj_set_size(back, ui_scale_min(40), ui_scale_min(40));
    lv_obj_set_style_bg_color(back, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_remove_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(bl, ui_font_sym(), 0);
    lv_obj_set_style_text_color(bl, lv_color_white(), 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, param_overlay_back, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, tr("拍摄参数", "Capture Params"));
    lv_obj_set_style_text_font(title, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    lv_obj_t *placeholder = lv_obj_create(header);
    lv_obj_set_size(placeholder, ui_scale_min(40), ui_scale_min(40));
    lv_obj_set_style_bg_opa(placeholder, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(placeholder, 0, 0);

    /* 内容区占位：参数行较多，竖屏下需要滚动。
     * 横屏底部弹窗高度 70%，内容超出时 body 内部滚动 */
    lv_obj_t *body = lv_obj_create(panel);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, ui_scale_min(10), 0);
    lv_obj_set_style_pad_gap(body, ui_scale_min(8), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    /* body 需要可滚动，避免参数列表超出面板 */
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
}

static void on_param_settings_click(lv_event_t *e)
{
    (void)e;
    LV_LOG_USER("Home: param settings button clicked, mode=%d, open param overlay", g_func_mode);
    if (!g_param_overlay) make_param_overlay();
    home_param_overlay_refresh();
    lv_obj_remove_flag(g_param_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_param_overlay);
}

/* ================================================================== */
/* 二级选择器弹层创建                                                  */
/* ================================================================== */
static void make_selector_overlay(void)
{
    g_selector_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_selector_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(g_selector_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_selector_overlay, LV_OPA_40, 0);
    lv_obj_set_style_pad_all(g_selector_overlay, 0, 0);
    lv_obj_set_style_border_width(g_selector_overlay, 0, 0);
    lv_obj_remove_flag(g_selector_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_selector_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_selector_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_selector_overlay, selector_overlay_bg_click, LV_EVENT_CLICKED, NULL);
    /* 左滑（LV_DIR_LEFT）关闭 overlay */
    ui_attach_back_gesture(g_selector_overlay, selector_overlay_bg_click);

    /* 选择器面板：横竖屏自适应，与参数 overlay 保持一致。
     * 竖屏：右侧滑出（82%宽 × 100%高）。
     * 横屏：底部弹出（限宽 720px 居中 × 80%高），选项列表可双列 wrap */
    lv_obj_t *panel = lv_obj_create(g_selector_overlay);
    lv_obj_set_style_bg_color(panel, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    if (ui_is_wide_layout()) {
        lv_obj_set_width(panel, ui_get_content_max_width());
        lv_obj_set_height(panel, lv_pct(80));
        lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_radius(panel, ui_scale_min(12), 0);
    } else {
        lv_obj_set_size(panel, lv_pct(82), lv_pct(100));
        lv_obj_align(panel, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_radius(panel, 0, 0);
    }

    /* 顶部栏 */
    lv_obj_t *header = lv_obj_create(panel);
    lv_obj_set_size(header, lv_pct(100), ui_scale_min(48));
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x222222), 0);
    lv_obj_set_style_pad_all(header, ui_scale_min(8), 0);
    lv_obj_set_style_pad_gap(header, ui_scale_min(8), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_button_create(header);
    lv_obj_set_size(back, ui_scale_min(40), ui_scale_min(40));
    lv_obj_set_style_bg_color(back, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_remove_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(bl, ui_font_sym(), 0);
    lv_obj_set_style_text_color(bl, lv_color_white(), 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, selector_overlay_bg_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, tr("选择", "Select"));
    lv_obj_set_style_text_font(title, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
}

/* ================================================================== */
/* 拍摄模式切换弹层                                                    */
/* ================================================================== */
static void home_mode_overlay_close(void)
{
    // #region debug-point C:mode-overlay
    dbg_log("C", "page_home.c:home_mode_overlay_close", "start");
    // #endregion
    if (!g_mode_overlay) {
        dbg_log("C", "page_home.c:home_mode_overlay_close", "no overlay");
        return;
    }
    if (app_ui_is_recreating()) return;
    if (!lv_obj_is_valid(g_mode_overlay)) {
        g_mode_overlay = NULL;
        return;
    }

    /* 关闭前刷新一次选中项，确保 g_mode_sel_idx 反映当前居中项。
     * 用户可能在 snap 动画未完成时点击确定/右滑，此时需重新计算 best。 */
    if (g_mode_list && !lv_obj_has_flag(g_mode_overlay, LV_OBJ_FLAG_HIDDEN)) {
        dbg_log("C", "page_home.c:home_mode_overlay_close", "animate before close");
        mode_list_animate_to_center();
    }

    /* 关闭时应用列表当前居中选中项；若 overlay 已被隐藏/销毁则不再操作 */
    if (!g_mode_overlay || lv_obj_has_flag(g_mode_overlay, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    int idx = g_mode_sel_idx;
    if (idx < 0) idx = 0;
    if (idx >= (int)MODE_COUNT) idx = (int)MODE_COUNT - 1;
    {
        char dbg_buf[64];
        snprintf(dbg_buf, sizeof(dbg_buf), "selected idx=%d cur=%d", idx, g_func_mode);
        dbg_log("C", "page_home.c:home_mode_overlay_close", dbg_buf);
    }
    if (idx >= 0 && idx < (int)MODE_COUNT) {
        if (idx != g_func_mode) {
            int old_mode = g_func_mode;
            g_func_mode = idx;
            /* 保存旧模式参数，加载新模式参数，实现各模式参数独立记忆 */
            mode_params_save(old_mode);
            mode_params_load(g_func_mode);
            home_refresh_mode_icon();
            home_refresh_bottom_params();
            home_refresh_rec_ui();
            home_refresh_grid();
            /* 模式切换后刷新裁剪引导：拍照类模式显示，视频/慢动作隐藏 */
            home_update_crop_guide();
            /* 夜景模式：自动切换到手动并设置高 ISO 预设
             * 慢动作模式：需重启摄像头协商高帧率（60fps）
             * 其他模式：应用当前参数到摄像头 */
            if (g_func_mode == 2) {
                /* 夜景模式：强制手动 + 高 ISO（800）+ 中央测光 */
                g_param_mode = 1;
                g_sel_iso = 4;       /* ISO 800 */
                g_sel_metering = 1;  /* 中央重点测光 */
                mode_params_save(g_func_mode);
            }
            /* 慢动作/视频模式需重启摄像头重新协商帧率 */
            if (g_func_mode == 1 || g_func_mode == 4) {
                home_camera_restart();
            } else {
                home_apply_camera_controls();
            }
            LV_LOG_USER("Mode applied: %d (%s)", idx, mode_label(idx));
        } else {
            LV_LOG_USER("Mode unchanged: %d (%s)", idx, mode_label(idx));
        }
        /* 无论模式是否变化，参数面板若处于打开状态都刷新其显示。
         * 防止模式切换后参数面板未及时更新 */
        home_param_overlay_refresh();
        /* 模式切换后刷新上滑菜单：拍照/夜景/延时显示画幅、图片大小、连拍；
         * 视频/慢动作/跟随显示分辨率、帧率。未切换时刷新也无副作用。 */
        page_quick_refresh();
    }
    lv_obj_add_flag(g_mode_overlay, LV_OBJ_FLAG_HIDDEN);
    LV_LOG_USER("Mode overlay closed (return to home)");
}

static void mode_overlay_close_cb(lv_event_t *e)
{
    (void)e;
    home_mode_overlay_close();
}

/* 根据每个 item 到列表可视区中心的距离，实时计算并设置：
 * - 图标颜色：只有最接近中心的 best 项为 COLOR_ACCENT（黄），其他项为灰色
 *   （使用阈值切换，避免平滑插值导致两个相邻项都呈黄色，造成"两个选中项"视觉混淆）
 * - 图标/文字透明度：中心处 255(不透明)，远离中心渐隐到约 160，增强纵深感。
 * - 图标缩放：居中选中项放大到 2x，远离中心缩小到 ~0.9x，按距离线性过渡。
 *   缩放直接通过样式设置，不启用 transition/animation，避免切换动画引发崩溃。 */
/* 记录上一次各 item 的样式值，避免 scroll 事件高频触发时重复设置相同样式，
 * 减少对 LVGL 样式/动画系统的压力。 */
typedef struct {
    int32_t opa;
    int32_t scale;
    lv_color_t color;
} mode_item_style_t;
static mode_item_style_t s_last_icon_style[MODE_ICON_MAX];
static mode_item_style_t s_last_text_style[MODE_ICON_MAX];

/* 模式项图标缩放：居中选中项最大(2x=512)，随距离线性缩小到远端(0.9x=230)。
 * 直接通过样式设置，不使用 lv_anim/transition，避免切换动画带来的重入/崩溃风险。 */
#define MODE_ICON_SCALE_SELECTED  512  /* 2.0x */
#define MODE_ICON_SCALE_FAR       230  /* ~0.9x */
#define MODE_ICON_SCALE_PIVOT_PCT 50

static void mode_list_animate_to_center(void)
{
    if (!g_mode_list) return;
    /* 防止在滚动事件处理中因修改样式触发再次布局/滚动，导致递归调用栈溢出或死机 */
    static int s_in_animate = 0;
    if (s_in_animate) return;
    s_in_animate = 1;

    // #region debug-point C:mode-anim
    dbg_log("C", "page_home.c:mode_list_animate_to_center", "start");
    // #endregion

    /* UI 重建窗口期禁止访问即将被销毁的旧对象 */
    if (app_ui_is_recreating()) {
        s_in_animate = 0;
        return;
    }

    /* 检查列表是否仍属于当前有效的 mode overlay，避免 UI 重建/销毁过程中访问已释放对象 */
    if (!g_mode_overlay || lv_obj_has_flag(g_mode_overlay, LV_OBJ_FLAG_HIDDEN)) {
        s_in_animate = 0;
        return;
    }

    /* 额外防御：列表对象是否仍有效（非 NULL 且未被删除） */
    if (!lv_obj_is_valid(g_mode_list)) {
        s_in_animate = 0;
        return;
    }

    int32_t view_mid;
    if (g_app_landscape) {
        view_mid = lv_obj_get_scroll_x(g_mode_list) + lv_obj_get_content_width(g_mode_list) / 2;
    } else {
        view_mid = lv_obj_get_scroll_y(g_mode_list) + lv_obj_get_content_height(g_mode_list) / 2;
    }

    /* 第一遍：遍历所有模式项（通过 g_mode_items 数组，跳过首尾占位），
     * 计算每项中心到可视区中心的距离，找出最近项 best。 */
    int best = 0;
    int32_t best_dist = INT32_MAX;
    int32_t item_main = ui_scale_min(g_app_landscape ? 220 : 160);
    if (item_main <= 0) item_main = 1;

    for (uint32_t i = 0; i < MODE_COUNT && i < MODE_ICON_MAX; i++) {
        lv_obj_t *item = g_mode_items[i];
        if (!item || !lv_obj_is_valid(item)) continue;

        int32_t item_center = g_app_landscape
                            ? lv_obj_get_x(item) + lv_obj_get_width(item) / 2
                            : lv_obj_get_y(item) + lv_obj_get_height(item) / 2;
        int32_t dist = item_center - view_mid;
        if (dist < 0) dist = -dist;
        if (dist < best_dist) {
            best_dist = dist;
            best = (int)i;
            int32_t im = g_app_landscape ? lv_obj_get_width(item) : lv_obj_get_height(item);
            if (im > 0) item_main = im;
        }
    }

    /* 第二遍：设置颜色（阈值切换）和透明度（按距离平滑淡出）。
     * 颜色只有 best 项为 accent，确保唯一选中态；透明度中心 255、远端 160。
     * 通过脏标记跳过未变化的样式设置，降低高频 scroll 事件对 LVGL 的压力。 */
    const lv_color_t accent = lv_color_hex(COLOR_ACCENT);
    const lv_color_t gray   = lv_color_hex(0x888888);

    for (uint32_t i = 0; i < MODE_COUNT && i < MODE_ICON_MAX; i++) {
        lv_obj_t *item = g_mode_items[i];
        if (!item || !lv_obj_is_valid(item)) continue;

        int32_t item_center = g_app_landscape
                            ? lv_obj_get_x(item) + lv_obj_get_width(item) / 2
                            : lv_obj_get_y(item) + lv_obj_get_height(item) / 2;
        int32_t dist = item_center - view_mid;
        if (dist < 0) dist = -dist;

        int32_t im = g_app_landscape ? lv_obj_get_width(item) : lv_obj_get_height(item);
        if (im <= 0) im = item_main;
        int32_t ratio256 = 256 - (dist * 256 / im);
        if (ratio256 < 0) ratio256 = 0;
        if (ratio256 > 256) ratio256 = 256;

        /* 图标/文字透明度：160 ~ 255，远端自然淡出 */
        int32_t opa = 160 + (ratio256 * 95 / 256);
        if (opa > 255) opa = 255;
        /* 颜色：只有 best 项为 accent，其他项为 gray，避免双黄 */
        lv_color_t c = ((int)i == best) ? accent : gray;
        /* 图标缩放：居中选中项 2x，远端 ~0.9x，按距离线性过渡。
         * 直接设置样式，无 transition/animation，避免切换动画引发崩溃。 */
        int32_t scale = MODE_ICON_SCALE_FAR + (MODE_ICON_SCALE_SELECTED - MODE_ICON_SCALE_FAR) * ratio256 / 256;
        if (scale < MODE_ICON_SCALE_FAR) scale = MODE_ICON_SCALE_FAR;
        if (scale > MODE_ICON_SCALE_SELECTED) scale = MODE_ICON_SCALE_SELECTED;

        if (g_mode_icons[i] && lv_obj_is_valid(g_mode_icons[i])) {
            mode_item_style_t *last = &s_last_icon_style[i];
            if (!lv_color_eq(last->color, c)) {
                lv_obj_set_style_text_color(g_mode_icons[i], c, 0);
                last->color = c;
            }
            if (last->opa != opa) {
                lv_obj_set_style_text_opa(g_mode_icons[i], opa, 0);
                last->opa = opa;
            }
            if (last->scale != scale) {
                lv_obj_set_style_transform_scale(g_mode_icons[i], scale, 0);
                lv_obj_set_style_transform_pivot_x(g_mode_icons[i], lv_pct(MODE_ICON_SCALE_PIVOT_PCT), LV_PART_MAIN);
                lv_obj_set_style_transform_pivot_y(g_mode_icons[i], lv_pct(MODE_ICON_SCALE_PIVOT_PCT), LV_PART_MAIN);
                last->scale = scale;
            }
        }
        if (g_mode_texts[i] && lv_obj_is_valid(g_mode_texts[i])) {
            mode_item_style_t *last = &s_last_text_style[i];
            /* 文字保持白色，但透明度随距离变化，增强层次感 */
            if (!lv_color_eq(last->color, lv_color_white())) {
                lv_obj_set_style_text_color(g_mode_texts[i], lv_color_white(), 0);
                last->color = lv_color_white();
            }
            if (last->opa != opa) {
                lv_obj_set_style_text_opa(g_mode_texts[i], opa, 0);
                last->opa = opa;
            }
        }
    }

    if (best != g_mode_sel_idx) {
        g_mode_sel_idx = best;
        LV_LOG_USER("Mode list scrolled: idx=%d (%s)", best,
                    best < (int)MODE_COUNT ? mode_label(best) : "?");
    }

    s_in_animate = 0;
}

/* 滚动事件：实时更新所有 item 的颜色/透明度动画 */
static void on_mode_list_scrolled(lv_event_t *e)
{
    (void)e;
    mode_list_animate_to_center();
}

/* 拍摄模式全屏页面：自定义滚动列表（图标+文字同时滚动选择）
 * - 透明背景（无选择框背景）
 * - 竖屏：垂直滚动；横屏：水平滚动
 * - 每个模式项内部横向排列：图标在左、文字在右（LV_FLEX_FLOW_ROW）
 * - 选中项图标放大一倍(2x)，路过项随距离平滑缩放/高亮
 * - 颜色阈值切换：只有居中项(best)为黄色，其余为灰色，避免"双黄"
 * 右滑(LV_DIR_RIGHT)或返回箭头退出回主页，关闭时应用选中项。
 * 布局：顶部 header / 中部滚动列表(flex_grow 填充) / 底部确定按钮 */
static void make_mode_overlay(void)
{
    /* 清空句柄数组与脏标记 */
    for (int i = 0; i < MODE_ICON_MAX; i++) {
        g_mode_icons[i] = NULL;
        g_mode_texts[i] = NULL;
        g_mode_items[i] = NULL;
        s_last_icon_style[i].opa   = -1;
        s_last_icon_style[i].scale = -1;
        s_last_icon_style[i].color = lv_color_hex(0);
        s_last_text_style[i].opa   = -1;
        s_last_text_style[i].scale = -1;
        s_last_text_style[i].color = lv_color_hex(0);
    }
    g_mode_spacer_start = NULL;
    g_mode_spacer_end   = NULL;

    g_mode_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_mode_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(g_mode_overlay, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(g_mode_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(g_mode_overlay, 0, 0);
    lv_obj_set_style_border_width(g_mode_overlay, 0, 0);
    lv_obj_set_style_radius(g_mode_overlay, 0, 0);
    lv_obj_remove_flag(g_mode_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_mode_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_flex_flow(g_mode_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_mode_overlay, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 右滑(LV_DIR_RIGHT)关闭：跟随系统手势，右滑退出回主页 */
    ui_attach_right_gesture(g_mode_overlay, mode_overlay_close_cb);

    /* 顶部栏：左上角返回箭头 + 标题 */
    lv_obj_t *header = lv_obj_create(g_mode_overlay);
    lv_obj_set_size(header, lv_pct(100), ui_scale_min(54));
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, ui_scale_min(8), 0);
    lv_obj_set_style_pad_gap(header, ui_scale_min(12), 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_button_create(header);
    lv_obj_set_size(back, ui_scale_min(44), ui_scale_min(44));
    lv_obj_set_style_bg_color(back, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_60, 0);
    lv_obj_set_style_radius(back, ui_scale_min(12), 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_remove_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(bl, ui_font_sym(), 0);
    lv_obj_set_style_text_color(bl, lv_color_white(), 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, mode_overlay_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, tr("拍摄模式", "Capture Mode"));
    lv_obj_set_style_text_font(title, ui_font_cjk_lg(), 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    /* 中部滚动列表：透明背景，flex_grow=1 填满 header 与底部按钮之间空间。
     * 竖屏=垂直滚动(column+snap_y)，横屏=水平滚动(row+snap_x)。
     * 每个 item = 图标(大)+文字，二者同时随列表滚动。
     * pad_gap 需足够大，避免图标放大到 2x 时渲染超出 item 边界遮挡相邻项。
     * 主轴使用 LV_FLEX_ALIGN_START（而非 CENTER），避免内容超出容器时
     * 首尾项坐标为负、无法滚动到可视区中心的问题；首尾居中由占位项实现。 */
    g_mode_list = lv_obj_create(g_mode_overlay);
    lv_obj_set_width(g_mode_list, lv_pct(100));
    lv_obj_set_flex_grow(g_mode_list, 1);
    lv_obj_set_flex_flow(g_mode_list, g_app_landscape ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_mode_list, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(g_mode_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_mode_list, 0, 0);
    lv_obj_set_style_radius(g_mode_list, 0, 0);
    lv_obj_set_style_pad_gap(g_mode_list, ui_scale_min(g_app_landscape ? 24 : 28), 0);
    /* snap 居中对齐 + 单项吸附：滚动停止时最近项自动对齐到可视区中心 */
    lv_obj_add_flag(g_mode_list, LV_OBJ_FLAG_SCROLL_ONE);
    if (g_app_landscape) {
        lv_obj_set_scroll_snap_x(g_mode_list, LV_SCROLL_SNAP_CENTER);
    } else {
        lv_obj_set_scroll_snap_y(g_mode_list, LV_SCROLL_SNAP_CENTER);
    }
    /* 列表保持 CLICKABLE+SCROLLABLE 以接收拖拽发起滚动；
     * 右滑关闭手势通过子对象默认的 GESTURE_BUBBLE 冒泡到 overlay。 */

    /* 构建各模式项：图标(左) + 文字(右) 横向排列（同时滚动）
     * item 内部使用 LV_FLEX_FLOW_ROW：图标在左、文字在右。
     * item 主轴尺寸需保持充足，避免图标/文字在滚动时相互遮挡。
     * 横屏(800x480)可视主轴尺寸更大，item 也相应放大。
     *
     * 重要：滚动过程中会高频调用 mode_list_animate_to_center() 更新每个 item 的
     * text_color / text_opa / transform_scale。为避免 transition/animation 系统
     * 在高频样式修改下产生内部状态异常，此处不设置 transition；滚动时直接设置目标
     * 样式值，依靠 16ms 刷新周期提供平滑视觉反馈，同时消除动画重入/堆叠风险。
     * 缩放仅作用于图标 label，并以中心为 pivot，避免文字缩放带来的渲染风险。 */
    int item_main = ui_scale_min(g_app_landscape ? 220 : 160);  /* 主轴尺寸 */
    int item_pad  = ui_scale_min(16);                          /* 内部 padding */
    int item_gap  = ui_scale_min(28);                          /* 图标与文字间距，避免拥挤 */

    /* 首部占位：透明、不可交互。主轴尺寸由 mode_overlay_scroll_to_current()
     * 动态设置为 (可视区主轴 - item_main) / 2，使首项也能滚动到列表中心。 */
    g_mode_spacer_start = lv_obj_create(g_mode_list);
    lv_obj_remove_style_all(g_mode_spacer_start);
    lv_obj_remove_flag(g_mode_spacer_start, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    if (g_app_landscape) {
        lv_obj_set_size(g_mode_spacer_start, 1, lv_pct(100));
    } else {
        lv_obj_set_size(g_mode_spacer_start, lv_pct(100), 1);
    }

    for (size_t i = 0; i < MODE_COUNT && i < MODE_ICON_MAX; i++) {
        lv_obj_t *item = lv_obj_create(g_mode_list);
        if (g_app_landscape) {
            lv_obj_set_size(item, item_main, lv_pct(100));
        } else {
            lv_obj_set_size(item, lv_pct(100), item_main);
        }
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(item, 0, 0);
        lv_obj_set_style_pad_all(item, item_pad, 0);
        lv_obj_set_style_pad_gap(item, item_gap, 0);
        lv_obj_set_style_radius(item, 0, 0);
        lv_obj_remove_flag(item, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        g_mode_items[i] = item;

        int is_default = ((int)i == (int)g_func_mode);

        /* 图标在左、文字在右（LV_FLEX_FLOW_ROW），横向水平排列。
         * 不使用 transform_scale，避免 SW 渲染器在真机对文字矩阵缩放时越界崩溃。
         * 默认选中项为黄色不透明，非选中项为灰色半透明。 */
        lv_obj_t *ic = lv_label_create(item);
        lv_label_set_text(ic, g_modes[i].icon);
        lv_obj_set_style_text_font(ic, &lv_font_montserrat_48, 0);
        lv_obj_set_style_text_color(ic, is_default ? lv_color_hex(COLOR_ACCENT) : lv_color_hex(0x888888), 0);
        lv_obj_set_style_text_opa(ic, is_default ? LV_OPA_COVER : 160, 0);
        g_mode_icons[i] = ic;

        lv_obj_t *tx = lv_label_create(item);
        lv_label_set_text(tx, mode_label((int)i));
        lv_obj_set_style_text_font(tx, ui_font_cjk_lg(), 0);
        /* 文字保持白色；透明度随距离变化，与图标同步 */
        lv_obj_set_style_text_color(tx, lv_color_white(), 0);
        lv_obj_set_style_text_opa(tx, is_default ? LV_OPA_COVER : 160, 0);
        g_mode_texts[i] = tx;
    }

    /* 尾部占位：与首部占位对称，使末项也能滚动到列表中心。 */
    g_mode_spacer_end = lv_obj_create(g_mode_list);
    lv_obj_remove_style_all(g_mode_spacer_end);
    lv_obj_remove_flag(g_mode_spacer_end, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    if (g_app_landscape) {
        lv_obj_set_size(g_mode_spacer_end, 1, lv_pct(100));
    } else {
        lv_obj_set_size(g_mode_spacer_end, lv_pct(100), 1);
    }

    /* 滚动事件：实时更新所有 item 的缩放/颜色动画 */
    lv_obj_add_event_cb(g_mode_list, on_mode_list_scrolled, LV_EVENT_SCROLL, NULL);

    /* 底部确认按钮（等价于右滑/返回：应用居中项并关闭） */
    lv_obj_t *ok = lv_button_create(g_mode_overlay);
    lv_obj_set_size(ok, ui_scale_min(180), ui_scale_min(56));
    lv_obj_set_style_bg_color(ok, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_radius(ok, ui_scale_min(14), 0);
    lv_obj_set_style_border_width(ok, 0, 0);
    lv_obj_set_style_pad_all(ok, ui_scale_min(8), 0);
    lv_obj_remove_flag(ok, LV_OBJ_FLAG_SCROLLABLE);
    ui_attach_press_scale(ok, 242);
    lv_obj_t *ok_lbl = lv_label_create(ok);
    lv_label_set_text(ok_lbl, tr("确定", "OK"));
    lv_obj_set_style_text_font(ok_lbl, ui_font_cjk_lg(), 0);
    lv_obj_set_style_text_color(ok_lbl, lv_color_black(), 0);
    lv_obj_center(ok_lbl);
    lv_obj_add_event_cb(ok, mode_overlay_close_cb, LV_EVENT_CLICKED, NULL);
}

/* 打开模式弹层时：布局完成后滚动到当前模式使其居中 */
static void mode_overlay_scroll_to_current(void)
{
    if (!g_mode_list || !g_mode_overlay) return;
    if (lv_obj_has_flag(g_mode_overlay, LV_OBJ_FLAG_HIDDEN)) return;
    if (!lv_obj_is_valid(g_mode_list) || !lv_obj_is_valid(g_mode_overlay)) return;
    if (app_ui_is_recreating()) return;

    lv_obj_update_layout(g_mode_list);

    if ((int)g_func_mode < 0 || (int)g_func_mode >= (int)MODE_COUNT) g_func_mode = 0;
    lv_obj_t *cur = (g_func_mode < MODE_ICON_MAX) ? g_mode_items[g_func_mode] : NULL;
    if (!cur) return;

    /* 动态设置首尾占位尺寸 = (可视区主轴 - 单项主轴) / 2，
     * 使首/末项也能滚动到列表可视区中心。占位尺寸依赖列表实际尺寸，
     * 故必须在此处（overlay 已显示、布局已就绪）设置。 */
    int32_t content_main = g_app_landscape
        ? lv_obj_get_content_width(g_mode_list)
        : lv_obj_get_content_height(g_mode_list);
    int32_t cur_main = g_app_landscape
        ? lv_obj_get_width(cur)
        : lv_obj_get_height(cur);
    int32_t spacer = (content_main - cur_main) / 2;
    if (spacer < 0) spacer = 0;

    if (g_mode_spacer_start) {
        if (g_app_landscape) lv_obj_set_width(g_mode_spacer_start, spacer);
        else                  lv_obj_set_height(g_mode_spacer_start, spacer);
    }
    if (g_mode_spacer_end) {
        if (g_app_landscape) lv_obj_set_width(g_mode_spacer_end, spacer);
        else                  lv_obj_set_height(g_mode_spacer_end, spacer);
    }
    /* 占位尺寸改变后需重新布局，否则后续坐标读取会过期 */
    lv_obj_update_layout(g_mode_list);

    /* 居中当前项：target = child_center - content_center，再 clamp 到 [0, max]。
     * lv_obj_get_y/x 返回未滚动的布局坐标（相对父内容区），
     * 可见窗口中心(布局坐标) = scroll + content_size/2。
     * 由于首尾占位已使所有项都能居中，target 不会为负。 */
    if (g_app_landscape) {
        int32_t content_w = lv_obj_get_content_width(g_mode_list);
        int32_t child_cx = lv_obj_get_x(cur) + lv_obj_get_width(cur) / 2;
        int32_t target = child_cx - content_w / 2;
        if (target < 0) target = 0;
        lv_obj_scroll_to_x(g_mode_list, target, LV_ANIM_OFF);
    } else {
        int32_t content_h = lv_obj_get_content_height(g_mode_list);
        int32_t child_cy = lv_obj_get_y(cur) + lv_obj_get_height(cur) / 2;
        int32_t target = child_cy - content_h / 2;
        if (target < 0) target = 0;
        lv_obj_scroll_to_y(g_mode_list, target, LV_ANIM_OFF);
    }

    g_mode_sel_idx = g_func_mode;
    mode_list_animate_to_center();
}

static void on_mode_icon_click(lv_event_t *e)
{
    (void)e;
    LV_LOG_USER("Home: mode icon clicked, open mode overlay");
    if (!g_mode_overlay) make_mode_overlay();
    lv_obj_remove_flag(g_mode_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_mode_overlay);
    ui_fade_in_overlay(g_mode_overlay, 180);
    /* 显示后再滚动到当前项（此时布局已就绪） */
    mode_overlay_scroll_to_current();
}

/* ================================================================== */
/* 变焦弹层                                                            */
/* ================================================================== */
static void home_zoom_overlay_close(void)
{
    if (g_zoom_overlay) {
        lv_obj_add_flag(g_zoom_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

static void zoom_overlay_bg_click(lv_event_t *e)
{
    (void)e;
    home_zoom_overlay_close();
}

static void zoom_set_value(float v)
{
    if (v < 1.0f) v = 1.0f;
    if (v > 10.0f) v = 10.0f;
    g_zoom = v;
    home_refresh_zoom_btn();

    /* 变焦应用到摄像头 V4L2 驱动（V4L2_CID_ZOOM_ABSOLUTE）
     * zoom_val 单位 0.1x：1.0x=100, 2.0x=200, 4.0x=400
     * 驱动不支持时 camera_set_controls 内部记录警告，不影响其他功能 */
    if (g_camera) {
        cam_control_t ctrl;
        memset(&ctrl, 0, sizeof(ctrl));
        ctrl.has_zoom = 1;
        ctrl.zoom_val = (int)(v * 100.0f);
        camera_set_controls(g_camera, &ctrl);
    }
}

static void on_zoom_arc_changed(lv_event_t *e)
{
    lv_obj_t *arc = lv_event_get_target(e);
    int v = lv_arc_get_value(arc);
    zoom_set_value(v / 10.0f);

    lv_obj_t *val_lbl = (lv_obj_t *)lv_event_get_user_data(e);
    if (val_lbl) {
        lv_label_set_text_fmt(val_lbl, "%.1fx", g_zoom);
    }
}

/* 加减按钮回调（保留，便于精确微调） */
static void on_zoom_minus(lv_event_t *e)
{
    (void)e;
    zoom_set_value(g_zoom - 0.1f);
}

static void on_zoom_plus(lv_event_t *e)
{
    (void)e;
    zoom_set_value(g_zoom + 0.1f);
}

/* 阻止事件冒泡到 overlay 背景，避免点击面板/按钮/滑块时关闭弹层 */
static void zoom_overlay_stop_bubble(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
}

static void make_zoom_overlay(void)
{
    const bool landscape = ui_is_landscape();
    /* 弧形直径：竖屏更大以充分利用高度，横屏适当缩小避免占用有限高度 */
    const int arc_size = landscape ? ui_scale_min(260) : ui_scale_min(320);

    g_zoom_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_zoom_overlay, lv_pct(100), lv_pct(100));
    /* 完全透明背景：只负责拦截点击和手势，不遮挡取景画面 */
    lv_obj_set_style_bg_opa(g_zoom_overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(g_zoom_overlay, 0, 0);
    lv_obj_set_style_border_width(g_zoom_overlay, 0, 0);
    lv_obj_remove_flag(g_zoom_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_zoom_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_zoom_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_zoom_overlay, zoom_overlay_bg_click, LV_EVENT_CLICKED, NULL);
    /* 右滑关闭（从右侧弹出的菜单自然对应向右滑走） */
    ui_attach_back_gesture(g_zoom_overlay, zoom_overlay_bg_click);

    /* 右侧弹出菜单面板：更窄、更透明，减少遮挡取景画面 */
    const int panel_w = arc_size / 2 + ui_scale_min(80);
    lv_obj_t *panel = lv_obj_create(g_zoom_overlay);
    lv_obj_set_size(panel, panel_w, lv_pct(100));
    lv_obj_align(panel, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    /* 仅左侧圆角（右侧贴边会被屏幕裁剪，视觉上只剩左侧圆角） */
    lv_obj_set_style_radius(panel, ui_scale_min(landscape ? 16 : 24), 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(panel, zoom_overlay_stop_bubble, LV_EVENT_CLICKED, NULL);
    /* 阻止面板上的手势冒泡到 overlay，避免在菜单上滑动变焦时误关闭 */
    lv_obj_add_event_cb(panel, zoom_overlay_stop_bubble, LV_EVENT_GESTURE, NULL);

    /* 标题 */
    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, tr("变焦", "Zoom"));
    lv_obj_set_style_text_font(title, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, -ui_scale_min(20), ui_scale_min(24));

    /* 当前变焦值 */
    lv_obj_t *val_lbl = lv_label_create(panel);
    lv_label_set_text_fmt(val_lbl, "%.1fx", g_zoom);
    lv_obj_set_style_text_font(val_lbl, ui_font_cjk_lg(), 0);
    lv_obj_set_style_text_color(val_lbl, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_align(val_lbl, LV_ALIGN_TOP_MID, -ui_scale_min(20), ui_scale_min(60));

    /* 弧形滑块容器：右边缘与屏幕/面板右边缘重合，圆心落在右侧边框上。
     * 通过向右偏移 arc_size/2，使 arc_cont 的中心精确对齐到面板（即屏幕）右边缘。 */
    lv_obj_t *arc_cont = lv_obj_create(panel);
    lv_obj_set_size(arc_cont, arc_size, arc_size);
    lv_obj_align(arc_cont, LV_ALIGN_RIGHT_MID, arc_size / 2, 0);
    lv_obj_set_style_bg_opa(arc_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(arc_cont, 0, 0);
    lv_obj_remove_flag(arc_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(arc_cont, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_flag(arc_cont, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(arc_cont, zoom_overlay_stop_bubble, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(arc_cont, zoom_overlay_stop_bubble, LV_EVENT_GESTURE, NULL);

    /* 半圆弧形滑块：圆心位于 arc_cont 中心 = 屏幕右边缘
     * 背景弧 90°~270°（左侧半圆），从下到上对应 1.0x~10.0x */
    g_zoom_arc = lv_arc_create(arc_cont);
    lv_obj_set_size(g_zoom_arc, arc_size, arc_size);
    lv_obj_center(g_zoom_arc);
    lv_arc_set_range(g_zoom_arc, 10, 100);
    lv_arc_set_value(g_zoom_arc, (int)(g_zoom * 10));
    lv_arc_set_mode(g_zoom_arc, LV_ARC_MODE_NORMAL);
    lv_arc_set_bg_angles(g_zoom_arc, 90, 270);
    lv_obj_set_style_arc_width(g_zoom_arc, ui_scale_min(12), LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_zoom_arc, ui_scale_min(12), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(g_zoom_arc, lv_color_hex(0x404040), LV_PART_MAIN);
    lv_obj_set_style_arc_color(g_zoom_arc, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g_zoom_arc, lv_color_hex(COLOR_ACCENT), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(g_zoom_arc, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_size(g_zoom_arc, ui_scale_min(28), ui_scale_min(28), LV_PART_KNOB);
    /* 移除手势冒泡，避免拖拽旋钮时触发右滑关闭 */
    lv_obj_remove_flag(g_zoom_arc, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(g_zoom_arc, zoom_overlay_stop_bubble, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(g_zoom_arc, zoom_overlay_stop_bubble, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(g_zoom_arc, on_zoom_arc_changed, LV_EVENT_VALUE_CHANGED, val_lbl);

    /* + / - 微调按钮：分别放在可见半圆的左上、左下 */
    lv_obj_t *plus = lv_button_create(arc_cont);
    lv_obj_set_size(plus, ui_scale_min(40), ui_scale_min(40));
    lv_obj_set_style_bg_color(plus, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(plus, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(plus, 0, 0);
    lv_obj_set_style_shadow_width(plus, 0, 0);
    lv_obj_remove_flag(plus, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(plus, zoom_overlay_stop_bubble, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(plus, on_zoom_plus, LV_EVENT_CLICKED, NULL);
    lv_obj_align(plus, LV_ALIGN_TOP_LEFT, ui_scale_min(8), ui_scale_min(8));
    lv_obj_t *pl = lv_label_create(plus);
    lv_label_set_text(pl, "+");
    lv_obj_set_style_text_font(pl, ui_font_sym(), 0);
    lv_obj_set_style_text_color(pl, lv_color_white(), 0);
    lv_obj_center(pl);

    lv_obj_t *minus = lv_button_create(arc_cont);
    lv_obj_set_size(minus, ui_scale_min(40), ui_scale_min(40));
    lv_obj_set_style_bg_color(minus, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(minus, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(minus, 0, 0);
    lv_obj_set_style_shadow_width(minus, 0, 0);
    lv_obj_remove_flag(minus, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(minus, zoom_overlay_stop_bubble, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(minus, on_zoom_minus, LV_EVENT_CLICKED, NULL);
    lv_obj_align(minus, LV_ALIGN_BOTTOM_LEFT, ui_scale_min(8), -ui_scale_min(8));
    lv_obj_t *ml = lv_label_create(minus);
    lv_label_set_text(ml, "-");
    lv_obj_set_style_text_font(ml, ui_font_sym(), 0);
    lv_obj_set_style_text_color(ml, lv_color_white(), 0);
    lv_obj_center(ml);
}

static void on_zoom_icon_click(lv_event_t *e)
{
    (void)e;
    LV_LOG_USER("Home: zoom button clicked, open zoom overlay");
    if (!g_zoom_overlay) make_zoom_overlay();
    if (g_zoom_arc && lv_obj_is_valid(g_zoom_arc)) {
        lv_arc_set_value(g_zoom_arc, (int)(g_zoom * 10));
    }
    lv_obj_remove_flag(g_zoom_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_zoom_overlay);
}

/* ================================================================== */
/* 主页创建                                                            */
/* ================================================================== */
static lv_obj_t *make_top_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, lv_pct(100), ui_scale_min(44));
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(bar, ui_scale(12), 0);
    lv_obj_set_style_pad_gap(bar, ui_scale(8), 0);
    /* 半透明黑色背景：预览全屏铺满时，状态栏叠加在预览之上，
     * 半透明背景确保状态栏文字/图标视觉可见，不被预览画面遮挡 */
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_60, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* 左侧：存储信息 + 录制按钮 + 录制时长 */
    g_storage_label = lv_label_create(bar);
    lv_obj_set_style_text_font(g_storage_label, ui_font_sym(), 0);
    lv_obj_set_style_text_color(g_storage_label, lv_color_white(), 0);
    update_storage_info(g_storage_label);

    g_rec_btn = lv_button_create(bar);
    lv_obj_set_size(g_rec_btn, ui_scale_min(40), ui_scale_min(40));
    lv_obj_set_style_bg_color(g_rec_btn, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_opa(g_rec_btn, LV_OPA_60, 0);
    lv_obj_set_style_radius(g_rec_btn, ui_scale_min(20), 0);
    lv_obj_set_style_border_width(g_rec_btn, 2, 0);
    lv_obj_set_style_border_color(g_rec_btn, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_shadow_width(g_rec_btn, 0, 0);
    lv_obj_remove_flag(g_rec_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_rec_btn, on_rec_btn_click, LV_EVENT_CLICKED, NULL);

    g_rec_label = lv_label_create(bar);
    lv_label_set_text(g_rec_label, "");
    lv_obj_set_style_text_font(g_rec_label, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(g_rec_label, lv_color_hex(0xFF0000), 0);

    /* 右侧弹性占位 */
    lv_obj_t *spacer = lv_obj_create(bar);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(spacer, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* 右侧：信息状态图标（GPS/WiFi/BT）紧贴电池，间距 5px */
    g_status_box = lv_obj_create(bar);
    lv_obj_remove_style_all(g_status_box);
    lv_obj_set_size(g_status_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g_status_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_status_box, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* 状态图标之间水平间距 20px（基于 min(宽,高) 缩放），
     * 让 GPS/WiFi/蓝牙 图标排列更清晰。 */
    lv_obj_set_style_pad_gap(g_status_box, ui_scale_min(20), 0);
    lv_obj_set_style_bg_opa(g_status_box, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(g_status_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* 状态图标与电池之间 5px 间距 */
    lv_obj_t *gap = lv_obj_create(bar);
    lv_obj_remove_style_all(gap);
    lv_obj_set_size(gap, ui_scale(5), 1);
    lv_obj_set_style_bg_opa(gap, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(gap, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* 最右侧：电池 */
    lv_obj_t *bat = lv_label_create(bar);
    lv_label_set_text(bat, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_style_text_font(bat, ui_font_sym(), 0);
    lv_obj_set_style_text_color(bat, lv_color_white(), 0);

    return bar;
}

static lv_obj_t *make_bottom_bar(lv_obj_t *parent)
{
    const bool landscape = ui_is_landscape();
    lv_obj_t *bar = lv_obj_create(parent);
    /* 横屏高度更小，避免状态图标/按钮超出预览区底部 */
    const int bar_h = landscape ? ui_scale_min(56) : ui_scale_min(100);
    const int bar_pad = landscape ? ui_scale_min(4) : ui_scale(12);
    lv_obj_set_size(bar, lv_pct(100), bar_h);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -ui_scale_min(landscape ? 4 : 16));
    lv_obj_set_style_pad_all(bar, bar_pad, 0);
    /* 半透明黑色背景：预览全屏铺满时，底部栏叠加在预览之上，
     * 半透明背景确保模式/快门/变焦按钮视觉可见，不被预览画面遮挡 */
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_60, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* 左下角：模式选择图标按钮（点击进入拍摄模式滚轮选择页）
     * 横屏进一步缩小尺寸，确保不溢出 bar 高度 */
    const int mode_btn_size = landscape ? ui_scale_min(44) : ui_scale_min(88);
    g_mode_icon_btn = make_text_icon_btn(bar, g_modes[g_func_mode].icon,
                                         mode_btn_size, on_mode_icon_click, NULL);
    lv_obj_align(g_mode_icon_btn, LV_ALIGN_BOTTOM_LEFT, ui_scale(4), 0);

    /* 水平居中：模式参数文字（如 "4K/16:9 · 增稳"） */
    g_mode_param_label = lv_label_create(bar);
    lv_obj_align(g_mode_param_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_font(g_mode_param_label, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(g_mode_param_label, lv_color_white(), 0);

    /* 右下角：变焦按钮
     * 横屏改为纯文字按钮（仅显示 1.0x），避免图标+文字在小尺寸内重叠溢出 */
    const int zoom_btn_size = landscape ? ui_scale_min(40) : ui_scale_min(80);
    g_zoom_btn = lv_button_create(bar);
    lv_obj_set_size(g_zoom_btn, zoom_btn_size, zoom_btn_size);
    lv_obj_set_style_bg_color(g_zoom_btn, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_opa(g_zoom_btn, LV_OPA_60, 0);
    lv_obj_set_style_radius(g_zoom_btn, ui_scale_min(10), 0);
    lv_obj_set_style_border_width(g_zoom_btn, 0, 0);
    lv_obj_set_style_shadow_width(g_zoom_btn, 0, 0);
    lv_obj_remove_flag(g_zoom_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_zoom_btn, on_zoom_icon_click, LV_EVENT_CLICKED, NULL);
    ui_attach_press_scale(g_zoom_btn, 230);
    lv_obj_align(g_zoom_btn, LV_ALIGN_BOTTOM_RIGHT, -ui_scale(4), 0);

    g_zoom_btn_label = lv_label_create(g_zoom_btn);
    lv_label_set_text_fmt(g_zoom_btn_label, "%.1fx", g_zoom);
    lv_obj_set_style_text_font(g_zoom_btn_label, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(g_zoom_btn_label, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_center(g_zoom_btn_label);

    /* 竖屏保留原来的图标+底部文字样式，信息更丰富 */
    if (!landscape) {
        lv_obj_t *ic = lv_label_create(g_zoom_btn);
        lv_label_set_text(ic, LV_SYMBOL_PLUS);
        lv_obj_set_style_text_font(ic, ui_font_sym(), 0);
        lv_obj_set_style_text_color(ic, lv_color_white(), 0);
        lv_obj_center(ic);
        /* 文字压在图标下方 */
        lv_obj_align(g_zoom_btn_label, LV_ALIGN_BOTTOM_MID, 0, -ui_scale_min(6));
    }

    return bar;
}

/* ================================================================== */
/* 摄像头预览                                                          */
/* ================================================================== */
/* 帧拉取定时器回调：检查 preview worker 是否有新帧 → 零拷贝更新 canvas → invalidate。
 *
 * 性能优化（K230）：YUV→RGB565 转换在 camera_preview worker 线程并行进行，
 * UI 线程此回调仅做 lv_canvas_set_buffer + lv_obj_invalidate（<0.1ms），
 * 彻底消除 8~12ms/帧的转换阻塞。
 *
 * 帧跳过机制：若 worker 转换完一帧但 UI 尚未消费上一帧，worker 自动丢弃新帧。
 * 这保证了 LVGL 渲染中的 display buffer 不会被 worker 覆盖（3 缓冲区方案）。 */
static void camera_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!g_camera || !g_camera_canvas || !g_preview) return;

    /* 异步拍照完成检查：JPEG 编码在后台线程完成时，刷新存储信息。
     * 轮询方式无需额外定时器，每次帧回调检查一次 */
    if (photo_async_done()) {
        LV_LOG_USER("异步拍照完成: %s", photo_last_path());
        if (g_storage_label) update_storage_info(g_storage_label);
    }

    /* 检查 preview worker 是否有新帧：零拷贝更新 canvas buffer */
    if (camera_preview_consume_new_frame(g_preview)) {
        const uint16_t *new_buf = camera_preview_get_display(g_preview);
        if (new_buf) {
            /* canvas 直接使用 worker 的 display buffer，无需 memcpy */
            lv_canvas_set_buffer(g_camera_canvas, (void *)new_buf,
                                  g_camera_buf_w, g_camera_buf_h,
                                  LV_COLOR_FORMAT_RGB565);
            /* 更新 g_camera_buf 指针，仅供其他逻辑读取当前预览帧
             * （拍照已改为全分辨率路径，不再使用此 buffer 保存） */
            g_camera_buf = (uint16_t *)new_buf;
            lv_obj_invalidate(g_camera_canvas);
        }
    }

    /* 全分辨率拍照快照就绪检查：worker 线程已完成"仅旋转不降采样"转换，
     * 取走 photo buffer 启动异步 JPEG 编码保存。
     * 这样保存的照片使用原始分辨率（如 1920x1080），而非降采样后的预览 buffer，
     * 避免画质损失和预览/保存画面不一致问题。 */
    if (camera_preview_is_photo_ready(g_preview)) {
        const uint16_t *photo_buf = NULL;
        int photo_w = 0, photo_h = 0;
        if (camera_preview_consume_photo(g_preview, &photo_buf, &photo_w, &photo_h) == 0
            && photo_buf && photo_w > 0 && photo_h > 0) {
            /* 计算目标保存尺寸（按当前画幅+图片大小档位） */
            int target_w = 0, target_h = 0;
            photosize_get_dims(g_sel_photosize, g_sel_ratio, &target_w, &target_h);
            /* photo_save_jpeg_scaled_async 内部会 memcpy 源数据到独立缓冲区，
             * 因此 consume_photo 返回的内部 buffer 可立即被下次 take_photo 复用。
             * 全分辨率源帧（如 1920x1080≈2MP）裁剪+缩放到目标尺寸（如 12MP），
             * 画质远优于之前用 480x800 预览 buffer 放大。 */
            int ret = photo_save_jpeg_scaled_async(photo_buf, photo_w, photo_h,
                                                     g_sel_ratio, target_w, target_h,
                                                     g_photo_save_dir);
            if (ret == 0) {
                home_flash_shutter();
                LV_LOG_USER("全分辨率拍照已启动异步编码 (源=%dx%d → %dx%d, 画幅=%s%s)",
                            photo_w, photo_h, target_w, target_h,
                            g_ratio_opts[g_sel_ratio],
                            g_burst_active ? " 连拍中" : "");
            } else if (ret == -2) {
                LV_LOG_WARN("拍照跳过: 上次JPEG编码仍在进行中");
            } else {
                LV_LOG_ERROR("拍照失败: photo_save_jpeg_scaled_async 返回 %d", ret);
            }
        }
    }

    /* 拍照请求触发：g_take_photo_pending 由快门按钮/倒计时/连拍定时器设置。
     * 不再直接使用预览 buffer 保存，而是请求 worker 做全分辨率快照，
     * worker 转换完成后通过上面的 photo_ready 检查路径启动编码保存。 */
    if (g_take_photo_pending) {
        g_take_photo_pending = 0;
        home_take_photo();
    }
}

/* 启动摄像头预览：打开摄像头 → 创建 preview worker → 创建 canvas → 启动定时器。
 * 失败时保持黑色背景（view 自身颜色），不阻塞 UI。
 *
 * 性能架构：
 *   - V4L2 采集 + YUV→RGB565 转换 + 录制器喂帧 → camera_preview worker 线程
 *   - UI 线程仅做 lv_canvas_set_buffer + invalidate（<0.1ms/帧）
 *   - 3 缓冲区零拷贝：canvas 直接使用 worker 的 display buffer
 *
 * K230 优化：转换在 worker 线程并行，UI 线程不再阻塞，预览帧率从 20fps 提升到 30fps */

/* 摄像头延迟启动定时器回调：UI 创建完成后触发，避免 V4L2 打开阻塞首帧渲染。
 * 此时 view 及其子控件（状态栏/按钮）已创建并渲染，用户先看到完整 UI 框架，
 * 摄像头预览在 100ms 后异步启动并铺满 view 底层。 */
static void camera_start_timer_cb(lv_timer_t *t)
{
    (void)t;
    g_camera_start_timer = NULL;  /* 定时器已执行，清空句柄（repeat_count=1 会自动销毁） */
    if (!g_camera_view) {
        LV_LOG_WARN("camera_start_timer_cb: g_camera_view 已失效，跳过启动");
        return;
    }
    LV_LOG_USER("摄像头延迟启动：开始初始化");
    home_camera_start(g_camera_view);
}

static void home_camera_start(lv_obj_t *parent_view)
{
    if (g_camera) return;  /* 已启动 */
    g_camera_view = parent_view;  /* 保存父容器，供重启摄像头使用 */

    /* 协商格式：从 device.json 读取摄像头能力上限。
     * 当前 GC2093 配置 1920x1080@30fps（预览无需 60fps，降低 CPU 占用）。
     * 后续更换 4K@60fps 摄像头时，只需修改 device.json 的 camera_capabilities，
     * 软件自动协商到更高分辨率，无需改代码。
     * 摄像头硬件会按其能力回退到最接近的支持格式。
     * 视频模式按用户选择的帧率协商（慢动作模式需高帧率） */
    const device_config_t *cfg = device_config_get();
    int cam_w = cfg->camera_caps.max_video_width;
    int cam_h = cfg->camera_caps.max_video_height;
    /* 视频模式按用户选择帧率协商，其他模式默认 30fps */
    int cam_fps = (g_func_mode == 1) ? atoi(g_fps_opts[g_sel_fps]) : 30;
    /* 慢动作模式强制高帧率（60fps） */
    if (g_func_mode == 4) cam_fps = 60;
    /* 限制不超过摄像头能力上限 */
    if (cam_fps > cfg->camera_caps.max_video_fps) cam_fps = cfg->camera_caps.max_video_fps;
    if (cam_fps < 24) cam_fps = 30;
    LV_LOG_USER("摄像头协商参数: %dx%d@%dfps（能力上限: %dx%d@%d）",
                cam_w, cam_h, cam_fps,
                cfg->camera_caps.max_video_width,
                cfg->camera_caps.max_video_height,
                cfg->camera_caps.max_video_fps);
    g_camera = camera_open_default(cam_w, cam_h, cam_fps);
    if (!g_camera) {
        LV_LOG_WARN("摄像头未检测到或打开失败，主页保持黑色背景");
        return;
    }

    camera_format_t fmt;
    if (camera_get_format(g_camera, &fmt) == 0) {
        LV_LOG_USER("摄像头已打开: %dx%d %s @%dfps",
                    fmt.width, fmt.height, camera_fourcc_name(fmt.fourcc), fmt.fps);
        /* 保存实际协商的原始帧尺寸，供录制配置使用 */
        g_cam_src_w = fmt.width;
        g_cam_src_h = fmt.height;
        g_cam_src_fps = fmt.fps;
    }

    /* canvas 尺寸 = 屏幕逻辑分辨率（全屏铺满，无黑边）。
     * 预览画面全屏幕区域显示，UI 元素（状态栏/底部栏/按钮）通过
     * 半透明背景叠加在预览之上，始终可见不被遮挡。
     * 预览画面按当前画幅裁剪源帧后填充屏幕，保证：
     *   1. 预览画面与保存照片视觉一致（都用相同画幅裁剪）
     *   2. 调整画幅时预览实时变化（画幅裁剪在转换时执行） */
    int scr_w = ui_get_screen_width();
    int scr_h = ui_get_screen_height();
    g_camera_buf_w = scr_w;
    g_camera_buf_h = scr_h;

    /* 旋转角度根据横竖屏自动选择：
     * - 横屏（宽>高）：180° 翻转（传感器倒装修正）
     * - 竖屏（高>宽）：90° 顺时针旋转（横向帧旋转为纵向显示） */
    camera_rotate_t rotate = (scr_w >= scr_h) ? CAM_ROTATE_180 : CAM_ROTATE_90;

    /* 创建预览 worker 模块：3 缓冲区 + 后台线程转换。
     * 缓冲区由 preview 模块内部管理（3 份 RGB565），UI 零拷贝使用 display buffer。
     * g_camera_buf 将指向 preview 的 display buffer（在 camera_timer_cb 中更新）。 */
    g_preview = camera_preview_create(g_camera, g_camera_buf_w, g_camera_buf_h, rotate);
    if (!g_preview) {
        LV_LOG_ERROR("camera_preview_create 失败");
        camera_close(g_camera);
        g_camera = NULL;
        g_camera_buf_w = 0;
        g_camera_buf_h = 0;
        return;
    }
    /* 设置初始画幅裁剪：预览按当前画幅裁剪源帧，与保存照片内容一致 */
    camera_preview_set_crop_ratio(g_preview, g_sel_ratio);

    /* 获取 preview 的初始 display buffer（全黑），用于 canvas 初始化。
     * g_camera_buf 指向 preview 管理的 buffer，不在此 malloc。 */
    g_camera_buf = (uint16_t *)camera_preview_get_display(g_preview);

    /* 创建 canvas，作为 parent_view 的子对象，置于最底层（z-order 最低）。
     * canvas 全屏铺满（与屏幕尺寸一致），预览画面按画幅裁剪后填充。
     * UI 元素（状态栏/底部栏/按钮）创建在 canvas 之前（page_home_create 中），
     * 配合半透明背景确保视觉可见，不被预览画面遮挡。
     * z-order 调整在 home_create_crop_guide 之后统一处理。 */
    g_camera_canvas = lv_canvas_create(parent_view);
    lv_canvas_set_buffer(g_camera_canvas, g_camera_buf,
                          g_camera_buf_w, g_camera_buf_h,
                          LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(g_camera_canvas, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(g_camera_canvas, 0, 0);
    lv_obj_set_style_border_width(g_camera_canvas, 0, 0);
    lv_obj_set_style_radius(g_camera_canvas, 0, 0);
    lv_obj_remove_flag(g_camera_canvas, LV_OBJ_FLAG_SCROLLABLE);
    /* canvas 不需要可点击/手势，让事件穿透到 view 处理双击/长按 */
    lv_obj_remove_flag(g_camera_canvas, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* 创建画幅裁剪引导 overlay（必须在状态栏/按钮/底部栏创建之前，
     * 保证 z-order：canvas < 裁剪引导 < 控件）。
     * overlay 全屏铺满，遮罩条相对屏幕定位。
     * 初始隐藏，下方启动定时器后由 on_param_changed/首次刷新时显示。 */
    home_create_crop_guide();

    /* 统一调整 z-order：canvas 最底层 < 裁剪引导 overlay < 状态栏/按钮/底部栏。
     * canvas 和 overlay 在状态栏等控件之后创建（延迟启动），z-order 默认在上层。
     * 先把 overlay move_background（移到状态栏之下），再把 canvas move_background
     * （移到 overlay 之下），确保预览画面不遮挡任何 UI 元素，
     * 裁剪引导遮罩条不遮挡状态栏/底部栏。 */
    if (g_crop_guide_overlay) {
        lv_obj_move_background(g_crop_guide_overlay);
    }
    lv_obj_move_background(g_camera_canvas);

    /* 开始采集 */
    if (camera_start(g_camera) != 0) {
        LV_LOG_ERROR("摄像头 STREAMON 失败");
        camera_preview_destroy(g_preview);
        g_preview = NULL;
        g_camera_buf = NULL;
        camera_close(g_camera);
        g_camera = NULL;
        if (g_camera_canvas) {
            lv_obj_delete(g_camera_canvas);
            g_camera_canvas = NULL;
        }
        return;
    }

    /* 启动 preview worker 线程（在 camera_start 之后，确保 streaming 已就绪） */
    if (camera_preview_start(g_preview) != 0) {
        LV_LOG_ERROR("camera_preview_start 失败");
        camera_stop(g_camera);
        camera_preview_destroy(g_preview);
        g_preview = NULL;
        g_camera_buf = NULL;
        camera_close(g_camera);
        g_camera = NULL;
        if (g_camera_canvas) {
            lv_obj_delete(g_camera_canvas);
            g_camera_canvas = NULL;
        }
        return;
    }

    /* 如果录制器已存在（重启摄像头场景），重新关联到 preview worker */
    if (g_recorder) {
        camera_preview_set_recorder(g_preview, g_recorder);
        if (g_recording) {
            camera_preview_set_recording(g_preview, 1);
        }
    }

    /* 帧拉取定时器周期：
     * 转换在 worker 线程进行，UI 线程此定时器仅轮询新帧 + invalidate，
     * 不再阻塞，所有平台统一使用 33ms（30fps 轮询）。
     * K230 不再需要降帧到 20fps（转换不阻塞 UI）。 */
    int timer_period = 33;
    g_camera_timer = lv_timer_create(camera_timer_cb, timer_period, NULL);
    LV_LOG_USER("摄像头预览已启动 (%dx%d, %dms周期=%dfps, worker线程转换)",
                g_camera_buf_w, g_camera_buf_h, timer_period, 1000/timer_period);

    /* 摄像头启动后应用一次当前拍摄参数（手动模式的曝光/白平衡等） */
    home_apply_camera_controls();

    /* 摄像头就绪后显示画幅裁剪引导（拍照模式 WYSIWYG）。
     * 缓冲区尺寸已确定，可精确计算遮罩位置。 */
    home_update_crop_guide();
}

/* 停止摄像头预览：停止定时器 → 停止 preview worker → 停止采集 → 释放资源。
 * 顺序很重要：
 *   1. 停定时器：避免回调访问已释放对象
 *   2. 停 preview worker：join 线程，确保不再访问 camera/canvas
 *   3. 删 canvas：确保 LVGL 不再渲染 preview 管理的 buffer
 *   4. 停 camera + close：所有 V4L2 buffer 已归还（worker 释放）
 *   5. 销毁 preview：释放 3 个 RGB565 buffer（canvas 已删，无悬空引用） */
static void home_camera_stop(void)
{
    if (g_camera_timer) {
        lv_timer_delete(g_camera_timer);
        g_camera_timer = NULL;
    }
    if (g_preview) {
        camera_preview_stop(g_preview);     /* join worker 线程 */
        camera_preview_set_recorder(g_preview, NULL);  /* 断开录制器关联 */
    }
    /* 先删 canvas，确保 LVGL 不再引用 preview 的 buffer */
    if (g_camera_canvas) {
        lv_obj_delete(g_camera_canvas);
        g_camera_canvas = NULL;
    }
    if (g_preview) {
        camera_preview_destroy(g_preview);  /* 释放 3 个 RGB565 buffer */
        g_preview = NULL;
    }
    if (g_camera) {
        camera_stop(g_camera);
        camera_close(g_camera);
        g_camera = NULL;
    }
    /* g_camera_buf 指向 preview 管理的 buffer，已随 preview 销毁。
     * 不在此 free，只置 NULL 避免悬空指针。 */
    g_camera_buf = NULL;
    g_camera_buf_w = 0;
    g_camera_buf_h = 0;
    g_cam_src_w = 0;
    g_cam_src_h = 0;
    g_cam_src_fps = 0;
}

/* 重启摄像头预览：用于分辨率/帧率变更后重新协商格式。
 * 停止当前采集 → 关闭设备 → 重新打开协商新格式 → 启动采集。
 * 重启失败时保持黑色背景（home_camera_start 内部处理失败） */
static void home_camera_restart(void)
{
    if (!g_camera_view) {
        LV_LOG_WARN("无法重启摄像头：预览区父容器未保存");
        return;
    }
    /* 录制中重启：先停止录制（录制器依赖连续帧供应）。
     * 注意：preview worker 会在 home_camera_stop 中自动停止喂帧，
     * 但录制器的 stop/destroy 必须在 home_camera_stop 之前完成，
     * 避免 worker 线程访问已销毁的录制器。 */
    if (g_recording) {
        /* 先通知 preview worker 停止喂帧 */
        if (g_preview) {
            camera_preview_set_recording(g_preview, 0);
            camera_preview_set_recorder(g_preview, NULL);
        }
        if (g_recorder) {
            recorder_stop(g_recorder);
            recorder_destroy(g_recorder);
            g_recorder = NULL;
        }
        g_recording = 0;
        if (g_rec_timer) {
            lv_timer_delete(g_rec_timer);
            g_rec_timer = NULL;
        }
        home_refresh_rec_ui();
    }
    home_camera_stop();
    home_camera_start(g_camera_view);
}

lv_obj_t * page_home_create(lv_obj_t *tv)
{
    static int params_inited = 0;
    if (!params_inited) {
        /* 先初始化默认参数，再尝试从文件加载（文件不存在或格式错误时保持默认） */
        mode_params_init();
        mode_params_load_from_file();
        /* 同步全局参数到当前默认模式（g_func_mode），避免初始 g_sel_* 与
         * g_mode_params[g_func_mode] 不一致（例如 g_func_mode=0 时全局
         * ratio 默认 1，但 photo 模式记录的 ratio 为 0） */
        mode_params_load(g_func_mode);
        params_inited = 1;
    }

    /* 主页 tile：tileview 原生滑动导航（无需手动手势回调）
     * LV_DIR_TOP    = 向上滑 → menu (0,0)
     * LV_DIR_BOTTOM = 向下滑 → quick (0,2)
     * LV_DIR_LEFT   = 向左滑 → storage (1,1) */
    lv_obj_t *tile = lv_tileview_add_tile(tv, 0, 1, LV_DIR_TOP | LV_DIR_BOTTOM | LV_DIR_LEFT);
    lv_obj_set_style_bg_color(tile, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(tile, 0, 0);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

    /* 黑色全屏预览区域 */
    lv_obj_t *view = lv_obj_create(tile);
    lv_obj_set_size(view, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(view, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(view, 0, 0);
    lv_obj_set_style_pad_all(view, 0, 0);
    lv_obj_remove_flag(view, LV_OBJ_FLAG_SCROLLABLE);

    /* 预览区手势：双击切换模式，长按打开快速设置
     * tileview 滑动导航由 tile 的 scroll_dir 处理，不需要手动 GESTURE 回调。
     * GESTURE_BUBBLE 允许手势从 view 冒泡到 tile（tileview 处理滚动）。 */
    ui_attach_double_click(view, on_view_double_click, NULL);
    ui_attach_long_press(view, 0, on_view_long_press, NULL);
    lv_obj_add_flag(view, LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* 摄像头预览：延迟启动以优化启动速度。
     * 原实现 home_camera_start 同步调用 V4L2 打开+MMAP+STREAMON+worker线程启动，
     * 阻塞 UI 创建流程导致首帧迟迟无法渲染。改为延迟 100ms 启动：
     *   1. 先完成 view/状态栏/按钮等 UI 创建，让用户立即看到完整框架
     *   2. 100ms 后异步启动摄像头，canvas 创建后 move_background 自动叠层
     * 失败（无摄像头/权限不足）时保持 view 的黑色背景，不阻塞 UI。 */
    g_camera_view = view;  /* 先保存父容器，供延迟回调使用 */
    g_camera_start_timer = lv_timer_create(camera_start_timer_cb, 100, NULL);
    lv_timer_set_repeat_count(g_camera_start_timer, 1);  /* 只触发一次 */

    /* 顶部状态栏 */
    make_top_bar(view);

    /* 左侧相册 */
    lv_obj_t *album_btn = make_large_icon_btn(view, LV_SYMBOL_IMAGE, ui_scale_min(84),
                                              on_album_click, NULL);
    lv_obj_align(album_btn, LV_ALIGN_LEFT_MID, ui_scale(16), 0);

    /* 右侧参数设置 */
    lv_obj_t *param_btn = make_large_icon_btn(view, LV_SYMBOL_SETTINGS, ui_scale_min(84),
                                              on_param_settings_click, NULL);
    lv_obj_align(param_btn, LV_ALIGN_RIGHT_MID, -ui_scale(16), 0);

    /* 底部功能区 */
    make_bottom_bar(view);

    /* 初始化刷新 */
    home_refresh_status_icons();
    home_refresh_mode_icon();
    home_refresh_bottom_params();
    home_refresh_zoom_btn();
    home_refresh_rec_ui();
    /* 根据保存的参数显示网格线（用户上次设置的开关状态） */
    home_refresh_grid();

    /* 启动速度优化：弹层改为懒加载（首次打开时创建）。
     * make_param_overlay/make_mode_overlay/make_zoom_overlay 等打开函数
     * 内部已支持 if (!g_*_overlay) make_*_overlay() 按需创建，
     * 启动时预创建会阻塞 UI 首帧渲染。 */

    /* WiFi 连接状态定时检查：每 2 秒检查 wpa_status，连接时显示 WiFi 图标，
     * 断开时不显示。定时器在 page_home_destroy 中删除。 */
#ifndef APP_WIFI_DISABLED
    if (!s_wifi_status_timer) {
        s_wifi_status_timer = lv_timer_create(wifi_status_timer_cb, 2000, NULL);
        /* 立即触发一次，避免首次显示需等待 2 秒 */
        lv_timer_ready(s_wifi_status_timer);
    }
#endif

    return tile;
}

/* ================================================================== */
/* 公共 API                                                            */
/* ================================================================== */
void page_home_destroy(void)
{
    /* 先停止页面级定时器，避免对象删除过程中定时器回调访问悬空指针。 */
    if (g_rec_timer) {
        lv_timer_delete(g_rec_timer);
        g_rec_timer = NULL;
    }
    /* 删除摄像头延迟启动定时器：若 page_home_destroy 在 100ms 内被调用
     * （如快速旋转重建），需取消待执行的 camera_start_timer_cb，避免
     * 回调在 view 销毁后访问悬空的 g_camera_view。 */
    if (g_camera_start_timer) {
        lv_timer_delete(g_camera_start_timer);
        g_camera_start_timer = NULL;
    }
    /* 清理录制器（如在录制中，先停止再销毁）。
     * 必须先断开 preview worker 的录制器关联，避免 worker 访问已销毁的录制器。 */
    if (g_preview) {
        camera_preview_set_recording(g_preview, 0);
        camera_preview_set_recorder(g_preview, NULL);
    }
    if (g_recorder) {
        recorder_stop(g_recorder);
        recorder_destroy(g_recorder);
        g_recorder = NULL;
    }
    g_recording = 0;

    /* 删除 WiFi 状态检查定时器（旋转重建时也会调用 destroy） */
    if (s_wifi_status_timer) {
        lv_timer_delete(s_wifi_status_timer);
        s_wifi_status_timer = NULL;
    }

    /* 清理拍照倒计时定时器和 overlay */
    if (g_countdown_timer) {
        lv_timer_delete(g_countdown_timer);
        g_countdown_timer = NULL;
    }
    g_countdown_remaining = 0;
    if (g_countdown_overlay && lv_obj_is_valid(g_countdown_overlay)) {
        lv_obj_delete(g_countdown_overlay);
        g_countdown_overlay = NULL;
        g_countdown_label = NULL;
    }

    /* 清理延时拍摄定时器 */
    if (g_timelapse_timer) {
        lv_timer_delete(g_timelapse_timer);
        g_timelapse_timer = NULL;
    }
    g_timelapse_active = 0;

    /* 清理连拍定时器 */
    home_stop_burst();

    /* 清理快门闪烁 overlay（如在动画中，先停动画） */
    if (g_flash_overlay && lv_obj_is_valid(g_flash_overlay)) {
        lv_anim_del(g_flash_overlay, flash_anim_cb);
        lv_obj_delete(g_flash_overlay);
        g_flash_overlay = NULL;
    }

    /* 清理网格线 overlay（旋转重建时需重新创建，避免位置错位） */
    if (g_grid_overlay && lv_obj_is_valid(g_grid_overlay)) {
        lv_obj_delete(g_grid_overlay);
        g_grid_overlay = NULL;
    }

    /* 清理画幅裁剪引导 overlay（随 view 一并销毁，但需清空句柄避免悬空） */
    if (g_crop_guide_overlay && lv_obj_is_valid(g_crop_guide_overlay)) {
        lv_obj_delete(g_crop_guide_overlay);
        g_crop_guide_overlay = NULL;
        g_crop_bar_a = NULL;
        g_crop_bar_b = NULL;
    }

    /* 停止摄像头预览：停止帧拉取定时器 → 停止 V4L2 采集 → 释放 canvas → 关闭设备。
     * 必须在 view 销毁前调用，避免 canvas 被销毁后定时器回调访问悬空指针。 */
    home_camera_stop();
    g_camera_view = NULL;  /* 清理父容器引用，避免悬空指针 */

    /* 删除弹层前先移除动态事件回调，防止对象析构期间仍有 pending 的
     * scroll/gesture 事件被分发到已释放内存（尤其是 mode_list 的 SCROLL
     * 回调会在滚动动画中高频触发）。 */
    if (g_mode_list && lv_obj_is_valid(g_mode_list)) {
        lv_obj_remove_event_cb(g_mode_list, on_mode_list_scrolled);
    }

    if (g_param_overlay && lv_obj_is_valid(g_param_overlay)) {
        lv_obj_add_flag(g_param_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_delete(g_param_overlay);
        g_param_overlay = NULL;
    }
    if (g_selector_overlay && lv_obj_is_valid(g_selector_overlay)) {
        lv_obj_add_flag(g_selector_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_delete(g_selector_overlay);
        g_selector_overlay = NULL;
    }
    if (g_mode_overlay && lv_obj_is_valid(g_mode_overlay)) {
        lv_obj_add_flag(g_mode_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_delete(g_mode_overlay);
        g_mode_overlay = NULL;
    }
    if (g_zoom_overlay && lv_obj_is_valid(g_zoom_overlay)) {
        lv_obj_add_flag(g_zoom_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_delete(g_zoom_overlay);
        g_zoom_overlay = NULL;
    }

    g_storage_label     = NULL;
    g_status_box        = NULL;
    g_rec_btn           = NULL;
    g_rec_label         = NULL;
    g_mode_icon_btn     = NULL;
    g_mode_param_label  = NULL;
    g_zoom_btn          = NULL;
    g_zoom_btn_label    = NULL;
    g_zoom_arc          = NULL;
    g_selector_list     = NULL;
    g_mode_list         = NULL;
    g_mode_spacer_start = NULL;
    g_mode_spacer_end   = NULL;
    /* 记住上次选择的模式索引，下次打开弹层时默认从该模式开始 */
    g_mode_sel_idx      = g_func_mode;
    for (int i = 0; i < MODE_ICON_MAX; i++) {
        g_mode_icons[i] = NULL;
        g_mode_texts[i] = NULL;
        g_mode_items[i] = NULL;
        s_last_icon_style[i].opa   = -1;
        s_last_icon_style[i].scale = -1;
        s_last_icon_style[i].color = lv_color_hex(0);
        s_last_text_style[i].opa   = -1;
        s_last_text_style[i].scale = -1;
        s_last_text_style[i].color = lv_color_hex(0);
    }
}

void page_home_refresh(void)
{
    if (g_storage_label) update_storage_info(g_storage_label);
    home_refresh_status_icons();
    home_refresh_mode_icon();
    home_refresh_bottom_params();
    home_refresh_zoom_btn();
    home_refresh_rec_ui();
}

void page_home_set_param(const char *res, const char *fps)
{
    /* res/fps 参数冗余：实际选中索引通过 page_home_sel_res/fps 直接修改全局变量，
     * 此处仅触发刷新与摄像头重启 */
    (void)res;
    (void)fps;
    /* 分辨率/帧率变更后刷新底部显示，保存当前模式参数，并重启摄像头以重新协商格式 */
    home_refresh_bottom_params();
    mode_params_save(g_func_mode);
    /* 重启摄像头使新帧率生效（视频模式按用户选择帧率协商） */
    if (g_camera) {
        LV_LOG_USER("分辨率/帧率变更，重启摄像头重新协商格式");
        home_camera_restart();
    }
}

void page_home_set_zoom(const char *zoom)
{
    float z = 1.0f;
    if (zoom) sscanf(zoom, "%f", &z);
    g_zoom = z;
    home_refresh_zoom_btn();
}

void page_home_update_storage(void)
{
    if (g_storage_label)
        update_storage_info(g_storage_label);
}

/* 压力测试：外部控制模式弹层开/关。
 * open=1 打开弹层并滚动到当前模式；open=0 关闭弹层并应用选中项。 */
void page_home_stress_mode_overlay(int open)
{
    if (open) {
        if (!g_mode_overlay) make_mode_overlay();
        if (g_mode_overlay && lv_obj_is_valid(g_mode_overlay)) {
            lv_obj_remove_flag(g_mode_overlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(g_mode_overlay);
            ui_fade_in_overlay(g_mode_overlay, 180);
            mode_overlay_scroll_to_current();
            LV_LOG_USER("STRESS: mode overlay opened");
        }
    } else {
        home_mode_overlay_close();
        LV_LOG_USER("STRESS: mode overlay closed");
    }
}

/* 压力测试入口：打开参数设置弹层 */
void page_home_stress_param_overlay(int open)
{
    if (open) {
        if (!g_param_overlay) make_param_overlay();
        if (g_param_overlay && lv_obj_is_valid(g_param_overlay)) {
            home_param_overlay_refresh();
            lv_obj_remove_flag(g_param_overlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(g_param_overlay);
            LV_LOG_USER("STRESS: param overlay opened");
        }
    } else {
        home_param_overlay_close();
        LV_LOG_USER("STRESS: param overlay closed");
    }
}

/* 压力测试入口：打开变焦弹层 */
void page_home_stress_zoom_overlay(int open)
{
    if (open) {
        if (!g_zoom_overlay) make_zoom_overlay();
        if (g_zoom_overlay && lv_obj_is_valid(g_zoom_overlay)) {
            if (g_zoom_arc && lv_obj_is_valid(g_zoom_arc)) {
                lv_arc_set_value(g_zoom_arc, (int)(g_zoom * 10));
            }
            lv_obj_remove_flag(g_zoom_overlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(g_zoom_overlay);
            LV_LOG_USER("STRESS: zoom overlay opened");
        }
    } else {
        home_zoom_overlay_close();
        LV_LOG_USER("STRESS: zoom overlay closed");
    }
}

const char ** page_home_res_opts(void) { return g_res_opts; }
const char ** page_home_fps_opts(void) { return g_fps_opts; }
int * page_home_sel_res(void) { return &g_sel_res; }
int * page_home_sel_fps(void) { return &g_sel_fps; }

/* 供快捷参数页读取拍照模式选项 */
const char ** page_home_ratio_opts(void) { return g_ratio_opts; }
const char ** page_home_photosize_opts(void) { return PHOTOSIZE_OPTS; }
const char ** page_home_burst_opts(void) { return BURST_OPTS; }
int * page_home_sel_ratio(void) { return &g_sel_ratio; }
int * page_home_sel_photosize(void) { return &g_sel_photosize; }
int * page_home_sel_burst(void) { return &g_sel_burst; }

/* 快捷参数页变更拍照参数后刷新主页底部显示，并保存到当前模式 */
void page_home_photo_param_changed(void)
{
    home_refresh_bottom_params();
    mode_params_save(g_func_mode);
}

/* 供熄屏显示页读取当前拍摄状态 */
int page_home_get_mode(void)
{
    return g_func_mode;
}

int page_home_is_recording(void)
{
    return g_recording;
}

void page_home_get_rec_time(char *buf, size_t sz)
{
    if (g_recording) {
        fmt_rec_time(lv_tick_elaps(g_rec_start_ms), buf, sz);
    } else {
        snprintf(buf, sz, "00:00");
    }
}

void page_home_get_mode_param_text(char *buf, size_t sz)
{
    if (g_func_mode < 0 || g_func_mode >= (int)MODE_COUNT) {
        snprintf(buf, sz, "%s %s", LV_SYMBOL_VIDEO, "4K/30");
        return;
    }
    const char *mode_icon = g_modes[g_func_mode].icon;
    if (g_func_mode == 0) {
        /* 拍照模式：显示画幅 */
        snprintf(buf, sz, "%s %s", mode_icon, g_ratio_opts[g_sel_ratio]);
    } else {
        /* 视频相关模式：显示分辨率/增稳 */
        snprintf(buf, sz, "%s %s %s", mode_icon, g_res_opts[g_sel_res], STAB_OPTS[g_sel_stab]);
    }
}
