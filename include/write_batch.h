#pragma once

#include <string>
#include <vector>
#include "status.h"

namespace kv {

// 操作类型
enum RecordType : uint8_t {
    kTypeDeletion = 0,
    kTypeValue = 1,
};

// WriteBatch 编码格式（二进制布局）：
// [batch_length: 4B]  -- 整个 body + checksum 的长度
// [sequence_number: 8B]
// [count: 4B] 
// [type: 1B] [key_len: 4B] [key] [value_len: 4B] [value] ... 
// [checksum: 4B]  -- 对 [sequence_number: 8B] 到 [value] 的校验
//
// 总记录大小 = 4 (batch_length) + batch_length
// 其中 batch_length = 8 (seq) + 4 (count) + records_size + 4 (checksum)
class WriteBatch {
public:
    WriteBatch();

    // 添加 Put 操作
    void Put(const std::string& key, const std::string& value);

    // 添加 Delete 操作（value 为空，type = kTypeDeletion）
    void Delete(const std::string& key);

    // 清空
    void Clear();

    // 获取操作数量
    int Count() const;

    // 序列化为字符串（用于 WAL 写入）
    // 返回的字符串包含完整编码（含 batch_length 头和 checksum 尾）
    Status Serialize(std::string* dst, uint64_t sequence_number) const;

    // 从字符串反序列化（用于 WAL 恢复）
    // src 必须包含完整的 [batch_length][body][checksum] 格式
    static Status Deserialize(const std::string& src, 
                              uint64_t* sequence_number,
                              WriteBatch* batch);

    // 遍历 Batch 中的每条记录（用于重放到 MemTable）
    class Handler {
    public:
        virtual void Put(const std::string& key, const std::string& value) = 0;
        virtual void Delete(const std::string& key) = 0;
        virtual ~Handler() {}
    };

    // 遍历并调用 handler 处理每条记录
    Status Iterate(Handler* handler) const;

private:
    struct Record {
        RecordType type;
        std::string key;
        std::string value;  // Delete 时为空
    };

    std::vector<Record> records_;
};

} // namespace kv