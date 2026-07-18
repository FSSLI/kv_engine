#pragma once

#include <atomic>
#include <cstdlib>
#include <string>
#include <vector>
#include <random>
#include <cstring>
#include "slice.h"
#include "iterator.h"
#include "status.h"

namespace kv {

// ============ Slice 已移到 slice.h ============

// 跳表节点
struct SkipListNode {
    std::string key;
    std::string value;
    int height;
    std::atomic<SkipListNode*> next_[1];
    SkipListNode() : height(0) {}
    SkipListNode(const SkipListNode&) = delete;
    SkipListNode& operator=(const SkipListNode&) = delete;
};

// 跳表
class SkipList {
public:
    static const int kMaxHeight = 12;
    static const int kBranching = 4;

    SkipList();
    ~SkipList();

    void Insert(const std::string& key, const std::string& value);
    bool Contains(const Slice& key, std::string* value) const;
    size_t ApproximateMemoryUsage() const;

    // 嵌套类直接在类内声明继承 ::kv::Iterator
    // 注意：::kv::Iterator 明确指定全局命名空间下的 Iterator 基类
    class Iterator : public ::kv::Iterator {
    public:
        explicit Iterator(const SkipList* list);
        void Seek(const Slice& target) override;
        void SeekToFirst() override;
        void Next() override;
        bool Valid() const override;
        Slice key() const override;
        Slice value() const override;
    private:
        const SkipList* list_;
        SkipListNode* node_;
    };

private:
    static size_t NodeSize(int height);
    static SkipListNode* NewNode(const std::string& key, 
                                  const std::string& value, 
                                  int height);
    int RandomHeight();
    void FindPrevNodes(const Slice& key, SkipListNode** prev) const;
    static bool KeyIsLess(const Slice& a, const Slice& b);

    SkipListNode* head_;
    std::atomic<int> max_height_;
    std::mt19937 rnd_;
    std::atomic<size_t> memory_usage_;
};

} // namespace kv