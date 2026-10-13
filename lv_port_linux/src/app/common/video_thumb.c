/**
 * @file video_thumb.c
 * @brief 视频缩略图提取实现（FFmpeg）
 *
 * 设计要点：
 *   - 仅提取第一帧，输出固定 256x256 以内保持宽高比的 JPEG
 *   - 缓存文件名包含视频路径、大小、mtime 的 SHA256 前缀，避免冲突和过期
 *   - 所有 FFmpeg 资源在使用后释放，避免内存泄漏
 *   - 失败时返回 NULL，相册回退到图标占位
 */
#include "video_thumb.h"
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

#if defined(APP_HAS_FFMPEG)

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

/* 缩略图缓存目录 */
#define THUMB_CACHE_DIR "/tmp/.lv_album_thumbs"

/* 缩略图最大边长（保持宽高比缩放） */
#define THUMB_MAX_SIZE 256

/* JPEG 编码质量 */
#define THUMB_JPEG_QUALITY 75

/* 静态缓冲区：返回给调用方的 LVGL 路径 */
static char s_thumb_path[512];

/* ------------------------------------------------------------------ */
/* 简易 SHA256 用于缓存文件名（避免路径中的 '/' 和空格问题）          */
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
/* 缓存路径管理                                                         */
/* ------------------------------------------------------------------ */
static int ensure_cache_dir(void)
{
    struct stat st;
    if (stat(THUMB_CACHE_DIR, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
    if (mkdir(THUMB_CACHE_DIR, 0755) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

/* 生成缓存文件路径：SHA256(path + size + mtime)[:16].jpg */
static void make_cache_path(const char *video_path, const struct stat *vst,
                            char *out, size_t out_sz)
{
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)video_path, strlen(video_path));
    char meta[64];
    int meta_len = snprintf(meta, sizeof(meta), "|%lld|%lld",
                            (long long)vst->st_size, (long long)vst->st_mtime);
    sha256_update(&ctx, (const uint8_t *)meta, (size_t)meta_len);
    uint8_t hash[32];
    char hex[65];
    sha256_final(&ctx, hash);
    bytes_to_hex(hash, hex, 16);
    snprintf(out, out_sz, "%s/%s.jpg", THUMB_CACHE_DIR, hex);
}

/* 检查缓存是否有效：存在且修改时间不早于视频文件 */
static int cache_is_valid(const char *cache_path, const struct stat *vst)
{
    struct stat st;
    if (stat(cache_path, &st) != 0) return 0;
    if (!S_ISREG(st.st_mode)) return 0;
    if (st.st_size <= 0) return 0;
    return st.st_mtime >= vst->st_mtime;
}

/* ------------------------------------------------------------------ */
/* JPEG 编码（最小实现，仅用于缩略图）                                   */
/* ------------------------------------------------------------------ */
#include <jpeglib.h>
#include <setjmp.h>

struct jpeg_err_ctx {
    struct jpeg_error_mgr pub;
    jmp_buf jb;
};

static void thumb_jpeg_error_exit(j_common_ptr cinfo)
{
    struct jpeg_err_ctx *ctx = (struct jpeg_err_ctx *)cinfo->err;
    longjmp(ctx->jb, 1);
}

static int save_rgb24_jpeg(const char *path, const uint8_t *rgb, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    struct jpeg_compress_struct cinfo;
    struct jpeg_err_ctx jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = thumb_jpeg_error_exit;

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
    jpeg_set_quality(&cinfo, THUMB_JPEG_QUALITY, TRUE);
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

/* ------------------------------------------------------------------ */
/* FFmpeg 提取第一帧                                                    */
/* ------------------------------------------------------------------ */
static int extract_first_frame(const char *video_path, const char *cache_path)
{
    AVFormatContext *fmt_ctx = NULL;
    AVCodecContext *dec_ctx = NULL;
    struct SwsContext *sws_ctx = NULL;
    AVFrame *frame = NULL, *rgb_frame = NULL;
    AVPacket *pkt = NULL;
    uint8_t *rgb_buf = NULL;
    int video_stream = -1;
    int ret = -1;

    if (avformat_open_input(&fmt_ctx, video_path, NULL, NULL) < 0) {
        LV_LOG_WARN("Thumb: 无法打开视频 %s", video_path);
        goto cleanup;
    }
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        LV_LOG_WARN("Thumb: 无法获取流信息 %s", video_path);
        goto cleanup;
    }

    for (unsigned int i=0; i<fmt_ctx->nb_streams; ++i) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream = (int)i;
            break;
        }
    }
    if (video_stream < 0) {
        LV_LOG_WARN("Thumb: 未找到视频流 %s", video_path);
        goto cleanup;
    }

    AVCodecParameters *codecpar = fmt_ctx->streams[video_stream]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        LV_LOG_WARN("Thumb: 找不到解码器 %s", video_path);
        goto cleanup;
    }
    dec_ctx = avcodec_alloc_context3(codec);
    if (!dec_ctx) goto cleanup;
    if (avcodec_parameters_to_context(dec_ctx, codecpar) < 0) goto cleanup;
    if (avcodec_open2(dec_ctx, codec, NULL) < 0) {
        LV_LOG_WARN("Thumb: 无法打开解码器 %s", video_path);
        goto cleanup;
    }

    frame = av_frame_alloc();
    rgb_frame = av_frame_alloc();
    pkt = av_packet_alloc();
    if (!frame || !rgb_frame || !pkt) goto cleanup;

    /* 计算保持宽高比的缩略图尺寸 */
    int src_w = dec_ctx->width;
    int src_h = dec_ctx->height;
    if (src_w <= 0 || src_h <= 0) goto cleanup;
    int dst_w = src_w, dst_h = src_h;
    if (src_w > src_h) {
        if (src_w > THUMB_MAX_SIZE) { dst_w = THUMB_MAX_SIZE; dst_h = src_h * THUMB_MAX_SIZE / src_w; }
    } else {
        if (src_h > THUMB_MAX_SIZE) { dst_h = THUMB_MAX_SIZE; dst_w = src_w * THUMB_MAX_SIZE / src_h; }
    }
    if (dst_w < 1) dst_w = 1;
    if (dst_h < 1) dst_h = 1;

    int rgb_buf_sz = av_image_get_buffer_size(AV_PIX_FMT_RGB24, dst_w, dst_h, 1);
    if (rgb_buf_sz <= 0) goto cleanup;
    rgb_buf = (uint8_t *)av_malloc((size_t)rgb_buf_sz);
    if (!rgb_buf) goto cleanup;
    av_image_fill_arrays(rgb_frame->data, rgb_frame->linesize, rgb_buf,
                         AV_PIX_FMT_RGB24, dst_w, dst_h, 1);

    sws_ctx = sws_getContext(src_w, src_h, dec_ctx->pix_fmt,
                             dst_w, dst_h, AV_PIX_FMT_RGB24,
                             SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws_ctx) goto cleanup;

    /* 读取并解码第一帧 */
    int decoded = 0;
    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if ((int)pkt->stream_index == video_stream) {
            ret = avcodec_send_packet(dec_ctx, pkt);
            if (ret < 0 && ret != AVERROR(EAGAIN)) {
                av_packet_unref(pkt);
                continue;
            }
            ret = avcodec_receive_frame(dec_ctx, frame);
            if (ret == 0) {
                sws_scale(sws_ctx, (const uint8_t *const *)frame->data,
                          frame->linesize, 0, src_h,
                          rgb_frame->data, rgb_frame->linesize);
                decoded = 1;
                av_packet_unref(pkt);
                break;
            }
        }
        av_packet_unref(pkt);
    }

    if (!decoded) {
        /* 尝试 flush 解码器 */
        avcodec_send_packet(dec_ctx, NULL);
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == 0) {
            sws_scale(sws_ctx, (const uint8_t *const *)frame->data,
                      frame->linesize, 0, src_h,
                      rgb_frame->data, rgb_frame->linesize);
            decoded = 1;
        }
    }

    if (decoded) {
        if (save_rgb24_jpeg(cache_path, rgb_buf, dst_w, dst_h) == 0) {
            LV_LOG_USER("Thumb: 已生成 %s -> %s (%dx%d)", video_path, cache_path, dst_w, dst_h);
            ret = 0;
        } else {
            LV_LOG_WARN("Thumb: JPEG 保存失败 %s", cache_path);
            ret = -1;
        }
    } else {
        LV_LOG_WARN("Thumb: 无法解码第一帧 %s", video_path);
        ret = -1;
    }

