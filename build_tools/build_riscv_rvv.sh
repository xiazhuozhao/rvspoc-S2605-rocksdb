#!/usr/bin/env bash

set -euo pipefail

repo_root=$(cd "$(dirname "$0")/.." && pwd)
mode=${1:-rvv}
build_dir=${BUILD_DIR:-"$repo_root/build-riscv-$mode"}
toolchain=${RISCV_TOOLCHAIN:-/opt/riscv}
sysroot=${RISCV_SYSROOT:-/opt/riscv-sysroot}
cxx_root=${RISCV_CXX_ROOT:-$sysroot}
gflags_root=${GFLAGS_ROOT:-/opt/riscv-gflags}
zstd_root=${ZSTD_ROOT:-}
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN)}

case "$mode" in
  scalar)
    march=rv64gc
    optimization_flags=""
    ;;
  rvv)
    march=rv64gcv
    # Keep the default binary portable across out-of-order RV64GCV systems.
    # A different scheduler can be selected through RISCV_TUNE without
    # changing the generated ISA contract.
    riscv_tune=${RISCV_TUNE:-generic-ooo}
    optimization_flags="-mtune=$riscv_tune -mrvv-vector-bits=scalable -mstringop-strategy=vector -flto=auto -fno-semantic-interposition"
    ;;
  *)
    echo "usage: $0 [scalar|rvv]" >&2
    exit 2
    ;;
esac

cc="$toolchain/bin/riscv64-unknown-linux-gnu-gcc"
cxx="$toolchain/bin/riscv64-unknown-linux-gnu-g++"
if [[ "$mode" == rvv ]]; then
  ar="$toolchain/bin/riscv64-unknown-linux-gnu-gcc-ar"
  ranlib="$toolchain/bin/riscv64-unknown-linux-gnu-gcc-ranlib"
  linker_flags="-L$gflags_root/lib -flto=auto"
else
  ar="$toolchain/bin/riscv64-unknown-linux-gnu-ar"
  ranlib="$toolchain/bin/riscv64-unknown-linux-gnu-ranlib"
  linker_flags="-L$gflags_root/lib"
fi
target_include="$sysroot/usr/riscv64-linux-gnu/include"
target_lib="$sysroot/usr/riscv64-linux-gnu/lib"
cxx_include="$cxx_root/usr/riscv64-linux-gnu/include/c++/11"
cxx_target_include="$cxx_include/riscv64-linux-gnu"
cxx_lib="$cxx_root/usr/lib/gcc-cross/riscv64-linux-gnu/11"

for required in "$cc" "$cxx" "$ar" "$ranlib" \
  "$target_include" "$target_lib" "$cxx_include" "$cxx_target_include" \
  "$cxx_lib" "$gflags_root/include" "$gflags_root/lib"; do
  if [[ ! -e "$required" ]]; then
    echo "missing required cross-build path: $required" >&2
    exit 1
  fi
done

cmake_prefix_path=$gflags_root
if [[ -n "$zstd_root" ]]; then
  for required in "$zstd_root/include/zstd.h" "$zstd_root/lib/libzstd.so"; do
    if [[ ! -e "$required" ]]; then
      echo "missing required Zstandard path: $required" >&2
      exit 1
    fi
  done
  cmake_prefix_path="$cmake_prefix_path;$zstd_root"
fi

common_flags="--sysroot=$sysroot -B$target_lib -L$cxx_lib -L$target_lib -march=$march -mabi=lp64d $optimization_flags"
c_flags="$common_flags -isystem $target_include"
cxx_flags="$common_flags -nostdinc++ -isystem $cxx_include -isystem $cxx_target_include -isystem $target_include -I$gflags_root/include"

cmake -S "$repo_root" -B "$build_dir" \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=riscv64 \
  -DCMAKE_C_COMPILER="$cc" \
  -DCMAKE_CXX_COMPILER="$cxx" \
  -DCMAKE_AR="$ar" \
  -DCMAKE_RANLIB="$ranlib" \
  -DCMAKE_C_FLAGS="$c_flags" \
  -DCMAKE_CXX_FLAGS="$cxx_flags" \
  -DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG -fomit-frame-pointer" \
  -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -fomit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="$linker_flags" \
  -DCMAKE_SKIP_RPATH=TRUE \
  -DCMAKE_PREFIX_PATH="$cmake_prefix_path" \
  -DCMAKE_BUILD_TYPE=Release \
  -DPORTABLE=1 \
  -DWITH_LIBURING=OFF \
  -DWITH_SNAPPY=OFF \
  -DWITH_LZ4=OFF \
  -DWITH_ZLIB=OFF \
  -DWITH_ZSTD=ON \
  -DWITH_JEMALLOC=OFF \
  -DWITH_NUMA=OFF \
  -DROCKSDB_BUILD_SHARED=OFF \
  -DWITH_CORE_TOOLS=OFF \
  -DWITH_TOOLS=OFF \
  -DWITH_TRACE_TOOLS=OFF \
  -DWITH_BENCHMARK_TOOLS=ON \
  -DWITH_ALL_TESTS=OFF \
  -DWITH_TESTS=OFF \
  -DFAIL_ON_WARNINGS=OFF

cmake --build "$build_dir" --target db_bench -j"$jobs"
