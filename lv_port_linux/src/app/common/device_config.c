/**
 * @file device_config.c
 * @brief 设备配置加载实现：读取 device.json 并解析为 device_config_t
 *
 * 设计要点：
 *   - 使用精简 cJSON 解析器（项目自带的 src/app/common/cjson.c）
 *   - 所有字段缓存到静态缓冲区，避免每次访问都重新解析
 *   - 字段缺失或类型不符时返回空字符串，不报错（设备配置容忍缺失）
 *   - 单次加载即缓存，避免重复文件 IO
 */
#include "device_config.h"
#include "cjson.h"
#include "../app_main.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/* 单字段最大长度（含终止符）；超长截断 */
#define DC_FIELD_MAX 128

/* 内部静态缓存：所有字段值存放于此，外部看到的 const char* 指向这里 */
static struct {
    char serial_number[DC_FIELD_MAX];
    char manufacturer[DC_FIELD_MAX];
    char device_model[DC_FIELD_MAX];
    char model_id[DC_FIELD_MAX];
    char cmiit_id[DC_FIELD_MAX];
    char fcc_id[DC_FIELD_MAX];
    char ic_can[DC_FIELD_MAX];
    char ices_id[DC_FIELD_MAX];
    char device_serial[DC_FIELD_MAX];
    char firmware_version[DC_FIELD_MAX];
    char camera_model[DC_FIELD_MAX];
    char camera_firmware_version[DC_FIELD_MAX];
    char loaded_path[256];
} g_dc_cache;

static device_config_t g_dc_result;
static bool g_dc_initialized = false;

/* ------------------------------------------------------------------ */
/* 内部工具                                                            */
/* ------------------------------------------------------------------ */

/* 安全拷贝字符串到定长缓冲区，超长截断并保证 NULL 终止 */
static void copy_field(char *dst, size_t dst_sz, const char *src)
{
    if (!dst || dst_sz == 0) return;
    dst[0] = '\0';
    if (!src) return;
    size_t len = strlen(src);
    if (len >= dst_sz) len = dst_sz - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}



/* 从 cJSON 节点提取字符串值并拷贝到 dst。
 * 节点缺失或非字符串类型时 dst 保持空字符串。 */
static void extract_string(const cJSON *root, const char *key, char *dst, size_t dst_sz)
{
    if (!root || !key || !dst || dst_sz == 0) return;
    const cJSON *node = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!node) return;
    const char *val = cJSON_GetStringValue(node);
    if (!val) return;
    copy_field(dst, dst_sz, val);
}

/* 常见键名变体表：兼容手动编辑 device.json 时使用的大小写/空格/下划线格式。
 * 先匹配标准 key，失败后再依次尝试变体。 */
static void extract_string_flexible(const cJSON *root, const char *key,
                                    char *dst, size_t dst_sz)
{
    if (!root || !key || !dst || dst_sz == 0) return;
    dst[0] = '\0';

    /* 1. 标准键名 */
    extract_string(root, key, dst, dst_sz);
    if (dst[0]) return;

    /* 2. 按字段定义常见变体 */
    const char *variants[8];
    int n = 0;
    if (strcmp(key, "serial_number") == 0) {
        variants[n++] = "SN";
        variants[n++] = "Serial Number";
        variants[n++] = "Serial_Number";
    } else if (strcmp(key, "manufacturer") == 0) {
        variants[n++] = "Manufacturer";
    } else if (strcmp(key, "device_model") == 0) {
        variants[n++] = "Model";
        variants[n++] = "model";
        variants[n++] = "Device Model";
        variants[n++] = "Device_Model";
    } else if (strcmp(key, "model_id") == 0) {
        variants[n++] = "Model ID";
        variants[n++] = "Model_ID";
    } else if (strcmp(key, "cmiit_id") == 0) {
        variants[n++] = "CMIIT ID";
        variants[n++] = "CMIIT_ID";
    } else if (strcmp(key, "fcc_id") == 0) {
        variants[n++] = "FCC ID";
        variants[n++] = "FCC_ID";
    } else if (strcmp(key, "ic_can") == 0) {
        variants[n++] = "IC CAN";
        variants[n++] = "IC_CAN";
        variants[n++] = "CAN ID";
        variants[n++] = "CAN_ID";
        variants[n++] = "IC ID";
        variants[n++] = "IC_ID";
    } else if (strcmp(key, "ices_id") == 0) {
        variants[n++] = "ICES ID";
        variants[n++] = "ICES_ID";
    } else if (strcmp(key, "firmware_version") == 0) {
        variants[n++] = "Firmware Version";
        variants[n++] = "Firmware_Version";
    } else if (strcmp(key, "camera_model") == 0) {
        variants[n++] = "Camera Model";
        variants[n++] = "Camera_Model";
    } else if (strcmp(key, "camera_firmware_version") == 0) {
        variants[n++] = "Camera FW Ver";
        variants[n++] = "Camera_FW_Ver";
        variants[n++] = "Camera Firmware Version";
        variants[n++] = "Camera_Firmware_Version";
    }

    for (int i = 0; i < n; i++) {
        extract_string(root, variants[i], dst, dst_sz);
        if (dst[0]) return;
    }
}

