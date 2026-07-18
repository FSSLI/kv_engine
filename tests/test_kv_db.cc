#include <iostream>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include "kv_db.h"

using namespace kv;

const std::string kTestDir = "/tmp/kv_db_test";

void CleanUp() {
    std::string cmd = "rm -rf " + kTestDir;
    int ret = system(cmd.c_str());
    (void)ret;
}

void TestBasicPutGet() {
    std::cout << "=== TestBasicPutGet ===" << std::endl;
    CleanUp();

    KVDB::Options options;
    options.dbname = kTestDir;
    options.write_buffer_size = 1024 * 1024;

    std::unique_ptr<KVDB> db;
    Status s = KVDB::Open(options, &db);
    assert(s.ok());

    s = db->Put("hello", "world");
    assert(s.ok());

    std::string value;
    s = db->Get("hello", &value);
    assert(s.ok() && value == "world");

    s = db->Get("notexist", &value);
    assert(s.IsNotFound());

    std::cout << "  PASS: basic put/get" << std::endl;
}

void TestDelete() {
    std::cout << "=== TestDelete ===" << std::endl;
    CleanUp();

    KVDB::Options options;
    options.dbname = kTestDir;
    options.write_buffer_size = 1024 * 1024;

    std::unique_ptr<KVDB> db;
    Status s = KVDB::Open(options, &db);
    assert(s.ok());

    s = db->Put("key1", "value1");
    assert(s.ok());

    s = db->Delete("key1");
    assert(s.ok());

    std::string value;
    s = db->Get("key1", &value);
    assert(s.IsNotFound());

    std::cout << "  PASS: delete" << std::endl;
}

void TestFlushAndReadSSTable() {
    std::cout << "=== TestFlushAndReadSSTable ===" << std::endl;
    CleanUp();

    KVDB::Options options;
    options.dbname = kTestDir;
    options.write_buffer_size = 1024 * 1024;

    std::unique_ptr<KVDB> db;
    Status s = KVDB::Open(options, &db);
    assert(s.ok());

    // 写入数据
    for (int i = 0; i < 100; ++i) {
        char key[32], value[32];
        snprintf(key, sizeof(key), "key%04d", i);
        snprintf(value, sizeof(value), "value%04d", i);
        s = db->Put(key, value);
        assert(s.ok());
    }

    // 手动 flush 到 SSTable
    s = db->TEST_FlushMemTable();
    assert(s.ok());

    // 从 SSTable 读取（MemTable 已清空）
    for (int i = 0; i < 100; ++i) {
        char key[32], expected[32];
        snprintf(key, sizeof(key), "key%04d", i);
        snprintf(expected, sizeof(expected), "value%04d", i);
        std::string value;
        s = db->Get(key, &value);
        assert(s.ok() && value == expected);
    }

    std::cout << "  PASS: flush and read from sstable" << std::endl;
}

void TestAutoFlush() {
    std::cout << "=== TestAutoFlush ===" << std::endl;
    CleanUp();

    KVDB::Options options;
    options.dbname = kTestDir;
    options.write_buffer_size = 512;  // 512 bytes，容易触发自动切换

    std::unique_ptr<KVDB> db;
    Status s = KVDB::Open(options, &db);
    assert(s.ok());

    // 写入大量数据，触发自动 MemTable 切换
    for (int i = 0; i < 500; ++i) {
        char key[32], value[256];
        snprintf(key, sizeof(key), "key%04d", i);
        memset(value, 'x', sizeof(value) - 1);
        value[sizeof(value) - 1] = '\0';
        s = db->Put(key, value);
        assert(s.ok());
    }

    // 验证所有数据可读（部分在 MemTable，部分在 SSTable）
    for (int i = 0; i < 500; ++i) {
        char key[32];
        snprintf(key, sizeof(key), "key%04d", i);
        std::string value;
        s = db->Get(key, &value);
        assert(s.ok());
        assert(value.size() == 255);
    }

    std::cout << "  PASS: auto flush" << std::endl;
}

