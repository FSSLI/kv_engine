#include "kv_db.h"
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>
#include "merging_iterator.h"

#ifdef KV_DEBUG
#define DEBUG_LOG(fmt, ...) printf("[DEBUG] " fmt "\n", ##__VA_ARGS__)
#else
#define DEBUG_LOG(fmt, ...) ((void)0)
#endif

namespace kv {

class MemTableInserter : public WriteBatch::Handler {
public:
    explicit MemTableInserter(SkipList* memtable) : memtable_(memtable) {}
    void Put(const std::string& key, const std::string& value) override {
        memtable_->Insert(key, value);
    }
    void Delete(const std::string& key) override {
        memtable_->Insert(key, "");
    }
private:
    SkipList* memtable_;
};

Status KVDB::Open(const Options& options, std::unique_ptr<KVDB>* dbptr) {
    struct stat st;
    if (stat(options.dbname.c_str(), &st) != 0) {
        if (mkdir(options.dbname.c_str(), 0755) != 0) {
            return Status::IOError("cannot create db directory: " + options.dbname);
        }
    }

    std::unique_ptr<KVDB> db(new KVDB(options));
    db->version_set_.reset(new VersionSet(options.dbname));
    Status s = db->version_set_->Recover();
    if (!s.ok()) return s;

    s = db->Recover();
    if (!s.ok()) return s;

    dbptr->reset(db.release());
    DEBUG_LOG("KVDB::Open: %s", options.dbname.c_str());
    return Status::OK();
}

KVDB::KVDB(const Options& options) : options_(options), memtable_(new SkipList()) {
    // BlockCache:所有 SSTable 读共享一份缓存,0 表示关闭
    if (options_.block_cache_capacity > 0) {
        block_cache_.reset(new LRUCache(options_.block_cache_capacity));
        table_cache_.SetBlockCache(block_cache_.get());
    }
}

KVDB::~KVDB() = default;

Status KVDB::OpenNewWAL() {
    uint64_t log_number = version_set_->NewFileNumber();
    char buf[128];
    snprintf(buf, sizeof(buf), "%s/%06lu.log", options_.dbname.c_str(), log_number);
    wal_filename_ = buf;

    std::unique_ptr<WAL> wal;
    Status s = WAL::Create(buf, &wal);
    if (!s.ok()) return s;
    wal_ = std::move(wal);

    version_set_->SetLogNumber(log_number);
    return version_set_->WriteSnapshot();  // 新增：持久化 log_number
}

Status KVDB::Put(const std::string& key, const std::string& value) {
    WriteBatch batch;
    batch.Put(key, value);
    return Write(&batch);
}

Status KVDB::Delete(const std::string& key) {
    WriteBatch batch;
    batch.Delete(key);
    return Write(&batch);
}

Status KVDB::Write(WriteBatch* batch) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!batch || batch->Count() == 0) {
        return Status::InvalidArgument("empty batch");
    }

    uint64_t seq = last_sequence_.fetch_add(1);

    Status s = wal_->Append(*batch, seq);
    if (!s.ok()) return s;

    MemTableInserter inserter(memtable_.get());
    s = batch->Iterate(&inserter);
    if (!s.ok()) return s;

    if (memtable_->ApproximateMemoryUsage() > options_.write_buffer_size) {
        s = SwitchMemTable();
        if (!s.ok()) return s;
    }

    return Status::OK();
}

Status KVDB::Get(const std::string& key, std::string* value) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (memtable_->Contains(key, value)) {
        if (value->empty()) return Status::NotFound("key deleted");
        return Status::OK();
    }

    // 查 SSTable：L0 从新到旧逐个查，L1+ 二分查找
    for (int level = 0; level < VersionSet::kMaxLevel; ++level) {
        const auto& files = version_set_->GetLevel(level);
        if (files.empty()) continue;

        if (level == 0) {
            for (auto it = files.rbegin(); it != files.rend(); ++it) {
                std::shared_ptr<SSTable> table;
                Status s = table_cache_.FindTable(it->file_number, options_.dbname, &table);
                // 修复:文件损坏要上报,不能静默跳过当成 NotFound
                if (!s.ok()) return s;
                s = table->Get(key, value);
                if (s.ok()) {
                    // 修复：空 value 表示 Delete 标记
                    if (value->empty()) return Status::NotFound("key deleted");
                    return Status::OK();
                }
                if (!s.IsNotFound()) return s;
            }
        } else {
            int left = 0, right = static_cast<int>(files.size()) - 1;
            while (left <= right) {
                int mid = left + (right - left) / 2;
                const auto& meta = files[mid];
                if (key < meta.smallest) {
                    right = mid - 1;
                } else if (key > meta.largest) {
                    left = mid + 1;
                } else {
                    std::shared_ptr<SSTable> table;
                    Status s = table_cache_.FindTable(meta.file_number, options_.dbname, &table);
                    if (!s.ok()) return s;
                    s = table->Get(key, value);
                    if (s.ok()) {
                        // 修复：空 value 表示 Delete 标记
                        if (value->empty()) return Status::NotFound("key deleted");
                        return Status::OK();
                    }
                    if (!s.IsNotFound()) return s;
                    break;
                }
            }
        }
    }

    return Status::NotFound("key not found");
}

