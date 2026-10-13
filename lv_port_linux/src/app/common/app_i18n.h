/**
 * @file app_i18n.h
 * @brief 中英文切换（i18n）模块
 *
 * 使用方式：
 *   lv_label_set_text(label, tr("设置", "Settings"));
 *
 * 切换语言：
 *   app_set_language(0);  // 切换为中文
 *   app_set_language(1);  // 切换为英文（触发 UI 重建）
 */
#ifndef APP_I18N_H
#define APP_I18N_H

/* 0 = 简体中文, 1 = English */
extern int g_app_language;

/** 根据当前语言返回对应字符串 */
const char * tr(const char *zh, const char *en);

/** 设置语言并触发 UI 重建 */
void app_set_language(int lang);

#endif /* APP_I18N_H */
