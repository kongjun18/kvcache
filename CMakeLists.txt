cmake_minimum_required(VERSION 3.10)

# 设置项目名称和版本
project(KVCache VERSION 0.1.0 LANGUAGES CXX)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 选项设置
option(KVCACHE_BUILD_TESTS "Build KVCache tests" OFF)
option(KVCACHE_BUILD_EXAMPLES "Build KVCache examples" OFF)

# 设置输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

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
)

# 下载并配置 RocksDB
include(FetchContent)
FetchContent_Declare(
    rocksdb
    GIT_REPOSITORY https://github.com/facebook/rocksdb.git
    GIT_TAG v7.10.2
)

# RocksDB 配置
set(ROCKSDB_BUILD_SHARED OFF CACHE BOOL "Build shared library")
set(WITH_TESTS OFF CACHE BOOL "Build with tests")
set(WITH_TOOLS OFF CACHE BOOL "Build with tools")
set(WITH_BENCHMARK_TOOLS OFF CACHE BOOL "Build with benchmark tools") 
set(WITH_CORE_TOOLS OFF CACHE BOOL "Build with core tools")
set(WITH_GFLAGS OFF CACHE BOOL "Build with gflags")

FetchContent_MakeAvailable(rocksdb)

# 包含 RocksDB 头文件
target_include_directories(${PROJECT_NAME} PRIVATE ${rocksdb_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${rocksdb_SOURCE_DIR}/include)
# 链接 RocksDB
target_link_libraries(${PROJECT_NAME} PRIVATE rocksdb)

# 设置编译选项
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif()

# 添加子目录
# if(KVCACHE_BUILD_TESTS)
    # enable_testing()/ 
    # add_subdirectory(tests)
# endif()

# if(KVCACHE_BUILD_EXAMPLES)
#     add_subdirectory(examples)
# endif()

# 安装配置
include(GNUInstallDirs)
install(TARGETS ${PROJECT_NAME}
    EXPORT ${PROJECT_NAME}Targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# 导出配置
install(EXPORT ${PROJECT_NAME}Targets
    FILE ${PROJECT_NAME}Targets.cmake
    NAMESPACE ${PROJECT_NAME}::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
)

# 生成版本文件
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${PROJECT_NAME}ConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

