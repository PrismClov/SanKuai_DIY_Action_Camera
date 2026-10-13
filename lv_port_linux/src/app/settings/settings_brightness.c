/**
 * @file settings_brightness.c
 * @brief 亮度设置页
 *
 * 特性：
 *   - 顶部栏：返回上一级 / 标题居中
 *   - 竖屏：顶部大字号百分比 + 居中纵向粗圆角滑动条 + 底部太阳图标
 *   - 横屏：顶部百分比/标签 + 底部横向滑动条 + 左侧太阳图标
 *   - 滑动时实时写入 /sys/class/backlight/.../brightness 控制屏幕背光
 *   - 自动探测常见 RK3566 背光节点，PC 模拟环境仅记录日志不报错
 */
#include "settings_brightness.h"
#include "../app_main.h"
#include "../common/ui_helpers.h"
#include "../common/app_i18n.h"

#include "lvgl/lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static lv_obj_t *g_overlay;
static lv_obj_t *g_slider;
static lv_obj_t *g_pct_label;

/* 当前亮度百分比 0-100 */
static int s_brightness_pct = 80;

/* ------------------------------------------------------------------ */
/* 背光 sysfs 路径探测与读写                                          */
/* ------------------------------------------------------------------ */
/* 候选背光节点：覆盖 RK3566（Rockchip）和 K230（Canaan）常见路径。
 * RK3566: backlight / rk28_bl / rk_backlight / rockchip
 * K230:    pwm-backlight / k230-bl / backlight.0
 * 通用:    pwm_backlight */
#define BRIGHTNESS_MAX_CANDIDATES 8

static struct {
    const char *brightness;
    const char *max;
} s_backlight_paths[BRIGHTNESS_MAX_CANDIDATES] = {
    /* RK3566 常见节点 */
    { "/sys/class/backlight/backlight/brightness",
      "/sys/class/backlight/backlight/max_brightness" },
    { "/sys/class/backlight/rk28_bl/brightness",
      "/sys/class/backlight/rk28_bl/max_brightness" },
    { "/sys/class/backlight/rk_backlight/brightness",
      "/sys/class/backlight/rk_backlight/max_brightness" },
    { "/sys/class/backlight/rockchip/brightness",
      "/sys/class/backlight/rockchip/max_brightness" },
    /* K230 常见节点 */
    { "/sys/class/backlight/pwm-backlight/brightness",
      "/sys/class/backlight/pwm-backlight/max_brightness" },
    { "/sys/class/backlight/k230-bl/brightness",
      "/sys/class/backlight/k230-bl/max_brightness" },
    { "/sys/class/backlight/backlight.0/brightness",
      "/sys/class/backlight/backlight.0/max_brightness" },
    /* 通用 PWM 背光 */
    { "/sys/class/backlight/pwm_backlight/brightness",
      "/sys/class/backlight/pwm_backlight/max_brightness" },
};

static int s_path_idx = -1;

static int brightness_path_detect(void)
{
    if (s_path_idx >= 0) return s_path_idx;

    // 允许通过环境变量覆盖背光节点，便于真机调试
    const char *env_path = getenv("APP_BACKLIGHT_BRIGHTNESS");
    if (env_path && env_path[0]) {
        s_backlight_paths[0].brightness = env_path;
        const char *env_max = getenv("APP_BACKLIGHT_MAX");
        if (env_max && env_max[0]) {
            /* 用户同时指定了 max 路径 */
            s_backlight_paths[0].max = env_max;
        } else {
            /* 用户只指定了 brightness 路径，自动推导 max 路径：
             * /sys/class/backlight/XXX/brightness → /sys/class/backlight/XXX/max_brightness
             * 确保两个路径指向同一设备，避免读取到错误的 max 值 */
            static char auto_max[128];
            const char *p = strrchr(env_path, '/');
            if (p) {
                /* 提取设备目录路径（不含 brightness 文件名） */
                size_t dir_len = (size_t)(p - env_path);
                if (dir_len + 32 < sizeof(auto_max)) {
                    memcpy(auto_max, env_path, dir_len);
                    memcpy(auto_max + dir_len, "/max_brightness", 16);
                    s_backlight_paths[0].max = auto_max;
                } else {
                    s_backlight_paths[0].max = "/sys/class/backlight/backlight/max_brightness";
                }
            } else {
                s_backlight_paths[0].max = "/sys/class/backlight/backlight/max_brightness";
            }
        }
        s_path_idx = 0;
        LV_LOG_USER("Brightness: using env path %s (max=%s)",
                    s_backlight_paths[0].brightness, s_backlight_paths[0].max);
        return s_path_idx;
    }

    /* 找到第一个存在的节点。
     * 注意：access(W_OK) 在非 root 用户下可能返回失败，但以 root 运行时
     * fopen("w") 仍可成功。因此这里只检查文件是否存在(F_OK)，
     * 实际写入是否可用由 brightness_write_raw 中的 fopen 判断。
     * 这样兼容 RK3566/K230 不同权限模型。 */
    for (int i = 0; i < BRIGHTNESS_MAX_CANDIDATES; i++) {
        if (access(s_backlight_paths[i].brightness, F_OK) == 0) {
            s_path_idx = i;
            LV_LOG_USER("Brightness: using %s (writable=%d)",
                        s_backlight_paths[i].brightness,
                        access(s_backlight_paths[i].brightness, W_OK) == 0);
            return s_path_idx;
        }
    }

    s_path_idx = 0;
    LV_LOG_USER("Brightness: no backlight node found, running in simulation mode");
    return s_path_idx;
}

