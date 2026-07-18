#include <iostream>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
#include <chrono>
#include <random>
#include <unistd.h>
#include <sys/stat.h>
#include "sstable.h"

using namespace kv;

const std::string kTestDir = "/tmp/kv_test";
const std::string kTestFile = kTestDir + "/000001.sst";

// 清理测试环境
void CleanUp() {
    std::string cmd = "rm -rf " + kTestDir + " && mkdir -p " + kTestDir;
    int ret = system(cmd.c_str());
    (void)ret;
}

// 测试 1：基本构建和读取
void TestBasicBuildRead() {
    std::cout << "=== TestBasicBuildRead ===" << std::endl;
    CleanUp();

    // 构建 SSTable
    {
        SSTableBuilder builder(kTestFile, 4096);

        builder.Add("apple", "1");
        builder.Add("banana", "2");
        builder.Add("cherry", "3");

        uint64_t file_size;
        Status s = builder.Finish(&file_size);
        assert(s.ok());
        assert(file_size > 0);

        std::cout << "  Built SSTable, size=" << file_size << std::endl;
    }

    // 读取 SSTable
    {
        std::unique_ptr<SSTable> table;
        Status s = SSTable::Open(kTestFile, &table);
        assert(s.ok());

        std::string value;
        s = table->Get("apple", &value);
        assert(s.ok() && value == "1");

        s = table->Get("banana", &value);
        assert(s.ok() && value == "2");

        s = table->Get("cherry", &value);
        assert(s.ok() && value == "3");

        s = table->Get("date", &value);
        assert(s.IsNotFound());

        std::cout << "  PASS: basic build and read" << std::endl;
    }
}

// 测试 2：大量数据，触发多 block
void TestMultipleBlocks() {
    std::cout << "=== TestMultipleBlocks ===" << std::endl;
    CleanUp();

    const int N = 10000;
    const size_t kBlockSize = 1024;  // 小 block 大小，强制产生多个 block

    // 构建
    {
        SSTableBuilder builder(kTestFile, kBlockSize);

        for (int i = 0; i < N; ++i) {
            char key_buf[32], value_buf[32];
            snprintf(key_buf, sizeof(key_buf), "key%04d", i);
            snprintf(value_buf, sizeof(value_buf), "value%04d", i);
            std::string key = key_buf;
            std::string value = value_buf;
            Status s = builder.Add(key, value);
            assert(s.ok());
        }

        uint64_t file_size;
        Status s = builder.Finish(&file_size);
        assert(s.ok());

        std::cout << "  Built " << N << " entries, file_size=" << file_size << std::endl;
    }

    // 读取验证
    {
        std::unique_ptr<SSTable> table;
        Status s = SSTable::Open(kTestFile, &table);
        assert(s.ok());

        // 随机查询
        for (int i = 0; i < N; i += 100) {
            char key_buf[32], value_buf[32];
            snprintf(key_buf, sizeof(key_buf), "key%04d", i);
            snprintf(value_buf, sizeof(value_buf), "value%04d", i);
            std::string key = key_buf;
            std::string expected = value_buf;
            std::string value;
            s = table->Get(key, &value);
            if (!s.ok() || value != expected) {
                std::cerr << "FAIL: key=" << key << " expected=" << expected 
                        << " got_status=" << s.ToString() << " got_value=" << value << std::endl;
            }
            assert(s.ok() && value == expected);
        }

        // 查询不存在的 key
        std::string value;
        s = table->Get("notexist", &value);
        assert(s.IsNotFound());

        std::cout << "  PASS: multiple blocks (" << N << " entries)" << std::endl;
    }
}

// 测试 3：迭代器遍历
void TestIterator() {
    std::cout << "=== TestIterator ===" << std::endl;
    CleanUp();

    const int N = 1000;

    // 构建
    {
        SSTableBuilder builder(kTestFile, 4096);
        for (int i = 0; i < N; ++i) {
            char key_buf[32], value_buf[32];
            snprintf(key_buf, sizeof(key_buf), "%04d", i);
            snprintf(value_buf, sizeof(value_buf), "%04d", i * i);
            builder.Add(key_buf, value_buf);
        }
        uint64_t file_size;
        Status s = builder.Finish(&file_size);
        assert(s.ok());
    }

    // 迭代器遍历
    {
        std::unique_ptr<SSTable> table;
        Status s = SSTable::Open(kTestFile, &table);
        assert(s.ok());

        auto iter = table->NewIterator();
        iter->SeekToFirst();

        int count = 0;
        while (iter->Valid()) {
            std::string key = iter->key().ToString();
            std::string value = iter->value().ToString();

            // 修改后
            char expected_key[32], expected_value[32];
            snprintf(expected_key, sizeof(expected_key), "%04d", count);
            snprintf(expected_value, sizeof(expected_value), "%04d", count * count);
            assert(key == expected_key);
            assert(value == expected_value);

            count++;
            iter->Next();
        }

        assert(count == N);
        std::cout << "  PASS: iterator (" << count << " entries)" << std::endl;
    }
}

