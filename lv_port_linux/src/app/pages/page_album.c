/**
 * @file page_album.c
 * @brief 相册查看页面
 *
 * 布局：
 *   - 顶部工具栏：排列切换按钮 + 照片计数
 *   - 主体：照片缩略图网格（3列小图 / 2列大图切换）
 *   - 全屏查看：点击缩略图 → 全屏图片 + 左上角浮动返回箭头
 *
 * 手势：
 *   - tileview 原生右滑(LV_DIR_RIGHT)回主页
 *   - 全屏查看时右滑关闭全屏
 */
#include "page_album.h"
#include "../app_main.h"
#include "../common/ui_helpers.h"
#include "../common/app_i18n.h"
#include "../common/video_thumb.h"         /* 视频缩略图提取 */
#include "../common/video_player.h"        /* 全屏视频播放 */
#include "../common/album_image_cache.h"   /* 大图降采样缓存 */
#include "../settings/settings_storage.h"  /* 存储路径选择 */
#include "../common/board_detect.h"        /* 板子型号检测：区分扫描路径 */

#include "lvgl/lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>   /* errno/strerror：删除文件错误信息 */
#include <time.h>    /* localtime/strftime：文件修改时间显示 */

/* ------------------------------------------------------------------ */
/* 配置                                                                */
/* ------------------------------------------------------------------ */
#define ALBUM_MAX_PHOTOS  64
#define ALBUM_PATH_LEN    256

/* 动态构建扫描目录列表（在 refresh_photo_list 中填充）。
 * 包含用户选择的存储位置 + 所有板子的常见路径，确保拍照/录制后能立即在相册看到。
 * 容量 6：base/DCIM + /userdata/DCIM + /root/DCIM + /DCIM + /mnt/sdcard/DCIM + ./photos */
static char s_scan_dirs[6][ALBUM_PATH_LEN];

/* ------------------------------------------------------------------ */
/* 状态                                                                */
/* ------------------------------------------------------------------ */
static lv_obj_t *g_tile;
static lv_obj_t *g_grid;          /* 缩略图网格容器 */
static lv_obj_t *g_count_label;   /* 照片计数标签 */
static lv_obj_t *g_arrange_btn;   /* 排列切换按钮 */
static lv_obj_t *g_fullscreen;        /* 全屏查看 overlay */
static lv_obj_t *g_full_img;          /* 全屏图片显示 */
static lv_obj_t *g_video_placeholder; /* 全屏视频占位提示 */
static lv_obj_t *g_context_menu;      /* 长按弹出的二级菜单 overlay */
static int g_arrange_mode = 0;    /* 0=3列小图, 1=2列大图 */
static int g_context_idx = -1;    /* 长按菜单当前操作的文件索引 */
static int g_ignore_next_click = 0; /* 长按触发后，忽略紧随其后的 CLICK 事件 */

/* 媒体文件路径列表（图片+视频） */
static char  g_photos[ALBUM_MAX_PHOTOS][ALBUM_PATH_LEN];
static int   g_photo_count = 0;

/* 判断文件是否为视频（通过扩展名） */
static int is_video_file(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return 0;
    return (strcasecmp(dot, ".mp4") == 0 ||
            strcasecmp(dot, ".mov") == 0 ||
            strcasecmp(dot, ".avi") == 0 ||
            strcasecmp(dot, ".mkv") == 0 ||
            strcasecmp(dot, ".ts") == 0);
}

/* 是否加载真实图片缩略图。
 * 默认开启（用户期望看到真实照片预览）。
 * 运行时可通过环境变量 APP_ALBUM_THUMBNAILS=0 关闭（OOM 排障时使用）。
 * 尺寸上限由 ALBUM_MAX_IMAGE_PIXELS 控制，超过则回退到占位图防 OOM。 */
static int s_load_thumbnails = 1;

/* ======================== 懒加载缩略图 ======================== */
/* 性能优化：相册打开时先创建所有占位格子（瞬时），再用定时器分批加载缩略图。
 * 避免一次性同步解码所有 JPEG/提取视频帧导致 UI 长时间冻结。 */

/* 每个格子的图片 widget 指针（用于后续异步更新缩略图） */
static lv_obj_t *g_cell_imgs[ALBUM_MAX_PHOTOS];
/* 每个格子的容器 cell 指针（用于更新样式） */
static lv_obj_t *g_cells[ALBUM_MAX_PHOTOS];
/* 懒加载定时器和当前加载索引 */
static lv_timer_t *g_lazy_timer    = NULL;
static int         g_lazy_idx      = 0;
/* 每次定时器回调加载的缩略图数量（平衡加载速度和 UI 流畅度） */
#define LAZY_BATCH_SIZE 2
/* 定时器周期（ms）：值越小加载越快但 UI 越卡 */
#define LAZY_TIMER_PERIOD 30

/* ------------------------------------------------------------------ */
/* 目录扫描                                                            */
/* ------------------------------------------------------------------ */
/* 检查文件扩展名是否为支持的图片格式 */
static int has_image_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    return (strcasecmp(dot, ".jpg") == 0 ||
            strcasecmp(dot, ".jpeg") == 0 ||
            strcasecmp(dot, ".bmp") == 0);
}

/* 检查文件扩展名是否为支持的媒体格式（图片+视频） */
static int has_media_ext(const char *name)
{
    return has_image_ext(name) || is_video_file(name);
}

/* 读取文件头部，做最小有效性校验，避免把损坏/非图片文件交给 LVGL
 * 解码器导致 DRM 真机死机。
 * JPEG 大图会通过 album_image_cache 自动降采样，因此允许更大的文件。 */
static int is_image_valid(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return 0;
    }

    /* 文件至少要有 JPEG SOI 标记或 BMP 文件头 */
    uint8_t hdr[8];
    size_t n = fread(hdr, 1, sizeof(hdr), f);
    fclose(f);
    if (n < 2) return 0;

    /* JPEG: 允许到 200MB（后续 album_image_cache 会缩放到安全尺寸） */
    if (hdr[0] == 0xFF && hdr[1] == 0xD8) {
        if (sz > (200L * 1024 * 1024)) {
            LV_LOG_WARN("Album: skip huge JPEG %s (size %ld)", path, sz);
            return 0;
        }
        return 1;
    }
    /* BMP: 无压缩文件过大，限制 20MB */
    if (hdr[0] == 'B' && hdr[1] == 'M') {
        if (sz > (20L * 1024 * 1024)) {
            LV_LOG_WARN("Album: skip huge BMP %s (size %ld)", path, sz);
            return 0;
        }
        return 1;
    }

    return 0;
}

/* 递归扫描目录，收集图片和视频文件路径。
 * 使用足够大的临时缓冲区拼接路径，再复制到持久化存储，避免 snprintf
 * 截断导致路径不完整或 LVGL 图片解码失败。
 * depth 参数限制递归深度，防止目录嵌套过深导致栈溢出（最大 2 层）。 */
static void scan_dir_impl(const char *dir, int *count, int depth)
{
    /* 递归深度保护：最多扫描 2 层子目录（DCIM/Camera/xxx.jpg 等） */
    if (depth > 2) return;

    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL && *count < ALBUM_MAX_PHOTOS) {
        if (entry->d_name[0] == '.') continue;

        /* 临时缓冲区限制为 ALBUM_PATH_LEN-2，确保后续 "A:%s" 复制到
         * g_photos（大小 ALBUM_PATH_LEN）时不会截断（预留 "A:" + NUL）。 */
        char fullpath[ALBUM_PATH_LEN - 2];
        int n = snprintf(fullpath, sizeof(fullpath), "%s/%s", dir, entry->d_name);
        if (n < 0 || n >= (int)sizeof(fullpath)) continue;

        struct stat st;
        if (stat(fullpath, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            /* 递归子目录（带深度限制） */
            scan_dir_impl(fullpath, count, depth + 1);
        } else if (S_ISREG(st.st_mode) && has_media_ext(entry->d_name)) {
            /* 视频文件直接收录（不做文件头校验，视频格式多样） */
            if (is_video_file(fullpath)) {
                snprintf(g_photos[*count], ALBUM_PATH_LEN, "A:%s", fullpath);
                (*count)++;
                continue;
            }
            /* 图片：先做最小文件头校验，过滤损坏文件，避免 LVGL 解码器在真机死机 */
            if (!is_image_valid(fullpath)) {
                LV_LOG_WARN("Album: skip invalid image %s", fullpath);
                continue;
            }
            /* 直接以 LVGL 文件路径格式 "A:/path" 保存，避免后续把栈缓冲地址
             * 交给 lv_image_set_src() 导致指针悬空（相册页黑屏/死机根因之一）。 */
            snprintf(g_photos[*count], ALBUM_PATH_LEN, "A:%s", fullpath);
            (*count)++;
        }
    }
    closedir(d);
}

