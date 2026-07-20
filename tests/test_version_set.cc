#include <iostream>
#include <cassert>
#include <cstdio>
#include <string>
#include "version_set.h"
#include <sys/stat.h>

using namespace kv;

const std::string kTestDir = "/tmp/kv_version_test";

void CleanUp() {
    std::string cmd = "rm -rf " + kTestDir;
    int ret = system(cmd.c_str());
    (void)ret;
    mkdir(kTestDir.c_str(), 0755);
}

void TestBasicWriteRead() {
    std::cout << "=== TestBasicWriteRead ===" << std::endl;
    CleanUp();

    {
        VersionSet vset(kTestDir);
        vset.SetLogNumber(5);
        vset.SetPrevLogNumber(3);
        vset.SetNextFileNumber(10);

        FileMetaData meta{1, 100, "aaa", "zzz"};
        vset.AddFile(0, meta);

        Status s = vset.WriteSnapshot();
        assert(s.ok());
    }

    {
        VersionSet vset(kTestDir);
        Status s = vset.Recover();
        assert(s.ok());
        assert(vset.LogNumber() == 5);
        assert(vset.PrevLogNumber() == 3);
        // 修复:MANIFEST 文件本身消耗一个 file number(LevelDB 同款设计),
        // WriteSnapshot 里 next_file_number_++ 取了 10 给 MANIFEST-000010,
        // 所以 Recover 回来是 11 而不是 10。Release 构建 NDEBUG 把 assert
        // 全禁了,这个错误断言一直没暴露 —— 测试必须在 Debug 构建下跑!
        assert(vset.NextFileNumber() == 11);
        assert(vset.NumLevelFiles(0) == 1);
        assert(vset.GetLevel(0)[0].smallest == "aaa");
    }

    std::cout << "  PASS: basic write/read" << std::endl;
}

void TestMultiLevel() {
    std::cout << "=== TestMultiLevel ===" << std::endl;
    CleanUp();

    {
        VersionSet vset(kTestDir);
        vset.SetNextFileNumber(100);

        vset.AddFile(0, FileMetaData{1, 100, "a", "b"});
        vset.AddFile(1, FileMetaData{2, 200, "c", "d"});

        Status s = vset.WriteSnapshot();
        assert(s.ok());
    }

    {
        VersionSet vset(kTestDir);
        Status s = vset.Recover();
        assert(s.ok());
        assert(vset.NumLevelFiles(0) == 1);
        assert(vset.NumLevelFiles(1) == 1);
        assert(vset.GetLevel(1)[0].file_number == 2);
    }

    std::cout << "  PASS: multi level" << std::endl;
}

void TestEmptyRecover() {
    std::cout << "=== TestEmptyRecover ===" << std::endl;
    CleanUp();

    VersionSet vset(kTestDir);
    Status s = vset.Recover();
    assert(s.ok());
    assert(vset.NumLevelFiles(0) == 0);

    std::cout << "  PASS: empty recover" << std::endl;
}

int main() {
    std::cout << "VersionSet Tests Starting..." << std::endl;
    TestBasicWriteRead();
    TestMultiLevel();
    TestEmptyRecover();
    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}