# LSM-Tree KV 存储引擎

自研 LSM-Tree 键值存储引擎,C++14 实现,架构对照 LevelDB 核心子集:从 MemTable、WAL、SSTable 到多版本管理与 Compaction,全部手写,不依赖第三方存储库。

## 特性

- **写路径**:WriteBatch 原子写入,先追加 WAL 再写 SkipList MemTable,顺序写替代随机写
- **双 MemTable + 异步 flush**:memtable 写满后切换为只读 immutable,后台线程落盘成 L0 SSTable,前台写不阻塞(对照 LevelDB `MakeRoomForWrite` 的简化版)
- **WAL 持久性可调**:支持 `sync` 开关——`sync=true` 每条写 fsync(强持久);`sync=false` 仅 flush 落盘时 fsync(语义对齐 LevelDB `sync=false`,吞吐优先)
- **SSTable**:DataBlock + IndexBlock + Footer 格式,索引常驻内存,点查一次定位数据块
- **布隆过滤器**:整文件一个过滤器(LevelDB 同款旋转哈希 + delta 步进算法),默认 10 bits/key、理论误判率约 1%,Open 时一次读入内存,无效磁盘 IO 在内存中拦掉
- **BlockCache**:线程安全 LRU,按字节计费,key 设计为 `(file_number << 32) | block_offset`,Compaction 删文件时按文件驱逐
- **TableCache**:SSTable 句柄缓存(索引 + 布隆常驻),`shared_ptr` 引用计数保证 Compaction 期间前台读不被中断
- **VersionSet**:Manifest + CURRENT 指针原子切换,崩溃恢复只重放活跃 WAL,避免重复恢复
- **自动 Compaction**:L0 文件数达阈值(4,对齐 LevelDB `kL0_CompactionTrigger`)时后台自动归并 L0→L1,多路归并去重 + tombstone 清理;与 flush 共用后台线程,手动/自动 compaction 互斥
- **并发正确性**:SSTable 读盘用 `pread` 原子定位,前台 Get 与后台归并共享文件句柄无竞态
- **基准测试**:内置 db_bench 风格 `bench_kv`,输出 ops/sec 与 p50/p99 延迟

## 架构

```
                     ┌──────────────────────────────┐
        写入 ──────▶ │            KVDB              │ ◀────── 读取
                     └──────────────────────────────┘
   写路径: Put/WriteBatch
      │
      ▼
  ┌────────┐   append   ┌──────────────┐
  │  WAL   │ ─────────▶ │  MemTable    │   (SkipList, 默认 4MB)
  │ (日志) │            │  (跳表)      │
  └────────┘            └──────────────┘
                               │ 写满,切换(原子)
                               ▼
                        ┌──────────────┐   后台线程 flush   ┌──────────────┐
                        │  Immutable   │ ────────────────▶ │ L0 SSTable   │
                        │  (只读跳表)  │                    │ (有序表文件) │
                        └──────────────┘                    └──────────────┘
                                                                   │ L0 文件数 ≥ 4
                                                                   ▼ 自动 Compaction
                                                            ┌──────────────┐
                                                            │ L1 SSTable   │
                                                            │ (归并去重后) │
                                                            └──────────────┘

  读路径: MemTable → Immutable → L0 → L1
          每层先查布隆过滤器(可能在才读盘),数据块走 BlockCache

  元数据: VersionSet(Manifest + CURRENT) 记录各层文件清单、
          活跃 WAL 编号、全局递增 sequence number
```

组件分层一句话版:API 层(KVDB:Put/Get/Delete/Write)→ 内存层(MemTable/Immutable 跳表 + WriteBatch)→ 持久层(WAL + SSTable:数据块 + 索引 + 布隆过滤器)→ 版本层(VersionSet/Manifest)→ 合并层(后台 flush + 自动 Compaction)→ 缓存层(TableCache 句柄 + BlockCache 数据块)。

## 构建与测试

```bash
# 依赖:C++14、CMake ≥ 3.10、pthread,无第三方库
mkdir build && cd build

# 跑功能测试必须用 Debug 构建:
# Release 下 -DNDEBUG 会禁掉 assert,测试会"假绿"
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
ctest --output-on-failure      # 11 个测试全部通过

# 跑 benchmark 必须用 Release 构建(-O2),Debug 数字无意义:
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
./bench_kv --num=200000
```

测试清单(11 个):`skiplist` `write_batch` `wal` `sstable` `version_set` `kv_db` `lru_cache` `merging_iterator` `async_flush`(双 MemTable 异步落盘) `auto_compact`(自动 Compaction) `bloom_filter`。

## API 速览

```cpp
#include "kv_db.h"
using namespace kv;

KVDB::Options options;
options.dbname = "/tmp/my_db";
options.write_buffer_size   = 4 << 20;   // MemTable 容量,默认 4MB
options.block_cache_capacity = 8 << 20;  // BlockCache 容量,0 = 关闭
options.bloom_bits_per_key  = 10;        // 布隆位数/key,0 = 关闭(理论误判 ~1%)

std::unique_ptr<KVDB> db;
Status s = KVDB::Open(options, &db);

db->Put("hello", "world");

WriteBatch batch;                 // 原子批量写:全部生效或全部不生效
batch.Put("k1", "v1");
batch.Delete("k2");
db->Write(&batch);

std::string value;
db->Get("hello", &value);         // s.IsNotFound() 区分"不存在"与"出错"
db->Delete("hello");
```

