/**
 * @file mp4_muxer.c
 * @brief MP4 容器写入器实现（支持 MJPEG 和 H.264）
 *
 * 文件结构（faststart：moov 在开头，最大播放兼容性）：
 *   ftyp  (32字节: isom/iso2/avc1/mp41)
 *   moov  (mvhd + trak/tkhd/mdia/mdhd/hdlr/minf/stbl)
 *   mdat  (帧数据，逐帧写入)
 *
 * Faststart 实现：
 *   - 录制时按 [ftyp|mdat] 顺序写入（mdat 偏移已知）
 *   - close 时：计算 moov 大小 → 调整 stco 偏移 → 写 moov 到临时文件
 *   - 最终重组为 [ftyp|moov|mdat] 并替换原文件
 *   - 失败时回退到尾部 moov（[ftyp|mdat|moov]）
 *
 * H.264 处理：
 *   - 输入 Annex-B（0x00000001 起始码），输出 AVCC（4字节长度前缀）
 *   - SPS/PPS 从首个 IDR 帧中提取，写入 avcC box
 *   - SPS/PPS 不写入 mdat（避免重复，由 avcC 提供）
 */
#include "mp4_muxer.h"
#include "../app_main.h"

#include "lvgl/lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>   /* mkdir() */
#include <sys/types.h>

/* ------------------------------------------------------------------ */
/* 配置常量                                                            */
/* ------------------------------------------------------------------ */

/* 帧索引初始容量（30fps*60s=1800帧） */
#define MP4_SAMPLE_INIT_CAP 1800

/* H.264 AVCC 转换缓冲区初始大小（1帧 1080p H.264 约 100~500KB） */
#define AVCC_BUF_INIT_SZ (512 * 1024)

/* SPS/PPS 最大大小（High profile H.264 SPS 可达 50~80 字节，留余量） */
#define SPS_MAX_SZ 256
#define PPS_MAX_SZ 256

/* ------------------------------------------------------------------ */
/* 数据结构                                                            */
/* ------------------------------------------------------------------ */

struct mp4_muxer {
    FILE             *fp;            /* 输出文件 */
    int               width;         /* 视频宽度 */
    int               height;        /* 视频高度 */
    int               fps;           /* 帧率 */
    mp4_track_type_t  track_type;    /* 轨道类型 */
    char              output_path[512]; /* 输出路径（用于 faststart 重命名） */

    /* mdat 写入状态 */
    long              mdat_offset;   /* mdat box 起始偏移 */
    long              mdat_data_start;/* mdat 数据起始偏移（= mdat_offset + 8） */

    /* 帧索引（用于 stts/stsc/stsz/stco） */
    uint32_t         *sample_sizes;  /* 每帧大小（字节） */
    uint32_t         *sample_offsets;/* 每帧偏移（相对文件起始） */
    int               sample_count;  /* 已写入帧数 */
    int               sample_cap;    /* 索引容量 */

    /* H.264 SPS/PPS（从首个 IDR 帧提取，用于 avcC box） */
    uint8_t           sps[SPS_MAX_SZ];
    int               sps_size;
    uint8_t           pps[PPS_MAX_SZ];
    int               pps_size;
    int               sps_pps_extracted; /* SPS/PPS 是否已提取 */

    /* H.264 AVCC 转换缓冲区（Annex-B → AVCC） */
    uint8_t          *avcc_buf;
    int               avcc_buf_sz;
};

/* ------------------------------------------------------------------ */
/* 大端写入辅助（MP4 使用大端字节序，与 AVI 相反）                       */
/* ------------------------------------------------------------------ */

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)((v >> 8) & 0xFF);
    p[1] = (uint8_t)(v & 0xFF);
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)((v >> 24) & 0xFF);
    p[1] = (uint8_t)((v >> 16) & 0xFF);
    p[2] = (uint8_t)((v >> 8) & 0xFF);
    p[3] = (uint8_t)(v & 0xFF);
}

/* ------------------------------------------------------------------ */
/* 目录创建（递归）                                                     */
/* ------------------------------------------------------------------ */

static void create_parent_dir(const char *path)
{
    char dir[256];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash) return;
    *slash = '\0';
    /* 逐级创建目录 */
    for (char *p = dir + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(dir, 0755);
            *p = '/';
        }
    }
    mkdir(dir, 0755);
}

/* ------------------------------------------------------------------ */
/* H.264 Annex-B → AVCC 转换 + SPS/PPS 提取                            */
/* ------------------------------------------------------------------ */

/* 查找下一个起始码位置（返回 0x000001 或 0x00000001 的起始位置，找不到返回 size） */
static size_t find_start_code(const uint8_t *data, size_t size, size_t from)
{
    for (size_t i = from; i + 2 < size; i++) {
        if (data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) return i;          /* 3字节起始码 */
            if (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1) return i; /* 4字节 */
        }
    }
    return size;
}

/* 返回起始码长度（3 或 4），非起始码返回 0 */
static int start_code_len(const uint8_t *data, size_t pos, size_t size)
{
    if (pos + 3 > size) return 0;
    if (data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 1) return 3;
    if (pos + 4 > size) return 0;
    if (data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 0 && data[pos + 3] == 1) return 4;
    return 0;
}

/* 将 Annex-B 格式 H.264 流转换为 AVCC 格式，并提取 SPS/PPS。
 * 参数：
 *   annexb    - 输入数据（含 0x00000001 起始码）
 *   size      - 输入数据大小
 *   m         - muxer 句柄（用于保存 SPS/PPS 和 AVCC 缓冲区）
 * 返回：AVCC 数据大小（写入 m->avcc_buf），负数失败 */