static int brightness_read_max(void)
{
    int idx = brightness_path_detect();
    char buf[32] = {0};
    ui_read_file_line(s_backlight_paths[idx].max, buf, sizeof(buf));
    int max = atoi(buf);
    return (max > 0) ? max : 255;
}

static int brightness_read_raw(void)
{
    int idx = brightness_path_detect();
    char buf[32] = {0};
    ui_read_file_line(s_backlight_paths[idx].brightness, buf, sizeof(buf));
    return atoi(buf);
}

static void brightness_write_raw(int raw)
{
    int idx = brightness_path_detect();
    const char *path = s_backlight_paths[idx].brightness;

    FILE *f = fopen(path, "w");
    if (!f) {
        /* 打不开时可能是 PC 模拟或节点不存在，仅记录日志 */
        LV_LOG_USER("Brightness: simulate set %d to %s", raw, path);
        return;
    }

    if (fprintf(f, "%d\n", raw) < 0) {
        LV_LOG_WARN("Brightness: failed to write %d to %s", raw, path);
    }
    fclose(f);
    LV_LOG_USER("Brightness: set raw=%d to %s", raw, path);
}

static int brightness_pct_to_raw(int pct)
{
    int max = brightness_read_max();
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return (pct * max) / 100;
}

static int brightness_raw_to_pct(int raw)
{
    int max = brightness_read_max();
    if (max <= 0) return 0;
    if (raw < 0) raw = 0;
    if (raw > max) raw = max;
    return (raw * 100) / max;
}

/* 外部模块（如自动恢复）也可通过百分比设置亮度 */
void settings_brightness_set_pct(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    s_brightness_pct = pct;
    brightness_write_raw(brightness_pct_to_raw(pct));
}

/* ------------------------------------------------------------------ */
/* UI 刷新                                                            */
/* ------------------------------------------------------------------ */
static void brightness_refresh_labels(void)
{
    if (g_pct_label) {
        lv_label_set_text_fmt(g_pct_label, "%d%%", s_brightness_pct);
    }
}

static void brightness_sync_slider(void)
{
    if (g_slider) {
        lv_slider_set_value(g_slider, s_brightness_pct, LV_ANIM_OFF);
    }
    brightness_refresh_labels();
}

/* ------------------------------------------------------------------ */
/* 回调                                                                */
/* ------------------------------------------------------------------ */
static void close_cb(lv_event_t *e)
{
    (void)e;
    if (g_overlay) lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
}

/* × 按钮：关闭当前页 + 返回主屏幕 */
static void return_home_cb(lv_event_t *e)
{
    (void)e;
    if (g_overlay) lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
    app_return_home();
}

static void slider_changed_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    s_brightness_pct = lv_slider_get_value(slider);
    brightness_write_raw(brightness_pct_to_raw(s_brightness_pct));
    brightness_refresh_labels();
}

/* ------------------------------------------------------------------ */
/* UI 创建                                                            */
/* ------------------------------------------------------------------ */
/**
 * @brief 创建一个粗圆角滑动条
 *
 * 竖屏：纵向粗圆角条，太阳图标在底部（参考图片样式）。
 * 横屏：横向粗圆角条，太阳图标在左侧。
 */
