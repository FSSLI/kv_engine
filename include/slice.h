#pragma once

#include <cstdlib>
#include <string>
#include <cstring>

namespace kv {

class Slice {
public:
    Slice() : data_(""), size_(0) {}
    Slice(const char* data, size_t size) : data_(data), size_(size) {}
    Slice(const std::string& s) : data_(s.data()), size_(s.size()) {}
    Slice(const char* data) : data_(data), size_(strlen(data)) {}

    const char* data() const { return data_; }
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    int compare(const Slice& other) const {
        const size_t min_len = (size_ < other.size_) ? size_ : other.size_;
        int r = memcmp(data_, other.data_, min_len);
        if (r == 0) {
            if (size_ < other.size_) return -1;
            if (size_ > other.size_) return 1;
            return 0;
        }
        return r;
    }

    std::string ToString() const { return std::string(data_, size_); }

private:
    const char* data_;
    size_t size_;
};

inline bool operator==(const Slice& a, const Slice& b) {
    return a.size() == b.size() && memcmp(a.data(), b.data(), a.size()) == 0;
}

inline bool operator!=(const Slice& a, const Slice& b) {
    return !(a == b);
}

} // namespace kv