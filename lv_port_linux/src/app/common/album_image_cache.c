/**
 * @file album_image_cache.c
 * @brief 相册图片降采样缓存
 *
 * 高分辨率照片（如 20MP+）直接交给 LVGL 解码会耗尽内存或触发尺寸限制。
 * 本模块在首次加载时，用 libjpeg 将其缩放到安全尺寸并缓存为 JPEG，
 * 后续直接使用缓存图显示缩略图和全屏预览。
 */
#include "album_image_cache.h"
#include "../app_main.h"

#include "lvgl/lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <unistd.h>

/* 优先使用系统 libjpeg；没有时只能原图显示（无法对大图降采样） */
#if __has_include(<jpeglib.h>)
#define AIC_USE_LIBJPEG 1
#include <jpeglib.h>
#include <setjmp.h>
#else
#define AIC_USE_LIBJPEG 0
#endif

#define AIC_CACHE_DIR "/tmp/.lv_album_cache"

/* 返回路径缓冲区（静态，调用方需立即使用或拷贝） */
static char s_return_path[2048];

/* ------------------------------------------------------------------ */
/* 简易 SHA256（与 video_thumb.c 同源，独立静态避免破坏现有文件）     */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buffer[64];
    size_t   buflen;
} sha256_ctx_t;

static const uint32_t K256[64] = {
    0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,
    0x923f82a4U,0xab1c5ed5U,0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,
    0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,0xe49b69c1U,0xefbe4786U,
    0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
    0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,
    0x06ca6351U,0x14292967U,0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,
    0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,0xa2bfe8a1U,0xa81a664bU,
    0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
    0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,
    0x5b9cca4fU,0x682e6ff3U,0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,
    0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U
};

#define ROTR(x,n) (((x)>>(n)) | ((x)<<(32-(n))))

static void sha256_transform(sha256_ctx_t *ctx, const uint8_t *data)
{
    uint32_t a,b,c,d,e,f,g,h,t1,t2,m[64];
    int i,j;
    for (i=0,j=0; i<16; ++i, j+=4)
        m[i] = ((uint32_t)data[j]<<24) | ((uint32_t)data[j+1]<<16) |
               ((uint32_t)data[j+2]<<8) | ((uint32_t)data[j+3]);
    for (; i<64; ++i) {
        uint32_t s0 = ROTR(m[i-15],7) ^ ROTR(m[i-15],18) ^ (m[i-15]>>3);
        uint32_t s1 = ROTR(m[i-2],17) ^ ROTR(m[i-2],19) ^ (m[i-2]>>10);
        m[i] = m[i-16] + s0 + m[i-7] + s1;
    }
    a=ctx->state[0]; b=ctx->state[1]; c=ctx->state[2]; d=ctx->state[3];
    e=ctx->state[4]; f=ctx->state[5]; g=ctx->state[6]; h=ctx->state[7];
    for (i=0;i<64;++i) {
        uint32_t S1 = ROTR(e,6) ^ ROTR(e,11) ^ ROTR(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        t1 = h + S1 + ch + K256[i] + m[i];
        uint32_t S0 = ROTR(a,2) ^ ROTR(a,13) ^ ROTR(a,22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        t2 = S0 + maj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    ctx->state[0]+=a; ctx->state[1]+=b; ctx->state[2]+=c; ctx->state[3]+=d;
    ctx->state[4]+=e; ctx->state[5]+=f; ctx->state[6]+=g; ctx->state[7]+=h;
}

static void sha256_init(sha256_ctx_t *ctx)
{
    ctx->buflen=0; ctx->bitlen=0;
    ctx->state[0]=0x6a09e667U; ctx->state[1]=0xbb67ae85U;
    ctx->state[2]=0x3c6ef372U; ctx->state[3]=0xa54ff53aU;
    ctx->state[4]=0x510e527fU; ctx->state[5]=0x9b05688cU;
    ctx->state[6]=0x1f83d9abU; ctx->state[7]=0x5be0cd19U;
}

static void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len)
{
    size_t i;
    for (i=0;i<len;++i) {
        ctx->buffer[ctx->buflen++] = data[i];
        if (ctx->buflen == 64) {
            sha256_transform(ctx, ctx->buffer);
            ctx->bitlen += 512;
            ctx->buflen = 0;
        }
    }
}

static void sha256_final(sha256_ctx_t *ctx, uint8_t hash[32])
{
    size_t i = ctx->buflen;
    ctx->buffer[i++] = 0x80;
    if (ctx->buflen >= 56) {
        while (i < 64) ctx->buffer[i++] = 0;
        sha256_transform(ctx, ctx->buffer);
        i = 0;
    }
    while (i < 56) ctx->buffer[i++] = 0;
    ctx->bitlen += (uint64_t)ctx->buflen * 8;
    for (int j=7;j>=0;--j) ctx->buffer[63-j] = (uint8_t)(ctx->bitlen >> (j*8));
    sha256_transform(ctx, ctx->buffer);
    for (i=0;i<8;++i) {
        hash[i*4+0] = (uint8_t)(ctx->state[i] >> 24);
        hash[i*4+1] = (uint8_t)(ctx->state[i] >> 16);
        hash[i*4+2] = (uint8_t)(ctx->state[i] >> 8);
        hash[i*4+3] = (uint8_t)(ctx->state[i]);
    }
}

static void bytes_to_hex(const uint8_t *in, char *out, size_t len)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i=0;i<len;++i) {
        out[i*2+0] = hex[(in[i]>>4)&0xF];
        out[i*2+1] = hex[in[i]&0xF];
    }
    out[len*2] = '\0';
}

