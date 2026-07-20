// Week 3 任务③验收测试:自动 Compaction
// 跑法:cmake Debug 构建后 ./test_auto_compact
// 没填 TODO 前:TestAutoCompactionTriggers 的 L1 断言会挂(L0 只增不减)
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

const std::string kTestDir = "/tmp/kv_compact_test";

void CleanUp() {
    std::string cmd = "rm -rf " + kTestDir;
    int ret = system(cmd.c_str());
    (void)ret;
}

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

KVDB::Options TestOptions() {
    KVDB::Options options;
    options.dbname = kTestDir;
    options.write_buffer_size = 4 * 1024;  // 4KB:600 个 key 能逼出十几次 flush
    return options;
}

// 核心:写够 5+ 个 L0 文件后,后台必须自动 compact,且数据不丢
void TestAutoCompactionTriggers() {
    std::cout << "=== TestAutoCompactionTriggers ===" << std::endl;
    CleanUp();

    std::unique_ptr<KVDB> db;
    Status s = KVDB::Open(TestOptions(), &db);
    assert(s.ok());

    const int kNumKeys = 600;  // ~13 次 flush → 触发多轮自动 compaction
    for (int i = 0; i < kNumKeys; ++i) {
        s = db->Put(MakeKey(i), MakeValue(i));
        assert(s.ok());
    }

    s = db->TEST_WaitForBackground();
    assert(s.ok());

    int l0 = db->TEST_NumLevelFiles(0);
    int l1 = db->TEST_NumLevelFiles(1);
    // compact 一次吃掉全部 L0:最终 L0 一定低于触发阈值,且 L1 有产物
    assert(l0 >= 0 && l0 < 4);
    assert(l1 >= 1);
    // 盘上文件数 == L0 + L1(旧文件已被清理,没有孤儿)
    assert(CountSstFiles() == l0 + l1);

    for (int i = 0; i < kNumKeys; ++i) {
        std::string value;
        s = db->Get(MakeKey(i), &value);
        assert(s.ok());
        assert(value == MakeValue(i));
    }

    std::cout << "  PASS: auto compaction triggered, L0=" << l0
              << " L1=" << l1 << " all 600 keys intact" << std::endl;
}

// 同一批 key 反复覆盖写(更新分散在多个 L0 文件里),compact 后新值必须赢
void TestNewestValueWinsAfterAutoCompaction() {
    std::cout << "=== TestNewestValueWinsAfterAutoCompaction ===" << std::endl;
    CleanUp();

    std::unique_ptr<KVDB> db;
    Status s = KVDB::Open(TestOptions(), &db);
    assert(s.ok());

    const int kNumKeys = 200;
    // 第一轮:v1(足够 flush 出多个 L0)
    for (int i = 0; i < kNumKeys; ++i) {
        s = db->Put(MakeKey(i), "v1-" + MakeValue(i));
        assert(s.ok());
    }
    // 第二轮:同 key 覆盖 v2
    for (int i = 0; i < kNumKeys; ++i) {
        s = db->Put(MakeKey(i), "v2-" + MakeValue(i));
        assert(s.ok());
    }

    s = db->TEST_WaitForBackground();
    assert(s.ok());
    assert(db->TEST_NumLevelFiles(1) >= 1);  // 确实发生过自动 compaction

    for (int i = 0; i < kNumKeys; ++i) {
        std::string value;
        s = db->Get(MakeKey(i), &value);
        if (!s.ok() || value != "v2-" + MakeValue(i)) {
            fprintf(stderr, "FAIL key=%s status=%s value=%.30s L0=%d L1=%d\n",
                    MakeKey(i).c_str(), s.ok() ? "ok" : "notfound/err",
                    value.c_str(), db->TEST_NumLevelFiles(0), db->TEST_NumLevelFiles(1));
        }
        assert(s.ok());
        assert(value == "v2-" + MakeValue(i));
    }

    std::cout << "  PASS: newest value wins across auto-compacted L0 files" << std::endl;
}

