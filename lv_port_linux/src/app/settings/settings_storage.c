/**
 * @file settings_storage.c
 * @brief 存储设置页
 *
 * 功能：
 *   - 存储选择页：内置存储 / SD 卡 / 存储管理
 *     · 点击内置/SD 卡切换拍摄素材保存位置，并高亮显示
 *     · 卡片中间用圆环显示已用/总容量，圆环中心显示容量文字
 *   - 存储管理二级菜单：格式化内置存储 / 格式化 SD 卡（带确认弹窗）
 *   - 支持横竖屏自适应
 *   - 选中位置持久化到 storage.cfg
 */
#include "settings_storage.h"
#include "../app_main.h"
#include "../common/ui_helpers.h"
#include "../common/app_i18n.h"
#include "../common/board_detect.h"  /* 板子型号检测：区分 RK3566/K230 内置存储路径 */

#include "lvgl/lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/statvfs.h>

/* ------------------------------------------------------------------ */
/* 静态句柄                                                            */
/* ------------------------------------------------------------------ */
static lv_obj_t *g_overlay;          /* 存储选择页 overlay */
static lv_obj_t *g_manage_overlay;   /* 存储管理二级菜单 overlay */
static lv_obj_t *g_dlg_overlay;      /* 确认弹窗 overlay */

static lv_obj_t *g_arc_int;          /* 内置存储圆环 */
static lv_obj_t *g_arc_sd;           /* SD 卡圆环 */
static lv_obj_t *g_cap_int;          /* 内置存储容量文字 */
static lv_obj_t *g_cap_sd;           /* SD 卡容量文字 */
static lv_obj_t *g_card_int;         /* 内置存储卡片 */
static lv_obj_t *g_card_sd;          /* SD 卡卡片 */

/* 当前选中的存储：0=内置，1=SD 卡 */
static int s_sel_storage = 0;

/* 弹窗上下文：0=格式化内置，1=格式化 SD 卡 */
static int s_dlg_target = 0;

/* ------------------------------------------------------------------ */
/* 持久化                                                              */
/* ------------------------------------------------------------------ */
#define STORAGE_CFG_FILE "storage.cfg"

static void storage_cfg_save(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", app_get_app_dir(), STORAGE_CFG_FILE);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%d\n", s_sel_storage);
        fclose(f);
        LV_LOG_USER("Storage selection saved: %d (%s)",
                    s_sel_storage, s_sel_storage ? "SD" : "Internal");
    } else {
        LV_LOG_WARN("Failed to save storage cfg to %s", path);
    }
}

static void storage_cfg_load(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", app_get_app_dir(), STORAGE_CFG_FILE);
    FILE *f = fopen(path, "r");
    if (f) {
        int val = 0;
        if (fscanf(f, "%d", &val) == 1) {
            s_sel_storage = val ? 1 : 0;
            LV_LOG_USER("Storage selection loaded: %d", s_sel_storage);
        }
        fclose(f);
    }
}

/* 外部模块获取当前保存位置：返回 0=内置，1=SD 卡 */
int settings_storage_get_sel(void)
{
    return s_sel_storage;
}

/* 获取内置存储根路径：根据板子型号返回不同路径。
 * RK3566：/userdata（独立数据分区）
 * K230：/root（root 用户家目录，DCIM 落在 /root/DCIM）
 * 未知板子：/userdata（保守回退） */
static const char * get_internal_base_path(void)
{
    board_type_t board = board_detect();
    if (board == BOARD_K230) {
        return "/root";  /* K230 root 家目录：DCIM/photos 落在 /root/DCIM */
    }
    return "/userdata";  /* RK3566 及未知板子使用 /userdata */
}

/* 外部模块获取当前保存路径 */
const char * settings_storage_get_path(void)
{
    if (s_sel_storage) {
        static char sd_path[128] = {0};
        long long t, u;
        if (ui_detect_sd_card(&t, &u, sd_path, sizeof(sd_path)) && sd_path[0]) {
            return sd_path;
        }
        return "/mnt/sdcard"; /* 回退 */
    }
    return get_internal_base_path(); /* 内置存储：根据板子型号返回 */
}