static int h264_annexb_to_avcc(const uint8_t *annexb, size_t size, mp4_muxer_t *m)
{
    int out_pos = 0;
    size_t i = 0;

    while (i < size) {
        int sc_len = start_code_len(annexb, i, size);
        if (sc_len == 0) {
            i++;
            continue;
        }

        size_t nal_start = i + sc_len;
        size_t next_sc = find_start_code(annexb, size, nal_start);
        int nal_size = (int)(next_sc - nal_start);
        if (nal_size <= 0) {
            i = next_sc;
            continue;
        }

        /* NALU 第1字节低5位为 type */
        uint8_t nal_type = annexb[nal_start] & 0x1F;

        if (nal_type == 7) {
            /* SPS */
            if (!m->sps_pps_extracted && nal_size <= SPS_MAX_SZ) {
                memcpy(m->sps, annexb + nal_start, nal_size);
                m->sps_size = nal_size;
            }
        } else if (nal_type == 8) {
            /* PPS */
            if (!m->sps_pps_extracted && nal_size <= PPS_MAX_SZ) {
                memcpy(m->pps, annexb + nal_start, nal_size);
                m->pps_size = nal_size;
            }
        } else if (nal_type == 6) {
            /* SEI，跳过不写入 mdat */
        } else {
            /* IDR(5)/非IDR(1) 等：写入 AVCC（4字节大端长度 + NALU） */
            if (out_pos + 4 + nal_size > m->avcc_buf_sz) {
                /* 缓冲区不足，尝试扩容 */
                int new_sz = m->avcc_buf_sz * 2;
                while (new_sz < out_pos + 4 + nal_size) new_sz *= 2;
                uint8_t *new_buf = realloc(m->avcc_buf, new_sz);
                if (!new_buf) {
                    LV_LOG_ERROR("MP4: AVCC 缓冲区扩容失败");
                    return -1;
                }
                m->avcc_buf = new_buf;
                m->avcc_buf_sz = new_sz;
            }
            put_be32(m->avcc_buf + out_pos, (uint32_t)nal_size);
            out_pos += 4;
            memcpy(m->avcc_buf + out_pos, annexb + nal_start, nal_size);
            out_pos += nal_size;
        }

        i = next_sc;
    }

    /* 标记 SPS/PPS 已提取（后续帧不再重复提取） */
    if (m->sps_size > 0 && m->pps_size > 0) {
        m->sps_pps_extracted = 1;
    }

    return out_pos;
}

/* ------------------------------------------------------------------ */
/* MP4 box 写入                                                        */
/* ------------------------------------------------------------------ */

/* 写入 ftyp box（文件类型） */
static void mp4_write_ftyp(mp4_muxer_t *m)
{
    /* ftyp: major(isom) + minor(0x200) + compatible(isom, iso2, avc1, mp41) */
    uint8_t ftyp[32];
    put_be32(ftyp, 32);                 /* box size */
    memcpy(ftyp + 4, "ftyp", 4);        /* box type */
    memcpy(ftyp + 8, "isom", 4);        /* major brand */
    put_be32(ftyp + 12, 0x200);         /* minor version */
    memcpy(ftyp + 16, "isom", 4);       /* compatible brand 1 */
    memcpy(ftyp + 20, "iso2", 4);       /* compatible brand 2 */
    memcpy(ftyp + 24, "avc1", 4);       /* compatible brand 3 */
    memcpy(ftyp + 28, "mp41", 4);       /* compatible brand 4 */
    fwrite(ftyp, 1, 32, m->fp);
}

/* 写入 mdat box 头部（size 占位，后续回填） */
static void mp4_write_mdat_header(mp4_muxer_t *m)
{
    m->mdat_offset = ftell(m->fp);
    uint8_t hdr[8];
    put_be32(hdr, 0);                   /* size 占位（close 时回填） */
    memcpy(hdr + 4, "mdat", 4);
    fwrite(hdr, 1, 8, m->fp);
    m->mdat_data_start = m->mdat_offset + 8;
}

/* ------------------------------------------------------------------ */
/* moov box 构建（内存缓冲区构建，最后写入文件）                        */
/* ------------------------------------------------------------------ */

/* 简化 box 写入辅助：往缓冲区写入一个完整 box */
/* 为简化实现，moov 直接写到文件，使用回填方式处理嵌套 box 大小 */

/* 写入 mvhd box（movie header）
 * mvhd v0 布局（108字节）：
 *   0-7: size+type
 *   8-11: version(1)+flags(3)
 *   12-15: creation_time
 *   16-19: modification_time
 *   20-23: timescale
 *   24-27: duration
 *   28-31: rate（16.16固定点，1.0=0x00010000）
 *   32-33: volume（8.8固定点，1.0=0x0100）
 *   34-43: reserved(10)
 *   44-79: matrix(36) 单位矩阵：[0][0]@44, [1][1]@60, [2][2]@76
 *   80-103: pre_defined(24)
 *   104-107: next_track_ID */