static lv_obj_t *make_slider_with_icon(lv_obj_t *parent, bool vertical)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    int track_w, track_h, radius;

    if (vertical) {
        /* 竖屏：粗圆角纵向滑动条，宽度约占屏宽 1/4 */
        lv_obj_set_size(box, ui_scale_min(110), lv_pct(68));
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(box, 0, 0);

        track_w = ui_scale_min(90);
        track_h = lv_pct(100);
        radius  = track_w / 2;
    } else {
        /* 横屏：太阳图标在左，粗圆角横向滑动条在右；加大滑动区域防误触 */
        lv_obj_set_size(box, lv_pct(90), ui_scale_min(120));
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(box, ui_scale_min(18), 0);

        track_w = lv_pct(100);
        track_h = ui_scale_min(72);
        radius  = track_h / 2;
    }

    /* 滑动条容器 */
    lv_obj_t *track_box = lv_obj_create(box);
    lv_obj_set_size(track_box, track_w, track_h);
    lv_obj_set_style_bg_opa(track_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(track_box, 0, 0);
    lv_obj_set_style_pad_all(track_box, 0, 0);
    lv_obj_remove_flag(track_box, LV_OBJ_FLAG_SCROLLABLE);

    g_slider = lv_slider_create(track_box);
    lv_obj_set_size(g_slider, lv_pct(100), lv_pct(100));
    lv_slider_set_range(g_slider, 0, 100);

    /* 轨道：深色圆角 pill */
    lv_obj_set_style_bg_color(g_slider, lv_color_hex(0x2A2A2A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(g_slider, radius, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_slider, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_slider, 0, LV_PART_MAIN);

    /* 指示条：白色圆角 pill */
    lv_obj_set_style_bg_color(g_slider, lv_color_hex(COLOR_TEXT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g_slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(g_slider, radius, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(g_slider, 0, LV_PART_INDICATOR);

    /* Knob：白色小圆点 */
    lv_obj_set_style_bg_color(g_slider, lv_color_hex(COLOR_TEXT), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(g_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(g_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_border_width(g_slider, 0, LV_PART_KNOB);
    if (vertical) {
        lv_obj_set_style_size(g_slider, track_w - ui_scale_min(16),
                              track_w - ui_scale_min(16), LV_PART_KNOB);
    } else {
        lv_obj_set_style_size(g_slider, track_h - ui_scale_min(16),
                              track_h - ui_scale_min(16), LV_PART_KNOB);
    }

    lv_obj_add_event_cb(g_slider, slider_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* 太阳图标：竖屏在滑动条底部，横屏在左侧 */
    lv_obj_t *icon = lv_label_create(box);
    lv_label_set_text(icon, LV_SYMBOL_TINT);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(COLOR_TEXT), 0);

    if (vertical) {
        /* 竖屏：图标在轨道下方一点，与轨道水平居中 */
        lv_obj_set_style_pad_top(icon, ui_scale_min(10), 0);
    }

    return box;
}

void settings_brightness_create(void)
{
    g_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(g_overlay, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(g_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(g_overlay, 0, 0);
    lv_obj_set_style_border_width(g_overlay, 0, 0);
    lv_obj_remove_flag(g_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_flex_flow(g_overlay, LV_FLEX_FLOW_COLUMN);
    /* 交叉轴居中：宽屏下 body 限宽后水平居中 */
    lv_obj_set_flex_align(g_overlay,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    ui_make_header(g_overlay, tr("亮度", "Brightness"),
                   close_cb, NULL, return_home_cb, NULL);

    /* 横屏操作亮度条时右滑容易误触退出，因此取消右滑返回手势，
     * 统一通过顶部左上角返回按钮或右上角×返回。 */
    // ui_attach_back_gesture(g_overlay, close_cb);

    lv_obj_t *body = lv_obj_create(g_overlay);
    /* 宽屏限宽 720px 居中，窄屏全宽 */
    lv_obj_set_width(body, ui_get_content_max_width());
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_style_pad_all(body, ui_scale_min(16), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    const bool landscape = ui_is_landscape();

    if (landscape) {
        /* 横屏：百分比/标签在上方居中，宽大的横向滑动条在下方 */
        lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(body, ui_scale_min(28), 0);

        /* 百分比 + 亮度标签（上下排列） */
        lv_obj_t *info = lv_obj_create(body);
        lv_obj_set_size(info, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(info, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(info, ui_scale_min(6), 0);
        lv_obj_set_style_bg_opa(info, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(info, 0, 0);
        lv_obj_remove_flag(info, LV_OBJ_FLAG_SCROLLABLE);

        g_pct_label = lv_label_create(info);
        lv_obj_set_style_text_font(g_pct_label, &lv_font_montserrat_48, 0);
        lv_obj_set_style_text_color(g_pct_label, lv_color_hex(COLOR_TEXT), 0);

        lv_obj_t *name = lv_label_create(info);
        lv_label_set_text(name, tr("亮度", "Brightness"));
        lv_obj_set_style_text_font(name, ui_font_cjk(), 0);
        lv_obj_set_style_text_color(name, lv_color_hex(COLOR_SECONDARY), 0);

        make_slider_with_icon(body, false);
    } else {
        /* 竖屏：参考图片布局
         *      [ 返回 ]  [ 亮度 ]  [ × ]
         *          63%
         *        [====]
         *        [====]
         *        [    ]
         *        [ ☀  ]
         */
        lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(body, ui_scale_min(24), 0);

        /* 大字号百分比，居中 */
        g_pct_label = lv_label_create(body);
        lv_obj_set_style_text_font(g_pct_label, &lv_font_montserrat_48, 0);
        lv_obj_set_style_text_color(g_pct_label, lv_color_hex(COLOR_TEXT), 0);

        /* 纵向滑动条 + 底部太阳图标 */
        make_slider_with_icon(body, true);
    }

    brightness_sync_slider();
}

void settings_brightness_open(void)
{
    if (!g_overlay) settings_brightness_create();
    if (!g_overlay) return;

    /* 打开时重新读取系统亮度，避免被其他应用修改后显示不一致 */
    int raw = brightness_read_raw();
    s_brightness_pct = brightness_raw_to_pct(raw);
    brightness_sync_slider();

    ui_open_sub_page(g_overlay);
}

void settings_brightness_destroy(void)
{
    /* overlay 在 lv_layer_top() 上，旋转重建时必须删除避免内存泄漏 + 悬空指针 */
    if (g_overlay) {
        lv_obj_delete(g_overlay);
        g_overlay = NULL;
    }
    g_slider = NULL;
    g_pct_label = NULL;
}