void TestRecover() {
    std::cout << "=== TestRecover ===" << std::endl;
    CleanUp();

    // 第一轮：打开 DB，写入数据，**不刷盘**，直接关闭
    {
        KVDB::Options options;
        options.dbname = kTestDir;
        options.write_buffer_size = 10 * 1024 * 1024;  // 10MB，不触发自动刷盘

        std::unique_ptr<KVDB> db;
        Status s = KVDB::Open(options, &db);
        assert(s.ok());

        for (int i = 0; i < 100; ++i) {
            char key[32], value[32];
            snprintf(key, sizeof(key), "key%04d", i);
            snprintf(value, sizeof(value), "value%04d", i);
            s = db->Put(key, value);
            assert(s.ok());
        }
        // db 在这里析构，MemTable 数据在内存中消失，但 WAL 已落盘
    }

    // 第二轮：重新打开 DB，验证 WAL 重放恢复
    {
        KVDB::Options options;
        options.dbname = kTestDir;
        options.write_buffer_size = 10 * 1024 * 1024;

        std::unique_ptr<KVDB> db;
        Status s = KVDB::Open(options, &db);
        assert(s.ok());

        for (int i = 0; i < 100; ++i) {
            char key[32], expected[32];
            snprintf(key, sizeof(key), "key%04d", i);
            snprintf(expected, sizeof(expected), "value%04d", i);
            std::string value;
            s = db->Get(key, &value);
            assert(s.ok() && value == expected);
        }
    }

    std::cout << "  PASS: recover from WAL" << std::endl;
}

void TestCompactManually() {
    std::cout << "=== TestCompactManually ===" << std::endl;
    CleanUp();

    KVDB::Options options;
    options.dbname = kTestDir;
    options.write_buffer_size = 512;  // 512 bytes，快速产生多个 L0 文件

    std::unique_ptr<KVDB> db;
    Status s = KVDB::Open(options, &db);
    assert(s.ok());

    // 写入 500 条，触发多次自动 MemTable 切换
    for (int i = 0; i < 500; ++i) {
        char key[32], value[64];
        snprintf(key, sizeof(key), "key%04d", i);
        memset(value, 'x', sizeof(value) - 1);
        value[sizeof(value) - 1] = '\0';
        s = db->Put(key, value);
        assert(s.ok());
    }

    // 手动 Compaction L0→L1
    s = db->CompactManually();
    assert(s.ok());

    // 验证所有数据可读
    for (int i = 0; i < 500; ++i) {
        char key[32];
        snprintf(key, sizeof(key), "key%04d", i);
        std::string value;
        s = db->Get(key, &value);
        assert(s.ok());
        assert(value.size() == 63);
    }

    std::cout << "  PASS: manual compaction" << std::endl;
}

void TestDeleteAfterFlush() {
    std::cout << "=== TestDeleteAfterFlush ===" << std::endl;
    CleanUp();

    KVDB::Options options;
    options.dbname = kTestDir;
    options.write_buffer_size = 1024 * 1024;

    std::unique_ptr<KVDB> db;
    Status s = KVDB::Open(options, &db);
    assert(s.ok());

    s = db->Put("key1", "value1");
    assert(s.ok());

    s = db->Delete("key1");
    assert(s.ok());

    // 强制刷盘，让 Delete 标记进入 SSTable
    s = db->TEST_FlushMemTable();
    assert(s.ok());

    // 从 SSTable 读取，应返回 NotFound
    std::string value;
    s = db->Get("key1", &value);
    assert(s.IsNotFound());

    std::cout << "  PASS: delete after flush" << std::endl;
}

void TestCompactionDeduplication() {
    std::cout << "=== TestCompactionDeduplication ===" << std::endl;
    CleanUp();

    KVDB::Options options;
    options.dbname = kTestDir;
    options.write_buffer_size = 512;  // 小 buffer，快速产生多个 L0 文件

    std::unique_ptr<KVDB> db;
    Status s = KVDB::Open(options, &db);
    assert(s.ok());

    // 写入 key1，然后覆盖，再覆盖
    s = db->Put("key1", "v1");
    assert(s.ok());
    s = db->Put("key1", "v2");
    assert(s.ok());
    s = db->Put("key1", "v3");
    assert(s.ok());

    // 再写一些其他 key，触发自动 flush
    for (int i = 0; i < 500; ++i) {
        char key[32], value[64];
        snprintf(key, sizeof(key), "key%04d", i);
        memset(value, 'x', sizeof(value) - 1);
        value[sizeof(value) - 1] = '\0';
        db->Put(key, value);
    }

    // 手动 Compaction
    s = db->CompactManually();
    assert(s.ok());

    // 验证 key1 只有最新版本 v3
    std::string value;
    s = db->Get("key1", &value);
    assert(s.ok() && value == "v3");

    std::cout << "  PASS: compaction deduplication" << std::endl;
}

int main() {
    std::cout << "KVDB Tests Starting..." << std::endl;

    TestBasicPutGet();
    TestDelete();
    TestFlushAndReadSSTable();
    TestAutoFlush();
    TestRecover();
    TestCompactManually();
    TestDeleteAfterFlush();
    TestCompactionDeduplication();

    CleanUp();
    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}