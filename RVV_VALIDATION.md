# S2605 RocksDB RVV 验证与复现说明

本文记录本分支的通用 RVV 构建、RocksDB/RocketMQ 正确性验证和性能复现方法。性能结果采用本分支与同源标量构建的对照。

## 提交信息

- 基线：赛事指定 RocksDB `11.1.1`
- 分支：`task5-rvv`
- 验证提交：`81303d88b8dc682265466b4b1fcd5f3380b160eb`
- 作者：Zhuozhao Xia `<i@xiazhuozhao.com>`

## RVV 算子与 ARM 优化覆盖审计

题面包含两个不同的覆盖要求，需分别统计：

| 题面明确要求的核心设施 | RVV 实现位置 | 状态 |
|---|---|---|
| Bloom Filter 位图查找 | `util/bloom_impl.h` | 已覆盖 |
| SST 序列化/反序列化热路径 | `include/rocksdb/slice.h`、`table/block_based/block.cc` | 已覆盖 |
| CRC32C 数据校验 | `util/crc32c.cc` | 已覆盖 |

因此，题面点名设施的功能覆盖为 **3/3（100%）**。这不等同于“已有 ARM/Neon 优化算子至少 90%”的架构对等覆盖。

以 `v11.1.1` 基线中 RocksDB 自有源码（排除第三方目录）实际存在的 ARM 专用加速设施为分母，可识别三族：Arm64 CRC32C（CRC/PMULL）、当前版 XXH3 NEON/SVE 哈希（`util/xxhash.h`）和格式兼容的 preview XXPH3 NEON 哈希（`util/xxph3.h`）。后两者产生和服务于不同的哈希接口，不能合并为一个实现。当前分支为三族都提供 RVV/通用 RISC-V 对等实现，因此严格架构对等覆盖为 **3/3（100%）**，超过 90% 要求。Bloom RVV 和 SST/Slice RVV 路径仍作为题面点名的新优化单独统计，不用于替代这三项分母。

```sh
git grep -n -E '__ARM_NEON|__aarch64__|HAVE_ARM64_CRC|XXPH_NEON' v11.1.1 -- \
  'util/*' 'table/*' 'db/*' 'memtable/*' 'port/*'
rg -n '__riscv_vector|__riscv_[A-Za-z0-9_]+' \
  util/bloom_impl.h util/crc32c.cc util/xxhash.h util/xxph3.h \
  include/rocksdb/slice.h \
  table/block_based/block.cc
```

这里的 3/3 是静态设施族覆盖率，不是动态向量指令比例。CRC、Bloom 和当前版 XXH3 均保留按长度/工作量分发；XXH3 的短中输入继续使用标量路径，足够长的批量 stripe 才进入 VLEN 无关的 RVV 后端。preview XXPH3 的长输入直接使用 RVV 后端。只有实际进入 RVV 分支的调用才产生 RVV 指令。

## 验证环境

- 真机：MUSE Pi Pro，RISC-V 64 位，RVV 1.0，VLEN=256
- 操作系统：Bianbu Star 2.1，Linux 6.6.63，glibc 2.39
- 编译器：GNU GCC 16.1，`riscv64-unknown-linux-gnu`
- 模拟器：QEMU 8.2.9 user mode，VLEN=128/256/512
- RocksDB：11.1.1
- RocketMQ：5.5.0
- 长稳验证服务器：SG2044，64 核 RV64GCV，openEuler 24.03 LTS-SP3，Linux 6.12.74，OpenJDK 21.0.9

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

在 RVV 构建目录的现有交叉编译 cache 上启用 JNI。`JAVA_HOME` 必须指向构建主机上可运行的 JDK；JNI 头文件不进入目标机运行时依赖：

```sh
JAVA_HOME=/path/to/host-jdk cmake -S . -B /data/xzz/build-rocksdb-rvv \
  -DWITH_JNI=ON
JAVA_HOME=/path/to/host-jdk cmake --build /data/xzz/build-rocksdb-rvv \
  --target rocksdbjava -j
```

