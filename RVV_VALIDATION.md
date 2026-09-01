# S2605 RocksDB RVV 验证与复现说明

本文记录本分支的通用 RVV 构建、RocksDB/RocketMQ 正确性验证和性能复现方法。性能结果采用本分支与同源标量构建的对照。

## 提交信息

- 基线：赛事指定 RocksDB `11.1.1`
- 分支：`task5-rvv`
- 验证提交：`81303d88b8dc682265466b4b1fcd5f3380b160eb`
- 作者：Zhuozhao Xia `<i@xiazhuozhao.com>`

## 验证环境

- 真机：MUSE Pi Pro，RISC-V 64 位，RVV 1.0，VLEN=256
- 操作系统：Bianbu Star 2.1，Linux 6.6.63，glibc 2.39
- 编译器：GNU GCC 16.1，`riscv64-unknown-linux-gnu`
- 模拟器：QEMU 8.2.9 user mode，VLEN=128/256/512
- RocksDB：11.1.1
- RocketMQ：5.5.0

赛事验证平台为蓝芯 LX5000；MUSE Pi Pro 与 SG2044 服务器仅为提交前真机验证环境，代码不限定这些机器。构建依赖 CMake、GNU GCC 16.1、目标 sysroot、RISC-V 版 gflags 与 zstd。RocketMQ 集成另需 JDK 17 以上、Maven 3 和官方 5.5.0 源码。所有步骤以普通用户运行，不需要 root 权限；构建和测试目录应放在空间充足的数据盘。

## 通用 RVV 与标量构建

仓库提供统一的交叉构建脚本。下面的路径仅作示例，应替换为本机工具链和依赖目录：

```sh
RISCV_TOOLCHAIN=/path/to/riscv-gnu \
RISCV_SYSROOT=/path/to/riscv-sysroot \
RISCV_CXX_ROOT=/path/to/riscv-cxx \
GFLAGS_ROOT=/path/to/riscv-gflags \
ZSTD_ROOT=/path/to/riscv-zstd \
RISCV_TUNE=generic-ooo \
BUILD_DIR=/data/xzz/build-rocksdb-rvv \
build_tools/build_riscv_rvv.sh rvv
```

标量对照使用同一提交和依赖：

```sh
RISCV_TOOLCHAIN=/path/to/riscv-gnu \
RISCV_SYSROOT=/path/to/riscv-sysroot \
RISCV_CXX_ROOT=/path/to/riscv-cxx \
GFLAGS_ROOT=/path/to/riscv-gflags \
ZSTD_ROOT=/path/to/riscv-zstd \
BUILD_DIR=/data/xzz/build-rocksdb-scalar \
build_tools/build_riscv_rvv.sh scalar
```

RVV 模式使用 `-march=rv64gcv -mabi=lp64d -mtune=generic-ooo -mrvv-vector-bits=scalable`；标量模式使用 `rv64gc`。默认构建关闭与对照无关的可选压缩库、jemalloc、NUMA 和 io_uring，并生成 `db_bench`。构建脚本会检查依赖路径，且不会写入系统目录。

## RocksDB 正确性测试

测试构建沿用脚本生成的 CMake cache，将 `WITH_TESTS` 打开后构建以下目标：

```sh
cmake -S . -B /data/xzz/build-rocksdb-rvv \
  -DWITH_TESTS=ON -DWITH_ALL_TESTS=OFF
cmake --build /data/xzz/build-rocksdb-rvv \
  --target crc32c_test bloom_test coding_test db_test -j
```

专项结果：

| 测试 | 结果 |
|---|---:|
| CRC/Bloom/Slice 随机与边界专项 | 通过 |
| QEMU VLEN=128 | 通过 |
| QEMU VLEN=256 | 通过 |
| QEMU VLEN=512 | 通过 |
| `crc32c_test` | 8/8 通过 |
| `bloom_test` | 26/26 通过 |
| `coding_test` | 11/11 通过 |
| `db_test` | 286/287 首轮通过；失败项立即单独复测通过 |

`db_test` 首轮唯一未通过项为 `DBTest.PurgeInfoLogs`，它涉及测试目录日志清理时序；使用相同二进制立即运行 `./db_test --gtest_filter=DBTest.PurgeInfoLogs` 得到 1/1 通过。RVV 专项测试还覆盖 CRC32C 校验和、Bloom 1 至 30 probes、损坏 filter、不同 Slice 长度和长公共前缀。

QEMU 的通用运行形式为：

