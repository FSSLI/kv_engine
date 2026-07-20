#pragma once

#include <atomic>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <cstdint>
#include <cstdio>
#include "slice.h"
#include "iterator.h"
#include "status.h"
#include <mutex>

namespace kv {

// 前向声明:头文件只持裸指针,不依赖 lru_cache.h,编译耦合最小
class LRUCache;

void EncodeFixed32(std::string* dst, uint32_t value);
void EncodeFixed64(std::string* dst, uint64_t value);
uint32_t DecodeFixed32(const char* ptr);
uint64_t DecodeFixed64(const char* ptr);

class SSTableBuilder {
public:
    // bloom_bits_per_key > 0 时,Finish 会为整文件写一个布隆过滤器块;
    // = 0 兼容旧行为(不生成过滤器,footer 里 filter_size = 0)
    explicit SSTableBuilder(const std::string& filename,
                            size_t block_size = 4096,
                            int bloom_bits_per_key = 0);
    ~SSTableBuilder();

    Status Add(const Slice& key, const Slice& value);
    Status Finish(uint64_t* file_size);
    uint64_t NumEntries() const { return num_entries_; }

private:
    Status FlushBlock();
    Status WriteRaw(const std::string& data);

    FILE* file_;
    std::string filename_;
    size_t block_size_;
    int bloom_bits_per_key_;
    std::vector<std::string> pending_keys_;  // 建过滤器用,Finish 时一次性编码
    std::string current_block_;
    uint32_t current_block_entries_;
    std::string first_key_;
    std::string last_key_;
    std::string index_block_;
    uint32_t index_block_count_;
    uint64_t num_entries_;
    uint64_t data_size_;
    uint64_t current_offset_;
    bool finished_;

    SSTableBuilder(const SSTableBuilder&) = delete;
    SSTableBuilder& operator=(const SSTableBuilder&) = delete;
};

class SSTable {
public:
    // file_number / block_cache 带默认值,老调用点(test_sstable.cc)不用改;
    // TableCache 打开文件时传入真实 file_number 和共享缓存
    static Status Open(const std::string& filename,
                       std::unique_ptr<SSTable>* table,
                       uint64_t file_number = 0,
                       LRUCache* block_cache = nullptr);

    ~SSTable();

    Status Get(const Slice& key, std::string* value) const;

    // 嵌套类直接在类内声明继承 ::kv::Iterator
    class Iterator : public ::kv::Iterator {
    public:
        explicit Iterator(const SSTable* table);
        void Seek(const Slice& target) override;
        void SeekToFirst() override;
        void Next() override;
        bool Valid() const override;
        Slice key() const override;
        Slice value() const override;
    private:
        Status LoadBlock(uint64_t offset, uint64_t size);
        void SeekInBlock(const Slice& target);

        const SSTable* table_;
        std::string block_data_;
        struct Entry {
            Slice key;
            Slice value;
        };
        std::vector<Entry> entries_;
        size_t current_index_;
        uint64_t current_block_offset_;
        uint64_t current_block_size_;
        bool valid_;
    };

    std::unique_ptr<Iterator> NewIterator() const;

    uint64_t FileSize() const { return file_size_; }
    uint64_t NumEntries() const { return num_entries_; }
    Slice SmallestKey() const { return Slice(smallest_key_); }
    Slice LargestKey() const { return Slice(largest_key_); }
    const std::string& Filename() const { return filename_; }

    // 布隆过滤器拦下的查询次数(被拦 = 一次磁盘读都没发生)。
    // 测试与压测用它量化过滤器效果。
    uint64_t FilterRejections() const { return filter_rejections_.load(); }

private:
    struct IndexEntry {
        std::string last_key;
        uint64_t offset;
        uint64_t size;
    };

    SSTable(FILE* file, const std::string& filename,
            uint64_t file_size, uint64_t num_entries,
            const std::string& smallest, const std::string& largest,
            uint64_t index_offset, uint64_t index_size,
            uint64_t file_number, LRUCache* block_cache);

    Status ReadAt(uint64_t offset, size_t size, std::string* result) const;
    Status FindBlock(const Slice& key, uint64_t* offset, uint64_t* size) const;

    FILE* file_;
    std::string filename_;
    uint64_t file_size_;
    uint64_t num_entries_;
    std::string smallest_key_;
    std::string largest_key_;
    uint64_t index_offset_;
    uint64_t index_size_;
    std::vector<IndexEntry> index_entries_;

    uint64_t file_number_ = 0;          // BlockCache key 的高 32 位
    LRUCache* block_cache_ = nullptr;   // 不持有所有权,owner 是 KVDB

    // Week 4:整文件布隆过滤器,Open 时一次读进内存(通常几百字节~几 KB)。
    // 为空 = 该文件没有过滤器(旧文件 / 构建时关闭)。
    std::string filter_data_;
    mutable std::atomic<uint64_t> filter_rejections_{0};

    SSTable(const SSTable&) = delete;
    SSTable& operator=(const SSTable&) = delete;
};

class TableCache {
public:
    TableCache() = default;
    ~TableCache() = default;

    // 由 KVDB 构造时调用,注入共享 BlockCache(不持有所有权)
    void SetBlockCache(LRUCache* c) { block_cache_ = c; }

    Status FindTable(uint64_t file_number, const std::string& dbname,
                     std::shared_ptr<SSTable>* table);
    void Evict(uint64_t file_number);
    void Clear();

private:
    std::mutex mutex_;
    std::unordered_map<uint64_t, std::weak_ptr<SSTable>> cache_;
    LRUCache* block_cache_ = nullptr;

    TableCache(const TableCache&) = delete;
    TableCache& operator=(const TableCache&) = delete;
};

} // namespace kv