产物位于构建目录的 `java/librocksdbjni-linux-riscv64.so` 和 `java/rocksdbjni-11.1.1-linux-riscv64.jar`。将 JAR 安装到隔离的 Maven 仓库，再构建官方 RocketMQ 5.5.0：

```sh
mvn -Dmaven.repo.local=/data/xzz/m2 \
  install:install-file \
  -Dfile=/path/to/rocksdbjni-11.1.1-linux-riscv64.jar \
  -DgroupId=org.apache.rocketmq -DartifactId=rocketmq-rocksdb \
  -Dversion=1.0.6 -Dpackaging=jar

cd /path/to/rocketmq-all-5.5.0
mvn -Dmaven.repo.local=/data/xzz/m2 -DskipTests package
```

全部 19 个 RocketMQ 模块构建通过。JNI 加载、短值保留和 8 字节大端物理偏移过滤语义均通过；Broker 启动后显示 `MessageRocksDBStorage init success`，验证期间后台错误计数为 0。

| 官方 benchmark 场景 | 结果 | 最大/平均 RT | 错误 |
|---|---:|---:|---:|
| 128 B，同步写，16 线程，100,000 条 | 1,703 TPS | 2,839 / 9.380 ms | 0 |
| 4 KiB，同步写，8 线程，20,000 条 | 715 TPS | 2,142 / 11.130 ms | 0 |
| 128 B 积压消费，16 线程 | 峰值 4,700 TPS | 积压消息端到端延迟 | 0 |

60 分钟长稳测试使用 `storeType=defaultRocksDB` 的单 Broker，并把 `storePathRootDir` 和 `storePathCommitLog` 指向空间充足的数据盘。配置文件至少包含以下项目，其余项沿用 RocketMQ 默认值：

```properties
brokerClusterName=Task5StabilityCluster
brokerName=broker-task5-rvv
brokerId=0
namesrvAddr=127.0.0.1:9876
brokerIP1=127.0.0.1
storeType=defaultRocksDB
storePathRootDir=/data/xzz/rocketmq-task5/store
storePathCommitLog=/data/xzz/rocketmq-task5/store/commitlog
autoCreateTopicEnable=true
```

以下命令同时覆盖 128 B 混合收发、4 KiB 大消息和延迟 10 分钟启动消费者形成的积压；目录和 JVM 大小应按验证机调整：

```sh
export JAVA_HOME=/path/to/jdk-17-or-newer
export ROCKETMQ_HOME=/path/to/rocketmq-5.5.0
export NAMESRV=127.0.0.1:9876
export CLASSPATH="$ROCKETMQ_HOME/conf:$ROCKETMQ_HOME/lib/*"
java_client=("$JAVA_HOME/bin/java" -Xms1g -Xmx1g -cp "$CLASSPATH")

JAVA_HOME="$JAVA_HOME" nohup "$ROCKETMQ_HOME/bin/mqnamesrv" \
  > namesrv.log 2>&1 &
namesrv_pid=$!
JAVA_HOME="$JAVA_HOME" nohup "$ROCKETMQ_HOME/bin/mqbroker" \
  -n "$NAMESRV" -c /path/to/broker-rocksdb.conf > broker.log 2>&1 &
broker_pid=$!

timeout 4200 "${java_client[@]}" \
  org.apache.rocketmq.example.benchmark.Consumer \
  -n "$NAMESRV" -t Task5StabilitySmall -g task5_small -w 16 -ri 10000 \
  > consumer-small.log 2>&1 &
small_consumer=$!
( sleep 600; timeout 3600 "${java_client[@]}" \
  org.apache.rocketmq.example.benchmark.Consumer \
  -n "$NAMESRV" -t Task5StabilityLarge -g task5_large -w 16 -ri 10000 \
  > consumer-large.log 2>&1 ) &
large_consumer=$!

timeout 3600 "${java_client[@]}" \
  -Dorg.apache.rocketmq.client.sendSmartMsg=true \
  org.apache.rocketmq.example.benchmark.Producer \
  -n "$NAMESRV" -t Task5StabilitySmall -s 128 -w 16 -q 0 -ri 10000 \
  > producer-small.log 2>&1 &
small_producer=$!
timeout 3600 "${java_client[@]}" \
  -Dorg.apache.rocketmq.client.sendSmartMsg=true \
  org.apache.rocketmq.example.benchmark.Producer \
  -n "$NAMESRV" -t Task5StabilityLarge -s 4096 -w 8 -q 0 -ri 10000 \
  > producer-large.log 2>&1 &
large_producer=$!
wait "$small_producer" || test $? -eq 124
wait "$large_producer" || test $? -eq 124
```

