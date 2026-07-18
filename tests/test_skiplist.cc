#include <iostream>
#include <thread>
#include <vector>
#include <cassert>
#include <chrono>
#include <random>
#include "memtable.h"

using namespace kv;

// 测试 1：基本插入和查询
void TestBasic() {
    std::cout << "=== TestBasic ===" << std::endl;

    SkipList list;

    // 插入
    list.Insert("apple", "1");
    list.Insert("banana", "2");
    list.Insert("cherry", "3");

    // 查询
    std::string value;
    assert(list.Contains("apple", &value) && value == "1");
    assert(list.Contains("banana", &value) && value == "2");
    assert(list.Contains("cherry", &value) && value == "3");
    assert(!list.Contains("date", &value));

    std::cout << "  PASS: basic insert/query" << std::endl;
}

// 测试 2：覆盖写入
void TestOverwrite() {
    std::cout << "=== TestOverwrite ===" << std::endl;

    SkipList list;
    list.Insert("key1", "old");
    list.Insert("key1", "new");

    std::string value;
    assert(list.Contains("key1", &value) && value == "new");

    std::cout << "  PASS: overwrite" << std::endl;
}

// 测试 3：迭代器（使用新的 Slice 接口）
void TestIterator() {
    std::cout << "=== TestIterator ===" << std::endl;

    SkipList list;
    list.Insert("a", "1");
    list.Insert("c", "3");
    list.Insert("b", "2");

    SkipList::Iterator iter(&list);
    iter.SeekToFirst();

    assert(iter.Valid());
    // Slice 支持 operator==，"a" 会隐式转为 Slice(const char*)
    assert(iter.key() == "a" && iter.value() == "1");
    iter.Next();
    assert(iter.Valid());
    assert(iter.key() == "b" && iter.value() == "2");
    iter.Next();
    assert(iter.Valid());
    assert(iter.key() == "c" && iter.value() == "3");
    iter.Next();
    assert(!iter.Valid());

    // 测试 Seek
    iter.Seek("b");
    assert(iter.Valid() && iter.key() == "b");

    iter.Seek("d");
    assert(!iter.Valid());

    // 测试 Seek 到不存在的 key（应定位到第一个 >= target 的）
    iter.Seek("bb");
    assert(iter.Valid() && iter.key() == "c");

    std::cout << "  PASS: iterator" << std::endl;
}