static void write_mvhd(mp4_muxer_t *m, uint32_t timescale, uint32_t duration)
{
    uint8_t b[108];
    memset(b, 0, sizeof(b));
    put_be32(b, 108);                   /* box size */
    memcpy(b + 4, "mvhd", 4);           /* box type */
    /* version(1)=0 + flags(3)=0 */
    /* creation_time(4)=0, modification_time(4)=0 */
    put_be32(b + 20, timescale);        /* timescale */
    put_be32(b + 24, duration);         /* duration */
    put_be32(b + 28, 0x00010000);       /* rate = 1.0 */
    put_be16(b + 32, 0x0100);           /* volume = 1.0 */
    /* reserved(10) = 0 */
    /* matrix(36): 单位矩阵 */
    put_be32(b + 44, 0x00010000);       /* [0][0] = 1.0 */
    put_be32(b + 60, 0x00010000);       /* [1][1] = 1.0 */
    put_be32(b + 76, 0x40000000);       /* [2][2] = 1.0 */
    /* pre_defined(24) = 0 */
    put_be32(b + 104, 2);               /* next_track_ID */
    fwrite(b, 1, 108, m->fp);
}

/* 写入 tkhd box（track header）
 * tkhd v0 布局（92字节）：
 *   0-7: size+type
 *   8-11: version(1)+flags(3)
 *   12-15: creation_time
 *   16-19: modification_time
 *   20-23: track_ID
 *   24-27: reserved
 *   28-31: duration
 *   32-39: reserved(8)
 *   40-41: layer
 *   42-43: alternate_group
 *   44-45: volume
 *   46-47: reserved(2)
 *   48-83: matrix(36) 单位矩阵：[0][0]@48, [1][1]@64, [2][2]@80
 *   84-87: width（16.16固定点）
 *   88-91: height（16.16固定点） */
static void write_tkhd(mp4_muxer_t *m, uint32_t duration, uint32_t timescale)
{
    uint8_t b[92];
    memset(b, 0, sizeof(b));
    put_be32(b, 92);                    /* box size */
    memcpy(b + 4, "tkhd", 4);           /* box type */
    put_be32(b + 8, 0x000007);          /* version=0, flags=7(enabled+in_movie+in_preview) */
    /* creation_time(4)=0, modification_time(4)=0 */
    put_be32(b + 20, 1);                /* track_ID */
    /* reserved(4)=0 */
    put_be32(b + 28, duration);         /* duration（movie timescale 单位） */
    /* reserved(8)=0 */
    put_be16(b + 40, 0);                /* layer */
    put_be16(b + 42, 0);                /* alternate_group */
    put_be16(b + 44, 0);                /* volume = 0（视频） */
    /* reserved(2)=0 */
    /* matrix(36): 单位矩阵 */
    put_be32(b + 48, 0x00010000);       /* [0][0] = 1.0 */
    put_be32(b + 64, 0x00010000);       /* [1][1] = 1.0 */
    put_be32(b + 80, 0x40000000);       /* [2][2] = 1.0 */
    /* width(4, 16.16固定点) + height(4, 16.16固定点) */
    put_be32(b + 84, (uint32_t)m->width << 16);   /* width */
    put_be32(b + 88, (uint32_t)m->height << 16);  /* height */
    fwrite(b, 1, 92, m->fp);
    (void)timescale;  /* duration 已是 movie timescale 单位，timescale 参数未使用 */
}

/* 写入 mdhd box（media header） */
static void write_mdhd(mp4_muxer_t *m, uint32_t timescale, uint32_t duration)
{
    uint8_t b[32];
    memset(b, 0, sizeof(b));
    put_be32(b, 32);                    /* box size */
    memcpy(b + 4, "mdhd", 4);
    /* version=0, flags=0 */
    /* creation_time=0, modification_time=0 */
    put_be32(b + 12, timescale);        /* timescale */
    put_be32(b + 16, duration);         /* duration */
    put_be16(b + 20, 0x55C4);           /* language = 'und' */
    put_be16(b + 22, 0);                /* quality */
    fwrite(b, 1, 32, m->fp);
}

/* 写入 hdlr box（handler） */
static void write_hdlr(mp4_muxer_t *m)
{
    /* hdlr box: 8(header) + 4(ver/flags) + 4(pre_defined) + 4(handler_type) + 12(reserved) + 13(name "VideoHandler\0") = 45 */
    uint8_t buf[45];
    memset(buf, 0, sizeof(buf));
    put_be32(buf, 45);
    memcpy(buf + 4, "hdlr", 4);
    /* version=0, flags=0 */
    /* pre_defined(4)=0 */
    memcpy(buf + 12, "vide", 4);        /* handler_type */
    /* reserved(12)=0 */
    memcpy(buf + 32, "VideoHandler", 12); /* name（不含null由memset补0） */
    fwrite(buf, 1, 45, m->fp);
}

/* 写入 vmhd box（video media header） */
static void write_vmhd(mp4_muxer_t *m)
{
    uint8_t b[20];
    memset(b, 0, sizeof(b));
    put_be32(b, 20);
    memcpy(b + 4, "vmhd", 4);
    put_be32(b + 8, 1);                 /* version=0, flags=1 */
    /* graphicsmode(2)=0, opcolor(6)=0 */
    fwrite(b, 1, 20, m->fp);
}

/* 写入 dinf/dref box（data reference） */
static void write_dinf(mp4_muxer_t *m)
{
    /* dinf 包含 dref，dref 包含一个 url entry */
    uint8_t b[36];
    memset(b, 0, sizeof(b));
    /* dinf header */
    put_be32(b, 36);
    memcpy(b + 4, "dinf", 4);
    /* dref header */
    put_be32(b + 8, 28);                /* dref size = 28 */
    memcpy(b + 12, "dref", 4);
    /* version=0, flags=0 */
    put_be32(b + 20, 1);                /* entry_count = 1 */
    /* url entry: size=12, 'url ', flags=1(self-contained) */
    put_be32(b + 24, 12);
    memcpy(b + 28, "url ", 4);
    put_be32(b + 32, 1);                /* version=0, flags=1 */
    fwrite(b, 1, 36, m->fp);
}

