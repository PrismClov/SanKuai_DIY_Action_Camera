/**
 * @file cjson.h
 * @brief cJSON 精简实现（接口兼容标准 cJSON 1.7.x）
 *
 * 说明：
 *   本文件是 cJSON 核心接口的精简实现，覆盖项目所需的解析/查询 API。
 *   不依赖外部库，仅用于读取 device.json 等小型扁平结构配置文件。
 *   完整版 cJSON 见 https://github.com/DaveGamble/cJSON
 *
 * 支持的 JSON 类型：Object / Array / String / Number / True / False / Null
 * 不支持：注释、尾随逗号、JSON5 扩展；不支持 Print/PrintUnformatted 等序列化
 */
#ifndef CJSON_H
#define CJSON_H

#include <stddef.h>   /* size_t */

#ifdef __cplusplus
extern "C" {
#endif

/* JSON 节点类型（与标准 cJSON 保持一致） */
typedef enum {
    cJSON_Invalid = 0,
    cJSON_False   = 1 << 0,
    cJSON_True    = 1 << 1,
    cJSON_NULL    = 1 << 2,
    cJSON_Number  = 1 << 3,
    cJSON_String  = 1 << 4,
    cJSON_Array   = 1 << 5,
    cJSON_Object  = 1 << 6,
    cJSON_Raw     = 1 << 7   /* 预留，本项目未使用 */
} cJSON_type;

/* JSON 节点结构（字段命名与标准 cJSON 一致，便于将来无缝替换） */
typedef struct cJSON {
    struct cJSON *next;          /* 兄弟节点（链表） */
    struct cJSON *prev;          /* 兄弟节点（链表，反向） */
    struct cJSON *child;         /* 第一个子节点（Object 的第一个成员 / Array 的第一个元素） */
    int            type;         /* cJSON_type 标志位 */
    char          *valuestring;  /* String 类型：字符串内容（NULL 结尾） */
    double         valuedouble;  /* Number 类型：浮点值 */
    int            valueint;     /* Number 类型：截断后的整型值 */
    char          *string;       /* Object 成员的键名；Array 元素此字段为 NULL */
} cJSON;

/* ------------------------------------------------------------------ */
/* 解析与释放                                                          */
/* ------------------------------------------------------------------ */

/* 解析 JSON 字符串，返回根节点；失败返回 NULL。
 * 调用方负责 cJSON_Delete 释放。 */
cJSON * cJSON_Parse(const char *value);

/* 带长度的解析接口（value 可能不含终止符） */
cJSON * cJSON_ParseWithLength(const char *value, size_t buffer_length);

/* 递归释放整个 JSON 树 */
void   cJSON_Delete(cJSON *item);

/* ------------------------------------------------------------------ */
/* 查询接口                                                            */
/* ------------------------------------------------------------------ */

/* 在 Object 中按 key 查找子节点（大小写敏感） */
cJSON * cJSON_GetObjectItem(const cJSON *object, const char *string);

/* 在 Object 中按 key 查找子节点（大小写不敏感） */
cJSON * cJSON_GetObjectItemCaseSensitive(const cJSON *object, const char *string);

/* 在 Array 中按下标查找元素 */
cJSON * cJSON_GetArrayItem(const cJSON *array, int index);

/* ------------------------------------------------------------------ */
/* 类型断言（与标准 cJSON 同名，便于将来替换）                          */
/* ------------------------------------------------------------------ */
int cJSON_IsInvalid(const cJSON *item);
int cJSON_IsFalse(const cJSON *item);
int cJSON_IsTrue(const cJSON *item);
int cJSON_IsBool(const cJSON *item);
int cJSON_IsNull(const cJSON *item);
int cJSON_IsNumber(const cJSON *item);
int cJSON_IsString(const cJSON *item);
int cJSON_IsArray(const cJSON *item);
int cJSON_IsObject(const cJSON *item);

/* ------------------------------------------------------------------ */
/* 便捷取值                                                            */
/* ------------------------------------------------------------------ */

/* 返回 String 节点的字符串内容；非 String 或 NULL 节点返回 NULL */
const char * cJSON_GetStringValue(const cJSON *item);

/* 返回 Number 节点的浮点值；非 Number 返回 0 */
double      cJSON_GetNumberValue(const cJSON *item);

/* ------------------------------------------------------------------ */
/* 错误信息                                                            */
/* ------------------------------------------------------------------ */

/* 返回上次解析错误的偏移量（字符位置），0 表示无错误 */
size_t cJSON_GetErrorPos(void);

/* 返回上次解析错误的描述字符串 */
const char * cJSON_GetErrorPtr(void);

#ifdef __cplusplus
}
#endif

#endif /* CJSON_H */
