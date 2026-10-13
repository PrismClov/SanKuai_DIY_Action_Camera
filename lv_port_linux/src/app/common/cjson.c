/**
 * @file cjson.c
 * @brief cJSON 精简实现（递归下降解析器）
 *
 * 设计取舍：
 *   - 单文件实现，无外部依赖，便于在 RK3566/K230 等嵌入式 Linux 上交叉编译
 *   - 覆盖项目所需核心 API：Parse / GetObjectItem / GetArrayItem /
 *     GetStringValue / GetNumberValue / Is* / Delete
 *   - 不实现 Print 序列化（项目仅需读配置，无需写回 JSON）
 *   - 不支持注释、尾随逗号、JSON5 扩展（严格遵守 RFC 8259）
 *   - 字符串解析支持常见转义：\" \\ \/ \b \f \n \r \t \uXXXX
 *   - 内存分配失败时整体回滚（cJSON_Delete 已分配节点）
 */
#include "cjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <limits.h>

/* ------------------------------------------------------------------ */
/* 错误状态                                                            */
/* ------------------------------------------------------------------ */
static size_t      g_error_pos = 0;        /* 错误字符偏移量 */
static const char *g_error_msg = NULL;     /* 错误描述 */

size_t      cJSON_GetErrorPos(void) { return g_error_pos; }
const char *cJSON_GetErrorPtr(void) { return g_error_msg; }

static void set_error(size_t pos, const char *msg)
{
    if (g_error_msg == NULL) {   /* 保留第一个错误 */
        g_error_pos = pos;
        g_error_msg = msg;
    }
}

/* ------------------------------------------------------------------ */
/* 解析上下文                                                          */
/* ------------------------------------------------------------------ */
typedef struct {
    const char *json;        /* JSON 字符串起始 */
    size_t      length;      /* 总长度 */
    size_t      pos;         /* 当前解析位置 */
} parse_context_t;

/* 跳过空白字符（空格 / 制表符 / 换行 / 回车） */
static void skip_whitespace(parse_context_t *ctx)
{
    while (ctx->pos < ctx->length) {
        char c = ctx->json[ctx->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ctx->pos++;
        } else {
            break;
        }
    }
}

/* 查看下一个非空白字符（不消费） */
static char peek_char(parse_context_t *ctx)
{
    skip_whitespace(ctx);
    if (ctx->pos >= ctx->length) return '\0';
    return ctx->json[ctx->pos];
}

/* ------------------------------------------------------------------ */
/* 节点创建与释放                                                      */
/* ------------------------------------------------------------------ */

/* 创建新节点；失败返回 NULL */
static cJSON *new_node(int type)
{
    cJSON *node = (cJSON *)calloc(1, sizeof(cJSON));
    if (!node) {
        set_error(0, "内存分配失败");
        return NULL;
    }
    node->type = type;
    return node;
}

/* 释放单个节点自身资源（不递归兄弟/子节点） */
static void free_node(cJSON *node)
{
    if (!node) return;
    if (node->valuestring) {
        free(node->valuestring);
        node->valuestring = NULL;
    }
    if (node->string) {
        free(node->string);
        node->string = NULL;
    }
    free(node);
}

/* 递归释放整个 JSON 树（含兄弟链与子节点链） */
void cJSON_Delete(cJSON *item)
{
    if (!item) return;
    /* 先递归释放子节点 */
    cJSON *child = item->child;
    while (child) {
        cJSON *next = child->next;
        cJSON_Delete(child);
        child = next;
    }
    /* 再释放自身 */
    item->child = NULL;
    item->next = NULL;
    item->prev = NULL;
    free_node(item);
}

/* ------------------------------------------------------------------ */
/* 字符串解析（含转义处理）                                            */
/* ------------------------------------------------------------------ */

/* 解析 \uXXXX 转义，将 Unicode 码点编码为 UTF-8 写入 out 缓冲区。
 * 返回写入的字节数；失败返回 0。 */
