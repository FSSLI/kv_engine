#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "memtable.h"
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
        std::string dbname = "/tmp/kv_db";
    };

    static Status Open(const Options& options, std::unique_ptr<KVDB>* dbptr);
    ~KVDB();

    Status Put(const std::string& key, const std::string& value);
    Status Get(const std::string& key, std::string* value);
    Status Delete(const std::string& key);
    Status Write(WriteBatch* batch);

    // 测试辅助
    Status TEST_FlushMemTable();

    Status CompactManually();

private:
    explicit KVDB(const Options& options);

    Status SwitchMemTable();
    Status OpenNewWAL();

    Status Recover();
    Status RecoverLogFile(const std::string& filename);

    Options options_;
    std::mutex mutex_;

    std::unique_ptr<SkipList> memtable_;
    std::unique_ptr<WAL> wal_;
    std::string wal_filename_;
    std::atomic<uint64_t> last_sequence_{1};

    std::unique_ptr<VersionSet> version_set_;
    TableCache table_cache_;
};

}  // namespace kv