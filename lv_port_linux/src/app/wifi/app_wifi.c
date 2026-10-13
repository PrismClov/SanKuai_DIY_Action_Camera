/**
 * @file app_wifi.c
 * @brief WiFi 扫描/连接 overlay 页面
 *
 * 全中文；键盘按屏幕分辨率自适应；使用 tiny_ttf 字体支持常用中文显示。
 * 线程模型：仅 LVGL 线程调用 lv_*；wpa_ctrl 阻塞操作在 pthread 中完成。
 */
#include "app_wifi.h"
#include "../app_main.h"
#include "../common/ui_helpers.h"
#include "../common/app_i18n.h"
#include "../../wifi/wpa_ctrl_wrap.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lvgl/lvgl.h"

/* ------------------------------------------------------------------ */
/* theme colors (sync with app_main.c)                                */
/* ------------------------------------------------------------------ */
#define COLOR_BG        0x050505
#define COLOR_PANEL     0x181818
#define COLOR_ACCENT    0xFFBF00
#define COLOR_TEXT      0xFFFFFF
#define COLOR_SECONDARY 0xAAAAAA

/* ------------------------------------------------------------------ */
/* shared context                                                     */
/* ------------------------------------------------------------------ */
typedef struct {
    wifi_net_t  nets[WIFI_NET_MAX];
    int         count;
    volatile int scanning;
    volatile int scan_done;
    volatile int connecting;
    volatile int connect_done;
    volatile int connect_ok;          /* 1 ok, -1 fail */
    char        pending_ssid[WIFI_SSID_MAX];
    char        pending_psk[WIFI_PSK_MAX];
    char        result_ssid[WIFI_SSID_MAX];
    char        result_ip[32];
    uint32_t    generation;           /* 每次 init/destroy 自增，丢弃旧 worker 结果 */
    pthread_mutex_t mtx;
    pthread_t   worker_tid;
} wifi_ctx_t;

static wifi_ctx_t g_ctx;

/* ------------------------------------------------------------------ */
/* UI handles                                                         */
/* ------------------------------------------------------------------ */
static lv_obj_t *g_wifi_root;
static lv_obj_t *g_conn_label;
static lv_obj_t *g_list;
static lv_obj_t *g_scan_label;
static lv_obj_t *g_scan_spinner;

/* password dialog */
static lv_obj_t *g_pwd_backdrop;
static lv_obj_t *g_pwd_ta;
static lv_obj_t *g_pwd_show_label;
static char      g_pwd_target_ssid[WIFI_SSID_MAX];

/* ------------------------------------------------------------------ */
/* forward declarations                                               */
/* ------------------------------------------------------------------ */
static void ui_build(void);
static void scan_start(void);
static void connect_start(const char *ssid, const char *psk);
static void rebuild_list_ui(void);
static void pwd_dialog_open(const char *ssid);
static void pwd_dialog_close(void);
static void set_conn_label_state(const char *state_text);

static void *scan_worker_thread(void *arg);
static void *connect_worker_thread(void *arg);

static void scan_done_timer_cb(lv_timer_t *t);
static void connect_done_timer_cb(lv_timer_t *t);
static void status_timer_cb(lv_timer_t *t);

static void scan_btn_cb(lv_event_t *e);
static void net_row_cb(lv_event_t *e);
static void pwd_connect_cb(lv_event_t *e);
static void pwd_cancel_cb(lv_event_t *e);
static void pwd_return_home_cb(lv_event_t *e);
static void pwd_show_hide_cb(lv_event_t *e);
static void close_btn_cb(lv_event_t *e);

/* 异步初始化相关的前向声明（定义在 app_wifi_init 之后）。
 * app_wifi_init 中通过 pthread 后台执行 wpa_init()，完成后
 * 通过 lv_async_call 切回 UI 线程完成 ui_build + 定时器 + scan_start */
typedef struct {
    int      wpa_ok;     /* wpa_init() 返回值：0=成功，负数=失败 */
    uint32_t generation; /* init 时的 generation，用于丢弃过期结果 */
} wifi_init_ctx_t;

static void * wifi_init_worker(void *arg);
static void wifi_post_init_async_cb(void *arg);
static void wifi_post_init(int *wpa_ok, uint32_t generation);

/* ================================================================== */
/* public entry                                                       */
/* ================================================================== */
/* WiFi timer 句柄（用于旋转重建时避免重复创建） */
static lv_timer_t *s_scan_timer    = NULL;
static lv_timer_t *s_connect_timer = NULL;
static lv_timer_t *s_status_timer  = NULL;
static int s_wpa_inited = 0;
static int s_wifi_initializing = 0;  /* 防止 app_wifi_init 重入/并发 */
static uint32_t s_wifi_generation = 0; /* 当前 generation，与 g_ctx.generation 匹配 */

