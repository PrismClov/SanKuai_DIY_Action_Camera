# LVGL WiFi App 构建与运行说明

RK3566 嵌入式 LVGL 应用，支持 WiFi 扫描连接、中文显示、运动相机风格交互 UI。

---

## 工程结构

```
lv_port_linux/
├── lv_conf.h              # RK3566 配置（DRM + EVDEV）
├── lv_conf_pc.h           # PC 配置（SDL2）
├── toolchain.cmake        # aarch64 交叉编译工具链
├── build_deploy.sh        # RK3566 一键编译打包脚本
├── build_pc.sh            # PC 一键编译脚本
├── CMakeLists.txt         # 顶层构建配置
└── src/
    ├── main.c             # 程序入口
    ├── wifi/
    │   └── wpa_ctrl_wrap.c/h   # wpa_supplicant 控制接口封装
    └── app/
        ├── app_main.c/h         # 主入口：tileview + 状态栏 + 页面组合
        ├── common/
        │   └── ui_helpers.c/h   # 主题色、字体加载、公共控件（卡片/标题栏/信息行）
        ├── pages/
        │   ├── page_home.c/h    # 主屏幕（参考图4）
        │   ├── page_quick.c/h   # 上滑拍摄参数页（参考图3）
        │   ├── page_menu.c/h    # 下滑功能选择页（参考图5）
        │   └── page_storage.c/h # 左滑存储查看页（参考图1）
        ├── settings/
        │   ├── settings_main.c/h     # 设置主页（卡片式入口）
        │   ├── settings_device.c/h    # 设备信息
        │   ├── settings_storage.c/h   # 存储管理
        │   ├── settings_language.c/h  # 语言设置
        │   └── settings_auth.c/h      # 认证信息
        └── wifi/
            └── app_wifi.c/h       # WiFi 扫描/连接/密码键盘 overlay
```

### 交互布局（tileview 坐标）

```
        ┌─────────────┐
        │  quick(0,0) │  上滑 → 拍摄参数页
        ├─────────────┤
        │  home(0,1)  │  主屏幕（默认显示）
        ├─────────────┬─────────────┐
        │  menu(0,2)  │ storage(1,2)│  下滑→功能菜单  左滑→存储页
        └─────────────┴─────────────┘
```

- 设置 / WiFi 以全屏 overlay 弹出
- 设置入口：功能菜单页 →「设置」卡片 → 系统设置主页 → 网络/存储/设备/语言/认证

---

## 一、RK3566 交叉编译

### 1.1 一键脚本

```bash
cd /home/rk/Desktop/tspi_rk3566/app/app3/lv_port_linux
./build_deploy.sh
```

产物输出到 `../app/app1/`，包含 `lvglsim`、依赖库、字体、`start.sh`。

### 1.2 手动编译

```bash
cd /home/rk/Desktop/tspi_rk3566/app/app3/lv_port_linux

# 清理旧构建
rm -rf build && mkdir build && cd build

# 设置 buildroot sysroot（libdrm / libevdev 头文件与库来源）
export PKG_CONFIG_LIBDIR=/home/rk/Desktop/tspi_rk3566/Release/buildroot/output/rockchip_rk3566/host/aarch64-buildroot-linux-gnu/sysroot/usr/lib/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=/home/rk/Desktop/tspi_rk3566/Release/buildroot/output/rockchip_rk3566/host/aarch64-buildroot-linux-gnu/sysroot

# CMake 配置 + 编译
cmake -DCMAKE_TOOLCHAIN_FILE=../toolchain.cmake ..
make -j$(nproc)

# 产物：build/bin/lvglsim
```

### 1.3 关键路径

| 项目 | 路径 |
|------|------|
| 工具链 | `Release/prebuilts/gcc/linux-x86/aarch64/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu/bin/` |
| aarch64 sysroot | `Release/buildroot/output/rockchip_rk3566/host/aarch64-buildroot-linux-gnu/sysroot` |
| libwpa_client.so | sysroot `usr/lib/` |
| 中文字体 | `/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf` |

---

## 二、RK3566 部署运行

### 2.1 推送到开发板

```bash
# 从开发机推送到板子（示例，IP/路径按实际替换）
scp -r /home/rk/Desktop/tspi_rk3566/app/app3/app1/* root@<板子IP>:/app/app1/
```

### 2.2 板上运行

