# LSM-Tree KV 存储引擎

> 基于 LSM-Tree 的高性能键值存储引擎，对标 LevelDB 核心子集，C++14 实现。

## 项目定位

- **名称**：基于 LSM-Tree 的高性能 KV 存储引擎
- **对标**：LevelDB / RocksDB（核心子集）
- **语言**：C++14
- **适用岗位**：C++ 后端开发 / 基础架构 / 分布式系统 / 数据库内核

**与现有 RPC 框架的关系**：
- RPC 框架解决**服务间通信**问题（已完成）
- KV 引擎解决**数据持久化**问题（本项目）
- 两者组合构成完整的分布式服务底座

---

## 架构概览

```
┌─────────────────────────────────────────────┐
│  应用层：RPC Client → RPC Server → KV Engine │
├─────────────────────────────────────────────┤
│  API 层：DB（Put/Get/Delete/Scan/WriteBatch）│
├─────────────────────────────────────────────┤
│  内存层：MemTable（跳表）+ WriteBatch（事务） │
├─────────────────────────────────────────────┤
│  持久层：WAL（预写日志）+ SSTable（有序表）   │
├─────────────────────────────────────────────┤
│  合并层：Compaction（Level 0 → Level N）     │
├─────────────────────────────────────────────┤
│  缓存层：BlockCache + BloomFilter（预留）    │
└─────────────────────────────────────────────┘
```

### 核心组件

| 组件 | 说明 | 状态 |
|------|------|------|
| **SkipList** | 内存表（MemTable）底层数据结构，支持并发读、单线程写 | ✅ 完成 |
| **WriteBatch** | 原子写入单元，支持 Put/Delete 混合批次 | ✅ 完成 |
| **WAL** | 预写日志，顺序追加写，崩溃后支持断点恢复 | ✅ 完成 |
| **SSTable** | 有序字符串表，DataBlock + IndexBlock + Footer 格式 | ✅ 完成 |
| **VersionSet** | 版本管理，Manifest + CURRENT 原子更新 | ✅ 完成 |
| **MergingIterator** | 最小堆归并，支持 MemTable + 多级 SSTable 统一遍历 | ✅ 完成 |
| **TableCache** | 文件句柄缓存，weak_ptr 引用计数保活 | ✅ 完成 |
| **Compaction** | L0→L1 手动触发，多路归并 + 去重 | ✅ 完成 |
| **BlockCache** | 分片 LRU 缓存（预留接口） | 🚧 待实现 |
| **BloomFilter** | 布隆过滤器，减少无效磁盘 IO（预留接口） | 🚧 待实现 |
| **自动 Compaction** | 后台线程自动调度，阈值触发 | 🚧 待实现 |
| **RPC 联动** | 通过 RPC 框架暴露 KvService | 🚧 待实现 |

---

## 核心特性

### 1. 顺序写替代随机写
- 所有写入先顺序追加到 WAL，再写内存中的 SkipList
- 写性能比 B+Tree 原地更新高一个数量级

### 2. 崩溃安全（Crash Safety）
- WAL 每条记录带 `batch_length + checksum`，尾部损坏自动跳过
- Manifest 采用 CURRENT 指针机制，原子更新保证元数据一致性
- Recover 时只重放 `log_number_` 和 `prev_log_number_` 对应的 WAL，避免重复恢复

### 3. 多版本去重
- 全局递增 Sequence Number，同一 key 多次写入可区分版本
- Compaction 时通过 MergingIterator 堆排序 + `last_key` 去重，只保留最新版本

### 4. 引用计数保活
- TableCache 使用 `weak_ptr`，Iterator 持有 `shared_ptr`
- SSTable 文件在 Compaction 期间若仍被前台读取，不会误删

---

## 快速开始

### 编译

```bash
# 克隆仓库
git clone <repo-url> kv_engine
cd kv_engine

# 创建构建目录
mkdir build && cd build

# 编译（Release 模式）
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# 编译（Debug 模式，开启日志）
cmake .. -DCMAKE_BUILD_TYPE=Debug -DKV_DEBUG=ON
make -j$(nproc)
```

### 运行测试

```bash
# 运行所有测试
ctest --output-on-failure

# 或单独运行
./test_skiplist
./test_write_batch
./test_wal
./test_sstable
./test_version_set
./test_merging_iterator
./test_kv_db
```

### 基础 API 使用

