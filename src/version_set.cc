#include "version_set.h"
#include <cstdio>
#include <cstring>

#ifdef KV_DEBUG
#define DEBUG_LOG(fmt, ...) printf("[DEBUG] " fmt "\n", ##__VA_ARGS__)
#else
#define DEBUG_LOG(fmt, ...) ((void)0)
#endif

namespace kv {

static const char kCurrentFileName[] = "CURRENT";

VersionSet::VersionSet(const std::string& dbname)
    : dbname_(dbname)
    , next_file_number_(1)
    , log_number_(0)
    , prev_log_number_(0)
{
}

VersionSet::~VersionSet() = default;

std::string VersionSet::ManifestFileName(uint64_t file_number) const {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s/MANIFEST-%06lu", dbname_.c_str(), file_number);
    return std::string(buf);
}

Status VersionSet::ReadCurrentFile(std::string* manifest_name) {
    std::string current_file = dbname_ + "/" + kCurrentFileName;
    FILE* f = fopen(current_file.c_str(), "r");
    if (!f) {
        return Status::NotFound("CURRENT file not found");
    }
    char buf[256];
    if (fgets(buf, sizeof(buf), f) == nullptr) {
        fclose(f);
        return Status::Corruption("CURRENT file empty");
    }
    fclose(f);

    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';

    *manifest_name = dbname_ + "/" + buf;
    return Status::OK();
}

Status VersionSet::WriteCurrentFile(const std::string& manifest_name) {
    std::string current_file = dbname_ + "/" + kCurrentFileName;
    std::string tmp_file = current_file + ".tmp";

    size_t pos = manifest_name.find_last_of('/');
    std::string basename = (pos == std::string::npos) ? manifest_name : manifest_name.substr(pos + 1);

    FILE* f = fopen(tmp_file.c_str(), "w");
    if (!f) {
        return Status::IOError("cannot write CURRENT.tmp");
    }
    fprintf(f, "%s\n", basename.c_str());
    fclose(f);

    if (rename(tmp_file.c_str(), current_file.c_str()) != 0) {
        remove(tmp_file.c_str());
        return Status::IOError("cannot rename CURRENT.tmp");
    }
    return Status::OK();
}

Status VersionSet::Recover() {
    std::string manifest_name;
    Status s = ReadCurrentFile(&manifest_name);
    if (s.IsNotFound()) {
        DEBUG_LOG("VersionSet::Recover: new database, no CURRENT");
        return Status::OK();
    }
    if (!s.ok()) return s;

    FILE* f = fopen(manifest_name.c_str(), "rb");
    if (!f) {
        return Status::IOError("cannot open manifest: " + manifest_name);
    }

    char header[8];
    if (fread(header, 1, 8, f) != 8 || strncmp(header, "MANIFEST", 8) != 0) {
        fclose(f);
        return Status::Corruption("bad manifest header");
    }

    auto read_u64 = [&](uint64_t* out) -> bool {
        char buf[8];
        if (fread(buf, 1, 8, f) != 8) return false;
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= (static_cast<uint64_t>(static_cast<uint8_t>(buf[i])) << (i * 8));
        }
        *out = v;
        return true;
    };

    auto read_u32 = [&](uint32_t* out) -> bool {
        char buf[4];
        if (fread(buf, 1, 4, f) != 4) return false;
        *out = (static_cast<uint32_t>(static_cast<uint8_t>(buf[0]))) |
               (static_cast<uint32_t>(static_cast<uint8_t>(buf[1])) << 8) |
               (static_cast<uint32_t>(static_cast<uint8_t>(buf[2])) << 16) |
               (static_cast<uint32_t>(static_cast<uint8_t>(buf[3])) << 24);
        return true;
    };

    auto read_string = [&](std::string* out) -> bool {
        uint32_t len = 0;
        if (!read_u32(&len)) return false;
        out->clear();
        if (len > 0) {
            out->resize(len);
            if (fread(&(*out)[0], 1, len, f) != len) return false;
        }
        return true;
    };