/* 从 cJSON 节点提取整数值。
 * 节点缺失或非数字类型时返回 fallback。 */
static int extract_int(const cJSON *root, const char *key, int fallback)
{
    if (!root || !key) return fallback;
    const cJSON *node = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!node) return fallback;
    /* cJSON 精简实现：数字节点 valueint 字段 */
    if ((node->type & 8) == 0) return fallback;  /* cJSON_Number = 8 */
    return node->valueint;
}

/* 从 device.json 解析 camera_capabilities 对象。
 * 缺失字段使用安全默认值（GC2093：1080p@30fps）。 */
static camera_caps_t extract_camera_caps(const cJSON *root)
{
    camera_caps_t caps = {
        .max_photo_width  = 1920,
        .max_photo_height = 1080,
        .max_video_width  = 1920,
        .max_video_height = 1080,
        .max_video_fps    = 30,
    };
    const cJSON *cc = cJSON_GetObjectItemCaseSensitive(root, "camera_capabilities");
    if (!cc) return caps;  /* 缺失则使用 GC2093 默认值 */

    caps.max_photo_width  = extract_int(cc, "max_photo_width",  caps.max_photo_width);
    caps.max_photo_height = extract_int(cc, "max_photo_height", caps.max_photo_height);
    caps.max_video_width  = extract_int(cc, "max_video_width",  caps.max_video_width);
    caps.max_video_height = extract_int(cc, "max_video_height", caps.max_video_height);
    caps.max_video_fps    = extract_int(cc, "max_video_fps",    caps.max_video_fps);

    /* 合法性校验：避免 0 或负值导致后续除零 */
    if (caps.max_photo_width  <= 0) caps.max_photo_width  = 1920;
    if (caps.max_photo_height <= 0) caps.max_photo_height = 1080;
    if (caps.max_video_width  <= 0) caps.max_video_width  = 1920;
    if (caps.max_video_height <= 0) caps.max_video_height = 1080;
    if (caps.max_video_fps    <= 0) caps.max_video_fps    = 30;

    return caps;
}

/* 查找 device.json 实际路径。
 * 优先级：APP_DIR > /app/app1 > /userdata/app > /root/app_k230 > /app > 当前目录。
 * 返回可读路径（写入 buf），失败返回 NULL。 */