```bash
cd /app/app1

# 方式一：用启动脚本（自动处理依赖库、权限、wpa_supplicant）
./start.sh

# 方式二：手动逐步执行
cp libevdev.so.2 /usr/lib/
cp libdrm.so.2 /usr/lib/
cp libwpa_client.so /usr/lib/

chmod 666 /dev/dri/card0
chmod 666 /dev/input/event2

ip link set wlan0 up 2>/dev/null || ifconfig wlan0 up 2>/dev/null

if [ ! -S /var/run/wpa_supplicant/wlan0 ]; then
    mkdir -p /var/run/wpa_supplicant
    /usr/sbin/wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf -D nl80211 -C /var/run/wpa_supplicant
fi

export LV_LINUX_DRM_CARD=/dev/dri/card0
export LV_LINUX_EVDEV_POINTER_DEVICE=/dev/input/event2

./lvglsim
```

> **注意**：须以 root 运行（wpa_ctrl 控制套接字需 root 权限）。

### 2.3 部署包文件清单

| 文件 | 说明 |
|------|------|
| `lvglsim` | 主程序（aarch64） |
| `libwpa_client.so` | WiFi 控制依赖 |
| `libevdev.so.2` | 触摸输入依赖 |
| `libdrm.so.2` | DRM 显示后端依赖 |
| `DroidSansFallbackFull.ttf` | 中文字体（3500+ 常用字） |
| `start.sh` | 启动脚本 |

---

## 三、PC 端 Ubuntu 编译运行（测试用）

### 3.1 安装依赖（仅需一次）

```bash
sudo apt-get install -y libsdl2-dev libsdl2-image-dev
```

### 3.2 一键脚本

```bash
cd /home/rk/Desktop/tspi_rk3566/app/app3/lv_port_linux
./build_pc.sh
```

### 3.3 手动编译

```bash
cd /home/rk/Desktop/tspi_rk3566/app/app3/lv_port_linux

rm -rf build_pc && mkdir build_pc && cd build_pc

# PC 专用配置（SDL2），关闭 WiFi（PC 无 wpa_client）
cmake -DLV_BUILD_CONF_PATH=../lv_conf_pc.h -DAPP_ENABLE_WIFI=OFF ..
make -j$(nproc)

# 复制字体
cp /usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf bin/
```

### 3.4 运行

```bash
cd /home/rk/Desktop/tspi_rk3566/app/app3/lv_port_linux/build_pc/bin

# SDL 窗口 800x480（默认）
./lvglsim -b SDL -W 800 -H 480

# 其他分辨率
./lvglsim -b SDL -W 1024 -H 600

# 查看支持的 backend
./lvglsim -B
```

> 鼠标拖动模拟触摸滑动，点击按钮触发交互。

---

## 四、配置对比

| 参数 | RK3566 | PC (Ubuntu) |
|------|--------|-------------|
| 配置文件 | `lv_conf.h` | `lv_conf_pc.h` |
| 显示后端 | DRM (`LV_USE_LINUX_DRM=1`) | SDL (`LV_USE_SDL=1`) |
| 输入后端 | EVDEV (`LV_USE_EVDEV=1`) | SDL 鼠标 |
| WiFi | `APP_ENABLE_WIFI=ON` | `APP_ENABLE_WIFI=OFF` |
| 编译器 | aarch64-linux-gnu-gcc 6.3.1 | 系统 gcc |
| CMake 参数 | `-DCMAKE_TOOLCHAIN_FILE=../toolchain.cmake` | `-DLV_BUILD_CONF_PATH=../lv_conf_pc.h` |
| 触摸光标 | 隐藏（ABS 设备） | SDL 鼠标光标 |

---

## 五、字体说明

### 中文字体

- 使用 `lv_tiny_ttf_create_file` 加载 `DroidSansFallbackFull.ttf`（覆盖 3500+ 常用汉字）
- 字号 20px
- 加载路径优先级：
  1. `./DroidSansFallbackFull.ttf`（工作目录）
  2. `/app/app1/DroidSansFallbackFull.ttf`（RK3566 部署路径）
  3. `/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf`（系统字体）
- 加载失败回退到内置 `lv_font_source_han_sans_sc_16_cjk`

### 符号字体

- 使用内置 `lv_font_montserrat_20`
- 用于 LVGL 符号字符（`LV_SYMBOL_WIFI`、`LV_SYMBOL_SETTINGS` 等）

### Fallback 机制（双重保障）

CJK 字体（tiny_ttf 动态创建时）设置 `fallback` 字段指向 montserrat 符号字体：

```c
/* 仅 tiny_ttf 动态字体可写；内置常量字体位于只读段，写入会段错误 */
if (s_cjk_is_ttf) {
    ((lv_font_t *)s_font_cjk)->fallback = s_font_sym;
}
```

同时在 `lv_tiny_ttf.c` 的 `ttf_get_glyph_dsc_cb` 中预检字形：

