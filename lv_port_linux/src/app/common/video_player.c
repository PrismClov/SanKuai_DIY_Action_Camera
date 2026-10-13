/**
 * @file video_player.c
 * @brief 全屏视频播放器实现（FFmpeg + LVGL）
 *
 * 设计要点：
 *   - 单例播放器：同一时间只播放一个视频
 *   - 解码在 LVGL 定时器上下文执行，避免多线程同步和锁
 *   - 使用 RGB888 帧缓冲 + lv_image_dsc_t 直接显示
 *   - 保持视频宽高比，黑边填充
 *   - 点击画面切换播放/暂停；播放/暂停状态图标淡入淡出提示
 */
#include "video_player.h"
#include "ui_helpers.h"
#include "../app_main.h"

#include "lvgl/lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#if defined(APP_HAS_FFMPEG)

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

/* ------------------------------------------------------------------ */
/* 播放器状态                                                           */
/* ------------------------------------------------------------------ */
typedef struct {
    /* FFmpeg */
    AVFormatContext *fmt_ctx;
    AVCodecContext  *dec_ctx;
    struct SwsContext *sws_ctx;
    AVFrame         *frame;
    AVFrame         *rgb_frame;
    AVPacket        *pkt;
    int              video_stream;
    int              src_w;
    int              src_h;
    double           fps;

    /* LVGL */
    lv_obj_t        *overlay;
    lv_obj_t        *img;
    lv_obj_t        *state_icon;
    lv_obj_t        *back_btn;
    lv_timer_t      *timer;

    /* 帧缓冲 */
    uint8_t         *rgb_buf;
    lv_image_dsc_t   img_dsc;

    /* 控制 */
    int              paused;
    int              eof;
    int              playing;
    uint32_t         last_frame_tick;
    double           frame_duration_ms;

    /* 诊断：前 N 帧的详细日志 */
    int              debug_count;

    /* 关闭回调 */
    void           (*close_cb)(void *);
    void            *close_user_data;
} video_player_t;

static video_player_t g_vp = {0};

/* ------------------------------------------------------------------ */
/* 工具函数                                                             */
/* ------------------------------------------------------------------ */
static int is_video_file(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return 0;
    return (strcasecmp(dot, ".mp4") == 0 ||
            strcasecmp(dot, ".mov") == 0 ||
            strcasecmp(dot, ".avi") == 0 ||
            strcasecmp(dot, ".mkv") == 0 ||
            strcasecmp(dot, ".ts") == 0);
}

/* 计算保持宽高比的显示尺寸，适配当前横竖屏 */
static void calc_display_size(int src_w, int src_h, int *out_w, int *out_h)
{
    int screen_w = ui_get_screen_width();
    int screen_h = ui_get_screen_height();
    *out_w = screen_w;
    *out_h = screen_h;
    if (src_w <= 0 || src_h <= 0) return;

    double src_ratio = (double)src_w / (double)src_h;
    double screen_ratio = (double)screen_w / (double)screen_h;

    if (src_ratio > screen_ratio) {
        *out_w = screen_w;
        *out_h = (int)((double)screen_w / src_ratio);
    } else {
        *out_h = screen_h;
        *out_w = (int)((double)screen_h * src_ratio);
    }
    if (*out_w < 1) *out_w = 1;
    if (*out_h < 1) *out_h = 1;
}

