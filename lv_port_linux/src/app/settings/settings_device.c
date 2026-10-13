/**
 * @file settings_device.c
 * @brief 设备信息页面
 *
 * 显示字段（device.json 来自项目 inf.json 编译时合并）：
 *   1. 设备名称      - hostname
 *   2. 厂商          - manufacturer
 *   3. 设备型号      - device_model
 *   4. CMIIT ID      - 中国无线电核准号
 *   5. FCC ID        - 美国 FCC 认证号
 *   6. IC CAN        - 加拿大 IC 认证号
 *   7. ICES ID       - 加拿大 ICES 编号
 *   8. 设备序列号    - SN（规则生成：s + 硬件ID + model_id + 日期末位）
 *   9. 硬件序列号    - serial_number（板子硬件ID，回退 device-tree/MAC）
 *  10. 固件版本      - firmware_version（工程固件版本，非内核版本）
 *  11. 内核版本      - uname release（Linux 内核版本）
 *  12. 摄像头型号    - camera_model
 *  13. 相机固件版本  - camera_firmware_version（实际固件号）
 *  14. 快速入门指南  - 可点击展开
 */
#include "settings_device.h"
#include "../app_main.h"
#include "../common/ui_helpers.h"
#include "../common/app_i18n.h"
#include "../common/device_config.h"

#include "lvgl/lvgl.h"
#include <sys/utsname.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

static lv_obj_t *g_overlay;
static lv_obj_t *g_guide_overlay;  /* 快速入门指南子页面 */

/* info row label 句柄（对应 device_config_t 各字段） */
static lv_obj_t *g_dev_name;        /* 设备名称（hostname） */
static lv_obj_t *g_dev_manufacturer;/* 厂商 */
static lv_obj_t *g_dev_model;       /* 设备型号 */
static lv_obj_t *g_dev_cmiit;       /* CMIIT ID */
static lv_obj_t *g_dev_fcc;         /* FCC ID */
static lv_obj_t *g_dev_ic_can;      /* IC CAN */
static lv_obj_t *g_dev_ices;        /* ICES ID */
static lv_obj_t *g_dev_serial;      /* 设备序列号（SN，规则生成） */
static lv_obj_t *g_dev_hw_serial;   /* 硬件序列号（serial_number） */
static lv_obj_t *g_dev_fw_version;  /* 工程固件版本 */
static lv_obj_t *g_dev_kernel_ver;  /* 内核版本（uname） */
static lv_obj_t *g_dev_cam_model;   /* 摄像头型号 */
static lv_obj_t *g_dev_cam_fw;      /* 相机固件版本 */