// 测试 4：迭代器 Seek
void TestIteratorSeek() {
    std::cout << "=== TestIteratorSeek ===" << std::endl;
    CleanUp();

    // 构建
    {
        SSTableBuilder builder(kTestFile, 1024);
        builder.Add("a", "1");
        builder.Add("c", "3");
        builder.Add("e", "5");
        builder.Add("g", "7");
        builder.Add("i", "9");
        uint64_t file_size;
        Status s = builder.Finish(&file_size);
        assert(s.ok());
    }

    // Seek 测试
    {
        std::unique_ptr<SSTable> table;
        Status s = SSTable::Open(kTestFile, &table);
        assert(s.ok());

        auto iter = table->NewIterator();

        // Seek 到存在的 key
        iter->Seek("e");
        assert(iter->Valid());
        assert(iter->key() == "e" && iter->value() == "5");

        // Seek 到不存在的 key（应定位到第一个 >= target）
        iter->Seek("d");
        assert(iter->Valid());
        assert(iter->key() == "e" && iter->value() == "5");

        iter->Seek("f");
        assert(iter->Valid());
        assert(iter->key() == "g" && iter->value() == "7");

        // Seek 到比所有 key 都大的
        iter->Seek("z");
        assert(!iter->Valid());

        // Seek 到比所有 key 都小的
        iter->Seek("0");
        assert(iter->Valid());
        assert(iter->key() == "a");

        std::cout << "  PASS: iterator seek" << std::endl;
    }
}

// 测试 5：TableCache
void TestTableCache() {
    std::cout << "=== TestTableCache ===" << std::endl;
    CleanUp();

    // 构建文件
    {
        SSTableBuilder builder(kTestFile, 4096);
        builder.Add("key1", "value1");
        builder.Add("key2", "value2");
        uint64_t file_size;
        Status s = builder.Finish(&file_size);
        assert(s.ok());
    }

    TableCache cache;

    // 第一次查找（cache miss）
    std::shared_ptr<SSTable> table1;
    Status s = cache.FindTable(1, kTestDir, &table1);
    assert(s.ok());

    std::string value;
    s = table1->Get("key1", &value);
    assert(s.ok() && value == "value1");

    // 第二次查找（cache hit）
    std::shared_ptr<SSTable> table2;
    s = cache.FindTable(1, kTestDir, &table2);
    assert(s.ok());
    assert(table1.get() == table2.get());  // 同一个对象

    // 释放所有引用后，再查找（应该重新加载）
    table1.reset();
    table2.reset();

    std::shared_ptr<SSTable> table3;
    s = cache.FindTable(1, kTestDir, &table3);
    assert(s.ok());

    s = table3->Get("key2", &value);
    assert(s.ok() && value == "value2");

    std::cout << "  PASS: table cache" << std::endl;
}

// 测试 6：元数据（smallest/largest key）
void TestMetadata() {
    std::cout << "=== TestMetadata ===" << std::endl;
    CleanUp();

    {
        SSTableBuilder builder(kTestFile, 4096);
        builder.Add("alpha", "1");
        builder.Add("beta", "2");
        builder.Add("gamma", "3");
        builder.Add("zeta", "4");
        uint64_t file_size;
        Status s = builder.Finish(&file_size);
        assert(s.ok());
    }

    {
        std::unique_ptr<SSTable> table;
        Status s = SSTable::Open(kTestFile, &table);
        assert(s.ok());

        assert(table->SmallestKey() == "alpha");
        assert(table->LargestKey() == "zeta");

        std::cout << "  PASS: metadata (smallest=" << table->SmallestKey().ToString()
                  << ", largest=" << table->LargestKey().ToString() << ")" << std::endl;
    }
}

