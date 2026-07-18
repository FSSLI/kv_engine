#include <iostream>
#include <cassert>
#include <cstring>
#include "write_batch.h"

using namespace kv;

// 测试 1：基本 Put + 序列化/反序列化
void TestBasicPut() {
    std::cout << "=== TestBasicPut ===" << std::endl;
    
    WriteBatch batch;
    batch.Put("hello", "world");
    batch.Put("foo", "bar");
    
    assert(batch.Count() == 2);
    
    std::string serialized;
    Status s = batch.Serialize(&serialized, 42);
    assert(s.ok());
    
    uint64_t seq;
    WriteBatch recovered;
    s = WriteBatch::Deserialize(serialized, &seq, &recovered);
    assert(s.ok());
    assert(seq == 42);
    assert(recovered.Count() == 2);
    
    std::cout << "  PASS: basic put serialize/deserialize" << std::endl;
}

// 测试 2：混合 Put + Delete
void TestMixedOperations() {
    std::cout << "=== TestMixedOperations ===" << std::endl;
    
    WriteBatch batch;
    batch.Put("key1", "value1");
    batch.Delete("key2");
    batch.Put("key3", "value3");
    
    assert(batch.Count() == 3);
    
    std::string serialized;
    Status s = batch.Serialize(&serialized, 100);
    assert(s.ok());
    
    uint64_t seq;
    WriteBatch recovered;
    s = WriteBatch::Deserialize(serialized, &seq, &recovered);
    assert(s.ok());
    assert(seq == 100);
    assert(recovered.Count() == 3);
    
    std::cout << "  PASS: mixed operations" << std::endl;
}

// 测试 3：Iterate 遍历
class TestHandler : public WriteBatch::Handler {
public:
    std::string log;
    
    void Put(const std::string& key, const std::string& value) override {
        log += "P[" + key + "=" + value + "]";
    }
    
    void Delete(const std::string& key) override {
        log += "D[" + key + "]";
    }
};

void TestIterate() {
    std::cout << "=== TestIterate ===" << std::endl;
    
    WriteBatch batch;
    batch.Put("a", "1");
    batch.Delete("b");
    batch.Put("c", "3");
    
    TestHandler handler;
    Status s = batch.Iterate(&handler);
    assert(s.ok());
    assert(handler.log == "P[a=1]D[b]P[c=3]");
    
    std::cout << "  PASS: iterate handler" << std::endl;
}

// 测试 4：损坏数据检测
void TestCorruption() {
    std::cout << "=== TestCorruption ===" << std::endl;
    
    WriteBatch batch;
    batch.Put("key", "value");
    
    std::string serialized;
    batch.Serialize(&serialized, 1);
    
    // 篡改数据
    serialized[10] = 0xFF;
    
    uint64_t seq;
    WriteBatch recovered;
    Status s = WriteBatch::Deserialize(serialized, &seq, &recovered);
    assert(!s.ok());
    assert(s.IsCorruption());
    
    std::cout << "  PASS: corruption detected" << std::endl;
}

// 测试 5：空 Batch 拒绝序列化
void TestEmptyBatch() {
    std::cout << "=== TestEmptyBatch ===" << std::endl;
    
    WriteBatch batch;
    std::string serialized;
    Status s = batch.Serialize(&serialized, 1);
    assert(!s.ok());
    
    std::cout << "  PASS: empty batch rejected" << std::endl;
}

// 测试 6：大数据量
void TestLargeBatch() {
    std::cout << "=== TestLargeBatch ===" << std::endl;
    
    WriteBatch batch;
    for (int i = 0; i < 1000; ++i) {
        batch.Put("key" + std::to_string(i), "value" + std::to_string(i));
    }
    
    std::string serialized;
    Status s = batch.Serialize(&serialized, 999);
    assert(s.ok());
    
    uint64_t seq;
    WriteBatch recovered;
    s = WriteBatch::Deserialize(serialized, &seq, &recovered);
    assert(s.ok());
    assert(recovered.Count() == 1000);
    
    std::cout << "  PASS: large batch (1000 records)" << std::endl;
}

int main() {
    std::cout << "WriteBatch Tests Starting..." << std::endl;
    
    TestBasicPut();
    TestMixedOperations();
    TestIterate();
    TestCorruption();
    TestEmptyBatch();
    TestLargeBatch();
    
    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}