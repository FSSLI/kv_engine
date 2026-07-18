#pragma once

#include <memory>
#include <queue>
#include <string>
#include <vector>
#include "iterator.h"

namespace kv {

// 多路归并迭代器：同时遍历多个有序数据源，输出全局有序序列
// 典型场景：MemTable + 多级 SSTable 的 Scan / Compaction
class MergingIterator : public Iterator {
public:
    explicit MergingIterator(std::vector<std::unique_ptr<Iterator>> iterators);

    void SeekToFirst() override;
    void Seek(const Slice& target) override;
    void Next() override;
    bool Valid() const override;
    Slice key() const override;
    Slice value() const override;

private:
    struct HeapItem {
        int index;          // 对应 iterators_ 的索引
        std::string key;    // 拷贝 key，避免 Slice 悬空（SSTable 跨 Block 时）
    };

    struct Compare {
        bool operator()(const HeapItem& a, const HeapItem& b) const {
            int cmp = Slice(a.key).compare(Slice(b.key));
            if (cmp != 0) {
                // 最小堆：key 字典序大的优先级低（沉底）
                return cmp > 0;
            }
            // key 相同时，index 小的优先级更高（L0 的 iterator 先加入，index 更小）
            return a.index > b.index;
        }
    };

    void PushIfValid(int index);

    std::vector<std::unique_ptr<Iterator>> iterators_;
    std::priority_queue<HeapItem, std::vector<HeapItem>, Compare> heap_;
    bool valid_;
};

} // namespace kv