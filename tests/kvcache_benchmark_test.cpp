#include <gtest/gtest.h>
#include "kvcache.h"
#include <chrono>
#include <random>
#include <filesystem>
#include <format>
#include <thread>



class KVCacheBenchmarkTest : public ::testing::Test {
protected:
    static std::string dbPath;
    static KVCache::SSD* ssd;
    KVCache::KVCache* cache_;

    static void SetUpTestSuite() {
        dbPath = std::format("./bench.db.{}", std::this_thread::get_id());
        if (std::filesystem::exists(dbPath)) {
            std::filesystem::remove_all(dbPath);
        }

        KVCache::SSD::Config config{
            .nr_channels = 8,
            .block_size = 8 * 1024 * 1024,  // 8MB
            .blocks_per_channel = 256,
        };
        auto status = KVCache::SSD::create(dbPath, &ssd, config);
        ASSERT_TRUE(status.ok()) << status.msg();
    }

    void SetUp() override {
        KVCache::Options options;
        options.slab_mem_budget = 300 * 1024 * 1024;  // 300MB
        options.index_mem_budget = 100 * 1024 * 1024; // 100MB
        options.index_table_size = 512 * 1024;        // 512KB
        cache_ = new KVCache::KVCache(ssd, options);
    }

    void TearDown() override {
        delete cache_;
    }

    static void TearDownTestSuite() {
        delete ssd;
        if (std::filesystem::exists(dbPath)) {
            std::filesystem::remove_all(dbPath);
        }
    }

};

std::string KVCacheBenchmarkTest::dbPath;
KVCache::SSD* KVCacheBenchmarkTest::ssd;

// Put and Get 2GiB data
TEST_F(KVCacheBenchmarkTest, PutAndGet2GiB) {
    const size_t total_size = 2ULL * 1024 * 1024 * 1024;
    const int min_value_size = 8;
    const int max_value_size = cache_->max_kv_size();
    size_t total_bytes = 0;
    
    std::vector<std::string> keys;
    std::vector<size_t> value_sizes;

    // value size is random
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(min_value_size, max_value_size);
    
    while (total_bytes < total_size) {
        size_t value_size = dis(gen);
        if (total_bytes + value_size > total_size) {
            value_size = total_size - total_bytes;
        }
        value_sizes.push_back(value_size);
        total_bytes += value_size;
    }

    const int num_operations = value_sizes.size();
    keys.reserve(num_operations);

    // write performance test
    auto write_start = std::chrono::high_resolution_clock::now();
    KVCache::Status status;
    size_t actual_write_bytes = 0;
    
    for (int i = 0; i < num_operations; i++) {
        std::string key = std::format("bench_key_{}", i);
        keys.push_back(key);
        
        std::string value(value_sizes[i], 'x');
        status = cache_->Put(key, value);
        if (!status.ok()) {
            continue;
        }
        actual_write_bytes += value_sizes[i];
    }
    
    auto write_end = std::chrono::high_resolution_clock::now();
    auto write_duration = std::chrono::duration_cast<std::chrono::seconds>(write_end - write_start);

    // read performance test
    auto read_start = std::chrono::high_resolution_clock::now();
    size_t actual_read_bytes = 0;
    
    for (int i = 0; i < num_operations; i++) {
        std::string value;
        status = cache_->Get(keys[i], &value);
        if (!status.ok()) {
            continue;
        }
        actual_read_bytes += value.size();
    }
    
    auto read_end = std::chrono::high_resolution_clock::now();
    auto read_duration = std::chrono::duration_cast<std::chrono::seconds>(read_end - read_start);

    double write_seconds = write_duration.count();
    double read_seconds = read_duration.count();
    
    std::cout << "Performance test results:\n";
    std::cout << std::format("Write: {:.2f} ops/s, {:.2f} MiB/s\n", 
        num_operations / write_seconds,
        actual_write_bytes / 1024.0 / 1024.0 / write_seconds);
    std::cout << std::format("Read: {:.2f} ops/s, {:.2f} MiB/s\n",
        num_operations / read_seconds, 
        actual_read_bytes / 1024.0 / 1024.0 / read_seconds);
}


