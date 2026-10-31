#!/bin/bash
# Build only the Mali r38p1 GPU driver against the currently configured out/ tree.
#
# Prerequisite (once, or after defconfig changes):
#   ./build.sh            # full kernel build (also builds the GPU driver)
# or:
#   make O=out ... evergo_defconfig && make O=out ... prepare
#
# This script is for fast iteration on drivers/misc/mediatek/gpu/.../mali-r38p1.
set -e

cd "$(dirname "$0")"

TC_DIR="$HOME/toolchains"
CLANG_DIR="$TC_DIR/Clang-16.0/bin/"
CLANG_LIB="$TC_DIR/Clang-16.0/lib/"

export PATH="$CLANG_DIR:$PATH"
export LD_LIBRARY_PATH="$CLANG_LIB:$LD_LIBRARY_PATH"
export ARCH=arm64
export SUBARCH=arm
export CLANG_TRIPLE=aarch64-linux-gnu-
export LD=ld.lld
export O=out

FLGS="--target=aarch64-linux-gnu \
      -Wno-implicit-function-declaration \
      -Wno-unused-but-set-variable \
      -Wno-unused-variable \
      -Wno-unused-function \
      -Wno-unused-label \
      -Wno-incompatible-pointer-types"

GPU_DIR=drivers/misc/mediatek/gpu/gpu_mali/mali_valhall/mali-r38p1/drivers/gpu/arm/midgard

make -j"$(nproc --all)" O=out \
    CC=clang \
    LD=ld.lld \
    CLANG_TRIPLE=aarch64-linux-gnu- \
    CROSS_COMPILE=aarch64-linux-gnu- \
    CROSS_COMPILE_COMPAT=aarch64-linux-gnueabi- \
    CROSS_COMPILE_ARM32=arm-linux-gnueabi- \
    LLVM_IAS=1 \
    KCFLAGS="$FLGS" \
    "$GPU_DIR/"
