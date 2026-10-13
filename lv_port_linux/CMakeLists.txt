cmake_minimum_required(VERSION 3.10)
project(lvgl)

# Set policy to allow to run the target_link_libraries cmd on targets that are build
# in another directory.
# Currently, the linking is not handled by env_support/cmake/os.cmake
# This means that if a driver is enabled and it requires linking an external library
# it needs to be handled in the top-level project.

cmake_policy(SET CMP0079 NEW)

# 默认 Release 编译（开启 -O3 优化，性能提升 3~10 倍）。
# 调试时通过命令行 -DCMAKE_BUILD_TYPE=Debug 覆盖。
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_C_STANDARD 99) # LVGL officially supports C99 and above
set(CMAKE_CXX_STANDARD 17) #C17
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR}/bin)

# 平台架构优化：根据交叉编译目标启用 SIMD 加速
# RK3566 (aarch64): Cortex-A55 + NEON SIMD
# K230 (riscv64): C908 + RVV 1.0
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    # RK3566: Cortex-A55 优化 + NEON SIMD
    # 注意：aarch64 架构下 NEON 默认启用，不需要 -mfpu 参数
    # 不用 -ffast-math：buildroot 工具链缺 crtfastmath.o 会链接失败，
    # 且本项目以整数像素操作为主，浮点优化收益极小。
    set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -Wall -Wextra -mcpu=cortex-a55 -mtune=cortex-a55 -ftree-vectorize -O3")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -mcpu=cortex-a55 -mtune=cortex-a55 -ftree-vectorize -O3")
    message(STATUS "平台架构: aarch64 (RK3566 Cortex-A55 + NEON SIMD, -O3)")
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "riscv64")
    # K230: C908 优化 + RVV 1.0 向量扩展
    set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -Wall -Wextra -march=rv64gc -mtune=c908 -ftree-vectorize -O3")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -march=rv64gc -mtune=c908 -ftree-vectorize -O3")
    message(STATUS "平台架构: riscv64 (K230 C908 + RVV 1.0, -O3)")
else()
    # PC 编译：通用 -O3
    set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic -O3 -ftree-vectorize")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -O3 -ftree-vectorize")
    message(STATUS "平台架构: ${CMAKE_SYSTEM_PROCESSOR} (PC 编译, -O3)")
endif()

# Release 模式额外优化
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_C_FLAGS_RELEASE   "-O3 -DNDEBUG")
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
    message(STATUS "编译模式: Release (-O3 -DNDEBUG)")
endif()

# 需要 ASan 排查内存损坏时打开下面三行并重新 cmake：
# set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
# set(CMAKE_LINKER_FLAGS "${CMAKE_LINKER_FLAGS} -fsanitize=address")

set(LV_BUILD_SET_CONFIG_OPTS ON CACHE BOOL
    "create CMAKE variables from lv_conf_internal.h" FORCE)

# App option: enable WiFi (requires libwpa_client). Disable for pure PC testing.
option(APP_ENABLE_WIFI "Enable WiFi scanner/connector (needs wpa_client)" ON)

# App option: load real image thumbnails in album grid.
# 默认开启（用户期望看到真实照片预览）。OOM 排障时可关闭或设置环境变量 APP_ALBUM_THUMBNAILS=0。
# 尺寸上限 ALBUM_MAX_IMAGE_PIXELS 会自动过滤过大图片防 OOM。
option(APP_ALBUM_LOAD_THUMBNAILS "Load real image thumbnails in album grid (high memory use)" ON)

add_subdirectory(lvgl)

if (APP_ALBUM_LOAD_THUMBNAILS)
    add_compile_definitions(APP_ALBUM_LOAD_THUMBNAILS=1)
endif()

if (CONFIG_LV_USE_EVDEV)
    message("Including EVDEV support")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(EVDEV REQUIRED libevdev)

    list(APPEND PKG_CONFIG_LIB ${EVDEV_LIBRARIES})
    list(APPEND PKG_CONFIG_INC ${EVDEV_INCLUDE_DIRS})
    list(APPEND LV_LINUX_BACKEND_SRC src/lib/indev_backends/evdev.c)

endif()

