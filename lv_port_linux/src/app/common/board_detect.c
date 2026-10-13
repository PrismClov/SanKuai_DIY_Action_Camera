/**
 * @file board_detect.c
 * @brief 板子型号自动检测实现
 */
#include "board_detect.h"
#include "../app_main.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>

/* 检测结果缓存 */
static board_type_t g_board = BOARD_UNKNOWN;
static int g_detected = 0;

/* 读取 /proc/device-tree/compatible 内容到 buf。
 * compatible 文件由多个 null 终止的字符串拼接而成，
 * 这里只取前 256 字节并按 null 分割为可读字符串。 */
static int read_compatible(char *buf, size_t buf_sz)
{
    if (!buf || buf_sz == 0) return 0;
    buf[0] = '\0';
    FILE *f = fopen("/proc/device-tree/compatible", "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, buf_sz - 1, f);
    fclose(f);
    if (n == 0) return 0;
    /* compatible 文件可能含多个 null 分隔的字符串。
     * 将 null 替换为 ',' 以便整体查看与匹配。 */
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == '\0' && i + 1 < n) buf[i] = ',';
    }
    buf[n] = '\0';
    return 1;
}

/* 检测板子类型：优先 device-tree，回退 uname.machine */
static board_type_t detect_board(void)
{
    /* 1. device-tree compatible 匹配（最可靠） */
    char compat[256] = {0};
    if (read_compatible(compat, sizeof(compat))) {
        /* 大小写不敏感匹配子串（compatible 可能含多个逗号分隔的值） */
        if (strstr(compat, "rk3566") || strstr(compat, "rk3568")) {
            return BOARD_RK3566;
        }
        if (strstr(compat, "k230")) {
            return BOARD_K230;
        }
    }

    /* 2. uname.machine 回退 */
    struct utsname u;
    if (uname(&u) == 0) {
        if (strcmp(u.machine, "aarch64") == 0) {
            /* RK3566 是项目内唯一 aarch64 平台 */
            return BOARD_RK3566;
        }
        if (strcmp(u.machine, "riscv64") == 0) {
            /* K230 是项目内唯一 riscv64 平台 */
            return BOARD_K230;
        }
    }

    return BOARD_UNKNOWN;
}

board_type_t board_detect(void)
{
    if (!g_detected) {
        g_board = detect_board();
        g_detected = 1;
        /* 首次检测打印日志，便于启动诊断（板子型号 + 架构） */
        LV_LOG_USER("板子识别: %s (%s)",
                    board_detect_name(), board_detect_arch());
    }
    return g_board;
}

const char * board_detect_name(void)
{
    switch (board_detect()) {
        case BOARD_RK3566: return "RK3566";
        case BOARD_K230:   return "K230";
        default:           return "Unknown";
    }
}

const char * board_detect_arch(void)
{
    switch (board_detect()) {
        case BOARD_RK3566: return "aarch64";
        case BOARD_K230:   return "riscv64";
        default:           return "unknown";
    }
}
