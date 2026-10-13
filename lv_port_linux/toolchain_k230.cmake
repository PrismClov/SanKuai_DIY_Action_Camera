# K230 RISC-V64 CMake交叉编译工具链
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR riscv64)

# 工具链前缀，和脚本保持一致
set(TC_PREFIX "riscv64-unknown-linux-gnu")
set(TOOLCHAIN_ROOT "/home/rk/Desktop/k230_linux/k230_linux_sdk/output/k230_canmv_lckfb_defconfig/host/bin")

set(CMAKE_C_COMPILER ${TOOLCHAIN_ROOT}/${TC_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_ROOT}/${TC_PREFIX}-g++)
set(CMAKE_AR ${TOOLCHAIN_ROOT}/${TC_PREFIX}-ar)
set(CMAKE_RANLIB ${TOOLCHAIN_ROOT}/${TC_PREFIX}-ranlib)
set(CMAKE_STRIP ${TOOLCHAIN_ROOT}/${TC_PREFIX}-strip)

# sysroot路径，和脚本对齐
set(SYSROOT "${TOOLCHAIN_ROOT}/../riscv64-buildroot-linux-gnu/sysroot")
set(CMAKE_SYSROOT ${SYSROOT})

# 链接器搜索路径
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# 编译通用参数
add_compile_options(-O2 -Wall -g)