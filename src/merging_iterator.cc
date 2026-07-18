#include "merging_iterator.h"
#include <cassert>

namespace kv {

MergingIterator::MergingIterator(std::vector<std::unique_ptr<Iterator>> iterators)
    : iterators_(std::move(iterators)), valid_(false) {}

void MergingIterator::SeekToFirst() {
    heap_ = decltype(heap_)();  // 清空堆
    for (size_t i = 0; i < iterators_.size(); ++i) {
        iterators_[i]->SeekToFirst();
        PushIfValid(static_cast<int>(i));
    }
    valid_ = !heap_.empty();
}

void MergingIterator::Seek(const Slice& target) {
    heap_ = decltype(heap_)();
    for (size_t i = 0; i < iterators_.size(); ++i) {
        iterators_[i]->Seek(target);
        PushIfValid(static_cast<int>(i));
    }
    valid_ = !heap_.empty();
}

void MergingIterator::Next() {
    if (!valid_) return;

    int index = heap_.top().index;
    heap_.pop();

    iterators_[index]->Next();
    PushIfValid(index);

    valid_ = !heap_.empty();
}

bool MergingIterator::Valid() const {
    return valid_;
}

Slice MergingIterator::key() const {
    assert(valid_);
    return Slice(heap_.top().key);
}

Slice MergingIterator::value() const {
    assert(valid_);
    return iterators_[heap_.top().index]->value();
}

void MergingIterator::PushIfValid(int index) {
    if (iterators_[index]->Valid()) {
        HeapItem item;
        item.index = index;
        item.key = iterators_[index]->key().ToString();  // 拷贝，避免跨 Block 悬空
        heap_.push(std::move(item));
    }
}

} // namespace kv