/* 写入 stsd box（sample description） */
static void write_stsd(mp4_muxer_t *m)
{
    if (m->track_type == MP4_TRACK_H264) {
        /* avc1 + avcC */
        /* avcC box 内容（ISO/IEC 14496-15 AVCDecoderConfigurationRecord）：
         *   1(configVersion) + 1(profile) + 1(compat) + 1(level)
         *   + 1(0xFF, lengthSizeMinusOne=3 → 4字节长度)
         *   + 1(0xE1, numSPS=1) + 2(SPS len) + SPS
         *   + 1(numPPS=1) + 2(PPS len) + PPS
         * = 6 + 2 + sps + 1 + 2 + pps = 11 + sps + pps */
        int avcc_content_sz = 11 + m->sps_size + m->pps_size;
        int avcc_box_sz = 8 + avcc_content_sz;

        /* avc1 entry（ISO/IEC 14496-12 VisualSampleEntry）：
         *   8(size+'avc1') + 6(reserved) + 2(dref_idx)
         *   + 2(pre_defined) + 2(reserved) + 12(pre_defined[3])
         *   + 2(w) + 2(h) + 4(hres) + 4(vres) + 4(reserved) + 2(frame_count)
         *   + 32(compressorname) + 2(depth) + 2(pre_defined) + avcC box
         * = 8 + 78 + avcC */
        int avc1_sz = 8 + 78 + avcc_box_sz;
        int stsd_sz = 8 + 8 + avc1_sz;  /* stsd header(8) + ver/flags(4) + entry_count(4) + entry */

        uint8_t *buf = (uint8_t *)malloc(stsd_sz);
        if (!buf) {
            LV_LOG_ERROR("MP4: stsd 缓冲区分配失败");
            return;
        }
        memset(buf, 0, stsd_sz);
        int p = 0;

        /* stsd box */
        put_be32(buf + p, stsd_sz); p += 4;
        memcpy(buf + p, "stsd", 4); p += 4;
        put_be32(buf + p, 0); p += 4;       /* version=0, flags=0 */
        put_be32(buf + p, 1); p += 4;       /* entry_count = 1 */

        /* avc1 entry */
        put_be32(buf + p, avc1_sz); p += 4;
        memcpy(buf + p, "avc1", 4); p += 4;
        /* reserved(6)=0 */
        put_be16(buf + p + 6, 1);           /* data_ref_index = 1 */
        p += 8;
        /* VisualSampleEntry: pre_defined(2) + reserved(2) + pre_defined[3](3×uint32=12) */
        p += 4;   /* pre_defined + reserved */
        p += 12;  /* pre_defined[3]: 3×uint32, 全0（memset） */
        put_be16(buf + p, (uint16_t)m->width); p += 2;
        put_be16(buf + p, (uint16_t)m->height); p += 2;
        put_be32(buf + p, 0x00480000); p += 4;  /* horizresolution = 72dpi */
        put_be32(buf + p, 0x00480000); p += 4;  /* vertresolution = 72dpi */
        put_be32(buf + p, 0); p += 4;          /* reserved */
        put_be16(buf + p, 1); p += 2;          /* frame_count = 1 */
        /* compressorname(32): 第1字节为长度，后跟字符串 */
        /* 留空（32字节0） */
        p += 32;
        put_be16(buf + p, 24); p += 2;        /* depth = 24 */
        put_be16(buf + p, 0xFFFF); p += 2;    /* pre_defined = -1 */

        /* avcC box */
        put_be32(buf + p, avcc_box_sz); p += 4;
        memcpy(buf + p, "avcC", 4); p += 4;
        buf[p++] = 1;                         /* configurationVersion = 1 */
        buf[p++] = m->sps_size > 0 ? m->sps[1] : 66;  /* AVCProfileIndication（取自 SPS） */
        buf[p++] = m->sps_size > 0 ? m->sps[2] : 0;   /* profile_compatibility */
        buf[p++] = m->sps_size > 0 ? m->sps[3] : 30;  /* AVCLevelIndication */
        buf[p++] = 0xFF;                      /* 111111 + lengthSizeMinusOne(3) = 4字节长度 */
        buf[p++] = 0xE1;                      /* 111 + numSPS(1) */
        put_be16(buf + p, (uint16_t)m->sps_size); p += 2;
        memcpy(buf + p, m->sps, m->sps_size); p += m->sps_size;
        buf[p++] = 1;                         /* numPPS = 1 */
        put_be16(buf + p, (uint16_t)m->pps_size); p += 2;
        memcpy(buf + p, m->pps, m->pps_size); p += m->pps_size;

        fwrite(buf, 1, stsd_sz, m->fp);
        free(buf);
    } else {
        /* mjpg entry（MJPEG）
         * fourcc 用 "mjpg" 而非 ISO 标准的 "mjpa"：
         *   - "mjpg" 被 FFmpeg/VLC/Android/iOS/Windows Media Player 广泛支持
         *   - "mjpa" 部分手机播放器（Android 系统播放器）不认识，导致无法播放
         * entry 布局（ISO/IEC 14496-12 VisualSampleEntry）:
         *   8(size+'mjpg') + 6(reserved) + 2(dref_idx)
         *   + 2(pre_defined) + 2(reserved) + 12(pre_defined[3])
         *   + 2(w) + 2(h) + 4(hres) + 4(vres) + 4(reserved) + 2(frame_count)
         *   + 32(compressorname) + 2(depth) + 2(pre_defined)
         * = 8 + 78 = 86 */
        int mjp1_sz = 86;
        int stsd_sz = 8 + 8 + mjp1_sz;

        uint8_t buf[8 + 8 + 86];
        memset(buf, 0, sizeof(buf));
        int p = 0;

        /* stsd box */
        put_be32(buf + p, stsd_sz); p += 4;
        memcpy(buf + p, "stsd", 4); p += 4;
        put_be32(buf + p, 0); p += 4;       /* version=0, flags=0 */
        put_be32(buf + p, 1); p += 4;       /* entry_count = 1 */

        /* mjpg entry */
        put_be32(buf + p, mjp1_sz); p += 4;
        memcpy(buf + p, "mjpg", 4); p += 4;
        p += 6;                              /* reserved */
        put_be16(buf + p, 1); p += 2;        /* data_ref_index = 1 */
        /* VisualSampleEntry: pre_defined(2) + reserved(2) + pre_defined[3](3×uint32=12) */
        p += 4;   /* pre_defined + reserved */
        p += 12;  /* pre_defined[3]: 3×uint32, 全0（memset） */
        put_be16(buf + p, (uint16_t)m->width); p += 2;
        put_be16(buf + p, (uint16_t)m->height); p += 2;
        put_be32(buf + p, 0x00480000); p += 4;  /* horizresolution */
        put_be32(buf + p, 0x00480000); p += 4;  /* vertresolution */
        p += 4;                              /* reserved */
        put_be16(buf + p, 1); p += 2;        /* frame_count = 1 */
        p += 32;                             /* compressorname */
        put_be16(buf + p, 24); p += 2;       /* depth = 24 */
        put_be16(buf + p, 0xFFFF); p += 2;   /* pre_defined = -1 */

        fwrite(buf, 1, stsd_sz, m->fp);
    }
}