Status KVDB::SwitchMemTable() {
    uint64_t file_number = version_set_->NewFileNumber();
    char sst_buf[128];
    snprintf(sst_buf, sizeof(sst_buf), "%s/%06lu.sst",
             options_.dbname.c_str(), file_number);

    SSTableBuilder builder(sst_buf, 4096);
    SkipList::Iterator iter(memtable_.get());
    iter.SeekToFirst();

    std::string first_key, last_key;
    bool first = true;
    while (iter.Valid()) {
        Status s = builder.Add(iter.key(), iter.value());
        if (!s.ok()) return s;
        if (first) {
            first_key = iter.key().ToString();
            first = false;
        }
        last_key = iter.key().ToString();
        iter.Next();
    }

    uint64_t file_size = 0;
    Status s = builder.Finish(&file_size);
    if (!s.ok()) return s;

    FileMetaData meta;
    meta.file_number = file_number;
    meta.file_size = file_size;
    meta.smallest = first_key;
    meta.largest = last_key;
    version_set_->AddFile(0, meta);

    s = version_set_->WriteSnapshot();
    if (!s.ok()) return s;

    wal_->Close();
    remove(wal_filename_.c_str());

    memtable_.reset(new SkipList());
    s = OpenNewWAL();
    if (!s.ok()) return s;

    DEBUG_LOG("KVDB::SwitchMemTable: flushed to %s, L0_files=%d",
              sst_buf, version_set_->NumLevelFiles(0));
    return Status::OK();
}

Status KVDB::TEST_FlushMemTable() {
    std::lock_guard<std::mutex> lock(mutex_);
    return SwitchMemTable();
}

