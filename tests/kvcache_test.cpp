#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <string>
#include <thread>
#include <vector>
#include <random>
#include <chrono>
#include "kvcache.h"
#include "rocksdb/db.h"
#include <filesystem>

class KVCacheTest : public ::testing::Test {
protected:
    static std::string db_path_;
    static KVCache::SSD *ssd_;
    std::unique_ptr<KVCache::KVCache> cache_;

    size_t random_data_size() {
        return 8 + (std::rand() % (cache_->MaxKVSize() - 8 + 1));
    }

    // I want to keep using my laptop for a few more years,
    // so I use a small write workload to reduce disk wear out.
    static void SetUpTestSuite() {
        db_path_ = std::format("./test.db.{}", std::this_thread::get_id());
        if (std::filesystem::exists(db_path_)) {
            std::filesystem::remove_all(db_path_);
        }

        // 200MiB SSD
        KVCache::SSD::Config config{
            .nr_channels = 8,
            .block_size = 200 * 1024, // 200 KiB
            .blocks_per_channel = 256,
        };
        auto status = KVCache::SSD::create(db_path_, &ssd_, config);
        ASSERT_TRUE(status.ok()) << status.msg();

        // Set random seed for reproducibility
        std::srand(42);
    }
    void SetUp() override {
        auto options = KVCache::KVCache::DefaultOptions(ssd_);
        options.slab_mem_budget = 10 * 1024 * 1024; // 10MiB
        options.index_mem_budget = 1 * 1024 * 1024; // 1MiB
        options.index_table_size = 512 * 1024; // 512KiB hash bucket
        cache_ = std::make_unique<KVCache::KVCache>(ssd_, options);
    }

    void TearDown() override {
        cache_.reset();
    }

    static void TearDownTestSuite() {
        if (std::filesystem::exists(db_path_)) {
            std::filesystem::remove_all(db_path_);
        }
    }
};

std::string KVCacheTest::db_path_;
KVCache::SSD *KVCacheTest::ssd_;

// Put - GET - Delete
// Put 1M keys, totally 1M*12*2=24MiB data(key+value),
// which is big enough to trigger GC.
TEST_F(KVCacheTest, TestBasicOperations) {
    KVCache::Status status;
    int num_keys = 1000000;
    for (int i = 0; i < num_keys; i++) {
        std::string key = std::format("test_key_{}", i);
        status = cache_->Put(key, key);
        ASSERT_TRUE(status.ok()) << status.msg();
    }

    // Get and verify all keys
    for (int i = 0; i < num_keys; i++) {
        std::string key = std::format("test_key_{}", i);
        std::string value;
        status = cache_->Get(key, &value);
        if (status.not_found()) {
            continue;
        }
        ASSERT_TRUE(status.ok()) << status.msg();
        ASSERT_EQ(value, key) << "Value mismatch for key " << key;
    }

    // Delete all keys
    for (int i = 0; i < num_keys; i++) {
        std::string key = std::format("test_key_{}", i);
        cache_->Delete(key);
    }

    // Verify all keys are deleted
    for (int i = 0; i < num_keys; i++) {
        std::string key = std::format("test_key_{}", i);
        std::string value;
        status = cache_->Get(key, &value);
        ASSERT_TRUE(status.not_found()) << status.msg();
    }
}

