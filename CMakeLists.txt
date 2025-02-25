cmake_minimum_required(VERSION 3.20)

project(KVCache VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(KVCACHE_BUILD_TESTS "Build KVCache tests" ON)
option(KVCACHE_BUILD_EXAMPLES "Build KVCache examples" OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

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

find_package(OpenSSL REQUIRED)

if(KVCACHE_BUILD_TESTS)
    find_package(GTest REQUIRED)
endif()

message(STATUS "Found RocksDB: ${ROCKSDB_LIBRARY}")
message(STATUS "RocksDB include: ${ROCKSDB_INCLUDE_DIR}")

add_library(${PROJECT_NAME} STATIC SHARED
    src/kvcache.cpp
    src/status.cpp
    src/ssd.cpp
    src/kvcache_c_api.cpp
)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_definitions(${PROJECT_NAME} PRIVATE NDEBUG)
endif()

target_include_directories(${PROJECT_NAME}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${ROCKSDB_INCLUDE_DIR}
        ${OPENSSL_INCLUDE_DIR}
)

target_link_libraries(${PROJECT_NAME} PRIVATE ${ROCKSDB_LIBRARY})
target_link_libraries(${PROJECT_NAME} PRIVATE OpenSSL::SSL OpenSSL::Crypto)

# Google Test
if(KVCACHE_BUILD_TESTS)
    enable_testing()

    add_executable(kvcache_test tests/kvcache_test.cpp)
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


# Install library targets
install(TARGETS ${PROJECT_NAME}
    EXPORT ${PROJECT_NAME}Targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Install header files
install(FILES include/kvcache.h include/kvcache_c_api.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

