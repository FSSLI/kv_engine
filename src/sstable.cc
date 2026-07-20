#include "sstable.h"
#include "lru_cache.h"
#include "bloom_filter.h"
#include <algorithm>
#include <cstring>
#include <cassert>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cerrno>

#ifdef KV_DEBUG
    #define DEBUG_LOG(fmt, ...) printf("[DEBUG] " fmt "\n", ##__VA_ARGS__)
#else
    #define DEBUG_LOG(fmt, ...) ((void)0)
#endif

namespace kv {

static const uint64_t kTableMagicNumber = 0x6373646264657665ULL;

// footer v2(48B):filter_offset(8) filter_size(8) index_offset(8)
//                 index_size(8) num_entries(8) magic(8)
// v1 是 32B、无过滤器字段;v2 通过 magic 不变、文件总长校验区分,
// 本项目内不保证打开 v1 旧文件(测试每次重建,无迁移负担)。
static const size_t kFooterSize = 48;

void EncodeFixed32(std::string* dst, uint32_t value) {
    char buf[4];
    buf[0] = static_cast<char>(value & 0xff);
    buf[1] = static_cast<char>((value >> 8) & 0xff);
    buf[2] = static_cast<char>((value >> 16) & 0xff);
    buf[3] = static_cast<char>((value >> 24) & 0xff);
    dst->append(buf, 4);
}

void EncodeFixed64(std::string* dst, uint64_t value) {
    char buf[8];
    for (int i = 0; i < 8; ++i) {
        buf[i] = static_cast<char>((value >> (i * 8)) & 0xff);
    }
    dst->append(buf, 8);
}

uint32_t DecodeFixed32(const char* ptr) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(ptr[0]))) |
           (static_cast<uint32_t>(static_cast<uint8_t>(ptr[1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(ptr[2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(ptr[3])) << 24);
}

uint64_t DecodeFixed64(const char* ptr) {
    uint64_t result = 0;
    for (int i = 0; i < 8; ++i) {
        result |= (static_cast<uint64_t>(static_cast<uint8_t>(ptr[i])) << (i * 8));
    }
    return result;
}

// ============ SSTableBuilder ============

SSTableBuilder::SSTableBuilder(const std::string& filename, size_t block_size,
                               int bloom_bits_per_key)
    : file_(nullptr)
    , filename_(filename)
    , block_size_(block_size)
    , bloom_bits_per_key_(bloom_bits_per_key)
    , current_block_entries_(0)
    , index_block_count_(0)
    , num_entries_(0)
    , data_size_(0)
    , current_offset_(0)
    , finished_(false)
{
    file_ = fopen(filename.c_str(), "wb");
    if (!file_) {
        DEBUG_LOG("SSTableBuilder: failed to create %s", filename.c_str());
    } else {
        setbuf(file_, nullptr);
        DEBUG_LOG("SSTableBuilder: created %s", filename.c_str());
    }
}

SSTableBuilder::~SSTableBuilder() {
    if (file_ && !finished_) {
        fclose(file_);
        remove(filename_.c_str());
        DEBUG_LOG("SSTableBuilder: removed incomplete file %s", filename_.c_str());
    } else if (file_) {
        fclose(file_);
    }
}

Status SSTableBuilder::Add(const Slice& key, const Slice& value) {
    if (!file_) {
        return Status::IOError("SSTable file not open");
    }
    if (finished_) {
        return Status::InvalidArgument("SSTable already finished");
    }

    // 新增：SSTable 要求 key 严格按字典序递增
    if (num_entries_ > 0 && key.compare(Slice(last_key_)) <= 0) {
        return Status::InvalidArgument(
            "keys must be added in strict sorted order");
    }

    if (num_entries_ == 0) {
        first_key_ = key.ToString();
    }

    if (current_block_entries_ > 0 && 
        current_block_.size() + key.size() + value.size() + 12 > block_size_) {
        Status s = FlushBlock();
        if (!s.ok()) return s;
    }

    EncodeFixed32(&current_block_, static_cast<uint32_t>(key.size()));
    current_block_.append(key.data(), key.size());
    EncodeFixed32(&current_block_, static_cast<uint32_t>(value.size()));
    current_block_.append(value.data(), value.size());

    current_block_entries_++;
    num_entries_++;
    last_key_ = key.ToString();
    if (bloom_bits_per_key_ > 0) {
        pending_keys_.push_back(key.ToString());  // Finish 时一次性编码过滤器
    }

    return Status::OK();
}

Status SSTableBuilder::FlushBlock() {
    if (current_block_entries_ == 0) {
        return Status::OK();
    }

    std::string block;
    EncodeFixed32(&block, current_block_entries_);
    block.append(current_block_);

    Status s = WriteRaw(block);
    if (!s.ok()) return s;

    EncodeFixed32(&index_block_, static_cast<uint32_t>(last_key_.size()));
    index_block_.append(last_key_);
    EncodeFixed64(&index_block_, current_offset_);
    EncodeFixed64(&index_block_, block.size());

    index_block_count_++;
    data_size_ += block.size();
    current_offset_ += block.size();

    current_block_.clear();
    current_block_entries_ = 0;

    DEBUG_LOG("SSTableBuilder::FlushBlock: offset=%lu, size=%zu", 
              current_offset_ - block.size(), block.size());

    return Status::OK();
}

Status SSTableBuilder::WriteRaw(const std::string& data) {
    size_t written = fwrite(data.data(), 1, data.size(), file_);
    if (written != data.size()) {
        return Status::IOError("SSTable write failed");
    }
    return Status::OK();
}

Status SSTableBuilder::Finish(uint64_t* file_size) {
    if (!file_) {
        return Status::IOError("SSTable file not open");
    }
    if (finished_) {
        return Status::InvalidArgument("SSTable already finished");
    }

    Status s = FlushBlock();
    if (!s.ok()) return s;

    // 过滤器块紧跟数据块之后、索引块之前
    uint64_t filter_offset = current_offset_;
    uint64_t filter_size = 0;
    if (bloom_bits_per_key_ > 0) {
        BloomFilterPolicy policy(bloom_bits_per_key_);
        std::string filter_block;
        policy.CreateFilter(pending_keys_, &filter_block);
        s = WriteRaw(filter_block);
        if (!s.ok()) return s;
        filter_size = filter_block.size();
        current_offset_ += filter_size;
    }

    std::string index;
    EncodeFixed32(&index, index_block_count_);
    index.append(index_block_);

    uint64_t index_offset = current_offset_;
    uint64_t index_size = index.size();

    s = WriteRaw(index);
    if (!s.ok()) return s;
    current_offset_ += index_size;

    std::string footer;
    EncodeFixed64(&footer, filter_offset);
    EncodeFixed64(&footer, filter_size);
    EncodeFixed64(&footer, index_offset);
    EncodeFixed64(&footer, index_size);
    EncodeFixed64(&footer, num_entries_);
    EncodeFixed64(&footer, kTableMagicNumber);

    s = WriteRaw(footer);
    if (!s.ok()) return s;
    current_offset_ += footer.size();

    if (fflush(file_) != 0) {
        return Status::IOError("SSTable fflush failed");
    }

    finished_ = true;
    *file_size = current_offset_;

    DEBUG_LOG("SSTableBuilder::Finish: entries=%lu, data_size=%lu, index_offset=%lu, file_size=%lu",
              num_entries_, data_size_, index_offset, *file_size);

    return Status::OK();
}

// ============ SSTable ============

SSTable::SSTable(FILE* file, const std::string& filename,
                 uint64_t file_size, uint64_t num_entries,
                 const std::string& smallest, const std::string& largest,
                 uint64_t index_offset, uint64_t index_size,
                 uint64_t file_number, LRUCache* block_cache)
    : file_(file)
    , filename_(filename)
    , file_size_(file_size)
    , num_entries_(num_entries)
    , smallest_key_(smallest)
    , largest_key_(largest)
    , index_offset_(index_offset)
    , index_size_(index_size)
    , file_number_(file_number)
    , block_cache_(block_cache)
{
}

SSTable::~SSTable() {
    if (file_) {
        fclose(file_);
    }
}

Status SSTable::Open(const std::string& filename,
                       std::unique_ptr<SSTable>* table,
                       uint64_t file_number,
                       LRUCache* block_cache) {
    FILE* file = fopen(filename.c_str(), "rb");
    if (!file) {
        return Status::IOError("cannot open SSTable: " + filename);
    }

    int fd = fileno(file);
    struct stat st;
    if (fstat(fd, &st) != 0) {
        fclose(file);
        return Status::IOError("cannot stat SSTable: " + filename);
    }
    uint64_t file_size = static_cast<uint64_t>(st.st_size);

    if (file_size < kFooterSize) {
        fclose(file);
        return Status::Corruption("SSTable file too small");
    }

    char footer_buf[kFooterSize];
    if (fseek(file, -static_cast<long>(kFooterSize), SEEK_END) != 0) {
        fclose(file);
        return Status::IOError("cannot seek to footer");
    }
    if (fread(footer_buf, 1, kFooterSize, file) != kFooterSize) {
        fclose(file);
        return Status::IOError("cannot read footer");
    }

    uint64_t filter_offset = DecodeFixed64(footer_buf);
    uint64_t filter_size = DecodeFixed64(footer_buf + 8);
    uint64_t index_offset = DecodeFixed64(footer_buf + 16);
    uint64_t index_size = DecodeFixed64(footer_buf + 24);
    uint64_t num_entries = DecodeFixed64(footer_buf + 32);
    uint64_t magic = DecodeFixed64(footer_buf + 40);

    if (magic != kTableMagicNumber) {
        fclose(file);
        return Status::Corruption("SSTable magic number mismatch");
    }

    if (index_offset + index_size + kFooterSize != file_size) {
        fclose(file);
        return Status::Corruption("SSTable footer inconsistent");
    }
    if (filter_size > 0 && filter_offset + filter_size != index_offset) {
        fclose(file);
        return Status::Corruption("SSTable filter block inconsistent");
    }

    // 过滤器块整个读进内存(10 bits/key,1 万个 key 也才 ~12KB)
    std::string filter_data;
    if (filter_size > 0) {
        filter_data.resize(static_cast<size_t>(filter_size));
        if (fseek(file, static_cast<long>(filter_offset), SEEK_SET) != 0 ||
            fread(&filter_data[0], 1, static_cast<size_t>(filter_size), file) != filter_size) {
            fclose(file);
            return Status::IOError("cannot read filter block");
        }
    }

    std::string index_data;
    index_data.resize(static_cast<size_t>(index_size));
    if (fseek(file, static_cast<long>(index_offset), SEEK_SET) != 0) {
        fclose(file);
        return Status::IOError("cannot seek to index");
    }
    if (fread(&index_data[0], 1, static_cast<size_t>(index_size), file) != index_size) {
        fclose(file);
        return Status::IOError("cannot read index");
    }

    uint32_t num_blocks = DecodeFixed32(index_data.data());
    const char* ptr = index_data.data() + 4;
    const char* end = index_data.data() + index_size;

    std::vector<IndexEntry> entries;
    entries.reserve(num_blocks);

    std::string largest_key;

    for (uint32_t i = 0; i < num_blocks; ++i) {
        if (ptr + 4 > end) {
            fclose(file);
            return Status::Corruption("SSTable index truncated");
        }
        uint32_t key_len = DecodeFixed32(ptr);
        ptr += 4;

        if (ptr + key_len + 16 > end) {
            fclose(file);
            return Status::Corruption("SSTable index truncated");
        }

        IndexEntry entry;
        entry.last_key.assign(ptr, key_len);
        ptr += key_len;
        entry.offset = DecodeFixed64(ptr);
        entry.size = DecodeFixed64(ptr + 8);
        ptr += 16;
        entries.push_back(std::move(entry));

        // 用 entries.back() 获取已存入的 last_key，避免访问被 move 的 entry
        largest_key = entries.back().last_key;
    }

    // 修正 smallest_key：读取第一个 DataBlock 的第一个 key
    std::string smallest_key;
    if (!entries.empty()) {
        const auto& first_entry = entries[0];
        std::string first_block_data;
        first_block_data.resize(static_cast<size_t>(first_entry.size));
        if (fseek(file, static_cast<long>(first_entry.offset), SEEK_SET) != 0) {
            fclose(file);
            return Status::IOError("cannot seek to first data block");
        }
        if (fread(&first_block_data[0], 1, static_cast<size_t>(first_entry.size), file) 
            != first_entry.size) {
            fclose(file);
            return Status::IOError("cannot read first data block");
        }

        uint32_t first_block_entries = DecodeFixed32(first_block_data.data());
        if (first_block_entries > 0) {
            const char* kp = first_block_data.data() + 4;
            uint32_t first_key_len = DecodeFixed32(kp);
            kp += 4;
            smallest_key.assign(kp, first_key_len);
        }
    }

    table->reset(new SSTable(file, filename, file_size, num_entries,
                              smallest_key, largest_key,
                              index_offset, index_size,
                              file_number, block_cache));
    (*table)->filter_data_ = std::move(filter_data);
                              
    size_t loaded_blocks = entries.size();
    (*table)->index_entries_ = std::move(entries);

    DEBUG_LOG("SSTable::Open: %s, size=%lu, blocks=%zu, entries=%lu, smallest=%s, largest=%s",
            filename.c_str(), file_size, loaded_blocks, num_entries,
            smallest_key.c_str(), largest_key.c_str());

        return Status::OK();
    }

// 并发安全版:pread 一次调用自带偏移、原子定位,不碰 FILE* 内部游标。

// 修复前(fseek+fread 两步):前台 Get 与后台 compaction 归并经 TableCache

// 共享同一 SSTable 实例时,两线程的 fseek/fread 会交错,读到错误偏移的块,

// 表现为 key 偶发"不存在"(复现:4 线程并发 Get,40 万次约 170 次错误)。

Status SSTable::ReadAt(uint64_t offset, size_t size, std::string* result) const {
    result->resize(size);
    int fd = fileno(file_);
    size_t done = 0;
    while (done < size) {
        ssize_t n = pread(fd, &(*result)[done], size - done,
                          static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            return Status::IOError("SSTable pread failed");
        }
        if (n == 0) {
            return Status::IOError("SSTable short read");
        }
        done += static_cast<size_t>(n);
    }
    return Status::OK();
}

Status SSTable::FindBlock(const Slice& key, uint64_t* offset, uint64_t* size) const {
    if (index_entries_.empty()) {
        return Status::NotFound("no blocks");
    }

    // lower_bound: 第一个 last_key >= key 的 block
    int left = 0;
    int right = static_cast<int>(index_entries_.size()) - 1;
    int target = static_cast<int>(index_entries_.size()) - 1;

    while (left <= right) {
        int mid = left + (right - left) / 2;
        const Slice mid_key(index_entries_[mid].last_key);
        if (key.compare(mid_key) <= 0) {
            target = mid;
            right = mid - 1;
        } else {
            left = mid + 1;
        }
    }

    *offset = index_entries_[target].offset;
    *size = index_entries_[target].size;
    return Status::OK();
}

Status SSTable::Get(const Slice& key, std::string* value) const {
    if (key.compare(Slice(smallest_key_)) < 0 ||
        key.compare(Slice(largest_key_)) > 0) {
        return Status::NotFound("key out of range");
    }

    // Week 4 布隆过滤器:一次内存位运算换掉一次磁盘读(+一次缓存查找)。
    // 被拦 = key 一定不在这个文件里,直接 NotFound。
    if (!filter_data_.empty() &&
        !BloomFilterPolicy::KeyMayMatch(key, Slice(filter_data_))) {
        filter_rejections_.fetch_add(1);
        DEBUG_LOG("BloomFilter reject: %.*s", static_cast<int>(key.size()), key.data());
        return Status::NotFound("key rejected by bloom filter");
    }

    uint64_t offset, size;
    Status s = FindBlock(key, &offset, &size);
    if (!s.ok()) return s;

    // BlockCache:同一个数据块只读一次磁盘,之后命中内存。
    // key = (file_number << 32) | block_offset,file_number 单调递增
    // 不复用,旧文件的 key 不会被新文件误命中。
    std::string block_data;
    uint64_t cache_key = MakeBlockCacheKey(file_number_, offset);
    if (block_cache_ && block_cache_->Get(cache_key, &block_data)) {
        DEBUG_LOG("BlockCache hit: file=%lu offset=%lu", file_number_, offset);
    } else {
        s = ReadAt(offset, static_cast<size_t>(size), &block_data);
        if (!s.ok()) return s;
        if (block_cache_) block_cache_->Put(cache_key, block_data);
    }

    uint32_t num_entries = DecodeFixed32(block_data.data());
    const char* ptr = block_data.data() + 4;
    const char* end = block_data.data() + block_data.size();

    for (uint32_t i = 0; i < num_entries; ++i) {
        if (ptr + 4 > end) break;
        uint32_t key_len = DecodeFixed32(ptr);
        ptr += 4;

        if (ptr + key_len + 4 > end) break;
        Slice entry_key(ptr, key_len);
        ptr += key_len;

        uint32_t value_len = DecodeFixed32(ptr);
        ptr += 4;

        if (ptr + value_len > end) break;
        Slice entry_value(ptr, value_len);
        ptr += value_len;

        if (entry_key == key) {
            value->assign(entry_value.data(), entry_value.size());
            return Status::OK();
        }

        if (entry_key.compare(key) > 0) {
            break;
        }
    }

    return Status::NotFound("key not in block");
}

std::unique_ptr<SSTable::Iterator> SSTable::NewIterator() const {
    return std::unique_ptr<Iterator>(new Iterator(this));
}

// ============ SSTable::Iterator ============

SSTable::Iterator::Iterator(const SSTable* table)
    : table_(table)
    , current_index_(0)
    , current_block_offset_(0)
    , current_block_size_(0)
    , valid_(false)
{
}

void SSTable::Iterator::SeekToFirst() {
    if (table_->index_entries_.empty()) {
        valid_ = false;
        return;
    }
    Status s = LoadBlock(table_->index_entries_[0].offset, 
                          table_->index_entries_[0].size);
    if (!s.ok()) {
        valid_ = false;
        return;
    }
    current_index_ = 0;
    valid_ = !entries_.empty();
}

void SSTable::Iterator::Seek(const Slice& target) {
    if (table_->index_entries_.empty()) {
        valid_ = false;
        return;
    }

    int left = 0;
    int right = static_cast<int>(table_->index_entries_.size()) - 1;
    int target_block = static_cast<int>(table_->index_entries_.size()) - 1;

    while (left <= right) {
        int mid = left + (right - left) / 2;
        Slice mid_key(table_->index_entries_[mid].last_key);
        if (target.compare(mid_key) <= 0) {
            target_block = mid;
            right = mid - 1;
        } else {
            left = mid + 1;
        }
    }

    Status s = LoadBlock(table_->index_entries_[target_block].offset,
                          table_->index_entries_[target_block].size);
    if (!s.ok()) {
        valid_ = false;
        return;
    }

    SeekInBlock(target);
}

void SSTable::Iterator::SeekInBlock(const Slice& target) {
    int left = 0;
    int right = static_cast<int>(entries_.size()) - 1;
    int result = 0;

    while (left <= right) {
        int mid = left + (right - left) / 2;
        int cmp = entries_[mid].key.compare(target);
        if (cmp < 0) {
            left = mid + 1;
        } else {
            result = mid;
            right = mid - 1;
        }
    }

    if (result < static_cast<int>(entries_.size()) && 
        entries_[result].key.compare(target) >= 0) {
        current_index_ = result;
        valid_ = true;
    } else {
        current_index_ = entries_.size();
        valid_ = false;
    }
}

void SSTable::Iterator::Next() {
    if (!valid_) return;

    current_index_++;
    if (current_index_ >= entries_.size()) {
        int next_block = -1;
        for (size_t i = 0; i < table_->index_entries_.size(); ++i) {
            if (table_->index_entries_[i].offset == current_block_offset_) {
                next_block = static_cast<int>(i) + 1;
                break;
            }
        }

        if (next_block >= 0 && next_block < static_cast<int>(table_->index_entries_.size())) {
            Status s = LoadBlock(table_->index_entries_[next_block].offset,
                                  table_->index_entries_[next_block].size);
            if (s.ok()) {
                current_index_ = 0;
                valid_ = !entries_.empty();
            } else {
                valid_ = false;
            }
        } else {
            valid_ = false;
        }
    }
}

bool SSTable::Iterator::Valid() const {
    return valid_ && current_index_ < entries_.size();
}

Slice SSTable::Iterator::key() const {
    assert(Valid());
    return entries_[current_index_].key;
}

Slice SSTable::Iterator::value() const {
    assert(Valid());
    return entries_[current_index_].value;
}

Status SSTable::Iterator::LoadBlock(uint64_t offset, uint64_t size) {
    current_block_offset_ = offset;
    current_block_size_ = size;

    // 与 Get 共用同一份 BlockCache:归并/全表扫描同样吃缓存
    uint64_t cache_key = MakeBlockCacheKey(table_->file_number_, offset);
    bool hit = false;
    if (table_->block_cache_) {
        hit = table_->block_cache_->Get(cache_key, &block_data_);
    }
    if (!hit) {
        Status s = table_->ReadAt(offset, static_cast<size_t>(size), &block_data_);
        if (!s.ok()) return s;
        if (table_->block_cache_) table_->block_cache_->Put(cache_key, block_data_);
    }

    entries_.clear();
    uint32_t num_entries = DecodeFixed32(block_data_.data());
    const char* ptr = block_data_.data() + 4;
    const char* end = block_data_.data() + block_data_.size();

    for (uint32_t i = 0; i < num_entries; ++i) {
        if (ptr + 4 > end) break;
        uint32_t key_len = DecodeFixed32(ptr);
        ptr += 4;

        if (ptr + key_len + 4 > end) break;
        Slice key(ptr, key_len);
        ptr += key_len;

        uint32_t value_len = DecodeFixed32(ptr);
        ptr += 4;

        if (ptr + value_len > end) break;
        Slice value(ptr, value_len);
        ptr += value_len;

        entries_.push_back({key, value});
    }

    return Status::OK();
}

// ============ TableCache ============

Status TableCache::FindTable(uint64_t file_number, const std::string& dbname,
                              std::shared_ptr<SSTable>* table) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = cache_.find(file_number);
    if (it != cache_.end()) {
        *table = it->second.lock();
        if (*table) {
            DEBUG_LOG("TableCache::FindTable: cache hit %lu", file_number);
            return Status::OK();
        }
        cache_.erase(it);
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "%06lu.sst", file_number);
    std::string filename = dbname + "/" + buf;

    std::unique_ptr<SSTable> new_table;
    Status s = SSTable::Open(filename, &new_table, file_number, block_cache_);
    if (!s.ok()) return s;

    *table = std::shared_ptr<SSTable>(new_table.release());
    cache_[file_number] = *table;

    DEBUG_LOG("TableCache::FindTable: cache miss %lu, loaded %s", 
              file_number, filename.c_str());
    return Status::OK();
}

void TableCache::Evict(uint64_t file_number) {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.erase(file_number);
    // 文件即将被 compaction 删除,同步清掉它的缓存块,防止死块白占空间
    if (block_cache_) block_cache_->EvictFile(file_number);
    DEBUG_LOG("TableCache::Evict: %lu", file_number);
}

void TableCache::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
    DEBUG_LOG("TableCache::Clear");
}

} // namespace kv