// Put distinct keys to trigger quick GC.
// >
// Normal GC can not reclaim even a bit of memory
// because all objects are in use, eventually
// hitting free_blocks_watermark_low. In this case,
// Quick GC will be triggered to reclaim memory.
TEST_F(KVCacheTest, TestQuickGC) {
    const uint64_t total_size = 2 * ssd_->nr_blocks_ * ssd_->block_size_;
    std::vector<std::string> keys;

    KVCache::Status status;
    for (uint64_t i = 0, size = 0; size < total_size; i++) {
        int value_size = random_data_size();
        std::string key = std::format("test_key_{}_{}", i, value_size);
        keys.push_back(key);

        status = cache_->Put(key, std::string(value_size, 'v'));
        size += value_size;
        if (status.object_too_large()) {
            auto size = keys[i].size() + value_size;
            ASSERT_TRUE(size > cache_->MaxKVSize())
                << "size: " << size << " max_kv_size: " << cache_->MaxKVSize();
            continue;
        }
        ASSERT_TRUE(status.ok()) << status.msg();
    }

    for (int i = 0; i < keys.size(); i++) {
        std::string value;
        status = cache_->Get(keys[i], &value);
        // Cache evicted or object too large
        if (status.not_found()) {
            continue;
        }
        ASSERT_TRUE(status.ok()) << status.msg();
        auto value_size = std::stoi(keys[i].substr(keys[i].find_last_of('_') + 1));
        ASSERT_EQ(value, std::string(value_size, 'v')) << status.msg();
    }

    for (size_t i = 0; i < keys.size(); i++) {
        cache_->Delete(keys[i]);
    }

    for (size_t i = 0; i < keys.size(); i++) {
        std::string value;
        status = cache_->Get(keys[i], &value);
        ASSERT_TRUE(status.not_found()) << status.msg();
    }
}

// Put a lot of same keys with similar
// value sizes to trigger normal GC.
TEST_F(KVCacheTest, TestNormalGC) {
    const uint64_t total_size = 2 * ssd_->nr_blocks_ * ssd_->block_size_;
    size_t size = 0;
    std::unordered_map<std::string, int> key_2_size;
    auto max_value_size = cache_->MaxKVSize();
    auto fixed_random_size = [this, max_value_size]() {
        static auto sizes = std::vector{max_value_size/4, max_value_size/8, max_value_size/16, max_value_size/32};
        return sizes[std::rand() % sizes.size()];
    };

    KVCache::Status status;
    const int max_key_id = 100;
    for (uint64_t i = 0, size = 0; size < total_size; i=(i+1)%max_key_id) {
        int value_size = fixed_random_size();
        std::string key = std::format("test_key_{}", i);
        key_2_size[key] = value_size;

        status = cache_->Put(key, std::string(value_size, 'v'));
        size += value_size;
        if (status.object_too_large()) {
            auto kv_size = key.size() + value_size;
            ASSERT_TRUE(kv_size > cache_->MaxKVSize())
                << "size: " << kv_size << " max_kv_size: " << cache_->MaxKVSize();
            continue;
        }
        ASSERT_TRUE(status.ok()) << status.msg();
    }

    for (auto& [key, value_size] : key_2_size) {
        std::string value;
        status = cache_->Get(key, &value);
        // Cache evicted or object too large
        if (status.not_found()) {
            continue;
        }
        ASSERT_TRUE(status.ok()) << status.msg();
        ASSERT_EQ(value, std::string(value_size, 'v')) << status.msg();
    }

    for (auto& [key, value_size] : key_2_size) {
        cache_->Delete(key);
    }

    for (auto& [key, value_size] : key_2_size) {
        std::string value;
        status = cache_->Get(key, &value);
        ASSERT_TRUE(status.not_found()) << status.msg();
    }
}


