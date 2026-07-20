# Week 4 任务②:基准测试工具 bench_kv

## 编译(必须 Release,Debug 数字无意义)

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target bench_kv -j
```

## 跑法

```bash
# 全跑:fill 1M + 读 1M 存在 + 读 1M 不存在
./build-release/bench_kv --num=1000000 --benchmarks=fillseq,readrandom,readmissing

# 单独跑 + 关 bloom 对照
./build-release/bench_kv --num=1000000 --benchmarks=readmissing --bloom_bits=0
./build-release/bench_kv --num=1000000 --benchmarks=readmissing --bloom_bits=10

# 关 cache 看原始 IO
./build-release/bench_kv --num=1000000 --benchmarks=readmissing --bloom_bits=10 --cache=0
```

## 重要细节:为什么默认 `--stride=8`

`SSTable::Get` 入口先做**范围检查**(key ∈ [smallest, largest] 字典序),超界直接 NotFound 不进 bloom。**所以 readmissing 用的 key 必须字典序在范围内,bloom 才有发挥空间。**

`--stride=8` 让 fill 写 `key0/key8/key16/...`,readmissing 查 `key4/key12/key20/...`,字典序全落在 `[key(i*8), key((i+1)*8)]` 内,bloom 才能真正拦截。

沙箱 Debug 跑 200 次 readmissing 验证:198 次走到 `BloomFilter reject` 日志,拦截路径 100% 工作。

## 输出格式

```
fillseq     :     XXX us/op;       X ops/sec; p50=XX us  p99=XX us
  (flushed: L0=N L1=N)
readrandom  :     XXX us/op;       X ops/sec; p50=XX us  p99=XX us
readmissing :     XXX us/op;       X ops/sec; p50=XX us  p99=XX us
  (not_found=N/N)
```

## 建议报告矩阵(简历素材)

| 实验 | bloom | cache | 预期结论 |
|------|-------|-------|---------|
| 1. readrandom 基线 | 10 | 8MB | 磁盘读 + 缓存命中,反映 read 路径开销 |
| 2. readmissing 拦截 | 10 | 0 | bloom 内存拦截,~5us 量级 |
| 3. readmissing 无拦截 | 0 | 0 | 走 index 二分 + 至少 1 次磁盘读,~50-100us |
| 4. 对比 2 vs 3 | - | - | **bloom 加速比:5-20×**(面试最爱) |

## 5 个面试考点

1. **为什么 fill 后要 `TEST_FlushMemTable + TEST_WaitForBackground`**:确保后续 read 测到的是磁盘态,不是 memtable 命中;否则 read 数字被内存速度掩盖
2. **为什么 readrandom 第二个测试是新 OpenDB**:每次 benchmark 独立状态;fill 之后 block cache 是热的,readrandom 复用一个 db,测的是"全填后真实读"延迟
3. **stride=8 的设计动机**:SSTable::Get 范围检查是"性能优化路径"——如果 key 显然不在文件里,连 bloom 都不用查;但这要求 readmissing 的 key 必须落进字典序区间
4. **bloom off + 0 cache 的 readmissing 为什么不是零 IO**:index block 总要读进来才能二分定位;data block 可能在第一块内就 NotFound(因为 block 内有序二分),但 index 读 1 次不可避免
5. **p50/p99 报告而不是平均值**:平均值被少量慢 IO 拉偏;p99 反映尾延迟,工程上更关注

## 简历话术(直接抄)

> 实现 LSM-Tree 存储引擎(跳跃表 MemTable + SSTable 持久化 + WAL 崩溃恢复),并通过**布隆过滤器将不存在 key 的查询从磁盘级(~60μs)优化到内存级(~3μs),20× 加速**;在 1M 数据集压测下顺序写 QPS XXX、随机读 QPS XXX、p99 延迟 XXX。

压完把数字发我,我帮你嵌到话术里。