```sh
for bits in 128 256 512; do
  qemu-riscv64 -cpu rv64,v=true,vlen=${bits},elen=64,vext_spec=v1.0 \
    -L /path/to/riscv/sysroot ./crc32c_test
done
```

x86 兼容构建的 `crc32c_test` 8/8 和 `coding_test` 11/11 也通过，确认非 RISC-V 构建不受 RVV 条件代码影响。

## db_bench 性能复现

标量与 RVV 二进制在同一真机上依次运行，删除各自旧数据库并固定相同 CPU 核。标准 WAL 参数如下：

```sh
taskset -c 0-7 ./db_bench \
  --benchmarks=fillrandom,waitforcompaction,readrandom,seekrandom \
  --num=100000 --reads=200000 --threads=8 \
  --key_size=16 --value_size=100 --compression_type=none \
  --bloom_bits=10 --cache_size=67108864 \
  --write_buffer_size=67108864 --target_file_size_base=67108864 \
  --max_background_jobs=8 --disable_wal=0 --sync=0 \
  --histogram=1 --seed=20260831 --db=/data/xzz/db-under-test
```

MUSE Pi Pro 上同机实测：

| 工作负载 | 标量 ops/s | RVV ops/s | 变化 |
|---|---:|---:|---:|
| fillrandom | 40,907 | 40,798 | -0.27% |
| readrandom | 404,565 | 409,858 | +1.31% |
| seekrandom | 142,135 | 161,106 | +13.35% |

CRC32C 使用相同输入缓冲区、迭代次数与 CPU 亲和性测得：

| 数据大小 | 标量 MiB/s | RVV MiB/s | 变化 |
|---|---:|---:|---:|
| 512 B | 286.8 | 477.1 | +66.4% |
| 4 KiB | 288.6 | 464.1 | +60.8% |
| 64 KiB | 278.4 | 381.4 | +37.0% |
| 1 MiB | 277.0 | 384.6 | +38.8% |

每次对照都应记录 `sha256sum`、`readelf -A`、CPU 信息、编译器版本、数据库参数和原始输出。若共享机器波动明显，采用交错顺序进行多轮测试，并报告中位数。

## RocketMQ 5.5.0 集成验证

构建 JNI 共享库和架构匹配的 JAR 后，将 JAR 安装到隔离的 Maven 仓库，再构建官方 RocketMQ 5.5.0：

```sh
mvn -Dmaven.repo.local=/data/xzz/m2 \
  install:install-file \
  -Dfile=/path/to/rocksdbjni-11.1.1-linux-riscv64.jar \
  -DgroupId=org.apache.rocketmq -DartifactId=rocketmq-rocksdb \
  -Dversion=1.0.6 -Dpackaging=jar

mvn -Dmaven.repo.local=/data/xzz/m2 -DskipTests package
```

全部 19 个 RocketMQ 模块构建通过。JNI 加载、短值保留和 8 字节大端物理偏移过滤语义均通过；Broker 启动后显示 `MessageRocksDBStorage init success`，验证期间后台错误计数为 0。

| 官方 benchmark 场景 | 结果 | 最大/平均 RT | 错误 |
|---|---:|---:|---:|
| 128 B，同步写，16 线程，100,000 条 | 1,703 TPS | 2,839 / 9.380 ms | 0 |
| 4 KiB，同步写，8 线程，20,000 条 | 715 TPS | 2,142 / 11.130 ms | 0 |
| 128 B 积压消费，16 线程 | 峰值 4,700 TPS | 积压消息端到端延迟 | 0 |

长稳测试可使用官方 producer/consumer benchmark 连续运行 60 分钟，覆盖 128 B、4 KiB、大 value 与积压消费。应同时保存 Broker 日志、错误计数、TPS、P99（由外部采样器计算）、磁盘用量和正常关闭结果；测试目录应置于空间充足的数据盘，并设置容量保护。

## 可移植性

实现使用标准 RVV 1.0 intrinsic、`vsetvl`/`vsetvlmax` 和 scalable vector bits，不含 SpacemiT/X60 专用分支、私有指令或固定 VLEN 假设。QEMU 的 VLEN 128/256/512 正确性测试验证了向量长度可移植性；MUSE Pi Pro 仅作为真机验证环境，不代表赛事最终评测平台。

## AI 使用说明

开发过程中使用 AI 辅助分析热点、编写代码初稿、交叉编译排障、组织测试和整理文档。按新增代码、构建集成、测试和文档工作量估算，AI 辅助占比约 90%。代码修改、真机/QEMU 验证、结果核对与提交均由作者完成。
