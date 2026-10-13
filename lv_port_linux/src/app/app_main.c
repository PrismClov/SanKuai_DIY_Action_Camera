/**
 * @file app_main.c
 * @brief 主 UI 入口：基于模块化 pages/settings/common 构建
 *
 * 布局（tileview 坐标）：
 *   - quick(0,2)   下拉设置菜单（主页下滑进入）
 *   - home(0,1)    主屏幕（默认）
 *   - menu(0,0)    上滑功能菜单
 *   - album(1,1)   主页右滑进入相册页
 *
 * 设置 / WiFi 以全屏 overlay 弹出。
 *
 * 横竖屏：点击功能菜单"旋转"调用 app_ui_recreate()，内部用
 *         lv_display_set_rotation() 切换逻辑分辨率并重建 UI。
 *         DRM 后端使用 FULL 模式：LVGL 渲染到逻辑分辨率的 render buffer，
 *         drm_flush 将其旋转复制到物理 dumb buffer（因 SW draw unit 不支持
 *         矩阵旋转，FULL+flush旋转是唯一正确的方案），详见 lv_linux_drm.c。
 */
#include "app_main.h"
#include "common/ui_helpers.h"
#include "common/app_stress_test.h"
#include "common/app_i18n.h"
#include "common/device_config.h"
#include "common/board_detect.h"
#include "pages/page_home.h"
#include "pages/page_quick.h"
#include "pages/page_menu.h"
#include "pages/page_album.h"
#include "pages/page_aod.h"
#include "settings/settings_main.h"
#include "settings/settings_device.h"
#include "settings/settings_storage.h"
#include "settings/settings_language.h"
#include "settings/settings_auth.h"
#include "settings/settings_wireless.h"
#include "settings/settings_brightness.h"
#include "settings/settings_aod.h"
#ifndef APP_WIFI_DISABLED
#include "wifi/app_wifi.h"
#include "../wifi/wpa_ctrl_wrap.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "lvgl/lvgl.h"

/* ------------------------------------------------------------------ */
/* Debug instrumentation (multi-crash-rotation-ui)                    */
/* ------------------------------------------------------------------ */
// #region debug-point A:rotation-trace
/*
 * 调试日志开关：
 *   0（默认）= 完全禁用，dbg_log 编译为无操作。
 *             避免在真机运行时因 socket/文件 IO 导致卡顿、阻塞或崩溃。
 *   1        = 仅写入本地 /tmp/rotation_trace.log（无网络）。
 *   2        = 本地文件 + 网络 Debug Server（仅实验室环境使用）。
 *
 * 真机不稳定的主要原因之一：旧实现默认开启网络 socket，每次 dbg_log 都执行
 * connect/send/close。若在 UI 事件/定时器中高频率调用，200ms 超时会阻塞
 * LVGL 主线程，且 socket 错误路径不可靠，极易在启动/旋转时触发段错误。
 * 因此生产版本默认关闭，需要排查时再打开。
 */
#ifndef APP_DEBUG_LOG_LEVEL
#define APP_DEBUG_LOG_LEVEL 0
#endif

#if APP_DEBUG_LOG_LEVEL > 0
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>

static char s_dbg_url[128] = "http://127.0.0.1:7777/event";
static char s_dbg_session[64] = "multi-crash-rotation-ui";
#endif

/* 获取应用部署目录。优先级：
 * 1. APP_DIR 环境变量（由启动脚本或用户指定）
 * 2. /app/app1（RK3566 标准部署路径）
 * 3. /userdata/app（RK3566 泰山派实际部署路径）
 * 4. /root/app_k230（K230 默认部署路径，root 用户家目录）
 * 5. /app（K230 根分区部署历史路径）
 * 6. 当前工作目录
 * 返回值指向内部静态缓冲区，无需释放。 */
