/**
 * @file app_i18n.c
 * @brief 中英文切换实现
 *
 * tr(zh, en) 根据 g_app_language 返回对应字符串。
 * app_set_language() 设置语言后异步重建 UI（app_ui_recreate_async）。
 * 异步重建是必须的：语言切换在按钮点击事件回调中触发，
 * 直接调用 app_ui_recreate() 会销毁事件源对象，导致 LVGL 事件循环
 * 访问已释放内存（use-after-free）而死机。
 */
#include "app_i18n.h"
#include "../app_main.h"

#include "lvgl/lvgl.h"

/* 全局语言标志：0=中文, 1=English */
int g_app_language = 0;

const char * tr(const char *zh, const char *en)
{
    if (!zh) return en;
    if (!en) return zh;
    return g_app_language ? en : zh;
}

void app_set_language(int lang)
{
    if (lang < 0 || lang > 1) return;
    if (g_app_language == lang) return;

    LV_LOG_USER("Language switch: %d -> %d", g_app_language, lang);
    g_app_language = lang;

    /* 异步重建 UI 以应用新语言（避免在事件回调中销毁事件源）。
     * 使用 language_async 保持当前横竖屏方向不变，避免切语言时意外旋转。 */
    app_ui_recreate_language_async();
}
