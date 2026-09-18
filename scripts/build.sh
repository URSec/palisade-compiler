#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${BUILD_DIR:-"$repo_root/build"}

jobs=$(getconf _NPROCESSORS_ONLN)
jobs=$((jobs > 8 ? 8 : jobs))
jobs=${JOBS:-$jobs}

cmake -S "$repo_root/llvm" -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Release}" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  '-DLLVM_ENABLE_PROJECTS=clang;lld' \
  -DLLVM_ENABLE_RUNTIMES= \
  -DLLVM_TARGETS_TO_BUILD=ARM \
  -DLLVM_ENABLE_ASSERTIONS=ON \
  -DLLVM_INCLUDE_TESTS=ON \
  -DLLVM_BUILD_TESTS=OFF \
  -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_INCLUDE_DOCS=OFF \
  -DLLVM_PARALLEL_LINK_JOBS="${LINK_JOBS:-2}" \
  "$@"

cmake --build "$build_dir" --parallel "$jobs" --target \
  clang lld llc opt llvm-ar llvm-ranlib llvm-as llvm-dis llvm-link \
  llvm-mc llvm-objcopy llvm-objdump llvm-readelf llvm-size

printf '\nCompiler and tools are available in %s/bin\n' "$build_dir"