// 测试 7：损坏文件检测
void TestCorruption() {
    std::cout << "=== TestCorruption ===" << std::endl;
    CleanUp();

    // 创建一个有效的 SSTable
    {
        SSTableBuilder builder(kTestFile, 4096);
        builder.Add("key", "value");
        uint64_t file_size;
        Status s = builder.Finish(&file_size);
        assert(s.ok());
    }

    // 篡改文件尾部（Footer）
    {
        FILE* f = fopen(kTestFile.c_str(), "r+b");
        assert(f);
        fseek(f, -8, SEEK_END);  // 定位到 magic number
        // 使用 static_cast<char> 避免窄化转换
        char bad_magic[8] = {
            static_cast<char>(0xFF), static_cast<char>(0xFF),
            static_cast<char>(0xFF), static_cast<char>(0xFF),
            static_cast<char>(0xFF), static_cast<char>(0xFF),
            static_cast<char>(0xFF), static_cast<char>(0xFF)
        };
        fwrite(bad_magic, 1, 8, f);
        fclose(f);

        std::unique_ptr<SSTable> table;
        Status s = SSTable::Open(kTestFile, &table);
        assert(s.IsCorruption());
        std::cout << "  PASS: corruption detected (bad magic)" << std::endl;
    }

    // 截断文件
    {
        CleanUp();
        {
            SSTableBuilder builder(kTestFile, 4096);
            builder.Add("key", "value");
            uint64_t file_size;
            Status s = builder.Finish(&file_size);
            assert(s.ok());
        }

        // 截断到只剩 10 字节
        FILE* f = fopen(kTestFile.c_str(), "r+b");
        assert(f);
        int ret = ftruncate(fileno(f), 10);
        (void)ret;
        fclose(f);

        std::unique_ptr<SSTable> table;
        Status s = SSTable::Open(kTestFile, &table);
        assert(s.IsCorruption());
        std::cout << "  PASS: corruption detected (truncated)" << std::endl;
    }
}

// 测试 8：空 SSTable（没有条目）
void TestEmptySSTable() {
    std::cout << "=== TestEmptySSTable ===" << std::endl;
    CleanUp();

    {
        SSTableBuilder builder(kTestFile, 4096);
        uint64_t file_size;
        Status s = builder.Finish(&file_size);
        assert(s.ok());
    }

    {
        std::unique_ptr<SSTable> table;
        Status s = SSTable::Open(kTestFile, &table);
        assert(s.ok());

        auto iter = table->NewIterator();
        iter->SeekToFirst();
        assert(!iter->Valid());

        std::string value;
        s = table->Get("anything", &value);
        assert(s.IsNotFound());

        std::cout << "  PASS: empty sstable" << std::endl;
    }
}

// 测试 9：性能基准
void TestPerformance() {
    std::cout << "=== TestPerformance ===" << std::endl;
    CleanUp();

    const int N = 100000;

    // 构建
    auto start = std::chrono::high_resolution_clock::now();
    {
        SSTableBuilder builder(kTestFile, 4096);
        for (int i = 0; i < N; ++i) {
            char key_buf[32];
            snprintf(key_buf, sizeof(key_buf), "%010d", i);  // 10位宽度保证字典序
            builder.Add(key_buf, std::string(100, 'x'));
        }
        uint64_t file_size;
        Status s = builder.Finish(&file_size);
        assert(s.ok());
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "  Build " << N << " entries: " << ms << " ms (" 
              << (N * 1000.0 / ms) << " ops/sec)" << std::endl;

    // 打开
    start = std::chrono::high_resolution_clock::now();
    std::unique_ptr<SSTable> table;
    Status s = SSTable::Open(kTestFile, &table);
    assert(s.ok());
    end = std::chrono::high_resolution_clock::now();
    ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "  Open: " << ms << " ms" << std::endl;

    // 随机查询
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, N - 1);

    start = std::chrono::high_resolution_clock::now();
    std::string value;
    for (int i = 0; i < 10000; ++i) {
        int key = dist(rng);
        char key_buf[32];
        snprintf(key_buf, sizeof(key_buf), "%010d", key);
        table->Get(key_buf, &value);
    }
    end = std::chrono::high_resolution_clock::now();
    ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "  Random query 10k: " << ms << " ms (" 
              << (10000 * 1000.0 / ms) << " ops/sec)" << std::endl;

    // 迭代器遍历
    start = std::chrono::high_resolution_clock::now();
    auto iter = table->NewIterator();
    iter->SeekToFirst();
    int count = 0;
    while (iter->Valid()) {
        count++;
        iter->Next();
    }
    end = std::chrono::high_resolution_clock::now();
    ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "  Iterate " << count << " entries: " << ms << " ms (" 
              << (count * 1000.0 / ms) << " ops/sec)" << std::endl;

    std::cout << "  PASS: performance" << std::endl;
}

int main() {
    std::cout << "SSTable Tests Starting..." << std::endl;

    TestBasicBuildRead();
    TestMultipleBlocks();
    TestIterator();
    TestIteratorSeek();
    TestTableCache();
    TestMetadata();
    TestCorruption();
    TestEmptySSTable();
    TestPerformance();

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}