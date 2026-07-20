cmake_minimum_required(VERSION 3.10)
project(kv_engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 编译优化选项
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DKV_DEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

option(KV_DEBUG "Enable debug logging" OFF)
if(KV_DEBUG)
    add_definitions(-DKV_DEBUG)
endif()

include_directories(${CMAKE_SOURCE_DIR}/include)

set(KV_SOURCES
    src/memtable.cc
    src/write_batch.cc
    src/wal.cc
    src/sstable.cc
    src/lru_cache.cc
)


enable_testing()

find_package(Threads REQUIRED)

add_executable(test_skiplist tests/test_skiplist.cc src/memtable.cc)
target_link_libraries(test_skiplist Threads::Threads)
add_test(NAME skiplist_test COMMAND test_skiplist)

add_executable(test_write_batch tests/test_write_batch.cc src/write_batch.cc)
add_test(NAME write_batch_test COMMAND test_write_batch)

add_executable(test_wal tests/test_wal.cc src/wal.cc src/write_batch.cc)
add_test(NAME wal_test COMMAND test_wal)

add_executable(test_sstable tests/test_sstable.cc src/sstable.cc src/memtable.cc src/lru_cache.cc src/bloom_filter.cc)
target_link_libraries(test_sstable Threads::Threads)
add_test(NAME sstable_test COMMAND test_sstable)

add_executable(test_version_set tests/test_version_set.cc src/version_set.cc)
target_link_libraries(test_version_set Threads::Threads)
add_test(NAME version_set_test COMMAND test_version_set)

# 修改 test_kv_db 的依赖，加入 version_set.cc
add_executable(test_kv_db tests/test_kv_db.cc src/kv_db.cc src/memtable.cc src/write_batch.cc src/wal.cc src/sstable.cc src/version_set.cc src/merging_iterator.cc src/lru_cache.cc src/bloom_filter.cc)
target_link_libraries(test_kv_db Threads::Threads)
add_test(NAME kv_db_test COMMAND test_kv_db)

add_executable(test_lru_cache tests/test_lru_cache.cc src/lru_cache.cc)
target_link_libraries(test_lru_cache Threads::Threads)
add_test(NAME lru_cache_test COMMAND test_lru_cache)

add_executable(test_merging_iterator tests/test_merging_iterator.cc src/merging_iterator.cc src/memtable.cc)
target_link_libraries(test_merging_iterator Threads::Threads)
add_test(NAME merging_iterator_test COMMAND test_merging_iterator)

# Week 3 任务②:双 MemTable + 异步 flush
add_executable(test_async_flush tests/test_async_flush.cc src/kv_db.cc src/memtable.cc src/write_batch.cc src/wal.cc src/sstable.cc src/version_set.cc src/merging_iterator.cc src/lru_cache.cc src/bloom_filter.cc)
target_link_libraries(test_async_flush Threads::Threads)
add_test(NAME async_flush_test COMMAND test_async_flush)


# Week 3 任务③:自动 Compaction
add_executable(test_auto_compact tests/test_auto_compact.cc src/kv_db.cc src/memtable.cc src/write_batch.cc src/wal.cc src/sstable.cc src/version_set.cc src/merging_iterator.cc src/lru_cache.cc src/bloom_filter.cc)
target_link_libraries(test_auto_compact Threads::Threads)
add_test(NAME auto_compact_test COMMAND test_auto_compact)

# Week 4 任务①:布隆过滤器
add_executable(test_bloom_filter tests/test_bloom_filter.cc src/bloom_filter.cc src/sstable.cc src/lru_cache.cc)
target_link_libraries(test_bloom_filter Threads::Threads)
add_test(NAME bloom_filter_test COMMAND test_bloom_filter)

# Week 4 任务②:基准测试(必须用 Release 构建跑,数字才有意义)
add_executable(bench_kv bench/bench_kv.cc src/kv_db.cc src/memtable.cc src/write_batch.cc src/wal.cc src/sstable.cc src/version_set.cc src/merging_iterator.cc src/lru_cache.cc src/bloom_filter.cc)
target_link_libraries(bench_kv Threads::Threads)
