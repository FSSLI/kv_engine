#include "write_batch.h"
#include <cstring>

#ifdef KV_DEBUG
    #define DEBUG_LOG(fmt, ...) printf("[DEBUG] " fmt "\n", ##__VA_ARGS__)
#else
    #define DEBUG_LOG(fmt, ...) ((void)0)
#endif

namespace kv {

// 简单累加和校验（Week 1 先用这个，Week 4 换 CRC32）
// 多项式滚动哈希：sum = sum * 31 + byte
// 碰撞率高于 CRC32，但实现简单，Week 1 够用
static uint32_t SimpleChecksum(const char* data, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; ++i) {
        sum = sum * 31 + static_cast<uint8_t>(data[i]);
    }
    return sum;
}

// 辅助函数：写入固定长度整数到字符串（little-endian）
static void EncodeFixed32(std::string* dst, uint32_t value) {
    char buf[4];
    buf[0] = static_cast<char>(value & 0xff);
    buf[1] = static_cast<char>((value >> 8) & 0xff);
    buf[2] = static_cast<char>((value >> 16) & 0xff);
    buf[3] = static_cast<char>((value >> 24) & 0xff);
    dst->append(buf, 4);
}

static void EncodeFixed64(std::string* dst, uint64_t value) {
    char buf[8];
    for (int i = 0; i < 8; ++i) {
        buf[i] = static_cast<char>((value >> (i * 8)) & 0xff);
    }
    dst->append(buf, 8);
}

static uint32_t DecodeFixed32(const char* ptr) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(ptr[0]))) |
           (static_cast<uint32_t>(static_cast<uint8_t>(ptr[1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(ptr[2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(ptr[3])) << 24);
}

static uint64_t DecodeFixed64(const char* ptr) {
    uint64_t result = 0;
    for (int i = 0; i < 8; ++i) {
        result |= (static_cast<uint64_t>(static_cast<uint8_t>(ptr[i])) << (i * 8));
    }
    return result;
}

// ============ WriteBatch 实现 ============

WriteBatch::WriteBatch() {}

void WriteBatch::Put(const std::string& key, const std::string& value) {
    records_.push_back({kTypeValue, key, value});
}

void WriteBatch::Delete(const std::string& key) {
    records_.push_back({kTypeDeletion, key, ""});
}

void WriteBatch::Clear() {
    records_.clear();
}

int WriteBatch::Count() const {
    return static_cast<int>(records_.size());
}

Status WriteBatch::Serialize(std::string* dst, uint64_t sequence_number) const {
    if (records_.empty()) {
        return Status::InvalidArgument("empty batch");
    }

    // 步骤 1：构建 body（不含 batch_length 和 checksum）
    std::string body;
    EncodeFixed64(&body, sequence_number);
    EncodeFixed32(&body, static_cast<uint32_t>(records_.size()));

    for (const auto& rec : records_) {
        body.push_back(static_cast<char>(rec.type));
        EncodeFixed32(&body, static_cast<uint32_t>(rec.key.size()));
        body.append(rec.key);

        if (rec.type == kTypeValue) {
            EncodeFixed32(&body, static_cast<uint32_t>(rec.value.size()));
            body.append(rec.value);
        } else {
            // Delete 操作：value_len = 0，不写入 value 数据
            EncodeFixed32(&body, 0);
        }
    }

    // 步骤 2：计算 checksum（只校验 body）
    uint32_t checksum = SimpleChecksum(body.data(), body.size());

    // 步骤 3：组装最终格式：[batch_length][body][checksum]
    // batch_length = body.size() + 4 (checksum 的 4 字节)
    dst->clear();
    EncodeFixed32(dst, static_cast<uint32_t>(body.size() + 4));
    dst->append(body);
    EncodeFixed32(dst, checksum);

    DEBUG_LOG("WriteBatch::Serialize: seq=%lu, count=%zu, total_size=%zu",
              sequence_number, records_.size(), dst->size());

    return Status::OK();
}

Status WriteBatch::Deserialize(const std::string& src, 
                                uint64_t* sequence_number,
                                WriteBatch* batch) {
    if (src.size() < 12) {  // 至少要有 batch_length(4) + seq(8)
        return Status::Corruption("batch too short");
    }

    // 验证总长度：src.size() 必须等于 4 + batch_length
    uint32_t batch_length = DecodeFixed32(src.data());
    if (batch_length + 4 != src.size()) {
        return Status::Corruption("batch length mismatch");
    }

    // body = src[4 : 4 + batch_length - 4]（去掉 batch_length 头和 checksum 尾）
    size_t body_size = batch_length - 4;
    const char* body = src.data() + 4;

    // 验证 checksum
    uint32_t stored_checksum = DecodeFixed32(src.data() + 4 + body_size);
    uint32_t computed_checksum = SimpleChecksum(body, body_size);
    if (stored_checksum != computed_checksum) {
        return Status::Corruption("checksum mismatch");
    }

    // 解析 body
    *sequence_number = DecodeFixed64(body);
    uint32_t count = DecodeFixed32(body + 8);

    batch->Clear();
    const char* ptr = body + 12;  // 跳过 seq(8) + count(4)
    const char* end = body + body_size;

    for (uint32_t i = 0; i < count; ++i) {
        if (ptr >= end) {
            return Status::Corruption("truncated batch record");
        }

        RecordType type = static_cast<RecordType>(*ptr++);

        if (ptr + 4 > end) return Status::Corruption("truncated key_len");
        uint32_t key_len = DecodeFixed32(ptr);
        ptr += 4;

        if (ptr + key_len > end) return Status::Corruption("truncated key");
        std::string key(ptr, key_len);
        ptr += key_len;

        if (ptr + 4 > end) return Status::Corruption("truncated value_len");
        uint32_t value_len = DecodeFixed32(ptr);
        ptr += 4;

        std::string value;
        if (value_len > 0) {
            if (ptr + value_len > end) return Status::Corruption("truncated value");
            value.assign(ptr, value_len);
            ptr += value_len;
        }

        if (type == kTypeValue) {
            batch->Put(key, value);
        } else {
            batch->Delete(key);
        }
    }

    DEBUG_LOG("WriteBatch::Deserialize: seq=%lu, count=%u", *sequence_number, count);
    return Status::OK();
}

Status WriteBatch::Iterate(Handler* handler) const {
    for (const auto& rec : records_) {
        if (rec.type == kTypeValue) {
            handler->Put(rec.key, rec.value);
        } else {
            handler->Delete(rec.key);
        }
    }
    return Status::OK();
}

} // namespace kv