/* ------------------------------------------------------------------ */
/* 容量读取                                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    long long total_mb;
    long long used_mb;
    long long free_mb;
    int       ok;
} storage_info_t;

static void read_storage_info(const char *path, storage_info_t *info)
{
    struct statvfs st;
    info->ok = 0;
    info->total_mb = 0;
    info->used_mb  = 0;
    info->free_mb  = 0;
    if (!path || statvfs(path, &st) != 0) return;

    unsigned long long total = (unsigned long long)st.f_blocks * st.f_frsize;
    unsigned long long free  = (unsigned long long)st.f_bfree  * st.f_frsize;
    if (total == 0) return;

    info->total_mb = (long long)(total / (1024 * 1024));
    info->free_mb  = (long long)(free  / (1024 * 1024));
    info->used_mb  = info->total_mb - info->free_mb;
    info->ok = 1;
}

static void detect_internal(storage_info_t *info)
{
    /* 根据板子型号检测内置存储：
     * RK3566：优先 /userdata，回退到 /
     * K230：直接检测 /root（root 家目录即内置存储） */
    const char *base = get_internal_base_path();
    read_storage_info(base, info);
    if (!info->ok && strcmp(base, "/") != 0) {
        /* RK3566 的 /userdata 不存在时回退到根分区 */
        read_storage_info("/", info);
    }
}

static void detect_sd(storage_info_t *info)
{
    char path[128] = {0};
    long long t, u;
    if (ui_detect_sd_card(&t, &u, path, sizeof(path)) && path[0]) {
        read_storage_info(path, info);
    } else {
        info->ok = 0;
    }
}

/* 将 MB 转为人类可读字符串（GB/TB） */
static void fmt_size(char *buf, size_t sz, long long mb)
{
    if (mb >= 1024 * 1024) {
        snprintf(buf, sz, "%.2fTB", mb / 1024.0 / 1024.0);
    } else if (mb >= 1024) {
        snprintf(buf, sz, "%.1fGB", mb / 1024.0);
    } else {
        snprintf(buf, sz, "%.0fMB", (double)mb);
    }
}

/* ------------------------------------------------------------------ */
/* UI 刷新                                                             */
/* ------------------------------------------------------------------ */
static void refresh_storage_card(lv_obj_t *arc, lv_obj_t *cap,
                                 const storage_info_t *info, int is_full_check)
{
    if (!arc || !cap) return;

    if (info->ok) {
        int pct = info->total_mb ? (int)(info->used_mb * 100 / info->total_mb) : 0;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        lv_arc_set_value(arc, pct);

        char used[32], total[32];
        fmt_size(used, sizeof(used), info->used_mb);
        fmt_size(total, sizeof(total), info->total_mb);

        if (is_full_check && info->free_mb < 100) { /* < 100MB 视为 Full */
            lv_obj_set_style_text_color(cap, lv_color_hex(COLOR_ACCENT), 0);
            lv_label_set_text_fmt(cap, "Full\n%s", total);
        } else {
            lv_obj_set_style_text_color(cap, lv_color_hex(COLOR_TEXT), 0);
            lv_label_set_text_fmt(cap, "%s\n%s", used, total);
        }
    } else {
        lv_arc_set_value(arc, 0);
        lv_obj_set_style_text_color(cap, lv_color_hex(COLOR_SECONDARY), 0);
        lv_label_set_text(cap, tr("未插入", "N/A"));
    }
}

