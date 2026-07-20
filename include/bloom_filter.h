#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "slice.h"

namespace kv {

// BloomFilterPolicy:LevelDB 同款算法(旋转哈希 + delta 步进探测)。
//
// 设计取舍:整文件一个过滤器(LevelDB 按 2KB key 区间分片)。
// 我们的 SSTable 单文件通常 < 4MB,key 数有限,一个过滤器足够,
// 且省掉了 metaindex/filter block 索引层,实现直白得多。
//
// 特性:只可能误报(may match),绝不漏报——加进去的 key 一定返回 true。
// bits_per_key = 10 时理论误判率约 1%。
class BloomFilterPolicy {
public:
    explicit BloomFilterPolicy(int bits_per_key);

    // 把 keys 编码成一个过滤器 blob 追加到 dst(可序列化落盘)。
    // blob 布局:[位图 bytes][1 字节 k],k = 每个 key 的探测次数。
    void CreateFilter(const std::vector<std::string>& keys, std::string* dst) const;

    // key 可能存在返回 true,一定不存在返回 false。
    // 静态方法:k 编码在 blob 里,查询侧不需要构造 policy 实例。
    static bool KeyMayMatch(const Slice& key, const Slice& filter);

private:
    int bits_per_key_;
    size_t k_;  // 每个 key 置位的位数,约 bits_per_key * ln2
};

}  // namespace kv
