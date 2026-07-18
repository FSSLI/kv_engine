#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include "slice.h"
#include "status.h"

namespace kv {

// SSTable 文件元数据
struct FileMetaData {
    uint64_t file_number;
    uint64_t file_size;
    std::string smallest;
    std::string largest;
};

class VersionSet {
public:
    static const int kMaxLevel = 7;

    explicit VersionSet(const std::string& dbname);
    ~VersionSet();

    // 从 CURRENT → Manifest 恢复
    Status Recover();

    // 原子写入 Manifest（生成新文件 + 更新 CURRENT）
    Status WriteSnapshot();

    void AddFile(int level, const FileMetaData& meta);
    void RemoveFile(int level, uint64_t file_number);

    const std::vector<FileMetaData>& GetLevel(int level) const {
        return levels_[level];
    }

    uint64_t NewFileNumber() { return next_file_number_++; }
    uint64_t NextFileNumber() const { return next_file_number_; }
    void SetNextFileNumber(uint64_t num) { next_file_number_ = num; }

    uint64_t LogNumber() const { return log_number_; }
    void SetLogNumber(uint64_t num) { log_number_ = num; }

    uint64_t PrevLogNumber() const { return prev_log_number_; }
    void SetPrevLogNumber(uint64_t num) { prev_log_number_ = num; }

    int NumLevelFiles(int level) const {
        return static_cast<int>(levels_[level].size());
    }

private:
    std::string dbname_;
    std::string manifest_filename_;
    uint64_t next_file_number_;
    uint64_t log_number_;
    uint64_t prev_log_number_;
    std::vector<FileMetaData> levels_[kMaxLevel];

    Status ReadCurrentFile(std::string* manifest_name);
    Status WriteCurrentFile(const std::string& manifest_name);
    std::string ManifestFileName(uint64_t file_number) const;
};

} // namespace kv