    if (!read_u64(&next_file_number_)) { fclose(f); return Status::Corruption("manifest truncated"); }
    if (!read_u64(&log_number_)) { fclose(f); return Status::Corruption("manifest truncated"); }
    if (!read_u64(&prev_log_number_)) { fclose(f); return Status::Corruption("manifest truncated"); }

    uint32_t num_levels = 0;
    if (!read_u32(&num_levels)) { fclose(f); return Status::Corruption("manifest truncated"); }
    for (uint32_t l = 0; l < num_levels && l < kMaxLevel; ++l) {
        uint32_t num_files = 0;
        if (!read_u32(&num_files)) { fclose(f); return Status::Corruption("manifest truncated"); }
        levels_[l].clear();
        levels_[l].reserve(num_files);
        for (uint32_t i = 0; i < num_files; ++i) {
            FileMetaData meta;
            if (!read_u64(&meta.file_number)) { fclose(f); return Status::Corruption("manifest truncated"); }
            if (!read_u64(&meta.file_size)) { fclose(f); return Status::Corruption("manifest truncated"); }
            if (!read_string(&meta.smallest)) { fclose(f); return Status::Corruption("manifest truncated"); }
            if (!read_string(&meta.largest)) { fclose(f); return Status::Corruption("manifest truncated"); }
            levels_[l].push_back(std::move(meta));
        }
    }

    fclose(f);
    manifest_filename_ = manifest_name;

    DEBUG_LOG("VersionSet::Recover: next=%lu log=%lu prev=%lu L0=%d L1=%d",
              next_file_number_, log_number_, prev_log_number_,
              NumLevelFiles(0), NumLevelFiles(1));
    return Status::OK();
}

Status VersionSet::WriteSnapshot() {
    uint64_t manifest_num = next_file_number_++;
    std::string manifest_name = ManifestFileName(manifest_num);

    FILE* f = fopen(manifest_name.c_str(), "wb");
    if (!f) {
        return Status::IOError("cannot create manifest: " + manifest_name);
    }

    fwrite("MANIFEST", 1, 8, f);

    auto write_u64 = [&](uint64_t v) {
        char buf[8];
        for (int i = 0; i < 8; ++i) {
            buf[i] = static_cast<char>((v >> (i * 8)) & 0xff);
        }
        fwrite(buf, 1, 8, f);
    };

    auto write_u32 = [&](uint32_t v) {
        char buf[4];
        buf[0] = static_cast<char>(v & 0xff);
        buf[1] = static_cast<char>((v >> 8) & 0xff);
        buf[2] = static_cast<char>((v >> 16) & 0xff);
        buf[3] = static_cast<char>((v >> 24) & 0xff);
        fwrite(buf, 1, 4, f);
    };

    auto write_string = [&](const std::string& s) {
        write_u32(static_cast<uint32_t>(s.size()));
        if (!s.empty()) fwrite(s.data(), 1, s.size(), f);
    };

    write_u64(next_file_number_);
    write_u64(log_number_);
    write_u64(prev_log_number_);

    write_u32(kMaxLevel);
    for (int l = 0; l < kMaxLevel; ++l) {
        write_u32(static_cast<uint32_t>(levels_[l].size()));
        for (const auto& meta : levels_[l]) {
            write_u64(meta.file_number);
            write_u64(meta.file_size);
            write_string(meta.smallest);
            write_string(meta.largest);
        }
    }

    fflush(f);
    fclose(f);

    Status s = WriteCurrentFile(manifest_name);
    if (!s.ok()) return s;

    manifest_filename_ = manifest_name;
    DEBUG_LOG("VersionSet::WriteSnapshot: %s", manifest_name.c_str());
    return Status::OK();
}

void VersionSet::AddFile(int level, const FileMetaData& meta) {
    levels_[level].push_back(meta);
}

void VersionSet::RemoveFile(int level, uint64_t file_number) {
    auto& files = levels_[level];
    files.erase(std::remove_if(files.begin(), files.end(),
        [file_number](const FileMetaData& m) {
            return m.file_number == file_number;
        }), files.end());
}

} // namespace kv