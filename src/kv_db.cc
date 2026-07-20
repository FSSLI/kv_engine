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

    db->bg_thread_ = std::thread(&KVDB::BackgroundWork, db.get());  // ① 启动线程

    dbptr->reset(db.release());
    DEBUG_LOG("KVDB::Open: %s", options.dbname.c_str());
    return Status::OK();
}

KVDB::KVDB(const Options& options) : options_(options), memtable_(new SkipList()) {
    if (options_.block_cache_capacity > 0) {
        block_cache_.reset(new LRUCache(options_.block_cache_capacity));
        table_cache_.SetBlockCache(block_cache_.get());
    }
}

KVDB::~KVDB() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        bg_exit_ = true;
    }
    bg_cv_.notify_one();  // ② 你漏掉的:叫醒睡在 wait 上的后台线程
    if (bg_thread_.joinable()) bg_thread_.join();
}

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
    return version_set_->WriteSnapshot();
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
    std::unique_lock<std::mutex> lock(mutex_);

    if (!batch || batch->Count() == 0) {
        return Status::InvalidArgument("empty batch");
    }

    Status s = MakeRoomForWrite(&lock);
    if (!s.ok()) return s;

    uint64_t seq = last_sequence_.fetch_add(1);

    s = wal_->Append(*batch, seq);
    if (!s.ok()) return s;

    MemTableInserter inserter(memtable_.get());
    s = batch->Iterate(&inserter);
    if (!s.ok()) return s;

    return Status::OK();
}

Status KVDB::MakeRoomForWrite(std::unique_lock<std::mutex>* lock) {
    while (true) {
        if (memtable_->ApproximateMemoryUsage() <= options_.write_buffer_size) {
            return Status::OK();  // 有空间:即使 imm_ 在刷也不等
        }
        if (imm_ != nullptr) {
            imm_done_cv_.wait(*lock);  // mem 满 + 后台在刷:节流
        } else {
            SwitchToImmutableLocked();
            return Status::OK();
        }
    }
}

void KVDB::SwitchToImmutableLocked() {
    wal_->Close();
    uint64_t old_log = version_set_->LogNumber();
    version_set_->SetPrevLogNumber(old_log);
    imm_log_number_ = old_log;

    imm_ = std::move(memtable_);
    memtable_.reset(new SkipList());

    Status s = OpenNewWAL();
    if (!s.ok()) DEBUG_LOG("SwitchToImmutable: open WAL failed");  // ③ 去掉 ...
    bg_cv_.notify_one();
}

