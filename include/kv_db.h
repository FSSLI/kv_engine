#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "memtable.h"
#include "lru_cache.h"
#include "sstable.h"
#include "status.h"
#include "version_set.h"
#include "wal.h"
#include "write_batch.h"

namespace kv {

class KVDB {
public:
    struct Options {
        size_t write_buffer_size = 4 * 1024 * 1024;
        size_t block_cache_capacity = 8 * 1024 * 1024;  // BlockCache 容量,0 = 关闭
        std::string dbname = "/tmp/kv_db";
    };

    static Status Open(const Options& options, std::unique_ptr<KVDB>* dbptr);
    ~KVDB();

    Status Put(const std::string& key, const std::string& value);
    Status Get(const std::string& key, std::string* value);
    Status Delete(const std::string& key);
    Status Write(WriteBatch* batch);

    // 测试辅助:强制把当前 memtable 刷成 L0 SSTable,并等后台落盘完成
    Status TEST_FlushMemTable();

    Status CompactManually();

private:
    explicit KVDB(const Options& options);

    // ===== Week 3 任务②:双 MemTable + 后台异步 flush =====
    //
    // 写路径分工(对照 LevelDB DBImpl::MakeRoomForWrite 的简化版):
    //   Write → MakeRoomForWrite(满了就切 imm_/必要时节流) → 写 WAL → 写 memtable_
    //   后台线程 BackgroundWork:拿 imm_ → WriteLevel0Table(纯 IO,不持锁) → 收尾
    //
    // imm_ 生命周期(全部在 mutex_ 保护下变更):
    //   nullptr --[MakeRoomForWrite 切换]--> 非空 --[BackgroundWork 落盘完成]--> nullptr
    Status MakeRoomForWrite(std::unique_lock<std::mutex>* lock);
    void   SwitchToImmutableLocked();   // 前提:调用方已持 mutex_
    void   BackgroundWork();            // 后台线程主循环
    // imm → SSTable 纯 IO。前提:调用时不持锁;imm 只读。
    // file_number 由调用方(持锁时)提前分配,meta 为出参。
    Status WriteLevel0Table(SkipList* imm, uint64_t file_number, FileMetaData* meta);

    Status OpenNewWAL();

    Status Recover();
    Status RecoverLogFile(const std::string& filename);

    Options options_;
    std::mutex mutex_;

    std::unique_ptr<SkipList> memtable_;
    std::unique_ptr<SkipList> imm_;     // 只读 memtable,等待后台落盘;nullptr = 空闲

    std::unique_ptr<WAL> wal_;
    std::string wal_filename_;
    std::atomic<uint64_t> last_sequence_{1};

    // 任务②:后台线程与同步原语
    std::thread bg_thread_;
    std::condition_variable bg_cv_;         // 前台 → 后台:有 imm_ 待刷 / 该退出了
    std::condition_variable imm_done_cv_;   // 后台 → 前台:imm_ 已清空
    bool bg_exit_ = false;                  // 置位后后台干完手头的活就退出
    uint64_t imm_log_number_ = 0;           // imm_ 对应的旧 WAL 文件号(落盘后凭它删文件)

    std::unique_ptr<VersionSet> version_set_;
    // 注意声明顺序:block_cache_ 必须在 table_cache_ 之前。
    // KVDB 析构时成员逆序销毁,SSTable(经 TableCache 持有)引用
    // block_cache_ 的裸指针,缓存必须比引用它的 SSTable 活得久。
    std::unique_ptr<LRUCache> block_cache_;
    TableCache table_cache_;
};

}  // namespace kv