cleanup:
    if (sws_ctx) sws_freeContext(sws_ctx);
    if (rgb_buf) av_free(rgb_buf);
    if (pkt) av_packet_free(&pkt);
    if (rgb_frame) av_frame_free(&rgb_frame);
    if (frame) av_frame_free(&frame);
    if (dec_ctx) avcodec_free_context(&dec_ctx);
    if (fmt_ctx) avformat_close_input(&fmt_ctx);
    return ret;
}

/* ------------------------------------------------------------------ */
/* 公共 API                                                             */
/* ------------------------------------------------------------------ */
const char * video_thumb_get(const char *video_path)
{
    s_thumb_path[0] = '\0';
    if (!video_path || !video_path[0]) return NULL;

    struct stat vst;
    if (stat(video_path, &vst) != 0 || !S_ISREG(vst.st_mode)) return NULL;

    if (ensure_cache_dir() != 0) return NULL;

    char cache_path[512];
    make_cache_path(video_path, &vst, cache_path, sizeof(cache_path));

    if (!cache_is_valid(cache_path, &vst)) {
        if (extract_first_frame(video_path, cache_path) != 0) return NULL;
    }

    /* 返回 LVGL 可识别的 "A:/path" 格式 */
    snprintf(s_thumb_path, sizeof(s_thumb_path), "A:%s", cache_path);
    return s_thumb_path;
}

int video_thumb_generate(const char *video_path)
{
    if (!video_path || !video_path[0]) return -1;
    struct stat vst;
    if (stat(video_path, &vst) != 0 || !S_ISREG(vst.st_mode)) return -1;
    if (ensure_cache_dir() != 0) return -1;
    char cache_path[512];
    make_cache_path(video_path, &vst, cache_path, sizeof(cache_path));
    return extract_first_frame(video_path, cache_path);
}

int video_thumb_available(void)
{
    return 1;
}

#else  /* !APP_HAS_FFMPEG */

const char * video_thumb_get(const char *video_path)
{
    (void)video_path;
    return NULL;
}

int video_thumb_generate(const char *video_path)
{
    (void)video_path;
    return -1;
}

int video_thumb_available(void)
{
    return 0;
}

#endif /* APP_HAS_FFMPEG */