Status KVDB::Get(const std::string& key, std::string* value) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (memtable_->Contains(key, value)) {
        if (value->empty()) return Status::NotFound("key deleted");
        return Status::OK();
    }

    // mem → imm → 磁盘:imm_ 比所有 SSTable 都新
    if (imm_ != nullptr && imm_->Contains(key, value)) {
        if (value->empty()) return Status::NotFound("key deleted");
        return Status::OK();
    }

    for (int level = 0; level < VersionSet::kMaxLevel; ++level) {
        const auto& files = version_set_->GetLevel(level);
        if (files.empty()) continue;

        if (level == 0) {
            for (auto it = files.rbegin(); it != files.rend(); ++it) {
                std::shared_ptr<SSTable> table;
                Status s = table_cache_.FindTable(it->file_number, options_.dbname, &table);
                if (!s.ok()) return s;
                s = table->Get(key, value);
                if (s.ok()) {
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

void KVDB::BackgroundWork() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        bg_cv_.wait(lock, [this]{ return imm_ != nullptr || bg_exit_; });

        if (imm_ != nullptr) {
            uint64_t file_number = version_set_->NewFileNumber();
            uint64_t old_log    = imm_log_number_;
            SkipList* imm       = imm_.get();

            lock.unlock();  // IO 放锁,前台 Get 可并发读 imm_
            FileMetaData meta;
            Status s = WriteLevel0Table(imm, file_number, &meta);
            lock.lock();

            if (s.ok()) {
                version_set_->AddFile(0, meta);
                version_set_->SetPrevLogNumber(0);
                s = version_set_->WriteSnapshot();
                if (s.ok()) {
                    char buf[128];
                    snprintf(buf, sizeof(buf), "%s/%06lu.log",
                             options_.dbname.c_str(), old_log);
                    remove(buf);
                }
                DEBUG_LOG("AsyncFlush done: file=%lu L0=%d",
                          file_number, version_set_->NumLevelFiles(0));
            } else {
                DEBUG_LOG("AsyncFlush FAILED: file=%lu", file_number);
            }
            imm_.reset();
            imm_done_cv_.notify_all();
        }

        if (bg_exit_ && imm_ == nullptr) return;
    }
}

Status KVDB::WriteLevel0Table(SkipList* imm, uint64_t file_number, FileMetaData* meta) {
    char sst_buf[128];
    snprintf(sst_buf, sizeof(sst_buf), "%s/%06lu.sst",
             options_.dbname.c_str(), file_number);

    SSTableBuilder builder(sst_buf, 4096);
    SkipList::Iterator iter(imm);
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

    meta->file_number = file_number;
    meta->file_size = file_size;
    meta->smallest = first_key;
    meta->largest = last_key;
    return Status::OK();
}

Status KVDB::TEST_FlushMemTable() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (imm_ != nullptr) {
        imm_done_cv_.wait(lock);
    }
    if (memtable_->ApproximateMemoryUsage() > 0) {
        SwitchToImmutableLocked();
        while (imm_ != nullptr) {
            imm_done_cv_.wait(lock);
        }
    }
    return Status::OK();
}

Status KVDB::CompactManually() {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto& l0_files = version_set_->GetLevel(0);
    if (l0_files.empty()) {
        return Status::OK();
    }

    std::vector<FileMetaData> inputs_l0 = l0_files;

    std::string smallest = inputs_l0[0].smallest;
    std::string largest = inputs_l0[0].largest;
    for (size_t i = 1; i < inputs_l0.size(); ++i) {
        if (inputs_l0[i].smallest < smallest) smallest = inputs_l0[i].smallest;
        if (inputs_l0[i].largest > largest) largest = inputs_l0[i].largest;
    }

    std::vector<FileMetaData> inputs_l1;
    const auto& l1_files = version_set_->GetLevel(1);
    for (const auto& meta : l1_files) {
        if (!(largest < meta.smallest || smallest > meta.largest)) {
            inputs_l1.push_back(meta);
        }
    }

    std::vector<std::shared_ptr<SSTable>> tables_to_keep_alive;
    std::vector<std::unique_ptr<Iterator>> iters;

    // P0 修复:L0 按"新→旧"顺序创建迭代器
    for (auto rit = inputs_l0.rbegin(); rit != inputs_l0.rend(); ++rit) {
        const auto& meta = *rit;
        std::shared_ptr<SSTable> table;
        Status s = table_cache_.FindTable(meta.file_number, options_.dbname, &table);
        if (!s.ok()) return s;
        tables_to_keep_alive.push_back(table);
        iters.emplace_back(table->NewIterator());
    }
    for (const auto& meta : inputs_l1) {
        std::shared_ptr<SSTable> table;
        Status s = table_cache_.FindTable(meta.file_number, options_.dbname, &table);
        if (!s.ok()) return s;
        tables_to_keep_alive.push_back(table);
        iters.emplace_back(table->NewIterator());
    }

    uint64_t new_file_number = version_set_->NewFileNumber();
    char sst_buf[128];
    snprintf(sst_buf, sizeof(sst_buf), "%s/%06lu.sst",
             options_.dbname.c_str(), new_file_number);

    SSTableBuilder builder(sst_buf, 4096);
    MergingIterator merge(std::move(iters));
    merge.SeekToFirst();

    std::string first_key, last_key;
    bool first = true;
    std::string last_output_key;

    while (merge.Valid()) {
        std::string cur_key = merge.key().ToString();

        if (cur_key != last_output_key) {
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

    for (const auto& meta : inputs_l0) {
        version_set_->RemoveFile(0, meta.file_number);
        table_cache_.Evict(meta.file_number);
    }
    for (const auto& meta : inputs_l1) {
        version_set_->RemoveFile(1, meta.file_number);
        table_cache_.Evict(meta.file_number);
    }

    if (first) {
        remove(sst_buf);
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

    // 先重放 prev_log(崩溃时 imm_ 还没落盘的那个旧 WAL)
    if (prev_log > 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s/%06lu.log", options_.dbname.c_str(), prev_log);
        Status s = RecoverLogFile(buf);
        if (!s.ok() && !s.IsNotFound()) return s;
    }

    if (log_number > 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s/%06lu.log", options_.dbname.c_str(), log_number);
        Status s = RecoverLogFile(buf);
        if (!s.ok() && !s.IsNotFound()) return s;

        std::unique_ptr<WAL> wal;
        s = WAL::OpenForAppend(buf, &wal);
        if (!s.ok()) return s;
        wal_ = std::move(wal);
        wal_filename_ = buf;
        return Status::OK();
    }

    return OpenNewWAL();
}

Status KVDB::RecoverLogFile(const std::string& filename) {
    std::unique_ptr<WAL> wal;
    Status s = WAL::OpenForRead(filename, &wal);
    if (s.IsNotFound()) return Status::OK();
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

    last_sequence_.store(max_seq + 1);
    return Status::OK();
}

}  // namespace kv