/* ------------------------------------------------------------------ */
/* 快速入门指南子页面                                                  */
/* ------------------------------------------------------------------ */
static void guide_close_cb(lv_event_t *e)
{
    (void)e;
    if (g_guide_overlay) lv_obj_add_flag(g_guide_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void guide_return_home_cb(lv_event_t *e)
{
    (void)e;
    if (g_guide_overlay) lv_obj_add_flag(g_guide_overlay, LV_OBJ_FLAG_HIDDEN);
    app_return_home();
}

static void make_guide_overlay(void)
{
    g_guide_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_guide_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(g_guide_overlay, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(g_guide_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(g_guide_overlay, 0, 0);
    lv_obj_set_style_border_width(g_guide_overlay, 0, 0);
    lv_obj_remove_flag(g_guide_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_guide_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_flex_flow(g_guide_overlay, LV_FLEX_FLOW_COLUMN);
    /* 交叉轴居中：宽屏下 body 限宽后水平居中 */
    lv_obj_set_flex_align(g_guide_overlay,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    ui_make_header(g_guide_overlay, tr("快速入门指南", "Quick Start Guide"),
                   guide_close_cb, NULL, guide_return_home_cb, NULL);

    lv_obj_t *body = lv_obj_create(g_guide_overlay);
    lv_obj_set_width(body, ui_get_content_max_width());
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, ui_scale_min(16), 0);
    lv_obj_set_style_pad_gap(body, ui_scale_min(12), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);

    lv_obj_t *txt = lv_label_create(body);
    lv_label_set_text(txt,
        tr("1. 开机后进入拍摄主界面\n"
           "2. 上滑进入功能菜单\n"
           "3. 下滑进入快捷设置\n"
           "4. 右滑进入相册\n"
           "5. 点击模式图标切换拍摄模式\n"
           "6. 在设置中配置无线连接\n"
           "7. 长按预览区可快速跳转设置",
           "1. Power on to enter shooting UI\n"
           "2. Swipe up for function menu\n"
           "3. Swipe down for quick settings\n"
           "4. Swipe right for album\n"
           "5. Tap mode icon to switch mode\n"
           "6. Configure wireless in Settings\n"
           "7. Long press preview for settings"));
    lv_obj_set_style_text_font(txt, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(txt, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_width(txt, lv_pct(100));

    ui_attach_back_gesture(g_guide_overlay, guide_close_cb);
}

static void on_guide_card(lv_event_t *e)
{
    (void)e;
    if (!g_guide_overlay) make_guide_overlay();
    ui_open_sub_page(g_guide_overlay);
}

/* ------------------------------------------------------------------ */
/* 可点击的信息行（带右箭头）                                          */
/* ------------------------------------------------------------------ */
static lv_obj_t *make_clickable_row(lv_obj_t *parent, const char *label,
                                     lv_event_cb_t cb)
{
    int pad = ui_scale_min(12);
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, ui_scale_min(56));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_radius(row, ui_scale_min(10), 0);
    lv_obj_set_style_pad_all(row, pad, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    ui_attach_press_scale(row, 242);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COLOR_SECONDARY), 0);

    lv_obj_t *arrow = lv_label_create(row);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(arrow, ui_font_sym(), 0);
    lv_obj_set_style_text_color(arrow, lv_color_hex(COLOR_SECONDARY), 0);

    return row;
}

/* ------------------------------------------------------------------ */
/* 刷新设备信息                                                        */
/* ------------------------------------------------------------------ */

/* 设置 label 文本：若 val 非空则显示，否则显示 fallback 并标灰。
 * 缺失字段用次要颜色提示用户该信息未配置。 */
static void set_field_or_fallback(lv_obj_t *lbl, const char *val, const char *fallback)
{
    if (val && val[0]) {
        lv_label_set_text(lbl, val);
        lv_obj_set_style_text_color(lbl, lv_color_hex(COLOR_TEXT), 0);
    } else {
        lv_label_set_text(lbl, fallback);
        lv_obj_set_style_text_color(lbl, lv_color_hex(COLOR_SECONDARY), 0);
    }
}

static void refresh_ui(void)
{
    char buf[256];

    /* 加载 device.json 配置（已加载则返回缓存，不重复读文件） */
    const device_config_t *cfg = device_config_get();

    /* 1. 设备名称：device.json 无此字段，始终用 hostname */
    gethostname(buf, sizeof(buf));
    lv_label_set_text(g_dev_name, buf);

    /* 2~7. JSON 配置字段：缺失时显示占位提示 */
    set_field_or_fallback(g_dev_manufacturer, cfg->manufacturer,    tr("未配置", "Not set"));
    set_field_or_fallback(g_dev_model,        cfg->device_model,    tr("未配置", "Not set"));
    set_field_or_fallback(g_dev_cmiit,        cfg->cmiit_id,        tr("未配置", "Not set"));
    set_field_or_fallback(g_dev_fcc,          cfg->fcc_id,          tr("未配置", "Not set"));
    set_field_or_fallback(g_dev_ic_can,       cfg->ic_can,          tr("未配置", "Not set"));
    set_field_or_fallback(g_dev_ices,         cfg->ices_id,         tr("未配置", "Not set"));

    /* 8. 设备序列号 SN：运行时按规则生成 s + 硬件ID + model_id + 日期末位 */
    char sn_buf[128];
    device_config_generate_sn(sn_buf, sizeof(sn_buf));
    lv_label_set_text(g_dev_serial, sn_buf);
    lv_obj_set_style_text_color(g_dev_serial, lv_color_hex(COLOR_TEXT), 0);

    /* 9. 硬件序列号：优先 device.json 的 serial_number，回退 device-tree/MAC */
    if (cfg->serial_number && cfg->serial_number[0]) {
        lv_label_set_text(g_dev_hw_serial, cfg->serial_number);
        lv_obj_set_style_text_color(g_dev_hw_serial, lv_color_hex(COLOR_TEXT), 0);
    } else {
        ui_read_file_line("/proc/device-tree/serial-number", buf, sizeof(buf));
        if (!buf[0])
            ui_read_file_line("/sys/class/net/wlan0/address", buf, sizeof(buf));
        if (buf[0]) {
            lv_label_set_text(g_dev_hw_serial, buf);
            lv_obj_set_style_text_color(g_dev_hw_serial, lv_color_hex(COLOR_TEXT), 0);
        } else {
            lv_label_set_text(g_dev_hw_serial, tr("未检测到", "Not detected"));
            lv_obj_set_style_text_color(g_dev_hw_serial, lv_color_hex(COLOR_SECONDARY), 0);
        }
    }

    /* 10. 工程固件版本：从 device.json 读取（非内核版本），缺失显示未配置 */
    set_field_or_fallback(g_dev_fw_version, cfg->firmware_version, tr("未配置", "Not set"));

    /* 11. 内核版本：运行时 uname 获取（独立于工程固件版本） */
    {
        struct utsname u;
        if (uname(&u) == 0) {
            snprintf(buf, sizeof(buf), "%s %s", u.sysname, u.release);
            lv_label_set_text(g_dev_kernel_ver, buf);
            lv_obj_set_style_text_color(g_dev_kernel_ver, lv_color_hex(COLOR_TEXT), 0);
        } else {
            lv_label_set_text(g_dev_kernel_ver, tr("未知", "Unknown"));
            lv_obj_set_style_text_color(g_dev_kernel_ver, lv_color_hex(COLOR_SECONDARY), 0);
        }
    }

    /* 12~13. 摄像头信息 */
    set_field_or_fallback(g_dev_cam_model, cfg->camera_model,             "GC2093");
    set_field_or_fallback(g_dev_cam_fw,    cfg->camera_firmware_version, "V1.0.0");
}

/* ------------------------------------------------------------------ */
/* 回调                                                                */
/* ------------------------------------------------------------------ */
static void close_cb(lv_event_t *e)
{
    (void)e;
    if (g_overlay) lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void return_home_cb(lv_event_t *e)
{
    (void)e;
    if (g_overlay) lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
    app_return_home();
}

/* ------------------------------------------------------------------ */
/* 创建 / 打开                                                        */
/* ------------------------------------------------------------------ */
void settings_device_create(void)
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

    ui_make_header(g_overlay, tr("设备信息", "Device Info"),
                   close_cb, NULL, return_home_cb, NULL);

    lv_obj_t *body = lv_obj_create(g_overlay);
    /* 宽屏限宽 720px 居中，窄屏全宽 */
    lv_obj_set_width(body, ui_get_content_max_width());
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, ui_scale_min(12), 0);
    lv_obj_set_style_pad_gap(body, ui_scale_min(8), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    /* 字段较多（12 行 + 指南），允许纵向滚动查看 */
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);

    /* 宽屏下信息行双列排列：用 ROW_WRAP 容器，每行 48% 宽度 */
    const bool wide = ui_is_wide_layout();
    lv_obj_t *info_container = body;
    if (wide) {
        info_container = lv_obj_create(body);
        lv_obj_set_width(info_container, lv_pct(100));
        lv_obj_set_flex_flow(info_container, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(info_container, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_all(info_container, 0, 0);
        lv_obj_set_style_pad_gap(info_container, ui_scale_min(8), 0);
        lv_obj_set_style_bg_opa(info_container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(info_container, 0, 0);
        lv_obj_remove_flag(info_container, LV_OBJ_FLAG_SCROLLABLE);
    }

    /* 设备信息字段（顺序对应设备信息页注释中的编号） */
    g_dev_name        = ui_info_row(info_container, tr("设备名称",       "Device Name"));
    g_dev_manufacturer= ui_info_row(info_container, tr("厂商",           "Manufacturer"));
    g_dev_model       = ui_info_row(info_container, tr("设备型号",       "Device Model"));
    g_dev_cmiit       = ui_info_row(info_container, tr("CMIIT ID",       "CMIIT ID"));
    g_dev_fcc         = ui_info_row(info_container, tr("FCC ID",         "FCC ID"));
    g_dev_ic_can      = ui_info_row(info_container, tr("IC CAN",         "IC CAN"));
    g_dev_ices        = ui_info_row(info_container, tr("ICES ID",        "ICES ID"));
    g_dev_serial      = ui_info_row(info_container, tr("设备序列号",     "Device Serial"));
    g_dev_hw_serial   = ui_info_row(info_container, tr("硬件序列号",     "Hardware Serial"));
    g_dev_fw_version  = ui_info_row(info_container, tr("固件版本",       "Firmware Version"));
    g_dev_kernel_ver  = ui_info_row(info_container, tr("内核版本",       "Kernel Version"));
    g_dev_cam_model   = ui_info_row(info_container, tr("摄像头型号",     "Camera Model"));
    g_dev_cam_fw      = ui_info_row(info_container, tr("相机固件版本",   "Camera Firmware"));

    /* 宽屏双列模式下，信息行（row 容器）宽度设为 48%。
     * 注意：ui_info_row 返回的是 value label，其父对象才是 row 容器。 */
    if (wide) {
        lv_obj_t *vals[] = {g_dev_name, g_dev_manufacturer, g_dev_model, g_dev_cmiit,
                            g_dev_fcc, g_dev_ic_can, g_dev_ices,
                            g_dev_serial, g_dev_hw_serial, g_dev_fw_version,
                            g_dev_kernel_ver, g_dev_cam_model, g_dev_cam_fw};
        for (size_t i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
            if (vals[i]) {
                lv_obj_t *row = lv_obj_get_parent(vals[i]);
                if (row) lv_obj_set_width(row, lv_pct(48));
            }
        }
    }

    /* 快速入门指南：可点击行（始终全宽） */
    make_clickable_row(body, tr("快速入门指南", "Quick Start Guide"), on_guide_card);

    ui_attach_back_gesture(g_overlay, close_cb);
}

void settings_device_open(void)
{
    refresh_ui();
    ui_open_sub_page(g_overlay);
}

void settings_device_destroy(void)
{
    /* guide_overlay 在 layer_top 上，旋转重建时需清理 */
    if (g_guide_overlay) {
        lv_obj_delete(g_guide_overlay);
        g_guide_overlay = NULL;
    }
    /* g_overlay 也在 layer_top 上，需一并清理避免悬空指针 */
    if (g_overlay) {
        lv_obj_delete(g_overlay);
        g_overlay = NULL;
    }
    g_dev_name         = NULL;
    g_dev_manufacturer = NULL;
    g_dev_model        = NULL;
    g_dev_cmiit        = NULL;
    g_dev_fcc          = NULL;
    g_dev_ic_can       = NULL;
    g_dev_ices         = NULL;
    g_dev_serial       = NULL;
    g_dev_hw_serial    = NULL;
    g_dev_fw_version   = NULL;
    g_dev_kernel_ver   = NULL;
    g_dev_cam_model    = NULL;
    g_dev_cam_fw       = NULL;
}