/* 显示播放/暂停图标，并在 800ms 后淡出 */
static void show_state_icon(const char *symbol)
{
    if (!g_vp.state_icon) return;
    lv_label_set_text(g_vp.state_icon, symbol);
    lv_obj_remove_flag(g_vp.state_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(g_vp.state_icon, LV_OPA_COVER, 0);
    lv_obj_fade_out(g_vp.state_icon, 600, 800);
}

/* 尝试从解码器取出一帧并显示。
 * 返回 1：成功输出一帧；0：需要更多输入（EAGAIN）；-1：EOF 或错误。 */
static int player_output_one_frame(void)
{
    int ret = avcodec_receive_frame(g_vp.dec_ctx, g_vp.frame);
    if (ret == AVERROR(EAGAIN)) return 0;
    if (ret == AVERROR_EOF) {
        if (g_vp.debug_count < 10) {
            LV_LOG_WARN("Player DBG[%d]: receive_frame EOF", g_vp.debug_count);
        }
        return -1;
    }
    if (ret < 0) {
        if (g_vp.debug_count < 10) {
            LV_LOG_WARN("Player DBG[%d]: receive_frame err=%d (%s)", g_vp.debug_count, ret, av_err2str(ret));
        }
        return -1;
    }

    /* 缩放/格式转换到 RGB888 */
    sws_scale(g_vp.sws_ctx,
              (const uint8_t *const *)g_vp.frame->data,
              g_vp.frame->linesize, 0, g_vp.src_h,
              g_vp.rgb_frame->data, g_vp.rgb_frame->linesize);

    /* 更新 LVGL 图像描述符 */
    g_vp.img_dsc.data = g_vp.rgb_buf;
    g_vp.img_dsc.data_size = (uint32_t)(g_vp.img_dsc.header.w * g_vp.img_dsc.header.h * 3);

    if (g_vp.img && lv_obj_is_valid(g_vp.img)) {
        lv_obj_invalidate(g_vp.img);
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* 播放器清理                                                           */
/* ------------------------------------------------------------------ */
static void player_cleanup(void)
{
    if (g_vp.timer) {
        lv_timer_delete(g_vp.timer);
        g_vp.timer = NULL;
    }
    if (g_vp.rgb_buf) {
        av_free(g_vp.rgb_buf);
        g_vp.rgb_buf = NULL;
    }
    if (g_vp.pkt) {
        av_packet_free(&g_vp.pkt);
        g_vp.pkt = NULL;
    }
    if (g_vp.rgb_frame) {
        av_frame_free(&g_vp.rgb_frame);
        g_vp.rgb_frame = NULL;
    }
    if (g_vp.frame) {
        av_frame_free(&g_vp.frame);
        g_vp.frame = NULL;
    }
    if (g_vp.sws_ctx) {
        sws_freeContext(g_vp.sws_ctx);
        g_vp.sws_ctx = NULL;
    }
    if (g_vp.dec_ctx) {
        avcodec_free_context(&g_vp.dec_ctx);
        g_vp.dec_ctx = NULL;
    }
    if (g_vp.fmt_ctx) {
        avformat_close_input(&g_vp.fmt_ctx);
        g_vp.fmt_ctx = NULL;
    }

    if (g_vp.overlay && lv_obj_is_valid(g_vp.overlay)) {
        lv_obj_delete(g_vp.overlay);
        g_vp.overlay = NULL;
    }
    g_vp.img = NULL;
    g_vp.state_icon = NULL;
    g_vp.back_btn = NULL;
    g_vp.playing = 0;
    g_vp.paused = 0;
    g_vp.eof = 0;
}

/* ------------------------------------------------------------------ */
/* 解码并显示一帧。
 * 采用 FFmpeg send/receive API 的标准模式：
 *   1. 先尝试取出解码器已缓冲的帧；
 *   2. 逐包送入，若输入队列满（EAGAIN）则先取帧再重试；
 *   3. 文件读完后 flush 解码器输出剩余帧。
 * 修复旧实现中“只播第一帧/立即 EOF”的问题。 */
static int decode_one_frame(void)
{
    int ret;
    int dbg = g_vp.debug_count;

    if (!g_vp.fmt_ctx || !g_vp.dec_ctx) return -1;
    if (g_vp.eof) return -1;

    /* 1. 先取出解码器内部已缓冲的帧（B 帧/ reorder 后的帧） */
    int out = player_output_one_frame();
    if (out != 0) {
        if (dbg < 10) {
            LV_LOG_WARN("Player DBG[%d]: buffered frame out=%d", dbg, out);
        }
        g_vp.debug_count++;
        return (out == 1) ? 0 : -1;
    }

    /* 2. 读包送入解码器 */
    int read_count = 0;
    while ((ret = av_read_frame(g_vp.fmt_ctx, g_vp.pkt)) >= 0) {
        read_count++;
        if (dbg < 10) {
            LV_LOG_WARN("Player DBG[%d]: read_frame #%d ok, stream=%d (want %d), size=%d, pts=%lld",
                        dbg, read_count, g_vp.pkt->stream_index, g_vp.video_stream,
                        g_vp.pkt->size, (long long)g_vp.pkt->pts);
        }

        if ((int)g_vp.pkt->stream_index != g_vp.video_stream) {
            av_packet_unref(g_vp.pkt);
            continue;
        }

        /* 发送包；若返回 EAGAIN，说明输入队列满，先取出一帧腾出空间 */
        int send_retry = 0;
        while ((ret = avcodec_send_packet(g_vp.dec_ctx, g_vp.pkt)) == AVERROR(EAGAIN)) {
            out = player_output_one_frame();
            if (out == 1) {
                if (dbg < 10) LV_LOG_WARN("Player DBG[%d]: send EAGAIN, got frame", dbg);
                av_packet_unref(g_vp.pkt);
                g_vp.debug_count++;
                return 0;
            }
            if (out == -1) {
                if (dbg < 10) LV_LOG_WARN("Player DBG[%d]: send EAGAIN, frame err", dbg);
                av_packet_unref(g_vp.pkt);
                g_vp.debug_count++;
                return -1;
            }
            /* 防御：避免极端情况下无限循环 */
            if (++send_retry > 16) break;
        }
        if (dbg < 10) {
            if (ret < 0) {
                LV_LOG_WARN("Player DBG[%d]: send_packet err=%d (%s)", dbg, ret, av_err2str(ret));
            } else {
                LV_LOG_WARN("Player DBG[%d]: send_packet ok", dbg);
            }
        }
        av_packet_unref(g_vp.pkt);
        if (ret < 0) continue;  /* 发送错误，丢弃该包继续 */

        /* 成功送入一包后，取出一帧 */
        out = player_output_one_frame();
        if (out == 1) {
            if (dbg < 10) LV_LOG_WARN("Player DBG[%d]: decoded frame ok", dbg);
            g_vp.debug_count++;
            return 0;
        }
        if (out == -1) {
            g_vp.debug_count++;
            return -1;
        }
        /* EAGAIN：需要继续读包，循环 */
    }

    /* av_read_frame 返回 < 0：区分 EOF 和错误 */
    if (dbg < 10) {
        if (ret == AVERROR_EOF) {
            LV_LOG_WARN("Player DBG[%d]: read_frame EOF (read %d pkts)", dbg, read_count);
        } else {
            LV_LOG_WARN("Player DBG[%d]: read_frame err=%d (%s)", dbg, ret, av_err2str(ret));
        }
    }

    /* 3. 文件读取结束：flush 解码器 */
    avcodec_send_packet(g_vp.dec_ctx, NULL);
    out = player_output_one_frame();
    if (out == 1) {
        if (dbg < 10) LV_LOG_WARN("Player DBG[%d]: flush got frame", dbg);
        g_vp.debug_count++;
        return 0;
    }
    if (dbg < 10) LV_LOG_WARN("Player DBG[%d]: flush no frame (out=%d), EOF", dbg, out);

    g_vp.debug_count++;
    return -1;
}

/* ------------------------------------------------------------------ */
/* 定时器回调：控制帧率并解码下一帧                                     */
/* ------------------------------------------------------------------ */
static void player_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!g_vp.playing || !g_vp.fmt_ctx) return;
    if (g_vp.paused) return;

    /* 按帧率控制：离上一帧至少间隔 frame_duration_ms */
    uint32_t elapsed = lv_tick_elaps(g_vp.last_frame_tick);
    if (elapsed < (uint32_t)(g_vp.frame_duration_ms * 0.8)) {
        return;
    }

    if (decode_one_frame() != 0) {
        /* 到达文件末尾：暂停播放并显示播放图标，避免定时器重复尝试解码 */
        g_vp.eof = 1;
        g_vp.paused = 1;
        show_state_icon(LV_SYMBOL_PLAY);
        LV_LOG_USER("Video player: EOF reached (decoded %d frames total)", g_vp.debug_count);
        return;
    }
    g_vp.last_frame_tick = lv_tick_get();
}

/* ------------------------------------------------------------------ */
/* UI 事件                                                              */
/* ------------------------------------------------------------------ */
static void player_toggle_play_cb(lv_event_t *e)
{
    (void)e;
    if (!g_vp.playing || !g_vp.fmt_ctx) return;

    if (g_vp.eof) {
        /* 已播完，再次点击从头播放 */
        int seek_ret = av_seek_frame(g_vp.fmt_ctx, g_vp.video_stream, 0, AVSEEK_FLAG_BACKWARD);
        LV_LOG_USER("Player DBG: replay seek ret=%d", seek_ret);
        avcodec_flush_buffers(g_vp.dec_ctx);
        g_vp.eof = 0;
        g_vp.paused = 0;
        g_vp.debug_count = 0;
        g_vp.last_frame_tick = lv_tick_get();
        show_state_icon(LV_SYMBOL_PAUSE);
        return;
    }

    g_vp.paused = !g_vp.paused;
    show_state_icon(g_vp.paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
}

static void player_close_cb(lv_event_t *e)
{
    (void)e;
    /* 阻止点击事件冒泡到 overlay，避免关闭播放器后又触发播放/暂停 */
    lv_event_stop_bubbling(e);
    video_player_close();
}

static void player_gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_RIGHT) {
        video_player_close();
    }
}