if (CONFIG_LV_USE_LINUX_DRM)

    message("Including DRM support")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBDRM REQUIRED libdrm)

    list(APPEND PKG_CONFIG_LIB ${LIBDRM_LIBRARIES})
    list(APPEND PKG_CONFIG_INC ${LIBDRM_INCLUDE_DIRS})
    list(APPEND LV_LINUX_BACKEND_SRC src/lib/display_backends/drm.c)

endif()

if (CONFIG_LV_USE_LINUX_DRM_GBM_BUFFERS OR CONFIG_LV_LINUX_DRM_USE_EGL)

    message("Including GBM support")

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBGBM REQUIRED gbm)

    list(APPEND PKG_CONFIG_LIB ${LIBGBM_LIBRARIES})
    list(APPEND PKG_CONFIG_INC ${LIBGBM_INCLUDE_DIRS})

endif()

if (CONFIG_LV_USE_LIBINPUT)
    message("Including libinput support")

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBINPUT REQUIRED libinput)

    list(APPEND PKG_CONFIG_LIB ${LIBINPUT_LIBRARIES})
    list(APPEND PKG_CONFIG_INC ${LIBINPUT_INCLUDE_DIRS})

endif()

if (TRUE)
    message("Including Freetype support")

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBFREETYPE REQUIRED freetype2)

    list(APPEND PKG_CONFIG_LIB ${LIBFREETYPE_LIBRARIES})
    list(APPEND PKG_CONFIG_INC ${LIBFREETYPE_INCLUDE_DIRS})

endif()

if (CONFIG_LV_USE_SDL OR CONFIG_LV_USE_DRAW_SDL)

    message("Including SDL2 support")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
    pkg_check_modules(SDL2_IMAGE REQUIRED SDL2_image)

    list(APPEND PKG_CONFIG_LIB ${SDL2_LIBRARIES} ${SDL2_IMAGE_LIBRARIES})
    list(APPEND PKG_CONFIG_INC ${SDL2_INCLUDE_DIRS} ${SDL2_IMAGE_INCLUDE_DIRS})

    list(APPEND LV_LINUX_BACKEND_SRC src/lib/display_backends/sdl.c)
endif()