// 测试 4：并发读（单线程写完后多线程读）
void TestConcurrentRead() {
    std::cout << "=== TestConcurrentRead ===" << std::endl;

    SkipList list;
    const int N = 10000;

    // 单线程写入
    for (int i = 0; i < N; ++i) {
        list.Insert(std::to_string(i), std::to_string(i * i));
    }

    // 多线程并发读
    const int num_threads = 4;
    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&list, N, t]() {
            std::string value;
            for (int i = 0; i < N; ++i) {
                assert(list.Contains(std::to_string(i), &value));
                assert(value == std::to_string(i * i));
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    std::cout << "  PASS: concurrent read (" << num_threads << " threads x " << N << " queries)" << std::endl;
}

// 测试 5：并发读迭代器
void TestConcurrentIterator() {
    std::cout << "=== TestConcurrentIterator ===" << std::endl;

    SkipList list;
    const int N = 1000;

    for (int i = 0; i < N; ++i) {
        list.Insert(std::to_string(i), std::to_string(i));
    }

    const int num_threads = 4;
    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&list, N]() {
            SkipList::Iterator iter(&list);
            iter.SeekToFirst();
            int count = 0;
            while (iter.Valid()) {
                count++;
                iter.Next();
            }
            assert(count == N);
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    std::cout << "  PASS: concurrent iterator (" << num_threads << " threads)" << std::endl;
}

// 测试 6：覆盖写入后迭代器顺序
void TestOverwriteIteratorOrder() {
    std::cout << "=== TestOverwriteIteratorOrder ===" << std::endl;

    SkipList list;
    list.Insert("a", "1");
    list.Insert("c", "3");
    list.Insert("b", "2");
    list.Insert("a", "10");  // 覆盖
    list.Insert("b", "20"); // 覆盖

    SkipList::Iterator iter(&list);
    iter.SeekToFirst();

    assert(iter.Valid() && iter.key() == "a" && iter.value() == "10");
    iter.Next();
    assert(iter.Valid() && iter.key() == "b" && iter.value() == "20");
    iter.Next();
    assert(iter.Valid() && iter.key() == "c" && iter.value() == "3");
    iter.Next();
    assert(!iter.Valid());

    std::cout << "  PASS: overwrite iterator order" << std::endl;
}

// 测试 7：空跳表行为
void TestEmptySkipList() {
    std::cout << "=== TestEmptySkipList ===" << std::endl;

    SkipList list;

    std::string value;
    assert(!list.Contains("anything", &value));

    SkipList::Iterator iter(&list);
    iter.SeekToFirst();
    assert(!iter.Valid());

    iter.Seek("anything");
    assert(!iter.Valid());

    assert(list.ApproximateMemoryUsage() > 0);  // 至少有 head_ 节点

    std::cout << "  PASS: empty skiplist" << std::endl;
}

// 测试 8：性能基准
void TestPerformance() {
    std::cout << "=== TestPerformance ===" << std::endl;

    SkipList list;
    const int N = 100000;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < N; ++i) {
        list.Insert(std::to_string(i), std::string(100, 'x'));
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "  Insert " << N << " items: " << ms << " ms (" 
              << (N * 1000.0 / ms) << " ops/sec)" << std::endl;

    // 查询性能
    start = std::chrono::high_resolution_clock::now();

    std::string value;
    for (int i = 0; i < N; ++i) {
        list.Contains(std::to_string(i), &value);
    }

    end = std::chrono::high_resolution_clock::now();
    ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "  Query " << N << " items: " << ms << " ms (" 
              << (N * 1000.0 / ms) << " ops/sec)" << std::endl;

    // 迭代器性能
    start = std::chrono::high_resolution_clock::now();

    SkipList::Iterator iter(&list);
    iter.SeekToFirst();
    int count = 0;
    while (iter.Valid()) {
        count++;
        iter.Next();
    }

    end = std::chrono::high_resolution_clock::now();
    ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "  Iterate " << N << " items: " << ms << " ms (" 
              << (N * 1000.0 / ms) << " ops/sec)" << std::endl;

    std::cout << "  Approximate memory: " << list.ApproximateMemoryUsage() / 1024 << " KB" << std::endl;

    assert(count == N);
    std::cout << "  PASS: performance" << std::endl;
}

// 测试 9：大量数据（100万条）
void TestLargeData() {
    std::cout << "=== TestLargeData ===" << std::endl;

    SkipList list;
    const int N = 1000000;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < N; ++i) {
        list.Insert(std::to_string(i), std::to_string(i));
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "  Insert " << N << " items: " << ms << " ms (" 
              << (N * 1000.0 / ms) << " ops/sec)" << std::endl;

    // 随机查询
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, N - 1);

    start = std::chrono::high_resolution_clock::now();

    std::string value;
    for (int i = 0; i < 100000; ++i) {
        int key = dist(rng);
        assert(list.Contains(std::to_string(key), &value));
        assert(value == std::to_string(key));
    }

    end = std::chrono::high_resolution_clock::now();
    ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "  Random query 100k items: " << ms << " ms (" 
              << (100000 * 1000.0 / ms) << " ops/sec)" << std::endl;

    std::cout << "  Approximate memory: " << list.ApproximateMemoryUsage() / 1024 / 1024 << " MB" << std::endl;

    std::cout << "  PASS: large data" << std::endl;
}

int main() {
    std::cout << "SkipList Tests Starting..." << std::endl;

    TestBasic();
    TestOverwrite();
    TestIterator();
    TestConcurrentRead();
    TestConcurrentIterator();
    TestOverwriteIteratorOrder();
    TestEmptySkipList();
    TestPerformance();
    TestLargeData();

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}