/* ------------------------------------------------------------------ */
/* 播放器初始化                                                         */
/* ------------------------------------------------------------------ */
static int player_init_ffmpeg(const char *video_path)
{
    if (avformat_open_input(&g_vp.fmt_ctx, video_path, NULL, NULL) < 0) {
        LV_LOG_WARN("Player: cannot open %s", video_path);
        return -1;
    }
    if (avformat_find_stream_info(g_vp.fmt_ctx, NULL) < 0) {
        LV_LOG_WARN("Player: cannot find stream info %s", video_path);
        return -1;
    }

    /* 诊断：打印容器和流信息 */
    LV_LOG_USER("Player DBG: format=%s, nb_streams=%u, duration=%lld us",
                g_vp.fmt_ctx->iformat ? g_vp.fmt_ctx->iformat->name : "?",
                g_vp.fmt_ctx->nb_streams,
                (long long)(g_vp.fmt_ctx->duration));

    for (unsigned int i = 0; i < g_vp.fmt_ctx->nb_streams; ++i) {
        AVStream *st = g_vp.fmt_ctx->streams[i];
        AVCodecParameters *cp = st->codecpar;
        LV_LOG_USER("Player DBG: stream[%u] codec_id=%d(%s), type=%d, %dx%d, nb_frames=%lld, duration=%lld, tb=%d/%d",
                    i, cp->codec_id, avcodec_get_name(cp->codec_id),
                    cp->codec_type, cp->width, cp->height,
                    (long long)st->nb_frames, (long long)st->duration,
                    st->time_base.num, st->time_base.den);
    }

    /* find_stream_info 后查找视频流并打开解码器。
     * 缩略图提取器验证：find_stream_info 后无需 seek 即可正常读取首帧。 */

    for (unsigned int i = 0; i < g_vp.fmt_ctx->nb_streams; ++i) {
        if (g_vp.fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            g_vp.video_stream = (int)i;
            break;
        }
    }
    if (g_vp.video_stream < 0) {
        LV_LOG_WARN("Player: no video stream %s", video_path);
        return -1;
    }

    AVCodecParameters *codecpar = g_vp.fmt_ctx->streams[g_vp.video_stream]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        LV_LOG_WARN("Player: decoder not found (codec_id=%d) %s", codecpar->codec_id, video_path);
        return -1;
    }
    LV_LOG_USER("Player DBG: decoder=%s, pix_fmt=%d", codec->name, codecpar->format);
    g_vp.dec_ctx = avcodec_alloc_context3(codec);
    if (!g_vp.dec_ctx) return -1;
    if (avcodec_parameters_to_context(g_vp.dec_ctx, codecpar) < 0) return -1;
    if (avcodec_open2(g_vp.dec_ctx, codec, NULL) < 0) {
        LV_LOG_WARN("Player: cannot open decoder %s", video_path);
        return -1;
    }

    /* find_stream_info 后 demuxer 位置已由 FFmpeg 内部管理（MP4 会回到首帧）。
     * 缩略图提取器验证：不做 seek 即可正确读取首帧。
     * 但需 flush 解码器，清除 find_stream_info 探测阶段可能缓存的帧，
     * 确保首次 decode_one_frame 从干净状态开始。 */
    avcodec_flush_buffers(g_vp.dec_ctx);
    LV_LOG_USER("Player DBG: decoder flushed after find_stream_info");

    g_vp.src_w = g_vp.dec_ctx->width;
    g_vp.src_h = g_vp.dec_ctx->height;
    if (g_vp.src_w <= 0 || g_vp.src_h <= 0) return -1;

    /* 估算帧率 */
    AVStream *st = g_vp.fmt_ctx->streams[g_vp.video_stream];
    if (st->avg_frame_rate.den > 0 && st->avg_frame_rate.num > 0) {
        g_vp.fps = av_q2d(st->avg_frame_rate);
    } else if (st->r_frame_rate.den > 0 && st->r_frame_rate.num > 0) {
        g_vp.fps = av_q2d(st->r_frame_rate);
    } else {
        g_vp.fps = 30.0;
    }
    if (g_vp.fps <= 0 || g_vp.fps > 120) g_vp.fps = 30.0;
    g_vp.frame_duration_ms = 1000.0 / g_vp.fps;

    /* 计算显示尺寸（保持宽高比） */
    int dst_w, dst_h;
    calc_display_size(g_vp.src_w, g_vp.src_h, &dst_w, &dst_h);

    /* 分配 RGB888 缓冲 */
    int rgb_buf_sz = av_image_get_buffer_size(AV_PIX_FMT_RGB24, dst_w, dst_h, 1);
    if (rgb_buf_sz <= 0) return -1;
    g_vp.rgb_buf = (uint8_t *)av_malloc((size_t)rgb_buf_sz);
    if (!g_vp.rgb_buf) return -1;

    g_vp.frame = av_frame_alloc();
    g_vp.rgb_frame = av_frame_alloc();
    g_vp.pkt = av_packet_alloc();
    if (!g_vp.frame || !g_vp.rgb_frame || !g_vp.pkt) return -1;

    av_image_fill_arrays(g_vp.rgb_frame->data, g_vp.rgb_frame->linesize,
                         g_vp.rgb_buf, AV_PIX_FMT_RGB24, dst_w, dst_h, 1);

    g_vp.sws_ctx = sws_getContext(g_vp.src_w, g_vp.src_h, g_vp.dec_ctx->pix_fmt,
                                  dst_w, dst_h, AV_PIX_FMT_RGB24,
                                  SWS_BILINEAR, NULL, NULL, NULL);
    if (!g_vp.sws_ctx) return -1;

    /* 初始化 LVGL 图像描述符 */
    memset(&g_vp.img_dsc, 0, sizeof(g_vp.img_dsc));
    g_vp.img_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    g_vp.img_dsc.header.cf = LV_COLOR_FORMAT_RGB888;
    g_vp.img_dsc.header.w = (uint32_t)dst_w;
    g_vp.img_dsc.header.h = (uint32_t)dst_h;
    g_vp.img_dsc.header.stride = (uint32_t)(dst_w * 3);
    g_vp.img_dsc.data_size = (uint32_t)rgb_buf_sz;
    g_vp.img_dsc.data = g_vp.rgb_buf;

    return 0;
}

