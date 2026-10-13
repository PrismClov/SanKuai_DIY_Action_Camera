/**
 * @file settings_storage.h
 * @brief 存储管理设置页
 */
#ifndef SETTINGS_STORAGE_H
#define SETTINGS_STORAGE_H

void settings_storage_create(void);
void settings_storage_open(void);
void settings_storage_destroy(void);

/* 获取当前选中的存储位置：0=内置，1=SD 卡 */
int settings_storage_get_sel(void);

/* 获取当前选中存储的挂载路径 */
const char * settings_storage_get_path(void);

#endif /* SETTINGS_STORAGE_H */
