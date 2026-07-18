#include "memtable.h"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <cstring>

#ifdef KV_DEBUG
    #define DEBUG_LOG(fmt, ...) printf("[DEBUG] " fmt "\n", ##__VA_ARGS__)
#else
    #define DEBUG_LOG(fmt, ...) ((void)0)
#endif

namespace kv {

// ============ SkipList 实现 ============

size_t SkipList::NodeSize(int height) {
    size_t base = sizeof(SkipListNode);
    size_t extra = sizeof(std::atomic<SkipListNode*>) * (height - 1);
    return base + extra;
}

SkipListNode* SkipList::NewNode(const std::string& key, 
                                  const std::string& value, 
                                  int height) 
{
    size_t size = NodeSize(height);
    // 使用 ::operator new 保证 max_align_t 对齐，适用于含 atomic/string 的节点
    char* mem = static_cast<char*>(::operator new(size));

    auto* node = new (mem) SkipListNode();

    // key/value 非 const，直接赋值即可
    node->key = key;
    node->value = value;

    node->height = height;

    for (int i = 0; i < height; ++i) {
        node->next_[i].store(nullptr, std::memory_order_release);
    }

    return node;
}

SkipList::SkipList() 
    : head_(NewNode("", "", kMaxHeight))
    , max_height_(1)
    , rnd_(std::random_device{}())
    , memory_usage_(NodeSize(kMaxHeight))
{
}

SkipList::~SkipList() {
    auto* node = head_->next_[0].load(std::memory_order_relaxed);
    while (node != nullptr) {
        auto* next = node->next_[0].load(std::memory_order_relaxed);
        node->~SkipListNode();
        ::operator delete(reinterpret_cast<void*>(node));
        node = next;
    }
    head_->~SkipListNode();
    ::operator delete(reinterpret_cast<void*>(head_));
}

int SkipList::RandomHeight() {
    std::uniform_int_distribution<int> dist(0, kBranching - 1);

    int height = 1;
    while (height < kMaxHeight && dist(rnd_) == 0) {
        height++;
    }
    return height;
}

void SkipList::FindPrevNodes(const Slice& key, SkipListNode** prev) const {
    auto* node = head_;

    for (int level = max_height_.load(std::memory_order_acquire) - 1; level >= 0; --level) {
        auto* next = node->next_[level].load(std::memory_order_acquire);

        while (next != nullptr && KeyIsLess(next->key, key)) {
            node = next;
            next = node->next_[level].load(std::memory_order_acquire);
        }

        prev[level] = node;
    }
}

void SkipList::Insert(const std::string& key, const std::string& value) {
    Slice key_slice(key);

    SkipListNode* prev[kMaxHeight];
    FindPrevNodes(key_slice, prev);

    auto* existing = prev[0]->next_[0].load(std::memory_order_relaxed);
    if (existing != nullptr && existing->key == key) {
        existing->value = value;
        DEBUG_LOG("SkipList::Insert: overwrite key=%s", key.c_str());
        return;
    }

    int height = RandomHeight();
    int cur_max = max_height_.load(std::memory_order_relaxed);
    if (height > cur_max) {
        for (int i = cur_max; i < height; ++i) {
            prev[i] = head_;
        }
        max_height_.store(height, std::memory_order_release);
    }

    auto* new_node = NewNode(key, value, height);

    // 内存占用：节点结构 + key/value 数据（跨平台保守估算，不再硬编码 SSO 阈值）
    memory_usage_.fetch_add(NodeSize(height) + key.size() + value.size(), 
                            std::memory_order_relaxed);

    for (int i = 0; i < height; ++i) {
        new_node->next_[i].store(prev[i]->next_[i].load(std::memory_order_relaxed),
                                  std::memory_order_release);
        prev[i]->next_[i].store(new_node, std::memory_order_release);
    }

    DEBUG_LOG("SkipList::Insert: key=%s, height=%d", key.c_str(), height);
}

bool SkipList::Contains(const Slice& key, std::string* value) const {
    auto* node = head_;

    for (int level = max_height_.load(std::memory_order_acquire) - 1; level >= 0; --level) {
        auto* next = node->next_[level].load(std::memory_order_acquire);

        while (next != nullptr && KeyIsLess(next->key, key)) {
            node = next;
            next = node->next_[level].load(std::memory_order_acquire);
        }
    }

    auto* target = node->next_[0].load(std::memory_order_acquire);
    if (target != nullptr && target->key == key.ToString()) {
        if (value != nullptr) {
            *value = target->value;
        }
        return true;
    }
    return false;
}

size_t SkipList::ApproximateMemoryUsage() const {
    return memory_usage_.load(std::memory_order_relaxed);
}

bool SkipList::KeyIsLess(const Slice& a, const Slice& b) {
    return a.compare(b) < 0;
}

// ============ Iterator 实现 ============

SkipList::Iterator::Iterator(const SkipList* list)
    : list_(list), node_(nullptr) {}

void SkipList::Iterator::Seek(const Slice& target) {
    auto* current = list_->head_;

    for (int level = list_->max_height_.load(std::memory_order_acquire) - 1; 
         level >= 0; --level) {
        auto* next = current->next_[level].load(std::memory_order_acquire);

        while (next != nullptr && KeyIsLess(next->key, target)) {
            current = next;
            next = current->next_[level].load(std::memory_order_acquire);
        }
    }

    node_ = current->next_[0].load(std::memory_order_acquire);
}

void SkipList::Iterator::SeekToFirst() {
    node_ = list_->head_->next_[0].load(std::memory_order_acquire);
}

void SkipList::Iterator::Next() {
    assert(node_ != nullptr);
    node_ = node_->next_[0].load(std::memory_order_acquire);
}

bool SkipList::Iterator::Valid() const {
    return node_ != nullptr;
}

Slice SkipList::Iterator::key() const {
    assert(Valid());
    return Slice(node_->key);
}

Slice SkipList::Iterator::value() const {
    assert(Valid());
    return Slice(node_->value);
}

} // namespace kv