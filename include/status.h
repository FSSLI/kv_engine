#pragma once

#include <string>

namespace kv {

enum class StatusCode {
    kOk = 0,
    kNotFound,
    kCorruption,
    kNotSupported,
    kInvalidArgument,
    kIOError,
};

class Status {
public:
    Status() noexcept : code_(StatusCode::kOk) {}

    static Status OK() { return Status(); }
    static Status NotFound(const std::string& msg) { return Status(StatusCode::kNotFound, msg); }
    static Status Corruption(const std::string& msg) { return Status(StatusCode::kCorruption, msg); }
    static Status IOError(const std::string& msg) { return Status(StatusCode::kIOError, msg); }
    static Status InvalidArgument(const std::string& msg) { return Status(StatusCode::kInvalidArgument, msg); }

    bool ok() const { return code_ == StatusCode::kOk; }
    bool IsNotFound() const { return code_ == StatusCode::kNotFound; }
    bool IsCorruption() const { return code_ == StatusCode::kCorruption; }
    bool IsIOError() const { return code_ == StatusCode::kIOError; }

    std::string ToString() const {
        if (ok()) return "OK";
        switch (code_) {
            case StatusCode::kNotFound: return "NotFound: " + msg_;
            case StatusCode::kCorruption: return "Corruption: " + msg_;
            case StatusCode::kNotSupported: return "NotSupported: " + msg_;
            case StatusCode::kInvalidArgument: return "InvalidArgument: " + msg_;
            case StatusCode::kIOError: return "IOError: " + msg_;
            default: return "Unknown: " + msg_;
        }
    }

private:
    Status(StatusCode code, const std::string& msg) : code_(code), msg_(msg) {}

    StatusCode code_;
    std::string msg_;
};

} // namespace kv