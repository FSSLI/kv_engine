#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <random>
#include <unistd.h>
#include <sys/stat.h>
#include "bloom_filter.h"
#include "sstable.h"

using namespace kv;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

static const std::string kTestDir = "/tmp/kv_bloom_test";

static void CleanUp() {
    std::string cmd = "rm -rf " + kTestDir + " && mkdir -p " + kTestDir;
    int ret = system(cmd.c_str());
    (void)ret;
}

static std::string Key(int i) {
    char buf[32];
    snprintf(buf, sizeof(buf), "key%08d", i);
    return buf;
}

// 1. 空过滤器:不加任何 key,任何查询都必须被拦下(位图全零)
static void TestEmptyFilter() {
    BloomFilterPolicy policy(10);
    std::string filter;
    policy.CreateFilter({}, &filter);
    CHECK(filter.size() >= 2);
    CHECK(!BloomFilterPolicy::KeyMayMatch(Slice("anything"), Slice(filter)));
    CHECK(!BloomFilterPolicy::KeyMayMatch(Slice(""), Slice(filter)));

    // 残缺 blob(len < 2):保守放行,不挡读
    std::string tiny = "x";
    CHECK(BloomFilterPolicy::KeyMayMatch(Slice("k"), Slice(tiny)));

    printf("  PASS: empty filter rejects everything\n");
}

// 2. 铁律:加进去的 key 一个都不许漏报(不允许 false negative)
static void TestNoFalseNegatives() {
    const int n = 10000;
    std::vector<std::string> keys;
    keys.reserve(n);
    for (int i = 0; i < n; ++i) keys.push_back(Key(i));

    BloomFilterPolicy policy(10);
    std::string filter;
    policy.CreateFilter(keys, &filter);
    Slice f(filter);
    for (int i = 0; i < n; ++i) {
        CHECK(BloomFilterPolicy::KeyMayMatch(Slice(keys[i]), f));
    }

    printf("  PASS: no false negatives over %d keys\n", n);
}

// 3. 误判率:10 bits/key 理论值 ~1%,实测放宽到 2% 以内
static void TestFalsePositiveRate() {
    const int n = 10000;
    std::vector<std::string> keys;
    keys.reserve(n);
    for (int i = 0; i < n; ++i) keys.push_back(Key(i));

    BloomFilterPolicy policy(10);
    std::string filter;
    policy.CreateFilter(keys, &filter);
    Slice f(filter);

    int fp = 0;
    const int probes = 10000;
    for (int i = 0; i < probes; ++i) {
        // 查一批确定没加过的 key(号段错开 + 前缀不同)
        char buf[32];
        snprintf(buf, sizeof(buf), "absent%08d", i * 7 + 3);
        if (BloomFilterPolicy::KeyMayMatch(Slice(buf), f)) ++fp;
    }
    double rate = static_cast<double>(fp) / probes;
    printf("  false positive rate: %.2f%% (%d/%d)\n", rate * 100, fp, probes);
    CHECK(rate < 0.02);

    printf("  PASS: false positive rate under 2%%\n");
}

// 4. SSTable 集成:存在的 key 照常读到;不存在的 key 被过滤器拦下,
//    FilterRejections 计数证明一次数据块都没碰。
//    注意:不存在的 key 必须落在 [smallest, largest] 区间内,
//    区间外的 key 在范围检查层就被拒了,到不了过滤器。
static void TestSSTableRejectsWithoutDiskRead() {
    CleanUp();
    const std::string file = kTestDir + "/000001.sst";

    // 稀疏键空间:偶数 key 写入文件,奇数 key 做"不存在但同区间"的探测
    const int n = 1000;
    {
        SSTableBuilder builder(file, 4096, 10);
        for (int i = 0; i < n; ++i) {
            char vbuf[64];
            snprintf(vbuf, sizeof(vbuf), "value-%d", i);
            CHECK(builder.Add(Slice(Key(2 * i)), Slice(vbuf)).ok());
        }
        uint64_t size = 0;
        CHECK(builder.Finish(&size).ok());
    }

    std::unique_ptr<SSTable> table;
    CHECK(SSTable::Open(file, &table, 1, nullptr).ok());

    // 命中路径:过滤器不许误伤真实 key
    std::string value;
    for (int i = 0; i < n; i += 50) {
        CHECK(table->Get(Slice(Key(2 * i)), &value).ok());
    }
    CHECK(table->FilterRejections() == 0);

    // 拦截路径:奇数 key 与偶数 key 同区间但不存在
    const int absent = 1000;
    for (int i = 0; i < absent; ++i) {
        CHECK(table->Get(Slice(Key(2 * i + 1)), &value).IsNotFound());
    }
    // 允许少量误判(漏过过滤器、落到块查找),但绝大多数必须被拦
    uint64_t rejected = table->FilterRejections();
    printf("  filter rejected %lu/%d absent-key lookups\n", rejected, absent);
    CHECK(rejected >= static_cast<uint64_t>(absent * 0.95));

    printf("  PASS: sstable rejects absent keys without disk read\n");
}

// 5. 兼容性:bloom_bits = 0 构建的文件没有过滤器,读写照常、计数恒 0
static void TestSSTableWithoutFilterStillWorks() {
    CleanUp();
    const std::string file = kTestDir + "/000002.sst";

    {
        SSTableBuilder builder(file, 4096, 0);
        for (int i = 0; i < 500; ++i) {
            CHECK(builder.Add(Slice(Key(i)), Slice("v")).ok());
        }
        uint64_t size = 0;
        CHECK(builder.Finish(&size).ok());
    }

    std::unique_ptr<SSTable> table;
    CHECK(SSTable::Open(file, &table, 2, nullptr).ok());

    std::string value;
    CHECK(table->Get(Slice(Key(250)), &value).ok());
    CHECK(value == "v");
    CHECK(table->Get(Slice("nope"), &value).IsNotFound());
    CHECK(table->FilterRejections() == 0);

    printf("  PASS: sstable without filter remains compatible\n");
}

int main() {
    printf("Running bloom filter tests...\n");
    TestEmptyFilter();
    TestNoFalseNegatives();
    TestFalsePositiveRate();
    TestSSTableRejectsWithoutDiskRead();
    TestSSTableWithoutFilterStillWorks();
    printf("All tests passed!\n");
    return 0;
}
