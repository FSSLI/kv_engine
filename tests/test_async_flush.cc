// Week 3 任务②验收测试:双 MemTable + 后台异步 flush
// 跑法:cmake Debug 构建后 ./test_async_flush
// 注意:TestWriteStall 若卡住不动 = MakeRoomForWrite 的等待/通知写错了
#include <iostream>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <dirent.h>
#include "kv_db.h"

using namespace kv;

const std::string kTestDir = "/tmp/kv_async_test";

void CleanUp() {
    std::string cmd = "rm -rf " + kTestDir;
    int ret = system(cmd.c_str());
    (void)ret;
}

// 数目录里的 .sst 文件个数(不碰内部实现,黑盒验证 flush 确实发生了)
int CountSstFiles() {
    DIR* dir = opendir(kTestDir.c_str());
    if (!dir) return 0;
    int count = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        const char* name = entry->d_name;
        size_t len = strlen(name);
        if (len > 4 && strcmp(name + len - 4, ".sst") == 0) count++;
    }
    closedir(dir);
    return count;
}

std::string MakeKey(int i) {
    char buf[32];
    snprintf(buf, sizeof(buf), "key%06d", i);
    return buf;
}

std::string MakeValue(int i, size_t size = 64) {
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "value%06d-", i);
    return std::string(prefix) + std::string(size > strlen(prefix) ? size - strlen(prefix) : 0, 'x');
}

// 大量写入触发多次自动切换,数据必须全部可读
void TestDataSurvivesAsyncFlush() {
    std::cout << "=== TestDataSurvivesAsyncFlush ===" << std::endl;
    CleanUp();

    KVDB::Options options;
    options.dbname = kTestDir;
    options.write_buffer_size = 4 * 1024;  // 4KB,逼出几十次切换

    std::unique_ptr<KVDB> db;
    Status s = KVDB::Open(options, &db);
    assert(s.ok());

    const int kNumKeys = 2000;
    for (int i = 0; i < kNumKeys; ++i) {
        s = db->Put(MakeKey(i), MakeValue(i));
        assert(s.ok());
    }

    for (int i = 0; i < kNumKeys; ++i) {
        std::string value;
        s = db->Get(MakeKey(i), &value);
        assert(s.ok());
        assert(value == MakeValue(i));
    }

    assert(CountSstFiles() >= 2);  // 确实发生过多次 flush
    std::cout << "  PASS: 2000 keys survive multiple async flushes, sst="
              << CountSstFiles() << std::endl;
}

// 边写边读:正在被后台落盘的数据也不能"消失"(Get 的 imm_ 分支)
void TestReadYourWritesDuringFlush() {
    std::cout << "=== TestReadYourWritesDuringFlush ===" << std::endl;
    CleanUp();

    KVDB::Options options;
    options.dbname = kTestDir;
    options.write_buffer_size = 2 * 1024;

    std::unique_ptr<KVDB> db;
    Status s = KVDB::Open(options, &db);
    assert(s.ok());

    const int kNumKeys = 600;
    for (int i = 0; i < kNumKeys; ++i) {
        s = db->Put(MakeKey(i), MakeValue(i));
        assert(s.ok());
        // 每写一条,立刻回读最近 50 条 —— 它们可能落在 mem_/imm_/L0 任意一层
        for (int j = std::max(0, i - 50); j <= i; ++j) {
            std::string value;
            s = db->Get(MakeKey(j), &value);
            assert(s.ok());
            assert(value == MakeValue(j));
        }
    }
    std::cout << "  PASS: read-your-writes during flush" << std::endl;
}

// 写满 mem + imm 在刷 → 写线程被节流,但必须能继续推进(等死=挂)
void TestWriteStallMakesProgress() {
    std::cout << "=== TestWriteStallMakesProgress ===" << std::endl;
    CleanUp();

    KVDB::Options options;
    options.dbname = kTestDir;
    options.write_buffer_size = 1024;  // 1KB:几条就写满,节流路径被反复碾压

    std::unique_ptr<KVDB> db;
    Status s = KVDB::Open(options, &db);
    assert(s.ok());

    const int kNumKeys = 300;
    for (int i = 0; i < kNumKeys; ++i) {
        s = db->Put(MakeKey(i), MakeValue(i, 200));
        assert(s.ok());
    }

    for (int i = 0; i < kNumKeys; ++i) {
        std::string value;
        s = db->Get(MakeKey(i), &value);
        assert(s.ok());
        assert(value == MakeValue(i, 200));
    }

    assert(CountSstFiles() >= 5);
    std::cout << "  PASS: write stall makes progress, sst=" << CountSstFiles()
              << std::endl;
}