if (CONFIG_LV_USE_WAYLAND)

    message("Including Wayland support")

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
    pkg_check_modules(WAYLAND_CURSOR REQUIRED wayland-cursor)
    pkg_check_modules(XKBCOMMON REQUIRED xkbcommon)

    list(APPEND PKG_CONFIG_LIB ${WAYLAND_CLIENT_LIBRARIES})
    list(APPEND PKG_CONFIG_LIB ${WAYLAND_CURSOR_LIBRARIES})
    list(APPEND PKG_CONFIG_LIB ${XKBCOMMON_LIBRARIES})

    # Wayland protocols
    pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols>=1.25)
    pkg_get_variable(WAYLAND_PROTOCOLS_BASE wayland-protocols pkgdatadir)

    if(DEFINED ENV{SDKTARGETSYSROOT} AND DEFINED ENV{SYSROOT})
        message(FATAL_ERROR "Both SDKTARGETSYSROOT and SYSROOT are set. Please set only one.")
    endif()
    
    if(DEFINED ENV{SDKTARGETSYSROOT})
        set(PROTOCOL_ROOT "$ENV{SDKTARGETSYSROOT}/usr/share/wayland-protocols")
    elseif(DEFINED ENV{SYSROOT})
        set(PROTOCOL_ROOT "$ENV{SYSROOT}/usr/share/wayland-protocols")
    else()
        set(PROTOCOL_ROOT "/usr/share/wayland-protocols")
    endif()
    
    if(NOT EXISTS ${PROTOCOL_ROOT})
        message(FATAL_ERROR "Wayland protocols not found at ${PROTOCOL_ROOT}")
    endif()
    
    set(PROTOCOLS_DIR "${CMAKE_CURRENT_BINARY_DIR}/protocols")
    file(MAKE_DIRECTORY ${PROTOCOLS_DIR})
    
    set(WAYLAND_PROTOCOLS_SRC "")
    
    # Generate xdg-shell protocol (always)
    set(XDG_SHELL_XML "${PROTOCOL_ROOT}/stable/xdg-shell/xdg-shell.xml")
    set(XDG_SHELL_HEADER "${PROTOCOLS_DIR}/wayland_xdg_shell.h")
    set(XDG_SHELL_SOURCE "${PROTOCOLS_DIR}/wayland_xdg_shell.c")
    
    if(NOT EXISTS ${XDG_SHELL_HEADER} OR NOT EXISTS ${XDG_SHELL_SOURCE})
        execute_process(COMMAND wayland-scanner client-header ${XDG_SHELL_XML} ${XDG_SHELL_HEADER})
        execute_process(COMMAND wayland-scanner private-code ${XDG_SHELL_XML} ${XDG_SHELL_SOURCE})
    endif()
    list(APPEND WAYLAND_PROTOCOLS_SRC ${XDG_SHELL_SOURCE})
    
    # Generate dmabuf protocol (if config is set)
    if(CONFIG_LV_WAYLAND_USE_DMABUF)
        set(DMABUF_XML "${PROTOCOL_ROOT}/stable/linux-dmabuf/linux-dmabuf-v1.xml")
        set(DMABUF_HEADER "${PROTOCOLS_DIR}/wayland_linux_dmabuf.h")
        set(DMABUF_SOURCE "${PROTOCOLS_DIR}/wayland_linux_dmabuf.c")
        
        if(NOT EXISTS ${DMABUF_HEADER} OR NOT EXISTS ${DMABUF_SOURCE})
            execute_process(COMMAND wayland-scanner client-header ${DMABUF_XML} ${DMABUF_HEADER})
            execute_process(COMMAND wayland-scanner private-code ${DMABUF_XML} ${DMABUF_SOURCE})
        endif()
        list(APPEND WAYLAND_PROTOCOLS_SRC ${DMABUF_SOURCE})
    endif()
    
    list(APPEND PKG_CONFIG_INC ${PROTOCOLS_DIR})
    list(APPEND LV_LINUX_BACKEND_SRC src/lib/display_backends/wayland.c ${WAYLAND_PROTOCOLS_SRC})

endif()

if (CONFIG_LV_USE_X11)

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(X11 REQUIRED x11)

    message("Including X11 support")

    list(APPEND PKG_CONFIG_INC ${X11_INCLUDE_DIRS})
    list(APPEND PKG_CONFIG_LIB ${X11_LIBRARIES})
    list(APPEND LV_LINUX_BACKEND_SRC src/lib/display_backends/x11.c)

endif()

if (CONFIG_LV_USE_LINUX_FBDEV)

    # FBDEV has no dependencies
    message("Including FBDEV support")
    list(APPEND LV_LINUX_BACKEND_SRC src/lib/display_backends/fbdev.c)

endif()

if (CONFIG_LV_USE_GLFW)

    message("Including GLFW support")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GLFW3 REQUIRED glfw3)
    pkg_check_modules(GLEW REQUIRED glew)

    list(APPEND PKG_CONFIG_LIB ${GLFW3_LIBRARIES})
    list(APPEND PKG_CONFIG_LIB ${GLEW_LIBRARIES})
    list(APPEND LV_LINUX_BACKEND_SRC src/lib/display_backends/glfw3.c)

endif()

if (CONFIG_LV_USE_DRAW_G2D)
    message("Including G2D support")
    find_library(G2D_LIBRARY NAMES g2d)
    list(APPEND PKG_CONFIG_LIB ${G2D_LIBRARY})
endif()

