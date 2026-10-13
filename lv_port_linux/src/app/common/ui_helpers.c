/**
 * @file ui_helpers.c
 * @brief 公共 UI 辅助函数实现
 */
#include "ui_helpers.h"
#include "../app_main.h"
#include "lvgl.h"
#if LV_USE_FREETYPE
    #include "src/libs/freetype/lv_freetype.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/statvfs.h>

/* ------------------------------------------------------------------ */
/* fonts                                                                */
/* ------------------------------------------------------------------ */
static lv_font_t *s_font_cjk = NULL; /* FreeType 创建的字体需要可写，不能加 const */
static lv_font_t *s_font_cjk_lg = NULL; /* 大号 CJK 字体（滚轮/标题用） */
static const lv_font_t *s_font_sym = NULL;

#if LV_USE_FREETYPE
/* 记录当前字体是否由 FreeType 创建，销毁时只释放 FreeType 字体，
 * 避免误删内置字体（如 lv_font_source_han_sans_sc_16_cjk）。 */
static bool s_font_cjk_is_freetype = false;
static bool s_font_cjk_lg_is_freetype = false;
#endif

/* 全局方向标志：1=横屏 800x480，0=竖屏 480x800（RK3566 默认）
 * 可通过环境变量 APP_LANDSCAPE=1 在启动前设置 */
int g_app_landscape = 0;

/* 初始化时从环境变量读取方向配置 */
__attribute__((constructor))
static void init_landscape_from_env(void)
{
    const char *env = getenv("APP_LANDSCAPE");
    if (env && (*env == '1' || *env == 'y' || *env == 'Y')) {
        g_app_landscape = 1;
    }
}

/* 尝试从指定路径加载 FreeType 字体，成功则同时创建 20px/28px 两个字号 */
static int try_load_font(const char *path)
{
#if LV_USE_FREETYPE
    if (!path || access(path, F_OK) != 0) return 0;

    /* 先创建两种字号，都成功后再统一赋值，避免只成功一种时泄漏。 */
    lv_font_t *cjk = lv_freetype_font_create(path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 20,
                                              LV_FREETYPE_FONT_STYLE_NORMAL);
    if (!cjk) return 0;

    lv_font_t *cjk_lg = lv_freetype_font_create(path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 28,
                                                 LV_FREETYPE_FONT_STYLE_NORMAL);
    if (!cjk_lg) {
        lv_freetype_font_delete(cjk);
        return 0;
    }

    s_font_cjk = cjk;
    s_font_cjk_lg = cjk_lg;
    s_font_cjk_is_freetype = true;
    s_font_cjk_lg_is_freetype = true;

    s_font_cjk->fallback = s_font_sym;
    s_font_cjk_lg->fallback = &lv_font_montserrat_28;
    LV_LOG_USER("CJK font loaded via FreeType: %s", path);
    return 1;
#else
    return 0;
#endif
}

/* 卸载 FreeType 字体。必须在所有使用这些字体的 UI 对象销毁后调用，
 * 否则删除字体时样式中仍保存字体指针，会导致 use-after-free。 */
void ui_font_unload(void)
{
#if LV_USE_FREETYPE
    if (s_font_cjk_is_freetype && s_font_cjk) {
        lv_freetype_font_delete(s_font_cjk);
        s_font_cjk_is_freetype = false;
    }
    if (s_font_cjk_lg_is_freetype && s_font_cjk_lg) {
        lv_freetype_font_delete(s_font_cjk_lg);
        s_font_cjk_lg_is_freetype = false;
    }
#endif
    s_font_cjk = NULL;
    s_font_cjk_lg = NULL;
    s_font_sym = NULL;
}