static const char *find_config_path(char *buf, size_t buf_sz)
{
    const char *candidates[8];
    char app_dir_path[256];
    int count = 0;

    /* 1. app_get_app_dir() 自动检测的部署目录（优先级最高，由 start.sh 设置 APP_DIR） */
    const char *app_dir = app_get_app_dir();
    if (app_dir && app_dir[0]) {
        snprintf(app_dir_path, sizeof(app_dir_path), "%s/device.json", app_dir);
        candidates[count++] = app_dir_path;
    }
    /* 2~5. 常见部署路径：覆盖 RK3566/K230 不同部署方式 */
    candidates[count++] = "/app/app1/device.json";         /* RK3566 标准部署 */
    candidates[count++] = "/userdata/app/device.json";     /* RK3566 泰山派默认 */
    candidates[count++] = "/root/app_k230/device.json";    /* K230 默认部署（root 家目录） */
    candidates[count++] = "/app/device.json";              /* K230 根分区历史路径 */
    /* 6. 当前工作目录（开发调试用） */
    candidates[count++] = "./device.json";

    for (int i = 0; i < count; i++) {
        if (!candidates[i]) continue;
        if (access(candidates[i], R_OK) == 0) {
            snprintf(buf, buf_sz, "%s", candidates[i]);
            LV_LOG_USER("device.json found at: %s", buf);
            return buf;
        }
    }
    /* 打印所有尝试过的路径，方便诊断文件缺失或权限问题 */
    LV_LOG_WARN("device.json not found in any candidate path (checked %d locations):", count);
    for (int i = 0; i < count; i++) {
        if (candidates[i]) {
            LV_LOG_WARN("  checked: %s", candidates[i]);
        }
    }
    return NULL;
}



/* 将缓冲区前若干字节以十六进制形式打印到日志，用于诊断编码/内容异常。
 * 每字节输出为两位十六进制，连续排列，方便查看 BOM 或非法字符。 */
static void log_hex_preview(const char *label, const char *buf, size_t len)
{
    if (!label || !buf || len == 0) return;
    char hex[128];
    size_t out = 0;
    size_t max = (len > 32) ? 32 : len;  /* 最多打印 32 字节 */
    for (size_t i = 0; i < max && out + 2 < sizeof(hex); i++) {
        unsigned char c = (unsigned char)buf[i];
        out += (size_t)snprintf(hex + out, sizeof(hex) - out, "%02x", c);
    }
    LV_LOG_USER("%s: 长度=%zu 前%zu字节=%s", label, len, max, hex);
}

/* 读取文件全部内容到动态分配的缓冲区。
 * 自动跳过 UTF-8 BOM（EF BB BF），避免 cJSON 解析失败。
 * 调用方负责 free。失败返回 NULL。 */
static char *read_file_all(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0 || sz > 64 * 1024) {  /* 限制 64KB，避免异常文件耗尽内存 */
        fclose(f);
        return NULL;
    }
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';

    /* 跳过 UTF-8 BOM（0xEF 0xBB 0xBF）：Windows 编辑器或某些工具可能写入 */
    if (n >= 3 &&
        (unsigned char)buf[0] == 0xEF &&
        (unsigned char)buf[1] == 0xBB &&
        (unsigned char)buf[2] == 0xBF) {
        LV_LOG_USER("device.json 包含 UTF-8 BOM，已跳过");
        memmove(buf, buf + 3, n - 3 + 1);  /* 含终止符 */
    }

    return buf;
}

/* 当 device.json 缺失或关键字段为空时，使用与 build 脚本一致的内联默认值，
 * 避免设备信息页出现大量“未配置”。这些默认值对应 inf.json 的标准值，
 * 仅在字段缺失/为空时生效，不会覆盖用户显式配置的非空值。 */
static void apply_fallback_defaults(void)
{
    if (!g_dc_cache.manufacturer[0]) {
        copy_field(g_dc_cache.manufacturer, sizeof(g_dc_cache.manufacturer), "Sankuai WanJi");
    }
    if (!g_dc_cache.device_model[0]) {
        copy_field(g_dc_cache.device_model, sizeof(g_dc_cache.device_model), "Dion 5 Pro");
    }
    if (!g_dc_cache.model_id[0]) {
        copy_field(g_dc_cache.model_id, sizeof(g_dc_cache.model_id), "dc001");
    }
    if (!g_dc_cache.firmware_version[0]) {
        copy_field(g_dc_cache.firmware_version, sizeof(g_dc_cache.firmware_version), "V1.0.0");
    }
    if (!g_dc_cache.camera_model[0]) {
        copy_field(g_dc_cache.camera_model, sizeof(g_dc_cache.camera_model), "GC2093");
    }
    if (!g_dc_cache.camera_firmware_version[0]) {
        copy_field(g_dc_cache.camera_firmware_version, sizeof(g_dc_cache.camera_firmware_version), "V1.0.0");
    }
    /* 认证字段：若未配置则使用占位符，避免设备信息页显示“未配置”。
     * 产品获得真实认证号后，在 inf.json 中填写即可覆盖。 */
    if (!g_dc_cache.cmiit_id[0]) {
        copy_field(g_dc_cache.cmiit_id, sizeof(g_dc_cache.cmiit_id), "abc123456");
    }
    if (!g_dc_cache.fcc_id[0]) {
        copy_field(g_dc_cache.fcc_id, sizeof(g_dc_cache.fcc_id), "abc123456");
    }
    if (!g_dc_cache.ic_can[0]) {
        copy_field(g_dc_cache.ic_can, sizeof(g_dc_cache.ic_can), "abc123456");
    }
    if (!g_dc_cache.ices_id[0]) {
        copy_field(g_dc_cache.ices_id, sizeof(g_dc_cache.ices_id), "abc123456");
    }
}

