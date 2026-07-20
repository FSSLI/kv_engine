#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include "lru_cache.h"

using namespace kv;

// 造一个恰好 size 字节的 value,方便精确控制计费
static std::string Val(char c, size_t size) { return std::string(size, c); }

// 测试 1:基本读写 + 命中/未命中计数 + 计费
void TestBasicPutGet() {
    std::cout << "=== TestBasicPutGet ===" << std::endl;
    LRUCache cache(1 << 20);

    cache.Put(1, "hello");
    std::string v;
    assert(cache.Get(1, &v));
    assert(v == "hello");

    assert(!cache.Get(2, &v));         // miss
    assert(cache.Hits() == 1);
    assert(cache.Misses() == 1);
    assert(cache.TotalCharge() == 5);  // "hello" = 5 字节

    std::cout << "  PASS: basic put/get" << std::endl;
}

// 测试 2:同 key 覆盖 —— 旧 charge 必须释放,值和热度都要刷新
void TestOverwrite() {
    std::cout << "=== TestOverwrite ===" << std::endl;
    LRUCache cache(1 << 20);

    cache.Put(1, Val('a', 100));
    cache.Put(1, Val('b', 50));  // 覆盖

    std::string v;
    assert(cache.Get(1, &v));
    assert(v.size() == 50 && v[0] == 'b');
    assert(cache.TotalCharge() == 50);  // 不是 150!

    std::cout << "  PASS: overwrite frees old charge" << std::endl;
}

// 测试 3:LRU 驱逐顺序 —— 被 Get 摸过的条目要活到最后
void TestLRUEvictionOrder() {
    std::cout << "=== TestLRUEvictionOrder ===" << std::endl;
    LRUCache cache(300);  // 恰好放下 3 个 100B 条目

    cache.Put(1, Val('a', 100));
    cache.Put(2, Val('b', 100));
    cache.Put(3, Val('c', 100));
    assert(cache.TotalCharge() == 300);

    std::string v;
    assert(cache.Get(1, &v));  // touch k1 -> 热度顺序(新->旧): 1, 3, 2

    cache.Put(4, Val('d', 100));  // 空间不够,应驱逐 k2(最久未用)
    assert(!cache.Get(2, &v));    // k2 已被驱逐
    assert(cache.Get(1, &v));     // k1 因为被摸过,活着
    assert(cache.Get(3, &v));
    assert(cache.Get(4, &v));
    assert(cache.TotalCharge() == 300);

    std::cout << "  PASS: LRU eviction order" << std::endl;
}

// 测试 4:大条目插入时需要连续驱逐多个旧条目
void TestEvictToFit() {
    std::cout << "=== TestEvictToFit ===" << std::endl;
    LRUCache cache(300);

    cache.Put(1, Val('a', 100));
    cache.Put(2, Val('b', 100));
    cache.Put(3, Val('c', 100));

    // 250B 新条目:要依次驱逐 k1、k2、k3 才放得下
    cache.Put(4, Val('d', 250));

    std::string v;
    assert(!cache.Get(1, &v));
    assert(!cache.Get(2, &v));
    assert(!cache.Get(3, &v));
    assert(cache.Get(4, &v));
    assert(cache.TotalCharge() == 250);

    std::cout << "  PASS: evict multiple to fit" << std::endl;
}

// 测试 5:超过整个容量的单条 —— 不缓存、不报错、不死循环
void TestOversizedNotCached() {
    std::cout << "=== TestOversizedNotCached ===" << std::endl;
    LRUCache cache(100);

    cache.Put(1, Val('x', 200));  // 比整个缓存还大

    std::string v;
    assert(!cache.Get(1, &v));
    assert(cache.TotalCharge() == 0);

    std::cout << "  PASS: oversized entry not cached" << std::endl;
}

// 测试 6:EvictFile —— 只清指定文件的块,别的文件不受影响
// (compaction 删除旧 SSTable 时靠它防止死文件的块白占缓存)
void TestEvictFile() {
    std::cout << "=== TestEvictFile ===" << std::endl;
    LRUCache cache(1 << 20);

    uint64_t f7b0 = MakeBlockCacheKey(7, 0);
    uint64_t f7b1 = MakeBlockCacheKey(7, 4096);
    uint64_t f9b0 = MakeBlockCacheKey(9, 0);

    cache.Put(f7b0, "a");
    cache.Put(f7b1, "b");
    cache.Put(f9b0, "c");

    cache.EvictFile(7);

    std::string v;
    assert(!cache.Get(f7b0, &v));
    assert(!cache.Get(f7b1, &v));
    assert(cache.Get(f9b0, &v));  // 文件 9 不受影响
    assert(cache.TotalCharge() == 1);

    std::cout << "  PASS: evict by file number" << std::endl;
}

// 测试 7:Clear
void TestClear() {
    std::cout << "=== TestClear ===" << std::endl;
    LRUCache cache(1 << 20);

    cache.Put(1, "a");
    cache.Put(2, "b");
    cache.Clear();

    std::string v;
    assert(!cache.Get(1, &v));
    assert(cache.TotalCharge() == 0);

    std::cout << "  PASS: clear" << std::endl;
}

// 测试 8:并发冒烟 —— 4 线程各写读 1000 次,不崩且计数自洽
// (Week 3 任务②引入后台线程后,这个测试就是保命符)
void TestConcurrentSmoke() {
    std::cout << "=== TestConcurrentSmoke ===" << std::endl;
    LRUCache cache(1 << 20);  // 1MB,本测试数据量小不会触发驱逐

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; t++) {
        threads.emplace_back([&cache, t] {
            for (int i = 0; i < 1000; i++) {
                uint64_t key = MakeBlockCacheKey(t + 1, i);
                cache.Put(key, "v");
                std::string out;
                cache.Get(key, &out);
            }
        });
    }
    for (auto& th : threads) th.join();

    // hits + misses 必须恰好等于 Get 总次数,说明计数没有竞态
    assert(cache.Hits() + cache.Misses() == 4000);

    std::cout << "  PASS: concurrent smoke (hits=" << cache.Hits()
              << ", misses=" << cache.Misses() << ")" << std::endl;
}

int main() {
    TestBasicPutGet();
    TestOverwrite();
    TestLRUEvictionOrder();
    TestEvictToFit();
    TestOversizedNotCached();
    TestEvictFile();
    TestClear();
    TestConcurrentSmoke();

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