if (CONFIG_LV_USE_GLTF)
    message(STATUS "Including with GLTF support")
    include(FetchContent)
  
    FetchContent_Declare(
      fastgltf
      GIT_REPOSITORY https://github.com/spnda/fastgltf
      GIT_TAG 4e2261350888bae7c35a1f39991f6233d57795f5)
  
    set(FASTGLTF_ENABLE_DEPRECATED_EXT
        ON
        CACHE BOOL "" FORCE)
  
    set(FASTGLTF_DIFFUSE_TRANSMISSION_SUPPORT
        ON
        CACHE BOOL "" FORCE)
  
    FetchContent_MakeAvailable(fastgltf)
  
    set(WEBP_BUILD_ANIM_UTILS
        OFF
        CACHE BOOL "" FORCE)
    set(WEBP_BUILD_CWEBP
        OFF
        CACHE BOOL "" FORCE)
    set(WEBP_BUILD_DWEBP
        OFF
        CACHE BOOL "" FORCE)
    set(WEBP_BUILD_GIF2WEBP
        OFF
        CACHE BOOL "" FORCE)
    set(WEBP_BUILD_IMG2WEBP
        OFF
        CACHE BOOL "" FORCE)
    set(WEBP_BUILD_VWEBP
        OFF
        CACHE BOOL "" FORCE)
    set(WEBP_BUILD_WEBPINFO
        OFF
        CACHE BOOL "" FORCE)
    set(WEBP_BUILD_WEBPMUX
        OFF
        CACHE BOOL "" FORCE)
    set(WEBP_BUILD_EXTRAS
        OFF
        CACHE BOOL "" FORCE)
  
    FetchContent_Declare(
      webp
      GIT_REPOSITORY https://github.com/webmproject/libwebp
      GIT_TAG fa6f56496a442eed59b103250021e4b14ebf1427)
  
    FetchContent_MakeAvailable(webp)
  
    list(APPEND PKG_CONFIG_LIB fastgltf webp)
endif()

if (CONFIG_LV_USE_GSTREAMER)

    message("Including GSTREAMER support")
    find_package(PkgConfig REQUIRED)

    pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0)
    pkg_check_modules(GSTREAMER_VIDEO REQUIRED gstreamer-video-1.0)
    pkg_check_modules(GSTREAMER_APP REQUIRED gstreamer-app-1.0)

    list(APPEND PKG_CONFIG_LIB ${GSTREAMER_VIDEO_LIBRARIES} ${GSTREAMER_APP_LIBRARIES})
    list(APPEND PKG_CONFIG_INC ${GSTREAMER_VIDEO_INCLUDE_DIRS} ${GSTREAMER_APP_INCLUDE_DIRS})
endif()