/* 对外接口：从根目录开始扫描（depth=0） */
static void scan_dir(const char *dir, int *count)
{
    scan_dir_impl(dir, count, 0);
}

static void refresh_photo_list(void)
{
    // #region debug-point B:album
    dbg_log("B", "page_album.c:refresh_photo_list", "start");
    // #endregion
    g_photo_count = 0;

    /* 动态构建扫描目录：优先用户选择的存储位置，再回退到所有可能路径。
     * 拍照/录制实际保存到 settings_storage_get_path()/DCIM/，
     * 相册必须扫描相同路径才能正确显示已拍摄的内容。
     * 同时扫描所有板的内置存储路径，确保切换板子或存储位置后都能看到历史文件。
     * s_scan_dirs 数组容量 6，使用时不要溢出 */
    const char *base = settings_storage_get_path();
    int n = 0;
    if (base && *base) {
        snprintf(s_scan_dirs[n], ALBUM_PATH_LEN, "%s/DCIM", base);
        n++;
    }
    /* 回退路径：扫描所有可能的内置存储位置，兼容不同板子和历史数据。
     * 注意 base 来自 settings_storage_get_path()，可能是 /userdata、/root 或 /mnt/sdcard */
    if (!base || strcmp(base, "/userdata") != 0) {
        snprintf(s_scan_dirs[n], ALBUM_PATH_LEN, "/userdata/DCIM");
        n++;
    }
    if (!base || strcmp(base, "/root") != 0) {
        snprintf(s_scan_dirs[n], ALBUM_PATH_LEN, "/root/DCIM");
        n++;
    }
    if (!base || strcmp(base, "/") != 0) {
        snprintf(s_scan_dirs[n], ALBUM_PATH_LEN, "/DCIM");
        n++;
    }
    if (!base || strcmp(base, "/mnt/sdcard") != 0) {
        snprintf(s_scan_dirs[n], ALBUM_PATH_LEN, "/mnt/sdcard/DCIM");
        n++;
    }
    /* 本地开发环境（PC 调试用） */
    snprintf(s_scan_dirs[n], ALBUM_PATH_LEN, "./photos");
    n++;

    for (int i = 0; i < n; i++) {
        scan_dir(s_scan_dirs[i], &g_photo_count);
        if (g_photo_count >= ALBUM_MAX_PHOTOS) break;
    }
    LV_LOG_USER("Album: found %d photos (scanned %d dirs, base=%s)",
                g_photo_count, n, base ? base : "NULL");
    // #region debug-point B:album
    dbg_log("B", "page_album.c:refresh_photo_list", "done");
    // #endregion
}

/* ------------------------------------------------------------------ */
/* 图片加载安全校验                                                    */
/* ------------------------------------------------------------------ */
/* 允许加载的最大像素数。
 * 1080p ≈ 2M 像素，按 RGB888 解码约需 6MB 内存。
 * 上限设为 4M 像素（约 2560x1600），覆盖主流摄像头输出，
 * 同时避免超大图（如 4K/8K）在真机触发 OOM/段错误。
 * 按 16bpp 估算 4M 像素约需 8MB 解码缓冲，RK3566 可承受。 */
#define ALBUM_MAX_IMAGE_PIXELS (4u * 1024u * 1024u)

/* 使用 LVGL 解码器获取图片头信息，并校验宽高与总像素。
 * 返回 true 表示可以安全加载，false 表示跳过该图片。 */
static int album_image_get_info(const char *path, lv_image_header_t *header)
{
    if (!path || !header) return 0;

    lv_result_t res = lv_image_decoder_get_info(path, header);
    if (res != LV_RESULT_OK) {
        LV_LOG_WARN("Album: failed to get image info: %s", path);
        return 0;
    }

    if (header->w <= 0 || header->h <= 0) {
        LV_LOG_WARN("Album: invalid image dimensions %dx%d: %s",
                    (int)header->w, (int)header->h, path);
        return 0;
    }

    /* 使用 uint64_t 计算像素数，避免大尺寸图片（如 65535x65535）溢出 uint32_t */
    uint64_t pixels = (uint64_t)header->w * (uint64_t)header->h;
    if (pixels > ALBUM_MAX_IMAGE_PIXELS) {
        LV_LOG_WARN("Album: image too large %dx%d (%llu pixels), skip: %s",
                    (int)header->w, (int)header->h,
                    (unsigned long long)pixels, path);
        return 0;
    }

    return 1;
}

/* ------------------------------------------------------------------ */
/* 全屏查看                                                             */
/* ------------------------------------------------------------------ */

/* 前向声明：format_file_size 在长按菜单区域定义，全屏视频占位也需要使用 */
static void format_file_size(long long sz, char *buf, size_t buf_sz);

static void fullscreen_close(void)
{
    if (g_fullscreen) {
        lv_obj_add_flag(g_fullscreen, LV_OBJ_FLAG_HIDDEN);
        LV_LOG_USER("Album: fullscreen closed");
    }
}

/* 返回主页（tileview 导航 + 右滑手势的等价点击入口） */
static void on_back_to_home_cb(lv_event_t *e)
{
    (void)e;
    LV_LOG_USER("Album: back-to-home clicked, navigate to home tile");
    lv_tileview_set_tile(g_tv, g_tile_home, LV_ANIM_ON);
}

/* tileview 切换回调：进入相册时刷新扫描，离开相册时关闭 overlay。
 * 进入相册刷新：page_album_create 为优化启动速度不再同步扫描照片，
 * 此处在用户滑动进入相册时触发 page_album_refresh，确保新拍摄的照片可见。
 * 离开相册清理：关闭播放器和悬浮层，避免回主页后定时器继续运行。 */
static void album_tile_change_cb(lv_event_t *e)
{
    (void)e;
    if (!g_tile) return;
    lv_obj_t *active = lv_tileview_get_tile_active(g_tv);

    if (active == g_tile) {
        /* 进入相册：触发扫描刷新（首次进入时网格为空，此处填充） */
        page_album_refresh();
        return;
    }

    /* 离开相册：关闭 overlay 和播放器 */
    video_player_close();
    fullscreen_close();
    if (g_context_menu && lv_obj_is_valid(g_context_menu)) {
        lv_obj_add_flag(g_context_menu, LV_OBJ_FLAG_HIDDEN);
    }
    g_context_idx = -1;
    LV_LOG_USER("Album: left album tile, closed overlays");
}

static void fullscreen_close_cb(lv_event_t *e)
{
    (void)e;
    fullscreen_close();
}

static void fullscreen_gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    LV_LOG_USER("Album fullscreen gesture: dir=%d", dir);
    /* 右滑或左滑都关闭全屏 */
    if (dir == LV_DIR_RIGHT || dir == LV_DIR_LEFT) {
        fullscreen_close();
    }
}