const char * app_get_app_dir(void)
{
    static char buf[128];
    const char *env = getenv("APP_DIR");
    if (env && env[0]) {
        strncpy(buf, env, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        return buf;
    }
    /* 自动检测常见部署路径：覆盖 RK3566/K230 不同部署方式 */
    if (access("/app/app1", F_OK) == 0) return "/app/app1";        /* RK3566 标准部署 */
    if (access("/userdata/app", F_OK) == 0) return "/userdata/app"; /* RK3566 泰山派 */
    if (access("/root/app_k230", F_OK) == 0) return "/root/app_k230"; /* K230 默认部署 */
    if (access("/app", F_OK) == 0) return "/app";                  /* K230 历史路径 */
    return ".";
}

#if APP_DEBUG_LOG_LEVEL > 0
static void dbg_load_env(void)
{
    char path[256];
    FILE *f = fopen(".dbg/multi-crash-rotation-ui.env", "r");
    if (!f) {
        snprintf(path, sizeof(path), "%s/.dbg/multi-crash-rotation-ui.env", app_get_app_dir());
        f = fopen(path, "r");
    }
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (strncmp(line, "DEBUG_SERVER_URL=", 17) == 0) {
            strncpy(s_dbg_url, line + 17, sizeof(s_dbg_url) - 1);
            s_dbg_url[sizeof(s_dbg_url) - 1] = '\0';
        } else if (strncmp(line, "DEBUG_SESSION_ID=", 17) == 0) {
            strncpy(s_dbg_session, line + 17, sizeof(s_dbg_session) - 1);
            s_dbg_session[sizeof(s_dbg_session) - 1] = '\0';
        }
    }
    fclose(f);
}
#endif

void dbg_log(const char *hypothesis, const char *location, const char *msg)
{
#if APP_DEBUG_LOG_LEVEL == 0
    (void)hypothesis; (void)location; (void)msg;
    return;
#else
    static int loaded = 0;
    if (!loaded) { dbg_load_env(); loaded = 1; }

    /* 同时追加到本地文件，死机后仍可读取。
     * 优先 /tmp（可读可写），失败则回退到应用部署目录。
     * 使用非缓冲写入并限制单条长度，避免失败路径阻塞。 */
    char log_path[256];
    FILE *lf = fopen("/tmp/rotation_trace.log", "a");
    if (!lf) {
        snprintf(log_path, sizeof(log_path), "%s/rotation_trace.log", app_get_app_dir());
        lf = fopen(log_path, "a");
    }
    if (lf) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        fprintf(lf, "[%lld.%03ld][%s][%s] %s\n",
                (long long)ts.tv_sec, ts.tv_nsec / 1000000,
                hypothesis, location, msg);
        fflush(lf);
        fclose(lf);
    }

#if APP_DEBUG_LOG_LEVEL >= 2
    const char *host = "127.0.0.1";
    int port = 7777;
    char url_path[64] = "/event";
    {
        char tmp[128];
        strncpy(tmp, s_dbg_url, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        char *p = strstr(tmp, "://");
        if (p) {
            host = p + 3;
            char *slash = strchr(host, '/');
        if (slash) {
            /* 先复制路径（含前导 '/'），再截断 host */
            strncpy(url_path, slash, sizeof(url_path) - 1);
            url_path[sizeof(url_path) - 1] = '\0';
            *slash = '\0';
        }
        char *colon = strchr(host, ':');
        if (colon) { *colon = '\0'; port = atoi(colon + 1); }
        }
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;

    /* 设置 fd 为非阻塞，避免 connect 阻塞 UI。 */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    /* 非阻塞 connect：立即返回，select 等待 50ms。 */
    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0 && errno == EINPROGRESS) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 50 * 1000 };
        rc = select(fd + 1, NULL, &fds, NULL, &tv);
        if (rc <= 0 || !FD_ISSET(fd, &fds)) {
            close(fd);
            return;
        }
        int soerr = 0;
        socklen_t len = sizeof(soerr);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
        if (soerr != 0) { close(fd); return; }
    } else if (rc < 0) {
        close(fd);
        return;
    }

    char body[512];
    snprintf(body, sizeof(body),
             "{\"sessionId\":\"%s\",\"runId\":\"pre\",\"hypothesisId\":\"%s\",\"location\":\"%s\",\"msg\":\"[DEBUG] %s\"}",
             s_dbg_session, hypothesis, location, msg);
    char req[768];
    int req_len = snprintf(req, sizeof(req),
                           "POST %s HTTP/1.1\r\nHost: %s:%d\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                           url_path, host, port, strlen(body), body);
    send(fd, req, req_len, MSG_NOSIGNAL);
    close(fd);
#endif /* APP_DEBUG_LOG_LEVEL >= 2 */
#endif /* APP_DEBUG_LOG_LEVEL > 0 */
}
// #endregion