TEST_F(KVCacheTest, TestConcurrentOperations) {
    const int num_threads = 4;
    const int ops_per_thread = 1000;

    auto worker = [&](int id) {
        KVCache::Status status;
        for (int i = 0; i < ops_per_thread; i++) {
            std::string key = std::format("key_{}_{}", id, i);
            std::string value = std::format("value_{}", random_data_size());

            status = cache_->Put(key, value);
            ASSERT_TRUE(status.ok()) << status.msg();

            std::string retrieved_value;
            status = cache_->Get(key, &retrieved_value);
            ASSERT_TRUE(status.ok()) << status.msg();
            EXPECT_EQ(value, retrieved_value);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back(worker, i);
    }

    for (auto& thread : threads) {
        thread.join();
    }
}

TEST_F(KVCacheTest, TestEdgeCases) {
    // Empty key
    auto status = cache_->Put("", "value");
    ASSERT_TRUE(status.ok()) << status.msg();

    // Large value
    std::string large_value(cache_->MaxKVSize()/2, 'x');
    status = cache_->Put("large_key", large_value);
    ASSERT_TRUE(status.ok()) << status.msg();

    // Too large value
    std::string too_large_value(2*cache_->MaxKVSize(), 'x');
    status = cache_->Put("too_large_key", too_large_value);
    ASSERT_TRUE(status.object_too_large()) << status.msg();

    // Non-existent key
    std::string retrieved_value;
    status = cache_->Get("non_existent_key", &retrieved_value);
    ASSERT_TRUE(status.not_found()) << status.msg();

    // Delete non-existent key
    cache_->Delete("non_existent_key");
}

// Put 4*SSD data with random value sizes and random keys
// and test the performance of sequential Put and Get.
TEST_F(KVCacheTest, BenchmarkPutAndGet4SSDWithRandomKeyAndValue) {
    std::cout << "Benchmark: Put and Get 4 SSDs with random key and value" << std::endl;
    const size_t total_size = 2 * ssd_->nr_blocks_ * ssd_->block_size_;
    const size_t max_keys = 1000000;

    std::unordered_map<std::string, int> key_2_size;

    KVCache::Status status;
    size_t actual_write_bytes = 0;
    size_t actual_read_bytes = 0;
    size_t read_ops = 0;
    size_t write_ops = 0;

    // write performance test
    std::string write_buffer(cache_->MaxKVSize(), 'x');
    auto write_start = std::chrono::high_resolution_clock::now();
    for (int size = 0; actual_write_bytes < total_size; ) {
        int key_id = std::rand() % max_keys;
        int value_size = random_data_size();
        size += value_size;

        std::string key = std::format("bench_key_{}", key_id);
        key_2_size[key] = value_size;

        status = cache_->Put(key, std::string_view(write_buffer.data(), value_size));
        write_ops++;
        if (!status.ok()) {
            ASSERT_TRUE(status.object_too_large()) << status.msg();
            continue;
        }
        actual_write_bytes += value_size;
    }

    auto write_end = std::chrono::high_resolution_clock::now();

    // read performance test
    std::string value;
    auto read_start = std::chrono::high_resolution_clock::now();
    for (int size = 0; actual_read_bytes < total_size; ) {
        for (auto& [key, value_size] : key_2_size) {
            size += value_size;
            if (size > total_size) {
                break;
            }
            status = cache_->Get(key, &value);
            read_ops++;
            if (!status.ok()) {
                ASSERT_TRUE(status.not_found()) << status.msg();
                continue;
            }
            actual_read_bytes += value.size();
        }
    }

    auto read_end = std::chrono::high_resolution_clock::now();

    auto write_duration = std::chrono::duration_cast<std::chrono::duration<double>>(write_end - write_start);
    auto read_duration = std::chrono::duration_cast<std::chrono::duration<double>>(read_end - read_start);


    double write_seconds = write_duration.count();
    double read_seconds = read_duration.count();

    ASSERT_GT(write_seconds, 0.0) << "Write duration too short to measure";
    ASSERT_GT(read_seconds, 0.0) << "Read duration too short to measure";
    std::cout << "write_seconds: " << write_seconds << " read_seconds: " << read_seconds << std::endl;
    std::cout << "Performance test results:\n";
    std::cout << std::format("Write: {:.2f} ops/s, actual write {:.2f} MiB/s\n",
        write_ops / write_seconds,
        actual_write_bytes / 1024.0 / 1024.0 / write_seconds);
    std::cout << std::format("Read: {:.2f} ops/s, actual read {:.2f} MiB/s\n",
        read_ops / read_seconds,
        actual_read_bytes / 1024.0 / 1024.0 / read_seconds);
}


