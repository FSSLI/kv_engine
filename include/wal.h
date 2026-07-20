#pragma once

#include <string>
#include <cstdio>
#include <memory>
#include "status.h"
#include "write_batch.h"

namespace kv {

// WAL 文件：只做追加写
// 文件格式：连续的 WriteBatch 序列化记录
// 每条记录格式：[batch_length: 4B][sequence_number: 8B][count: 4B][records...][checksum: 4B]
// 其中 batch_length = body_size + 4 (checksum 的 4 字节)
// 总记录大小 = 4 + batch_length = body_size + 8
class WAL {
public:
    // 创建新的 WAL 文件（写模式）
    static Status Create(const std::string& filename, std::unique_ptr<WAL>* wal);

    // 打开已有 WAL 文件（读模式，用于恢复）
    static Status OpenForRead(const std::string& filename, std::unique_ptr<WAL>* wal);

    static Status OpenForAppend(const std::string& filename, std::unique_ptr<WAL>* wal);

    ~WAL();

    // 追加一条 WriteBatch
    // 默认每次 Append 后 fsync(生产语义);SetSyncOnWrite(false) 可关,仅供压测。
    Status Append(const WriteBatch& batch, uint64_t sequence_number);

    // 读取下一条记录（用于恢复）
    // 返回 true：成功读取一条
    // 返回 false：文件结束或损坏（损坏时会跳过该记录）
    bool ReadNext(uint64_t* sequence_number, WriteBatch* batch);

    // 强制刷盘
    Status Sync();

    // 关闭文件（Close 始终会 Sync 一次,与 sync_on_write_ 无关）
    Status Close();

    // 获取当前文件大小
    uint64_t FileSize() const;

    // 压测开关:是否每次 Append 后 fsync。默认 true,行为与之前完全一致。
    // 只在写入开始前调用(非线程安全的热切换,压测场景够用)。
    void SetSyncOnWrite(bool on) { sync_on_write_ = on; }

private:
    WAL(FILE* file, bool writable);

    FILE* file_;
    bool writable_;
    uint64_t file_size_;
    bool sync_on_write_ = true;

    // 禁止拷贝
    WAL(const WAL&) = delete;
    WAL& operator=(const WAL&) = delete;
};

} // namespace kv