```cpp
#include "kv_db.h"

using namespace kv;

// 打开数据库
KVDB::Options options;
options.dbname = "/tmp/my_db";
options.write_buffer_size = 4 * 1024 * 1024;  // 4MB

std::unique_ptr<KVDB> db;
Status s = KVDB::Open(options, &db);
assert(s.ok());

// 写入
s = db->Put("hello", "world");
assert(s.ok());

// 原子批量写入
WriteBatch batch;
batch.Put("key1", "value1");
batch.Put("key2", "value2");
batch.Delete("key3");
s = db->Write(&batch);
assert(s.ok());

// 读取
std::string value;
s = db->Get("hello", &value);
if (s.ok()) {
    std::cout << "value: " << value << std::endl;
} else if (s.IsNotFound()) {
    std::cout << "key not found" << std::endl;
}

// 删除
s = db->Delete("hello");
assert(s.ok());

// 手动触发 Compaction（L0 → L1）
s = db->CompactManually();
assert(s.ok());
```

---

## 核心设计决策

### 为什么选 SkipList 而不是 B+Tree？

- **实现简单**：跳表用指针链接，无需处理 B+Tree 的节点分裂、合并、平衡
- **并发读友好**：单线程写 + 多线程读模型下，跳表的 `atomic` 指针保证读可见性
- **和 LevelDB 一致**：面试时可直接对标业界成熟方案

### WAL 格式设计

```
[batch_length: 4B]  -- 整个 body + checksum 的长度
[sequence_number: 8B]  -- 全局递增序列号
[count: 4B]  -- Batch 内操作条数
[type: 1B] [key_len: 4B] [key] [value_len: 4B] [value] ...  -- 记录体
[checksum: 4B]  -- 对 body 的校验
```

- **WriteBatch 作为 WAL 最小单元**：从 Day 1 统一格式，后续扩展原子写入零改动
- **sequence_number**：全局原子递增，用于版本控制、Compaction 可见性判断、未来 Snapshot 扩展

### SSTable 文件格式

```
┌────────────────────────────────────────┐
│  DataBlock 1  (若干键值对，按 Key 升序)  │
├────────────────────────────────────────┤
│  DataBlock 2                           │
├────────────────────────────────────────┤
│  ...                                   │
├────────────────────────────────────────┤
│  IndexBlock                            │
│  [last_key_1, offset_1, size_1]        │
│  [last_key_2, offset_2, size_2]        │
│  ...                                   │
├────────────────────────────────────────┤
│  Footer (32B)                          │
│  [index_offset: 8B]                    │
│  [index_size: 8B]                      │
│  [num_entries: 8B]                     │
│  [magic_number: 8B]                    │
└────────────────────────────────────────┘
```

### Compaction 流程（L0 → L1）

1. **选文件**：取 Level 0 所有文件 + Level 1 中 Key 范围重叠的文件
2. **建立归并迭代器**：MergingIterator 覆盖上述所有文件
3. **遍历归并输出**：
   - 相同 key 只保留第一个（最新版本）
   - 跳过 Delete tombstone（空 value）
4. **原子更新 VersionSet**：写入 Manifest，记录新文件列表 + 删除旧文件列表
5. **清理**：删除旧 SSTable 文件

### Recover 流程

```
1. 检查 CURRENT 文件是否存在
   ├── 不存在 → 新 DB，跳过恢复
   └── 存在 → 读取 Manifest，恢复 VersionSet（SSTable 列表 + log_number + prev_log_number）

2. 列出目录下所有 .log 文件
   ├── 只保留编号等于 log_number 或 prev_log_number 的 WAL
   └── 其他 .log 文件直接删除（已刷盘，无需恢复）

3. 按编号从小到大依次重放 WAL：
   ├── 读取每条 WriteBatch 的 batch_length + checksum
   ├── 校验 checksum：失败 → 跳过该条记录（尾部损坏）
   └── 反序列化 WriteBatch → 插入 MemTable

4. 恢复完成，DB 就绪
```

---

## 性能数据

> 测试环境：本地开发机（待补充实际压测数据）

| 操作 | QPS | 说明 |
|------|-----|------|
| 随机写 | — | 待压测 |
| 随机读 | — | 待压测 |
| 范围扫描 | — | 待压测 |

---

## 项目结构