/* ------------------------------------------------------------------ */
/* globals exported to page modules                                   */
/* ------------------------------------------------------------------ */
lv_obj_t *g_tv;
lv_obj_t *g_tile_home;
lv_obj_t *g_tile_quick;
lv_obj_t *g_tile_func;
lv_obj_t *g_tile_album;

/* g_app_landscape 定义见 ui_helpers.c（统一管理） */

/* ------------------------------------------------------------------ */
/* local globals                                                      */
/* ------------------------------------------------------------------ */
static lv_obj_t *g_scr;

/* ------------------------------------------------------------------ */
/* rotation persistence                                               */
/* ------------------------------------------------------------------ */
#ifndef APP_ROTATION_CFG_FILE
#define APP_ROTATION_CFG_FILE "/app/app1/rotation.cfg"
#endif

/* 读取持久化的横竖屏状态。优先级：配置文件 > APP_LANDSCAPE 环境变量 > 默认竖屏 */
void app_rotation_load(void)
{
    char cfg_path[256];
    snprintf(cfg_path, sizeof(cfg_path), "%s/rotation.cfg", app_get_app_dir());
    FILE *f = fopen(cfg_path, "r");
    if (f) {
        int val = 0;
        if (fscanf(f, "%d", &val) == 1) {
            g_app_landscape = val ? 1 : 0;
            LV_LOG_USER("Rotation loaded from %s: landscape=%d",
                        APP_ROTATION_CFG_FILE, g_app_landscape);
        }
        fclose(f);
    } else {
        /* 配置文件不存在时回退到环境变量（保持旧行为） */
        const char *env = getenv("APP_LANDSCAPE");
        if (env && (*env == '1' || *env == 'y' || *env == 'Y')) {
            g_app_landscape = 1;
            LV_LOG_USER("Rotation loaded from env APP_LANDSCAPE: landscape=%d",
                        g_app_landscape);
        }
    }
}

/* 保存当前横竖屏状态到配置文件 */
void app_rotation_save(void)
{
    char cfg_path[256];
    snprintf(cfg_path, sizeof(cfg_path), "%s/rotation.cfg", app_get_app_dir());
    FILE *f = fopen(cfg_path, "w");
    if (f) {
        fprintf(f, "%d\n", g_app_landscape);
        fclose(f);
        LV_LOG_USER("Rotation saved to %s: landscape=%d",
                    cfg_path, g_app_landscape);
    } else {
        LV_LOG_WARN("Failed to save rotation to %s", cfg_path);
    }
}


/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */

const lv_font_t * app_font_cjk(void)
{
    return ui_font_cjk();
}

const lv_font_t * app_font_sym(void)
{
    return ui_font_sym();
}

/* 返回主屏幕：关闭设置 overlay，将 tileview 切回主页 tile。
 * 子页面（storage/device/language/auth/wifi）在调用本函数前应先隐藏自身 overlay。 */
void app_return_home(void)
{
    /* 关闭设置主页 overlay */
    settings_page_close();

    /* 切回主页 tile */
    if (g_tv && g_tile_home) {
        lv_tileview_set_tile(g_tv, g_tile_home, LV_ANIM_ON);
    }
}

