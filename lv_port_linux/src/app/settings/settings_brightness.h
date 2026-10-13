/**
 * @file settings_brightness.h
 * @brief 亮度设置页
 */
#ifndef SETTINGS_BRIGHTNESS_H
#define SETTINGS_BRIGHTNESS_H

void settings_brightness_create(void);
void settings_brightness_open(void);
void settings_brightness_destroy(void);
void settings_brightness_set_pct(int pct);

#endif /* SETTINGS_BRIGHTNESS_H */