/* 写入 stts box（time to sample：每帧 1/fps 秒） */
static void write_stts(mp4_muxer_t *m)
{
    /* 1 entry: sample_count=总帧数, sample_delta=1（timescale=fps 时每帧1单位） */
    uint8_t b[24];
    memset(b, 0, sizeof(b));
    put_be32(b, 24);
    memcpy(b + 4, "stts", 4);
    put_be32(b + 8, 0);                 /* version=0, flags=0 */
    put_be32(b + 12, 1);                /* entry_count = 1 */
    put_be32(b + 16, (uint32_t)m->sample_count);  /* sample_count */
    put_be32(b + 20, 1);                /* sample_delta = 1 */
    fwrite(b, 1, 24, m->fp);
}

/* 写入 stsc box（sample to chunk：每 chunk 1 sample） */
static void write_stsc(mp4_muxer_t *m)
{
    uint8_t b[28];
    memset(b, 0, sizeof(b));
    put_be32(b, 28);
    memcpy(b + 4, "stsc", 4);
    put_be32(b + 8, 0);                 /* version=0, flags=0 */
    put_be32(b + 12, 1);                /* entry_count = 1 */
    put_be32(b + 16, 1);                /* first_chunk = 1 */
    put_be32(b + 20, 1);                /* samples_per_chunk = 1 */
    put_be32(b + 24, 1);                /* sample_description_index = 1 */
    fwrite(b, 1, 28, m->fp);
}

/* 写入 stsz box（sample sizes：每帧大小） */
static void write_stsz(mp4_muxer_t *m)
{
    int sz = 12 + 8 + m->sample_count * 4;  /* header(8) + ver/flags(4) + sample_size(4) + count(4) + entries */
    uint8_t *buf = (uint8_t *)malloc(sz);
    if (!buf) {
        LV_LOG_ERROR("MP4: stsz 缓冲区分配失败");
        return;
    }
    memset(buf, 0, sz);
    put_be32(buf, sz);
    memcpy(buf + 4, "stsz", 4);
    put_be32(buf + 8, 0);               /* version=0, flags=0 */
    put_be32(buf + 12, 0);              /* sample_size = 0（变长） */
    put_be32(buf + 16, (uint32_t)m->sample_count);
    for (int i = 0; i < m->sample_count; i++) {
        put_be32(buf + 20 + i * 4, m->sample_sizes[i]);
    }
    fwrite(buf, 1, sz, m->fp);
    free(buf);
}

/* 写入 stco box（chunk offsets：每帧偏移） */
static void write_stco(mp4_muxer_t *m)
{
    int sz = 8 + 8 + m->sample_count * 4;  /* header(8) + ver/flags(4) + count(4) + entries */
    uint8_t *buf = (uint8_t *)malloc(sz);
    if (!buf) {
        LV_LOG_ERROR("MP4: stco 缓冲区分配失败");
        return;
    }
    memset(buf, 0, sz);
    put_be32(buf, sz);
    memcpy(buf + 4, "stco", 4);
    put_be32(buf + 8, 0);               /* version=0, flags=0 */
    put_be32(buf + 12, (uint32_t)m->sample_count);
    for (int i = 0; i < m->sample_count; i++) {
        put_be32(buf + 16 + i * 4, m->sample_offsets[i]);
    }
    fwrite(buf, 1, sz, m->fp);
    free(buf);
}