// 删除标记必须挺过自动 compaction,且重开后依然有效(manifest 持久化正确)
void TestTombstonesSurviveAutoCompaction() {
    std::cout << "=== TestTombstonesSurviveAutoCompaction ===" << std::endl;
    CleanUp();

    {
        std::unique_ptr<KVDB> db;
        Status s = KVDB::Open(TestOptions(), &db);
        assert(s.ok());

        const int kNumKeys = 600;
        for (int i = 0; i < kNumKeys; ++i) {
            s = db->Put(MakeKey(i), MakeValue(i));
            assert(s.ok());
        }
        for (int i = 0; i < kNumKeys; i += 2) {  // 删一半
            s = db->Delete(MakeKey(i));
            assert(s.ok());
        }
        // 再写一批新 key,把含 tombstone 的 memtable 也逼进 L0 + 触发 compact
        for (int i = 600; i < 900; ++i) {
            s = db->Put(MakeKey(i), MakeValue(i));
            assert(s.ok());
        }
        s = db->TEST_WaitForBackground();
        assert(s.ok());
        assert(db->TEST_NumLevelFiles(1) >= 1);
    }  // 关库

    // 重开:manifest 里应只有 compact 后的文件
    std::unique_ptr<KVDB> db;
    Status s = KVDB::Open(TestOptions(), &db);
    assert(s.ok());

    for (int i = 0; i < 900; ++i) {
        std::string value;
        s = db->Get(MakeKey(i), &value);
        if (i < 600 && i % 2 == 0) {
            assert(s.IsNotFound());        // 删过的必须查不到
        } else {
            assert(s.ok());
            assert(value == MakeValue(i));
        }
    }

    std::cout << "  PASS: tombstones survive auto compaction + reopen" << std::endl;
}

// compaction 放锁窗口期间,前台读必须照常工作(读不阻塞、结果正确)
void TestReadDuringAutoCompaction() {
    std::cout << "=== TestReadDuringAutoCompaction ===" << std::endl;
    CleanUp();

    std::unique_ptr<KVDB> db;
    Status s = KVDB::Open(TestOptions(), &db);
    assert(s.ok());

    // 锚点 key:先落盘且永不删除,读者全程盯着它们
    const int kAnchors = 50;
    for (int i = 0; i < kAnchors; ++i) {
        s = db->Put(MakeKey(i), MakeValue(i));
        assert(s.ok());
    }
    s = db->TEST_FlushMemTable();
    assert(s.ok());

    std::atomic<bool> stop{false};
    std::atomic<int> read_errors{0};

    auto reader = [&]() {
        std::string value;
        while (!stop.load()) {
            for (int i = 0; i < kAnchors && !stop.load(); ++i) {
                Status rs = db->Get(MakeKey(i), &value);
                if (!rs.ok() || value != MakeValue(i)) {
                    read_errors.fetch_add(1);
                }
            }
        }
    };

    std::thread r1(reader), r2(reader);

    // 写线程:持续制造 flush + 自动 compaction
    for (int i = 0; i < 800; ++i) {
        Status ws = db->Put("w" + MakeKey(i), MakeValue(i));
        assert(ws.ok());
    }

    stop.store(true);
    r1.join();
    r2.join();

    s = db->TEST_WaitForBackground();
    assert(s.ok());
    assert(db->TEST_NumLevelFiles(1) >= 1);  // 确实 compact 过
    assert(read_errors.load() == 0);         // 读的全程零错误

    std::cout << "  PASS: concurrent reads clean during auto compaction" << std::endl;
}

// 手动入口在自动 compact 时代依然可用(等后台空闲、空 L0 直接返回)
void TestManualCompactStillWorks() {
    std::cout << "=== TestManualCompactStillWorks ===" << std::endl;
    CleanUp();

    std::unique_ptr<KVDB> db;
    Status s = KVDB::Open(TestOptions(), &db);
    assert(s.ok());

    for (int i = 0; i < 600; ++i) {
        s = db->Put(MakeKey(i), MakeValue(i));
        assert(s.ok());
    }
    s = db->TEST_WaitForBackground();
    assert(s.ok());

    // 自动 compact 后 L0 < 4,手动再压一次:要么空 L0 直接返回,要么把剩下的也压掉
    s = db->CompactManually();
    assert(s.ok());
    assert(db->TEST_NumLevelFiles(0) == 0);

    for (int i = 0; i < 600; ++i) {
        std::string value;
        s = db->Get(MakeKey(i), &value);
        assert(s.ok());
        assert(value == MakeValue(i));
    }

    std::cout << "  PASS: CompactManually still works after auto compactions" << std::endl;
}

int main() {
    TestAutoCompactionTriggers();
    TestNewestValueWinsAfterAutoCompaction();
    TestTombstonesSurviveAutoCompaction();
    TestReadDuringAutoCompaction();
    TestManualCompactStillWorks();

    std::cout << "\nAll tests passed!" << std::endl;
    CleanUp();
    return 0;
}
