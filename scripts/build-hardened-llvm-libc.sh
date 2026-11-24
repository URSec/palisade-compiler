#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${BUILD_DIR:-"$repo_root/build"}
libc_dir=${LIBC_DIR:-"$repo_root/build-hardened-libc"}
triple=thumbv8.1m.main-none-eabi
flags="-mcpu=cortex-m85+nomve -mfloat-abi=hard -fpalisade"

compile="<DEFINES> <INCLUDES> <FLAGS> -O2 -fno-pie -o <OBJECT> -c <SOURCE>"

rm -rf "$libc_dir"

cmake -S "$repo_root/runtimes" -B "$libc_dir/cmake" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SYSTEM_NAME=Generic \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DCMAKE_C_COMPILER="$build_dir/bin/clang" \
  -DCMAKE_CXX_COMPILER="$build_dir/bin/clang++" \
  -DCMAKE_C_COMPILER_TARGET=$triple \
  -DCMAKE_CXX_COMPILER_TARGET=$triple \
  -DCMAKE_AR="$build_dir/bin/llvm-ar" \
  -DCMAKE_RANLIB="$build_dir/bin/llvm-ranlib" \
  -DCMAKE_C_FLAGS="$flags" \
  -DCMAKE_CXX_FLAGS="$flags" \
  -DCMAKE_C_COMPILE_OBJECT="<CMAKE_C_COMPILER> $compile" \
  -DCMAKE_CXX_COMPILE_OBJECT="<CMAKE_CXX_COMPILER> $compile" \
  -DCMAKE_INSTALL_PREFIX="$libc_dir" \
  -DLLVM_ENABLE_RUNTIMES=libc \
  -DLLVM_LIBC_FULL_BUILD=ON \
  -DLLVM_INCLUDE_TESTS=OFF \
  -DLIBC_TARGET_TRIPLE=$triple \
  -DLIBC_CONF_PRINTF_DISABLE_FLOAT=OFF \
  -DLIBC_CONF_SCANF_DISABLE_FLOAT=OFF \
  "$@"

cmake --build "$libc_dir/cmake" --target install-libc install-libc-headers

printf '\nHardened LLVM libc is in %s\n' "$libc_dir"
