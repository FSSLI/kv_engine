#include "lru_cache.h"

namespace kv {

LRUCache::LRUCache(size_t capacity_bytes)
    : capacity_(capacity_bytes) {
}

bool LRUCache::Get(uint64_t key, std::string* value) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = index_.find(key);
    if (it == index_.end()) {
        misses_++;
        return false;
    }
    // splice 是 O(1) 节点转移,迭代器不失效,移到头部表示刚被用过
    lru_.splice(lru_.begin(), lru_, it->second);
    hits_++;
    *value = it->second->value;
    return true;
}

// 私有助手:调用时锁已持有,绝对不加锁
void LRUCache::EraseUnlocked(std::list<Node>::iterator it) {
    usage_ -= it->charge;
    index_.erase(it->key);
    lru_.erase(it);
}

void LRUCache::Put(uint64_t key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t charge = value.size();
    if (charge > capacity_) return;  // 超大条目不缓存

    // 覆盖语义:先释放旧节点,再驱逐,最后插入(顺序不能乱)
    auto it = index_.find(key);
    if (it != index_.end()) {
        EraseUnlocked(it->second);
    }
    while (usage_ + charge > capacity_ && !lru_.empty()) {
        EraseUnlocked(--lru_.end());  // 从尾部(最久未用)开始驱逐
    }
    lru_.push_front({key, value, charge});
    index_[key] = lru_.begin();
    usage_ += charge;
}

void LRUCache::EvictFile(uint64_t file_number) {
    std::lock_guard<std::mutex> lock(mutex_);
    // O(n) 线性扫可接受:LevelDB 同样线性扫;只在 compaction 删文件时调用,频率极低
    for (auto it = index_.begin(); it != index_.end();) {
        if ((it->first >> 32) == file_number) {
            usage_ -= it->second->charge;
            lru_.erase(it->second);
            it = index_.erase(it);  // C++11: erase 返回下一个迭代器
        } else {
            ++it;
        }
    }
}

void LRUCache::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    lru_.clear();
    index_.clear();
    usage_ = 0;
    // hits_/misses_ 不清:累计统计,Week 4 压测要用
}

size_t LRUCache::TotalCharge() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return usage_;
}

uint64_t LRUCache::Hits() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hits_;
}

uint64_t LRUCache::Misses() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return misses_;
}

}  // namespace kv
