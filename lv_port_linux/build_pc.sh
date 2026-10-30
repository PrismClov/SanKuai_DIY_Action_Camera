#!/bin/bash
set -e

########################################################
# PC (Ubuntu) 一键编译 + 打包脚本
# 后端：SDL2 窗口   输入：SDL 鼠标
# WiFi 关闭（PC 无 wpa_client）
# 打包目录：app/app1（本地测试运行）
########################################################

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
cd "${SCRIPT_DIR}"

ROOT_DIR="${SCRIPT_DIR}"
BUILD_DIR="${ROOT_DIR}/build_pc"
TARGET_APP_DIR="${ROOT_DIR}/../app/app1"

# 中文字体
FONT_SRC="/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf"

echo "============================================="
echo "  PC (Ubuntu) 编译打包"
echo "  工程目录: ${ROOT_DIR}"
echo "  后端:     SDL2"
echo "  打包目录: ${TARGET_APP_DIR}"
echo "============================================="

# 检查 SDL2
if ! pkg-config --exists sdl2; then
    echo "❌ 未安装 SDL2，请先执行:"
    echo "   sudo apt-get install -y libsdl2-dev libsdl2-image-dev"
    exit 1
fi
if ! pkg-config --exists SDL2_image; then
    echo "❌ 未安装 SDL2_image，请先执行:"
    echo "   sudo apt-get install -y libsdl2-dev libsdl2-image-dev"
    exit 1
fi

# 1. 检查是否需要清理（仅当传递 clean 参数时）
if [ "$1" = "clean" ]; then
    echo "[1/4] 清理旧编译目录（clean 模式）..."
    rm -rf ${BUILD_DIR}
fi

mkdir -p ${BUILD_DIR}

cd ${BUILD_DIR}

# 仅在 build 目录不存在 CMakeCache.txt 时执行 cmake（支持增量编译）
if [ ! -f CMakeCache.txt ]; then
    cmake -DLV_BUILD_CONF_PATH=${ROOT_DIR}/lv_conf_pc.h -DAPP_ENABLE_WIFI=OFF ..
fi

make -j$(nproc)
cd ${ROOT_DIR}

# 2. 检查产物
if [ ! -f "${BUILD_DIR}/bin/lvglsim" ]; then
    echo "❌ 编译失败：未生成 lvglsim"
    exit 1
fi

# 3. 打包到 app/app1（清空旧内容）
echo "[2/4] 打包到 ${TARGET_APP_DIR}..."
rm -rf ${TARGET_APP_DIR}
mkdir -p ${TARGET_APP_DIR}

cp ${BUILD_DIR}/bin/lvglsim ${TARGET_APP_DIR}/
cp ${FONT_SRC} ${TARGET_APP_DIR}/

# 生成 device.json：PC 模拟器也需要设备信息，与交叉编译脚本保持一致
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
echo "固件构建号: ${BUILD_NUM}（版本号将附加 +build${BUILD_NUM}）"

INF_JSON="${ROOT_DIR}/inf.json"
if [ ! -f "${INF_JSON}" ]; then
    echo "⚠️  项目信息源 inf.json 不存在: ${INF_JSON}"
    echo "    将使用脚本内联的默认信息值生成 device.json"
    INF_JSON=""
fi

if command -v python3 >/dev/null 2>&1; then
    python3 - "${INF_JSON}" "${TARGET_APP_DIR}/device.json" "${BUILD_NUM}" << 'PYEOF'
import json, sys, os, re

inf_path, out_path, build_num = sys.argv[1], sys.argv[2], sys.argv[3]

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
    for k in keys:
        v = data.get(k)
        if isinstance(v, str) and v:
            return v
    return ""

def split_model(model):
    m = re.search(r'[,，]\s*([A-Za-z0-9_-]+)\s*$', model)
    if m:
        return model[:m.start()].strip(), m.group(1).lower()
    return model.strip(), ""

data = {}
if inf_path and os.path.isfile(inf_path):
    with open(inf_path, "r", encoding="utf-8") as f:
        data = json.load(f)

for k in info:
    val = get_val(data, [k])
    if not val and k in legacy_map:
        val = get_val(data, legacy_map[k])
    if val:
        info[k] = val

if "Model" in data and isinstance(data["Model"], str) and data["Model"]:
    model_name, model_id = split_model(data["Model"])
    if not info["device_model"]:
        info["device_model"] = model_name
    if not info["model_id"]:
        info["model_id"] = model_id

# 固件版本附加 build 号：去掉已有的 +build 后缀（避免重复），再附加当前 build 号
fw_base = info["firmware_version"].split("+")[0]
info["firmware_version"] = f"{fw_base}+build{build_num}"

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
    echo "⚠️  python3 不可用，使用内联默认值生成 device.json"
    cat > ${TARGET_APP_DIR}/device.json << 'EOF'
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
    sed -i "s/BUILD_NUM_PLACEHOLDER/build${BUILD_NUM}/g" ${TARGET_APP_DIR}/device.json
fi

# 架构标识
cat > ${TARGET_APP_DIR}/BUILD_INFO.txt << EOF
arch=$(uname -m) (PC Ubuntu)
backend=SDL2
wifi=disabled
firmware_version=V1.0.0+build${BUILD_NUM}
build_number=${BUILD_NUM}
build_time=$(date '+%Y-%m-%d %H:%M:%S')
EOF

# 4. 生成运行脚本
echo "[3/4] 生成 run.sh..."
cat > ${TARGET_APP_DIR}/run.sh << 'EOF'
#!/bin/sh
DIR=$(cd "$(dirname "$0")" && pwd)
cd "$DIR"

# 默认 480x800 (RK3566 真实分辨率)，可通过参数覆盖
W=${1:-480}
H=${2:-800}

echo "======================================"
echo "  PC LVGL SDL 测试  ${W}x${H}"
echo "  (RK3566 真实分辨率: 480x800)"
echo "======================================"

./lvglsim -b SDL -W ${W} -H ${H}
EOF
chmod +x ${TARGET_APP_DIR}/run.sh

echo "[4/4] 完成"
echo "============================================="
echo "✅ PC 打包完成！"
echo "运行: cd ${TARGET_APP_DIR} && ./run.sh"
echo "     自定义分辨率: ./run.sh 800 480  (横屏)"
echo "============================================="