void app_ui_create(void)
{
    // #region debug-point A:create
    dbg_log("A", "app_main.c:app_ui_create", "start");
    // #endregion
    ui_font_load();

    /* 预加载设备配置与板子识别：提前读取 device.json 缓存到静态结构体，
     * 避免设置页首次打开时阻塞 UI；同时打印板子型号便于日志诊断。
     * 失败不阻塞启动（device_config 内部已做容错，返回空默认值）。 */
    board_detect();
    device_config_load();

    lv_display_t *disp = lv_display_get_default();

    /* 根据当前 display rotation 设置 g_app_landscape 标志。
     * g_app_landscape 用于 UI 布局决策（如网格列数、缩放比例）。
     *
     * 旋转由 lv_display_set_rotation() 控制（在 app_ui_recreate() 中调用），
     * 这里只是读取当前 rotation 状态并同步到 g_app_landscape。
     *
     * 注意：lv_display_get_horizontal_resolution() 返回的是逻辑分辨率
     * （考虑 rotation 后的值），可用于判断当前是横屏还是竖屏。 */
    if (disp) {
        int log_w = lv_display_get_horizontal_resolution(disp);
        int log_h = lv_display_get_vertical_resolution(disp);
        g_app_landscape = (log_w > log_h) ? 1 : 0;
        LV_LOG_USER("UI create: logical %dx%d, landscape=%d",
                    log_w, log_h, g_app_landscape);

        /* 注意：DRM 后端使用 FULL 模式 + flush 旋转复制（见 lv_linux_drm.c）。
         * 不再需要 lv_display_set_matrix_rotation()，因为 SW draw unit 不支持
         * 矩阵旋转（仅设置 layer->matrix 但 SW blend 不应用它，导致越界访问/死机）。
         * FULL 模式下 LVGL 渲染到逻辑分辨率的 render buffer，
         * drm_flush 负责旋转复制到物理 dumb buffer。 */
    }

#if !LV_USE_LINUX_DRM
    /* 非 DRM 后端（如 SDL）：首次创建时根据 g_app_landscape 设置分辨率。
     * 旋转时（app_ui_recreate）由调用方先 set_rotation 再调用本函数。 */
    if (disp && lv_display_get_rotation(disp) == LV_DISPLAY_ROTATION_0 && g_app_landscape) {
        /* SDL 横屏模式：设置物理分辨率为 800x480 */
        lv_display_set_resolution(disp, 800, 480);
    } else if (disp && lv_display_get_rotation(disp) == LV_DISPLAY_ROTATION_0 && !g_app_landscape) {
        lv_display_set_resolution(disp, 480, 800);
    }
#endif

    /* WiFi 初始化必须在 g_app_landscape 设置之后，
     * 因为 app_wifi_init() 内部调用 ui_scale_min() 等依赖方向标志的函数 */
#ifndef APP_WIFI_DISABLED
    app_wifi_init();
#endif

    g_scr = lv_obj_create(NULL);
    lv_screen_load(g_scr);
    lv_obj_set_style_bg_color(g_scr, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(g_scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(g_scr, LV_OBJ_FLAG_SCROLLABLE);

    g_tv = lv_tileview_create(g_scr);
    lv_obj_set_size(g_tv, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(g_tv, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_tv, 0, 0);
    lv_obj_set_style_pad_all(g_tv, 0, 0);

    /* tile 坐标：quick(0,2), home(0,1), menu(0,0), album(1,1) */
    g_tile_quick   = page_quick_create(g_tv);
    g_tile_home    = page_home_create(g_tv);
    g_tile_func    = page_menu_create(g_tv);
    g_tile_album   = page_album_create(g_tv);

    /* 默认显示主页面 */
    lv_tileview_set_tile(g_tv, g_tile_home, LV_ANIM_OFF);

    page_aod_create();
    settings_page_create();

    // #region debug-point A:create
    dbg_log("A", "app_main.c:app_ui_create", "done");
    // #endregion

    /* 不再创建状态栏（status bar），时间/WiFi/电池信息集成到主页 UI */
}

void app_ui_destroy(void)
{
    // #region debug-point A:destroy
    dbg_log("A", "app_main.c:app_ui_destroy", "start");
    // #endregion

    /* 1. 先停止所有页面级定时器，避免后续销毁 WiFi/设置模块时定时器回调
     *    访问已释放的资源。page_home 的 WiFi 状态定时器/录制定时器必须在
     *    其他资源释放前删除。 */
    page_home_destroy();
    page_album_destroy();
    page_menu_destroy();
    page_quick_destroy();
    page_aod_destroy();

    /* 2. 删除设置页 overlay（在 lv_layer_top() 上，不在 g_scr 下）。
     *    每个 settings 子模块的 overlay 都在 lv_layer_top() 上创建，
     *    必须逐一删除，否则旋转重建时会内存泄漏 + 旧句柄悬空。 */
    settings_page_destroy();
    settings_device_destroy();
    settings_storage_destroy();
    settings_language_destroy();
    settings_auth_destroy();
    settings_wireless_destroy();
    settings_brightness_destroy();
    settings_aod_destroy();

#ifndef APP_WIFI_DISABLED
    /* 3. 删除 WiFi overlay（也在 lv_layer_top() 上）。
     *    在页面定时器已删除之后执行，避免 status/scan/connect 定时器
     *    在 WiFi 上下文被重置期间触发。 */
    app_wifi_destroy();
#endif

    /* 4. 删除 screen（自动删除所有子对象，包括 tileview 及其 tile）。
     *    注意：page_album 的全屏 overlay 若仍作为 g_tile 子对象存在，
     *    会在此处随 g_tile 一起释放；page_album_destroy 已做去耦处理。 */
    if (g_scr) {
        lv_obj_delete(g_scr);
        g_scr = NULL;
    }

    /* 5. 重置全局句柄 */
    g_tv = NULL;
    g_tile_home = NULL;
    g_tile_quick = NULL;
    g_tile_func = NULL;
    g_tile_album = NULL;

    /* 6. 所有使用 FreeType 字体的 UI 对象已销毁，安全释放字体资源，
     *    避免反复旋转/切换语言时持续泄漏字体内存。 */
    ui_font_unload();

    // #region debug-point A:destroy
    dbg_log("A", "app_main.c:app_ui_destroy", "done");
    // #endregion
}

/* 异步重建状态 */
static int s_recreate_scheduled = 0;
static lv_display_rotation_t s_recreate_target_rot = LV_DISPLAY_ROTATION_0;
static bool s_recreate_active = false;

/* 旋转屏幕：使用 lv_display_set_rotation() 实现运行时横竖屏切换
 *
 * 原理：
 *   lv_display_set_rotation() 只改变逻辑分辨率（hor_res/ver_res 互换），
 *   不改变物理缓冲区。渲染流程：
 *   1. LVGL 渲染逻辑画面到 render buffer（FULL 模式，逻辑分辨率）
 *   2. drm_flush 将 render buffer 旋转复制到物理 dumb buffer（见 lv_linux_drm.c）
 *   3. 触摸坐标自动旋转（indev_pointer_proc）
 *
 * 过渡优化（平滑无卡顿 / 减少不必要重绘）：
 *   1. 先冻结失效（lv_display_enable_invalidation(false)），
 *      使销毁/重建期间的失效调用全部成为 no-op；
 *   2. 先销毁旧 UI（在旧分辨率下删除，避免 set_rotation 的 update_resolution
 *      去调整并失效"即将被删除的旧 UI 树"——纯属浪费）；
 *   3. 再 set_rotation（此时旧 screen 已删除，update_resolution 仅作用于
 *      sys_layer，开销最小）；
 *   4. 重建新 UI（按新逻辑分辨率布局）；
 *   5. 解冻失效并对新 screen 整屏 invalidate，
 *      确保切换后恰好产生一帧干净的全量渲染（无半截画面/花屏）。 */
void app_ui_recreate(lv_display_rotation_t target_rot)
{
    lv_display_t *disp = lv_display_get_default();
    if (!disp) {
        LV_LOG_WARN("No display, cannot rotate");
        s_recreate_active = false;
        return;
    }

    /* target_rot 必须是 0 或 90 */
    if (target_rot != LV_DISPLAY_ROTATION_0 && target_rot != LV_DISPLAY_ROTATION_90) {
        target_rot = LV_DISPLAY_ROTATION_0;
    }

    /* 标记重建窗口开始，供压力测试等异步任务识别并跳过该危险区间 */
    s_recreate_active = true;

    lv_display_rotation_t cur = lv_display_get_rotation(disp);
    (void)cur;
    LV_LOG_USER("Recreate UI: rotation %d -> %d", (int)cur, (int)target_rot);
    dbg_log("A", "app_main.c:app_ui_recreate", "rotation start");

    /* 1. 冻结失效：销毁/重建期间不产生任何中间渲染 */
    lv_display_enable_invalidation(disp, false);
    dbg_log("A", "app_main.c:app_ui_recreate", "invalidation disabled");

    /* 2. 先加载一个临时空白 screen，再销毁旧 UI。
     * 这样保证 act_scr 在旧 g_scr 被删除后仍指向有效对象，避免
     * lv_display_set_rotation() -> update_resolution() 期间访问悬空指针。
     * 临时 screen 在 invalidation 冻结期间加载，不会触发任何渲染。 */
    lv_obj_t *temp_scr = lv_obj_create(NULL);
    if (temp_scr) {
        lv_obj_set_style_bg_color(temp_scr, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(temp_scr, LV_OPA_COVER, 0);
        lv_screen_load(temp_scr);
    } else {
        LV_LOG_ERROR("Failed to create temp blank screen during recreate");
    }
    dbg_log("A", "app_main.c:app_ui_recreate", "temp blank screen loaded");

    /* 3. 销毁旧 UI（避免 set_rotation 浪费地去 resize/失效旧树） */
    app_ui_destroy();
    dbg_log("A", "app_main.c:app_ui_recreate", "old UI destroyed");

    /* 4. 设置目标旋转（旧 screen 已删除，update_resolution 开销最小） */
    lv_display_set_rotation(disp, target_rot);
    dbg_log("A", "app_main.c:app_ui_recreate", "rotation set");

    /* 5. 同步方向标志（必须在 app_ui_create 之前，供布局决策使用） */
    g_app_landscape = (target_rot == LV_DISPLAY_ROTATION_90);
    dbg_log("A", "app_main.c:app_ui_recreate", "landscape flag updated");

    /* 6. 重建 UI（按新逻辑分辨率布局） */
    app_ui_create();
    dbg_log("A", "app_main.c:app_ui_recreate", "new UI created");

    /* 7. 新 UI 已加载为 act_scr，删除临时 blank screen。
     * 若重建失败（g_scr == NULL）则保留 temp_scr，避免 act_scr 悬空。 */
    if (temp_scr) {
        if (g_scr) {
            lv_obj_delete(temp_scr);
            temp_scr = NULL;
        } else {
            LV_LOG_ERROR("app_ui_create failed, keeping temp blank screen");
        }
    }

    /* 8. 仅在 g_scr 有效时才启用失效、整屏失效并强制渲染一帧。
     * 避免 app_ui_create 失败时调用 lv_obj_invalidate(NULL) 触发断言
     * 或对不存在的 screen 调用 lv_refr_now() 导致崩溃。 */
    lv_display_enable_invalidation(disp, true);
    dbg_log("A", "app_main.c:app_ui_recreate", "invalidation enabled");

    if (g_scr) {
        lv_obj_t *active = lv_screen_active();
        if (active) {
            lv_obj_invalidate(active);
        }
        if (lv_layer_top()) {
            lv_obj_invalidate(lv_layer_top());
        }
        if (lv_layer_sys()) {
            lv_obj_invalidate(lv_layer_sys());
        }
        dbg_log("A", "app_main.c:app_ui_recreate", "screen and layers invalidated");

        /* 立即强制渲染一帧：消除"async_recreate_cb 返回 → 下一次
         * lv_timer_handler"的间隙里、显示仍停留在旧方向残影导致的
         * 用户感知到的花屏。 */
        lv_refr_now(disp);
        dbg_log("A", "app_main.c:app_ui_recreate", "forced first frame rendered");

        /* 9. 持久化当前方向，下次启动时恢复（语言切换时方向也会继承） */
        app_rotation_save();
    } else {
        /* 重建失败：至少让临时空白 screen 能渲染，避免黑屏/断言 */
        if (temp_scr) {
            lv_obj_invalidate(temp_scr);
            lv_refr_now(disp);
        }
        LV_LOG_ERROR("app_ui_create failed, rotation not saved");
    }

    LV_LOG_USER("Rotation complete: logical %dx%d",
                lv_display_get_horizontal_resolution(disp),
                lv_display_get_vertical_resolution(disp));
    dbg_log("A", "app_main.c:app_ui_recreate", "rotation complete");

    /* 重建窗口结束 */
    s_recreate_active = false;
}


/* 异步重建回调：由 lv_async_call 在下一个 timer_handler 周期调用。
 * 此时当前事件回调已返回，事件源对象不再被 LVGL 事件循环引用，
 * 可以安全销毁整个 UI 树。
 * data 指向目标旋转方向（int 值）。 */
static void async_recreate_cb(void * data)
{
    int * scheduled = (int *)data;
    /* data 实际指向 s_recreate_scheduled，先取出目标方向再重置标志 */
    lv_display_rotation_t target = s_recreate_target_rot;
    if (scheduled) *scheduled = 0;  /* 重置标志，允许后续再次调度 */
    LV_LOG_USER("Async UI recreate triggered, target rotation=%d", (int)target);
    app_ui_recreate(target);
}

void app_ui_recreate_async(void)
{
    /* 防止重复调度（例如快速连续点击旋转/语言切换） */
    if (s_recreate_scheduled) {
        LV_LOG_WARN("Async recreate already scheduled, skip");
        return;
    }

    lv_display_t *disp = lv_display_get_default();
    if (!disp) return;

    /* 切换旋转：0↔90 */
    lv_display_rotation_t cur = lv_display_get_rotation(disp);
    s_recreate_target_rot = (cur == LV_DISPLAY_ROTATION_0)
                            ? LV_DISPLAY_ROTATION_90
                            : LV_DISPLAY_ROTATION_0;

    s_recreate_scheduled = 1;
    lv_async_call(async_recreate_cb, &s_recreate_scheduled);
}

void app_ui_recreate_language_async(void)
{
    /* 防止重复调度 */
    if (s_recreate_scheduled) {
        LV_LOG_WARN("Async recreate already scheduled, skip");
        return;
    }

    lv_display_t *disp = lv_display_get_default();
    if (!disp) return;

    /* 语言切换时保持当前旋转方向不变 */
    s_recreate_target_rot = lv_display_get_rotation(disp);

    s_recreate_scheduled = 1;
    lv_async_call(async_recreate_cb, &s_recreate_scheduled);
}

bool app_ui_is_recreating(void)
{
    return s_recreate_active;
}

/* ------------------------------------------------------------------ */
/* 自动点击冒烟测试（开发调试用）                                       */
/* ------------------------------------------------------------------ */
/* 在 dummy / 无输入环境下模拟用户点击主要入口，捕获崩溃栈。
 * target:
 *   "album"  -> 点击相册按钮切换 tile
 *   "mode"   -> 打开拍摄模式弹层并关闭
 *   "param"  -> 打开参数设置弹层
 *   "zoom"   -> 打开变焦弹层
 *   "lang"   -> 切换语言（重建 UI）
 *   "bright" -> 打开亮度设置弹层
 *   "storage" -> 打开存储设置弹层
 *   "aod"    -> 打开熄屏显示页面
 *   "auth"   -> 打开认证信息页面
 */
static const char *s_auto_click_target = NULL;

static void app_auto_click_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (app_ui_is_recreating()) return;

    LV_LOG_USER("AUTO_CLICK: trigger %s", s_auto_click_target ? s_auto_click_target : "none");

    if (!s_auto_click_target) return;
    if (strcmp(s_auto_click_target, "album") == 0) {
        if (g_tv && g_tile_album && lv_obj_is_valid(g_tv) && lv_obj_is_valid(g_tile_album)) {
            lv_tileview_set_tile(g_tv, g_tile_album, LV_ANIM_ON);
        }
    } else if (strcmp(s_auto_click_target, "mode") == 0) {
        extern void page_home_stress_mode_overlay(int open);
        page_home_stress_mode_overlay(1);
    } else if (strcmp(s_auto_click_target, "param") == 0) {
        extern void page_home_stress_param_overlay(int open);
        page_home_stress_param_overlay(1);
    } else if (strcmp(s_auto_click_target, "lang") == 0) {
        extern void app_set_language(int lang);
        app_set_language(g_app_language ? 0 : 1);
    } else if (strcmp(s_auto_click_target, "bright") == 0) {
        extern void settings_brightness_open(void);
        settings_brightness_open();
    } else if (strcmp(s_auto_click_target, "storage") == 0) {
        extern void settings_storage_open(void);
        settings_storage_open();
    } else if (strcmp(s_auto_click_target, "aod") == 0) {
        extern void page_aod_open(void);
        page_aod_open();
    } else if (strcmp(s_auto_click_target, "zoom") == 0) {
        extern void page_home_stress_zoom_overlay(int open);
        page_home_stress_zoom_overlay(1);
    } else if (strcmp(s_auto_click_target, "auth") == 0) {
        extern void settings_auth_open(void);
        settings_auth_open();
    }
}

void app_auto_click_schedule(const char *target)
{
    if (!target) return;
    s_auto_click_target = target;
    LV_LOG_USER("AUTO_CLICK scheduled: %s (after 1000ms)", target);
    lv_timer_t *t = lv_timer_create(app_auto_click_timer_cb, 1000, NULL);
    lv_timer_set_repeat_count(t, 1);  /* 只触发一次，避免无限循环 */
}

