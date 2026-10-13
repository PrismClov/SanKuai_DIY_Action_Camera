/**
 * @file app_stress_test.h
 * @brief UI 稳定性压力测试开关
 *
 * 通过环境变量 APP_STRESS_TEST=1 启用，模拟高频旋转、模式弹层、WiFi 弹层等
 * 用户操作，用于验证修复后是否仍有闪退/花屏。
 */
#ifndef APP_STRESS_TEST_H
#define APP_STRESS_TEST_H

#include <stdbool.h>

/* 检查环境变量，返回是否启用压力测试 */
bool app_stress_test_enabled(void);

/* 启动/停止压力测试定时器 */
void app_stress_test_start(void);
void app_stress_test_stop(void);

#endif /* APP_STRESS_TEST_H */