/* 写入完整的 moov box */
static void mp4_write_moov(mp4_muxer_t *m)
{
    /* 计算各 box 大小（从内到外） */
    /* stbl 内: stsd + stts(24) + stsc(28) + stsz + stco */
    /* stsd 大小依赖 track_type。
     * H.264: stsd(16) + avc1(86) + avcC box(8 header + 11 fixed + sps + pps)
     * MJPEG: stsd(16) + mjpa(86) */
    int stsd_sz = (m->track_type == MP4_TRACK_H264)
                  ? (16 + 86 + 8 + 11 + m->sps_size + m->pps_size)
                  : (16 + 86);
    int stts_sz = 24;
    int stsc_sz = 28;
    int stsz_sz = 12 + 8 + m->sample_count * 4;
    int stco_sz = 8 + 8 + m->sample_count * 4;

    /* stbl = 8(header) + stsd + stts + stsc + stsz + stco */
    int stbl_sz = 8 + stsd_sz + stts_sz + stsc_sz + stsz_sz + stco_sz;
    /* minf = 8(header) + vmhd(20) + dinf(36) + stbl */
    int minf_sz = 8 + 20 + 36 + stbl_sz;
    /* mdia = 8(header) + mdhd(32) + hdlr(45) + minf */
    int mdia_sz = 8 + 32 + 45 + minf_sz;
    /* trak = 8(header) + tkhd(92) + mdia */
    int trak_sz = 8 + 92 + mdia_sz;

    /* movie duration（用 mvhd 的 timescale=1000 即毫秒） */
    uint32_t movie_timescale = 1000;
    uint32_t movie_duration = (uint32_t)((uint64_t)m->sample_count * 1000 / (m->fps > 0 ? m->fps : 30));
    /* media timescale = fps，duration = frame_count */
    uint32_t media_timescale = (uint32_t)(m->fps > 0 ? m->fps : 30);
    uint32_t media_duration = (uint32_t)m->sample_count;

    /* moov = 8(header) + mvhd(108) + trak */
    int moov_sz = 8 + 108 + trak_sz;

    /* 写入 moov header */
    uint8_t moov_hdr[8];
    put_be32(moov_hdr, moov_sz);
    memcpy(moov_hdr + 4, "moov", 4);
    fwrite(moov_hdr, 1, 8, m->fp);

    /* mvhd */
    write_mvhd(m, movie_timescale, movie_duration);

    /* trak header */
    uint8_t trak_hdr[8];
    put_be32(trak_hdr, trak_sz);
    memcpy(trak_hdr + 4, "trak", 4);
    fwrite(trak_hdr, 1, 8, m->fp);

    /* tkhd */
    write_tkhd(m, movie_duration, movie_timescale);

    /* mdia header */
    uint8_t mdia_hdr[8];
    put_be32(mdia_hdr, mdia_sz);
    memcpy(mdia_hdr + 4, "mdia", 4);
    fwrite(mdia_hdr, 1, 8, m->fp);

    /* mdhd */
    write_mdhd(m, media_timescale, media_duration);

    /* hdlr */
    write_hdlr(m);

    /* minf header */
    uint8_t minf_hdr[8];
    put_be32(minf_hdr, minf_sz);
    memcpy(minf_hdr + 4, "minf", 4);
    fwrite(minf_hdr, 1, 8, m->fp);

    /* vmhd */
    write_vmhd(m);

    /* dinf */
    write_dinf(m);

    /* stbl header */
    uint8_t stbl_hdr[8];
    put_be32(stbl_hdr, stbl_sz);
    memcpy(stbl_hdr + 4, "stbl", 4);
    fwrite(stbl_hdr, 1, 8, m->fp);

    /* stsd, stts, stsc, stsz, stco */
    write_stsd(m);
    write_stts(m);
    write_stsc(m);
    write_stsz(m);
    write_stco(m);
}

/* ------------------------------------------------------------------ */
/* 公共 API 实现                                                       */
/* ------------------------------------------------------------------ */

mp4_muxer_t * mp4_muxer_create(const char *path, int width, int height,
                                int fps, mp4_track_type_t track_type)
{
    if (!path || width <= 0 || height <= 0 || fps <= 0) {
        LV_LOG_ERROR("MP4: 配置无效（path=%p, %dx%d@%d）", path, width, height, fps);
        return NULL;
    }

    mp4_muxer_t *m = (mp4_muxer_t *)calloc(1, sizeof(struct mp4_muxer));
    if (!m) return NULL;

    m->width = width;
    m->height = height;
    m->fps = fps;
    m->track_type = track_type;
    snprintf(m->output_path, sizeof(m->output_path), "%s", path);

    /* 分配帧索引数组 */
    m->sample_cap = MP4_SAMPLE_INIT_CAP;
    m->sample_sizes = (uint32_t *)malloc(m->sample_cap * sizeof(uint32_t));
    m->sample_offsets = (uint32_t *)malloc(m->sample_cap * sizeof(uint32_t));
    if (!m->sample_sizes || !m->sample_offsets) {
        free(m->sample_sizes);
        free(m->sample_offsets);
        free(m);
        return NULL;
    }

    /* H.264 需要 AVCC 转换缓冲区 */
    if (track_type == MP4_TRACK_H264) {
        m->avcc_buf_sz = AVCC_BUF_INIT_SZ;
        m->avcc_buf = (uint8_t *)malloc(m->avcc_buf_sz);
        if (!m->avcc_buf) {
            free(m->sample_sizes);
            free(m->sample_offsets);
            free(m);
            return NULL;
        }
    }

    /* 创建父目录 */
    create_parent_dir(path);

    /* 打开输出文件 */
    m->fp = fopen(path, "wb");
    if (!m->fp) {
        LV_LOG_ERROR("MP4: 无法创建文件: %s", path);
        free(m->avcc_buf);
        free(m->sample_sizes);
        free(m->sample_offsets);
        free(m);
        return NULL;
    }

    /* 写入 ftyp + mdat header */
    mp4_write_ftyp(m);
    mp4_write_mdat_header(m);

    LV_LOG_USER("MP4: muxer 已创建（%dx%d@%d, %s）→ %s",
                width, height, fps,
                track_type == MP4_TRACK_H264 ? "H.264" : "MJPEG",
                path);
    return m;
}