void ui_font_load(void)
{
    /* 先释放上一次创建的 FreeType 字体，防止重复加载导致内存泄漏。
     * 首次调用时指针均为 NULL，此调用为 no-op。 */
    ui_font_unload();

    /* 符号字体先就位，作为 CJK 字体的 fallback */
    s_font_sym = &lv_font_montserrat_20;

    /* 字体搜索路径优先级：
     * 1. 当前工作目录（PC 模拟器或启动脚本已 cd 到部署目录）
     * 2. app_get_app_dir() 自动检测的部署目录（/userdata/app 或 /app/app1）
     * 3. 系统字体目录
     * 这样无论用户把包部署到哪个目录都能正确加载字体。 */
    char app_font_path[256];
    const char *app_dir = app_get_app_dir();
    snprintf(app_font_path, sizeof(app_font_path), "%s/DroidSansFallbackFull.ttf", app_dir);

    static const char *ttf_paths[] = {
        "./DroidSansFallbackFull.ttf",
        NULL,  /* 占位：将由 app_font_path 填充 */
        "/userdata/app/DroidSansFallbackFull.ttf",
        "/app/app1/DroidSansFallbackFull.ttf",
        "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
        NULL
    };
    ttf_paths[1] = app_font_path;

    for (int i = 0; ttf_paths[i]; i++) {
        if (try_load_font(ttf_paths[i])) break;
    }

    if (!s_font_cjk) {
        LV_LOG_USER("FreeType failed, fallback to built-in CJK");
        s_font_cjk = (lv_font_t *)&lv_font_source_han_sans_sc_16_cjk;
    }
    if (!s_font_cjk_lg) {
        s_font_cjk_lg = (lv_font_t *)&lv_font_montserrat_28;
    }
}

const lv_font_t * ui_font_cjk(void)
{
    return s_font_cjk ? s_font_cjk : &lv_font_source_han_sans_sc_16_cjk;
}

const lv_font_t * ui_font_cjk_lg(void)
{
    return s_font_cjk_lg ? s_font_cjk_lg : &lv_font_montserrat_28;
}

const lv_font_t * ui_font_sym(void)
{
    return s_font_sym ? s_font_sym : &lv_font_montserrat_16;
}

/* ------------------------------------------------------------------ */
/* screen size helpers                                                  */
/* ------------------------------------------------------------------ */
int ui_get_screen_width(void)
{
    /* RK3566 真实分辨率：竖屏 480x800，横屏 800x480 */
    return g_app_landscape ? 800 : 480;
}

int ui_get_screen_height(void)
{
    return g_app_landscape ? 480 : 800;
}

int ui_is_landscape(void)
{
    return g_app_landscape;
}

/* 缩放函数：基于宽度 480 为基准（适配 RK3566 实际分辨率） */
int ui_scale(int base)
{
    int w = ui_get_screen_width();
    return base * w / 480;
}

/* 基于 min(宽, 高) 缩放：保证正方形/圆形元素在横竖屏下比例一致 */
int ui_scale_min(int base)
{
    int w = ui_get_screen_width();
    int h = ui_get_screen_height();
    int min = (w < h) ? w : h;
    return base * min / 480;
}

/* 基于宽度缩放 */
int ui_scale_w(int base)
{
    int w = ui_get_screen_width();
    return base * w / 480;
}

/* 基于高度缩放 */
int ui_scale_h(int base)
{
    int h = ui_get_screen_height();
    return base * h / 800;
}

/* ------------------------------------------------------------------ */
/* 横竖屏布局辅助                                                       */
/* ------------------------------------------------------------------ */
/* 判断是否为宽屏布局：横屏 800x480 视为宽屏（>= 720px）。
 * 宽屏下内容区限宽居中，窄屏下全屏宽度。 */
bool ui_is_wide_layout(void)
{
    return ui_get_screen_width() >= 720;
}

/* 获取内容区最大宽度：宽屏返回 720 像素（居中显示），
 * 窄屏返回 lv_pct(100)（全屏宽度）。
 * 配合 overlay 的 cross_place = LV_FLEX_ALIGN_CENTER 实现居中。 */
int ui_get_content_max_width(void)
{
    return ui_is_wide_layout() ? 720 : lv_pct(100);
}

/* ------------------------------------------------------------------ */
/* header                                                               */
/* ------------------------------------------------------------------ */
/* 通用 header：左侧返回箭头、中间标题、右侧×关闭按钮。
 * 布局：[← back] [    title (居中)    ] [× home]
 *   - back_cb  → 左侧返回箭头，返回上一级
 *   - home_cb  → 右侧×按钮，返回主屏幕；为 NULL 时用等宽透明占位保持标题居中
 * 两侧按钮等宽(36px)，标题 flex_grow(1) + text_align center，保证视觉居中。 */