static int parse_unicode_escape(parse_context_t *ctx, char *out, size_t out_sz)
{
    if (ctx->pos + 4 > ctx->length) {
        set_error(ctx->pos, "\\u 转义序列不完整");
        return 0;
    }
    char hex[5] = {0};
    memcpy(hex, ctx->json + ctx->pos, 4);
    ctx->pos += 4;
    char *endp = NULL;
    unsigned long code = strtoul(hex, &endp, 16);
    if (endp != hex + 4) {
        set_error(ctx->pos - 4, "\\u 转义非十六进制");
        return 0;
    }
    /* 处理代理对（surrogate pair）：高代理在 D800~DBFF，需后接低代理 */
    if (code >= 0xD800 && code <= 0xDBFF) {
        if (ctx->pos + 6 <= ctx->length &&
            ctx->json[ctx->pos] == '\\' && ctx->json[ctx->pos + 1] == 'u') {
            char hex2[5] = {0};
            memcpy(hex2, ctx->json + ctx->pos + 2, 4);
            unsigned long low = strtoul(hex2, NULL, 16);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                ctx->pos += 6;
                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
            }
        }
    }
    /* 编码为 UTF-8 */
    if (code <= 0x7F) {
        if (out_sz < 1) return 0;
        out[0] = (char)code;
        return 1;
    } else if (code <= 0x7FF) {
        if (out_sz < 2) return 0;
        out[0] = (char)(0xC0 | (code >> 6));
        out[1] = (char)(0x80 | (code & 0x3F));
        return 2;
    } else if (code <= 0xFFFF) {
        if (out_sz < 3) return 0;
        out[0] = (char)(0xE0 | (code >> 12));
        out[1] = (char)(0x80 | ((code >> 6) & 0x3F));
        out[2] = (char)(0x80 | (code & 0x3F));
        return 3;
    } else {
        if (out_sz < 4) return 0;
        out[0] = (char)(0xF0 | (code >> 18));
        out[1] = (char)(0x80 | ((code >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((code >> 6) & 0x3F));
        out[3] = (char)(0x80 | (code & 0x3F));
        return 4;
    }
}

/* 解析字符串字面量（已位于首个引号之后），返回动态分配的字符串。
 * 调用方负责 free。失败返回 NULL。 */
static char *parse_string_value(parse_context_t *ctx)
{
    /* 估算容量：剩余字符数（上界）+ 1 */
    size_t cap = ctx->length - ctx->pos + 1;
    char  *buf = (char *)malloc(cap);
    if (!buf) {
        set_error(ctx->pos, "字符串内存分配失败");
        return NULL;
    }
    size_t out = 0;
    while (ctx->pos < ctx->length) {
        char c = ctx->json[ctx->pos++];
        if (c == '"') {
            buf[out] = '\0';
            return buf;
        }
        if (c == '\\') {
            if (ctx->pos >= ctx->length) {
                set_error(ctx->pos, "转义序列在字符串末尾截断");
                free(buf);
                return NULL;
            }
            char esc = ctx->json[ctx->pos++];
            switch (esc) {
                case '"':  buf[out++] = '"';  break;
                case '\\': buf[out++] = '\\'; break;
                case '/':  buf[out++] = '/';  break;
                case 'b':  buf[out++] = '\b'; break;
                case 'f':  buf[out++] = '\f'; break;
                case 'n':  buf[out++] = '\n'; break;
                case 'r':  buf[out++] = '\r'; break;
                case 't':  buf[out++] = '\t'; break;
                case 'u': {
                    char u8[4];
                    int n = parse_unicode_escape(ctx, u8, sizeof(u8));
                    if (n <= 0) { free(buf); return NULL; }
                    memcpy(buf + out, u8, n);
                    out += n;
                    break;
                }
                default:
                    set_error(ctx->pos - 1, "未知转义字符");
                    free(buf);
                    return NULL;
            }
        } else {
            /* 普通字符（含 UTF-8 多字节序列） */
            buf[out++] = c;
        }
    }
    set_error(ctx->pos, "字符串未闭合（缺少结尾引号）");
    free(buf);
    return NULL;
}

/* 解析字符串字面量，包装为 cJSON 节点；首字符应为 '"' */
static cJSON *parse_string(parse_context_t *ctx)
{
    if (ctx->pos >= ctx->length || ctx->json[ctx->pos] != '"') {
        set_error(ctx->pos, "字符串应以 '\"' 开头");
        return NULL;
    }
    ctx->pos++;  /* 跳过起始引号 */
    char *value = parse_string_value(ctx);
    if (!value) return NULL;
    cJSON *node = new_node(cJSON_String);
    if (!node) {
        free(value);
        return NULL;
    }
    node->valuestring = value;
    return node;
}

/* ------------------------------------------------------------------ */
/* 数字解析                                                            */
/* ------------------------------------------------------------------ */
static cJSON *parse_number(parse_context_t *ctx)
{
    size_t start = ctx->pos;
    /* 整数部分 */
    if (ctx->pos < ctx->length && ctx->json[ctx->pos] == '-') ctx->pos++;
    if (ctx->pos < ctx->length && ctx->json[ctx->pos] == '0') {
        ctx->pos++;
    } else {
        while (ctx->pos < ctx->length &&
               ctx->json[ctx->pos] >= '1' && ctx->json[ctx->pos] <= '9') {
            ctx->pos++;
        }
    }
    /* 小数部分 */
    if (ctx->pos < ctx->length && ctx->json[ctx->pos] == '.') {
        ctx->pos++;
        while (ctx->pos < ctx->length &&
               isdigit((unsigned char)ctx->json[ctx->pos])) {
            ctx->pos++;
        }
    }
    /* 指数部分 */
    if (ctx->pos < ctx->length &&
        (ctx->json[ctx->pos] == 'e' || ctx->json[ctx->pos] == 'E')) {
        ctx->pos++;
        if (ctx->pos < ctx->length &&
            (ctx->json[ctx->pos] == '+' || ctx->json[ctx->pos] == '-')) {
            ctx->pos++;
        }
        while (ctx->pos < ctx->length &&
               isdigit((unsigned char)ctx->json[ctx->pos])) {
            ctx->pos++;
        }
    }
    size_t len = ctx->pos - start;
    if (len == 0) {
        set_error(ctx->pos, "无效的数字字面量");
        return NULL;
    }
    char numbuf[64];
    if (len >= sizeof(numbuf)) len = sizeof(numbuf) - 1;
    memcpy(numbuf, ctx->json + start, len);
    numbuf[len] = '\0';
    char *endp = NULL;
    double d = strtod(numbuf, &endp);
    if (endp != numbuf + len) {
        set_error(start, "数字解析失败");
        return NULL;
    }
    cJSON *node = new_node(cJSON_Number);
    if (!node) return NULL;
    node->valuedouble = d;
    /* 截断为 int，与标准 cJSON 行为一致 */
    if (d >= INT_MIN && d <= INT_MAX) {
        node->valueint = (int)d;
    } else {
        node->valueint = INT_MAX;
    }
    return node;
}

/* ------------------------------------------------------------------ */
/* 字面量（true / false / null）解析                                    */
/* ------------------------------------------------------------------ */
static int match_literal(parse_context_t *ctx, const char *literal, size_t len)
{
    if (ctx->pos + len > ctx->length) return 0;
    if (memcmp(ctx->json + ctx->pos, literal, len) != 0) return 0;
    ctx->pos += len;
    return 1;
}

/* ------------------------------------------------------------------ */
/* 数组解析                                                            */
/* ------------------------------------------------------------------ */
static cJSON *parse_value(parse_context_t *ctx);  /* 前向声明 */

static cJSON *parse_array(parse_context_t *ctx)
{
    /* 进入时已消费起始 '[' */
    cJSON *arr = new_node(cJSON_Array);
    if (!arr) return NULL;
    cJSON *tail = NULL;

    if (peek_char(ctx) == ']') {
        ctx->pos++;  /* 空数组 */
        return arr;
    }
    while (1) {
        cJSON *elem = parse_value(ctx);
        if (!elem) {
            cJSON_Delete(arr);
            return NULL;
        }
        if (tail) {
            tail->next = elem;
            elem->prev = tail;
        } else {
            arr->child = elem;
        }
        tail = elem;
        char c = peek_char(ctx);
        if (c == ']') {
            ctx->pos++;
            break;
        } else if (c == ',') {
            ctx->pos++;
            continue;
        } else {
            set_error(ctx->pos, "数组缺少 ',' 或 ']'");
            cJSON_Delete(arr);
            return NULL;
        }
    }
    return arr;
}

/* ------------------------------------------------------------------ */
/* 对象解析                                                            */
/* ------------------------------------------------------------------ */
static cJSON *parse_object(parse_context_t *ctx)
{
    /* 进入时已消费起始 '{' */
    cJSON *obj = new_node(cJSON_Object);
    if (!obj) return NULL;
    cJSON *tail = NULL;

    if (peek_char(ctx) == '}') {
        ctx->pos++;  /* 空对象 */
        return obj;
    }
    while (1) {
        /* 键必须是字符串 */
        skip_whitespace(ctx);
        if (ctx->pos >= ctx->length || ctx->json[ctx->pos] != '"') {
            set_error(ctx->pos, "对象键应为字符串");
            cJSON_Delete(obj);
            return NULL;
        }
        ctx->pos++;  /* 跳过起始引号 */
        char *key = parse_string_value(ctx);
        if (!key) {
            cJSON_Delete(obj);
            return NULL;
        }
        /* 冒号分隔符 */
        if (peek_char(ctx) != ':') {
            set_error(ctx->pos, "对象键后缺少 ':'");
            free(key);
            cJSON_Delete(obj);
            return NULL;
        }
        ctx->pos++;  /* 跳过 ':' */
        /* 值 */
        cJSON *val = parse_value(ctx);
        if (!val) {
            free(key);
            cJSON_Delete(obj);
            return NULL;
        }
        val->string = key;
        if (tail) {
            tail->next = val;
            val->prev = tail;
        } else {
            obj->child = val;
        }
        tail = val;
        /* 下一个分隔符 */
        char c = peek_char(ctx);
        if (c == '}') {
            ctx->pos++;
            break;
        } else if (c == ',') {
            ctx->pos++;
            continue;
        } else {
            set_error(ctx->pos, "对象缺少 ',' 或 '}'");
            cJSON_Delete(obj);
            return NULL;
        }
    }
    return obj;
}

/* ------------------------------------------------------------------ */
/* 值分发                                                              */
/* ------------------------------------------------------------------ */
static cJSON *parse_value(parse_context_t *ctx)
{
    skip_whitespace(ctx);
    if (ctx->pos >= ctx->length) {
        set_error(ctx->pos, "意外的输入结尾");
        return NULL;
    }
    char c = ctx->json[ctx->pos];
    switch (c) {
        case '{':
            ctx->pos++;
            return parse_object(ctx);
        case '[':
            ctx->pos++;
            return parse_array(ctx);
        case '"':
            return parse_string(ctx);
        case '-':
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            return parse_number(ctx);
        case 't':
            if (match_literal(ctx, "true", 4)) {
                return new_node(cJSON_True);
            }
            set_error(ctx->pos, "未知字面量");
            return NULL;
        case 'f':
            if (match_literal(ctx, "false", 5)) {
                return new_node(cJSON_False);
            }
            set_error(ctx->pos, "未知字面量");
            return NULL;
        case 'n':
            if (match_literal(ctx, "null", 4)) {
                return new_node(cJSON_NULL);
            }
            set_error(ctx->pos, "未知字面量");
            return NULL;
        default:
            set_error(ctx->pos, "非法字符");
            return NULL;
    }
}

/* ------------------------------------------------------------------ */
/* 公共 API                                                            */
/* ------------------------------------------------------------------ */

cJSON * cJSON_ParseWithLength(const char *value, size_t buffer_length)
{
    g_error_pos = 0;
    g_error_msg = NULL;
    if (!value || buffer_length == 0) {
        set_error(0, "输入为空");
        return NULL;
    }
    parse_context_t ctx = { value, buffer_length, 0 };
    cJSON *root = parse_value(&ctx);
    if (!root) {
        return NULL;
    }
    /* 解析完成后应只剩空白 */
    skip_whitespace(&ctx);
    if (ctx.pos != ctx.length) {
        set_error(ctx.pos, "JSON 根节点后存在多余内容");
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

cJSON * cJSON_Parse(const char *value)
{
    if (!value) {
        g_error_pos = 0;
        g_error_msg = "输入为 NULL";
        return NULL;
    }
    return cJSON_ParseWithLength(value, strlen(value));
}

/* ------------------------------------------------------------------ */
/* 查询接口                                                            */
/* ------------------------------------------------------------------ */

cJSON * cJSON_GetObjectItem(const cJSON *object, const char *string)
{
    /* 标准 cJSON 行为：默认大小写不敏感（兼容历史代码）。
     * 本精简实现直接转发到大小写敏感版本（项目内所有调用方均使用
     * cJSON_GetObjectItemCaseSensitive，且 JSON 键名在 device.json 中
     * 全部为小写，无需大小写不敏感匹配）。 */
    return cJSON_GetObjectItemCaseSensitive(object, string);
}

cJSON * cJSON_GetObjectItemCaseSensitive(const cJSON *object, const char *string)
{
    if (!object || !string || !(object->type & cJSON_Object)) return NULL;
    cJSON *child = object->child;
    while (child) {
        if (child->string && strcmp(child->string, string) == 0) {
            return child;
        }
        child = child->next;
    }
    return NULL;
}

cJSON * cJSON_GetArrayItem(const cJSON *array, int index)
{
    if (!array || !(array->type & cJSON_Array) || index < 0) return NULL;
    cJSON *child = array->child;
    for (int i = 0; child && i < index; i++) {
        child = child->next;
    }
    return child;
}

/* ------------------------------------------------------------------ */
/* 类型断言                                                            */
/* ------------------------------------------------------------------ */
int cJSON_IsInvalid(const cJSON *item) { return !item || (item->type == cJSON_Invalid); }
int cJSON_IsFalse(const cJSON *item)   { return item && (item->type & cJSON_False); }
int cJSON_IsTrue(const cJSON *item)    { return item && (item->type & cJSON_True); }
int cJSON_IsBool(const cJSON *item)    { return item && (item->type & (cJSON_True | cJSON_False)); }
int cJSON_IsNull(const cJSON *item)    { return item && (item->type & cJSON_NULL); }
int cJSON_IsNumber(const cJSON *item)  { return item && (item->type & cJSON_Number); }
int cJSON_IsString(const cJSON *item)  { return item && (item->type & cJSON_String); }
int cJSON_IsArray(const cJSON *item)   { return item && (item->type & cJSON_Array); }
int cJSON_IsObject(const cJSON *item)  { return item && (item->type & cJSON_Object); }

/* ------------------------------------------------------------------ */
/* 便捷取值                                                            */
/* ------------------------------------------------------------------ */
const char * cJSON_GetStringValue(const cJSON *item)
{
    if (!item || !(item->type & cJSON_String)) return NULL;
    return item->valuestring;
}

double cJSON_GetNumberValue(const cJSON *item)
{
    if (!item || !(item->type & cJSON_Number)) return 0;
    return item->valuedouble;
}