/* ------------------------------------------------------------------ */
/* 工具函数                                                             */
/* ------------------------------------------------------------------ */
static const char * to_real_path(const char *path)
{
    if (path && path[0] == 'A' && path[1] == ':') return path + 2;
    return path;
}

static const char * lvgl_path(const char *real_path)
{
    s_return_path[0] = '\0';
    if (!real_path) return NULL;
    snprintf(s_return_path, sizeof(s_return_path), "A:%s", real_path);
    return s_return_path;
}

static int ensure_cache_dir(void)
{
    struct stat st;
    if (stat(AIC_CACHE_DIR, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
    if (mkdir(AIC_CACHE_DIR, 0755) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

static void make_cache_path(const char *src_path, const struct stat *vst,
                            int cache_type, char *out, size_t out_sz)
{
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)src_path, strlen(src_path));
    char meta[64];
    int meta_len = snprintf(meta, sizeof(meta), "|%lld|%lld|%d",
                            (long long)vst->st_size, (long long)vst->st_mtime, cache_type);
    sha256_update(&ctx, (const uint8_t *)meta, (size_t)meta_len);
    uint8_t hash[32];
    char hex[65];
    sha256_final(&ctx, hash);
    bytes_to_hex(hash, hex, 16);
    snprintf(out, out_sz, "%s/%s.jpg", AIC_CACHE_DIR, hex);
}

static int cache_is_valid(const char *cache_path, const struct stat *vst)
{
    struct stat st;
    if (stat(cache_path, &st) != 0) return 0;
    if (!S_ISREG(st.st_mode)) return 0;
    if (st.st_size <= 0) return 0;
    return st.st_mtime >= vst->st_mtime;
}

static int is_jpeg(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return 0;
    return (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0);
}

/* ------------------------------------------------------------------ */
/* libjpeg 操作                                                         */
/* ------------------------------------------------------------------ */
#if AIC_USE_LIBJPEG

struct aic_jpeg_err {
    struct jpeg_error_mgr pub;
    jmp_buf jb;
};

static void aic_jpeg_error_exit(j_common_ptr cinfo)
{
    struct aic_jpeg_err *err = (struct aic_jpeg_err *)cinfo->err;
    longjmp(err->jb, 1);
}

/* 快速读取 JPEG 宽高 */
static int jpeg_get_dimensions(const char *path, int *w, int *h)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    struct jpeg_decompress_struct cinfo;
    struct aic_jpeg_err jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = aic_jpeg_error_exit;

    if (setjmp(jerr.jb)) {
        jpeg_destroy_decompress(&cinfo);
        fclose(f);
        return -1;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, f);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        fclose(f);
        return -1;
    }
    *w = (int)cinfo.image_width;
    *h = (int)cinfo.image_height;
    jpeg_destroy_decompress(&cinfo);
    fclose(f);
    return 0;
}

