#include "bloom_filter.h"

namespace kv {

// FNV-1a:简单、确定、字节级混合均匀。
// Bloom 场景不需要密码学强度,需要的是一个 seed 固定的稳定哈希。
static uint32_t BloomHash(const Slice& key) {
    uint32_t h = 2166136261u;  // FNV offset basis
    for (size_t i = 0; i < key.size(); ++i) {
        h ^= static_cast<uint8_t>(key.data()[i]);
        h *= 16777619u;  // FNV prime
    }
    return h;
}

BloomFilterPolicy::BloomFilterPolicy(int bits_per_key)
    : bits_per_key_(bits_per_key) {
    // 最优 k ≈ (m/n) * ln2 ≈ bits_per_key * 0.69(LevelDB 同款)
    k_ = static_cast<size_t>(bits_per_key * 0.69);
    if (k_ < 1) k_ = 1;
    if (k_ > 30) k_ = 30;
}

void BloomFilterPolicy::CreateFilter(const std::vector<std::string>& keys,
                                     std::string* dst) const {
    size_t bits = keys.size() * static_cast<size_t>(bits_per_key_);
    if (bits < 64) bits = 64;  // key 太少时也保证最小位图,压低误判率

    size_t bytes = (bits + 7) / 8;
    bits = bytes * 8;

    const size_t init_size = dst->size();
    dst->resize(init_size + bytes, 0);
    dst->push_back(static_cast<char>(k_));  // blob 最后一字节存 k

    char* array = &(*dst)[init_size];
    for (const auto& key : keys) {
        // 双哈希技巧的变体:每次探测把 h 加上一个旋转过的 delta,
        // 等价于 k 个近似独立的哈希函数,省掉 k 次完整哈希计算
        uint32_t h = BloomHash(Slice(key));
        const uint32_t delta = (h >> 17) | (h << 15);
        for (size_t j = 0; j < k_; ++j) {
            const uint32_t bitpos = h % bits;
            array[bitpos / 8] |= (1 << (bitpos % 8));
            h += delta;
        }
    }
}

bool BloomFilterPolicy::KeyMayMatch(const Slice& key, const Slice& filter) {
    const size_t len = filter.size();
    if (len < 2) return true;  // 空/残缺过滤器:保守放行,不挡读

    const char* array = filter.data();
    const size_t bits = (len - 1) * 8;
    const size_t k = static_cast<size_t>(static_cast<uint8_t>(array[len - 1]));
    if (k > 30) return true;  // 给未来编码留的保留口子(LevelDB 同款)

    uint32_t h = BloomHash(key);
    const uint32_t delta = (h >> 17) | (h << 15);
    for (size_t j = 0; j < k; ++j) {
        const uint32_t bitpos = h % bits;
        if ((array[bitpos / 8] & (1 << (bitpos % 8))) == 0) return false;
        h += delta;
    }
    return true;
}

}  // namespace kv
