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
    // BlockCache:所有 SSTable 读共享一份缓存,0 表示关闭
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
    wal->SetSyncOnWrite(wal_sync_on_write_);  // 轮换出的新 WAL 继承压测开关
    wal_ = std::move(wal);

    version_set_->SetLogNumber(log_number);
    return version_set_->WriteSnapshot();  // 持久化 log_number(及此前设置的 prev_log)
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
    std::unique_lock<std::mutex> lock(mutex_);  // cv.wait 需要 unique_lock

    if (!batch || batch->Count() == 0) {
        return Status::InvalidArgument("empty batch");
    }

    // 任务②:写前先确保 memtable_ 有空间(满了切 imm_,imm_ 在刷则节流等待)
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

// ============================================================
// TODO(马超) #1:写前确保 mem 有空间 —— 「节流」核心
//
// 伪代码(三个分支的顺序是考点!):
//   while (true) {
//       分支1: if (memtable_->ApproximateMemoryUsage()
//                     <= options_.write_buffer_size)
//                  return Status::OK();       // 有空间:直接放行
//              ⚠️ 必须先查空间!即使 imm_ 正在后台刷,只要 mem 没满
//                 就不等 —— 否则每次写都陪等 IO,异步失去意义。
//       分支2: if (imm_ != nullptr)
//                  imm_done_cv_.wait(*lock); // mem 满 + 后台在刷:等
//              // wait 会原子释放锁,被唤醒后重新拿锁、回到循环顶部复查
//       分支3: else {                         // mem 满 + 后台空闲:切换
//                  SwitchToImmutableLocked();
//                  return Status::OK();       // 新 mem 是空的,必有空间
//              }
//   }
//
// 对应测试:TestWriteStallMakesProgress(死等会超时)、
//           TestDataSurvivesAsyncFlush
// ============================================================
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