static int save_rgb24_jpeg(const char *path, const uint8_t *rgb, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    struct jpeg_compress_struct cinfo;
    struct aic_jpeg_err jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = aic_jpeg_error_exit;

    if (setjmp(jerr.jb)) {
        jpeg_destroy_compress(&cinfo);
        fclose(f);
        return -1;
    }

    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, f);
    cinfo.image_width = (JDIMENSION)w;
    cinfo.image_height = (JDIMENSION)h;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 88, TRUE);
    cinfo.dct_method = JDCT_FASTEST;
    jpeg_start_compress(&cinfo, TRUE);

    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row = (JSAMPROW)(rgb + cinfo.next_scanline * w * 3);
        jpeg_write_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    fclose(f);
    return 0;
}

/* 将 JPEG 缩放到最大边长不超过 max_side，保存为 cache_path */
static int generate_scaled_cache(const char *src_path, const char *cache_path, int max_side)
{
    FILE *f = fopen(src_path, "rb");
    if (!f) return -1;

    struct jpeg_decompress_struct cinfo;
    struct aic_jpeg_err jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = aic_jpeg_error_exit;

    uint8_t *rgb = NULL;
    int ret = -1;

    if (setjmp(jerr.jb)) {
        if (rgb) free(rgb);
        jpeg_destroy_decompress(&cinfo);
        fclose(f);
        return -1;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, f);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) goto done;

    /* libjpeg 支持的缩放分母为 1/2/4/8 */
    int denom = 1;
    while (denom < 8 &&
           ((int)cinfo.image_width / (denom * 2) > max_side ||
            (int)cinfo.image_height / (denom * 2) > max_side)) {
        denom *= 2;
    }
    cinfo.scale_num = 1;
    cinfo.scale_denom = (unsigned int)denom;

    if (!jpeg_start_decompress(&cinfo)) goto done;

    int out_w = (int)cinfo.output_width;
    int out_h = (int)cinfo.output_height;
    int row_stride = (int)cinfo.output_width * cinfo.output_components;

    rgb = (uint8_t *)malloc((size_t)row_stride * out_h);
    if (!rgb) goto done;

    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW row = (JSAMPROW)(rgb + cinfo.output_scanline * row_stride);
        if (jpeg_read_scanlines(&cinfo, &row, 1) != 1) break;
    }
    if (cinfo.output_scanline < cinfo.output_height) goto done;

    if (save_rgb24_jpeg(cache_path, rgb, out_w, out_h) == 0) {
        LV_LOG_USER("Album cache: %s -> %s (%dx%d, denom=%d)",
                    src_path, cache_path, out_w, out_h, denom);
        ret = 0;
    }

done:
    if (rgb) free(rgb);
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(f);
    return ret;
}

#else /* !AIC_USE_LIBJPEG */

static int jpeg_get_dimensions(const char *path, int *w, int *h)
{
    (void)path; (void)w; (void)h;
    return -1;
}
static int generate_scaled_cache(const char *src_path, const char *cache_path, int max_side)
{
    (void)src_path; (void)cache_path; (void)max_side;
    return -1;
}

#endif /* AIC_USE_LIBJPEG */

/* ------------------------------------------------------------------ */
/* 公共 API                                                             */
/* ------------------------------------------------------------------ */
const char * album_image_get_path(const char *src_path, int cache_type)
{
    s_return_path[0] = '\0';
    if (!src_path || !src_path[0]) return NULL;

    const char *real_path = to_real_path(src_path);

    /* 非 JPEG 文件（如 BMP）直接返回原路径，由 LVGL 处理 */
    if (!is_jpeg(real_path)) {
        return src_path;
    }

    struct stat vst;
    if (stat(real_path, &vst) != 0 || !S_ISREG(vst.st_mode)) return NULL;

    int max_side = (cache_type == ALBUM_IMAGE_CACHE_THUMB)
                       ? ALBUM_IMAGE_CACHE_THUMB_MAX
                       : ALBUM_IMAGE_CACHE_PREVIEW_MAX;

    /* 无需降采样的原图直接返回 */
    int w = 0, h = 0;
    if (jpeg_get_dimensions(real_path, &w, &h) == 0 &&
        w <= max_side && h <= max_side) {
        return src_path;
    }

    /* 大图需要缓存 */
    if (ensure_cache_dir() != 0) return NULL;

    char cache_path[1024];
    make_cache_path(real_path, &vst, cache_type, cache_path, sizeof(cache_path));

    if (!cache_is_valid(cache_path, &vst)) {
        if (generate_scaled_cache(real_path, cache_path, max_side) != 0) {
            LV_LOG_WARN("Album cache: failed to generate %s", real_path);
            return NULL;
        }
    }

    return lvgl_path(cache_path);
}