运行期间每 30 秒记录 Broker/NameServer 存活状态、内存和磁盘余量，并在磁盘可用空间低于预计安全线时主动终止。生产结束后允许消费者排空积压，再用 `mqadmin consumerProgress` 确认两个消费组的 `Diff Total` 均为 0；随后按启动时记录的 PID 终止两个消费者及本轮 Broker/NameServer 进程树并等待正常退出。同时检查 producer/consumer 失败计数、Broker 的 OOM/Corruption/Background error 和最终磁盘用量。RocketMQ 自带的 `mqshutdown` 按进程名匹配，在同一账号运行多个实例的共享服务器上不可使用，以免停止其他实例。P99 需由外部采样器从同一轮请求延迟采样计算，不能用最大 RT 代替。

2026-09-01 在 SG2044 上完成了一轮正式长稳验证。生产持续 60 分钟，完整流程从 14:23:13 至 15:24:36；4 KiB 消费者延迟 10 分钟启动，形成约 10 分钟积压后追平。使用的 `rocketmq-rocksdb-1.0.6.jar` SHA-256 为 `dac722d0fd05054669c66cde4c3b1620eab3e722893bf97b881e26bbfd7e35e1`。

| 场景 | 10 秒样本数 | TPS 中位数 | 样本平均 RT 中位数 | 失败 |
|---|---:|---:|---:|---:|
| 128 B 同步生产，16 线程 | 359 | 4,126 | 3.875 ms | 0 |
| 4 KiB 同步生产，8 线程 | 359 | 2,449 | 3.261 ms | 0 |
| 128 B 消费，16 线程 | 357 | 4,125 | 6.643 ms B2C | 0 |
| 4 KiB 积压消费，16 线程 | 300 | 2,464 | 6.314 ms B2C | 0 |

最终 Broker 状态中 `putMessageTimesTotal` 与 `msgGetTotalTodayNow` 均为 23,455,128，`dispatchBehindBytes=0`；两个消费组均为 `Diff Total=0`、`Inflight Total=0`。120 次健康采样中的最低磁盘可用空间为 724,601,757,696 字节，最终 store 目录约 42 GB。Broker 和客户端日志中没有 OOM、Corruption、Background error、发送失败、响应失败或消费失败，测试进程正常退出。4 KiB 消费端观测到的最大 B2C 延迟为 602,030 ms，对应人为设置的 10 分钟积压窗口，不是稳态请求 P99。

## 可移植性

实现使用标准 RVV 1.0 intrinsic、`vsetvl`/`vsetvlmax` 和 scalable vector bits，不含 SpacemiT/X60 专用分支、私有指令或固定 VLEN 假设。QEMU 的 VLEN 128/256/512 正确性测试验证了向量长度可移植性；MUSE Pi Pro 仅作为真机验证环境，不代表赛事最终评测平台。

## AI 使用说明

开发过程中使用 AI 辅助分析热点、编写代码初稿、交叉编译排障、组织测试和整理文档。按新增代码、构建集成、测试和文档工作量估算，AI 辅助占比约 90%。代码修改、真机/QEMU 验证、结果核对与提交均由作者完成。