/* 确保帧索引容量足够 */
static int ensure_sample_cap(mp4_muxer_t *m)
{
    if (m->sample_count < m->sample_cap) return 0;
    int new_cap = m->sample_cap * 2;
    uint32_t *new_sz = realloc(m->sample_sizes, new_cap * sizeof(uint32_t));
    uint32_t *new_off = realloc(m->sample_offsets, new_cap * sizeof(uint32_t));
    if (!new_sz || !new_off) {
        if (new_sz) m->sample_sizes = new_sz;
        if (new_off) m->sample_offsets = new_off;
        return -1;
    }
    m->sample_sizes = new_sz;
    m->sample_offsets = new_off;
    m->sample_cap = new_cap;
    return 0;
}

int mp4_muxer_write_sample(mp4_muxer_t *m, const uint8_t *data, size_t size,
                            int is_keyframe)
{
    if (!m || !m->fp || !data || size == 0) return -1;

    const uint8_t *write_data = data;
    size_t write_size = size;

    /* H.264: Annex-B → AVCC 转换 */
    if (m->track_type == MP4_TRACK_H264) {
        int avcc_sz = h264_annexb_to_avcc(data, size, m);
        if (avcc_sz <= 0) {
            LV_LOG_WARN("MP4: H.264 AVCC 转换失败，跳过该帧");
            return -1;
        }
        write_data = m->avcc_buf;
        write_size = avcc_sz;
    }

    /* 确保索引容量 */
    if (ensure_sample_cap(m) != 0) {
        LV_LOG_WARN("MP4: 帧索引扩容失败，停止录制");
        return -1;
    }

    /* 记录帧偏移（相对文件起始）和大小 */
    long cur_pos = ftell(m->fp);
    m->sample_offsets[m->sample_count] = (uint32_t)cur_pos;
    m->sample_sizes[m->sample_count] = (uint32_t)write_size;
    m->sample_count++;

    /* 写入帧数据 */
    fwrite(write_data, 1, write_size, m->fp);

    (void)is_keyframe;  /* MJPEG 全关键帧；H.264 关键帧信息不写入 MP4（播放器自动判断） */
    return 0;
}

