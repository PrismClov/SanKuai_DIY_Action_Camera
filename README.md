# K230 运动相机 LVGL 应用

基于 [LVGL 9.4](https://github.com/lvgl/lvgl) 的嵌入式 Linux 运动相机风格 GUI 应用，运行于 **Canaan K230**（riscv64，CanMV / Linux 6.6.36），支持摄像头预览、拍照录像、相册回放、WiFi 连接等完整功能，并可在 PC（SDL2）上模拟运行，方便无硬件开发调试。

## 功能特性

- **摄像头预览**
  - V4L2 MMAP 零拷贝采集，4 缓冲区队列，1080p@30fps
  - 帧格式自动协商：RGB565 > NV12/NV21 > YUYV
  - 双路径自动回退：K230 VPU/G2D 硬件加速（dlopen libmpi.so）→ 软件转换（含 180° 倒装修正 / 90° 旋转，合并到转换流程无二次遍历）
  - 预览后台线程渲染，YUV→RGB565 转换不阻塞 UI 线程
- **拍照**
  - libjpeg / libjpeg-turbo（SIMD 加速）JPEG 编码，不可用时回退 BMP
  - 画幅（16:9 / 4:3 / 1:1）、图片大小、连拍可选
- **录像**
  - 可插拔录制抽象层（`recorder_hal`），MJPEG 软件后端 + 自研 MP4 容器写入器（ftyp/mdat/moov）
  - 接口预置 4K@60fps / H.265 能力，后续接入 K230 VPU 硬件编码后，仅需修改 `device.json` 即可启用
- **相册**
  - 照片网格 + 缩略图降采样缓存（避免高分辨率照片 OOM）
  - 视频缩略图与全屏播放（FFmpeg 可选，未检测到时自动编译空桩）
  - 长按二级菜单、横竖屏自适应布局
- **拍摄模式**：拍照 / 视频 / 超级夜景 / 人物跟随 / 慢动作 / 静止延时（间隔拍照），上滑快捷参数页随模式切换显示
- **手势导航**：tileview 布局，上滑参数页 / 下滑功能菜单 / 左滑存储页 / 右滑相册
- **息屏显示（AOD）** 与亮度调节
- **WiFi**：基于 wpa_supplicant 的扫描 / 连接 / 密码键盘（`wpa_ctrl` 封装）
- **设置中心**：设备信息、存储管理、中英文切换（i18n）、认证信息、网络、亮度、AOD
- **横竖屏旋转**：DRM FULL 模式 + flush 阶段旋转，横屏 800x480 / 竖屏 480x800 自适应
- **平台自动识别**：`board_detect` 通过 `/proc/device-tree/compatible` + `uname` 识别硬件平台，一套二进制多板兼容
- **设备信息**：`device.json` 由 `inf.json` 编译时自动生成，固件版本 build 号自动递增

## 支持平台

| 平台 | 架构 | 显示后端 | 输入后端 | 硬件加速 |
|------|------|----------|----------|----------|
| Canaan K230（CanMV） | riscv64 | DRM | EVDEV 触摸 | VPU/G2D（dlopen libmpi.so） |
| PC（开发调试） | x86_64 | SDL2 | SDL 鼠标 | 无（软件路径） |

## 环境依赖

- **通用**：CMake ≥ 3.10、GCC、pkg-config
- **PC 模拟**：`libsdl2-dev`、`libsdl2-image-dev`、`libjpeg-dev`（可选 FFmpeg 开发库）
- **K230 交叉编译**：K230 Linux SDK 交叉工具链（`riscv64-unknown-linux-gnu-gcc`）+ 目标 sysroot（libdrm / libevdev / libwpa_client）

## 快速开始（PC 模拟，无硬件可跑）

```bash
# 1. 安装依赖（仅一次）
sudo apt-get install -y libsdl2-dev libsdl2-image-dev

# 2. 一键编译 + 打包
cd lv_port_linux
./build_pc.sh

# 3. 运行（默认 480x800 竖屏，可指定横屏 800x480）
cd ../app/app1
./run.sh            # 480x800
./run.sh 800 480    # 横屏
```

PC 端鼠标拖动可模拟触摸滑动，点击按钮触发交互。

## 交叉编译与部署（K230）

```bash
cd lv_port_linux
# K230_SDK_ROOT 等按需修改脚本顶部的配置区
./build_deploy_riscv64.sh
```

产物输出到 `../app/app_k230/`。

```bash
# 推送到板子（IP 按实际替换）
scp -r ../app/app_k230/* root@<板子IP>:/app/app_k230/

# 板上以 root 运行
cd /app/app_k230
./start.sh          # 自动处理依赖库拷贝、设备权限、性能调优、wpa_supplicant
```

> 详细的手动部署步骤、权限配置与常见问题见 [lv_port_linux/BUILD_RUN.md](lv_port_linux/BUILD_RUN.md)。

## 工程结构

```
app3/
├── inf.json                 # 设备信息源（编译时合并生成 device.json）
├── lv_port_linux/           # 主工程
│   ├── lv_conf.h            # 平台配置（DRM + EVDEV）
│   ├── lv_conf_pc.h         # PC 配置（SDL2）
│   ├── toolchain_k230.cmake # riscv64 交叉编译工具链
│   ├── build_pc.sh          # PC 一键编译打包
│   ├── build_deploy_riscv64.sh # K230 一键编译打包
│   ├── CMakeLists.txt       # 顶层构建配置
│   ├── lvgl/                # LVGL 9.4.0 源码（含 tiny_ttf 等增强）
│   └── src/
│       ├── main.c           # 程序入口（后端选择、参数解析）
│       ├── lib/             # 显示/输入后端（drm/sdl/fbdev/wayland/x11/glfw3/evdev）
│       └── app/
│           ├── app_main.c   # 主 UI：tileview + 状态栏 + 页面组合
│           ├── common/      # ui_helpers / app_i18n / device_config / board_detect / cjson
│           │                # album_image_cache / video_thumb / video_player / app_stress_test
│           ├── camera/      # camera_hal + camera_v4l2 + camera_preview
│           │                # k230_accel / photo_saver
│           │                # recorder_hal + recorder_mjpeg + mp4_muxer
│           ├── pages/       # page_home / page_quick / page_menu / page_album / page_aod / page_storage
│           ├── settings/    # 设置主页 / 设备 / 存储 / 语言 / 认证 / 网络 / 亮度 / AOD
│           └── wifi/        # app_wifi（扫描/连接）+ wpa_ctrl_wrap 封装
└── app/app_k230/            # K230 部署包（构建产物）
```

### K230 部署包结构（`app/app_k230/`）

```
app/app_k230/
├── lvglsim                    # 主程序（riscv64 ELF，strip 后）
├── device.json                # 设备信息 + 摄像头能力声明
├── BUILD_INFO.txt             # 构建信息（arch/toolchain/defconfig/固件版本）
├── start.sh                   # 一键启动（拷贝依赖库/权限/性能调优/WiFi）
├── run.sh                     # 备用运行脚本
├── DroidSansFallbackFull.ttf  # 中文字体
├── libdrm.so.2                # DRM 显示后端依赖
├── libevdev.so.2              # 触摸输入依赖
├── libwpa_client.so           # WiFi 控制依赖
├── libjpeg.so.9               # JPEG 编解码（拍照/缩略图）
├── libavcodec.so.58           # ┐
├── libavformat.so.58          # ├ FFmpeg 视频解码（视频缩略图/播放）
├── libavutil.so.56            # │
└── libswscale.so.5            # ┘
```

K230 构建关键参数：架构 `riscv64` / 内核 `Linux 6.6.36`（canaan），工具链 `riscv64-unknown-linux-gnu-gcc`，编译优化 `-march=rv64gc -mtune=c908 -ftree-vectorize -O3`（C908 + RVV），K230 VPU/G2D 硬件加速通过 `dlopen libmpi.so` 加载、失败自动回退软件路径。`start.sh` 启动流程：拷贝依赖库 → 放开 `/dev/dri/card0`、`/dev/input/event*`、`/dev/video*` 权限 → CPU 调速器切 performance → 拉起 wpa_supplicant → `nice -n -10 ./lvglsim`。

## 架构设计

```
┌─────────────────────────────────────────────────┐
│                    LVGL UI 层                    │
│  pages(首页/参数/菜单/相册/AOD)  settings  wifi  │
├─────────────────────────────────────────────────┤
│                 应用服务层                        │
│  camera_hal  recorder_hal  photo_saver          │
│  album_cache  video_player  device_config       │
│  app_i18n(中英)  board_detect                   │
├───────────────────┬─────────────────────────────┤
│   加速层（自动回退） │  显示/输入后端               │
│  K230 VPU/G2D      │  DRM / SDL / FBDEV ...     │
│  软件转换 ← 兜底    │  EVDEV / SDL 鼠标           │
├───────────────────┴─────────────────────────────┤
│              Linux 内核 / 驱动                   │
│  V4L2(GC2093)  DRM  EVDEV  wpa_supplicant       │
└─────────────────────────────────────────────────┘
```

**核心设计思想：**

- **HAL 抽象 + 运行时回退**：摄像头、录制、图像加速均通过 HAL 层分发，按平台（`board_detect` 识别）自动选择最优实现，硬件后端不可用即静默回退到软件路径，保证一份二进制在不同平台正常运转。
- **零拷贝优先**：预览采用 V4L2 MMAP 采集；YUV→RGB565 转换、缩放、旋转在 VPU/G2D 上一次完成，无硬件时软件路径合并旋转避免二次遍历。
- **录制解耦**：录制与预览独立，`recorder_feed_frame` 非阻塞（<5ms），防止编码阻塞预览帧率；后端通过 magic 字段分发，预留 4K60 能力。
- **UI 响应性**：重计算（图像转换/编码/缩略图）全部下沉到后台线程或 worker，LVGL 主循环保持流畅。

## 配置说明

| 文件 | 作用 |
|------|------|
| `inf.json` | 设备信息源（厂商/型号/固件版本），编译时合并生成 `device.json` |
| `device.json` | 运行时设备信息 + `camera_capabilities` 能力声明（决定预览/录制上限） |
| `lv_conf.h` / `lv_conf_pc.h` | LVGL 平台配置（DRM/EVDEV vs SDL） |
| `CMakeLists.txt` | 平台架构优化（riscv64 RVV）、可选组件开关 |

**CMake 可选开关：**

| 选项 | 默认 | 说明 |
|------|------|------|
| `APP_ENABLE_WIFI` | ON | WiFi 扫描/连接（需 libwpa_client，PC 端置 OFF） |
| `APP_ALBUM_LOAD_THUMBNAILS` | ON | 相册加载真实图片缩略图（高内存，可关闭防 OOM） |
| `APP_ENABLE_FFMPEG` | ON | 视频缩略图与全屏播放（检测到 FFmpeg 开发库时启用） |

## 常见问题

- 中文字体缺失 / `cache not allocated`：见 [BUILD_RUN.md](lv_port_linux/BUILD_RUN.md) 第六节（双层 fallback 机制说明）
- 链接 `libdrm` / 库版本错误：见 [BUILD_RUN.md](lv_port_linux/BUILD_RUN.md) 第六节（sysroot / 工具链版本说明）

## 开源许可

本项目源码以 **GNU GPL v3.0** 发布，详见 [LICENSE](LICENSE)。

- 依赖组件：LVGL（MIT）、lv_port_linux 后端（MIT）、libjpeg-turbo（BSD）、FFmpeg（LGPL，可选）、wpa_supplicant 控制接口（BSD）
- 内置字体 DroidSansFallbackFull.ttf 版权归 Google/其作者所有，Apache 2.0 许可，仅供演示使用

## 致谢

- [LVGL](https://github.com/lvgl/lvgl) — 开源嵌入式图形库
- [lv_port_linux](https://github.com/lvgl/lv_port_linux) — Linux 平台显示/输入后端参考实现
- Canaan（嘉楠科技）— K230 硬件平台与 SDK