## Benchmark

`bench_kv` 为 db_bench 风格基准工具:

```bash
# 顺序灌 20 万条后随机读 / 查缺失 key(布隆拦截路径)
./bench_kv --num=200000 --benchmarks=fillseq,readrandom,readmissing

# 布隆过滤器对照:必须重新灌数据(--bloom_bits 在建文件时生效,
# 已落盘的过滤器按 footer 加载,与启动参数无关)
./bench_kv --num=200000 --benchmarks=fillseq,readmissing --bloom_bits=0 --db=/tmp/kv_nobloom

# 强持久口径:每条写 WAL 后 fsync(条数建议降到 1 万)
./bench_kv --num=10000 --benchmarks=fillseq --sync=1 --db=/tmp/kv_sync
```

| Flag | 默认 | 说明 |
|---|---|---|
| `--num` | 1000000 | 操作条数 |
| `--value_size` | 100 | value 字节数 |
| `--bloom_bits` | 10 | 布隆过滤器 bits/key,0 = 关闭 |
| `--cache` | 8388608 | BlockCache 字节数,0 = 关闭 |
| `--stride` | 8 | fill 步长;留字典序空隙,让 readmissing 落在已排序 key 区间内 |
| `--benchmarks` | fillseq,readrandom,readmissing | 逗号分隔,另有 fillrandom |
| `--db` | /tmp/kv_bench | 数据目录 |
| `--sync` | 0 | 1 = 每条写 fsync WAL(强持久);0 = flush 落盘时才 fsync |

## 压测结果

测试环境:WSL(Ubuntu on Windows,虚拟磁盘),单线程、单客户端;20 万条、value=100B,数据总量约 22MB,全部命中 page cache。**该口径测量的是内存命中下引擎路径(跳表 + 布隆 + 缓存 + 归并)的纯 CPU 效率**,与 LevelDB `db_bench --threads=1` 的默认口径一致,不代表真实磁盘 IO 场景。

| Benchmark | 吞吐 | p50 | p99 | 说明 |
|---|---|---|---|---|
| fillseq(sync=0) | 709,025 ops/s | 0.86 us | 2.75 us | 20 万条顺序写 |
| readrandom | 590,606 ops/s | 1.69 us | — | 随机读已存在的 key |
| readmissing | 3,215,640 ops/s | 0.31 us | 1.78 us | 查缺失 key,布隆在内存中拦截 |
| fillseq(sync=1) | 673.8 ops/s | — | 3.06 ms | 1 万条,每条写后 fsync |

布隆过滤器对照(同样 20 万条,重新灌数据):关闭时 readmissing 801,424 ops/s,开启后 3,215,640 ops/s,**约 4.0 倍**。

两个口径的含义:

- **sync=0(默认)**:WAL 写入只到 page cache,持久性粒度是"每次 flush";进程崩溃可能丢失尚未落盘的尾部写。这是 LevelDB `WriteOptions.sync=false` 的默认语义,也是上面 70 万 ops/s 的口径。
- **sync=1**:每条写 WAL 后 fsync,崩溃不丢已确认写入,但吞吐被磁盘 fsync 延迟(WSL 虚拟盘约 2ms/次)钉死。生产级用法是 sync=0 + 定期 flush,或像 Raft/RocksDB 那样批量 group commit。

## 项目结构

```
kv_engine/
├── CMakeLists.txt
├── bench/bench_kv.cc        # db_bench 风格基准测试
├── include/
│   ├── kv_db.h              # 主接口与 Options
│   ├── write_batch.h        # 原子批量写入
│   ├── memtable.h           # SkipList 内存表
│   ├── wal.h                # 预写日志
│   ├── sstable.h            # SSTable 读写 + TableCache
│   ├── bloom_filter.h       # 布隆过滤器策略
│   ├── lru_cache.h          # BlockCache(线程安全 LRU)
│   ├── merging_iterator.h   # 最小堆多路归并迭代器
│   ├── version_set.h        # 版本管理(Manifest/CURRENT)
│   └── iterator.h / slice.h / status.h
├── src/                     # 对应实现(9 个 .cc)
└── tests/                   # 11 个测试
```

## 已知限制

- 仅 L0/L1 两层,没有更深的 Level 与按层容量调度;写入持续放大时缺少 Write Stall 背压
- 数据块未压缩(LevelDB 默认 Snappy)
- BlockCache 为单个全局 LRU(一把锁),未做分片降低锁竞争
- 布隆过滤器按整文件一个设计(LevelDB 按 2KB key 区间分片,大文件下更省内存)
- 无 Snapshot 读(sequence number 已具备,未暴露一致性快照)
- 嵌入式单机库;基于本项目 + 自研 RPC 框架的服务化封装(KvService,双层错误模型)在配套仓库

## 后续计划

1. L2+ 多层与 Leveled 容量调度、L0 堆积时的 Write Stall 背压
2. BlockCache 分片 + 数据块 Snappy 压缩
3. Snapshot 一致性读与迭代器全局视图
4. 服务化形态下的端到端压测(RPC + KV 全链路)

## 作者

马超,计算机硕士在读。项目用于学习存储引擎内核与 C++ 系统编程,求职方向:C++ 后端 / 基础架构。