int mp4_muxer_close(mp4_muxer_t *m)
{
    if (!m || !m->fp) return -1;

    /* 回填 mdat box size */
    long file_end = ftell(m->fp);
    uint32_t mdat_sz = (uint32_t)(file_end - m->mdat_offset);
    fseek(m->fp, m->mdat_offset, SEEK_SET);
    uint8_t sz_buf[4];
    put_be32(sz_buf, mdat_sz);
    fwrite(sz_buf, 1, 4, m->fp);
    fseek(m->fp, file_end, SEEK_SET);
    fflush(m->fp);

    /* Faststart：将 moov 移到文件开头（ftyp 之后，mdat 之前）。
     * 布局变化：[ftyp|mdat] → [ftyp|moov|mdat]
     * 所有 stco 偏移需增加 moov_sz（mdat 数据整体后移）。
     * 兼容性：iOS QuickTime、部分 Windows 播放器要求 moov 在前才能播放。 */

    /* 步骤 1：计算 moov 大小（与 mp4_write_moov 内部计算一致） */
    int stsd_sz = (m->track_type == MP4_TRACK_H264)
                  ? (16 + 86 + 8 + 11 + m->sps_size + m->pps_size)
                  : (16 + 86);
    int stts_sz = 24;
    int stsc_sz = 28;
    int stsz_sz = 12 + 8 + m->sample_count * 4;
    int stco_sz = 8 + 8 + m->sample_count * 4;
    int stbl_sz = 8 + stsd_sz + stts_sz + stsc_sz + stsz_sz + stco_sz;
    int minf_sz = 8 + 20 + 36 + stbl_sz;
    int mdia_sz = 8 + 32 + 45 + minf_sz;
    int trak_sz = 8 + 92 + mdia_sz;
    int moov_sz = 8 + 108 + trak_sz;

    /* 步骤 2：调整 stco 偏移（mdat 整体后移 moov_sz 字节） */
    for (int i = 0; i < m->sample_count; i++) {
        m->sample_offsets[i] += (uint32_t)moov_sz;
    }

    /* 步骤 3：将 moov 写入临时缓冲。
     * 优先用 tmpfile()（内存临时文件，最快）；
     * 失败时（嵌入式设备 /tmp 可能空间不足或不可用）回退到
     * 输出文件同目录的 .moov.tmp 文件（与输出文件在同一文件系统，
     * rename 可原子替换）。两种方式都能保证 faststart 成功，
     * 不回退到尾部 moov（尾部 moov 会导致部分手机/Windows 播放器无法播放）。 */
    FILE *moov_tmp = tmpfile();
    int moov_tmp_is_file = 0;  /* 标记临时 moov 是否为磁盘文件（需 remove 清理） */
    char moov_tmp_path[600] = {0};
    if (!moov_tmp) {
        /* tmpfile 失败：用输出文件同目录的临时文件 */
        snprintf(moov_tmp_path, sizeof(moov_tmp_path), "%s.moov.tmp", m->output_path);
        moov_tmp = fopen(moov_tmp_path, "wb+");
        if (moov_tmp) {
            moov_tmp_is_file = 1;
            LV_LOG_USER("MP4: tmpfile 不可用，使用磁盘临时文件 %s", moov_tmp_path);
        }
    }
    if (!moov_tmp) {
        /* 两种方式都失败（极罕见）：最后回退到尾部 moov */
        LV_LOG_WARN("MP4: 所有临时文件方案均失败，回退到尾部 moov");
        mp4_write_moov(m);
        fflush(m->fp);
        fclose(m->fp);
        m->fp = NULL;
        LV_LOG_USER("MP4: 文件已关闭（共 %d 帧，tail moov fallback）", m->sample_count);
        return 0;
    }
    FILE *orig_fp = m->fp;
    m->fp = moov_tmp;
    mp4_write_moov(m);
    fflush(moov_tmp);
    long moov_written_sz = ftell(moov_tmp);
    m->fp = orig_fp;

    if (moov_written_sz != moov_sz) {
        LV_LOG_WARN("MP4: moov 大小不匹配（计算=%d, 实际=%ld），回退到尾部 moov",
                    moov_sz, moov_written_sz);
        /* 撤销偏移调整 */
        for (int i = 0; i < m->sample_count; i++) {
            m->sample_offsets[i] -= (uint32_t)moov_sz;
        }
        fclose(moov_tmp);
        if (moov_tmp_is_file) remove(moov_tmp_path);
        mp4_write_moov(m);
        fflush(m->fp);
        fclose(m->fp);
        m->fp = NULL;
        LV_LOG_USER("MP4: 文件已关闭（共 %d 帧，tail moov fallback）", m->sample_count);
        return 0;
    }

    /* 步骤 4：创建最终文件 [ftyp|moov|mdat] 到临时路径 */
    char tmp_path[600];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", m->output_path);
    FILE *final_fp = fopen(tmp_path, "wb");
    if (!final_fp) {
        LV_LOG_WARN("MP4: 无法创建临时文件 %s，回退到尾部 moov", tmp_path);
        /* 撤销偏移调整 */
        for (int i = 0; i < m->sample_count; i++) {
            m->sample_offsets[i] -= (uint32_t)moov_sz;
        }
        fclose(moov_tmp);
        if (moov_tmp_is_file) remove(moov_tmp_path);
        mp4_write_moov(m);
        fflush(m->fp);
        fclose(m->fp);
        m->fp = NULL;
        LV_LOG_USER("MP4: 文件已关闭（共 %d 帧，tail moov fallback）", m->sample_count);
        return 0;
    }

    /* 4a: 复制 ftyp（原始文件前 mdat_offset 字节 = ftyp 32 字节） */
    fseek(m->fp, 0, SEEK_SET);
    uint8_t ftyp_buf[32];
    if (fread(ftyp_buf, 1, 32, m->fp) != 32) {
        LV_LOG_WARN("MP4: ftyp 读取失败");
        fclose(final_fp);
        fclose(moov_tmp);
        if (moov_tmp_is_file) remove(moov_tmp_path);
        fclose(m->fp);
        m->fp = NULL;
        return -1;
    }
    fwrite(ftyp_buf, 1, 32, final_fp);

    /* 4b: 复制 moov（从临时内存文件） */
    fseek(moov_tmp, 0, SEEK_SET);
    uint8_t copy_buf[8192];
    size_t moov_remaining = (size_t)moov_written_sz;
    while (moov_remaining > 0) {
        size_t to_read = sizeof(copy_buf);
        if (to_read > moov_remaining) to_read = moov_remaining;
        size_t n = fread(copy_buf, 1, to_read, moov_tmp);
        if (n == 0) break;
        fwrite(copy_buf, 1, n, final_fp);
        moov_remaining -= n;
    }
    fclose(moov_tmp);
    if (moov_tmp_is_file) remove(moov_tmp_path);

    /* 4c: 复制 mdat（从原始文件 mdat_offset 到文件尾） */
    fseek(m->fp, m->mdat_offset, SEEK_SET);
    long mdat_total = file_end - m->mdat_offset;
    long mdat_copied = 0;
    while (mdat_copied < mdat_total) {
        size_t to_read = sizeof(copy_buf);
        if ((long)to_read > mdat_total - mdat_copied) to_read = (size_t)(mdat_total - mdat_copied);
        size_t n = fread(copy_buf, 1, to_read, m->fp);
        if (n == 0) break;
        fwrite(copy_buf, 1, n, final_fp);
        mdat_copied += (long)n;
    }

    fflush(final_fp);
    fclose(final_fp);
    fclose(m->fp);
    m->fp = NULL;

    /* 步骤 5：用临时文件替换原文件 */
    if (rename(tmp_path, m->output_path) != 0) {
        LV_LOG_ERROR("MP4: 重命名失败 %s → %s (%s)",
                     tmp_path, m->output_path, strerror(errno));
        /* 尝试用 remove + rename（跨文件系统回退） */
        remove(m->output_path);
        if (rename(tmp_path, m->output_path) != 0) {
            LV_LOG_ERROR("MP4: 重命名再次失败，临时文件保留在 %s", tmp_path);
            return -1;
        }
    }

    LV_LOG_USER("MP4: 文件已关闭（faststart，共 %d 帧，moov=%d 字节，mdat=%u 字节）",
                m->sample_count, moov_sz, mdat_sz);
    return 0;
}

void mp4_muxer_destroy(mp4_muxer_t *m)
{
    if (!m) return;
    if (m->fp) {
        /* 未正常 close，文件不完整（无 moov），直接关闭 */
        fclose(m->fp);
        m->fp = NULL;
    }
    if (m->sample_sizes) free(m->sample_sizes);
    if (m->sample_offsets) free(m->sample_offsets);
    if (m->avcc_buf) free(m->avcc_buf);
    free(m);
}