```
kv_engine/
├── CMakeLists.txt              # 构建配置
├── README.md                   # 项目说明
├── include/                    # 头文件
│   ├── kv_db.h                 # 主接口
│   ├── write_batch.h           # 批量写入
│   ├── memtable.h              # 跳表内存表
│   ├── wal.h                   # 预写日志
│   ├── sstable.h               # 有序字符串表
│   ├── iterator.h              # 迭代器接口
│   ├── merging_iterator.h      # 最小堆归并迭代器
│   ├── version_set.h           # 版本管理
│   └── status.h                # 状态码
├── src/                        # 实现
│   ├── kv_db.cc
│   ├── write_batch.cc
│   ├── memtable.cc
│   ├── wal.cc
│   ├── sstable.cc
│   ├── version_set.cc
│   └── merging_iterator.cc
└── tests/                      # 单元测试
    ├── test_skiplist.cc
    ├── test_write_batch.cc
    ├── test_wal.cc
    ├── test_sstable.cc
    ├── test_version_set.cc
    ├── test_merging_iterator.cc
    └── test_kv_db.cc
```

---

## 面试核心考点

| 考点 | 答案要点 |
|------|----------|
| **为什么选 LSM-Tree 而不是 B+Tree** | 顺序写替代随机写，写性能提升一个数量级；代价是读放大，适合写多读少场景 |
| **WAL 和 MemTable 的关系** | WAL 保证持久性，MemTable 保证低延迟读；写路径先 WAL 再 MemTable，崩溃后重放 WAL 恢复 |
| **Compaction 策略** | Leveled Compaction，每层大小上限 10 倍递增；多路归并减少写放大 |
| **Sequence Number 的作用** | 全局递增序列号，用于版本控制、Compaction 时判断数据可见性、未来扩展 Snapshot 隔离 |
| **TableCache 和 BlockCache 的区别** | TableCache 缓存打开的文件句柄（Index Block + BloomFilter 元数据）；BlockCache 缓存解压后的数据块 |
| **和 LevelDB/RocksDB 的区别** | 自研更轻量，核心逻辑亲手实现；RocksDB 功能更全面但代码量巨大 |
| **双 MemTable 切换机制** | mutable 写满后设为 immutable，后台异步落盘为 SSTable，前台继续写新的 mutable。避免前台阻塞等待落盘 |
| **WAL 文件版本号管理** | Manifest 记录 `log_number_`（活跃 WAL）和 `prev_log_number_`（待刷盘 WAL），Recover 只重放这两个编号的 WAL，避免重复恢复 |

---

## 防坑记录

| 坑点 | 应对方案 |
|------|----------|
| WAL 写一半崩溃，文件尾部损坏 | 记录头带 `batch_length + checksum`，解析时跳过损坏记录 |
| Compaction 中途崩溃，一半数据在新文件一半在旧文件 | Manifest 记录新版本，旧文件未删也不影响正确性 |
| Iterator 遍历期间 SSTable 被 Compaction 删除 | Iterator 持有 `shared_ptr<Table>`，引用计数保活 |
| 时钟回拨导致 WAL 顺序错乱 | 用单调递增的 sequence number，不依赖系统时间 |
| 写入速度超过 Compaction 速度，Level 0 堆积导致 OOM | Write Stall：Level 0 文件数 ≥ 8 时阻塞前台 Put |
| 每次 Get 都重新 open() SSTable 文件，性能极差 | TableCache 缓存打开的文件句柄，Week 1 写 SSTable 时顺带实现 |

---

## 开发计划

| 阶段 | 时间 | 目标 | 状态 |
|------|------|------|------|
| Week 1 | 7.21-7.27 | 单文件读写：SkipList + WAL + SSTable + 基础 Get | ✅ 提前完成 |
| Week 2 | 7.28-8.03 | 多文件恢复 + 基础合并：MergingIterator + VersionSet + Recover + 手动 Compaction | ✅ 提前完成 |
| Week 3 | 8.04-8.10 | 多级合并 + 清理：自动 Compaction + Tombstone 清理 + BlockCache | 🚧 待开始 |
| Week 4 | 8.11-8.17 | 优化 + 联动 + 压测：BloomFilter + RPC 挂载 + 压测 + 文档 | 🚧 待开始 |

---

## 作者

- **马超** — 西北大学计算机硕士
- 求职方向：C++ 后端开发 / 基础架构 / 分布式系统
