# TODO - 设备配置 / 摄像头预览 / K230 交叉编译

> 任务：读取本地 JSON 配置、主页面摄像头预览、K230 riscv64 交叉编译版本
> 日期：2026-08-01
> 状态：代码实现完成，待实际板端验证

---

## 一、已完成事项

| 模块 | 内容 | 关键文件 |
|------|------|----------|
| JSON 配置读取 | 精简 cJSON 解析器 + device_config 模块，11 项字段从 device.json 读取 | `src/app/common/cjson.c` `device_config.c` |
| 板子自动识别 | board_detect 模块，通过 device-tree + uname 识别 RK3566/K230，首次检测打印日志 | `src/app/common/board_detect.c` |
| 摄像头 HAL | V4L2 后端，MMAP 零拷贝采集，YUV→RGB565 软件转换 | `src/app/camera/camera_v4l2.c` |
| 主页摄像头预览 | page_home 集成摄像头预览，无摄像头时黑色背景，连续 30 次取帧失败自动停止 | `src/app/pages/page_home.c` |
| 设备信息页扩展 | settings_device 显示 12 项字段，全部从 JSON 读取，缺失字段灰色提示 | `src/app/settings/settings_device.c` |
| 启动预加载 | app_ui_create 中预加载 board_detect + device_config，避免设置页首次打开卡顿 | `src/app/app_main.c` |
| K230 编译脚本 | build_k230.sh，针对 canaan 6.6.36 riscv64，生成 device.json 示例 | `build_k230.sh` |
| RK3566 编译脚本 | 补充 device.json 生成 + 摄像头权限放开 + 字段顺序对齐 | `build_rk3566.sh` |
| CMake 构建 | 添加 4 个新源文件到 APP_SOURCES | `CMakeLists.txt` |
| device.json 规范 | 字段顺序与设置页显示顺序一致，含 `_comment` 说明字段 | `build_k230.sh` `build_rk3566.sh` |

---

## 二、待办事宜（需用户支持）

### 1. ⚠️ K230 SDK 路径确认

**问题**：`build_deploy_riscv64.sh` 中 K230 SDK 路径为硬编码默认值，需确认与实际环境一致。

**当前配置**：
```bash
K230_SDK_ROOT="/home/rk/Desktop/k230_linux/k230_linux_sdk"
BR_DEFCONFIG="k230_canmv_lckfb_defconfig"
```

**操作指引**：
- 确认 K230 SDK 已完整编译（`make CONF=k230_canmv_lckfb_defconfig`）
- 如路径不同，编辑 `build_deploy_riscv64.sh` 顶部的 `K230_SDK_ROOT` 变量
- 确认 sysroot 中存在 `libdrm.so` / `libevdev.so` / `libwpa_client.so`

### 2. ⚠️ device.json 实际设备信息填写

**问题**：已统一从 `lv_port_linux/inf.json` 生成 `device.json`，但需确认板上实际部署的文件包含正确信息。

**字段清单**：
```json
{
    "serial_number": "SDC00125",
    "manufacturer": "Sankuai WanJi",
    "device_model": "Dion 5 Pro",
    "model_id": "dc001",
    "cmiit_id": "abc123456",
    "fcc_id": "abc123456",
    "ic_can": "abc123456",
    "ices_id": "abc123456",
    "firmware_version": "V1.0.0",
    "camera_model": "GC2093",
    "camera_firmware_version": "1.5"
}
```

**操作指引**：
- 修改 `lv_port_linux/inf.json` 后，重新编译即可自动更新部署目录的 `device.json`
- 如需在板上临时验证，可直接编辑 `/root/app_k230/device.json`（K230）或 `/app/app1/device.json`（RK3566）
- 缺失字段会在设置页显示为「未配置」（灰色），不影响 app 运行

### 3. ⚠️ 实际编译验证（Windows 环境无法执行）

**问题**：当前开发环境为 Windows，无法运行 `build_deploy_*.sh` 验证编译。

**操作指引**：
- 切换到 Ubuntu 编译机，进入 `app/app3/lv_port_linux`
- 执行 RK3566 编译部署：`./build_deploy_arm64.sh`
- 执行 K230 编译部署：`./build_deploy_riscv64.sh`
- PC 模拟器测试：`./build_pc.sh`
- 启动后在串口/ADB 查看日志，确认出现 `device.json 已加载: ...` 且关键字段非空

### 4. ⚠️ 摄像头硬件验证

**问题**：摄像头预览代码已实现，但未在实际板子 + 摄像头模组上验证。

**需验证项**：
- [ ] RK3566 + USB 摄像头：`/dev/video0` 是否存在，权限是否放开
- [ ] K230 + MIPI 摄像头：V4L2 设备节点是否正常
- [ ] 格式协商：RGB565 / NV12 / YUYV 三种格式是否至少有一种被摄像头支持
- [ ] 帧率：30fps 定时器是否流畅，有无卡顿
- [ ] 无摄像头场景：主页是否保持黑色背景（不崩溃）

**操作指引**：
- 板上执行 `ls /dev/video*` 确认摄像头节点
- 执行 `v4l2-ctl --device=/dev/video0 --list-formats-ext` 查看支持的格式
- 启动 app 后查看日志：`摄像头已打开: 1280x720 NV12 @30fps`

---

## 三、缺少的配置 / 依赖

| 项 | 说明 | 解决方式 |
|----|------|----------|
| K230 sysroot | 需确认 `libdrm` / `libevdev` / `libwpa_client` 已在 K230 SDK 中编译 | `make CONF=k230_canmv_lckfb_defconfig` 勾选对应包 |
| RK3566 自定义 sysroot | `sysroot_rk3566` / `gcc_runtime_rk3566` 需已通过 `setup_sysroot.sh` 准备 | 执行 `./setup_sysroot.sh` |
| 中文字体 | `DroidSansFallbackFull.ttf` 需存在于编译机 `/usr/share/fonts/truetype/droid/` | `sudo apt install fonts-droid-fallback` |
| 摄像头权限 | 板上 `/dev/video*` 默认仅 root 可访问 | `start.sh` 已自动 `chmod 666` |

---

## 四、后续优化方向（非阻塞）

1. **RK3566 硬件加速**：当前 YUV→RGB565 为软件转换，可接入 `librga` 实现 RGA 硬件缩放转换，降低 CPU 占用
2. **K230 VPU 加速**：K230 可调用 MPI VPU 进行硬件解码 + 缩放
3. **4K 支持**：当前协商 1280x720，后续可提升至 3840x2160（需确认摄像头模组支持 + DRM 后端 4K 输出能力）
4. **摄像头热插拔**：当前无摄像头时不再重试，后续可监听 uevent 动态启动/停止预览
5. **device.json 热更新**：当前单次加载缓存，后续可加文件监听支持运行时修改