void app_wifi_init(void)
{
    // #region debug-point B:wifi
    dbg_log("B", "app_wifi.c:app_wifi_init", "start");
    // #endregion
    /* 如果 UI 已存在，说明已初始化过 */
    if (g_wifi_root) return;

    /* 防止重复进入：例如 app_ui_create 与 app_wifi_open 几乎同时调用 */
    if (s_wifi_initializing) {
        LV_LOG_WARN("app_wifi_init already in progress, skip");
        return;
    }
    s_wifi_initializing = 1;

    if (!s_wpa_inited) {
        pthread_mutex_init(&g_ctx.mtx, NULL);
        s_wpa_inited = 1;
    }

    /* 新一代：旧 worker 线程的结果将不再被新 UI 的定时器回调处理 */
    pthread_mutex_lock(&g_ctx.mtx);
    s_wifi_generation = ++g_ctx.generation;
    uint32_t my_generation = s_wifi_generation;
    pthread_mutex_unlock(&g_ctx.mtx);

    /* 性能优化：wpa_init() 会阻塞 UI 线程最多 5 秒（10×500ms 重试等待
     * wpa_supplicant 就绪）。原来在启动期间同步调用，导致 app 首屏迟迟无法渲染。
     * 改为后台线程执行 wpa_init()，完成后通过 lv_async_call 回到 UI 线程
     * 完成 ui_build + 定时器 + scan_start。这样 app 启动时 UI 立即渲染，
     * WiFi 连接在后台异步完成。 */
    wifi_init_ctx_t *ctx = (wifi_init_ctx_t *)malloc(sizeof(wifi_init_ctx_t));
    if (!ctx) {
        /* 内存不足：回退到同步初始化（保证功能可用） */
        wpa_init();
        wifi_post_init(NULL, my_generation);
        return;
    }
    ctx->generation = my_generation;
    ctx->wpa_ok = -1;

    pthread_t tid;
    if (pthread_create(&tid, NULL, wifi_init_worker, ctx) != 0) {
        /* 线程创建失败：回退到同步初始化 */
        free(ctx);
        wpa_init();
        wifi_post_init(NULL, my_generation);
        return;
    }
    pthread_detach(tid);  /* 自动回收，无需 join */
}

/* 后台线程：执行阻塞的 wpa_init()（打开 wpa_supplicant 控制套接字，
 * 最多重试 10×500ms=5s）。完成后通过 lv_async_call 切回 UI 线程。 */
static void * wifi_init_worker(void *arg)
{
    wifi_init_ctx_t *ctx = (wifi_init_ctx_t *)arg;
    ctx->wpa_ok = wpa_init();
    /* 切回 UI 线程完成后续初始化（lv_* 必须在主线程调用） */
    lv_async_call(wifi_post_init_async_cb, ctx);
    return NULL;
}

/* lv_async_call 回调：在 UI 线程执行，从 ctx 取出 wpa 结果并完成初始化 */
static void wifi_post_init_async_cb(void *arg)
{
    wifi_init_ctx_t *ctx = (wifi_init_ctx_t *)arg;
    if (!ctx) return;
    int wpa_ok = ctx->wpa_ok;
    uint32_t gen = ctx->generation;
    free(ctx);
    wifi_post_init(&wpa_ok, gen);
}

/* UI 线程：完成 WiFi 初始化的 UI 部分ui_build + 定时器 + scan_start）。
 * wpa_ok != NULL 时表示来自后台线程（已执行 wpa_init）；NULL 表示同步回退。
 * generation 用于丢弃过期结果（旋转重建期间旧后台线程仍在跑）。 */
static void wifi_post_init(int *wpa_ok, uint32_t generation)
{
    /* 丢弃过期结果：若期间发生 destroy/重新 init，generation 不匹配 */
    pthread_mutex_lock(&g_ctx.mtx);
    uint32_t cur_gen = g_ctx.generation;
    pthread_mutex_unlock(&g_ctx.mtx);
    if (generation != cur_gen) {
        LV_LOG_USER("wifi_post_init: stale generation %u != %u, discard",
                    generation, cur_gen);
        s_wifi_initializing = 0;
        return;
    }

    (void)wpa_ok;  /* wpa_init 失败也构建 UI（status_timer 会重试连接） */

    ui_build();

    /* 仅首次创建 timer，旋转重建时复用 */
    if (!s_scan_timer)    s_scan_timer    = lv_timer_create(scan_done_timer_cb, 200, NULL);
    if (!s_connect_timer) s_connect_timer = lv_timer_create(connect_done_timer_cb, 200, NULL);
    if (!s_status_timer)  s_status_timer  = lv_timer_create(status_timer_cb, 2000, NULL);

    scan_start();
    s_wifi_initializing = 0;
}

