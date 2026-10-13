/**
 * @file board_detect.h
 * @brief 板子型号自动检测模块
 *
 * 通过 /proc/device-tree/compatible 与 uname 识别当前硬件平台，
 * 用于一份二进制兼容多板（RK3566 / K230）的运行时分发。
 *
 * 识别策略（按优先级）：
 *   1. /proc/device-tree/compatible 内容匹配
 *      - "rockchip,rk3566" → BOARD_RK3566
 *      - "canaan,k230" / "canaan,k230b" / "k230" → BOARD_K230
 *   2. uname.machine 回退
 *      - "aarch64" → BOARD_RK3566（仅 RK3566 使用 aarch64）
 *      - "riscv64" → BOARD_K230（仅 K230 使用 riscv64）
 *   3. 都失败 → BOARD_UNKNOWN
 */
#ifndef BOARD_DETECT_H
#define BOARD_DETECT_H

/* 支持的板子类型 */
typedef enum {
    BOARD_UNKNOWN = 0,
    BOARD_RK3566,        /* 泰山派 RK3566 (aarch64, Rockchip) */
    BOARD_K230,          /* Canaan K230 (riscv64, CanMV) */
} board_type_t;

/* 检测当前板子类型。结果缓存到内部静态变量，重复调用无额外开销。 */
board_type_t board_detect(void);

/* 获取板子的人类可读名称（"RK3566" / "K230" / "Unknown"） */
const char * board_detect_name(void);

/* 获取板子的架构名称（"aarch64" / "riscv64" / "unknown"） */
const char * board_detect_arch(void);

#endif /* BOARD_DETECT_H */