/* ------------------------------------------------------------------ */
/* 公共 API                                                             */
/* ------------------------------------------------------------------ */
void video_player_open(lv_obj_t *parent, const char *video_path,
                       void (*on_close_cb)(void *), void *user_data)
{
    if (!video_path || !video_path[0] || !is_video_file(video_path)) {
        LV_LOG_WARN("Player: invalid video path");
        return;
    }

    /* 关闭旧播放器 */
    video_player_close();

    memset(&g_vp, 0, sizeof(g_vp));
    g_vp.close_cb = on_close_cb;
    g_vp.close_user_data = user_data;

    /* 初始化 FFmpeg */
    if (player_init_ffmpeg(video_path) != 0) {
        player_cleanup();
        LV_LOG_WARN("Player: init failed for %s", video_path);
        return;
    }

    /* 创建全屏覆盖层 */
    g_vp.overlay = lv_obj_create(parent);
    lv_obj_set_size(g_vp.overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(g_vp.overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_vp.overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(g_vp.overlay, 0, 0);
    lv_obj_set_style_border_width(g_vp.overlay, 0, 0);
    lv_obj_remove_flag(g_vp.overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(g_vp.overlay, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_move_foreground(g_vp.overlay);

    /* 点击画面切换播放/暂停 */
    lv_obj_add_event_cb(g_vp.overlay, player_toggle_play_cb, LV_EVENT_CLICKED, NULL);
    /* 右滑关闭 */
    lv_obj_add_event_cb(g_vp.overlay, player_gesture_cb, LV_EVENT_GESTURE, NULL);

    /* 视频图像 */
    g_vp.img = lv_image_create(g_vp.overlay);
    lv_obj_set_size(g_vp.img, g_vp.img_dsc.header.w, g_vp.img_dsc.header.h);
    lv_obj_center(g_vp.img);
    lv_image_set_src(g_vp.img, &g_vp.img_dsc);

    /* 左上角返回按钮 */
    int bsz = ui_scale_min(44);
    g_vp.back_btn = lv_button_create(g_vp.overlay);
    lv_obj_set_size(g_vp.back_btn, bsz, bsz);
    lv_obj_align(g_vp.back_btn, LV_ALIGN_TOP_LEFT, ui_scale_min(8), ui_scale_min(8));
    lv_obj_set_style_bg_color(g_vp.back_btn, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_vp.back_btn, LV_OPA_50, 0);
    lv_obj_set_style_radius(g_vp.back_btn, bsz / 2, 0);
    lv_obj_set_style_border_width(g_vp.back_btn, 0, 0);
    lv_obj_remove_flag(g_vp.back_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(g_vp.back_btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(g_vp.back_btn, player_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *bl = lv_label_create(g_vp.back_btn);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(bl, ui_font_sym(), 0);
    lv_obj_set_style_text_color(bl, lv_color_white(), 0);
    lv_obj_center(bl);

    /* 播放/暂停状态图标（居中，初始隐藏） */
    g_vp.state_icon = lv_label_create(g_vp.overlay);
    lv_label_set_text(g_vp.state_icon, LV_SYMBOL_PAUSE);
    lv_obj_set_style_text_font(g_vp.state_icon, ui_font_sym(), 0);
    lv_obj_set_style_text_color(g_vp.state_icon, lv_color_white(), 0);
    lv_obj_add_flag(g_vp.state_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(g_vp.state_icon);

    /* 预解码第一帧并显示 */
    int pre_ret = decode_one_frame();
    LV_LOG_USER("Player DBG: pre-decode ret=%d", pre_ret);
    if (pre_ret == 0) {
        g_vp.playing = 1;
        g_vp.paused = 0;
        g_vp.last_frame_tick = lv_tick_get();
        show_state_icon(LV_SYMBOL_PAUSE);
    } else {
        g_vp.playing = 0;
        g_vp.paused = 1;
        show_state_icon(LV_SYMBOL_PLAY);
    }

    /* 启动定时器：按估算帧率驱动解码 */
    uint32_t period_ms = (uint32_t)(g_vp.frame_duration_ms + 0.5);
    if (period_ms < 16) period_ms = 16;
    if (period_ms > 100) period_ms = 100;
    g_vp.timer = lv_timer_create(player_timer_cb, period_ms, NULL);

    LV_LOG_USER("Video player opened: %s (%dx%d @ %.1ffps), display=%dx%d",
                video_path, g_vp.src_w, g_vp.src_h, g_vp.fps,
                (int)g_vp.img_dsc.header.w, (int)g_vp.img_dsc.header.h);
}

void video_player_close(void)
{
    if (!g_vp.overlay && !g_vp.fmt_ctx) return;

    void (*cb)(void *) = g_vp.close_cb;
    void *ud = g_vp.close_user_data;

    player_cleanup();
    LV_LOG_USER("Video player closed");

    if (cb) cb(ud);
}

int video_player_available(void)
{
    return 1;
}

#else  /* !APP_HAS_FFMPEG */

void video_player_open(lv_obj_t *parent, const char *video_path,
                       void (*on_close_cb)(void *), void *user_data)
{
    (void)parent; (void)video_path; (void)on_close_cb; (void)user_data;
    LV_LOG_WARN("Player: FFmpeg not available");
}

void video_player_close(void)
{
}

int video_player_available(void)
{
    return 0;
}

#endif /* APP_HAS_FFMPEG */