Status KVDB::CompactManually() {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto& l0_files = version_set_->GetLevel(0);
    if (l0_files.empty()) {
        return Status::OK();
    }

    // 1. 收集 L0 文件
    std::vector<FileMetaData> inputs_l0 = l0_files;

    // 2. 计算 L0 整体 key 范围
    std::string smallest = inputs_l0[0].smallest;
    std::string largest = inputs_l0[0].largest;
    for (size_t i = 1; i < inputs_l0.size(); ++i) {
        if (inputs_l0[i].smallest < smallest) smallest = inputs_l0[i].smallest;
        if (inputs_l0[i].largest > largest) largest = inputs_l0[i].largest;
    }

    // 3. 找 L1 重叠文件（L1 文件之间不重叠，直接遍历检查）
    std::vector<FileMetaData> inputs_l1;
    const auto& l1_files = version_set_->GetLevel(1);
    for (const auto& meta : l1_files) {
        if (!(largest < meta.smallest || smallest > meta.largest)) {
            inputs_l1.push_back(meta);
        }
    }

    // 4. 打开所有文件，创建 MergingIterator
    // 注意：Iterator 内部只持有 SSTable 裸指针，必须额外保活 shared_ptr
    std::vector<std::shared_ptr<SSTable>> tables_to_keep_alive;
    std::vector<std::unique_ptr<Iterator>> iters;

    // 修复(P0):L0 必须按"新→旧"顺序创建迭代器。
    // levels_[0] 是 Append 顺序(旧文件在前),正序遍历会让最旧的 L0 文件
    // 拿到最小 index;而 MergingIterator 在同 key 时让 index 小的先出堆,
    // 去重保留先出堆的版本 → 旧值会"复活"。倒序后最新 L0 拿到 index 0。
    for (auto rit = inputs_l0.rbegin(); rit != inputs_l0.rend(); ++rit) {
        const auto& meta = *rit;
        std::shared_ptr<SSTable> table;
        Status s = table_cache_.FindTable(meta.file_number, options_.dbname, &table);
        if (!s.ok()) return s;
        tables_to_keep_alive.push_back(table);  // 保活，直到遍历结束
        iters.emplace_back(table->NewIterator());
    }
    for (const auto& meta : inputs_l1) {
        std::shared_ptr<SSTable> table;
        Status s = table_cache_.FindTable(meta.file_number, options_.dbname, &table);
        if (!s.ok()) return s;
        tables_to_keep_alive.push_back(table);  // 保活，直到遍历结束
        iters.emplace_back(table->NewIterator());
    }

    // 5. 归并输出到新的 L1 SSTable
    uint64_t new_file_number = version_set_->NewFileNumber();
    char sst_buf[128];
    snprintf(sst_buf, sizeof(sst_buf), "%s/%06lu.sst",
             options_.dbname.c_str(), new_file_number);

    SSTableBuilder builder(sst_buf, 4096);
    MergingIterator merge(std::move(iters));
    merge.SeekToFirst();

    std::string first_key, last_key;
    bool first = true;
    std::string last_output_key;  // 新增：用于同一 key 多版本去重

    while (merge.Valid()) {
        std::string cur_key = merge.key().ToString();

        // 修复：相同 key 只保留第一个（最新版本，因为 L0 的 iterator index 更小，先出堆）
        if (cur_key != last_output_key) {
            // Week 1 简化：空 value 视为 Delete，Compaction 时跳过
            if (merge.value().size() > 0) {
                Status s = builder.Add(merge.key(), merge.value());
                if (!s.ok()) return s;
                if (first) {
                    first_key = cur_key;
                    first = false;
                }
                last_key = cur_key;
            }
            last_output_key = cur_key;
        }
        merge.Next();
    }

    uint64_t file_size = 0;
    Status s = builder.Finish(&file_size);
    if (!s.ok()) return s;

    // 6. 更新 VersionSet
    for (const auto& meta : inputs_l0) {
        version_set_->RemoveFile(0, meta.file_number);
        table_cache_.Evict(meta.file_number);
    }
    for (const auto& meta : inputs_l1) {
        version_set_->RemoveFile(1, meta.file_number);
        table_cache_.Evict(meta.file_number);
    }

    // 修复:输入全是 tombstone(或空)时没有任何有效输出,此时不能 AddFile,
    // 否则会产生 smallest/largest 为空串的 L1 文件,污染二分查找的区间假设
    if (first) {
        remove(sst_buf);  // 删掉空产物文件
    } else {
        FileMetaData new_meta;
        new_meta.file_number = new_file_number;
        new_meta.file_size = file_size;
        new_meta.smallest = first_key;
        new_meta.largest = last_key;
        version_set_->AddFile(1, new_meta);
    }

    s = version_set_->WriteSnapshot();
    if (!s.ok()) return s;

    // 7. 删除旧 SSTable 文件
    for (const auto& meta : inputs_l0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s/%06lu.sst", options_.dbname.c_str(), meta.file_number);
        remove(buf);
    }
    for (const auto& meta : inputs_l1) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s/%06lu.sst", options_.dbname.c_str(), meta.file_number);
        remove(buf);
    }

    DEBUG_LOG("KVDB::CompactManually: L0=%d L1=%d new_file=%lu",
              version_set_->NumLevelFiles(0), version_set_->NumLevelFiles(1),
              new_file_number);
    return Status::OK();
}

Status KVDB::Recover() {
    uint64_t log_number = version_set_->LogNumber();
    uint64_t prev_log = version_set_->PrevLogNumber();

    // 先重放 prev_log（如果有，对应 immutable_mem_ 的旧 WAL）
    if (prev_log > 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s/%06lu.log", options_.dbname.c_str(), prev_log);
        Status s = RecoverLogFile(buf);
        if (!s.ok() && !s.IsNotFound()) return s;
    }

    // 重放当前活跃的 WAL
    if (log_number > 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s/%06lu.log", options_.dbname.c_str(), log_number);
        Status s = RecoverLogFile(buf);
        if (!s.ok() && !s.IsNotFound()) return s;

        // 继续追加写同一个 WAL 文件（数据安全：MemTable 里的数据仍受 WAL 保护）
        std::unique_ptr<WAL> wal;
        s = WAL::OpenForAppend(buf, &wal);
        if (!s.ok()) return s;
        wal_ = std::move(wal);
        wal_filename_ = buf;
        return Status::OK();
    }

    // 全新数据库
    return OpenNewWAL();
}

Status KVDB::RecoverLogFile(const std::string& filename) {
    std::unique_ptr<WAL> wal;
    Status s = WAL::OpenForRead(filename, &wal);
    if (s.IsNotFound()) return Status::OK();  // 文件不存在，正常
    if (!s.ok()) return s;

    uint64_t seq = 0;
    WriteBatch batch;
    uint64_t max_seq = 0;

    while (wal->ReadNext(&seq, &batch)) {
        MemTableInserter inserter(memtable_.get());
        s = batch.Iterate(&inserter);
        if (!s.ok()) return s;
        if (seq > max_seq) max_seq = seq;
    }

    // 恢复序列号：下一条写入用 max_seq + 1
    last_sequence_.store(max_seq + 1);
    return Status::OK();
}

}  // namespace kv