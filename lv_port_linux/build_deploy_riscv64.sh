#!/bin/bash
set -euo pipefail
########################################################
# LVGL DRM+libevdev 一键编译部署脚本（K230 RISC-V64）
# 目标平台：Canaan K230 / Linux 6.6.36 riscv64 / Buildroot
# 功能：CMake交叉编译→依赖抽取→启动脚本生成→打包→可选adb推送
########################################################

# ====================== 用户可修改配置区 ======================
# K230 Linux SDK 根目录
K230_SDK_ROOT="/home/rk/Desktop/k230_linux/k230_linux_sdk"
# Buildroot defconfig 名称
BR_DEFCONFIG="k230_canmv_lckfb_defconfig"
# 可执行程序最终名称
APP_BIN_NAME="lvglsim"
# 本地最终输出文件夹（指定到app_k230）
TARGET_APP_DIR_NAME="app_k230"
# 交叉工具链前缀
TC_PREFIX="riscv64-unknown-linux-gnu"
# Buildroot sysroot前缀
SYSROOT_PREFIX="riscv64-buildroot-linux-gnu"
# 中文字体本地路径
LOCAL_FONT_PATH="/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf"
# ==========================================================

# ====================== 自动推导路径 ======================
ROOT_DIR=$(pwd)
BUILD_DIR="${ROOT_DIR}/build"
DEPLOY_DIR="${ROOT_DIR}/deploy"
FINAL_OUT_DIR="../app/${TARGET_APP_DIR_NAME}"

# SDK编译后产出路径
HOST_DIR="${K230_SDK_ROOT}/output/${BR_DEFCONFIG}/host"
TC_BIN_DIR="${HOST_DIR}/bin"
STAGING_DIR="${HOST_DIR}/${SYSROOT_PREFIX}/sysroot"
STRIP_TOOL="${TC_BIN_DIR}/${TC_PREFIX}-strip"

# ====================== 打印环境信息 ======================
echo -e "\033[32m=============================================\033[0m"
echo "        K230 RISC-V64 LVGL DRM 编译部署工具"
echo "SDK 根目录      : ${K230_SDK_ROOT}"
echo "Buildroot配置   : ${BR_DEFCONFIG}"
echo "交叉工具链目录  : ${TC_BIN_DIR}"
echo "Sysroot系统库   : ${STAGING_DIR}"
echo "目标可执行文件 : ${APP_BIN_NAME}"
echo "输出部署文件夹 : ${FINAL_OUT_DIR}"
echo -e "\033[32m=============================================\033[0m"

# ====================== 前置依赖校验 ======================
check_tool() {
    local bin_path=$1
    local tip_msg=$2
    if [[ ! -x "${bin_path}" ]]; then
        echo -e "\033[31m❌ 错误：未找到 ${bin_path}\033[0m"
        echo "提示：${tip_msg}"
        exit 1
    fi
}

# 校验交叉编译器
check_tool "${TC_BIN_DIR}/${TC_PREFIX}-gcc" "请先进入SDK目录执行 make CONF=${BR_DEFCONFIG} 完整编译Buildroot"
# 校验strip工具
check_tool "${STRIP_TOOL}" "Buildroot编译完成后才会生成strip工具"
# 校验sysroot库目录
if [[ ! -d "${STAGING_DIR}/usr/lib/pkgconfig" ]]; then
    echo -e "\033[31m❌ sysroot缺少pkgconfig库文件，请完整编译SDK并勾选 libdrm libevdev wpa_supplicant\033[0m"
    exit 1
fi

# 加入工具链到PATH
export PATH="${TC_BIN_DIR}:$PATH"
export STAGING_DIR