// ============================================================
// TODO(马超) #2:mem → imm 切换 + WAL 切换(调用方已持 mutex_)
//
// 伪代码(顺序 = 崩溃安全,不要乱):
//   1. wal_->Close();
//   2. uint64_t old_log = version_set_->LogNumber();
//      version_set_->SetPrevLogNumber(old_log);  // manifest 记住旧 log 还有数据在 imm_
//      imm_log_number_ = old_log;                // 落盘后凭它删文件
//   3. imm_ = std::move(memtable_);
//      memtable_.reset(new SkipList());
//   4. Status s = OpenNewWAL();
//      // 现有函数:分配新 log_number + WriteSnapshot,
//      // snapshot 会把 prev_log + 新 log 一起原子持久化
//      if (!s.ok()) DEBUG_LOG("SwitchToImmutable: open WAL failed: %s", ...);
//   5. bg_cv_.notify_one();   // 唤醒后台 flush
//
// 面试必答:为什么 prev_log 必须先随新 WAL 的 snapshot 落盘?
//   崩溃窗口:若新 log 已生效而 prev_log 没记住,崩溃后旧 log 无人重放,
//   imm_ 里的数据直接丢失。Recover() 已实现按 prev_log → log 顺序重放。
// ============================================================
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

    // 任务②:mem → imm → 磁盘。imm_ 比所有 SSTable 都新
    if (imm_ != nullptr && imm_->Contains(key, value)) {
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

// ============================================================
// TODO(马超) #4:后台线程主循环 —— 本任务最核心的一段
//
// 骨架(锁的收放是考点):
//   std::unique_lock<std::mutex> lock(mutex_);
//   while (true) {
//       // 1. 带谓词的 wait(防虚假唤醒 + 防通知丢失):
//       bg_cv_.wait(lock, [this]{ return imm_ != nullptr || bg_exit_; });
//
//       // 2. 有活干活
//       if (imm_ != nullptr) {
//           // a. 持锁取现场(NewFileNumber 动 VersionSet,必须持锁)
//           uint64_t file_number = version_set_->NewFileNumber();
//           uint64_t old_log    = imm_log_number_;
//           SkipList* imm       = imm_.get();
//
//           // b. 释放锁做 IO —— imm_ 只读,前台可并发 Get 它、写新 mem
//           lock.unlock();
//           FileMetaData meta;
//           Status s = WriteLevel0Table(imm, file_number, &meta);
//           lock.lock();
//
//           // c. 持锁收尾
//           if (s.ok()) {
//               version_set_->AddFile(0, meta);
//               version_set_->SetPrevLogNumber(0);
//               s = version_set_->WriteSnapshot();
//               // 先 snapshot 再删旧 WAL(崩溃安全:
//               // 删早了丢数据,留久了只是重放一遍、幂等无害)
//               if (s.ok()) {
//                   char buf[128];
//                   snprintf(buf, sizeof(buf), "%s/%06lu.log",
//                            options_.dbname.c_str(), old_log);
//                   remove(buf);
//               }
//               DEBUG_LOG("AsyncFlush done: file=%lu L0=%d",
//                         file_number, version_set_->NumLevelFiles(0));
//           } else {
//               // 简化:IO 失败也清 imm_ 放行写线程(LevelDB 会转只读模式)
//               DEBUG_LOG("AsyncFlush FAILED: file=%lu", file_number);
//           }
//           imm_.reset();
//           imm_done_cv_.notify_all();   // 叫醒被节流的写线程
//       }
//
//       // 3. 退出条件:要求退出 且 手头的活已干完
//       if (bg_exit_ && imm_ == nullptr) return;
//   }
//
// 面试三连:
//   - wait 为什么带谓词?(虚假唤醒;且 notify 时若条件已满足不该睡死)
//   - IO 为什么释放锁?(不然前台 Get/Put 全程陪等,异步白做)
//   - imm_.reset() 为什么要持锁?(写线程 wait 的谓词在读它)
// ============================================================
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

            lock.unlock();
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

// 完整实现已给:imm → SSTable 的纯 IO 部分(从旧 SwitchMemTable 搬迁)。
// 前提:调用时不持锁;imm 只读,遍历期间不会有并发写。
Status KVDB::WriteLevel0Table(SkipList* imm, uint64_t file_number, FileMetaData* meta) {
    char sst_buf[128];
    snprintf(sst_buf, sizeof(sst_buf), "%s/%06lu.sst",
             options_.dbname.c_str(), file_number);

    SSTableBuilder builder(sst_buf, 4096, options_.bloom_bits_per_key);
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

// 测试辅助(已写好):强制切换并【同步等】后台落盘完成。
// 注意它本身就是对你 TODO #1/#2/#4 的一次端到端调用。
Status KVDB::TEST_FlushMemTable() {
    std::unique_lock<std::mutex> lock(mutex_);
    // 等上一轮 imm_ 刷完(若有)
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

// 任务③测试辅助:等后台彻底空闲(imm_ 刷完 + compaction 收尾)。
Status KVDB::TEST_WaitForBackground() {
    std::unique_lock<std::mutex> lock(mutex_);
    WaitForIdleLocked(&lock);
    return Status::OK();
}

// 任务③测试辅助:断言某层文件数用。
int KVDB::TEST_NumLevelFiles(int level) {
    std::lock_guard<std::mutex> lock(mutex_);
    return version_set_->NumLevelFiles(level);
}

// 压测辅助:记录开关并转发给当前 WAL;之后轮换出的新 WAL 在 OpenNewWAL 里继承。
// 持锁与 Put 互斥,避免切换瞬间撞上 wal_ 替换。
void KVDB::SetWALSyncOnWrite(bool on) {
    std::lock_guard<std::mutex> lock(mutex_);
    wal_sync_on_write_ = on;
    wal_->SetSyncOnWrite(on);
}

// 任务③重构:手动 compact 变成"等后台空闲 → 走同一条 CompactL0ToL1Locked"的壳。
// 行为对外不变(L0 空直接返回、返回前 compact 完成),只是不再自己持锁从头干到尾。
Status KVDB::CompactManually() {
    std::unique_lock<std::mutex> lock(mutex_);
    WaitForIdleLocked(&lock);
    return CompactL0ToL1Locked(&lock);
}

void KVDB::WaitForIdleLocked(std::unique_lock<std::mutex>* lock) {
    while (imm_ != nullptr || bg_compacting_) {
        imm_done_cv_.wait(*lock);
    }
}

// 任务③核心:L0(全部)+ 重叠 L1 → 一个新 L1 文件。
// 锁节奏(与 flush 同款):持锁取现场 → 放锁做归并 IO → 持锁原子提交版本变更。
// 放锁期间 version_set_ 未变、旧文件都在盘上,前台 Get 照常工作。
Status KVDB::CompactL0ToL1Locked(std::unique_lock<std::mutex>* lock) {
    const auto& l0_files = version_set_->GetLevel(0);
    if (l0_files.empty()) {
        return Status::OK();
    }

    // 占坑:等待方(CompactManually / TEST_WaitForBackground)凭它知道 compact 进行中
    bg_compacting_ = true;

    // ===== 持锁取现场:输入文件快照 + 新文件号(version_set_ 不是线程安全的) =====
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

    // 新文件号也必须在持锁阶段分配(NewFileNumber 动 version_set_)
    uint64_t new_file_number = version_set_->NewFileNumber();

    // ===== 放锁做 IO:打开输入表 + 归并 + 写新 SSTable =====
    lock->unlock();

    Status s = Status::OK();
    uint64_t file_size = 0;
    std::string first_key, last_key;
    bool first = true;

    {
        // 4. 打开所有文件，创建 MergingIterator
        // 注意：Iterator 内部只持有 SSTable 裸指针，必须额外保活 shared_ptr
        std::vector<std::shared_ptr<SSTable>> tables_to_keep_alive;
        std::vector<std::unique_ptr<Iterator>> iters;

        // 修复(P0):L0 必须按"新→旧"顺序创建迭代器。
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

        // 5. 归并输出到新的 L1 SSTable
        if (s.ok()) {
            char sst_buf[128];
            snprintf(sst_buf, sizeof(sst_buf), "%s/%06lu.sst",
                     options_.dbname.c_str(), new_file_number);

            SSTableBuilder builder(sst_buf, 4096, options_.bloom_bits_per_key);
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
        // 6. 更新 VersionSet
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
            // 7. snapshot 落盘后才删旧 SSTable(与 WAL 同款崩溃安全顺序)
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

    // 收尾:无论成败都要清标志 + 通知,否则等待方睡死(与任务②析构 notify 同款教训)
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

    // 先重放 prev_log(任务②:对应崩溃时 imm_ 还没落盘的那个旧 WAL)
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
