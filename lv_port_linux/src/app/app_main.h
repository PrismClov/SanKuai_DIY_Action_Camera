/**
 * @file app_main.h
 * @brief 主应用入口与全局字体
 */
#ifndef APP_MAIN_H
#define APP_MAIN_H

#include "lvgl/lvgl.h"

void app_ui_create(void);
void app_ui_destroy(void);

/* 重建 UI 并切换到指定旋转方向。
 * target_rot 为 LV_DISPLAY_ROTATION_0（竖屏）或 LV_DISPLAY_ROTATION_90（横屏）。
 * 旋转按钮用 app_ui_recreate_async()，语言切换用 app_ui_recreate_language_async()
 * 以保持当前方向不变。 */
void app_ui_recreate(lv_display_rotation_t target_rot);

/* 异步重建 UI：通过 lv_async_call 在下一个 lv_timer_handler 周期执行重建。
 * 必须用于事件回调中触发重建的场景（旋转、语言切换），
 * 避免在事件处理过程中销毁事件源对象导致 use-after-free 死机。 */
void app_ui_recreate_async(void);

/* 异步重建 UI，但保持当前旋转方向不变。供语言切换使用，避免切语言时把屏幕方向也切了。 */
void app_ui_recreate_language_async(void);

/* 查询当前是否正在执行 UI 重建（旋转/语言切换）。
 * 压力测试等异步任务可通过此接口跳过重建窗口期，避免访问未就绪或已销毁的 UI。 */
bool app_ui_is_recreating(void);

/* 横竖屏状态持久化：保存/读取 rotation.cfg */
void app_rotation_load(void);
void app_rotation_save(void);

/* 获取应用部署目录。优先级：APP_DIR 环境变量 > /userdata/app > /app/app1 > 当前目录 */
const char * app_get_app_dir(void);

const lv_font_t * app_font_cjk(void);
const lv_font_t * app_font_sym(void);

/* 返回主屏幕：关闭所有设置 overlay 并将 tileview 切回主页 tile。
 * 供各设置页右上角 × 按钮调用。 */
void app_return_home(void);

/* 调试日志上报：向 .dbg/<session>.env 指定的 Debug Server 发送事件。
 * 仅在调试会话中使用，正式发布前会移除。 */
void dbg_log(const char *hypothesis, const char *location, const char *msg);

/* tileview 与各 tile 全局句柄，供 pages 模块跳转使用 */
extern lv_obj_t *g_tv;
extern lv_obj_t *g_tile_home;
extern lv_obj_t *g_tile_quick;
extern lv_obj_t *g_tile_func;
extern lv_obj_t *g_tile_album;

/* 全局方向模式：0=竖屏 1=横屏（用于重建 UI 时决定布局尺寸） */
extern int g_app_landscape;

#endif /* APP_MAIN_H */
