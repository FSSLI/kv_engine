// bench_kv.cc — KV 引擎基准测试(db_bench 风格)
//
// 用法:
//   ./bench_kv --num=1000000 --benchmarks=fillseq,readrandom,readmissing --sync=0
//   ./bench_kv --num=100000  --benchmarks=fillseq --sync=1   # 保持久化的诚实数字
//   ./bench_kv --num=1000000 --benchmarks=readmissing --bloom_bits=0 --sync=0
//
// 输出:每个 benchmark 的 us/op、ops/sec、p50/p99 延迟。
// 注意:必须用 Release 构建(-O2),Debug 数字无意义。
// --sync=1(默认): 每次 Put 后 WAL fsync,测的是"保数据"口径,慢但诚实
// --sync=0:        关 fsync,测纯引擎吞吐(业界压测标准做法,如 RocksDB disableWAL)

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "kv_db.h"

using Clock = std::chrono::steady_clock;

namespace {

int    FLAGS_num        = 1000000;
int    FLAGS_value_size = 100;
int    FLAGS_bloom_bits = 10;          // 0 = 关闭布隆过滤器
size_t FLAGS_cache      = 8 << 20;     // BlockCache 字节数, 0 = 关闭
int    FLAGS_stride     = 8;           // fill 步长:8 留出字典序空隙,让 readmissing 真正能命中 bloom 拦截路径
int    FLAGS_sync       = 1;           // 1 = 每次 Put 后 WAL fsync;0 = 关闭(压吞吐用)
const char* FLAGS_benchmarks = "fillseq,readrandom,readmissing";
const char* FLAGS_db    = "/tmp/kv_bench";

struct Stats {
    std::vector<uint64_t> lat_ns;
    void Add(uint64_t ns) { lat_ns.push_back(ns); }
    void Reset() { lat_ns.clear(); }
    void Report(const char* name) {
        if (lat_ns.empty()) return;
        double total_s = static_cast<double>(
            std::accumulate(lat_ns.begin(), lat_ns.end(), uint64_t(0))) / 1e9;
        std::sort(lat_ns.begin(), lat_ns.end());
        double avg_us = total_s * 1e6 / lat_ns.size();
        double p50_us = lat_ns[lat_ns.size() * 50 / 100] / 1e3;
        double p99_us = lat_ns[lat_ns.size() * 99 / 100] / 1e3;
        double qps = lat_ns.size() / total_s;
        printf("%-12s: %10.3f us/op; %12.1f ops/sec; p50=%.2f us  p99=%.2f us\n",
               name, avg_us, qps, p50_us, p99_us);
    }
};

std::string MakeKey(int64_t i) {
    char buf[32];
    snprintf(buf, sizeof(buf), "key%016lld", (long long)i);
    return buf;
}

void RemoveDb() {
    std::string cmd = std::string("rm -rf ") + FLAGS_db;
    int rc = system(cmd.c_str());
    (void)rc;
}

std::unique_ptr<kv::KVDB> OpenDb() {
    kv::KVDB::Options options;
    options.dbname = FLAGS_db;
    options.bloom_bits_per_key = FLAGS_bloom_bits;
    options.block_cache_capacity = FLAGS_cache;
    std::unique_ptr<kv::KVDB> db;
    kv::Status s = kv::KVDB::Open(options, &db);
    if (!s.ok()) {
        fprintf(stderr, "KVDB::Open failed\n");
        exit(1);
    }
    if (!FLAGS_sync) db->SetWALSyncOnWrite(false);
    return db;
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--num=", 6) == 0)            FLAGS_num = atoi(argv[i] + 6);
        else if (strncmp(argv[i], "--value_size=", 13) == 0) FLAGS_value_size = atoi(argv[i] + 13);
        else if (strncmp(argv[i], "--bloom_bits=", 13) == 0) FLAGS_bloom_bits = atoi(argv[i] + 13);
        else if (strncmp(argv[i], "--cache=", 8) == 0)       FLAGS_cache = strtoull(argv[i] + 8, nullptr, 10);
        else if (strncmp(argv[i], "--stride=", 9) == 0)      FLAGS_stride = atoi(argv[i] + 9);
        else if (strncmp(argv[i], "--sync=", 7) == 0)        FLAGS_sync = atoi(argv[i] + 7);
        else if (strncmp(argv[i], "--benchmarks=", 13) == 0) FLAGS_benchmarks = argv[i] + 13;
        else if (strncmp(argv[i], "--db=", 5) == 0)          FLAGS_db = argv[i] + 5;
        else { fprintf(stderr, "unknown flag: %s\n", argv[i]); return 1; }
    }