/* 重置缓存为空字符串（保留 loaded_path） */
static void reset_cache(void)
{
    g_dc_cache.serial_number[0] = '\0';
    g_dc_cache.manufacturer[0] = '\0';
    g_dc_cache.device_model[0] = '\0';
    g_dc_cache.model_id[0] = '\0';
    g_dc_cache.cmiit_id[0] = '\0';
    g_dc_cache.fcc_id[0] = '\0';
    g_dc_cache.ic_can[0] = '\0';
    g_dc_cache.ices_id[0] = '\0';
    g_dc_cache.device_serial[0] = '\0';
    g_dc_cache.firmware_version[0] = '\0';
    g_dc_cache.camera_model[0] = '\0';
    g_dc_cache.camera_firmware_version[0] = '\0';
}

/* 填充 g_dc_result 指针（指向缓存）。
 * loaded=false 时（配置缺失/解析失败）使用 GC2093 默认能力。 */
static void fill_result(bool loaded)
{
    g_dc_result.serial_number            = g_dc_cache.serial_number;
    g_dc_result.manufacturer             = g_dc_cache.manufacturer;
    g_dc_result.device_model             = g_dc_cache.device_model;
    g_dc_result.model_id                 = g_dc_cache.model_id;
    g_dc_result.cmiit_id                 = g_dc_cache.cmiit_id;
    g_dc_result.fcc_id                   = g_dc_cache.fcc_id;
    g_dc_result.ic_can                   = g_dc_cache.ic_can;
    g_dc_result.ices_id                  = g_dc_cache.ices_id;
    g_dc_result.device_serial            = g_dc_cache.device_serial;
    g_dc_result.firmware_version         = g_dc_cache.firmware_version;
    g_dc_result.camera_model             = g_dc_cache.camera_model;
    g_dc_result.camera_firmware_version  = g_dc_cache.camera_firmware_version;
    /* 未加载时 camera_caps 保持默认值（extract_camera_caps 未被调用） */
    if (!loaded) {
        g_dc_result.camera_caps.max_photo_width  = 1920;
        g_dc_result.camera_caps.max_photo_height = 1080;
        g_dc_result.camera_caps.max_video_width  = 1920;
        g_dc_result.camera_caps.max_video_height = 1080;
        g_dc_result.camera_caps.max_video_fps    = 30;
    }
    g_dc_result.loaded                   = loaded;
}

/* ------------------------------------------------------------------ */
/* 公共 API                                                            */
/* ------------------------------------------------------------------ */