static void refresh_ui(void)
{
    storage_info_t info_int = {0};
    storage_info_t info_sd  = {0};
    detect_internal(&info_int);
    detect_sd(&info_sd);

    refresh_storage_card(g_arc_int, g_cap_int, &info_int, 1);
    refresh_storage_card(g_arc_sd,  g_cap_sd,  &info_sd,  0);

    /* 高亮当前选中卡片 */
    if (g_card_int && g_card_sd) {
        lv_obj_set_style_border_color(g_card_int,
            s_sel_storage ? lv_color_hex(0x333333) : lv_color_hex(COLOR_YELLOW_BD), 0);
        lv_obj_set_style_bg_color(g_card_int,
            s_sel_storage ? lv_color_hex(COLOR_PANEL) : lv_color_hex(COLOR_YELLOW), 0);

        lv_obj_set_style_border_color(g_card_sd,
            s_sel_storage ? lv_color_hex(COLOR_YELLOW_BD) : lv_color_hex(0x333333), 0);
        lv_obj_set_style_bg_color(g_card_sd,
            s_sel_storage ? lv_color_hex(COLOR_YELLOW) : lv_color_hex(COLOR_PANEL), 0);
    }
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

static void on_sel_internal(lv_event_t *e)
{
    (void)e;
    s_sel_storage = 0;
    storage_cfg_save();
    refresh_ui();
}

static void on_sel_sd(lv_event_t *e)
{
    (void)e;
    s_sel_storage = 1;
    storage_cfg_save();
    refresh_ui();
}

static void on_open_manage(lv_event_t *e)
{
    (void)e;
    if (!g_manage_overlay) return;
    lv_obj_remove_flag(g_manage_overlay, LV_OBJ_FLAG_HIDDEN);
    ui_open_sub_page(g_manage_overlay);
}

/* ------------------------------------------------------------------ */
/* 存储管理二级菜单                                                    */
/* ------------------------------------------------------------------ */
static void manage_close_cb(lv_event_t *e)
{
    (void)e;
    if (g_manage_overlay) lv_obj_add_flag(g_manage_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void manage_return_home_cb(lv_event_t *e)
{
    (void)e;
    if (g_manage_overlay) lv_obj_add_flag(g_manage_overlay, LV_OBJ_FLAG_HIDDEN);
    if (g_overlay)        lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
    app_return_home();
}

static void show_confirm_dialog(int target);

static void on_format_internal(lv_event_t *e)
{
    (void)e;
    show_confirm_dialog(0);
}

static void on_format_sd(lv_event_t *e)
{
    (void)e;
    show_confirm_dialog(1);
}

/* 实际格式化操作：
 * - 内置存储：清理 DCIM 目录下的照片和视频（保留配置文件），不格式化文件系统
 * - SD 卡：使用 mkfs.vfat 格式化整个分区（需先卸载） */
static void do_format_internal(void)
{
    LV_LOG_USER("格式化内置存储：清理 DCIM 目录");
    /* 删除内置存储 DCIM 目录下的所有照片和视频文件
     * 保留 .cfg 配置文件和其他系统文件。
     * 路径根据板子型号选择：RK3566=/userdata/DCIM，K230=/root/DCIM */
    const char *base = get_internal_base_path();
    char cmd[256];
    /* 统一拼路径：base 为 /userdata 或 /root，rm -rf base/DCIM/*
     * base 来自板子检测的静态字符串，不存在命令注入风险 */
    snprintf(cmd, sizeof(cmd), "rm -rf %s/DCIM/* 2>/dev/null", base);
    int ret = system(cmd);
    if (ret != 0) {
        LV_LOG_WARN("内置存储清理返回非零值: %d（可能部分文件被占用）", ret);
    } else {
        LV_LOG_USER("内置存储 DCIM 目录已清理: %s", cmd);
    }
}

static void do_format_sd(void)
{
    LV_LOG_USER("格式化 SD 卡");
    /* SD 卡格式化流程：
     * 1. 卸载 SD 卡（umount）
     * 2. 格式化为 FAT32（mkfs.vfat）
     * 3. 重新挂载（挂载成功格式化的那个设备节点）
     * 注意：使用 system() 调用，目标板需有 mkfs.vfat 工具
     * SD 卡设备节点通常为 /dev/mmcblk0p1 或 /dev/mmcblk1p1
     * 格式化是危险操作，此处仅处理已知设备节点，避免误格式化其他设备 */
    int ret;
    const char *formatted_dev = NULL;  /* 记录成功格式化的设备节点 */

    /* 先尝试卸载（可能未挂载，忽略错误） */
    system("umount /mnt/sdcard 2>/dev/null");

    /* 尝试格式化已知 SD 卡设备节点（mmcblk0p1 或 mmcblk1p1） */
    ret = system("mkfs.vfat -F 32 /dev/mmcblk0p1 2>/dev/null");
    if (ret == 0) {
        formatted_dev = "/dev/mmcblk0p1";
    } else {
        LV_LOG_WARN("mmcblk0p1 格式化失败，尝试 mmcblk1p1");
        ret = system("mkfs.vfat -F 32 /dev/mmcblk1p1 2>/dev/null");
        if (ret == 0) {
            formatted_dev = "/dev/mmcblk1p1";
        }
    }

    if (!formatted_dev) {
        LV_LOG_ERROR("SD 卡格式化失败（返回 %d），可能未插入 SD 卡或设备节点不匹配", ret);
    } else {
        LV_LOG_USER("SD 卡已格式化为 FAT32: %s", formatted_dev);

        /* 重新挂载：优先挂载刚成功格式化的设备节点，避免挂载到错误分区 */
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "mount -t vfat %s /mnt/sdcard 2>/dev/null", formatted_dev);
        ret = system(cmd);
        if (ret != 0) {
            /* 回退：尝试另一个设备节点（可能格式化的设备未挂载成功，但另一个已存在文件系统） */
            const char *alt = (strcmp(formatted_dev, "/dev/mmcblk0p1") == 0) ?
                              "/dev/mmcblk1p1" : "/dev/mmcblk0p1";
            snprintf(cmd, sizeof(cmd), "mount -t vfat %s /mnt/sdcard 2>/dev/null", alt);
            system(cmd);
        }

        /* 创建 DCIM 目录 */
        system("mkdir -p /mnt/sdcard/DCIM 2>/dev/null");
        LV_LOG_USER("SD 卡已重新挂载并创建 DCIM 目录");
    }
}

/* 确认弹窗回调 */
static void dlg_close_cb(lv_event_t *e)
{
    (void)e;
    if (g_dlg_overlay) lv_obj_add_flag(g_dlg_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void dlg_confirm_cb(lv_event_t *e)
{
    (void)e;
    if (s_dlg_target == 0) {
        do_format_internal();
    } else {
        do_format_sd();
    }
    if (g_dlg_overlay) lv_obj_add_flag(g_dlg_overlay, LV_OBJ_FLAG_HIDDEN);
    /* 格式化后刷新容量显示 */
    refresh_ui();
}

/* ------------------------------------------------------------------ */
/* 确认弹窗                                                            */
/* ------------------------------------------------------------------ */
static void show_confirm_dialog(int target)
{
    s_dlg_target = target;
    if (!g_dlg_overlay) return;

    lv_obj_t *box = lv_obj_get_child(g_dlg_overlay, 0);
    if (!box) return;

    lv_obj_t *msg = lv_obj_get_child(box, 0);
    if (msg) {
        lv_label_set_text_fmt(msg, "%s %s?",
            tr("确认格式化", "Confirm format"),
            target ? tr("SD 卡", "SD Card") : tr("内置存储", "Internal"));
    }

    lv_obj_remove_flag(g_dlg_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void create_confirm_dialog(void)
{
    g_dlg_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_dlg_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(g_dlg_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_dlg_overlay, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(g_dlg_overlay, 0, 0);
    lv_obj_set_style_border_width(g_dlg_overlay, 0, 0);
    lv_obj_remove_flag(g_dlg_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_dlg_overlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *box = lv_obj_create(g_dlg_overlay);
    lv_obj_set_size(box, ui_scale_min(280), LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_radius(box, ui_scale_min(16), 0);
    lv_obj_set_style_pad_all(box, ui_scale_min(20), 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *msg = lv_label_create(box);
    lv_label_set_text(msg, tr("确认格式化?", "Confirm format?"));
    lv_obj_set_style_text_font(msg, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_width(msg, lv_pct(100));
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *btns = lv_obj_create(box);
    lv_obj_set_size(btns, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(btns, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btns, 0, 0);
    lv_obj_set_style_pad_top(btns, ui_scale_min(16), 0);
    lv_obj_remove_flag(btns, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *btn_cancel = lv_button_create(btns);
    lv_obj_set_size(btn_cancel, ui_scale_min(100), ui_scale_min(44));
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(btn_cancel, ui_scale_min(10), 0);
    lv_obj_add_event_cb(btn_cancel, dlg_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lc = lv_label_create(btn_cancel);
    lv_label_set_text(lc, tr("取消", "Cancel"));
    lv_obj_set_style_text_font(lc, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(lc, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_center(lc);

    lv_obj_t *btn_ok = lv_button_create(btns);
    lv_obj_set_size(btn_ok, ui_scale_min(100), ui_scale_min(44));
    lv_obj_set_style_bg_color(btn_ok, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_radius(btn_ok, ui_scale_min(10), 0);
    lv_obj_add_event_cb(btn_ok, dlg_confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lo = lv_label_create(btn_ok);
    lv_label_set_text(lo, tr("确定", "OK"));
    lv_obj_set_style_text_font(lo, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(lo, lv_color_hex(0x000000), 0);
    lv_obj_center(lo);
}

/* ------------------------------------------------------------------ */
/* 通用卡片创建                                                        */
/* ------------------------------------------------------------------ */
static lv_obj_t *make_storage_card(lv_obj_t *parent, const char *title,
                                   lv_obj_t **out_arc, lv_obj_t **out_cap,
                                   lv_event_cb_t cb)
{
    lv_obj_t *card = lv_button_create(parent);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_radius(card, ui_scale_min(14), 0);
    lv_obj_set_style_border_width(card, ui_scale_min(3), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x333333), 0);
    lv_obj_set_style_pad_all(card, ui_scale_min(10), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    ui_attach_press_scale(card, 242);
    lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, NULL);

    /* 标题 */
    lv_obj_t *tl = lv_label_create(card);
    lv_label_set_text(tl, title);
    lv_obj_set_style_text_font(tl, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(tl, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_width(tl, lv_pct(100));
    lv_obj_set_style_text_align(tl, LV_TEXT_ALIGN_CENTER, 0);

    /* 圆环容器 */
    int ring_sz = ui_scale_min(120);
    lv_obj_t *arc_box = lv_obj_create(card);
    lv_obj_set_size(arc_box, ring_sz, ring_sz);
    lv_obj_set_style_bg_opa(arc_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(arc_box, 0, 0);
    lv_obj_remove_flag(arc_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *arc = lv_arc_create(arc_box);
    lv_obj_set_size(arc, ring_sz - ui_scale_min(10), ring_sz - ui_scale_min(10));
    lv_obj_center(arc);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 0);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_obj_set_style_arc_color(arc, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, ui_scale_min(10), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, ui_scale_min(10), LV_PART_MAIN);
    /* 去掉旋钮 */
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);

    /* 容量文字居中 */
    lv_obj_t *cap = lv_label_create(arc_box);
    lv_obj_set_style_text_font(cap, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_width(cap, ring_sz - ui_scale_min(20));
    lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(cap, LV_LABEL_LONG_DOT);
    lv_obj_center(cap);

    *out_arc = arc;
    *out_cap = cap;
    return card;
}

/* ------------------------------------------------------------------ */
/* 存储选择页创建                                                      */
/* ------------------------------------------------------------------ */
void settings_storage_create(void)
{
    storage_cfg_load();

    /* ---- 主 overlay：存储选择 ---- */
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

    ui_make_header(g_overlay, tr("存储", "Storage"),
                   close_cb, NULL, return_home_cb, NULL);

    /* 右滑返回上一级（overlay 自身移除 GESTURE_BUBBLE，由 ui_attach_back_gesture 处理） */
    ui_attach_back_gesture(g_overlay, close_cb);

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
        /* 横屏：3 张卡片排成一行，等高 */
        lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(body, LV_FLEX_ALIGN_SPACE_EVENLY,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(body, ui_scale_min(12), 0);

        g_card_int = make_storage_card(body, tr("内置存储", "Internal"),
                                       &g_arc_int, &g_cap_int, on_sel_internal);
        lv_obj_set_flex_grow(g_card_int, 1);
        lv_obj_set_height(g_card_int, lv_pct(88));

        g_card_sd = make_storage_card(body, tr("SD 卡", "SD Card"),
                                      &g_arc_sd, &g_cap_sd, on_sel_sd);
        lv_obj_set_flex_grow(g_card_sd, 1);
        lv_obj_set_height(g_card_sd, lv_pct(88));

        lv_obj_t *card_manage = lv_button_create(body);
        lv_obj_set_flex_grow(card_manage, 1);
        lv_obj_set_height(card_manage, lv_pct(88));
        lv_obj_set_style_bg_color(card_manage, lv_color_hex(COLOR_PANEL), 0);
        lv_obj_set_style_radius(card_manage, ui_scale_min(14), 0);
        lv_obj_set_style_border_width(card_manage, ui_scale_min(3), 0);
        lv_obj_set_style_border_color(card_manage, lv_color_hex(0x333333), 0);
        lv_obj_set_flex_flow(card_manage, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card_manage, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(card_manage, LV_OBJ_FLAG_SCROLLABLE);
        ui_attach_press_scale(card_manage, 242);
        lv_obj_add_event_cb(card_manage, on_open_manage, LV_EVENT_CLICKED, NULL);

        lv_obj_t *lm = lv_label_create(card_manage);
        lv_label_set_text(lm, tr("存储管理", "Manage"));
        lv_obj_set_style_text_font(lm, ui_font_cjk(), 0);
        lv_obj_set_style_text_color(lm, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_set_width(lm, lv_pct(100));
        lv_obj_set_style_text_align(lm, LV_TEXT_ALIGN_CENTER, 0);
    } else {
        /* 竖屏：内置/SD 在上排（占 55% 高度），存储管理在下（占 35% 高度） */
        lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(body, ui_scale_min(16), 0);

        lv_obj_t *top = lv_obj_create(body);
        lv_obj_set_size(top, lv_pct(100), lv_pct(55));
        lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_EVENLY,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(top, ui_scale_min(12), 0);
        lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(top, 0, 0);
        lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);

        g_card_int = make_storage_card(top, tr("内置存储", "Internal"),
                                       &g_arc_int, &g_cap_int, on_sel_internal);
        lv_obj_set_size(g_card_int, lv_pct(48), lv_pct(100));

        g_card_sd = make_storage_card(top, tr("SD 卡", "SD Card"),
                                      &g_arc_sd, &g_cap_sd, on_sel_sd);
        lv_obj_set_size(g_card_sd, lv_pct(48), lv_pct(100));

        lv_obj_t *card_manage = lv_button_create(body);
        lv_obj_set_size(card_manage, lv_pct(100), lv_pct(35));
        lv_obj_set_style_bg_color(card_manage, lv_color_hex(COLOR_PANEL), 0);
        lv_obj_set_style_radius(card_manage, ui_scale_min(14), 0);
        lv_obj_set_style_border_width(card_manage, ui_scale_min(3), 0);
        lv_obj_set_style_border_color(card_manage, lv_color_hex(0x333333), 0);
        lv_obj_set_flex_flow(card_manage, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card_manage, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(card_manage, LV_OBJ_FLAG_SCROLLABLE);
        ui_attach_press_scale(card_manage, 242);
        lv_obj_add_event_cb(card_manage, on_open_manage, LV_EVENT_CLICKED, NULL);

        lv_obj_t *lm = lv_label_create(card_manage);
        lv_label_set_text(lm, tr("存储管理", "Manage"));
        lv_obj_set_style_text_font(lm, ui_font_cjk(), 0);
        lv_obj_set_style_text_color(lm, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_set_width(lm, lv_pct(100));
        lv_obj_set_style_text_align(lm, LV_TEXT_ALIGN_CENTER, 0);
    }

    /* ---- 存储管理二级菜单 overlay ---- */
    g_manage_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_manage_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(g_manage_overlay, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(g_manage_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(g_manage_overlay, 0, 0);
    lv_obj_set_style_border_width(g_manage_overlay, 0, 0);
    lv_obj_remove_flag(g_manage_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_manage_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_flex_flow(g_manage_overlay, LV_FLEX_FLOW_COLUMN);
    /* 交叉轴居中：宽屏下 body 限宽后水平居中 */
    lv_obj_set_flex_align(g_manage_overlay,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    ui_make_header(g_manage_overlay, tr("存储管理", "Storage Manage"),
                   manage_close_cb, NULL, manage_return_home_cb, NULL);

    ui_attach_back_gesture(g_manage_overlay, manage_close_cb);

    lv_obj_t *m_body = lv_obj_create(g_manage_overlay);
    /* 宽屏限宽 720px 居中，窄屏全宽 */
    lv_obj_set_width(m_body, ui_get_content_max_width());
    lv_obj_set_flex_grow(m_body, 1);
    lv_obj_set_flex_flow(m_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(m_body, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(m_body, ui_scale_min(16), 0);
    lv_obj_set_style_pad_gap(m_body, ui_scale_min(12), 0);
    lv_obj_set_style_bg_opa(m_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m_body, 0, 0);
    lv_obj_remove_flag(m_body, LV_OBJ_FLAG_SCROLLABLE);

    {
        lv_obj_t *btn = lv_button_create(m_body);
        lv_obj_set_size(btn, lv_pct(100), ui_scale_min(56));
        lv_obj_set_style_bg_color(btn, lv_color_hex(COLOR_PANEL), 0);
        lv_obj_set_style_radius(btn, ui_scale_min(12), 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(btn, ui_scale_min(16), 0);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        ui_attach_press_scale(btn, 242);
        lv_obj_add_event_cb(btn, on_format_internal, LV_EVENT_CLICKED, NULL);

        lv_obj_t *l = lv_label_create(btn);
        lv_label_set_text(l, tr("格式化内置存储", "Format Internal"));
        lv_obj_set_style_text_font(l, ui_font_cjk(), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(COLOR_TEXT), 0);

        lv_obj_t *arrow = lv_label_create(btn);
        lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_font(arrow, ui_font_sym(), 0);
        lv_obj_set_style_text_color(arrow, lv_color_hex(COLOR_SECONDARY), 0);
    }

    {
        lv_obj_t *btn = lv_button_create(m_body);
        lv_obj_set_size(btn, lv_pct(100), ui_scale_min(56));
        lv_obj_set_style_bg_color(btn, lv_color_hex(COLOR_PANEL), 0);
        lv_obj_set_style_radius(btn, ui_scale_min(12), 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(btn, ui_scale_min(16), 0);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        ui_attach_press_scale(btn, 242);
        lv_obj_add_event_cb(btn, on_format_sd, LV_EVENT_CLICKED, NULL);

        lv_obj_t *l = lv_label_create(btn);
        lv_label_set_text(l, tr("格式化 SD 卡", "Format SD Card"));
        lv_obj_set_style_text_font(l, ui_font_cjk(), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(COLOR_TEXT), 0);

        lv_obj_t *arrow = lv_label_create(btn);
        lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_font(arrow, ui_font_sym(), 0);
        lv_obj_set_style_text_color(arrow, lv_color_hex(COLOR_SECONDARY), 0);
    }

    /* ---- 确认弹窗 ---- */
    create_confirm_dialog();

    refresh_ui();
}

void settings_storage_open(void)
{
    if (!g_overlay) settings_storage_create();
    if (!g_overlay) return;
    refresh_ui();
    ui_open_sub_page(g_overlay);
}

void settings_storage_destroy(void)
{
    if (g_overlay) {
        lv_obj_delete(g_overlay);
        g_overlay = NULL;
    }
    if (g_manage_overlay) {
        lv_obj_delete(g_manage_overlay);
        g_manage_overlay = NULL;
    }
    if (g_dlg_overlay) {
        lv_obj_delete(g_dlg_overlay);
        g_dlg_overlay = NULL;
    }
    g_arc_int = NULL;
    g_arc_sd  = NULL;
    g_cap_int = NULL;
    g_cap_sd  = NULL;
    g_card_int = NULL;
    g_card_sd  = NULL;
}