lv_obj_t * ui_make_header(lv_obj_t *parent, const char *title,
                          lv_event_cb_t back_cb, void *back_user_data,
                          lv_event_cb_t home_cb, void *home_user_data)
{
    int btn_sz = ui_scale_min(36);

    lv_obj_t *header = lv_obj_create(parent);
    lv_obj_set_size(header, lv_pct(100), ui_scale(40));
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(header, ui_scale_min(8), 0);
    lv_obj_set_style_pad_gap(header, ui_scale_min(4), 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 左侧：返回箭头 ---- */
    lv_obj_t *back = lv_button_create(header);
    lv_obj_set_size(back, btn_sz, btn_sz);
    lv_obj_set_style_bg_color(back, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_remove_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    ui_attach_press_scale(back, 242);

    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(bl, ui_font_sym(), 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_center(bl);

    if (back_cb) {
        lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, back_user_data);
    }

    /* ---- 中间：标题（flex_grow 填充剩余空间，文字居中） ---- */
    lv_obj_t *title_label = lv_label_create(header);
    lv_label_set_text(title_label, title ? title : "");
    lv_obj_set_style_text_font(title_label, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(title_label, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_flex_grow(title_label, 1);
    lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
    /* 标题 label 不需要可滚动/可点击，避免拦截手势 */
    lv_obj_remove_flag(title_label, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* ---- 右侧：× 关闭按钮（返回主屏幕）或等宽占位 ---- */
    if (home_cb) {
        lv_obj_t *close_btn = lv_button_create(header);
        lv_obj_set_size(close_btn, btn_sz, btn_sz);
        lv_obj_set_style_bg_color(close_btn, lv_color_hex(COLOR_PANEL), 0);
        lv_obj_set_style_border_width(close_btn, 0, 0);
        lv_obj_remove_flag(close_btn, LV_OBJ_FLAG_SCROLLABLE);
        ui_attach_press_scale(close_btn, 242);

        lv_obj_t *cl = lv_label_create(close_btn);
        lv_label_set_text(cl, LV_SYMBOL_CLOSE);
        lv_obj_set_style_text_font(cl, ui_font_sym(), 0);
        lv_obj_set_style_text_color(cl, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_center(cl);

        lv_obj_add_event_cb(close_btn, home_cb, LV_EVENT_CLICKED, home_user_data);
    }
    else {
        /* 等宽透明占位：保证无 × 按钮时标题仍视觉居中 */
        lv_obj_t *spacer = lv_obj_create(header);
        lv_obj_set_size(spacer, btn_sz, btn_sz);
        lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(spacer, 0, 0);
        lv_obj_remove_flag(spacer, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }

    return header;
}

/* ------------------------------------------------------------------ */
/* card                                                                 */
/* ------------------------------------------------------------------ */
lv_obj_t * ui_create_card(lv_obj_t *parent, const char *icon,
                          const lv_font_t *icon_font,
                          const char *title, const char *subtitle,
                          lv_event_cb_t cb)
{
    int pad = ui_scale_min(12);

    lv_obj_t *card = lv_button_create(parent);
    /* 宽屏下卡片占 48% 宽度（双列网格），窄屏下全宽 */
    lv_obj_set_width(card, ui_is_wide_layout() ? lv_pct(48) : lv_pct(100));
    lv_obj_set_height(card, ui_scale_min(72));
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(card, pad, 0);
    lv_obj_set_style_pad_gap(card, pad, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_radius(card, ui_scale_min(12), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);  /* 性能：关闭阴影 */
    lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    /* 卡片按下轻微缩小反馈 */
    ui_attach_press_scale(card, 242);

    /* icon circle */
    int icon_size = ui_scale_min(40);
    lv_obj_t *icon_box = lv_obj_create(card);
    lv_obj_set_size(icon_box, icon_size, icon_size);
    lv_obj_set_style_bg_color(icon_box, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_radius(icon_box, icon_size / 2, 0);
    lv_obj_set_style_border_width(icon_box, 0, 0);
    /* LVGL v9: lv_obj_create 默认带 CLICKABLE，会拦截父按钮的点击事件。
     * 移除 CLICKABLE 让点击穿透到 card 按钮。 */
    lv_obj_remove_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *ic = lv_label_create(icon_box);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_font(ic, icon_font, 0);
    lv_obj_set_style_text_color(ic, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_text_align(ic, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(ic, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_center(ic);

    /* text area */
    lv_obj_t *text_box = lv_obj_create(card);
    lv_obj_set_height(text_box, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(text_box, 1);
    lv_obj_set_flex_flow(text_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(text_box, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_bg_opa(text_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(text_box, 0, 0);
    /* 同上：移除 CLICKABLE 让点击穿透到 card 按钮 */
    lv_obj_remove_flag(text_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *tl = lv_label_create(text_box);
    lv_label_set_text(tl, title);
    lv_obj_set_style_text_font(tl, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(tl, lv_color_hex(COLOR_TEXT), 0);

    if (subtitle && subtitle[0]) {
        lv_obj_t *sl = lv_label_create(text_box);
        lv_label_set_text(sl, subtitle);
        lv_obj_set_style_text_font(sl, ui_font_cjk(), 0);
        lv_obj_set_style_text_color(sl, lv_color_hex(COLOR_SECONDARY), 0);
    }

    return card;
}

/* ------------------------------------------------------------------ */
/* info row                                                             */
/* ------------------------------------------------------------------ */
lv_obj_t * ui_info_row(lv_obj_t *parent, const char *label)
{
    int pad = ui_scale_min(12);

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, ui_scale_min(56));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_radius(row, ui_scale_min(10), 0);
    lv_obj_set_style_pad_all(row, pad, 0);
    lv_obj_set_style_border_width(row, 0,0);
    lv_obj_set_style_shadow_width(row, 0, 0);  /* 性能：关闭阴影 */
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COLOR_SECONDARY), 0);

    lv_obj_t *v = lv_label_create(row);
    lv_obj_set_style_text_font(v, ui_font_cjk(), 0);
    lv_obj_set_style_text_color(v, lv_color_hex(COLOR_TEXT), 0);
    return v;
}

/* ------------------------------------------------------------------ */
/* sub page helpers                                                     */
/* ------------------------------------------------------------------ */
void ui_open_sub_page(lv_obj_t *overlay)
{
    if (overlay) {
        lv_obj_remove_flag(overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(overlay);
        ui_fade_in_overlay(overlay, 180);
    }
}

/* 通用手势返回回调包装 */
typedef struct {
    lv_dir_t dirs;
    lv_event_cb_t cb;
} gesture_user_data_t;

static void gesture_dir_cb(lv_event_t *e)
{
    gesture_user_data_t *ud = (gesture_user_data_t *)lv_event_get_user_data(e);
    if (!ud || !ud->cb) return;

    lv_obj_t *obj = lv_event_get_current_target(e);
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());

    const char *dir_name = "UNKNOWN";
    if      (dir == LV_DIR_LEFT)   dir_name = "LEFT";
    else if (dir == LV_DIR_RIGHT)  dir_name = "RIGHT";
    else if (dir == LV_DIR_TOP)    dir_name = "TOP";
    else if (dir == LV_DIR_BOTTOM) dir_name = "BOTTOM";

    int matched = (ud->dirs & dir) != 0;
    LV_LOG_USER("Gesture on %p: dir=%s(%d), listen_mask=%d, matched=%d",
                (void *)obj, dir_name, dir, ud->dirs, matched);
    if (matched) {
        ud->cb(e);
    }
}

static void gesture_cleanup_cb(lv_event_t *e)
{
    gesture_user_data_t *ud = (gesture_user_data_t *)lv_event_get_user_data(e);
    if (ud) {
        ud->dirs = 0;
        ud->cb = NULL;
        lv_free(ud);
    }
}

/* 多方向手势返回 */
void ui_attach_gesture(lv_obj_t *obj, lv_dir_t dirs, lv_event_cb_t close_cb)
{
    if (!obj || !close_cb || dirs == 0) return;

    /* 动态分配 per-object 上下文，避免静态池在 UI 重建/多对象场景下被覆盖，
     * 导致手势事件分发到已释放或错误的回调。 */
    gesture_user_data_t *ud = (gesture_user_data_t *)lv_malloc(sizeof(*ud));
    if (!ud) return;
    ud->dirs = dirs;
    ud->cb = close_cb;

    lv_obj_add_event_cb(obj, gesture_dir_cb, LV_EVENT_GESTURE, ud);
    /* 对象销毁时释放上下文，防止内存泄漏 */
    lv_obj_add_event_cb(obj, gesture_cleanup_cb, LV_EVENT_DELETE, ud);
    /* LVGL v9: lv_obj_create 默认带 GESTURE_BUBBLE（lv_obj.c:582），
     * indev_gesture() 会沿 GESTURE_BUBBLE 链向上冒泡直到找到没有该 flag 的对象，
     * 然后只向那个对象发送 LV_EVENT_GESTURE。
     * 因此必须在"想接收手势回调"的对象上移除 GESTURE_BUBBLE，
     * 让来自子对象的手势在本对象停止并触发回调。
     * 子对象默认有 GESTURE_BUBBLE，所以手势会从子对象冒泡到本对象。 */
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);

    LV_LOG_USER("Gesture attached: obj=%p, dirs=%d (R=%d), cb=0x%lx",
                (void *)obj, dirs, (dirs & LV_DIR_RIGHT) != 0,
                (unsigned long)(uintptr_t)close_cb);
}

/* 统一返回/关闭手势：右滑（LV_DIR_RIGHT）触发关闭回调。
 * 全应用统一手势约定 —— 右滑 = 返回上一级 / 关闭当前 overlay（iOS 风格）。
 * 适用于：设置页、WiFi、参数面板、选择器、变焦等所有 overlay。 */
void ui_attach_back_gesture(lv_obj_t *obj, lv_event_cb_t close_cb)
{
    ui_attach_gesture(obj, LV_DIR_RIGHT, close_cb);
}

/* 右滑关闭手势（LV_DIR_RIGHT）：语义同 ui_attach_back_gesture，显式命名。
 * 用于全屏页面（拍摄模式、相册全屏）右滑退出回主页。 */
void ui_attach_right_gesture(lv_obj_t *obj, lv_event_cb_t close_cb)
{
    ui_attach_gesture(obj, LV_DIR_RIGHT, close_cb);
}

/* ------------------------------------------------------------------ */
/* double-click / long-press helpers                                    */
/* ------------------------------------------------------------------ */
typedef struct {
    lv_obj_t     *obj;
    lv_event_cb_t cb;
    void         *user_data;
    uint32_t      last_click;
} double_click_ctx_t;

static void double_click_event_cb(lv_event_t *e)
{
    double_click_ctx_t *ctx = (double_click_ctx_t *)lv_event_get_user_data(e);
    if (!ctx || !ctx->cb) return;

    /* 基于注册对象（current_target）判断，允许子对象点击冒泡 */
    if (lv_event_get_current_target(e) != ctx->obj) return;

    uint32_t now = lv_tick_get();
    uint32_t elapsed = lv_tick_elaps(ctx->last_click);

    if (elapsed > 0 && elapsed < 400) {
        ctx->last_click = 0;
        LV_LOG_USER("Double click detected (elapsed=%dms)", elapsed);
        ctx->cb(e);
    } else {
        ctx->last_click = now;
    }
}

static void double_click_cleanup_cb(lv_event_t *e)
{
    double_click_ctx_t *ctx = (double_click_ctx_t *)lv_event_get_user_data(e);
    if (ctx) {
        ctx->obj = NULL;
        ctx->cb = NULL;
        lv_free(ctx);
    }
}

void ui_attach_double_click(lv_obj_t *obj, lv_event_cb_t cb, void *user_data)
{
    if (!obj || !cb) return;

    double_click_ctx_t *ctx = (double_click_ctx_t *)lv_malloc(sizeof(*ctx));
    if (!ctx) return;
    ctx->obj = obj;
    ctx->cb = cb;
    ctx->user_data = user_data;
    ctx->last_click = 0;

    lv_obj_add_event_cb(obj, double_click_event_cb, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(obj, double_click_cleanup_cb, LV_EVENT_DELETE, ctx);
}

typedef struct {
    lv_event_cb_t cb;
    void         *user_data;
} long_press_ctx_t;

static void long_press_event_cb(lv_event_t *e)
{
    long_press_ctx_t *ctx = (long_press_ctx_t *)lv_event_get_user_data(e);
    if (ctx && ctx->cb) {
        LV_LOG_USER("Long press detected");
        ctx->cb(e);
    }
}

static void long_press_cleanup_cb(lv_event_t *e)
{
    long_press_ctx_t *ctx = (long_press_ctx_t *)lv_event_get_user_data(e);
    if (ctx) {
        ctx->cb = NULL;
        lv_free(ctx);
    }
}

void ui_attach_long_press(lv_obj_t *obj, uint16_t press_time_ms,
                          lv_event_cb_t cb, void *user_data)
{
    if (!obj || !cb) return;

    long_press_ctx_t *ctx = (long_press_ctx_t *)lv_malloc(sizeof(*ctx));
    if (!ctx) return;
    ctx->cb = cb;
    ctx->user_data = user_data;

    /* LVGL v9 不支持 per-object press_time 样式；如需自定义长按阈值，
     * 应通过 lv_indev_set_long_press_time() 在输入设备层面设置。
     * 当前仅使用默认长按时间，保留参数以兼容调用签名。 */
    (void)press_time_ms;

    lv_obj_add_event_cb(obj, long_press_event_cb, LV_EVENT_LONG_PRESSED, ctx);
    lv_obj_add_event_cb(obj, long_press_cleanup_cb, LV_EVENT_DELETE, ctx);
}

/* ------------------------------------------------------------------ */
/* system helpers                                                       */
/* ------------------------------------------------------------------ */
void ui_read_file_line(const char *path, char *buf, size_t sz)
{
    buf[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fgets(buf, (int)sz, f)) {
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
    }
    fclose(f);
}

void ui_read_storage(const char *path, long long *total_mb,
                     long long *used_mb, int *ok)
{
    struct statvfs st;
    *ok = 0;
    *total_mb = 0;
    *used_mb = 0;
    if (!path || statvfs(path, &st) != 0) return;
    unsigned long long total = (unsigned long long)st.f_blocks * st.f_frsize;
    unsigned long long free  = (unsigned long long)st.f_bfree  * st.f_frsize;
    if (total == 0) return;
    *total_mb = (long long)(total / (1024 * 1024));
    *used_mb  = (long long)((total - free) / (1024 * 1024));
    *ok = 1;
}

/* 尝试多个挂载点，找到第一个可读且非只读的小容量存储
 * 返回 ok=1 表示找到 */
static int try_mount_points(const char *const *paths, int n,
                            long long *total, long long *used, char *found_path, size_t fpsz)
{
    for (int i = 0; i < n; i++) {
        long long t = 0, u = 0;
        int ok = 0;
        ui_read_storage(paths[i], &t, &u, &ok);
        if (ok && t > 0) {
            *total = t;
            *used  = u;
            if (found_path && fpsz > 0) {
                strncpy(found_path, paths[i], fpsz - 1);
                found_path[fpsz - 1] = '\0';
            }
            return 1;
        }
    }
    return 0;
}

/* 智能检测 SD 卡：尝试常见挂载点 */
int ui_detect_sd_card(long long *total_mb, long long *used_mb, char *out_path, size_t sz)
{
    static const char *const candidates[] = {
        "/mnt/sdcard",
        "/mnt/sd",
        "/run/media/sdcard",
        "/media/sdcard",
        "/sdcard",
        "/mnt/mmcblk0p1",
        NULL
    };
    return try_mount_points(candidates, sizeof(candidates)/sizeof(candidates[0]) - 1,
                            total_mb, used_mb, out_path, sz);
}

/* 智能检测内部存储 */
int ui_detect_internal_storage(long long *total_mb, long long *used_mb)
{
    char tmp[64] = {0};
    return try_mount_points((const char *const[]){"/"}, 1,
                            total_mb, used_mb, tmp, sizeof(tmp));
}

/* ------------------------------------------------------------------ */
/* animation helpers                                                  */
/* ------------------------------------------------------------------ */
void ui_attach_press_scale(lv_obj_t *obj, int scale_pressed_256)
{
    if (!obj) return;
    if (scale_pressed_256 < 128) scale_pressed_256 = 128;
    if (scale_pressed_256 > 280) scale_pressed_256 = 280;

    /* pivot 设在中心，缩放时以中心为基准 */
    lv_obj_set_style_transform_pivot_x(obj, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(obj, LV_PCT(50), 0);
    lv_obj_set_style_transform_scale(obj, 256, 0);
    /* 按下状态立即缩放，不设置 transition。
     * 原因：
     * 1. 按钮按下反馈是即时状态变化，不需要 120ms 过渡动画；
     * 2. 应用中存在相册缩略图等大量按钮（>64 个），静态 transition descriptor
     *    池容易循环覆盖仍在使用的描述符，导致样式指针悬空或内存异常；
     * 3. 消除 transition 动画系统在高频创建/销毁 UI（旋转/语言切换）时的压力。 */
    lv_obj_set_style_transform_scale(obj, scale_pressed_256, LV_STATE_PRESSED);
}

void ui_fade_in_overlay(lv_obj_t *obj, uint32_t time_ms)
{
    if (!obj) return;
    if (time_ms < 50) time_ms = 50;
    if (time_ms > 500) time_ms = 500;
    lv_obj_fade_in(obj, time_ms, 0);
}