```c
/* 字形不存在则静默返回 false，交由 LVGL 走 fallback */
if(stbtt_FindGlyphIndex(&dsc->info, (int)unicode_letter) == 0) {
    return false;
}
```

两层配合：CJK 字体不含 `LV_SYMBOL_*` 私用区字符时，预检直接返回 false（不触发 cache 创建失败），LVGL 自动回退到 montserrat 渲染符号，彻底消除 `cache not allocated` 错误日志。

---

## 六、常见问题

### Q: `cache not allocated lv_tiny_ttf.c:322`

**原因**：CJK 字体不含 LVGL 符号字符（Unicode 私用区），`stbtt_FindGlyphIndex` 返回 0，cache 创建回调返回 false，`lv_cache_acquire_or_create` 返回 NULL 触发错误日志。

**修复（双层）**：
1. `ui_helpers.c`：仅对 tiny_ttf 动态字体设置 `fallback` 到 montserrat（内置只读字体不可写，否则段错误）。
2. `lv_tiny_ttf.c`：`ttf_get_glyph_dsc_cb` 调 cache 前预检字形，不存在则静默返回 false，让 LVGL 走 fallback。

### Q: `skipping incompatible libdrm.so`

**原因**：链接了 x86 版本的 libdrm。

**修复**：设置 `PKG_CONFIG_SYSROOT_DIR` 指向 buildroot aarch64 sysroot。

### Q: `undefined reference to stat64@GLIBC_2.33`

**原因**：buildroot libdrm 基于 glibc 2.33，工具链自带 glibc 较低。

**修复**：`toolchain.cmake` 中设置 `CMAKE_SYSROOT` 统一库来源。

### Q: WiFi 列表点击索引错位

**原因**：排序使用局部数组，回调索引与共享数组不一致。

**修复**：直接对共享数组排序（见 `app_wifi.c`）。

### Q: 触摸光标不隐藏

**修复**：`evdev.c` 中仅对 REL（鼠标）设备显示光标，ABS（触摸屏）跳过。

---

## 七、本次完善记录（bug 修复）

逐项代码审查发现并修复以下 7 个问题，RK3566 与 PC 双端编译均无 warning/error：

| # | 严重度 | 文件 | 问题 | 修复 |
|---|--------|------|------|------|
| A | 严重(段错误) | `common/ui_helpers.c` | 对内置 const 字体（`lv_font_source_han_sans_sc_16_cjk`，位于只读段）强转写 `fallback` 字段，TTF 加载失败回退时触发段错误 | 用 `s_cjk_is_ttf` 标记区分，仅 tiny_ttf 动态字体（堆可写）才设置 fallback |
| B | 中 | `settings/settings_storage.c` | 切换内置/SD 卡 tab 时按钮高亮不更新（内置存储按钮始终黄色） | 记录两个按钮句柄，`refresh_ui` 中按选中态切换背景色 |
| C | 中 | `pages/page_storage.c` | 顶部日期时间静态不刷新（创建时读取一次） | 新增 `g_dt_label` 与每秒定时器 `time_cb` 实时刷新 |
| D | 中 | `pages/page_storage.c` | 格式化按钮无事件回调，点击无反应 | 绑定 `on_format_cb`（记录日志，实际格式化需二次确认） |
| E | 中 | `settings/settings_main.c` | `settings_page_open` 缺 `move_foreground`，可能被其他 overlay 遮挡 | 打开时调用 `lv_obj_move_foreground` |
| F | 小(规范) | `app_main.h` / `pages/page_menu.c` | `page_menu.c` 散落 `extern` 全局变量声明，不规范 | 在 `app_main.h` 统一 `extern` 声明 tileview 句柄，页面 include 引用 |
| G | 小(警告) | `settings/settings_main.c` | PC 版 WiFi 关闭时 `on_network_card` 未使用，触发 `-Wunused-function` | 用 `#ifndef APP_WIFI_DISABLED` 条件编译包裹函数定义 |
| H | 严重(日志) | `lvgl/src/libs/tiny_ttf/lv_tiny_ttf.c` | CJK 字体不含 `LV_SYMBOL_*` 私用区字符时，cache 创建失败打印 `cache not allocated` error | `ttf_get_glyph_dsc_cb` 调 cache 前预检 `stbtt_FindGlyphIndex`，不存在则静默返回 false 走 fallback |

### 验证结果

- **RK3566 交叉编译**：`./build_rk3566.sh` → 无 warning/error，打包 `app/app1/`（3.0M lvglsim）
- **PC Ubuntu 编译**：`./build_pc.sh` → 无 warning/error，SDL 窗口运行启动正常
- **运行时日志**：PC 运行 5s 验证，`cache not allocated` error 已消除，仅剩 `CJK font loaded` 正常日志