file(GLOB LV_LINUX_SRC src/lib/*.c)
set(LV_LINUX_INC src/lib)

target_include_directories(lvgl PUBLIC ${PKG_CONFIG_INC})

add_library(lvgl_linux STATIC ${LV_LINUX_SRC} ${LV_LINUX_BACKEND_SRC})
target_include_directories(lvgl_linux PUBLIC ${PKG_CONFIG_INC})

# If LVGL is configured to use LV_CONF_PATH or Kconfig
# Set the exactly the same definitions on the lvgl_linux target
set_target_properties(lvgl_linux PROPERTIES COMPILE_DEFINITIONS "${LVGL_COMPILER_DEFINES}")
target_include_directories(lvgl_linux PUBLIC
    ${LV_LINUX_INC} ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src/lib ${LVGL_CONF_INC_DIR})

# Link LVGL with external dependencies - Modern CMake/CMP0079 allows this
target_link_libraries(lvgl PUBLIC ${PKG_CONFIG_LIB} m pthread)

set(APP_SOURCES
    src/main.c
    src/app/app_main.c
    src/app/common/ui_helpers.c
    src/app/common/app_i18n.c
    src/app/common/app_stress_test.c
    # 设备配置：JSON 解析 + 板子自动识别 + device.json 加载
    src/app/common/cjson.c
    src/app/common/device_config.c
    src/app/common/board_detect.c
    # 摄像头 HAL：V4L2 后端（MMAP 零拷贝采集 + YUV→RGB565 转换）
    src/app/camera/camera_v4l2.c
    # 摄像头预览后台线程：将 YUV→RGB565 转换从 UI 线程移到 worker 线程
    # 3 缓冲区零拷贝方案，消除 K230 软件 YUV→RGB565 转换（8~12ms/帧）对 UI 的阻塞
    src/app/camera/camera_preview.c
    # RK3566 RGA 硬件加速：YUV→RGB565 + 缩放 + 旋转（一次完成，<2ms/帧）
    # K230 平台自动编译为空实现，运行时回退到软件路径
    src/app/camera/rga_accel.c
    # K230 VPU/G2D 硬件加速：YUV→RGB565 + 缩放 + 旋转 + H.264 编码（dlopen libmpi.so）
    # RK3566/PC 平台自动编译为空实现，运行时回退到 RGA 或软件路径
    src/app/camera/k230_accel.c
    # 录制抽象层：公共 API 分发层 + MJPEG 软件后端 + RK MPP 硬件后端 + MP4 容器写入器
    # recorder_hal.c：后端选择（RK3566→RKMPP H.264，其他→MJPEG），统一 MP4 容器
    # recorder_mjpeg.c：MJPEG 软件编码 + MP4（libjpeg，全平台兜底）
    # recorder_rkmpp.c：H.264 硬件编码 + MP4（dlopen librockchip_mpp，仅 aarch64）
    # mp4_muxer.c：MP4 容器写入（ftyp/mdat/moov，支持 MJPEG 和 H.264）
    src/app/camera/recorder_hal.c
    src/app/camera/recorder_mjpeg.c
    src/app/camera/recorder_rkmpp.c
    src/app/camera/mp4_muxer.c
    # 拍照保存：RGB565→JPEG 编码（libjpeg，不可用时回退 BMP）
    src/app/camera/photo_saver.c
    # 视频缩略图与全屏播放（FFmpeg 可选，未找到时编译空桩）
    src/app/common/video_thumb.c
    src/app/common/video_player.c
    # 相册大图降采样缓存（libjpeg，避免高分辨率照片 OOM）
    src/app/common/album_image_cache.c
    src/app/pages/page_home.c
    src/app/pages/page_quick.c
    src/app/pages/page_menu.c
    src/app/pages/page_album.c
    src/app/pages/page_aod.c
    src/app/settings/settings_main.c
    src/app/settings/settings_device.c
    src/app/settings/settings_storage.c
    src/app/settings/settings_language.c
    src/app/settings/settings_auth.c
    src/app/settings/settings_wireless.c
    src/app/settings/settings_brightness.c
    src/app/settings/settings_aod.c
)

if (APP_ENABLE_WIFI)
    list(APPEND APP_SOURCES src/wifi/wpa_ctrl_wrap.c src/app/wifi/app_wifi.c)
else()
    add_compile_definitions(APP_WIFI_DISABLED)
endif()

add_executable(lvglsim
    ${APP_SOURCES}
    ${LV_LINUX_SRC}
    ${LV_LINUX_BACKEND_SRC}
)

if (APP_ENABLE_WIFI)
    target_link_libraries(lvglsim lvgl_linux lvgl wpa_client)
else()
    target_link_libraries(lvglsim lvgl_linux lvgl)
endif()

# 链接 libjpeg（拍照 JPEG 编码、MJPEG 录制、视频缩略图）。
# 性能优化：优先查找 libjpeg-turbo（NEON/RVV SIMD 加速，编码速度 2~4 倍于原版 libjpeg）。
find_library(JPEG_LIB NAMES jpeg turbojpeg)
if(NOT JPEG_LIB)
    # PC 上常只安装运行时（libjpeg.so.8），无 -dev 提供的 libjpeg.so 软链接。
    # 用 glob 兜底查找版本化 so，保证能链接并继续编译。
    file(GLOB JPEG_CANDIDATES
        /usr/lib/${CMAKE_SYSTEM_PROCESSOR}-linux-gnu/libjpeg.so*
        /usr/lib/libjpeg.so*
        /usr/lib64/libjpeg.so*
        /lib/${CMAKE_SYSTEM_PROCESSOR}-linux-gnu/libjpeg.so*
        /lib/libjpeg.so*)
    list(LENGTH JPEG_CANDIDATES _jpeg_len)
    if(_jpeg_len GREATER 0)
        list(GET JPEG_CANDIDATES 0 JPEG_LIB)
    endif()
endif()
if(JPEG_LIB)
    target_link_libraries(lvglsim ${JPEG_LIB})
    # 优先使用系统 libjpeg 头文件；PC 编译若未安装 libjpeg-dev，
    # 从 aarch64 sysroot 借用头文件（API 兼容），避免添加整个 sysroot include 路径。
    find_path(JPEG_INCLUDE_DIR jpeglib.h)
    if(NOT JPEG_INCLUDE_DIR)
        set(JPEG_FALLBACK_INCLUDE "${CMAKE_BINARY_DIR}/jpeg_include")
        file(MAKE_DIRECTORY ${JPEG_FALLBACK_INCLUDE})
        foreach(_jpeg_h jpeglib.h jconfig.h jmorecfg.h jerror.h)
            if(NOT EXISTS "${JPEG_FALLBACK_INCLUDE}/${_jpeg_h}")
                file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/../sysroot_rk3566/usr/include/${_jpeg_h}"
                     DESTINATION ${JPEG_FALLBACK_INCLUDE})
            endif()
        endforeach()
        target_include_directories(lvglsim PRIVATE ${JPEG_FALLBACK_INCLUDE})
        message(STATUS "libjpeg header fallback: ${JPEG_FALLBACK_INCLUDE}")
    else()
        target_include_directories(lvglsim PRIVATE ${JPEG_INCLUDE_DIR})
    endif()
    message(STATUS "libjpeg found: ${JPEG_LIB}（若为 libjpeg-turbo 则启用 SIMD 加速）")
else()
    message(WARNING "libjpeg not found, photo_saver will fallback to BMP")
endif()

# 可选 FFmpeg：视频缩略图（video_thumb.c）与全屏播放（video_player.c）。
# 检测到 FFmpeg 开发库且 libjpeg 可用时才启用；否则编译空桩，相册对视频回退到图标占位。
option(APP_ENABLE_FFMPEG "Enable FFmpeg-based video thumbnail and playback" ON)
if(APP_ENABLE_FFMPEG)
    find_library(FFMPEG_AVFORMAT NAMES avformat)
    find_library(FFMPEG_AVCODEC NAMES avcodec)
    find_library(FFMPEG_AVUTIL  NAMES avutil)
    find_library(FFMPEG_SWSCALE NAMES swscale)
    if(FFMPEG_AVFORMAT AND FFMPEG_AVCODEC AND FFMPEG_AVUTIL AND FFMPEG_SWSCALE AND JPEG_LIB)
        target_link_libraries(lvglsim ${FFMPEG_AVFORMAT} ${FFMPEG_AVCODEC} ${FFMPEG_AVUTIL} ${FFMPEG_SWSCALE})
        target_compile_definitions(lvglsim PRIVATE APP_HAS_FFMPEG=1)
        target_include_directories(lvglsim PRIVATE ${CMAKE_SYSROOT}/usr/include)
        message(STATUS "FFmpeg video support enabled: ${FFMPEG_AVFORMAT}")
    else()
        message(WARNING "FFmpeg or libjpeg not found, video thumbnail/player disabled")
    endif()
endif()

# 链接 libdl（rga_accel.c / k230_accel.c 通过 dlopen 动态加载 librga.so / libmpi.so）。
# K230 平台无 librga，RK3566 平台无 libmpi，dlopen 失败时自动回退到软件路径。
target_link_libraries(lvglsim dl pthread)

# RK3566 RGA 头文件路径（sysroot 中 rga/ 目录）。
# 仅在 aarch64 平台添加，K230 无此头文件。
# 注意：im2d.h 中的 wrapbuffer_virtualaddr 宏直接调用 wrapbuffer_virtualaddr_t()，
# 该函数在 librga.so 中，必须在编译期链接（dlopen 方式无法覆盖宏展开的函数调用）。
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    target_include_directories(lvglsim PRIVATE ${CMAKE_SYSROOT}/usr/include)
    target_link_libraries(lvglsim rga)
    message(STATUS "RGA 硬件加速: 启用（aarch64 平台，链接 librga）")
else()
    message(STATUS "RGA 硬件加速: 禁用（${CMAKE_SYSTEM_PROCESSOR} 平台无 librga）")
endif()



# Install the lvgl_linux library and its headers
install(DIRECTORY src/lib/
    DESTINATION include/lvgl
    FILES_MATCHING
    PATTERN "backends*" EXCLUDE
    PATTERN "*.h")

install(TARGETS lvgl_linux
    ARCHIVE DESTINATION lib
)

add_custom_target(run COMMAND ${EXECUTABLE_OUTPUT_PATH}/lvglsim DEPENDS lvglsim)