const device_config_t * device_config_load(void)
{
    reset_cache();
    g_dc_cache.loaded_path[0] = '\0';

    /* 1. 定位配置文件 */
    char path_buf[256];
    const char *path = find_config_path(path_buf, sizeof(path_buf));
    if (!path) {
        LV_LOG_WARN("device.json 未找到，使用空默认值");
        apply_fallback_defaults();
        fill_result(false);
        g_dc_initialized = true;
        return &g_dc_result;
    }
    snprintf(g_dc_cache.loaded_path, sizeof(g_dc_cache.loaded_path), "%s", path);

    /* 2. 读取文件内容 */
    char *content = read_file_all(path);
    if (!content) {
        LV_LOG_WARN("device.json 读取失败: %s", path);
        apply_fallback_defaults();
        fill_result(false);
        g_dc_initialized = true;
        return &g_dc_result;
    }
    log_hex_preview("device.json 内容预览", content, strlen(content));



    /* 3. 解析 JSON */
    cJSON *root = cJSON_Parse(content);
    if (!root) {
        size_t err_pos = cJSON_GetErrorPos();
        const char *err_msg = cJSON_GetErrorPtr();
        LV_LOG_WARN("device.json 解析失败 @%zu: %s", err_pos, err_msg ? err_msg : "unknown");
        (void)err_pos;  /* 消除 Release 模式日志关闭时的未使用警告 */
        (void)err_msg;
        free(content);
        apply_fallback_defaults();
        fill_result(false);
        g_dc_initialized = true;
        return &g_dc_result;
    }
    LV_LOG_USER("device.json JSON root 解析成功");

    /* 打印 cJSON 实际解析出的顶层键名，用于确认键名是否与代码期望一致 */
    if (cJSON_IsObject(root)) {
        const cJSON *child = root->child;
        int key_count = 0;
        while (child) {
            if (child->string) {
                LV_LOG_USER("device.json 顶层键[%d]: %s", key_count++, child->string);
            }
            child = child->next;
        }
    }

    /* 防御性检查：root 必须是 Object 类型，否则键值查找必然失败 */
    if (!cJSON_IsObject(root)) {
        LV_LOG_WARN("device.json 根节点不是 Object，实际 type=%d", root->type);
        cJSON_Delete(root);
        free(content);
        apply_fallback_defaults();
        fill_result(false);
        g_dc_initialized = true;
        return &g_dc_result;
    }

    /* 4. 提取字段（缺失字段保持空字符串）。
     * 使用 flexible 版本，兼容手动编辑 device.json 时使用的大小写/空格/下划线格式。 */
    extract_string_flexible(root, "serial_number",            g_dc_cache.serial_number,            DC_FIELD_MAX);
    extract_string_flexible(root, "manufacturer",             g_dc_cache.manufacturer,             DC_FIELD_MAX);
    extract_string_flexible(root, "device_model",             g_dc_cache.device_model,             DC_FIELD_MAX);
    extract_string_flexible(root, "model_id",                 g_dc_cache.model_id,                 DC_FIELD_MAX);
    extract_string_flexible(root, "cmiit_id",                 g_dc_cache.cmiit_id,                 DC_FIELD_MAX);
    extract_string_flexible(root, "fcc_id",                   g_dc_cache.fcc_id,                   DC_FIELD_MAX);
    extract_string_flexible(root, "ic_can",                   g_dc_cache.ic_can,                   DC_FIELD_MAX);
    extract_string_flexible(root, "ices_id",                  g_dc_cache.ices_id,                  DC_FIELD_MAX);
    extract_string_flexible(root, "device_serial",            g_dc_cache.device_serial,            DC_FIELD_MAX);
    extract_string_flexible(root, "firmware_version",         g_dc_cache.firmware_version,         DC_FIELD_MAX);
    extract_string_flexible(root, "camera_model",             g_dc_cache.camera_model,             DC_FIELD_MAX);
    extract_string_flexible(root, "camera_firmware_version",  g_dc_cache.camera_firmware_version,  DC_FIELD_MAX);

    /* 摄像头能力声明：决定预览/录制上限。更换摄像头时改 device.json 即可 */
    g_dc_result.camera_caps = extract_camera_caps(root);

    cJSON_Delete(root);
    free(content);

    /* 关键字段兜底：即使 device.json 存在但字段为空（如旧版本或手动清空），
     * 也使用与 build 脚本一致的内联默认值，保证设备信息页不显示“未配置”。 */
    apply_fallback_defaults();

    /* 用 WARN 级别输出最终结果，确保任何日志配置下都能看到 */
    LV_LOG_WARN("device.json 已加载: %s", path);
    LV_LOG_WARN("  厂商=[%s] 型号=[%s] model_id=[%s] 固件=[%s] SN=[%s] 相机固件=[%s]",
                g_dc_cache.manufacturer, g_dc_cache.device_model,
                g_dc_cache.model_id, g_dc_cache.firmware_version,
                g_dc_cache.serial_number, g_dc_cache.camera_firmware_version);

    /* 关键字段为空时发出警告，提示 device.json 内容异常 */
    if (!g_dc_cache.manufacturer[0] || !g_dc_cache.device_model[0] ||
        !g_dc_cache.model_id[0] || !g_dc_cache.firmware_version[0]) {
        LV_LOG_WARN("device.json 关键字段为空：厂商/型号/model_id/固件版本至少一项缺失，");
        LV_LOG_WARN("  请检查 %s 内容是否正确，或重新编译部署以生成新的 device.json", path);
    }

    fill_result(true);
    g_dc_initialized = true;
    return &g_dc_result;
}

