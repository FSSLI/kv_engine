// ReadAt 竞态复现:共享一个 SSTable 实例,多线程并发读,
// fseek+fread 非原子 → 读到错误偏移 → 内容校验失败。
// 修复(pread)后:两轮都应为 0 错误。
#define private public  // 仅为复现程序打开 ReadAt 访问
#include "sstable.h"
#undef private
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <memory>
#include <sys/stat.h>

using namespace kv;

static const char* kFile = "/tmp/race_repro.sst";

std::string MakeKey(int i) {
    char buf[32];
    snprintf(buf, sizeof(buf), "key%06d", i);
    return buf;
}
std::string MakeVal(int i) {
    char buf[32];
    snprintf(buf, sizeof(buf), "val%06d-", i);
    return std::string(buf) + std::string(90, 'x');
}

int main() {
    remove(kFile);

    // 1. 造一个足够大的 SSTable(多块,~5MB)
    const int kNumKeys = 40000;
    {
        SSTableBuilder builder(kFile, 4096, 10);  // 和产线一致:bloom=10
        for (int i = 0; i < kNumKeys; ++i) {
            Status s = builder.Add(MakeKey(i), MakeVal(i));
            if (!s.ok()) { printf("build add fail\n"); return 1; }
        }
        uint64_t size = 0;
        Status s = builder.Finish(&size);
        if (!s.ok()) { printf("build finish fail\n"); return 1; }
        printf("built %s, size=%lu\n", kFile, size);
    }

    // 2. 打开,拿共享实例(复刻 TableCache 共享语义)
    std::unique_ptr<SSTable> t;
    Status s = SSTable::Open(kFile, &t, /*file_number=*/1, /*block_cache=*/nullptr);
    if (!s.ok()) { printf("open fail\n"); return 1; }
    std::shared_ptr<SSTable> table(std::move(t));

    // ---- Phase A: prod 式 —— 4 线程 Get 不同块的 key,校验值 ----
    {
        std::atomic<long> errors{0};
        const int keys[4] = {5, 10005, 20005, 30005};  // 必然落在不同块
        std::vector<std::thread> ts;
        for (int ti = 0; ti < 4; ++ti) {
            ts.emplace_back([&, ti]() {
                std::string v;
                std::string expect = MakeVal(keys[ti]);
                for (int it = 0; it < 100000; ++it) {
                    Status rs = table->Get(MakeKey(keys[ti]), &v);
                    if (!rs.ok() || v != expect) errors++;
                }
            });
        }
        for (auto& th : ts) th.join();
        printf("Phase A (Get x4 threads): errors=%ld\n", errors.load());
    }

    // ---- Phase B: 机制直证 —— 2 线程 ReadAt 读不同偏移,逐字节校验 ----
    {
        struct stat st;
        stat(kFile, &st);
        size_t fsize = static_cast<size_t>(st.st_size);
        size_t chunk = fsize / 2;

        // 单线程参考内容
        std::string ref0, ref1;
        {
            SSTable::Open(kFile, &t, 1, nullptr);  // 重新借用接口读参考
        }
        std::unique_ptr<SSTable> t2;
        SSTable::Open(kFile, &t2, 1, nullptr);
        t2->ReadAt(0, chunk, &ref0);
        t2->ReadAt(chunk, fsize - chunk, &ref1);

        std::atomic<long> mismatch{0};
        std::thread a([&]() {
            std::string buf;
            for (int it = 0; it < 3000; ++it) {
                table->ReadAt(0, chunk, &buf);
                if (buf != ref0) mismatch++;
            }
        });
        std::thread b([&]() {
            std::string buf;
            for (int it = 0; it < 3000; ++it) {
                table->ReadAt(chunk, fsize - chunk, &buf);
                if (buf != ref1) mismatch++;
            }
        });
        a.join(); b.join();
        printf("Phase B (ReadAt x2 threads): mismatches=%ld\n", mismatch.load());
    }

    return 0;
}