void app_wifi_open(void)
{
    app_wifi_init();
    if (g_wifi_root) {
        lv_obj_remove_flag(g_wifi_root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(g_wifi_root);
        scan_start();
    }
}

void app_wifi_close(void)
{
    if (g_wifi_root) {
        lv_obj_add_flag(g_wifi_root, LV_OBJ_FLAG_HIDDEN);
    }
}

static void close_btn_cb(lv_event_t *e)
{
    (void)e;
    if (g_wifi_root) lv_obj_add_flag(g_wifi_root, LV_OBJ_FLAG_HIDDEN);
}

/* × 按钮：关闭 WiFi 页 + 返回主屏幕 */
static void wifi_return_home_cb(lv_event_t *e)
{
    (void)e;
    if (g_wifi_root) lv_obj_add_flag(g_wifi_root, LV_OBJ_FLAG_HIDDEN);
    app_return_home();
}

/* 销毁 WiFi overlay（用于方向切换时重建 UI） */
void app_wifi_destroy(void)
{
    // #region debug-point B:wifi
    dbg_log("B", "app_wifi.c:app_wifi_destroy", "start");
    // #endregion
    /* 先删除定时器：定时器回调会访问 UI 句柄，必须在删除 UI 前移除，
     * 避免 UI 删除后定时器触发访问悬空指针。
     * 删除后 app_wifi_init() 会通过 if(!timer) 检查重新创建。 */
    if (s_scan_timer) {
        lv_timer_delete(s_scan_timer);
        s_scan_timer = NULL;
    }
    if (s_connect_timer) {
        lv_timer_delete(s_connect_timer);
        s_connect_timer = NULL;
    }
    if (s_status_timer) {
        lv_timer_delete(s_status_timer);
        s_status_timer = NULL;
    }

    if (g_wifi_root) {
        lv_obj_delete(g_wifi_root);
        g_wifi_root = NULL;
    }
    if (g_pwd_backdrop) {
        lv_obj_delete(g_pwd_backdrop);
        g_pwd_backdrop = NULL;
    }
    /* 重置所有 UI 句柄，避免 worker 线程通过定时器回调访问已释放内存 */
    g_conn_label    = NULL;
    g_list          = NULL;
    g_scan_label    = NULL;
    g_scan_spinner  = NULL;
    g_pwd_ta        = NULL;
    g_pwd_show_label = NULL;

    /* 重置 g_ctx 状态标志。
     *
     * 后台 scan/connect worker 线程是 pthread_detach 的，无法 join 等待。
     * 如果在 worker 仍在运行时调用 destroy，worker 会继续把结果写入 g_ctx。
     * 重置标志 + 清空缓冲后：
     *   ① 若 worker 完成后回调到已删除的定时器，无副作用（定时器已删，回调不触发）
     *   ② 下次 app_wifi_init() 启动新一轮 scan/connect 时，scan_start() 的
     *      `if (scanning || connecting)` 检查会因为我们清零标志而允许启动
     *   ③ 清空 nets/count 避免旧扫描结果显示在新界面上（视觉错乱）
     *
     * 注意：g_ctx 是 static 全局，worker 线程对它的访问是安全的（无 use-after-free）。
     * 真正有风险的是 worker 通过定时器回调访问 UI 句柄——这已通过先删定时器解决。 */
    pthread_mutex_lock(&g_ctx.mtx);
    g_ctx.scanning    = 0;
    g_ctx.scan_done   = 0;
    g_ctx.connecting  = 0;
    g_ctx.connect_done = 0;
    g_ctx.count       = 0;
    g_ctx.pending_ssid[0] = '\0';
    g_ctx.pending_psk[0]  = '\0';
    g_ctx.result_ssid[0]  = '\0';
    g_ctx.result_ip[0]    = '\0';
    pthread_mutex_unlock(&g_ctx.mtx);
}

/* ================================================================== */
/* UI construction                                                    */
/* ================================================================== */
static const lv_font_t *f_cjk(void)
{
    const lv_font_t *f = app_font_cjk();
    return f ? f : &lv_font_source_han_sans_sc_16_cjk;
}
static const lv_font_t *f_sym(void)
{
    const lv_font_t *f = app_font_sym();
    return f ? f : &lv_font_montserrat_16;
}

static void ui_build(void)
{
    g_wifi_root = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_wifi_root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(g_wifi_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_wifi_root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(g_wifi_root, 0, 0);
    lv_obj_set_style_border_width(g_wifi_root, 0, 0);
    lv_obj_set_style_radius(g_wifi_root, 0, 0);
    lv_obj_remove_flag(g_wifi_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_wifi_root, LV_OBJ_FLAG_HIDDEN);

    /* 左滑（LV_DIR_LEFT）关闭 overlay，与 settings overlay 行为一致 */
    ui_attach_back_gesture(g_wifi_root, close_btn_cb);

    lv_obj_t *root = g_wifi_root;
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    /* header：左返回箭头 + 中标题 + 右×返回主屏幕 */
    ui_make_header(root, tr("WiFi 网络", "WiFi Network"),
                   close_btn_cb, NULL, wifi_return_home_cb, NULL);

    /* 顶部工具行：刷新按钮 */
    lv_obj_t *tool_row = lv_obj_create(root);
    lv_obj_set_width(tool_row, lv_pct(100));
    lv_obj_set_height(tool_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tool_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tool_row, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(tool_row, ui_scale_min(8), 0);
    lv_obj_set_style_pad_gap(tool_row, ui_scale_min(8), 0);
    lv_obj_set_style_bg_opa(tool_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tool_row, 0, 0);
    lv_obj_remove_flag(tool_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *refresh = lv_button_create(tool_row);
    lv_obj_set_size(refresh, ui_scale_min(36), ui_scale_min(36));
    lv_obj_set_style_bg_color(refresh, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_border_width(refresh, 0, 0);
    lv_obj_remove_flag(refresh, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *rl = lv_label_create(refresh);
    lv_label_set_text(rl, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_font(rl, f_sym(), 0);
    lv_obj_set_style_text_color(rl, lv_color_white(), 0);
    lv_obj_center(rl);
    lv_obj_add_event_cb(refresh, scan_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *body = lv_obj_create(root);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    int body_pad = ui_scale_min(8);
    lv_obj_set_style_pad_all(body, body_pad, 0);
    lv_obj_set_style_pad_gap(body, body_pad, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    g_conn_label = lv_label_create(body);
    lv_label_set_text(g_conn_label, tr("正在初始化...", "Initializing..."));
    lv_label_set_long_mode(g_conn_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(g_conn_label, lv_pct(100));
    lv_obj_set_style_text_font(g_conn_label, f_cjk(), 0);
    lv_obj_set_style_text_color(g_conn_label, lv_color_white(), 0);

    lv_obj_t *srow = lv_obj_create(body);
    lv_obj_set_width(srow, lv_pct(100));
    lv_obj_set_height(srow, ui_scale_min(30));
    lv_obj_set_flex_flow(srow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(srow, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(srow, ui_scale_min(8), 0);
    lv_obj_set_style_pad_all(srow, ui_scale_min(2), 0);
    lv_obj_set_style_bg_opa(srow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(srow, 0, 0);
    lv_obj_remove_flag(srow, LV_OBJ_FLAG_SCROLLABLE);

    g_scan_label = lv_label_create(srow);
    lv_label_set_text(g_scan_label, tr("点击右上角刷新扫描", "Tap refresh to scan"));
    lv_obj_set_style_text_font(g_scan_label, f_cjk(), 0);
    lv_obj_set_style_text_color(g_scan_label, lv_color_white(), 0);

    g_scan_spinner = lv_bar_create(srow);
    lv_obj_set_size(g_scan_spinner, ui_scale_min(100), ui_scale_min(8));
    lv_bar_set_range(g_scan_spinner, 0, 100);
    lv_obj_add_flag(g_scan_spinner, LV_OBJ_FLAG_HIDDEN);

    g_list = lv_list_create(body);
    lv_obj_set_width(g_list, lv_pct(100));
    lv_obj_set_flex_grow(g_list, 1);
}

static void set_conn_label_state(const char *s)
{
    if (g_conn_label) lv_label_set_text(g_conn_label, s);
}

/* ================================================================== */
/* scan                                                               */
/* ================================================================== */
static void scan_start(void)
{
    pthread_mutex_lock(&g_ctx.mtx);
    if (g_ctx.scanning || g_ctx.connecting) {
        pthread_mutex_unlock(&g_ctx.mtx);
        return;
    }
    g_ctx.scanning = 1;
    g_ctx.scan_done = 0;
    pthread_mutex_unlock(&g_ctx.mtx);

    if (g_scan_label) {
        lv_label_set_text(g_scan_label, tr("扫描中...", "Scanning..."));
        lv_obj_remove_flag(g_scan_spinner, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(g_scan_spinner, 5, LV_ANIM_OFF);
    }

    pthread_create(&g_ctx.worker_tid, NULL, scan_worker_thread, NULL);
    pthread_detach(g_ctx.worker_tid);
}

static void *scan_worker_thread(void *arg)
{
    (void)arg;
    if (wpa_worker_init() == 0) {
        if (wpa_scan_start() == 0) {
            wifi_net_t tmp[WIFI_NET_MAX];
            int n = 0;
            if (wpa_scan_results(tmp, WIFI_NET_MAX, &n) == 0) {
                pthread_mutex_lock(&g_ctx.mtx);
                memcpy(g_ctx.nets, tmp, sizeof(tmp));
                g_ctx.count = n;
                g_ctx.scan_done = 1;
                g_ctx.scanning = 0;
                pthread_mutex_unlock(&g_ctx.mtx);
            } else {
                pthread_mutex_lock(&g_ctx.mtx);
                g_ctx.count = 0;
                g_ctx.scan_done = 1;
                g_ctx.scanning = 0;
                pthread_mutex_unlock(&g_ctx.mtx);
            }
        } else {
            pthread_mutex_lock(&g_ctx.mtx);
            g_ctx.count = 0;
            g_ctx.scan_done = 1;
            g_ctx.scanning = 0;
            pthread_mutex_unlock(&g_ctx.mtx);
        }
        wpa_worker_deinit();
    } else {
        pthread_mutex_lock(&g_ctx.mtx);
        g_ctx.count = 0;
        g_ctx.scan_done = 1;
        g_ctx.scanning = 0;
        pthread_mutex_unlock(&g_ctx.mtx);
    }
    return NULL;
}

static void scan_done_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!g_wifi_root || lv_obj_has_flag(g_wifi_root, LV_OBJ_FLAG_HIDDEN)) return;

    int done;
    uint32_t gen;
    pthread_mutex_lock(&g_ctx.mtx);
    done = g_ctx.scan_done;
    g_ctx.scan_done = 0;
    gen = g_ctx.generation;
    pthread_mutex_unlock(&g_ctx.mtx);

    /* 丢弃旧 worker 结果：generation 不匹配说明结果来自已被销毁的 UI 周期 */
    if (gen != s_wifi_generation) {
        LV_LOG_WARN("scan_done_timer: stale generation %lu, current %lu, ignore",
                    (unsigned long)gen, (unsigned long)s_wifi_generation);
        return;
    }

    if (!done) {
        if (g_scan_spinner) {
            int v = (lv_bar_get_value(g_scan_spinner) + 12) % 100;
            lv_bar_set_value(g_scan_spinner, v, LV_ANIM_OFF);
        }
        return;
    }

    if (g_scan_spinner) lv_obj_add_flag(g_scan_spinner, LV_OBJ_FLAG_HIDDEN);
    int count = g_ctx.count;
    if (g_scan_label) {
        if (count > 0)
            lv_label_set_text_fmt(g_scan_label, tr("找到 %d 个网络", "Found %d networks"), count);
        else
            lv_label_set_text(g_scan_label, tr("未找到网络", "No networks found"));
    }
    rebuild_list_ui();
}

static void rebuild_list_ui(void)
{
    if (!g_list) return;
    lv_obj_clean(g_list);

    pthread_mutex_lock(&g_ctx.mtx);
    int n = g_ctx.count;

    /* 按信号质量降序排序共享数组，使后续事件索引与 g_ctx.nets 一致 */
    for (int i = 0; i < n - 1; i++) {
        int best = i;
        for (int j = i + 1; j < n; j++)
            if (g_ctx.nets[j].quality > g_ctx.nets[best].quality) best = j;
        if (best != i) {
            wifi_net_t tmp = g_ctx.nets[i];
            g_ctx.nets[i] = g_ctx.nets[best];
            g_ctx.nets[best] = tmp;
        }
    }
    pthread_mutex_unlock(&g_ctx.mtx);

    if (n <= 0) {
        lv_obj_t *t = lv_list_add_text(g_list, tr("未找到网络", "No networks found"));
        lv_obj_set_style_text_font(t, f_cjk(), 0);
        lv_obj_set_style_text_color(t, lv_color_white(), 0);
        return;
    }

    for (int i = 0; i < n; i++) {
        pthread_mutex_lock(&g_ctx.mtx);
        wifi_net_t net = g_ctx.nets[i];
        pthread_mutex_unlock(&g_ctx.mtx);

        const char *ssid = net.ssid[0] ? net.ssid : tr("<隐藏>", "<Hidden>");
        lv_obj_t *btn = lv_list_add_button(g_list, NULL, ssid);
        lv_obj_set_size(btn, lv_pct(100), ui_scale_min(56));
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x202020), 0);
        lv_obj_set_style_radius(btn, ui_scale_min(8), 0);
        lv_obj_set_style_pad_all(btn, ui_scale_min(8), 0);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t *name = lv_obj_get_child(btn, 0);
        if (name) {
            lv_obj_set_style_text_font(name, f_cjk(), 0);
            lv_obj_set_style_text_color(name, lv_color_white(), 0);
        }

        lv_obj_t *info = lv_label_create(btn);
        lv_label_set_text_fmt(info, "%d%%  %s", net.quality,
                              net.secured ? tr("加密", "Secured") : tr("开放", "Open"));
        lv_obj_set_style_text_font(info, f_cjk(), 0);
        lv_obj_set_style_text_color(info, lv_color_hex(COLOR_SECONDARY), 0);

        lv_obj_add_event_cb(btn, net_row_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        ui_attach_press_scale(btn, 242);
    }
}

/* ================================================================== */
/* connect                                                            */
/* ================================================================== */
static void connect_start(const char *ssid, const char *psk)
{
    pthread_mutex_lock(&g_ctx.mtx);
    if (g_ctx.scanning || g_ctx.connecting) {
        pthread_mutex_unlock(&g_ctx.mtx);
        return;
    }
    strncpy(g_ctx.pending_ssid, ssid ? ssid : "", WIFI_SSID_MAX - 1);
    g_ctx.pending_ssid[WIFI_SSID_MAX - 1] = '\0';
    strncpy(g_ctx.pending_psk, psk ? psk : "", WIFI_PSK_MAX - 1);
    g_ctx.pending_psk[WIFI_PSK_MAX - 1] = '\0';
    g_ctx.connecting = 1;
    g_ctx.connect_done = 0;
    g_ctx.connect_ok = 0;
    g_ctx.result_ssid[0] = '\0';
    g_ctx.result_ip[0] = '\0';
    pthread_mutex_unlock(&g_ctx.mtx);

    char buf[WIFI_SSID_MAX + 32];
    snprintf(buf, sizeof(buf), tr("正在连接：%s...", "Connecting: %s..."), ssid ? ssid : "?");
    set_conn_label_state(buf);

    pthread_create(&g_ctx.worker_tid, NULL, connect_worker_thread, NULL);
    pthread_detach(g_ctx.worker_tid);
}

static void *connect_worker_thread(void *arg)
{
    (void)arg;
    char ssid[WIFI_SSID_MAX];
    char psk[WIFI_PSK_MAX];

    pthread_mutex_lock(&g_ctx.mtx);
    strncpy(ssid, g_ctx.pending_ssid, sizeof(ssid) - 1); ssid[sizeof(ssid)-1]='\0';
    strncpy(psk,  g_ctx.pending_psk,  sizeof(psk) - 1);  psk[sizeof(psk)-1]='\0';
    pthread_mutex_unlock(&g_ctx.mtx);

    int ok = -1;
    if (wpa_worker_init() == 0) {
        if (wpa_connect(ssid, psk) >= 0) {
            char buf[WPA_REPLY_MAX];
            for (int i = 0; i < 30; i++) {
                if (wpa_cmd("STATUS", buf, sizeof(buf)) == 0 &&
                    strstr(buf, "wpa_state=COMPLETED")) {
                    ok = 1;
                    char *p = strstr(buf, "ip_address=");
                    pthread_mutex_lock(&g_ctx.mtx);
                    if (p) {
                        strncpy(g_ctx.result_ip, p + 11, sizeof(g_ctx.result_ip) - 1);
                        g_ctx.result_ip[sizeof(g_ctx.result_ip)-1]='\0';
                        char *nl = strchr(g_ctx.result_ip, '\n');
                        if (nl) *nl = '\0';
                    }
                    strncpy(g_ctx.result_ssid, ssid, sizeof(g_ctx.result_ssid)-1);
                    g_ctx.result_ssid[sizeof(g_ctx.result_ssid)-1]='\0';
                    pthread_mutex_unlock(&g_ctx.mtx);
                    break;
                }
                usleep(500 * 1000);
            }
        }
        wpa_worker_deinit();
    }

    pthread_mutex_lock(&g_ctx.mtx);
    g_ctx.connect_ok = ok;
    g_ctx.connect_done = 1;
    g_ctx.connecting = 0;
    pthread_mutex_unlock(&g_ctx.mtx);
    return NULL;
}

static void connect_done_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!g_wifi_root || lv_obj_has_flag(g_wifi_root, LV_OBJ_FLAG_HIDDEN)) return;

    int done, ok;
    uint32_t gen;
    char ssid[WIFI_SSID_MAX] = {0}, ip[32] = {0};
    pthread_mutex_lock(&g_ctx.mtx);
    done = g_ctx.connect_done;
    ok = g_ctx.connect_ok;
    gen = g_ctx.generation;
    g_ctx.connect_done = 0;
    strncpy(ssid, g_ctx.result_ssid, sizeof(ssid) - 1);
    strncpy(ip, g_ctx.result_ip, sizeof(ip) - 1);
    pthread_mutex_unlock(&g_ctx.mtx);

    /* 丢弃旧 worker 结果 */
    if (gen != s_wifi_generation) {
        LV_LOG_WARN("connect_done_timer: stale generation %lu, current %lu, ignore",
                    (unsigned long)gen, (unsigned long)s_wifi_generation);
        return;
    }

    if (!done) return;

    char buf[WIFI_SSID_MAX + 48];
    if (ok == 1)
        snprintf(buf, sizeof(buf), tr("已连接：%s  IP %s", "Connected: %s  IP %s"),
                 ssid[0] ? ssid : "?", ip[0] ? ip : "?");
    else
        snprintf(buf, sizeof(buf), tr("连接失败：%s", "Connect failed: %s"),
                 ssid[0] ? ssid : "?");
    set_conn_label_state(buf);
}

static void status_timer_cb(lv_timer_t *t)
{
    (void)t;
    // #region debug-point B:wifi
    dbg_log("B", "app_wifi.c:status_timer_cb", "fire");
    // #endregion
    if (!g_wifi_root || lv_obj_has_flag(g_wifi_root, LV_OBJ_FLAG_HIDDEN)) return;

    char ssid[WIFI_SSID_MAX] = {0};
    char ip[32] = {0};
    char state[32] = {0};

    if (g_ctx.connecting) return;

    if (wpa_status(ssid, sizeof(ssid), ip, sizeof(ip),
                   state, sizeof(state)) != 0) {
        set_conn_label_state(tr("wpa_supplicant 未运行", "wpa_supplicant not running"));
        return;
    }

    char buf[WIFI_SSID_MAX + 48];
    if (strcmp(state, "COMPLETED") == 0 && ssid[0]) {
        snprintf(buf, sizeof(buf), tr("已连接：%s  IP %s", "Connected: %s  IP %s"),
                 ssid, ip[0] ? ip : "?");
    } else if (strcmp(state, "SCANNING") == 0) {
        snprintf(buf, sizeof(buf), "%s", tr("状态：扫描中", "Status: Scanning"));
    } else if (state[0]) {
        snprintf(buf, sizeof(buf), tr("状态：%s", "Status: %s"), state);
    } else {
        snprintf(buf, sizeof(buf), "%s", tr("未连接", "Not connected"));
    }
    set_conn_label_state(buf);
}

/* ================================================================== */
/* event handlers                                                     */
/* ================================================================== */
static void scan_btn_cb(lv_event_t *e)
{
    (void)e;
    if (wpa_init() == 0) {
        scan_start();
    } else {
        lv_label_set_text(g_scan_label, tr("wpa_supplicant 未运行", "wpa_supplicant not running"));
    }
}

static void net_row_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    pthread_mutex_lock(&g_ctx.mtx);
    if (idx < 0 || idx >= g_ctx.count) {
        pthread_mutex_unlock(&g_ctx.mtx);
        return;
    }
    wifi_net_t net = g_ctx.nets[idx];
    pthread_mutex_unlock(&g_ctx.mtx);

    if (net.ssid[0] == '\0') return;

    pwd_dialog_open(net.ssid);
}

/* ================================================================== */
/* password dialog (自适应屏幕分辨率，深色主题)                          */
/* ================================================================== */
static void pwd_dialog_open(const char *ssid)
{
    strncpy(g_pwd_target_ssid, ssid, sizeof(g_pwd_target_ssid) - 1);
    g_pwd_target_ssid[sizeof(g_pwd_target_ssid) - 1] = '\0';

    lv_display_t *disp = lv_display_get_default();
    int32_t dw = lv_display_get_horizontal_resolution(disp);
    int32_t dh = lv_display_get_vertical_resolution(disp);
    if (dw <= 0) dw = 800;
    if (dh <= 0) dh = 480;

    /* 全屏半透明背板，吞掉外部触摸 */
    g_pwd_backdrop = lv_obj_create(g_wifi_root);
    lv_obj_set_size(g_pwd_backdrop, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(g_pwd_backdrop, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(g_pwd_backdrop, LV_OPA_90, 0);
    lv_obj_set_style_border_width(g_pwd_backdrop, 0, 0);
    lv_obj_remove_flag(g_pwd_backdrop, LV_OBJ_FLAG_SCROLLABLE);

    /* 主容器：全屏，纵向 flex，键盘占剩余空间 */
    lv_obj_t *cont = lv_obj_create(g_pwd_backdrop);
    lv_obj_set_size(cont, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_style_pad_gap(cont, 0, 0);
    lv_obj_set_style_bg_color(cont, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_radius(cont, 0, 0);
    lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    /* header：左返回箭头 + 中标题 + 右×返回主屏幕 */
    ui_make_header(cont, tr("连接网络", "Connect Network"),
                   pwd_cancel_cb, NULL, pwd_return_home_cb, NULL);

    /* 输入区（固定高度，不挤压键盘） */
    lv_obj_t *input_area = lv_obj_create(cont);
    lv_obj_set_width(input_area, lv_pct(100));
    lv_obj_set_height(input_area, ui_scale_min(130));
    lv_obj_set_flex_flow(input_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(input_area, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    int input_pad = ui_scale_min(12);
    lv_obj_set_style_pad_all(input_area, input_pad, 0);
    lv_obj_set_style_pad_gap(input_area, input_pad, 0);
    lv_obj_set_style_bg_opa(input_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(input_area, 0, 0);
    lv_obj_remove_flag(input_area, LV_OBJ_FLAG_SCROLLABLE);

    g_pwd_ta = lv_textarea_create(input_area);
    lv_obj_set_width(g_pwd_ta, lv_pct(100));
    lv_obj_set_height(g_pwd_ta, ui_scale_min(54));
    char placeholder[128];
    snprintf(placeholder, sizeof(placeholder), tr("请输入 %s 的密码", "Enter password for %s"), ssid);
    lv_textarea_set_placeholder_text(g_pwd_ta, placeholder);
    lv_textarea_set_password_mode(g_pwd_ta, true);
    lv_textarea_set_password_bullet(g_pwd_ta, "*");
    lv_textarea_set_one_line(g_pwd_ta, true);
    lv_textarea_set_max_length(g_pwd_ta, 63);
    lv_obj_set_style_text_font(g_pwd_ta, f_cjk(), 0);
    lv_obj_set_style_text_color(g_pwd_ta, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_bg_color(g_pwd_ta, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_border_color(g_pwd_ta, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(g_pwd_ta, 2, 0);

    lv_obj_t *row = lv_obj_create(input_area);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, ui_scale_min(48));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row, ui_scale_min(12), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *show_btn = lv_button_create(row);
    lv_obj_set_size(show_btn, ui_scale_min(52), ui_scale_min(44));
    g_pwd_show_label = lv_label_create(show_btn);
    lv_label_set_text(g_pwd_show_label, LV_SYMBOL_EYE_OPEN);
    lv_obj_set_style_text_font(g_pwd_show_label, f_sym(), 0);
    lv_obj_center(g_pwd_show_label);
    lv_obj_add_event_cb(show_btn, pwd_show_hide_cb, LV_EVENT_CLICKED, NULL);
    ui_attach_press_scale(show_btn, 242);

    lv_obj_t *cancel_btn = lv_button_create(row);
    lv_obj_set_size(cancel_btn, ui_scale_min(90), ui_scale_min(44));
    lv_obj_t *cl = lv_label_create(cancel_btn);
    lv_label_set_text(cl, tr("取消", "Cancel"));
    lv_obj_set_style_text_font(cl, f_cjk(), 0);
    lv_obj_set_style_text_color(cl, lv_color_white(), 0);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x444444), 0);
    lv_obj_center(cl);
    lv_obj_add_event_cb(cancel_btn, pwd_cancel_cb, LV_EVENT_CLICKED, NULL);
    ui_attach_press_scale(cancel_btn, 242);

    lv_obj_t *conn_btn = lv_button_create(row);
    lv_obj_set_size(conn_btn, ui_scale_min(90), ui_scale_min(44));
    lv_obj_t *nl = lv_label_create(conn_btn);
    lv_label_set_text(nl, tr("连接", "Connect"));
    lv_obj_set_style_text_font(nl, f_cjk(), 0);
    lv_obj_set_style_bg_color(conn_btn, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_text_color(nl, lv_color_black(), 0);
    lv_obj_center(nl);
    lv_obj_add_event_cb(conn_btn, pwd_connect_cb, LV_EVENT_CLICKED, NULL);
    ui_attach_press_scale(conn_btn, 242);

    /* 键盘区：占满剩余高度，保证按键尺寸 */
    lv_obj_t *kb_area = lv_obj_create(cont);
    lv_obj_set_width(kb_area, lv_pct(100));
    lv_obj_set_flex_grow(kb_area, 1);
    lv_obj_set_style_pad_all(kb_area, ui_scale_min(6), 0);
    lv_obj_set_style_bg_color(kb_area, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_border_width(kb_area, 0, 0);
    lv_obj_remove_flag(kb_area, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *kb = lv_keyboard_create(kb_area);
    lv_obj_set_size(kb, lv_pct(100), lv_pct(100));
    lv_obj_set_style_text_font(kb, f_sym(), 0);
    lv_keyboard_set_textarea(kb, g_pwd_ta);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
}

static void pwd_dialog_close(void)
{
    if (g_pwd_backdrop) {
        lv_obj_delete(g_pwd_backdrop);
        g_pwd_backdrop = NULL;
        g_pwd_ta = NULL;
        g_pwd_show_label = NULL;
    }
}

static void pwd_connect_cb(lv_event_t *e)
{
    (void)e;
    if (!g_pwd_ta) return;
    const char *psk = lv_textarea_get_text(g_pwd_ta);
    char ssid[WIFI_SSID_MAX];
    strncpy(ssid, g_pwd_target_ssid, sizeof(ssid) - 1);
    ssid[sizeof(ssid) - 1] = '\0';
    pwd_dialog_close();
    connect_start(ssid, psk);
}

static void pwd_cancel_cb(lv_event_t *e)
{
    (void)e;
    pwd_dialog_close();
}

/* × 按钮：关闭密码弹窗 + 关闭 WiFi 页 + 返回主屏幕 */
static void pwd_return_home_cb(lv_event_t *e)
{
    (void)e;
    pwd_dialog_close();
    if (g_wifi_root) lv_obj_add_flag(g_wifi_root, LV_OBJ_FLAG_HIDDEN);
    app_return_home();
}

static void pwd_show_hide_cb(lv_event_t *e)
{
    (void)e;
    if (!g_pwd_ta) return;
    bool pwd = lv_textarea_get_password_mode(g_pwd_ta);
    lv_textarea_set_password_mode(g_pwd_ta, !pwd);
    if (g_pwd_show_label)
        lv_label_set_text(g_pwd_show_label,
                          pwd ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
}