# ====================== 1. 清理操作【彻底修复参数未定义】 ======================
CLEAN_FLAG=0
EXIT_AFTER_CLEAN=0
# 先判断是否传入了参数，再读取$1
if [[ $# -ge 1 ]]; then
    ARG1="$1"
    if [[ "${ARG1}" == "clean" ]]; then
        CLEAN_FLAG=1
        # 判断是否有第二个参数
        if [[ $# -ge 2 && "$2" == "exit" ]]; then
            EXIT_AFTER_CLEAN=1
        fi
    fi
fi

if [[ ${CLEAN_FLAG} -eq 1 ]]; then
    echo -e "\033[34m[1/6] 清理历史编译产物与部署包\033[0m"
    rm -rf "${BUILD_DIR}" "${DEPLOY_DIR}" "${FINAL_OUT_DIR}"
    echo "清理完成"
    if [[ ${EXIT_AFTER_CLEAN} -eq 1 ]]; then
        exit 0
    fi
fi

mkdir -p "${BUILD_DIR}" "${DEPLOY_DIR}"

# ====================== 2. CMake 交叉编译 ======================
echo -e "\033[34m[2/6] CMake 交叉编译 Release 版本\033[0m"
# pkgconfig 交叉编译定向sysroot
export PKG_CONFIG_LIBDIR="${STAGING_DIR}/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="${STAGING_DIR}"

pushd "${BUILD_DIR}" >/dev/null
# 存在缓存则跳过CMake配置，直接make
if [[ ! -f CMakeCache.txt ]]; then
    cmake \
        -DCMAKE_TOOLCHAIN_FILE="${ROOT_DIR}/toolchain_k230.cmake" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${BUILD_DIR}/install" \
        ..
fi
make -j$(nproc)
popd >/dev/null

# 对可执行文件瘦身strip
echo "执行strip体积压缩：${APP_BIN_NAME}"
"${STRIP_TOOL}" "${BUILD_DIR}/bin/${APP_BIN_NAME}"

# ====================== 3. 抽取依赖库 & 资源文件 ======================
echo -e "\033[34m[3/6] 拷贝可执行文件、依赖动态库、字体资源\033[0m"
rm -rf "${DEPLOY_DIR:?}"/*

# 主程序拷贝
cp "${BUILD_DIR}/bin/${APP_BIN_NAME}" "${DEPLOY_DIR}/"
chmod +x "${DEPLOY_DIR}/${APP_BIN_NAME}"

# DRM、evdev 输入库完整拷贝（带软链接）
cp -d "${STAGING_DIR}/usr/lib/libdrm.so"* "${DEPLOY_DIR}/"
cp -d "${STAGING_DIR}/usr/lib/libevdev.so"* "${DEPLOY_DIR}/" 2>/dev/null || true

# wpa_client WiFi库（可选）
if [[ -f "${STAGING_DIR}/usr/lib/libwpa_client.so" ]]; then
    cp -d "${STAGING_DIR}/usr/lib/libwpa_client.so" "${DEPLOY_DIR}/"
fi

# 中文字体文件
if [[ -f "${LOCAL_FONT_PATH}" ]]; then
    cp "${LOCAL_FONT_PATH}" "${DEPLOY_DIR}/DroidSansFallbackFull.ttf"
else
    echo -e "\033[33m⚠️ 本地未找到Droid中文字体，后续需手动上传ttf字体文件\033[0m"
fi

# FFmpeg 运行库（视频缩略图与播放，可选；不存在时跳过）
for ff_lib in libavformat libavcodec libavutil libswscale; do
    for f in "${STAGING_DIR}/usr/lib/${ff_lib}.so"*; do
        [ -e "$f" ] && cp -d "$f" "${DEPLOY_DIR}/" 2>/dev/null || true
    done
done

# libjpeg（相册大图降采样缓存与视频缩略图，可选）
for f in "${STAGING_DIR}/usr/lib/libjpeg.so"*; do
    [ -e "$f" ] && cp -d "$f" "${DEPLOY_DIR}/" 2>/dev/null || true
done

# ====================== 4. 生成板端启动脚本（修复引号语法） ======================
echo -e "\033[34m[4/6] 生成 start.sh 正式启动脚本 & run.sh兜底脚本\033[0m"

# 主启动脚本 start.sh
cat > "${DEPLOY_DIR}/start.sh" << 'RAW_SCRIPT'
#!/bin/sh
# 注意：不使用 set -e。下面大量步骤是“环境准备/优化”，失败不应导致启动退出。
# 引号严格成对，杜绝脚本解析报错
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "${DIR}" || exit 1

echo "======================================"
echo "   K230 CANAAN LVGL DRM APP START"
echo "   Arch: riscv64  Kernel: Linux 6.6.36"
echo "   DRM Render + libevdev Touch"
echo "======================================"

# 依赖库拷贝到系统库目录（可选；当前目录已有 so，LD_LIBRARY_PATH 已包含 .）
cp libdrm.so.2 /usr/lib/ 2>/dev/null || true
cp libevdev.so.2 /usr/lib/ 2>/dev/null || true
[ -f libwpa_client.so ] && (cp libwpa_client.so /usr/lib/ 2>/dev/null || true)
# FFmpeg 视频解码库（可选，用于视频缩略图与播放）
for ff_lib in libavformat.so* libavcodec.so* libavutil.so* libswscale.so*; do
    [ -f "$ff_lib" ] && (cp "$ff_lib" /usr/lib/ 2>/dev/null || true)
done
# libjpeg（相册大图降采样与视频缩略图）
for jpeg_lib in libjpeg.so*; do
    [ -f "$jpeg_lib" ] && (cp "$jpeg_lib" /usr/lib/ 2>/dev/null || true)
done

# DRM显卡权限
chmod 666 /dev/dri/card0 2>/dev/null || true
# 全部触摸设备放开权限
for dev in /dev/input/event*; do
    [ -e "$dev" ] && (chmod 666 "$dev" 2>/dev/null || true)
done

# 放开摄像头设备权限（V4L2 自动扫描 /dev/video0~9）
for dev in /dev/video*; do
    [ -e "$dev" ] && (chmod 666 "$dev" 2>/dev/null || true)
done

# 性能优化：设置 CPU 调速器为 performance 模式，C908 双核全速运行。
# 默认 ondemand 调速器会动态降频，导致 UI 帧率抖动。
for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    [ -e "$cpu" ] && (echo performance > "$cpu" 2>/dev/null || true)
done

# WiFi自动重连，清理wpa_supplicant冲突锁文件
if [ -f /etc/wpa_supplicant.conf ]; then
    mkdir -p /var/run/wpa_supplicant || true
    killall -9 wpa_supplicant 2>/dev/null || true
    rm -rf /var/run/wpa_supplicant/wlan0 || true
    wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf -D nl80211 2>/dev/null || true
    sleep 1
    udhcpc -i wlan0 2>/dev/null &
fi

# LVGL DRM环境变量
export LV_LINUX_DRM_CARD="/dev/dri/card0"
# 优先加载当前目录so库
export LD_LIBRARY_PATH="${DIR}:${LD_LIBRARY_PATH:-}"
# 显式指定应用部署目录，供 C 层定位 device.json / 配置文件
export APP_DIR="${DIR}"

echo "APP_DIR=${DIR}"
echo "Starting ./APP_BIN_REPLACE ..."

# 启动程序
# 性能优化：nice -n -10 提升进程优先级，避免后台任务抢占 UI 帧时间
exec nice -n -10 ./APP_BIN_REPLACE
RAW_SCRIPT

# 替换程序名占位符
sed -i "s/APP_BIN_REPLACE/${APP_BIN_NAME}/g" "${DEPLOY_DIR}/start.sh"

# 极简兜底启动脚本 run.sh
cat > "${DEPLOY_DIR}/run.sh" << EOF
#!/bin/sh
export LD_LIBRARY_PATH=./:\$LD_LIBRARY_PATH
export LV_LINUX_DRM_CARD=/dev/dri/card0
chmod 666 /dev/dri/card0
./${APP_BIN_NAME}
EOF

# 去除Windows CRLF换行，避免板端语法异常
sed -i 's/\r//g' "${DEPLOY_DIR}/start.sh" "${DEPLOY_DIR}/run.sh"
chmod +x "${DEPLOY_DIR}/start.sh" "${DEPLOY_DIR}/run.sh"

# 编译版本记录文件
cat > "${DEPLOY_DIR}/BUILD_INFO.txt" << EOF
arch=riscv64 (Canaan K230)
kernel=Linux 6.6.36
render_backend=DRM
touch_driver=libevdev
wifi_dep=libwpa_client.so
toolchain=${TC_PREFIX}-gcc
defconfig=${BR_DEFCONFIG}
sdk_root=${K230_SDK_ROOT}
build_time=$(date '+%Y-%m-%d %H:%M:%S')
strip=enabled
EOF

# ====================== 5. 打包最终输出目录 ======================
echo -e "\033[34m[5/6] 整合最终部署包到 ${FINAL_OUT_DIR}\033[0m"
rm -rf "${FINAL_OUT_DIR}"
mkdir -p "${FINAL_OUT_DIR}"
cp -r "${DEPLOY_DIR}"/* "${FINAL_OUT_DIR}/"

# ====================== 固件版本号自动递增 ======================
# 每次编译 build 号 +1，附加到固件版本号后（如 V1.0.0+build42）
# .build_number 文件持久化存储当前 build 号，跨编译累积递增
BUILD_NUMBER_FILE="${ROOT_DIR}/.build_number"
if [ -f "${BUILD_NUMBER_FILE}" ]; then
    BUILD_NUM=$(cat "${BUILD_NUMBER_FILE}" | tr -d '[:space:]')
else
    BUILD_NUM=0
fi
BUILD_NUM=$((BUILD_NUM + 1))
echo "${BUILD_NUM}" > "${BUILD_NUMBER_FILE}"
echo -e "\033[33m固件构建号: ${BUILD_NUM}（版本号将附加 +build${BUILD_NUM}）\033[0m"

# 生成 device.json：从项目 inf.json 读取设备信息并合并摄像头能力声明
# inf.json 是项目信息源（lv_port_linux/inf.json），修改设备信息只需改 inf.json 后重新编译
# 即使 inf.json 文件缺失或 python3 不可用，也使用内联的 inf.json 默认值生成 device.json，
# 保证部署到机器后能正常识别设备信息（不会出现空白）。
INF_JSON="${ROOT_DIR}/inf.json"
if [ ! -f "${INF_JSON}" ]; then
    echo "⚠️  项目信息源 inf.json 不存在: ${INF_JSON}"
    echo "    将使用脚本内联的默认信息值生成 device.json"
    INF_JSON=""
fi

# 用 python3 解析 inf.json 并生成 device.json（合并摄像头能力声明）
# python3 不可用时回退到内联默认值（与 inf.json 保持一致）
if command -v python3 >/dev/null 2>&1; then
    python3 - "${INF_JSON}" "${FINAL_OUT_DIR}/device.json" "${BUILD_NUM}" << 'PYEOF'
import json, sys, os, re

inf_path, out_path, build_num = sys.argv[1], sys.argv[2], sys.argv[3]

# inf.json 标准键名默认值（与 app3/inf.json 保持一致）：
# 当 inf.json 文件缺失或字段为空时使用，保证部署到机器后能正确识别设备信息
info = {
    "serial_number": "",
    "manufacturer": "Sankuai WanJi",
    "device_model": "Dion 5 Pro",
    "model_id": "dc001",
    "firmware_version": "V1.0.0",
    "camera_firmware_version": "V1.0.0",
    "cmiit_id": "",
    "fcc_id": "",
    "ic_can": "",
    "ices_id": "",
}

# 旧版 inf.json 键名映射（兼容历史格式，大小写/空格不敏感）
legacy_map = {
    "manufacturer": ["Manufacturer"],
    "device_model": ["Model"],
    "model_id": ["Model ID", "Model_ID"],
    "cmiit_id": ["CMIIT ID", "CMIIT_ID"],
    "fcc_id": ["FCC ID", "FCC_ID"],
    "ic_can": ["CAN ID", "CAN_ID", "IC ID", "IC_ID"],
    "ices_id": ["ICES ID", "ICES_ID"],
    "serial_number": ["SN", "Serial Number", "Serial_Number"],
    "camera_firmware_version": ["Camera FW Ver", "Camera_FW_Ver", "Camera Firmware Version"],
}

def get_val(data, keys):
    """按顺序尝试多个键名，返回第一个非空字符串值"""
    for k in keys:
        v = data.get(k)
        if isinstance(v, str) and v:
            return v
    return ""

def split_model(model):
    """从 'Dion 5 Pro, DC001' 中拆分出型号名与 model_id"""
    m = re.search(r'[,，]\s*([A-Za-z0-9_-]+)\s*$', model)
    if m:
        return model[:m.start()].strip(), m.group(1).lower()
    return model.strip(), ""

data = {}
if inf_path and os.path.isfile(inf_path):
    with open(inf_path, "r", encoding="utf-8") as f:
        data = json.load(f)

# 读取标准键和旧版键（标准键优先）
for k in info:
    val = get_val(data, [k])
    if not val and k in legacy_map:
        val = get_val(data, legacy_map[k])
    if val:
        info[k] = val

# 特殊处理旧版 Model 字段：可能同时包含 device_model 和 model_id
if "Model" in data and isinstance(data["Model"], str) and data["Model"]:
    model_name, model_id = split_model(data["Model"])
    if not info["device_model"]:
        info["device_model"] = model_name
    if not info["model_id"]:
        info["model_id"] = model_id

# 固件版本附加 build 号：去掉已有的 +build 后缀（避免重复），再附加当前 build 号
fw_base = info["firmware_version"].split("+")[0]
info["firmware_version"] = f"{fw_base}+build{build_num}"

# 组装 device.json：inf.json 字段 + 摄像头能力声明
device = {
    "_comment": "设备信息配置：由 build 脚本从 inf.json 合并生成。修改设备信息请改 inf.json 后重新编译",
    "serial_number": info["serial_number"],
    "manufacturer": info["manufacturer"],
    "device_model": info["device_model"],
    "model_id": info["model_id"],
    "cmiit_id": info["cmiit_id"],
    "fcc_id": info["fcc_id"],
    "ic_can": info["ic_can"],
    "ices_id": info["ices_id"],
    "firmware_version": info["firmware_version"],
    "camera_model": "GC2093",
    "camera_firmware_version": info["camera_firmware_version"],
    "camera_capabilities": {
        "_comment": "摄像头能力声明：决定录制/预览上限。更换 4K 摄像头时修改此段即可",
        "max_photo_width": 1920,
        "max_photo_height": 1080,
        "max_video_width": 1920,
        "max_video_height": 1080,
        "max_video_fps": 30,
        "supported_video_resolutions": [
            {"label": "1080P/16:9", "width": 1920, "height": 1080, "fps": [24, 25, 30]},
            {"label": "720P/16:9",  "width": 1280, "height": 720,  "fps": [30, 60]}
        ]
    }
}

with open(out_path, "w", encoding="utf-8") as f:
    json.dump(device, f, ensure_ascii=False, indent=4)
print(f"device.json 已生成: 厂商={info['manufacturer']} 型号={info['device_model']} model_id={info['model_id']} 固件={info['firmware_version']}")
PYEOF
else
    echo "⚠️  python3 不可用，使用内联默认值生成 device.json（值与 inf.json 保持一致）"
    # 内联默认值必须与 app3/inf.json 字段保持一致，否则机器上设备信息会显示错误
    # firmware_version 使用 sed 替换写入实际 build 号
    cat > "${FINAL_OUT_DIR}/device.json" << 'EOF'
{
    "_comment": "设备信息配置：由 build 脚本内联默认值生成（python3 不可用）。修改设备信息请改 inf.json 后重新编译",
    "serial_number": "",
    "manufacturer": "Sankuai WanJi",
    "device_model": "Dion 5 Pro",
    "model_id": "dc001",
    "cmiit_id": "",
    "fcc_id": "",
    "ic_can": "",
    "ices_id": "",
    "firmware_version": "V1.0.0+BUILD_NUM_PLACEHOLDER",
    "camera_model": "GC2093",
    "camera_firmware_version": "V1.0.0",
    "camera_capabilities": {
        "_comment": "摄像头能力声明：决定录制/预览上限。更换 4K 摄像头时修改此段即可",
        "max_photo_width": 1920,
        "max_photo_height": 1080,
        "max_video_width": 1920,
        "max_video_height": 1080,
        "max_video_fps": 30,
        "supported_video_resolutions": [
            {"label": "1080P/16:9", "width": 1920, "height": 1080, "fps": [24, 25, 30]},
            {"label": "720P/16:9",  "width": 1280, "height": 720,  "fps": [30, 60]}
        ]
    }
}
EOF
    # 替换 build 号占位符
    sed -i "s/BUILD_NUM_PLACEHOLDER/build${BUILD_NUM}/g" "${FINAL_OUT_DIR}/device.json"
fi

# 将固件版本写入 BUILD_INFO.txt（追加 build 号信息）
echo "firmware_version=V1.0.0+build${BUILD_NUM}" >> "${FINAL_OUT_DIR}/BUILD_INFO.txt"
echo "build_number=${BUILD_NUM}" >> "${FINAL_OUT_DIR}/BUILD_INFO.txt"

# ====================== 6. ADB推送函数，同样先判断参数 ======================
adb_push_to_board() {
    echo -e "\033[34m[6/6] 执行ADB推送至K230板子 /root/\033[0m"
    adb shell rm -rf /root/${TARGET_APP_DIR_NAME}
    adb push "${FINAL_OUT_DIR}" /root/
    echo -e "\033[32m✅ 推送完成！板端执行命令：\033[0m"
    echo "cd /root/${TARGET_APP_DIR_NAME} && ./start.sh"
}

# 判断是否传入push参数再推送
if [[ $# -ge 1 && "$1" == "push" ]]; then
    adb_push_to_board
fi

# ====================== 结束提示 ======================
echo -e "\033[32m=============================================\033[0m"
echo "✅ 全部流程执行完毕！"
echo "本地部署包路径：${FINAL_OUT_DIR}"
ls -lh "${FINAL_OUT_DIR}"
echo "---------------------------------------------"
echo "使用命令规范："
echo "1. 仅清理不编译：  ./$(basename $0) clean exit"
echo "2. 清理后重新编译：./$(basename $0) clean"
echo "3. 正常直接编译：  ./$(basename $0)"
echo "4. 编译+ADB上板： ./$(basename $0) push"
echo "板端运行指令：cd /root/${TARGET_APP_DIR_NAME} && ./start.sh"
echo -e "\033[32m=============================================\033[0m"