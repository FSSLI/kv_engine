#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

namespace kv {

// BlockCache 的 key 设计:(file_number << 32) | block_offset
// - 同一个数据块由「哪个文件 + 文件内偏移」唯一确定
// - file_number 单调递增、永不复用,所以 compaction 删掉旧文件后,
//   旧 key 不会被新数据误命中(只会白占空间,靠 EvictFile 清理)
inline uint64_t MakeBlockCacheKey(uint64_t file_number, uint64_t block_offset) {
    return (file_number << 32) | (block_offset & 0xFFFFFFFFull);
}

// 线程安全的 LRU 缓存,按字节计费,用来缓存 SSTable 的数据块:
// 同一个 (file_number, block_offset) 只读一次磁盘,之后全部走内存。
//
// 用法:
//   LRUCache cache(8 << 20);             // 8MB 容量
//   cache.Put(key, block_data);
//   std::string data;
//   if (cache.Get(key, &data)) { ... }   // 命中,跳过 ReadAt
//
// ============ 实现提示(src/lru_cache.cc 由你完成)============
// 数据结构:classics = 双向链表 + 哈希表
//   - lru_ 头部 = 最近使用(MRU),尾部 = 最久未用(LRU)
//   - index_ 提供 O(1) 定位:list<Node>::iterator
//
// 各方法要点:
//   Get:
//     - miss:misses_++,返回 false
//     - hit:把节点 splice 到 lru_ 头部,hits_++,拷贝 value 到 *value
//   Put:
//     - key 已存在:先 EraseUnlocked 旧节点(把旧 charge 从 usage_ 减掉),
//       再按新值走正常插入(=覆盖语义 + 刷新热度)
//     - charge(= value.size())> capacity_:直接返回,不插入、不报错
//       (单条就放得下整个缓存的块不值得缓存,且能避免驱逐死循环)
//     - 插入前驱逐:while (usage_ + charge > capacity_ && !lru_.empty())
//       从尾部 EraseUnlocked,直到放得下
//     - 新节点放头部,usage_ += charge
//   EvictFile(file_number):
//     - 遍历 index_,凡 (key >> 32) == file_number 的条目全部擦除
//     - O(n) 可接受:LevelDB 的 Evict 也是线性扫;它只在 compaction
//       删文件时调用,频率极低
//   Clear:清空,usage_ 归零(命中/miss 计数不清)
//
// 线程安全:
//   - 所有 public 方法进来先 std::lock_guard<std::mutex> lock(mutex_);
//   - 内部 helper(EraseUnlocked)假设锁已持有,绝不再加锁
//   - Week 3 任务②引入后台 flush 线程后,读路径会并发,锁从第一天就要对
// ============================================================
class LRUCache {
public:
    explicit LRUCache(size_t capacity_bytes);
    ~LRUCache() = default;

    // 插入或覆盖一个缓存条目,必要时按 LRU 顺序驱逐旧条目
    void Put(uint64_t key, const std::string& value);

    // 命中返回 true 并把值拷到 *value;未命中返回 false
    bool Get(uint64_t key, std::string* value);

    // 驱逐某个 SSTable 文件的全部缓存块(compaction 删除文件时调用)
    void EvictFile(uint64_t file_number);

    void Clear();

    size_t TotalCharge() const;  // 当前已用字节数(恒 <= capacity)
    uint64_t Hits() const;       // 累计命中次数,Week 4 压测写简历要用
    uint64_t Misses() const;     // 累计未命中次数

private:
    struct Node {
        uint64_t key;
        std::string value;
        size_t charge;  // 该条目计费字节数(= value.size())
    };

    // 调用前必须已持有 mutex_
    void EraseUnlocked(std::list<Node>::iterator it);

    const size_t capacity_;
    size_t usage_ = 0;
    uint64_t hits_ = 0;
    uint64_t misses_ = 0;

    mutable std::mutex mutex_;
    std::list<Node> lru_;  // front = MRU,back = LRU
    std::unordered_map<uint64_t, std::list<Node>::iterator> index_;

    LRUCache(const LRUCache&) = delete;
    LRUCache& operator=(const LRUCache&) = delete;
};

}  // namespace kv
