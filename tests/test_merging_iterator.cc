#include <iostream>
#include <cassert>
#include <memory>
#include "merging_iterator.h"
#include "memtable.h"

using namespace kv;

void TestBasicMerge() {
    std::cout << "=== TestBasicMerge ===" << std::endl;

    SkipList list1;
    list1.Insert("a", "1");
    list1.Insert("c", "3");
    list1.Insert("e", "5");

    SkipList list2;
    list2.Insert("b", "2");
    list2.Insert("d", "4");
    list2.Insert("f", "6");

    std::vector<std::unique_ptr<Iterator>> iters;
    iters.emplace_back(new SkipList::Iterator(&list1));
    iters.emplace_back(new SkipList::Iterator(&list2));

    MergingIterator merge(std::move(iters));
    merge.SeekToFirst();

    assert(merge.Valid() && merge.key() == "a" && merge.value() == "1");
    merge.Next();
    assert(merge.Valid() && merge.key() == "b" && merge.value() == "2");
    merge.Next();
    assert(merge.Valid() && merge.key() == "c" && merge.value() == "3");
    merge.Next();
    assert(merge.Valid() && merge.key() == "d" && merge.value() == "4");
    merge.Next();
    assert(merge.Valid() && merge.key() == "e" && merge.value() == "5");
    merge.Next();
    assert(merge.Valid() && merge.key() == "f" && merge.value() == "6");
    merge.Next();
    assert(!merge.Valid());

    std::cout << "  PASS: basic merge" << std::endl;
}

void TestMergeWithOverlap() {
    std::cout << "=== TestMergeWithOverlap ===" << std::endl;

    // 两个数据源有重叠 key（模拟 MemTable 覆盖旧 SSTable 的场景）
    SkipList list1;
    list1.Insert("a", "1");
    list1.Insert("b", "old");

    SkipList list2;
    list2.Insert("b", "new");
    list2.Insert("c", "3");

    std::vector<std::unique_ptr<Iterator>> iters;
    iters.emplace_back(new SkipList::Iterator(&list1));
    iters.emplace_back(new SkipList::Iterator(&list2));

    MergingIterator merge(std::move(iters));
    merge.SeekToFirst();

    // 归并后所有 key 都出现（包括重复的 b），去重由上层（Compaction）处理
    int count = 0;
    while (merge.Valid()) {
        count++;
        merge.Next();
    }
    assert(count == 4);  // a, b(old), b(new), c

    // 测试 Seek
    iters.clear();
    SkipList list3;
    list3.Insert("a", "1");
    list3.Insert("c", "3");
    SkipList list4;
    list4.Insert("b", "2");
    list4.Insert("d", "4");

    iters.emplace_back(new SkipList::Iterator(&list3));
    iters.emplace_back(new SkipList::Iterator(&list4));

    MergingIterator merge2(std::move(iters));
    merge2.Seek("c");
    assert(merge2.Valid() && merge2.key() == "c");

    std::cout << "  PASS: merge with overlap" << std::endl;
}

void TestEmptyMerge() {
    std::cout << "=== TestEmptyMerge ===" << std::endl;

    std::vector<std::unique_ptr<Iterator>> iters;
    MergingIterator merge(std::move(iters));
    merge.SeekToFirst();
    assert(!merge.Valid());

    merge.Seek("anything");
    assert(!merge.Valid());

    std::cout << "  PASS: empty merge" << std::endl;
}

void TestSingleSource() {
    std::cout << "=== TestSingleSource ===" << std::endl;

    SkipList list;
    list.Insert("x", "10");
    list.Insert("y", "20");

    std::vector<std::unique_ptr<Iterator>> iters;
    iters.emplace_back(new SkipList::Iterator(&list));

    MergingIterator merge(std::move(iters));
    merge.SeekToFirst();
    assert(merge.Valid() && merge.key() == "x");
    merge.Next();
    assert(merge.Valid() && merge.key() == "y");
    merge.Next();
    assert(!merge.Valid());

    std::cout << "  PASS: single source" << std::endl;
}

int main() {
    std::cout << "MergingIterator Tests Starting..." << std::endl;
    TestBasicMerge();
    TestMergeWithOverlap();
    TestEmptyMerge();
    TestSingleSource();
    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}