// 1 写 3 读并发:读线程反复读已确认存在的 key,一次都不许丢
void TestConcurrentReadWrite() {
    std::cout << "=== TestConcurrentReadWrite ===" << std::endl;
    CleanUp();

    KVDB::Options options;
    options.dbname = kTestDir;
    options.write_buffer_size = 4 * 1024;

    std::unique_ptr<KVDB> db;
    Status s = KVDB::Open(options, &db);
    assert(s.ok());

    // 预写 200 条,读线程只读这些(必然存在)
    const int kPreload = 200;
    for (int i = 0; i < kPreload; ++i) {
        s = db->Put(MakeKey(i), MakeValue(i));
        assert(s.ok());
    }

    std::atomic<bool> stop{false};
    std::atomic<int> read_failures{0};

    auto reader = [&]() {
        std::string value;
        while (!stop.load()) {
            for (int i = 0; i < kPreload; ++i) {
                Status rs = db->Get(MakeKey(i), &value);
                if (!rs.ok() || value != MakeValue(i)) {
                    read_failures++;
                }
            }
        }
    };

    std::vector<std::thread> readers;
    for (int t = 0; t < 3; ++t) readers.emplace_back(reader);

    // 写线程:持续制造 flush
    for (int i = 0; i < 1500; ++i) {
        Status ws = db->Put("w" + MakeKey(i), MakeValue(i));
        assert(ws.ok());
    }

    stop.store(true);
    for (auto& t : readers) t.join();

    assert(read_failures.load() == 0);
    std::cout << "  PASS: concurrent read/write, zero read failures" << std::endl;
}

// 部分数据已落盘 + 部分还在 WAL,关闭重开后都必须在
void TestRecoveryAcrossAsyncFlush() {
    std::cout << "=== TestRecoveryAcrossAsyncFlush ===" << std::endl;
    CleanUp();

    const int kFlushed = 300;
    const int kInWal = 100;

    {
        KVDB::Options options;
        options.dbname = kTestDir;
        options.write_buffer_size = 2 * 1024;

        std::unique_ptr<KVDB> db;
        Status s = KVDB::Open(options, &db);
        assert(s.ok());

        for (int i = 0; i < kFlushed; ++i) {
            s = db->Put(MakeKey(i), MakeValue(i));
            assert(s.ok());
        }
        s = db->TEST_FlushMemTable();  // 同步屏障:这些一定在 L0 了
        assert(s.ok());

        for (int i = kFlushed; i < kFlushed + kInWal; ++i) {
            s = db->Put(MakeKey(i), MakeValue(i));
            assert(s.ok());
        }
        // db 析构:memtable_ 里这 100 条只在 WAL 里
    }

    {
        KVDB::Options options;
        options.dbname = kTestDir;
        options.write_buffer_size = 2 * 1024;

        std::unique_ptr<KVDB> db;
        Status s = KVDB::Open(options, &db);
        assert(s.ok());

        for (int i = 0; i < kFlushed + kInWal; ++i) {
            std::string value;
            s = db->Get(MakeKey(i), &value);
            assert(s.ok());
            assert(value == MakeValue(i));
        }
    }
    std::cout << "  PASS: recovery across async flush (sst + wal)" << std::endl;
}

// flush 正在进行时直接析构:不崩、不挂、重开数据一条不少
void TestGracefulShutdown() {
    std::cout << "=== TestGracefulShutdown ===" << std::endl;
    CleanUp();

    const int kNumKeys = 800;
    {
        KVDB::Options options;
        options.dbname = kTestDir;
        options.write_buffer_size = 1024;  // 高频切换,析构时大概率有 imm_ 在飞

        std::unique_ptr<KVDB> db;
        Status s = KVDB::Open(options, &db);
        assert(s.ok());

        for (int i = 0; i < kNumKeys; ++i) {
            s = db->Put(MakeKey(i), MakeValue(i, 200));
            assert(s.ok());
        }
        // 立刻析构 —— 后台可能正在落盘
    }

    {
        KVDB::Options options;
        options.dbname = kTestDir;
        options.write_buffer_size = 1024;

        std::unique_ptr<KVDB> db;
        Status s = KVDB::Open(options, &db);
        assert(s.ok());

        for (int i = 0; i < kNumKeys; ++i) {
            std::string value;
            s = db->Get(MakeKey(i), &value);
            assert(s.ok());
            assert(value == MakeValue(i, 200));
        }
    }
    std::cout << "  PASS: graceful shutdown with flush in flight" << std::endl;
}

int main() {
    TestDataSurvivesAsyncFlush();
    TestReadYourWritesDuringFlush();
    TestWriteStallMakesProgress();
    TestConcurrentReadWrite();
    TestRecoveryAcrossAsyncFlush();
    TestGracefulShutdown();
    std::cout << "\nAll async flush tests passed!" << std::endl;
    return 0;
}
