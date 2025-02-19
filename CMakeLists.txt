cmake_minimum_required(VERSION 3.10)


# 设置项目名称和版本
project(KVCache VERSION 0.1.0 LANGUAGES CXX)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 选项设置
option(KVCACHE_BUILD_TESTS "Build KVCache tests" ON)
option(KVCACHE_BUILD_EXAMPLES "Build KVCache examples" OFF)

# 设置输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# 手动查找 RocksDB
find_path(ROCKSDB_INCLUDE_DIR rocksdb/db.h
    PATHS
        ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
        /usr/include
        /usr/local/include
)

find_library(ROCKSDB_LIBRARY
    NAMES rocksdb
    PATHS
        ${CMAKE_SOURCE_DIR}/third_party/rocksdb/lib
        /usr/lib
        /usr/local/lib
        /usr/lib64
        /usr/local/lib64
)

if(NOT ROCKSDB_INCLUDE_DIR OR NOT ROCKSDB_LIBRARY)
    message(FATAL_ERROR "RocksDB not found. Please install RocksDB first.")
endif()

message(STATUS "Found RocksDB: ${ROCKSDB_LIBRARY}")
message(STATUS "RocksDB include: ${ROCKSDB_INCLUDE_DIR}")

# 创建库目标
add_library(${PROJECT_NAME} STATIC
    src/kvcache.cpp
)

# 设置库的包含目录
target_include_directories(${PROJECT_NAME}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${ROCKSDB_INCLUDE_DIR}
)

# 链接 RocksDB
target_link_libraries(${PROJECT_NAME} PRIVATE ${ROCKSDB_LIBRARY})

find_package(OpenSSL REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE OpenSSL::SSL OpenSSL::Crypto)
target_include_directories(${PROJECT_NAME} PRIVATE ${OPENSSL_INCLUDE_DIR})

# Google Test
if(KVCACHE_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG release-1.12.1
    )
    FetchContent_MakeAvailable(googletest)
    
    enable_testing()
    add_executable(kvcache_test tests/kvcache_test.cpp)
    target_include_directories(kvcache_test PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )
    target_link_libraries(kvcache_test PRIVATE 
        ${PROJECT_NAME}
        GTest::gtest_main
        GTest::gmock
    )

    add_test(NAME kvcache_test COMMAND kvcache_test)
endif()

# if(KVCACHE_BUILD_EXAMPLES)
#     add_subdirectory(examples)
# endif()


install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

