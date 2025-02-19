#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <string>
#include <thread>
#include <vector>
#include <random>
#include <chrono>
#include "kvcache.h"
#include "rocksdb/db.h"


class KVCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::string dbPath = "./test.db";
        auto ssd = KVCache::SSD::create(dbPath);
        ASSERT_TRUE(ssd != nullptr);

        cache_ = std::make_unique<KVCache::KVCache>(ssd);
    }

    std::unique_ptr<KVCache::KVCache> cache_;
};

TEST_F(KVCacheTest, BasicOperations) {
    // Put 1M keys
    KVCache::Status status;
    for (int i = 0; i < 1000000; i++) {
        std::string key = "test_key_" + std::to_string(i);
        ASSERT_TRUE(cache_->Put(key, key).ok());
    }

    // Get and verify all keys
    for (int i = 0; i < 1000000; i++) {
        std::string key = "test_key_" + std::to_string(i);
        std::string value;
        status = cache_->Get(key, &value);
        if (status.is_not_found()) {
            continue;
        }
        ASSERT_TRUE(status.ok());
        ASSERT_EQ(value, key);
    }

    // Delete all keys
    for (int i = 0; i < 1000000; i++) {
        std::string key = "test_key_" + std::to_string(i);
        cache_->Delete(key);
    }

    // Verify all keys are deleted
    for (int i = 0; i < 1000000; i++) {
        std::string key = "test_key_" + std::to_string(i);
        std::string value;
        status = cache_->Get(key, &value);
        ASSERT_TRUE(status.is_not_found());
    }


}

// // 并发测试
// TEST_F(KVCacheTest, ConcurrentOperations) {
//     const int num_threads = 4;
//     const int ops_per_thread = 1000;
    
//     auto worker = [&](int id) {
//         std::random_device rd;
//         std::mt19937 gen(rd());
//         std::uniform_int_distribution<> dis(0, 999);
        
//         for (int i = 0; i < ops_per_thread; i++) {
//             std::string key = "key" + std::to_string(id) + "_" + std::to_string(i);
//             std::string value = "value" + std::to_string(dis(gen));
            
//             ASSERT_TRUE(cache_->Put(key, value).ok());
            
//             std::string retrieved_value;
//             ASSERT_TRUE(cache_->Get(key, &retrieved_value).ok());
//             EXPECT_EQ(value, retrieved_value);
//         }
//     };
    
//     std::vector<std::thread> threads;
//     for (int i = 0; i < num_threads; i++) {
//         threads.emplace_back(worker, i);
//     }
    
//     for (auto& thread : threads) {
//         thread.join();
//     }
// }

// // 性能基准测试
// TEST_F(KVCacheTest, PerformanceBenchmark) {
//     const int num_operations = 10000;
    
//     // 写入性能测试
//     auto start = std::chrono::high_resolution_clock::now();
//     for (int i = 0; i < num_operations; i++) {
//         std::string key = "bench_key" + std::to_string(i);
//         std::string value = "bench_value" + std::to_string(i);
//         ASSERT_TRUE(cache_->Put(key, value).ok());
//     }
//     auto end = std::chrono::high_resolution_clock::now();
//     auto write_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
//     // 读取性能测试
//     start = std::chrono::high_resolution_clock::now();
//     std::string value;
//     for (int i = 0; i < num_operations; i++) {
//         std::string key = "bench_key" + std::to_string(i);
//         ASSERT_TRUE(cache_->Get(key, &value).ok());
//     }
//     end = std::chrono::high_resolution_clock::now();
//     auto read_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
//     std::cout << "性能测试结果:\n";
//     std::cout << "写入" << num_operations << "次操作耗时: " 
//               << write_duration.count() << "ms\n";
//     std::cout << "读取" << num_operations << "次操作耗时: " 
//               << read_duration.count() << "ms\n";
// }

// // 边界条件测试
// TEST_F(KVCacheTest, EdgeCases) {
//     // 空键测试
//     ASSERT_FALSE(cache_->Put("", "value").ok());
    
//     // 大数据测试
//     std::string large_value(1024 * 1024, 'x'); // 1MB value
//     ASSERT_TRUE(cache_->Put("large_key", large_value).ok());
    
//     std::string retrieved_value;
//     ASSERT_TRUE(cache_->Get("large_key", &retrieved_value).ok());
//     EXPECT_EQ(large_value, retrieved_value);
    
//     // 删除不存在的键
//     ASSERT_FALSE(cache_->Delete("non_existent_key").ok());
// }
