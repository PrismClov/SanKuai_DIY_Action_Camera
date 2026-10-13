/**
 * @file mp4_muxer.h
 * @brief MP4 容器写入器（支持 MJPEG 和 H.264）
 *
 * 设计目标：
 *   - 最小可用 MP4 muxer，仅支持单视频轨（无音频）
 *   - 支持 MJPEG（每帧独立 JPEG，全关键帧）和 H.264（AVCC 格式 NALU）
 *   - moov 在文件尾部（faststart 风格的逆序：ftyp + mdat + moov）
 *     现代播放器（VLC/mpv/QuickTime/Windows 10+）均支持尾部 moov
 *   - 与 recorder 后端解耦：后端编码后调用 mp4_muxer_write_sample 写入
 *
 * 文件结构：
 *   ftyp  (文件类型: isom/iso2/avc1/mp41)
 *   mdat  (媒体数据: 逐帧写入，记录偏移)
 *   moov  (电影盒: mvhd + trak/tkhd/mdia/mdhd/hdlr/minf/stbl)
 *     └─ stbl: stsd(mjpa/avc1) + stts + stsc + stsz + stco
 *
 * H.264 处理：
 *   - 输入 Annex-B 格式（0x00000001 起始码 + NALU）
 *   - 内部转换为 AVCC 格式（4字节长度前缀 + NALU）写入 mdat
 *   - SPS/PPS 提取后写入 avcC box（stsd 内）
 *
 * 使用流程：
 *   mp4_muxer_t *m = mp4_muxer_create("/tmp/test.mp4", 1920, 1080, 30, MP4_TRACK_H264);
 *   if (m) {
 *       while (recording) {
 *           mp4_muxer_write_sample(m, nal_data, nal_size, is_keyframe);
 *       }
 *       mp4_muxer_close(m);  // 写入 moov 并关闭文件
 *   }
 */
#ifndef MP4_MUXER_H
#define MP4_MUXER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 类型定义                                                            */
/* ------------------------------------------------------------------ */

/* 轨道类型（决定 stsd 中的 sample description） */
typedef enum {
    MP4_TRACK_MJPEG = 0,   /* MJPEG：每帧为独立 JPEG，全关键帧 */
    MP4_TRACK_H264  = 1,   /* H.264：NALU 流，需 SPS/PPS 提取 */
} mp4_track_type_t;

/* MP4 muxer 句柄（不透明指针） */
typedef struct mp4_muxer mp4_muxer_t;

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/* 创建 MP4 muxer。
 * 参数：
 *   path       - 输出文件路径（会自动创建父目录）
 *   width      - 视频宽度（像素）
 *   height     - 视频高度（像素）
 *   fps        - 帧率（用于 timescale 计算）
 *   track_type - 轨道类型（MJPEG 或 H.264）
 * 返回：成功返回句柄，失败返回 NULL */
mp4_muxer_t * mp4_muxer_create(const char *path, int width, int height,
                                int fps, mp4_track_type_t track_type);

/* 写入一帧 sample。
 * 参数：
 *   m           - muxer 句柄
 *   data        - 帧数据
 *   size        - 帧数据大小（字节）
 *   is_keyframe - 是否关键帧（H.264 IDR 帧；MJPEG 始终传 1）
 * 返回：0 成功，负数失败。
 * 注意：
 *   - H.264：data 为 Annex-B 格式（含 0x00000001 起始码），内部转 AVCC
 *   - MJPEG：data 为完整 JPEG 数据（含 SOI/EOI） */
int mp4_muxer_write_sample(mp4_muxer_t *m, const uint8_t *data, size_t size,
                            int is_keyframe);

/* 关闭 muxer（写入 moov + 关闭文件）。返回 0 成功，负数失败。 */
int mp4_muxer_close(mp4_muxer_t *m);

/* 销毁 muxer（未 close 时强制释放资源，文件可能不完整） */
void mp4_muxer_destroy(mp4_muxer_t *m);

#ifdef __cplusplus
}
#endif

#endif /* MP4_MUXER_H */
