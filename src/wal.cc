#include "wal.h"
#include <cstdint>
#include <cstring>
#include <unistd.h>
#include <sys/stat.h>

#ifdef KV_DEBUG
    #define DEBUG_LOG(fmt, ...) printf("[DEBUG] " fmt "\n", ##__VA_ARGS__)
#else
    #define DEBUG_LOG(fmt, ...) ((void)0)
#endif

namespace kv {

static uint32_t DecodeFixed32(const char* ptr) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(ptr[0]))) |
           (static_cast<uint32_t>(static_cast<uint8_t>(ptr[1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(ptr[2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(ptr[3])) << 24);
}

WAL::WAL(FILE* file, bool writable)
    : file_(file), writable_(writable), file_size_(0) {
    if (!writable && file_) {
        int fd = fileno(file_);
        struct stat st;
        if (fstat(fd, &st) == 0) {
            file_size_ = static_cast<uint64_t>(st.st_size);
        }
    }
}

WAL::~WAL() {
    if (file_) {
        fclose(file_);
    }
}

Status WAL::Create(const std::string& filename, std::unique_ptr<WAL>* wal) {
    FILE* file = fopen(filename.c_str(), "wb");
    if (!file) {
        return Status::IOError("cannot create WAL: " + filename);
    }

    setbuf(file, nullptr);

    wal->reset(new WAL(file, true));
    DEBUG_LOG("WAL::Create: %s", filename.c_str());
    return Status::OK();
}

Status WAL::OpenForRead(const std::string& filename, std::unique_ptr<WAL>* wal) {
    FILE* file = fopen(filename.c_str(), "rb");
    if (!file) {
        return Status::IOError("cannot open WAL for read: " + filename);
    }

    wal->reset(new WAL(file, false));
    DEBUG_LOG("WAL::OpenForRead: %s, size=%lu", filename.c_str(), (*wal)->file_size_);
    return Status::OK();
}

Status WAL::OpenForAppend(const std::string& filename, std::unique_ptr<WAL>* wal) {
    FILE* file = fopen(filename.c_str(), "ab");
    if (!file) {
        return Status::IOError("cannot open WAL for append: " + filename);
    }
    setbuf(file, nullptr);
    wal->reset(new WAL(file, true));
    int fd = fileno(file);
    struct stat st;
    if (fstat(fd, &st) == 0) {
        (*wal)->file_size_ = static_cast<uint64_t>(st.st_size);
    }
    DEBUG_LOG("WAL::OpenForAppend: %s, size=%lu", filename.c_str(), (*wal)->file_size_);
    return Status::OK();
}

Status WAL::Append(const WriteBatch& batch, uint64_t sequence_number) {
    if (!writable_) {
        return Status::IOError("WAL not writable");
    }

    std::string record;
    Status s = batch.Serialize(&record, sequence_number);
    if (!s.ok()) {
        return s;
    }

    size_t written = fwrite(record.data(), 1, record.size(), file_);
    if (written != record.size()) {
        return Status::IOError("WAL write failed");
    }

    file_size_ += written;

    // 默认每次都 fsync(崩溃不丢数据);压测可用 SetSyncOnWrite(false) 关掉。
    // 关掉后持久性边界退化为:memtable flush 时 WAL::Close 仍会 Sync 一次。
    if (sync_on_write_) {
        s = Sync();
        if (!s.ok()) {
            return s;
        }
    }

    DEBUG_LOG("WAL::Append: seq=%lu, size=%zu, file_size=%lu",
              sequence_number, record.size(), file_size_);
    return Status::OK();
}

bool WAL::ReadNext(uint64_t* sequence_number, WriteBatch* batch) {
    if (writable_) {
        return false;
    }

    // 单条记录上限 64MB，防止 batch_length 损坏导致内存爆炸
    const uint32_t kMaxRecordSize = 64 * 1024 * 1024;

    while (true) {
        // 读取 batch_length（4 字节）
        char len_buf[4];
        size_t n = fread(len_buf, 1, 4, file_);
        if (n == 0) {
            return false;  // 正常 EOF
        }
        if (n != 4) {
            DEBUG_LOG("WAL::ReadNext: truncated length header (got %zu bytes)", n);
            return false;  // 尾部截断，无法继续
        }

        uint32_t batch_length = DecodeFixed32(len_buf);

        if (batch_length > kMaxRecordSize) {
            DEBUG_LOG("WAL::ReadNext: invalid batch_length %u, stopping recovery", batch_length);
            return false;
        }

        // 组装完整记录
        std::string record;
        record.resize(4 + batch_length);
        memcpy(&record[0], len_buf, 4);

        n = fread(&record[4], 1, batch_length, file_);
        if (n != batch_length) {
            DEBUG_LOG("WAL::ReadNext: truncated record body, expected=%u, got=%zu",
                      batch_length, n);
            return false;  // 尾部截断
        }

        // 反序列化（含 checksum 校验）
        Status s = WriteBatch::Deserialize(record, sequence_number, batch);
        if (s.ok()) {
            return true;
        }

        // 单条记录损坏（如 checksum 失败），跳过并尝试读取下一条
        // 此时文件指针已自然位于下一条记录的开头
        DEBUG_LOG("WAL::ReadNext: deserialize failed: %s, skipping to next record",
                  s.ToString().c_str());
    }
}

Status WAL::Sync() {
    if (!file_) {
        return Status::IOError("WAL not open");
    }

    if (fflush(file_) != 0) {
        return Status::IOError("WAL fflush failed");
    }

    if (fsync(fileno(file_)) != 0) {
        return Status::IOError("WAL fsync failed");
    }

    return Status::OK();
}

Status WAL::Close() {
    if (!file_) {
        return Status::OK();
    }

    Status s = Sync();
    fclose(file_);
    file_ = nullptr;
    return s;
}

uint64_t WAL::FileSize() const {
    return file_size_;
}

} // namespace kv
