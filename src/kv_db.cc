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

    db->bg_thread_ = std::thread(&KVDB::BackgroundWork, db.get());

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
    bg_cv_.notify_one();
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

// 任务②:节流核心 —— 先查空间再放行 → mem 满+后台在刷则等 → mem 满+后台空闲则切换
Status KVDB::MakeRoomForWrite(std::unique_lock<std::mutex>* lock) {
    while (true) {
        if (memtable_->ApproximateMemoryUsage() <= options_.write_buffer_size) {
            return Status::OK();
        }
        if (imm_ != nullptr) {
            imm_done_cv_.wait(*lock);
        } else {
            SwitchToImmutableLocked();
            return Status::OK();
        }
    }
}

// 任务②:mem → imm 切换 + WAL 切换(顺序 = 崩溃安全:prev_log 先随 snapshot 落盘)
void KVDB::SwitchToImmutableLocked() {
    wal_->Close();
    uint64_t old_log = version_set_->LogNumber();
    version_set_->SetPrevLogNumber(old_log);
    imm_log_number_ = old_log;

    imm_ = std::move(memtable_);
    memtable_.reset(new SkipList());

    Status s = OpenNewWAL();
    if (!s.ok()) DEBUG_LOG("SwitchToImmutable: open WAL failed");
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

// 后台主循环:先 flush(数据安全),再 compact(性能整理)
void KVDB::BackgroundWork() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        // 任务③:wait 谓词加 L0 触发条件 —— flush 完回到循环顶部会重查谓词,
        // 不需要额外 notify 也能发现该 compact 了
        bg_cv_.wait(lock, [this]{
            return imm_ != nullptr || bg_exit_ ||
                   version_set_->NumLevelFiles(0) >= kL0CompactionTrigger;
        });

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

        // 任务③:flush 完(或本就无 flush 而是被 L0 触发唤醒)检查是否该 compact
        if (version_set_->NumLevelFiles(0) >= kL0CompactionTrigger) {
            CompactL0ToL1Locked(&lock);
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

// 任务③:等后台彻底空闲(imm_ 刷完 + compaction 收尾)
Status KVDB::TEST_WaitForBackground() {
    std::unique_lock<std::mutex> lock(mutex_);
    WaitForIdleLocked(&lock);
    return Status::OK();
}

int KVDB::TEST_NumLevelFiles(int level) {
    std::lock_guard<std::mutex> lock(mutex_);
    return version_set_->NumLevelFiles(level);
}

// 任务③:手动 compact = 等后台空闲 → 走同一条 CompactL0ToL1Locked
Status KVDB::CompactManually() {
    std::unique_lock<std::mutex> lock(mutex_);
    WaitForIdleLocked(&lock);
    return CompactL0ToL1Locked(&lock);
}

// 任务③:谓词必须同时覆盖 imm_ 和 bg_compacting_,
// 只查 imm_ 会在 compact 提交半途提前放行调用方
void KVDB::WaitForIdleLocked(std::unique_lock<std::mutex>* lock) {
    while (imm_ != nullptr || bg_compacting_) {
        imm_done_cv_.wait(*lock);
    }
}

// 任务③核心:L0(全部)+ 重叠 L1 → 一个新 L1 文件。
// 锁节奏(与 flush 同款):持锁取现场 → 放锁做归并 IO → 持锁原子提交。
Status KVDB::CompactL0ToL1Locked(std::unique_lock<std::mutex>* lock) {
    const auto& l0_files = version_set_->GetLevel(0);
    if (l0_files.empty()) {
        return Status::OK();
    }

    // 占坑:等待方(CompactManually / TEST_WaitForBackground)凭它知道 compact 进行中
    bg_compacting_ = true;

    // ===== 持锁取现场:输入文件快照 + 新文件号(version_set_ 不是线程安全的) =====
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

    uint64_t new_file_number = version_set_->NewFileNumber();

    // ===== 放锁做 IO:打开输入表 + 归并 + 写新 SSTable =====
    // 放锁期间安全:version_set_ 未变、旧文件都在盘上,前台 Get 照常;
    // 窗口内绝不提前 return(错误用 break 攒在 s 里,出窗口统一处理)
    lock->unlock();

    Status s = Status::OK();
    uint64_t file_size = 0;
    std::string first_key, last_key;
    bool first = true;

    {
        // Iterator 内部只持有 SSTable 裸指针,必须额外保活 shared_ptr
        std::vector<std::shared_ptr<SSTable>> tables_to_keep_alive;
        std::vector<std::unique_ptr<Iterator>> iters;

        // P0 修复:L0 必须按"新→旧"顺序创建迭代器
        for (auto rit = inputs_l0.rbegin(); rit != inputs_l0.rend(); ++rit) {
            std::shared_ptr<SSTable> table;
            s = table_cache_.FindTable(rit->file_number, options_.dbname, &table);
            if (!s.ok()) break;
            tables_to_keep_alive.push_back(table);
            iters.emplace_back(table->NewIterator());
        }
        for (const auto& meta : inputs_l1) {
            if (!s.ok()) break;
            std::shared_ptr<SSTable> table;
            s = table_cache_.FindTable(meta.file_number, options_.dbname, &table);
            if (!s.ok()) break;
            tables_to_keep_alive.push_back(table);
            iters.emplace_back(table->NewIterator());
        }

        if (s.ok()) {
            char sst_buf[128];
            snprintf(sst_buf, sizeof(sst_buf), "%s/%06lu.sst",
                     options_.dbname.c_str(), new_file_number);

            SSTableBuilder builder(sst_buf, 4096);
            MergingIterator merge(std::move(iters));
            merge.SeekToFirst();

            std::string last_output_key;
            while (merge.Valid()) {
                std::string cur_key = merge.key().ToString();

                if (cur_key != last_output_key) {
                    if (merge.value().size() > 0) {
                        s = builder.Add(merge.key(), merge.value());
                        if (!s.ok()) break;
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

            if (s.ok()) {
                s = builder.Finish(&file_size);
            }
        }
    }

    // ===== 持锁提交:版本变更 + 驱逐缓存 + 删旧文件,一个临界区完成 =====
    lock->lock();

    if (s.ok()) {
        for (const auto& meta : inputs_l0) {
            version_set_->RemoveFile(0, meta.file_number);
            table_cache_.Evict(meta.file_number);
        }
        for (const auto& meta : inputs_l1) {
            version_set_->RemoveFile(1, meta.file_number);
            table_cache_.Evict(meta.file_number);
        }

        if (first) {
            // 全 tombstone:输出为空,新文件直接删、不进版本
            char sst_buf[128];
            snprintf(sst_buf, sizeof(sst_buf), "%s/%06lu.sst",
                     options_.dbname.c_str(), new_file_number);
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
        if (s.ok()) {
            // snapshot 落盘后才删旧 SSTable(与 WAL 同款崩溃安全顺序)
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
        }
    }

    // 收尾:无论成败都要清标志 + 通知,否则等待方睡死
    bg_compacting_ = false;
    imm_done_cv_.notify_all();

    DEBUG_LOG("KVDB::CompactL0ToL1: L0=%d L1=%d new_file=%lu",
              version_set_->NumLevelFiles(0), version_set_->NumLevelFiles(1),
              new_file_number);
    return s;
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