    printf("KV-Bench: num=%d value_size=%d bloom_bits=%d cache=%zu sync=%d db=%s\n\n",
           FLAGS_num, FLAGS_value_size, FLAGS_bloom_bits, FLAGS_cache, FLAGS_sync, FLAGS_db);

    std::string value(FLAGS_value_size, 'v');
    Stats stats;

    std::string bench_list(FLAGS_benchmarks);
    size_t pos = 0;
    while (pos <= bench_list.size()) {
        size_t comma = bench_list.find(',', pos);
        std::string name = (comma == std::string::npos)
            ? bench_list.substr(pos)
            : bench_list.substr(pos, comma - pos);
        pos = (comma == std::string::npos) ? bench_list.size() + 1 : comma + 1;
        if (name.empty()) continue;

        if (name == "fillseq" || name == "fillrandom") {
            RemoveDb();
            auto db = OpenDb();

            std::vector<int64_t> order(FLAGS_num);
            std::iota(order.begin(), order.end(), int64_t(0));
            // 用 stride 8 留出字典序空隙,readmissing 查 i*8+4 必落在 [i*8, (i+1)*8] 区间内
            for (int i = 0; i < FLAGS_num; i++) order[i] = int64_t(i) * FLAGS_stride;
            if (name == "fillrandom") {
                std::mt19937_64 rng(42);
                std::shuffle(order.begin(), order.end(), rng);
            }

            stats.Reset();
            for (int i = 0; i < FLAGS_num; i++) {
                auto t0 = Clock::now();
                kv::Status s = db->Put(MakeKey(order[i]), value);
                auto t1 = Clock::now();
                if (!s.ok()) { fprintf(stderr, "Put failed at i=%d\n", i); return 1; }
                stats.Add(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            }
            // 等后台 flush/compaction 全部落地,后续读测到的是磁盘态而非内存命中
            db->TEST_FlushMemTable();
            db->TEST_WaitForBackground();
            stats.Report(name.c_str());
            printf("  (flushed: L0=%d L1=%d)\n",
                   db->TEST_NumLevelFiles(0), db->TEST_NumLevelFiles(1));

        } else if (name == "readrandom" || name == "readmissing") {
            // 读测试基于已存在的数据库 —— 先跑 fill*,再单独跑 read*
            auto db = OpenDb();

            std::vector<int64_t> order(FLAGS_num);
            if (name == "readrandom") {
                // 查存在的 key,与 fill 步长对齐
                for (int i = 0; i < FLAGS_num; i++) order[i] = int64_t(i) * FLAGS_stride;
            } else {
                // readmissing 查 i*stride + stride/2,字典序落在 (i*stride, (i+1)*stride) 内,
                // 必不在文件里,bloom 拦截路径才会真正生效
                int64_t half = FLAGS_stride / 2;
                for (int i = 0; i < FLAGS_num; i++) order[i] = int64_t(i) * FLAGS_stride + half;
            }
            std::mt19937_64 rng(123);
            std::shuffle(order.begin(), order.end(), rng);

            stats.Reset();
            std::string out;
            int not_found = 0;
            for (int i = 0; i < FLAGS_num; i++) {
                auto t0 = Clock::now();
                kv::Status s = db->Get(MakeKey(order[i]), &out);
                auto t1 = Clock::now();
                stats.Add(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
                if (name == "readrandom") {
                    if (!s.ok()) {
                        fprintf(stderr, "readrandom: key %lld missing!\n", (long long)order[i]);
                        return 1;
                    }
                } else if (s.IsNotFound()) {
                    not_found++;
                }
            }
            stats.Report(name.c_str());
            if (name == "readmissing") {
                printf("  (not_found=%d/%d)\n", not_found, FLAGS_num);
            }

        } else {
            fprintf(stderr, "unknown benchmark: %s\n", name.c_str());
            fprintf(stderr, "available: fillseq, fillrandom, readrandom, readmissing\n");
            return 1;
        }
    }
    return 0;
}
