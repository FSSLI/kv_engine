#include <iostream>
#include <cassert>
#include <cstdio>
#include <string>
#include "wal.h"
#include "write_batch.h"

using namespace kv;

const std::string kTestFile = "/tmp/test_wal.log";

// 清理测试文件
void CleanUp() {
    remove(kTestFile.c_str());
}

// 测试 1：基本追加和读取
void TestBasicAppend() {
    std::cout << "=== TestBasicAppend ===" << std::endl;
    CleanUp();
    
    // 创建 WAL 并写入
    {
        std::unique_ptr<WAL> wal;
        Status s = WAL::Create(kTestFile, &wal);
        assert(s.ok());
        
        WriteBatch batch;
        batch.Put("key1", "value1");
        batch.Put("key2", "value2");
        
        s = wal->Append(batch, 1);
        assert(s.ok());
        
        WriteBatch batch2;
        batch2.Delete("key3");
        s = wal->Append(batch2, 2);
        assert(s.ok());
    }
    
    // 读取验证
    {
        std::unique_ptr<WAL> wal;
        Status s = WAL::OpenForRead(kTestFile, &wal);
        assert(s.ok());
        
        uint64_t seq;
        WriteBatch batch;
        
        assert(wal->ReadNext(&seq, &batch));
        assert(seq == 1);
        assert(batch.Count() == 2);
        
        assert(wal->ReadNext(&seq, &batch));
        assert(seq == 2);
        assert(batch.Count() == 1);
        
        // 应该结束了
        assert(!wal->ReadNext(&seq, &batch));
    }
    
    std::cout << "  PASS: basic append and read" << std::endl;
}

// 测试 2：空 WAL
void TestEmptyWAL() {
    std::cout << "=== TestEmptyWAL ===" << std::endl;
    CleanUp();
    
    {
        std::unique_ptr<WAL> wal;
        Status s = WAL::Create(kTestFile, &wal);
        assert(s.ok());
    }
    
    {
        std::unique_ptr<WAL> wal;
        Status s = WAL::OpenForRead(kTestFile, &wal);
        assert(s.ok());
        
        uint64_t seq;
        WriteBatch batch;
        assert(!wal->ReadNext(&seq, &batch));
    }
    
    std::cout << "  PASS: empty WAL" << std::endl;
}

// 测试 3：多条记录
void TestMultipleRecords() {
    std::cout << "=== TestMultipleRecords ===" << std::endl;
    CleanUp();
    
    const int N = 100;
    
    // 写入
    {
        std::unique_ptr<WAL> wal;
        Status s = WAL::Create(kTestFile, &wal);
        assert(s.ok());
        
        for (int i = 0; i < N; ++i) {
            WriteBatch batch;
            batch.Put("key" + std::to_string(i), "value" + std::to_string(i));
            s = wal->Append(batch, i + 1);
            assert(s.ok());
        }
    }
    
    // 读取验证
    {
        std::unique_ptr<WAL> wal;
        Status s = WAL::OpenForRead(kTestFile, &wal);
        assert(s.ok());
        
        uint64_t seq;
        WriteBatch batch;
        int count = 0;
        
        while (wal->ReadNext(&seq, &batch)) {
            assert(seq == count + 1);
            assert(batch.Count() == 1);
            count++;
        }
        
        assert(count == N);
    }
    
    std::cout << "  PASS: multiple records (" << N << ")" << std::endl;
}

// 测试 4：文件大小
void TestFileSize() {
    std::cout << "=== TestFileSize ===" << std::endl;
    CleanUp();
    
    std::unique_ptr<WAL> wal;
    Status s = WAL::Create(kTestFile, &wal);
    assert(s.ok());
    
    assert(wal->FileSize() == 0);
    
    WriteBatch batch;
    batch.Put("hello", "world");
    s = wal->Append(batch, 1);
    assert(s.ok());
    
    assert(wal->FileSize() > 0);
    
    std::cout << "  PASS: file size tracking" << std::endl;
}

// 测试 5：只读 WAL 不能写，只写 WAL 不能读
void TestReadWriteMode() {
    std::cout << "=== TestReadWriteMode ===" << std::endl;
    CleanUp();
    
    // 创建并写入
    {
        std::unique_ptr<WAL> wal;
        Status s = WAL::Create(kTestFile, &wal);
        assert(s.ok());
        
        WriteBatch batch;
        batch.Put("key", "value");
        s = wal->Append(batch, 1);
        assert(s.ok());
    }
    
    // 以读模式打开，尝试写入应该失败
    {
        std::unique_ptr<WAL> wal;
        Status s = WAL::OpenForRead(kTestFile, &wal);
        assert(s.ok());
        
        WriteBatch batch;
        batch.Put("key2", "value2");
        s = wal->Append(batch, 2);
        assert(!s.ok());
    }
    
    std::cout << "  PASS: read/write mode separation" << std::endl;
}

int main() {
    std::cout << "WAL Tests Starting..." << std::endl;
    
    TestBasicAppend();
    TestEmptyWAL();
    TestMultipleRecords();
    TestFileSize();
    TestReadWriteMode();
    
    CleanUp();
    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}