static void open_fullscreen(int idx)
{
    if (idx < 0 || idx >= g_photo_count) return;
    if (!g_fullscreen) return;
    // #region debug-point B:album
    dbg_log("B", "page_album.c:open_fullscreen", "start");
    // #endregion

    /* 路径格式为 "A:/path"，去掉前缀 "A:" 得到原始路径用于文件操作 */
    const char *lvgl_path = g_photos[idx];
    const char *real_path = lvgl_path;
    if (lvgl_path[0] == 'A' && lvgl_path[1] == ':') real_path = lvgl_path + 2;

    /* 视频文件：显示占位提示（LV_USE_FFMPEG=0，暂无视频播放能力）。
     * 隐藏图片控件，显示视频占位界面（含文件名和大小），避免纯灰/空白。 */
    if (is_video_file(real_path)) {
        if (g_full_img) lv_obj_add_flag(g_full_img, LV_OBJ_FLAG_HIDDEN);
        if (g_video_placeholder) {
            lv_obj_remove_flag(g_video_placeholder, LV_OBJ_FLAG_HIDDEN);

            /* 更新文件名标签（第 3 个子对象：vname） */
            lv_obj_t *vname = lv_obj_get_child(g_video_placeholder, 2);
            if (vname) {
                const char *fname = strrchr(real_path, '/');
                fname = fname ? fname + 1 : real_path;
                lv_label_set_text(vname, fname);
            }

            /* 更新文件大小标签（第 4 个子对象：vsize） */
            lv_obj_t *vsize = lv_obj_get_child(g_video_placeholder, 3);
            if (vsize) {
                struct stat vst;
                char size_str[32] = "";
                if (stat(real_path, &vst) == 0) {
                    format_file_size((long long)vst.st_size, size_str, sizeof(size_str));
                }
                lv_label_set_text(vsize, size_str);
            }
        }

        LV_LOG_USER("Album: video file %s - 播放功能待接入 RK MPP", real_path);
        lv_obj_remove_flag(g_fullscreen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(g_fullscreen);
        return;
    }

    /* 图片文件：显示图片控件，隐藏视频占位。
     * 注意必须取消 g_full_img 的隐藏标志——上一次查看视频时已隐藏它 */
    if (g_video_placeholder) lv_obj_add_flag(g_video_placeholder, LV_OBJ_FLAG_HIDDEN);
    if (g_full_img) lv_obj_remove_flag(g_full_img, LV_OBJ_FLAG_HIDDEN);
    if (g_full_img) {
        /* 大图使用预览缓存，避免 LVGL 直接解码 20MP+ 原图导致 OOM */
        const char *preview_path = album_image_get_path(g_photos[idx], ALBUM_IMAGE_CACHE_PREVIEW);
        lv_image_header_t header;
        if (preview_path && album_image_get_info(preview_path, &header)) {
            lv_image_set_src(g_full_img, preview_path);
            LV_LOG_USER("Album: fullscreen load %dx%d: %s",
                        (int)header.w, (int)header.h, preview_path);
        } else {
            /* 无法加载时显示空白，避免设置无效 src 导致渲染崩溃 */
            lv_image_set_src(g_full_img, NULL);
            LV_LOG_WARN("Album: fullscreen skip unloadable photo %d", idx);
        }
    }

    lv_obj_remove_flag(g_fullscreen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_fullscreen);
    LV_LOG_USER("Album: fullscreen open photo %d: %s", idx, g_photos[idx]);
}

static void create_fullscreen_overlay(lv_obj_t *parent)
{
    g_fullscreen = lv_obj_create(parent);
    lv_obj_set_size(g_fullscreen, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(g_fullscreen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_fullscreen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(g_fullscreen, 0, 0);
    lv_obj_set_style_border_width(g_fullscreen, 0, 0);
    lv_obj_remove_flag(g_fullscreen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_fullscreen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(g_fullscreen, LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* 右滑/左滑关闭全屏 */
    lv_obj_add_event_cb(g_fullscreen, fullscreen_gesture_cb, LV_EVENT_GESTURE, NULL);

    /* 全屏图片 */
    g_full_img = lv_image_create(g_fullscreen);
    lv_obj_set_size(g_full_img, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(g_full_img, lv_color_hex(0x000000), 0);
    /* 不设置 image_recolor：recolor + OPA_COVER 会把图片完全覆盖成纯色，
     * 导致照片显示为灰色（之前的 bug 根因）。保持原色显示。 */
    lv_obj_center(g_full_img);
    lv_image_set_scale(g_full_img, 256);  /* 1:1 scale */

    /* 视频文件全屏占位提示（无视频解码器时显示）。
     * 放在返回按钮之前，确保返回按钮仍在上层可点击 */
    g_video_placeholder = lv_obj_create(g_fullscreen);
    lv_obj_set_size(g_video_placeholder, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(g_video_placeholder, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(g_video_placeholder, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(g_video_placeholder, 0, 0);
    lv_obj_set_style_border_width(g_video_placeholder, 0, 0);
    lv_obj_set_flex_flow(g_video_placeholder, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_video_placeholder, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(g_video_placeholder, ui_scale_min(12), 0);
    lv_obj_remove_flag(g_video_placeholder, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(g_video_placeholder, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *vicon = lv_label_create(g_video_placeholder);
    lv_label_set_text(vicon, LV_SYMBOL_VIDEO);
    lv_obj_set_style_text_font(vicon, ui_font_sym(), 0);
    lv_obj_set_style_text_color(vicon, lv_color_hex(COLOR_ACCENT), 0);

    lv_obj_t *vmsg = lv_label_create(g_video_placeholder);
    lv_label_set_text(vmsg, tr("视频播放待接入", "Video playback coming soon"));
    lv_obj_set_style_text_font(vmsg, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(vmsg, lv_color_hex(0x888888), 0);
    lv_obj_set_width(vmsg, lv_pct(80));
    lv_label_set_long_mode(vmsg, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(vmsg, LV_TEXT_ALIGN_CENTER, 0);

    /* 文件名标签（运行时由 open_fullscreen 动态填充） */
    lv_obj_t *vname = lv_label_create(g_video_placeholder);
    lv_label_set_text(vname, "");
    lv_obj_set_style_text_font(vname, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(vname, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_width(vname, lv_pct(80));
    lv_label_set_long_mode(vname, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(vname, LV_TEXT_ALIGN_CENTER, 0);

    /* 文件大小标签（运行时由 open_fullscreen 动态填充） */
    lv_obj_t *vsize = lv_label_create(g_video_placeholder);
    lv_label_set_text(vsize, "");
    lv_obj_set_style_text_font(vsize, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(vsize, lv_color_hex(0x666666), 0);

    /* 左上角浮动返回箭头（透明背景，不影响图片显示） */
    lv_obj_t *back = lv_button_create(g_fullscreen);
    int bsz = ui_scale_min(44);
    lv_obj_set_size(back, bsz, bsz);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, ui_scale_min(8), ui_scale_min(8));
    lv_obj_set_style_bg_color(back, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_50, 0);
    lv_obj_set_style_radius(back, bsz / 2, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_remove_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(back, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(back, fullscreen_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(bl, ui_font_sym(), 0);
    lv_obj_set_style_text_color(bl, lv_color_white(), 0);
    lv_obj_center(bl);
}

/* ------------------------------------------------------------------ */
/* 缩略图网格                                                           */
/* ------------------------------------------------------------------ */
static void on_thumb_click(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= g_photo_count) return;

    /* 长按触发后，紧随其后的 release 仍可能产生 CLICK 事件，
     * 这里忽略一次，避免菜单弹出的同时又打开全屏/播放器。 */
    if (g_ignore_next_click) {
        g_ignore_next_click = 0;
        return;
    }

    const char *lvgl_path = g_photos[idx];
    const char *real_path = lvgl_path;
    if (lvgl_path[0] == 'A' && lvgl_path[1] == ':') real_path = lvgl_path + 2;

    /* 视频文件：优先使用 FFmpeg 播放器全屏播放；
     * 播放器不可用时回退到原来的占位提示界面。 */
    if (is_video_file(real_path) && video_player_available()) {
        video_player_open(g_tile, real_path, NULL, NULL);
        return;
    }

    open_fullscreen(idx);
}

/* ------------------------------------------------------------------ */
/* 长按二级菜单：查看信息 / 删除                                        */
/* ------------------------------------------------------------------ */

/* 格式化文件大小为人类可读字符串 */
static void format_file_size(long long sz, char *buf, size_t buf_sz)
{
    if (sz >= 1024 * 1024 * 1024) {
        snprintf(buf, buf_sz, "%.2f GB", (double)sz / (1024 * 1024 * 1024));
    } else if (sz >= 1024 * 1024) {
        snprintf(buf, buf_sz, "%.1f MB", (double)sz / (1024 * 1024));
    } else if (sz >= 1024) {
        snprintf(buf, buf_sz, "%.1f KB", (double)sz / 1024);
    } else {
        snprintf(buf, buf_sz, "%lld B", sz);
    }
}

/* 获取文件修改时间字符串 */
static void get_file_mtime(const char *path, char *buf, size_t buf_sz)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        snprintf(buf, buf_sz, "--");
        return;
    }
    struct tm *t = localtime(&st.st_mtime);
    if (t) {
        strftime(buf, buf_sz, "%Y-%m-%d %H:%M:%S", t);
    } else {
        snprintf(buf, buf_sz, "%lld", (long long)st.st_mtime);
    }
}

/* 关闭长按菜单。
 * 作为 LVGL 事件回调注册时必须接收 lv_event_t* 参数，
 * 内部不使用该参数（仅隐藏 overlay 并重置索引）。 */
static void context_menu_close(lv_event_t *e)
{
    (void)e;
    if (g_context_menu) {
        lv_obj_add_flag(g_context_menu, LV_OBJ_FLAG_HIDDEN);
    }
    g_context_idx = -1;
}

/* 前向声明 */
static void rebuild_grid(void);
static void context_menu_show_actions(void);

/* 菜单背景点击/左滑关闭 */
static void context_menu_bg_cb(lv_event_t *e)
{
    (void)e;
    context_menu_close(NULL);
}

/* 信息页返回按钮：返回操作菜单（信息/删除），不关闭整个 overlay */
static void context_menu_back_to_actions(lv_event_t *e)
{
    (void)e;
    context_menu_show_actions();
}

/* 删除按钮回调：实际执行文件删除并刷新网格 */
static void context_menu_delete_cb(lv_event_t *e)
{
    (void)e;
    int idx = g_context_idx;
    if (idx < 0 || idx >= g_photo_count) {
        context_menu_close(NULL);
        return;
    }

    /* 如果当前正在全屏查看或播放被删除的文件，先关闭，
     * 避免文件删除后播放器/全屏仍引用旧路径或占用屏幕。 */
    video_player_close();
    fullscreen_close();

    /* g_photos[idx] 格式为 "A:/path"，去掉 "A:" 前缀得到真实路径 */
    const char *lvgl_path = g_photos[idx];
    const char *real_path = lvgl_path;
    if (lvgl_path[0] == 'A' && lvgl_path[1] == ':') real_path = lvgl_path + 2;

    int ret = remove(real_path);
    if (ret != 0) {
        LV_LOG_ERROR("删除文件失败: %s (errno=%d: %s)", real_path, errno, strerror(errno));
        /* 删除失败时仍关闭菜单，但不刷新网格 */
        context_menu_close(NULL);
        return;
    }

    LV_LOG_USER("已删除文件: %s", real_path);

    /* 从 g_photos 数组中移除该项：后续元素前移 */
    for (int i = idx; i < g_photo_count - 1; i++) {
        strncpy(g_photos[i], g_photos[i + 1], ALBUM_PATH_LEN - 1);
        g_photos[i][ALBUM_PATH_LEN - 1] = '\0';
    }
    g_photos[g_photo_count - 1][0] = '\0';
    g_photo_count--;

    context_menu_close(NULL);
    rebuild_grid();  /* 刷新网格显示 */
}

/* 信息按钮回调：显示文件详细信息 */
static void context_menu_info_cb(lv_event_t *e)
{
    (void)e;
    int idx = g_context_idx;
    if (idx < 0 || idx >= g_photo_count) {
        context_menu_close(NULL);
        return;
    }

    const char *lvgl_path = g_photos[idx];
    const char *real_path = lvgl_path;
    if (lvgl_path[0] == 'A' && lvgl_path[1] == ':') real_path = lvgl_path + 2;

    /* 获取文件名 */
    const char *name = strrchr(real_path, '/');
    name = name ? name + 1 : real_path;

    /* 获取文件大小 */
    struct stat st;
    char size_str[32] = "--";
    if (stat(real_path, &st) == 0) {
        format_file_size((long long)st.st_size, size_str, sizeof(size_str));
    }

    /* 获取修改时间 */
    char time_str[32];
    get_file_mtime(real_path, time_str, sizeof(time_str));

    /* 获取图片尺寸（仅图片文件）。大图使用预览缓存的尺寸，避免原图被过滤后拿不到信息 */
    char dim_str[32] = "";
    if (!is_video_file(real_path)) {
        const char *preview_path = album_image_get_path(g_photos[idx], ALBUM_IMAGE_CACHE_PREVIEW);
        lv_image_header_t header;
        if (preview_path && album_image_get_info(preview_path, &header)) {
            snprintf(dim_str, sizeof(dim_str), "%dx%d", (int)header.w, (int)header.h);
        }
    }

    /* 获取文件类型 */
    const char *type_str;
    if (is_video_file(real_path)) {
        const char *dot = strrchr(real_path, '.');
        type_str = dot ? dot + 1 : "VIDEO";
    } else {
        const char *dot = strrchr(real_path, '.');
        if (dot && (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0)) {
            type_str = "JPEG";
        } else if (dot && strcasecmp(dot, ".bmp") == 0) {
            type_str = "BMP";
        } else {
            type_str = "IMAGE";
        }
    }

    /* 将信息显示到菜单的 info_label 上，替换原来的按钮列表 */
    if (!g_context_menu) {
        context_menu_close(NULL);
        return;
    }

    /* 找到信息内容容器（panel 的第二个子对象，索引 1） */
    lv_obj_t *panel = lv_obj_get_child(g_context_menu, 0);
    if (!panel) {
        context_menu_close(NULL);
        return;
    }

    /* 清空 panel 中的按钮，重建为信息展示界面 */
    lv_obj_clean(panel);

    /* 标题栏：返回按钮 + "文件信息" 标题 */
    lv_obj_t *header = lv_obj_create(panel);
    lv_obj_set_size(header, lv_pct(100), ui_scale_min(44));
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x222222), 0);
    lv_obj_set_style_pad_all(header, ui_scale_min(8), 0);
    lv_obj_set_style_pad_gap(header, ui_scale_min(8), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back_btn = lv_button_create(header);
    lv_obj_set_size(back_btn, ui_scale_min(36), ui_scale_min(36));
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_remove_flag(back_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(back_btn, context_menu_back_to_actions, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back_btn);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(bl, ui_font_sym(), 0);
    lv_obj_set_style_text_color(bl, lv_color_white(), 0);
    lv_obj_center(bl);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, tr("文件信息", "File Info"));
    lv_obj_set_style_text_font(title, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    /* 信息内容区（可滚动，防止小屏幕溢出）。
     * 横屏使用 ROW_WRAP 双列网格提升空间利用率，竖屏使用 COLUMN 单列排列。
     * 文件名行始终全宽显示（内容较长），其余信息行横屏下双列并排 */
    lv_obj_t *body = lv_obj_create(panel);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(body, ui_scale_min(12), 0);
    lv_obj_set_style_pad_gap(body, ui_scale_min(8), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);

    int wide = ui_is_wide_layout();
    lv_obj_set_flex_flow(body, wide ? LV_FLEX_FLOW_ROW_WRAP : LV_FLEX_FLOW_COLUMN);

    /* 辅助宏：创建一行信息（label + value），row_w_pct 控制行宽度占比 */
    #define ADD_INFO_ROW(BODY, LABEL, VALUE, ROW_W_PCT) do {              \
        lv_obj_t *row = lv_obj_create(BODY);                              \
        lv_obj_set_width(row, lv_pct(ROW_W_PCT));                        \
        lv_obj_set_height(row, LV_SIZE_CONTENT);                         \
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);                     \
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,          \
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);\
        lv_obj_set_style_pad_all(row, ui_scale_min(4), 0);               \
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);                  \
        lv_obj_set_style_border_width(row, 0, 0);                        \
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);                 \
        lv_obj_t *kl = lv_label_create(row);                             \
        lv_label_set_text(kl, LABEL);                                    \
        lv_obj_set_style_text_font(kl, ui_font_cjk(), 0);               \
        lv_obj_set_style_text_color(kl, lv_color_hex(0x888888), 0);     \
        lv_obj_t *vl = lv_label_create(row);                             \
        lv_label_set_text(vl, VALUE);                                    \
        lv_obj_set_style_text_font(vl, ui_font_cjk(), 0);               \
        lv_obj_set_style_text_color(vl, lv_color_white(), 0);           \
        lv_obj_set_width(vl, lv_pct(60));                               \
        lv_label_set_long_mode(vl, LV_LABEL_LONG_DOT);                  \
        lv_obj_set_style_text_align(vl, LV_TEXT_ALIGN_RIGHT, 0);        \
    } while (0)

    /* 文件名行：始终全宽显示（文件名通常较长，避免双列挤压） */
    ADD_INFO_ROW(body, tr("文件名", "Name"), name, 100);

    /* 其余信息行：横屏双列（各占 48%），竖屏单列（全宽） */
    int info_w_pct = wide ? 48 : 100;
    ADD_INFO_ROW(body, tr("类型", "Type"),     type_str,  info_w_pct);
    ADD_INFO_ROW(body, tr("大小", "Size"),     size_str,  info_w_pct);

    /* 图片尺寸行（仅图片显示） */
    if (dim_str[0]) {
        ADD_INFO_ROW(body, tr("尺寸", "Dimensions"), dim_str, info_w_pct);
    }

    ADD_INFO_ROW(body, tr("修改时间", "Time"), time_str, info_w_pct);

    #undef ADD_INFO_ROW
}

/* 重建操作菜单面板（信息/删除按钮列表）。
 * 被 on_thumb_long_press 和信息页返回按钮共同调用 */
static void context_menu_show_actions(void)
{
    if (!g_context_menu || g_context_idx < 0) return;

    /* 清空旧内容并重建为菜单按钮列表 */
    lv_obj_t *panel = lv_obj_get_child(g_context_menu, 0);
    if (!panel) return;
    lv_obj_clean(panel);

    /* 标题栏 */
    lv_obj_t *header = lv_obj_create(panel);
    lv_obj_set_size(header, lv_pct(100), ui_scale_min(44));
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x222222), 0);
    lv_obj_set_style_pad_all(header, ui_scale_min(8), 0);
    lv_obj_set_style_pad_gap(header, ui_scale_min(8), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *close_btn = lv_button_create(header);
    lv_obj_set_size(close_btn, ui_scale_min(36), ui_scale_min(36));
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(close_btn, 0, 0);
    lv_obj_remove_flag(close_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(close_btn, context_menu_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(close_btn);
    lv_label_set_text(cl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(cl, ui_font_sym(), 0);
    lv_obj_set_style_text_color(cl, lv_color_white(), 0);
    lv_obj_center(cl);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, tr("操作", "Actions"));
    lv_obj_set_style_text_font(title, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    /* 按钮列表：横屏使用 ROW 横向排列（两按钮并排），竖屏使用 COLUMN 纵向排列。
     * 这样横屏底部弹窗高度更紧凑，竖屏右侧滑出面板按钮更易点击 */
    lv_obj_t *body = lv_obj_create(panel);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(body, ui_scale_min(12), 0);
    lv_obj_set_style_pad_gap(body, ui_scale_min(8), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);

    int btn_w_pct;
    if (ui_is_wide_layout()) {
        /* 横屏：两按钮并排，各占 48% 宽度，留 4% 间距 */
        lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
        btn_w_pct = 48;
    } else {
        /* 竖屏：按钮纵向堆叠，各占 90% 宽度 */
        lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
        btn_w_pct = 90;
    }

    /* 文件信息按钮 */
    lv_obj_t *info_btn = lv_button_create(body);
    lv_obj_set_size(info_btn, lv_pct(btn_w_pct), ui_scale_min(44));
    lv_obj_set_style_bg_color(info_btn, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_border_width(info_btn, 0, 0);
    lv_obj_set_style_radius(info_btn, ui_scale_min(8), 0);
    lv_obj_remove_flag(info_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(info_btn, context_menu_info_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *info_lbl = lv_label_create(info_btn);
    lv_label_set_text(info_lbl, tr("文件信息", "File Info"));
    lv_obj_set_style_text_font(info_lbl, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(info_lbl, lv_color_white(), 0);
    lv_obj_center(info_lbl);

    /* 删除按钮（红色警告色） */
    lv_obj_t *del_btn = lv_button_create(body);
    lv_obj_set_size(del_btn, lv_pct(btn_w_pct), ui_scale_min(44));
    lv_obj_set_style_bg_color(del_btn, lv_color_hex(0x8B0000), 0);  /* 深红色 */
    lv_obj_set_style_border_width(del_btn, 0, 0);
    lv_obj_set_style_radius(del_btn, ui_scale_min(8), 0);
    lv_obj_remove_flag(del_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(del_btn, context_menu_delete_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *del_lbl = lv_label_create(del_btn);
    lv_label_set_text(del_lbl, tr("删除", "Delete"));
    lv_obj_set_style_text_font(del_lbl, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(del_lbl, lv_color_white(), 0);
    lv_obj_center(del_lbl);
}

/* 长按缩略图回调：弹出二级菜单 */
static void on_thumb_long_press(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= g_photo_count) return;

    g_context_idx = idx;
    if (!g_context_menu) return;

    /* 标记忽略下一次 CLICK，防止长按松开后误触发打开全屏/播放器 */
    g_ignore_next_click = 1;

    context_menu_show_actions();

    /* 显示菜单 */
    lv_obj_remove_flag(g_context_menu, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_context_menu);
    LV_LOG_USER("Album: long press on photo %d, context menu opened", idx);
}

/* 创建长按二级菜单 overlay（在 page_album_create 中调用一次）。
 * 横竖屏布局自适应：
 *   - 竖屏 480x800：右侧滑出面板（82% 宽 × 100% 高），符合单手操作习惯
 *   - 横屏 800x480：底部弹出面板（限宽 720px 居中，高度自适应内容），
 *                   避免横屏右侧面板过窄导致按钮挤压 */
static void create_context_menu_overlay(lv_obj_t *parent)
{
    g_context_menu = lv_obj_create(parent);
    lv_obj_set_size(g_context_menu, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(g_context_menu, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_context_menu, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(g_context_menu, 0, 0);
    lv_obj_set_style_border_width(g_context_menu, 0, 0);
    lv_obj_remove_flag(g_context_menu, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_context_menu, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(g_context_menu, LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* 点击背景关闭菜单 */
    lv_obj_add_event_cb(g_context_menu, context_menu_bg_cb, LV_EVENT_CLICKED, NULL);
    /* 左滑关闭菜单（与系统返回手势一致） */
    ui_attach_back_gesture(g_context_menu, context_menu_bg_cb);

    /* 面板容器：根据横竖屏采用不同布局 */
    lv_obj_t *panel = lv_obj_create(g_context_menu);
    lv_obj_set_style_bg_color(panel, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    if (ui_is_wide_layout()) {
        /* 横屏：底部弹出，限宽 720px 居中，高度占屏幕 65%。
         * 固定高度比例保证 body 的 flex_grow 能正确分配剩余空间，
         * 内容超出时 body 内部滚动 */
        lv_obj_set_width(panel, ui_get_content_max_width());
        lv_obj_set_height(panel, lv_pct(65));
        lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_radius(panel, ui_scale_min(12), 0);
    } else {
        /* 竖屏：右侧滑出，82% 宽 × 100% 高，符合单手右滑操作习惯 */
        lv_obj_set_size(panel, lv_pct(82), lv_pct(100));
        lv_obj_align(panel, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_radius(panel, 0, 0);
    }
}

/* 前向声明：create_thumbnail 在检测到无效图片时会回退到占位图，
 * 视频文件则回退到轻量照片单元（图标+文件名占位）。 */
static lv_obj_t *create_placeholder(lv_obj_t *parent, int cell_size);
static lv_obj_t *create_photo_cell(lv_obj_t *parent, int idx, int cell_size);

/* 创建带播放图标覆盖层的视频缩略图单元 */
static lv_obj_t *create_video_thumb_cell(lv_obj_t *parent, int idx, int cell_size,
                                          const char *thumb_path)
{
    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_set_size(cell, cell_size, cell_size);
    lv_obj_set_style_bg_color(cell, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_radius(cell, ui_scale_min(6), 0);
    lv_obj_set_style_pad_all(cell, 0, 0);
    lv_obj_set_style_border_width(cell, 0, 0);
    lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(cell, lv_color_hex(0x3A3A3A), LV_STATE_PRESSED);

    /* 实际可点击的按钮层 */
    lv_obj_t *btn = lv_button_create(cell);
    lv_obj_set_size(btn, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, on_thumb_click, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    lv_obj_add_event_cb(btn, on_thumb_long_press, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)idx);
    ui_attach_press_scale(btn, 242);

    /* 缩略图图片 */
    lv_obj_t *img = lv_image_create(btn);
    lv_image_set_src(img, thumb_path);
    lv_obj_set_size(img, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(img, lv_color_hex(0x1A1A1A), 0);
    lv_obj_center(img);

    /* 播放图标（居中，半透明圆形背景） */
    int play_sz = ui_scale_min(36);
    lv_obj_t *play_bg = lv_obj_create(btn);
    lv_obj_set_size(play_bg, play_sz, play_sz);
    lv_obj_set_style_bg_color(play_bg, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(play_bg, LV_OPA_60, 0);
    lv_obj_set_style_radius(play_bg, play_sz / 2, 0);
    lv_obj_set_style_border_width(play_bg, 0, 0);
    lv_obj_remove_flag(play_bg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(play_bg);

    lv_obj_t *play_ic = lv_label_create(play_bg);
    lv_label_set_text(play_ic, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_font(play_ic, ui_font_sym(), 0);
    lv_obj_set_style_text_color(play_ic, lv_color_white(), 0);
    lv_obj_center(play_ic);

    return cell;
}

static lv_obj_t *create_thumbnail(lv_obj_t *parent, int idx, int cell_size)
{
    const char *lvgl_path = g_photos[idx];
    const char *real_path = lvgl_path;
    if (lvgl_path[0] == 'A' && lvgl_path[1] == ':') real_path = lvgl_path + 2;

    /* 视频文件：尝试提取第一帧作为缩略图；失败则回退到图标占位 */
    if (is_video_file(real_path)) {
        if (video_thumb_available()) {
            const char *thumb_path = video_thumb_get(real_path);
            if (thumb_path) {
                /* 校验缩略图可被 LVGL 解码 */
                lv_image_header_t header;
                if (album_image_get_info(thumb_path, &header)) {
                    return create_video_thumb_cell(parent, idx, cell_size, thumb_path);
                }
            }
        }
        return create_photo_cell(parent, idx, cell_size);
    }

    /* 大图会通过 album_image_cache 自动降采样到安全尺寸；
     * 这里取缩略图缓存路径，再校验 LVGL 能否加载。 */
    const char *thumb_path = album_image_get_path(g_photos[idx], ALBUM_IMAGE_CACHE_THUMB);
    if (!thumb_path) {
        LV_LOG_WARN("Album: thumbnail %d cache unavailable, use placeholder", idx);
        return create_placeholder(parent, cell_size);
    }

    lv_image_header_t header;
    if (!album_image_get_info(thumb_path, &header)) {
        LV_LOG_WARN("Album: thumbnail %d unloadable, use placeholder", idx);
        return create_placeholder(parent, cell_size);
    }

    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_set_size(cell, cell_size, cell_size);
    lv_obj_set_style_bg_color(cell, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_radius(cell, ui_scale_min(6), 0);
    lv_obj_set_style_pad_all(cell, 0, 0);
    lv_obj_set_style_border_width(cell, 0, 0);
    lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(cell, lv_color_hex(0x3A3A3A), LV_STATE_PRESSED);

    /* 实际可点击的按钮层叠在 cell 上 */
    lv_obj_t *btn = lv_button_create(cell);
    lv_obj_set_size(btn, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, on_thumb_click, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    /* 长按弹出二级菜单（信息/删除） */
    lv_obj_add_event_cb(btn, on_thumb_long_press, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)idx);
    ui_attach_press_scale(btn, 242);

    /* 图片：使用缩略图缓存路径（含 "A:/path"），避免直接加载大图导致 OOM */
    lv_obj_t *img = lv_image_create(btn);
    lv_image_set_src(img, thumb_path);
    lv_obj_set_size(img, lv_pct(100), lv_pct(100));
    /* 不设置 image_recolor：recolor + OPA_COVER 会把缩略图完全覆盖成纯色，
     * 导致照片显示为灰色（之前的 bug 根因）。保持原色显示。 */
    lv_obj_set_style_bg_color(img, lv_color_hex(0x1A1A1A), 0);
    lv_obj_center(img);

    return cell;
}

static lv_obj_t *create_placeholder(lv_obj_t *parent, int cell_size)
{
    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_set_size(cell, cell_size, cell_size);
    lv_obj_set_style_bg_color(cell, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_radius(cell, ui_scale_min(6), 0);
    lv_obj_set_style_pad_all(cell, 0, 0);
    lv_obj_set_style_border_width(cell, 0, 0);
    lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *ic = lv_label_create(cell);
    lv_label_set_text(ic, LV_SYMBOL_IMAGE);
    lv_obj_set_style_text_font(ic, ui_font_sym(), 0);
    lv_obj_set_style_text_color(ic, lv_color_hex(0x555555), 0);
    lv_obj_center(ic);

    return cell;
}

/* 轻量照片单元：不加载真实缩略图，仅显示图标+文件名，点击可全屏查看。
 * 用于 RK3566 默认关闭缩略图解码的场景，避免高清图片同时解码导致 OOM/死机。 */
static lv_obj_t *create_photo_cell(lv_obj_t *parent, int idx, int cell_size)
{
    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_set_size(cell, cell_size, cell_size);
    lv_obj_set_style_bg_color(cell, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_radius(cell, ui_scale_min(6), 0);
    lv_obj_set_style_pad_all(cell, ui_scale_min(4), 0);
    lv_obj_set_style_border_width(cell, 0, 0);
    lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(cell, lv_color_hex(0x3A3A3A), LV_STATE_PRESSED);

    /* 点击打开全屏 */
    lv_obj_add_event_cb(cell, on_thumb_click, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    /* 长按弹出二级菜单（信息/删除） */
    lv_obj_add_event_cb(cell, on_thumb_long_press, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)idx);
    ui_attach_press_scale(cell, 242);

    /* 图标：视频文件用视频图标（亮色），图片用图片图标（暗色） */
    lv_obj_t *ic = lv_label_create(cell);
    const char *real_path = g_photos[idx];
    if (real_path[0] == 'A' && real_path[1] == ':') real_path += 2;
    int is_vid = is_video_file(real_path);
    lv_label_set_text(ic, is_vid ? LV_SYMBOL_VIDEO : LV_SYMBOL_IMAGE);
    lv_obj_set_style_text_font(ic, ui_font_sym(), 0);
    /* 视频图标用强调色，图片图标用暗灰色，便于区分 */
    lv_obj_set_style_text_color(ic, is_vid ? lv_color_hex(COLOR_ACCENT) : lv_color_hex(0x555555), 0);
    lv_obj_center(ic);

    /* 文件名（底部居中，超长截断） */
    const char *path = g_photos[idx];
    const char *name = strrchr(path, '/');
    if (name) name++;
    else name = path;
    if (name[0] == 'A' && name[1] == ':') name += 2;  /* 跳过盘符前缀 */

    lv_obj_t *nl = lv_label_create(cell);
    lv_obj_set_width(nl, lv_pct(90));
    lv_obj_set_style_text_font(nl, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(nl, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_align(nl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(nl, LV_LABEL_LONG_DOT);
    lv_label_set_text(nl, name);
    lv_obj_align(nl, LV_ALIGN_BOTTOM_MID, 0, 0);

    return cell;
}

/* ------------------------------------------------------------------ */
/* 懒加载：先创建占位格子，再分批加载缩略图                              */
/* ------------------------------------------------------------------ */

/* 创建占位格子（带按钮+事件回调，但图片为占位图标）。
 * 同时记录 cell 和 image widget 指针到全局数组，供后续懒加载更新。
 * 返回 cell 对象 */
static lv_obj_t *create_thumbnail_slot(lv_obj_t *parent, int idx, int cell_size)
{
    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_set_size(cell, cell_size, cell_size);
    lv_obj_set_style_bg_color(cell, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_radius(cell, ui_scale_min(6), 0);
    lv_obj_set_style_pad_all(cell, 0, 0);
    lv_obj_set_style_border_width(cell, 0, 0);
    lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(cell, lv_color_hex(0x3A3A3A), LV_STATE_PRESSED);

    /* 可点击按钮层 */
    lv_obj_t *btn = lv_button_create(cell);
    lv_obj_set_size(btn, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, on_thumb_click, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    lv_obj_add_event_cb(btn, on_thumb_long_press, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)idx);
    ui_attach_press_scale(btn, 242);

    /* 占位图片：初始显示加载中图标，后续懒加载替换为真实缩略图 */
    lv_obj_t *img = lv_image_create(btn);
    lv_label_set_text(img, LV_SYMBOL_IMAGE);  /* 占位图标 */
    lv_obj_set_style_text_color(img, lv_color_hex(0x555555), 0);
    lv_obj_center(img);

    /* 文件名标签（底部居中） */
    const char *path = g_photos[idx];
    const char *name = strrchr(path, '/');
    if (name) name++;
    else name = path;
    if (name[0] == 'A' && name[1] == ':') name += 2;
    lv_obj_t *nl = lv_label_create(cell);
    lv_obj_set_width(nl, lv_pct(90));
    lv_obj_set_style_text_font(nl, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(nl, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_align(nl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(nl, LV_LABEL_LONG_DOT);
    lv_label_set_text(nl, name);
    lv_obj_align(nl, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* 记录指针供懒加载使用 */
    g_cells[idx]    = cell;
    g_cell_imgs[idx] = img;

    return cell;
}

/* 懒加载定时器回调：每次加载 LAZY_BATCH_SIZE 个缩略图。
 * 加载完成后自动删除定时器。 */
static void lazy_load_cb(lv_timer_t *t)
{
    (void)t;
    int loaded = 0;
    while (g_lazy_idx < g_photo_count && loaded < LAZY_BATCH_SIZE) {
        int idx = g_lazy_idx++;
        lv_obj_t *img = g_cell_imgs[idx];
        if (!img) { loaded++; continue; }

        const char *lvgl_path = g_photos[idx];
        const char *real_path = lvgl_path;
        if (lvgl_path[0] == 'A' && lvgl_path[1] == ':') real_path = lvgl_path + 2;

        /* 视频文件：提取第一帧作为缩略图 */
        if (is_video_file(real_path)) {
            if (video_thumb_available()) {
                const char *thumb_path = video_thumb_get(real_path);
                if (thumb_path) {
                    lv_image_header_t header;
                    if (album_image_get_info(thumb_path, &header)) {
                        lv_image_set_src(img, thumb_path);
                        lv_obj_set_style_text_color(img, lv_color_white(), 0);
                        loaded++;
                        continue;
                    }
                }
            }
            /* 视频缩略图提取失败：显示播放图标 */
            lv_label_set_text(img, LV_SYMBOL_PLAY);
            lv_obj_set_style_text_color(img, lv_color_hex(0x888888), 0);
            loaded++;
            continue;
        }

        /* 照片：获取缩略图缓存路径并设置 */
        const char *thumb_path = album_image_get_path(g_photos[idx], ALBUM_IMAGE_CACHE_THUMB);
        if (thumb_path) {
            lv_image_header_t header;
            if (album_image_get_info(thumb_path, &header)) {
                lv_image_set_src(img, thumb_path);
                lv_obj_set_style_text_color(img, lv_color_white(), 0);
                loaded++;
                continue;
            }
        }
        /* 缩略图加载失败：保持占位图标 */
        loaded++;
    }

    /* 所有缩略图加载完成：删除定时器 */
    if (g_lazy_idx >= g_photo_count) {
        LV_LOG_USER("Album: 懒加载完成（%d/%d）", g_lazy_idx, g_photo_count);
        if (g_lazy_timer) {
            lv_timer_delete(g_lazy_timer);
            g_lazy_timer = NULL;
        }
    }
}

/* 停止懒加载（页面销毁或重建时调用） */
static void lazy_load_stop(void)
{
    if (g_lazy_timer) {
        lv_timer_delete(g_lazy_timer);
        g_lazy_timer = NULL;
    }
    g_lazy_idx = 0;
    for (int i = 0; i < ALBUM_MAX_PHOTOS; i++) {
        g_cell_imgs[i] = NULL;
        g_cells[i] = NULL;
    }
}

/* 根据横竖屏和排列模式计算网格列数，提升空间利用率 */
static int album_get_grid_cols(void)
{
    if (ui_is_wide_layout()) {
        /* 横屏 800x480：小图 5 列、大图 3 列 */
        return g_arrange_mode ? 3 : 5;
    }
    /* 竖屏 480x800：小图 3 列、大图 2 列 */
    return g_arrange_mode ? 2 : 3;
}

/* 重建网格内容。
 * 性能优化：先创建所有占位格子（瞬时完成），再启动定时器分批加载缩略图。
 * 避免一次性同步解码所有 JPEG/提取视频帧导致 UI 长时间冻结。 */
static void rebuild_grid(void)
{
    if (!g_grid) return;
    // #region debug-point B:album
    dbg_log("B", "page_album.c:rebuild_grid", "start");
    // #endregion

    /* 停止上次的懒加载（如有） */
    lazy_load_stop();

    lv_obj_clean(g_grid);

    int cols = album_get_grid_cols();
    int gap = ui_scale_min(4);
    int avail_w = ui_get_screen_width() - ui_scale_min(16) * 2;  /* 减去左右 padding */
    int cell_size = (avail_w - gap * (cols - 1)) / cols;

    lv_obj_set_flex_flow(g_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(g_grid, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(g_grid, gap, 0);

    if (g_photo_count == 0) {
        /* 无照片：空白显示，不创建占位图，仅更新计数标签 */
        if (g_count_label) {
            lv_label_set_text(g_count_label, tr("暂无照片", "No photos"));
        }
        return;
    }

    /* 第一阶段：创建所有占位格子（瞬时完成，无 JPEG 解码/视频提取） */
    for (int i = 0; i < g_photo_count; i++) {
        if (s_load_thumbnails) {
            create_thumbnail_slot(g_grid, i, cell_size);
        } else {
            create_photo_cell(g_grid, i, cell_size);
        }
    }

    if (g_count_label) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d %s", g_photo_count, tr("张照片", "photos"));
        lv_label_set_text(g_count_label, buf);
    }

    /* 第二阶段：启动懒加载定时器，分批加载缩略图 */
    if (s_load_thumbnails && g_photo_count > 0) {
        g_lazy_idx = 0;
        g_lazy_timer = lv_timer_create(lazy_load_cb, LAZY_TIMER_PERIOD, NULL);
        LV_LOG_USER("Album: 懒加载已启动（%d 张，每批 %d 张，周期 %dms）",
                    g_photo_count, LAZY_BATCH_SIZE, LAZY_TIMER_PERIOD);
    }
}

/* ------------------------------------------------------------------ */
/* 排列切换                                                            */
/* ------------------------------------------------------------------ */
static void on_arrange_toggle(lv_event_t *e)
{
    (void)e;
    g_arrange_mode = !g_arrange_mode;
    LV_LOG_USER("Album: arrange mode -> %d", g_arrange_mode);
    rebuild_grid();

    /* 更新按钮图标：grid 模式显示 BARS 图标，list 模式显示 LIST 图标 */
    if (g_arrange_btn) {
        lv_obj_t *ic = lv_obj_get_child(g_arrange_btn, 0);
        if (ic) {
            lv_label_set_text(ic, g_arrange_mode ? LV_SYMBOL_LIST : LV_SYMBOL_BARS);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 存储容量摘要（顶部显示）                                             */
/* ------------------------------------------------------------------ */
static void update_storage_summary(lv_obj_t *label)
{
    long long ti = 0, ui_ = 0, ts = 0, us = 0;
    int ok_int = ui_detect_internal_storage(&ti, &ui_);
    int ok_sd  = ui_detect_sd_card(&ts, &us, NULL, 0);

    char buf[64];
    if (ok_sd && ts > 0) {
        /* SD 卡和内置都显示可用空间，保持一致 */
        long long int_free = ti - ui_;
        long long sd_free = ts - us;
        snprintf(buf, sizeof(buf), "%s: %.1fGB  %s: %.1fGB",
                 tr("内置可用", "Int free"), int_free / 1024.0,
                 tr("SD可用", "SD free"), sd_free / 1024.0);
    } else if (ok_int && ti > 0) {
        long long free_mb = ti - ui_;
        snprintf(buf, sizeof(buf), "%s: %.1fGB",
                 tr("内置可用", "Internal free"), free_mb / 1024.0);
    } else {
        snprintf(buf, sizeof(buf), "%s", tr("未检测到存储", "No storage"));
    }
    lv_label_set_text(label, buf);
}

/* ------------------------------------------------------------------ */
/* 页面创建 / 销毁                                                     */
/* ------------------------------------------------------------------ */
lv_obj_t * page_album_create(lv_obj_t *tv)
{
    // #region debug-point B:album
    dbg_log("B", "page_album.c:page_album_create", "start");
    // #endregion

    /* 决定是否加载真实缩略图：默认开启（用户期望看到真实照片预览）。
     * 编译时可通过 APP_ALBUM_LOAD_THUMBNAILS=0 关闭；
     * 运行时可通过环境变量 APP_ALBUM_THUMBNAILS=0 关闭（OOM 排障时使用）。
     * 尺寸上限 ALBUM_MAX_IMAGE_PIXELS 会过滤过大图片防 OOM。 */
#if defined(APP_ALBUM_LOAD_THUMBNAILS) && (APP_ALBUM_LOAD_THUMBNAILS == 0)
    s_load_thumbnails = 0;
#else
    s_load_thumbnails = 1;
#endif
    const char *env = getenv("APP_ALBUM_THUMBNAILS");
    if (env) {
        /* 支持 =0/=1 或 =n/=y 显式控制 */
        if (*env == '0' || *env == 'n' || *env == 'N') {
            s_load_thumbnails = 0;
        } else if (*env == '1' || *env == 'y' || *env == 'Y') {
            s_load_thumbnails = 1;
        }
    }
    LV_LOG_USER("Album: load_thumbnails=%d", s_load_thumbnails);

    /* 相册 tile (1,1)：右滑(LV_DIR_RIGHT)回主页 */
    g_tile = lv_tileview_add_tile(tv, 1, 1, LV_DIR_RIGHT);
    lv_obj_set_style_bg_color(g_tile, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(g_tile, 0, 0);
    lv_obj_remove_flag(g_tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(g_tile, LV_FLEX_FLOW_COLUMN);

    /* 监听 tileview 切换事件：离开相册时自动关闭播放器/全屏/菜单 */
    lv_obj_add_event_cb(tv, album_tile_change_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* 顶部工具栏 */
    lv_obj_t *toolbar = lv_obj_create(g_tile);
    lv_obj_set_width(toolbar, lv_pct(100));
    lv_obj_set_height(toolbar, ui_scale_min(44));
    lv_obj_set_flex_flow(toolbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(toolbar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(toolbar, ui_scale_min(8), 0);
    lv_obj_set_style_pad_gap(toolbar, ui_scale_min(8), 0);
    lv_obj_set_style_bg_color(toolbar, lv_color_hex(0x0A0A0A), 0);
    lv_obj_set_style_border_width(toolbar, 0, 0);
    lv_obj_remove_flag(toolbar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* 左侧：返回箭头 + 存储摘要 */
    lv_obj_t *back_btn = lv_button_create(toolbar);
    lv_obj_set_size(back_btn, ui_scale_min(36), ui_scale_min(36));
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_80, 0);
    lv_obj_set_style_radius(back_btn, ui_scale_min(8), 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_remove_flag(back_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(back_btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_t *back_ic = lv_label_create(back_btn);
    lv_label_set_text(back_ic, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_ic, ui_font_sym(), 0);
    lv_obj_set_style_text_color(back_ic, lv_color_white(), 0);
    lv_obj_center(back_ic);
    lv_obj_add_event_cb(back_btn, on_back_to_home_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *storage_lbl = lv_label_create(toolbar);
    lv_obj_set_style_text_font(storage_lbl, ui_font_sym(), 0);
    lv_obj_set_style_text_color(storage_lbl, lv_color_hex(COLOR_SECONDARY), 0);
    update_storage_summary(storage_lbl);

    /* 右侧：计数 + 排列切换 */
    lv_obj_t *right = lv_obj_create(toolbar);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(right, ui_scale_min(8), 0);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    g_count_label = lv_label_create(right);
    lv_obj_set_style_text_font(g_count_label, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(g_count_label, lv_color_hex(COLOR_SECONDARY), 0);

    g_arrange_btn = lv_button_create(right);
    lv_obj_set_size(g_arrange_btn, ui_scale_min(36), ui_scale_min(36));
    lv_obj_set_style_bg_color(g_arrange_btn, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_border_width(g_arrange_btn, 0, 0);
    lv_obj_remove_flag(g_arrange_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *arr_ic = lv_label_create(g_arrange_btn);
    lv_label_set_text(arr_ic, LV_SYMBOL_BARS);
    lv_obj_set_style_text_font(arr_ic, ui_font_sym(), 0);
    lv_obj_set_style_text_color(arr_ic, lv_color_white(), 0);
    lv_obj_center(arr_ic);
    lv_obj_add_event_cb(g_arrange_btn, on_arrange_toggle, LV_EVENT_CLICKED, NULL);

    /* 照片网格容器（仅纵向滚动：横向右滑交给 tileview 导航回主页） */
    g_grid = lv_obj_create(g_tile);
    lv_obj_set_width(g_grid, lv_pct(100));
    lv_obj_set_flex_grow(g_grid, 1);
    lv_obj_set_style_bg_color(g_grid, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(g_grid, ui_scale_min(8), 0);
    lv_obj_set_style_border_width(g_grid, 0, 0);
    lv_obj_set_style_pad_gap(g_grid, ui_scale_min(4), 0);
    /* 仅允许纵向滚动，横向手势冒泡到 tile 触发 tileview 右滑导航 */
    lv_obj_set_scroll_dir(g_grid, LV_DIR_VER);

    /* 全屏查看 overlay（叠在 tile 上，不在 lv_layer_top） */
    create_fullscreen_overlay(g_tile);

    /* 长按二级菜单 overlay（信息/删除） */
    create_context_menu_overlay(g_tile);

    /* 启动速度优化：不在此处同步扫描照片目录。
     * 首次进入相册时 on_album_click → page_album_refresh 会触发扫描，
     * 避免启动时扫描 DCIM 目录 + 加载缩略图阻塞 UI 创建。
     * 网格初始为空，进入相册时再填充。 */
    return g_tile;
}

void page_album_refresh(void)
{
    /* 退出相册时可能停留在全屏查看、视频播放或长按菜单界面，下次进入应回到相册主页面。
     * 这里先关闭所有 overlay/播放器，确保层级关系重置 */
    video_player_close();
    if (g_fullscreen && lv_obj_is_valid(g_fullscreen)) {
        lv_obj_add_flag(g_fullscreen, LV_OBJ_FLAG_HIDDEN);
    }
    if (g_context_menu && lv_obj_is_valid(g_context_menu)) {
        lv_obj_add_flag(g_context_menu, LV_OBJ_FLAG_HIDDEN);
    }
    g_context_idx = -1;

    /* 重新扫描目录并重建网格：每次进入相册时调用，
     * 确保新拍摄的照片/视频立即可见 */
    refresh_photo_list();
    rebuild_grid();
}

void page_album_destroy(void)
{
    /* 停止懒加载定时器，防止回调访问已销毁的 widget */
    lazy_load_stop();

    /* 先关闭可能正在运行的视频播放器，避免其定时器在 UI 树销毁后继续访问
     * 已释放的 overlay 对象。 */
    video_player_close();

    /* 全屏 overlay 和长按菜单 overlay 都是 g_tile 的子对象，
     * 会随 lv_obj_delete(g_scr) 自动删除。
     * 但为防止旋转/语言切换期间本函数与后续 g_scr 删除产生竞态（例如
     * g_fullscreen 已在事件中被删除但指针未清空），这里先做有效性校验，
     * 再显式删除并切断与 g_tile 的父子关系，确保不会重复释放。
     *
     * 注意：lv_obj_delete 内部会把对象从父对象的子对象链表中移除，因此
     * 后续 g_scr -> g_tile 删除时不会再次释放它。 */
    if (g_fullscreen && lv_obj_is_valid(g_fullscreen)) {
        lv_obj_add_flag(g_fullscreen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_delete(g_fullscreen);
    }
    if (g_context_menu && lv_obj_is_valid(g_context_menu)) {
        lv_obj_add_flag(g_context_menu, LV_OBJ_FLAG_HIDDEN);
        lv_obj_delete(g_context_menu);
    }
    g_fullscreen        = NULL;
    g_full_img          = NULL;
    g_video_placeholder = NULL;
    g_context_menu      = NULL;
    g_context_idx  = -1;
    g_tile         = NULL;
    g_grid         = NULL;
    g_count_label  = NULL;
    g_arrange_btn  = NULL;
    g_photo_count  = 0;
}