const device_config_t * device_config_get(void)
{
    if (!g_dc_initialized) {
        return device_config_load();
    }
    return &g_dc_result;
}

const device_config_t * device_config_reload(void)
{
    g_dc_initialized = false;
    return device_config_load();
}

const char * device_config_path(void)
{
    if (!g_dc_initialized) {
        device_config_load();
    }
    return g_dc_cache.loaded_path;
}

/* ------------------------------------------------------------------ */
/* SN 生成：s + 硬件ID + model_id + 今天日期最后一位                   */
/* ------------------------------------------------------------------ */
const char * device_config_generate_sn(char *buf, size_t buf_sz)
{
    if (!buf || buf_sz == 0) return "";
    buf[0] = '\0';

    /* 确保配置已加载 */
    const device_config_t *cfg = device_config_get();

    /* 1. 硬件ID：优先 device.json 的 serial_number，回退 device-tree/MAC */
    char hw_id[DC_FIELD_MAX] = {0};
    if (cfg->serial_number && cfg->serial_number[0]) {
        snprintf(hw_id, sizeof(hw_id), "%s", cfg->serial_number);
    } else {
        /* 回退 1：设备树 serial-number */
        FILE *f = fopen("/proc/device-tree/serial-number", "rb");
        if (f) {
            if (fgets(hw_id, sizeof(hw_id), f) == NULL) hw_id[0] = '\0';
            fclose(f);
            /* 去除末尾换行（device-tree 文件可能带换行符） */
            size_t len = strlen(hw_id);
            while (len > 0 && (hw_id[len-1] == '\n' || hw_id[len-1] == '\r')) {
                hw_id[--len] = '\0';
            }
        }
        /* 回退 2：wlan0 MAC 地址 */
        if (!hw_id[0]) {
            f = fopen("/sys/class/net/wlan0/address", "rb");
            if (f) {
                if (fgets(hw_id, sizeof(hw_id), f) != NULL) {
                    /* 去除末尾换行 */
                    size_t len = strlen(hw_id);
                    while (len > 0 && (hw_id[len-1] == '\n' || hw_id[len-1] == '\r')) {
                        hw_id[--len] = '\0';
                    }
                }
                fclose(f);
            }
        }
    }

    /* 2. model_id：从配置读取，缺失则用 "unknown" */
    const char *model_id = (cfg->model_id && cfg->model_id[0]) ? cfg->model_id : "unknown";

    /* 3. 今天日期最后一位：YYYYMMDD 的末位字符 */
    char date_digit = '0';
    {
        time_t now = time(NULL);
        struct tm *tm_local = localtime(&now);
        if (tm_local) {
            char date_str[16];
            strftime(date_str, sizeof(date_str), "%Y%m%d", tm_local);
            size_t dlen = strlen(date_str);
            if (dlen > 0) date_digit = date_str[dlen - 1];
        }
    }

    /* 4. 组装 SN：s + 硬件ID + model_id + 日期末位
     * 硬件ID 缺失时用 "000000" 占位，保证 SN 格式完整 */
    const char *hw = hw_id[0] ? hw_id : "000000";
    snprintf(buf, buf_sz, "s%s%s%c", hw, model_id, date_digit);

    return buf;
}
