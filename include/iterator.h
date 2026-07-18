#pragma once

#include "slice.h"

namespace kv {

class Iterator {
public:
    virtual ~Iterator() {}
    virtual void SeekToFirst() = 0;
    virtual void Seek(const Slice& target) = 0;
    virtual void Next() = 0;
    virtual bool Valid() const = 0;
    virtual Slice key() const = 0;
    virtual Slice value() const = 0;
};

} // namespace kv